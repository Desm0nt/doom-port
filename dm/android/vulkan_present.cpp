#include <unordered_map>
#include <cmath>
// 安卓Vulkan呈现与窗口生命周期；依据原WSI桥及PR9/10的可回退呈现/钉核实验。
// Android Vulkan presentation and window lifetimes, based on the original WSI bridge and reversible PR9/10 overlap/affinity experiments.
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan_present.h"
#ifdef DM_GAME_DOOM
extern "C" void dm_aperf_present(ANativeWindow*,float);
extern "C" void dm_aperf_note_wait(int64_t);   // [r2] perf_android.c: the present thread's acquire wait (DM_ADPF_WORK=wall)
extern "C" void dm_aperf_frame_tid(int);   // [mt 2a] perf_android.c: the ADPF session's frame thread when presents run elsewhere
#endif
#include "../gfx/vulkan/nvn_vk.h"
#include "../gfx/xtrace.h"
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sched.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include "cpu_topo.h"   // [DOOM] thread placement by core capacity (DM_CPU_TOPO)
#include "../gfx/vulkan/engine/xbg_astc_overlay.h"   // [nonadreno] ASTC / ETC2 / EAC overlay (variant A): the boot choice below
#include "../gfx/vulkan/engine/xbg_astc_hdr_vk.h"   // [nonadreno] its ASTC HDR GPU self-test (BUGS_AND_FIXES 2.37)
#include <thread>
#include <unistd.h>
#include <time.h>
// [nonadreno] Mali: vkWaitForFences returns VK_ERROR_INITIALIZATION_FAILED when its ppoll() is interrupted by a signal (Skyline
// fence_cycle.h cbc896c8f8, idea only); retried. Other results unchanged, so Adreno sees the same calls.
static VkResult dm_present_wait_fences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout){
    VkResult r=vkWaitForFences(d,n,f,all,timeout);
    for(unsigned tries=0;r==VK_ERROR_INITIALIZATION_FAILED && tries<1000;++tries)r=vkWaitForFences(d,n,f,all,timeout);
    return r;}
#define vkWaitForFences dm_present_wait_fences
extern "C" double dm_present_period(void);
// 错过上屏节拍的帧数（晚于「上次期望时刻 + 周期」2 ms 以上）；自适应出图档按它降档 / 试升
// Frames that missed their display slot (more than 2 ms later than "previous desired time + period"); the adaptive present tier steps down / probes up by it
static std::atomic<uint64_t> dm_present_late{0};
extern "C" uint64_t dm_present_late_count(void){return dm_present_late.load(std::memory_order_relaxed);}
namespace {
std::mutex window_mutex;std::condition_variable window_cv;
// Vulkan状态仅宿主线程使用；UI锁只保护窗口快照，不跨GPU等待。
// Serialize Vulkan callers separately; never hold the UI window lock across GPU waits.
std::mutex render_mutex;
ANativeWindow *desired=nullptr,*active=nullptr;uint64_t generation=0,active_generation=0;
VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
VkSurfaceKHR surface;VkSwapchainKHR swapchain;VkFormat swap_format;VkExtent2D extent;
VkSurfaceTransformFlagBitsKHR created_transform;VkExtent2D created_surface_extent;
// 无旋转 blit 由系统合成器旋转；记录属性以免 SUBOPTIMAL 触发逐帧重建。
// The compositor rotates our unrotated blit; cache properties to avoid rebuilding every SUBOPTIMAL frame.
bool surface_properties_changed(){
    VkSurfaceCapabilitiesKHR c{};
    if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&c)!=VK_SUCCESS)return true;
    return c.currentTransform!=created_transform || c.currentExtent.width!=created_surface_extent.width || c.currentExtent.height!=created_surface_extent.height;
}
std::vector<VkImage> images;VkCommandPool command_pool;VkCommandBuffer command;
VkSemaphore acquired;std::vector<VkSemaphore> finished;VkFence fence;bool ready=false,swap_dirty=false;std::string shader_path;
// 缺省开启（标记文件 gfx-present-overlap 为 '0' 时关）：最多两次呈现 blit 在途，不在每帧后清空队列，游戏可在 GPU 收尾本帧时翻译下一帧。
// On by default (marker file gfx-present-overlap = '0' turns it off): keep up to two present blits in flight instead of draining the queue
// after every frame, so the game translates the next frame while the GPU finishes this one. Each in-flight frame has
// its own pool, acquire semaphore and fence; slot 0 is the original set. Later engine barriers on the source image are
// ALL_COMMANDS and its release retires with a later submission, which covers this blit in queue order.
bool overlap=false;VkCommandPool overlap_pool;VkCommandBuffer overlap_command;VkSemaphore overlap_acquired;VkFence overlap_fence;
bool in_flight[2];unsigned flight=0;
// Diagnostic (marker gfx-present-timing; overlap mode): GPU time of each present's clear and blit, from two timestamps per
// in-flight frame, read once that frame's fence has signalled; averaged over 300 presents.
bool present_timing=false;VkQueryPool present_queries=VK_NULL_HANDLE;bool present_stamped[2];double present_tick_ns=1,present_gpu_ns=0;unsigned present_timed=0;
// 游戏线程（调用呈现的线程；同步翻译时也是翻译线程）钉在最高频的核上。Redmi K50 Ultra 上调度器一直把它放在 A710
// （cpu4–6，MIUI 封顶 2.57 GHz），从不用空着的 X2（cpu7，封顶 2.25 GHz）；钉到 X2 后营地在 3D 比例 0.6 下从 40.2 升到
// 44.3 fps（同进程 A/B，CPU 17.1 → 14.3 ms）。MIUI 会改线程的可用核掩码，所以每 120 次呈现核对一次。
// 标记 gfx-no-pin 关闭，gfx-pin-cpu = 十六进制掩码 指定核；gfx-ab-toggle = N 加 gfx-ab-target = pin 时每 N 秒钉 / 放开交替。
// The game thread (the caller of present, which also translates in sync mode) runs on the cores with the highest maximum
// frequency. On the Redmi K50 Ultra the scheduler kept it on the A710 cores (cpu4-6, capped at 2.57 GHz by MIUI) and never
// used the idle X2 (cpu7, capped at 2.25 GHz); pinned to the X2 the camp went from 40.2 to 44.3 fps at 3D scale 0.6
// (in-process A/B, CPU 17.1 -> 14.3 ms). MIUI rewrites thread affinity masks, so the pin is checked every 120 presents.
// Marker gfx-no-pin turns it off and gfx-pin-cpu = hex mask picks the cores; gfx-ab-toggle = N with gfx-ab-target = pin
// alternates pinned and released every N seconds, in phase with the engine's A/B (its PERF lines carry ab=).
unsigned pin_mask=0;int pin_ab=0;int pin_state=-1;unsigned pin_presents=0,pin_drifts=0,pin_failures=0;cpu_set_t pin_original;bool pin_have_original=false;
// [DOOM] The default pin comes from cpu_topo.c (dm_cpu_plan()->translate): on SoCs with a second strong cluster (flagships) it is the
// old fastest_cores() result (the CPUs with the highest cpuinfo_max_freq, none when all share it); on one-big-cluster SoCs (SD680 4+4,
// Helio G99 2+6) it is the fastest core alone, so the record thread gets the next big core instead of the little ones.
// DM_CPU_TOPO=index gives the old rule. pin_planned: the pin is the plan's (no marker / DM_GFX_PIN_CPU changed it), so the record and
// present threads may take the plan's masks; with a hand-set pin they keep the old "every core but the pin".
bool pin_planned=false;
// [r2] Where the present (translation) thread really runs: sched_getcpu() once per present, and every 30 s one line
// "NVN pin cpus 30s: ..." with the share of presents seen on each CPU, the mask checks of that window and how many found the
// mask reset (HyperOS rewrites it). Always on, pin or not (one getcpu per present).
unsigned pin_hist[16],pin_hist_n=0,pin_checks_w=0,pin_drifts_w=0;int64_t pin_hist_t0=0;
void pin_hist_tick(){
    int c=sched_getcpu();if(c>=0&&c<16)++pin_hist[c];++pin_hist_n;
    timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);int64_t now=int64_t(ts.tv_sec)*1000000000+ts.tv_nsec;
    if(!pin_hist_t0){pin_hist_t0=now;return;}
    if(now-pin_hist_t0<30000000000ll)return;
    char line[256];int at=snprintf(line,sizeof line,"NVN pin cpus 30s: tid=%d mask=%x presents=%u",int(gettid()),pin_mask,pin_hist_n);
    for(int i=0;i<16 && at<int(sizeof line)-16;i++)if(pin_hist[i])at+=snprintf(line+at,sizeof line-at," c%d=%.0f%%",i,100.0*pin_hist[i]/pin_hist_n);
    if(at<int(sizeof line)-64)snprintf(line+at,sizeof line-at," mask_checks=%u found_reset=%u (total resets %u)",pin_checks_w,pin_drifts_w,pin_drifts);
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","%s",line);
    memset(pin_hist,0,sizeof pin_hist);pin_hist_n=pin_checks_w=pin_drifts_w=0;pin_hist_t0=now;
}
std::atomic<int> pin_tid{0};   // [DOOM] the thread pin_update pins (the caller of present; a guest thread with DM_NVN_SYNC=1): perf_android.c skips it
void pin_update(){
    pin_hist_tick();{const int t=int(gettid());if(pin_tid.load(std::memory_order_relaxed)!=t)pin_tid.store(t,std::memory_order_relaxed);}
    if(!pin_mask)return;bool want=true;
    if(pin_ab>0){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);want=((ts.tv_sec/pin_ab)&1)==0;}
    bool changed=int(want)!=pin_state;if(!changed && ++pin_presents%120)return;
    if(!pin_have_original){CPU_ZERO(&pin_original);if(sched_getaffinity(0,sizeof pin_original,&pin_original))for(int c=0;c<32;c++)CPU_SET(c,&pin_original);pin_have_original=true;}
    cpu_set_t set;CPU_ZERO(&set);if(want){for(int c=0;c<32;c++)if(pin_mask>>c&1)CPU_SET(c,&set);}else set=pin_original;
    if(!changed){++pin_checks_w;cpu_set_t cur;CPU_ZERO(&cur);if(!sched_getaffinity(0,sizeof cur,&cur) && CPU_EQUAL(&cur,&set))return;++pin_drifts;++pin_drifts_w;}
    int r=sched_setaffinity(0,sizeof set,&set),err=r?errno:0;pin_state=want;if(r)++pin_failures;
    // Refused while the app's cpuset lacks the cores (Android moves the app between cpusets, at start for one): the kernel
    // keeps the request and applies it again when the cpuset allows, and the next check retries.
    if(changed || pin_drifts<=4 || (r && pin_failures<=4))__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN pin tid=%d %s mask=%x result=%d errno=%d drifts=%u failures=%u",int(gettid()),want?"pinned":"released",want?pin_mask:0u,r,err,pin_drifts,pin_failures);
}
void log_message(void*,const char *text){__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","%s",text);}
[[noreturn]] void fatal(void*,const char *text){__android_log_print(ANDROID_LOG_FATAL,"dm-vulkan","%s",text);abort();}
namespace upscale{bool enabled();void fail(const char*,int);void drop_targets();bool targets(const std::vector<VkImage>&,VkFormat,VkExtent2D);}   // [DOOM] DM_GFX_UPSCALE, below
void destroy_present_semaphores(){for(auto s:finished)if(s)vkDestroySemaphore(device,s,nullptr);finished.clear();upscale::drop_targets();}   // [DOOM] + the upscale pass's views of the swapchain images
// 调用者先等队列空闲；基础WSI销毁仍沿用WaitIdle惯例，不声称提供呈现栅栏保证。
// Call only after queue idle. Base WSI teardown follows the WaitIdle convention,
// which is not the explicit presentation-fence guarantee of swapchain_maintenance1.
void destroy_swap(){destroy_present_semaphores();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;images.clear();if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;if(active)ANativeWindow_release(active);active=nullptr;}
void cleanup_initialization(){
    if(queue)vkQueueWaitIdle(queue);
    if(device){if(fence)vkDestroyFence(device,fence,nullptr);if(acquired)vkDestroySemaphore(device,acquired,nullptr);if(command_pool)vkDestroyCommandPool(device,command_pool,nullptr);}
    fence=VK_NULL_HANDLE;acquired=VK_NULL_HANDLE;command_pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;
    if(device){if(overlap_fence)vkDestroyFence(device,overlap_fence,nullptr);if(overlap_acquired)vkDestroySemaphore(device,overlap_acquired,nullptr);if(overlap_pool)vkDestroyCommandPool(device,overlap_pool,nullptr);}
    overlap_fence=VK_NULL_HANDLE;overlap_acquired=VK_NULL_HANDLE;overlap_pool=VK_NULL_HANDLE;overlap_command=VK_NULL_HANDLE;overlap=false;in_flight[0]=in_flight[1]=false;flight=0;
    destroy_swap();if(device)vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;queue=VK_NULL_HANDLE;
    if(instance)vkDestroyInstance(instance,nullptr);instance=VK_NULL_HANDLE;physical=VK_NULL_HANDLE;
}
// 刷新率投票：App 不投票时，安卓（MIUI）让这个表面跑 60 Hz，尽管屏和系统设置都允许 120。投 120 Hz 不改 60 帧的出图节奏（nvn_pace_hz），
// 但赶不上 16.7 ms 的那一帧在屏上停 25 ms 而不是 33.3 ms。标记 gfx-frame-rate = N（Hz；0 = 不投票）。
// Frame-rate vote: without a vote from the app, Android (MIUI) runs this surface at 60 Hz although the panel and the system
// setting allow 120. A 120 Hz vote keeps the 60 fps present pacing (nvn_pace_hz), but a frame that misses 16.7 ms stays on
// screen for 25 ms instead of 33.3. Marker gfx-frame-rate = N (Hz; 0 = no vote).
float frame_rate_vote=120;
// 按游戏输出尺寸出图：窗口缓冲设成游戏输出的尺寸（2000×900），由显示硬件放大到屏幕，呈现端只做 1:1 拷贝，不再每帧清屏、
// 用 GPU 按最近邻放大到 2712×1220。分辨率不变，最后一步放大从 GPU 移到显示控制器（滤波也更平滑）。标记 gfx-present-native = 0 关。
// Present at the game's output size: the window's buffers take the game's output size (2000x900) and the display hardware
// scales them to the screen, so the presenter copies 1:1 instead of clearing and nearest-scaling to 2712x1220 on the GPU
// every frame. Same resolution; the final upscale moves from the GPU to the display controller (with smoother filtering).
// Marker gfx-present-native = 0 turns it off.
#ifdef __OHOS__
bool present_native=false;   // 鸿蒙：缓冲尺寸与合成缩放未在真机核过，先关；标记 gfx-present-native = 1 打开 | HarmonyOS: buffer geometry vs compositor scaling not yet checked on device; off, marker gfx-present-native = 1 enables
#else
bool present_native=true;
#endif
int native_w=0,native_h=0;
int create_surface(){
    destroy_swap();native_w=native_h=0;
    {std::lock_guard<std::mutex> lock(window_mutex);if(!desired)return VK_NOT_READY;active=desired;ANativeWindow_acquire(active);active_generation=generation;}
    if(frame_rate_vote>0){
        // 运行时查询API，系统拒绝投票不会中止Vulkan初始化。 | Resolve the API at runtime; a refused vote does not abort Vulkan initialization.
        static auto set_rate=[]{void *lib=dlopen("libnativewindow.so",RTLD_NOW);return reinterpret_cast<int32_t(*)(ANativeWindow*,float,int8_t)>(lib?dlsym(lib,"ANativeWindow_setFrameRate"):nullptr);}();
        int fr=set_rate?set_rate(active,frame_rate_vote,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT):-1;
        __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Frame rate vote %.0f Hz result=%d (marker gfx-frame-rate; display request, not FPS)",double(frame_rate_vote),fr);
    }
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};ci.window=active;
    VkResult r=vkCreateAndroidSurfaceKHR(instance,&ci,nullptr,&surface);if(r)return r;
    if(!device)return 0;VkBool32 supported=VK_FALSE;r=vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported);if(r||!supported)return r?r:VK_ERROR_FEATURE_NOT_PRESENT;return 0;
}
int create_swap(){
    VkSurfaceCapabilitiesKHR cap;VkResult r=vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&cap);if(r)return r;
    if(!(cap.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!(cap.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t n=0;r=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,nullptr);if(r||!n)return r?r:VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<VkSurfaceFormatKHR> formats(n);r=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,formats.data());if(r)return r;
    VkSurfaceFormatKHR sf=formats[0];if(sf.format==VK_FORMAT_UNDEFINED)sf.format=VK_FORMAT_R8G8B8A8_UNORM;
    for(auto f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM||f.format==VK_FORMAT_B8G8R8A8_UNORM){sf=f;break;}
    extent=cap.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp(uint32_t(std::max(1,ANativeWindow_getWidth(active))),cap.minImageExtent.width,cap.maxImageExtent.width);extent.height=std::clamp(uint32_t(std::max(1,ANativeWindow_getHeight(active))),cap.minImageExtent.height,cap.maxImageExtent.height);}if(!extent.width||!extent.height)return VK_NOT_READY;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;
#ifdef __OHOS__
    {unsigned want=4;if(const char *e=getenv("DM_SWAP_IMAGES"))want=unsigned(std::max(2,atoi(e)));ci.minImageCount=std::max(want,cap.minImageCount);}   // 马良最佳实践：重负载 3–4 张；按期望上屏时刻排帧时合成器手里多压一张，取 4 张免得翻译线程等交换链、反压到游戏帧节奏 | Maleoon best practice: 3–4 under heavy load; with desired display times the compositor holds one more buffer, so take 4 to keep the translation thread from waiting on the swapchain and back-pressuring the game cadence
#else
    ci.minImageCount=std::max(2u,cap.minImageCount);
#endif
    if(cap.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,cap.maxImageCount);ci.imageFormat=sf.format;ci.imageColorSpace=sf.colorSpace;ci.imageExtent=extent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(cap.supportedCompositeAlpha&ci.compositeAlpha))for(uint32_t b=1;b<=8;b<<=1)if(cap.supportedCompositeAlpha&b){ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(b);break;}
    // [DOOM] DM_GFX_UPSCALE: the upscale pass renders into the swapchain images (the blit fallback still needs TRANSFER_DST)
    if(upscale::enabled()){if(cap.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)ci.imageUsage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;else upscale::fail("swapchain colour-attachment usage",0);}
    ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;ci.oldSwapchain=swapchain;VkSwapchainKHR replacement=VK_NULL_HANDLE;r=vkCreateSwapchainKHR(device,&ci,nullptr,&replacement);
    // 创建失败也会退休oldSwapchain；重试不得继续传入退休句柄。
    // Even failed creation retires oldSwapchain; never pass that retired handle on retry.
    destroy_present_semaphores();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=r?VK_NULL_HANDLE:replacement;images.clear();if(r)return r;swap_format=sf.format;
    created_transform=cap.currentTransform;created_surface_extent=cap.currentExtent;
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Swapchain: window=%dx%d extent=%ux%u currentTransform=%u supported=%u selected=%u",ANativeWindow_getWidth(active),ANativeWindow_getHeight(active),extent.width,extent.height,unsigned(cap.currentTransform),unsigned(cap.supportedTransforms),unsigned(ci.preTransform));
    r=vkGetSwapchainImagesKHR(device,swapchain,&n,nullptr);if(r)return r;images.resize(n);r=vkGetSwapchainImagesKHR(device,swapchain,&n,images.data());if(r)return r;images.resize(n);
    if(upscale::enabled())upscale::targets(images,swap_format,extent);   // [DOOM] DM_GFX_UPSCALE: views + framebuffers (a failure turns the pass off, the blit stays)
    // 呈现完成信号量按交换链图像分配；重新获取该图像后才能再次发信号。
    // Index present-wait semaphores by acquired image, not by CPU frame/fence.
    finished.resize(n,VK_NULL_HANDLE);VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto &s:finished){r=vkCreateSemaphore(device,&sc,nullptr,&s);if(r)return r;}return VK_SUCCESS;
}
// 诊断（标记 gfx-membench）：每种主机可见内存类型的 CPU 写、CPU 读，以及 GPU 填充后的 CPU 读速度，启动时测一次。
// Diagnostic (marker gfx-membench): CPU write, CPU read, and CPU read after a GPU fill, for each host-visible memory type; run once at start.
void membench(){
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    const VkDeviceSize n=4u<<20;std::vector<uint8_t> host(n,1),back(n);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;VkCommandPool pool;if(vkCreateCommandPool(device,&cp,nullptr,&pool))return;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;if(vkAllocateCommandBuffers(device,&ca,&cmd)){vkDestroyCommandPool(device,pool,nullptr);return;}
    auto now=[]{timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return double(ts.tv_sec)*1e9+double(ts.tv_nsec);};
    for(uint32_t t=0;t<mp.memoryTypeCount;t++){
        VkMemoryPropertyFlags f=mp.memoryTypes[t].propertyFlags;if(!(f&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=n;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VkBuffer b;if(vkCreateBuffer(device,&bi,nullptr,&b))continue;
        VkMemoryRequirements r;vkGetBufferMemoryRequirements(device,b,&r);if(!(r.memoryTypeBits&(1u<<t))){vkDestroyBuffer(device,b,nullptr);continue;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=t;VkDeviceMemory m;if(vkAllocateMemory(device,&ai,nullptr,&m)){vkDestroyBuffer(device,b,nullptr);continue;}
        void *p=nullptr;if(vkBindBufferMemory(device,b,m,0)||vkMapMemory(device,m,0,VK_WHOLE_SIZE,0,&p)){vkFreeMemory(device,m,nullptr);vkDestroyBuffer(device,b,nullptr);continue;}
        bool coherent=(f&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=m;mr.size=VK_WHOLE_SIZE;
        double w=1e30,rc=1e30,rd=1e30,flush=0,inv=0;bool ok=true;
        for(int k=0;k<4;k++){double t0=now();memcpy(p,host.data(),n);double t1=now();if(!coherent)vkFlushMappedMemoryRanges(device,1,&mr);double t2=now();w=std::min(w,t1-t0);flush=std::max(flush,t2-t1);}
        for(int k=0;k<4;k++){double t0=now();memcpy(back.data(),p,n);rc=std::min(rc,now()-t0);}
        for(int k=0;k<4;k++){
            vkResetCommandBuffer(cmd,0);VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkBeginCommandBuffer(cmd,&cb);
            uint32_t value=0x01020304u+uint32_t(k);vkCmdFillBuffer(cmd,b,0,n,value);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
            vkEndCommandBuffer(cmd);VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;if(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)||vkQueueWaitIdle(queue)){ok=false;break;}
            double t0=now();if(!coherent)vkInvalidateMappedMemoryRanges(device,1,&mr);double t1=now();memcpy(back.data(),p,n);double t2=now();
            rd=std::min(rd,t2-t1);inv=std::max(inv,t1-t0);uint32_t got;memcpy(&got,back.data()+n-4,4);ok=ok&&got==value;
        }
        __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN membench type=%u flags=%#x write=%.2f GB/s (flush %.0f us) read=%.2f GB/s read_after_gpu=%.2f GB/s (invalidate %.0f us) ok=%d",
            t,unsigned(f),double(n)/w,flush/1e3,double(n)/rc,double(n)/rd,inv/1e3,int(ok));
        vkUnmapMemory(device,m);vkFreeMemory(device,m,nullptr);vkDestroyBuffer(device,b,nullptr);
    }
    vkDestroyCommandPool(device,pool,nullptr);
}
int has_window(void*){std::lock_guard<std::mutex> l(window_mutex);return desired!=nullptr;}
void wait_window(){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait(lock,[]{return desired!=nullptr;});}
bool window_changed(){std::lock_guard<std::mutex> lock(window_mutex);return !desired||generation!=active_generation;}
void wait_window_change(uint64_t bad){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait(lock,[bad]{return generation!=bad;});}
void wait_resize(){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait_for(lock,std::chrono::milliseconds(16),[]{return generation!=active_generation;});}
// [DOOM] Optional sharpening of the game's frame at present: RCAS, the sharpening pass of AMD FSR 1 (shaders/rcas.comp, offline
// compiled). DM_GFX_SHARPEN=<stops> turns it on (0 = strongest, 0.2 typical, 1 mild); used with DM_DOOM_RES (the game renders
// 3D at a fraction and upscales it itself). Only for 8-bit UNORM RGBA/BGRA frames; anything else is presented unsharpened.
extern "C" { int dm_vk_format_list=0; }
// [nonadreno][afbc] VK_EXT_image_compression_control (query only) / VK_EXT or KHR_load_store_op_none enabled on the device (read by the engine)
extern "C" { int dm_vk_compression_control=0,dm_vk_store_none=0; }
// [DOOM] DM_TEX_DISCARD=1: a second queue of the same family for the engine's texture restore worker (xbg_tex_discard.inc), which
// reads Vulkan images back while the translation thread owns the main queue (no engine lock on the fault path). Null = none.
extern "C" { VkQueue dm_vk_aux_queue=VK_NULL_HANDLE; uint32_t dm_vk_aux_family=0; }   // VK_KHR_image_format_list enabled on the device (read by the Vulkan engine)
namespace rcas {
const uint32_t code[]={
#include "shaders/rcas.spv.inc"
};
int on=-1;float sharpness=1;VkImage image{};VkDeviceMemory memory{};VkImageView view{};uint32_t w=0,h=0;
VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkPipeline pipeline{};VkDescriptorPool pool{};VkDescriptorSet sets[2]{};VkSampler sampler{};VkImageView source_views[2]{};
bool enabled(){if(on<0){const char *e=getenv("DM_GFX_SHARPEN");on=e && *e && atof(e)>=0;if(on){sharpness=std::exp2(-float(atof(e)));__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","RCAS sharpening at present: stops=%s (DM_GFX_SHARPEN)",e);}}return on>0;}
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags want){VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want)return i;return 0;}
bool setup(){
    if(pipeline)return true;
    VkDescriptorSetLayoutBinding b[2]{};b[0].binding=0;b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[0].descriptorCount=1;b[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding=1;b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;b[1].descriptorCount=1;b[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=b;if(vkCreateDescriptorSetLayout(device,&dl,nullptr,&set_layout))return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,12};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;
    if(vkCreatePipelineLayout(device,&pl,nullptr,&layout))return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof code;mi.pCode=code;VkShaderModule module{};if(vkCreateShaderModule(device,&mi,nullptr,&module))return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=layout;
    VkResult r=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);vkDestroyShaderModule(device,module,nullptr);if(r)return false;
    VkDescriptorPoolSize ps[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=2;dp.poolSizeCount=2;dp.pPoolSizes=ps;
    if(vkCreateDescriptorPool(device,&dp,nullptr,&pool))return false;
    VkDescriptorSetLayout ls[2]={set_layout,set_layout};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=2;ai.pSetLayouts=ls;if(vkAllocateDescriptorSets(device,&ai,sets))return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;if(vkCreateSampler(device,&si,nullptr,&sampler))return false;
    return true;
}
bool target(uint32_t width,uint32_t height){
    if(image && w==width && h==height)return true;
    if(view)vkDestroyImageView(device,view,nullptr);if(image)vkDestroyImage(device,image,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);view={};image={};memory={};
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R8G8B8A8_UNORM;ic.extent={width,height,1};ic.mipLevels=ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;
    ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;if(vkCreateImage(device,&ic,nullptr,&image))return false;
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,image,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device,&ma,nullptr,&memory) || vkBindImageMemory(device,image,memory,0))return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ic.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};if(vkCreateImageView(device,&vi,nullptr,&view))return false;
    w=width;h=height;return true;
}
// Records the pass; on success the caller blits from rcas::image (TRANSFER_SRC_OPTIMAL) instead of the source.
bool record(VkCommandBuffer cmd,unsigned f,VkImage source,VkFormat format,uint32_t width,uint32_t height){
    if(!enabled() || (format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_UNORM))return false;
    if(!setup() || !target(width,height)){on=0;__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","RCAS setup failed: sharpening off");return false;}
    if(source_views[f])vkDestroyImageView(device,source_views[f],nullptr);   // this flight's previous frame has retired (its fence was waited)
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=source;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(device,&vi,nullptr,&source_views[f])){source_views[f]={};return false;}
    VkDescriptorImageInfo si{sampler,source_views[f],VK_IMAGE_LAYOUT_GENERAL},di{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2]{};for(auto &x:wr){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=sets[f];x.descriptorCount=1;}
    wr[0].dstBinding=0;wr[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[0].pImageInfo=&si;
    wr[1].dstBinding=1;wr[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;wr[1].pImageInfo=&di;vkUpdateDescriptorSets(device,2,wr,0,nullptr);
    VkImageMemoryBarrier b[2]{};
    for(auto &x:b){x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    b[0].image=source;b[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].newLayout=VK_IMAGE_LAYOUT_GENERAL;b[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    b[1].image=image;b[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b[1].newLayout=VK_IMAGE_LAYOUT_GENERAL;b[1].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b[1].dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,2,b);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&sets[f],0,nullptr);
    struct{int32_t w,h;float s;}push{int32_t(width),int32_t(height),sharpness};vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
    vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
    b[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL;b[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b[0].dstAccessMask=0;   // back to the layout the engine tracks
    b[1].oldLayout=VK_IMAGE_LAYOUT_GENERAL;b[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[1].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b);
    return true;
}
}
// [DOOM] DM_GFX_UPSCALE=gsr (default off; gear "Upscale to screen resolution (GSR)", next launch; docs/SWITCHES.md, BUGS_AND_FIXES 2.19):
// the swapchain takes the window's own size (e.g. 2400x1080) instead of the game's output size (1600x720), and one fragment pass
// upscales the game's frame into it with a Snapdragon-GSR-style edge-adaptive filter (shaders/gsr.frag, BSD-3-Clause, docs/THIRD_PARTY.md)
// instead of the 1:1 blit + the compositor's bilinear. With DM_GFX_SHARPEN the same pass also RCAS-sharpens (no separate RCAS pass).
// The crop (wide-live crop169, the engine's crop), the letterbox bars (render pass clear), the vertical mirror of
// DM_GFX_PRESENT_FLIP_FOLD and DM_PRESENT_STRETCH are all in the pass's coordinate mapping (push constants). Two timestamps per present;
// every ~3 s "NVN present upscale gsr WxH -> WxH gpu_us=..". Only 8-bit / 10-bit UNORM frames; any other format, a frame that is not
// enlarged, or a setup failure falls back to the old blit (linear filter, since the buffer is the window size). Off: nothing here runs.
namespace upscale {
const uint32_t vert_code[]={
#include "shaders/upscale.vert.spv.inc"
};
const uint32_t frag_code[]={
#include "shaders/gsr.frag.spv.inc"
};
int on=-1;const float edge_sharpness=2.0f;   // SGSR EdgeSharpness
VkSampler sampler{};VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkDescriptorPool pool{};VkDescriptorSet sets[2]{};VkImageView source_views[2]{};
VkFormat target_format=VK_FORMAT_UNDEFINED;VkRenderPass passes[2]{};VkPipeline pipeline{};   // passes[0] load DONT_CARE, [1] load CLEAR (letterbox bars); compatible
std::vector<VkImageView> views;std::vector<VkFramebuffer> framebuffers;VkExtent2D fb_extent{};
VkQueryPool queries{};bool stamped[2]{};double tick_ns=0;double sum_ns=0,max_ns=0;unsigned timed=0;int64_t window_t0=0;int last_sw=0,last_sh=0,last_dw=0,last_dh=0;
int64_t mono_ns(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return int64_t(ts.tv_sec)*1000000000+ts.tv_nsec;}
bool enabled(){
    if(on<0){const char *e=getenv("DM_GFX_UPSCALE");on=e && (!strcmp(e,"gsr") || !strcmp(e,"1"));
        if(on)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale gsr on (DM_GFX_UPSCALE=%s): swapchain at the window size, Snapdragon GSR-style pass%s",e,rcas::enabled()?" with RCAS in the same pass (DM_GFX_SHARPEN)":"");
        else if(e && *e && strcmp(e,"0") && strcmp(e,"off"))__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","NVN present upscale: DM_GFX_UPSCALE=%s is not known (gsr | 0): off",e);}
    return on>0;
}
void fail(const char *what,int r){if(on>0)__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","NVN present upscale off: %s failed (VkResult %d); the plain blit from now on",what,r);on=0;}
// Frames the pass takes: sampled as UNORM and written to the UNORM swapchain (sRGB / float frames keep the blit).
bool takes(VkFormat f,VkFormatFeatureFlags features){return enabled() && (f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_A2B10G10R10_UNORM_PACK32) && (features&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);}
void drop_targets(){for(auto fb:framebuffers)if(fb)vkDestroyFramebuffer(device,fb,nullptr);for(auto v:views)if(v)vkDestroyImageView(device,v,nullptr);framebuffers.clear();views.clear();fb_extent={};}
bool setup(){
    if(layout)return true;VkResult r;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if((r=vkCreateSampler(device,&si,nullptr,&sampler))){fail("sampler",r);return false;}
    VkDescriptorSetLayoutBinding b{};b.binding=0;b.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b.descriptorCount=1;b.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=1;dl.pBindings=&b;if((r=vkCreateDescriptorSetLayout(device,&dl,nullptr,&set_layout))){fail("descriptor set layout",r);return false;}
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=2;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
    if((r=vkCreateDescriptorPool(device,&dp,nullptr,&pool))){fail("descriptor pool",r);return false;}
    VkDescriptorSetLayout ls[2]={set_layout,set_layout};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=2;ai.pSetLayouts=ls;
    if((r=vkAllocateDescriptorSets(device,&ai,sets))){fail("descriptor sets",r);return false;}
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT,0,32};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;
    if((r=vkCreatePipelineLayout(device,&pl,nullptr,&layout))){fail("pipeline layout",r);return false;}
    // GPU cost: timestamps need a queue family with timestamp bits
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> q(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q.data());
    VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);
    if(family<n && q[family].timestampValidBits && pp.limits.timestampPeriod>0){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=4;if(vkCreateQueryPool(device,&qi,nullptr,&queries))queries=VK_NULL_HANDLE;else tick_ns=pp.limits.timestampPeriod;}
    if(!queries)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale: no timestamps on this queue, gpu_us is not measured");
    return true;
}
// Render passes and the pipeline for the swapchain's format (made again only when the format changes).
bool make_pipeline(VkFormat format){
    if(pipeline && format==target_format)return true;
    if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);pipeline={};for(auto &p:passes){if(p)vkDestroyRenderPass(device,p,nullptr);p={};}target_format=VK_FORMAT_UNDEFINED;VkResult r;
    for(int k=0;k<2;k++){
        VkAttachmentDescription a{};a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=k?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
        // The layout transition waits for the acquire semaphore (the submit waits it at COLOR_ATTACHMENT_OUTPUT); the present after it.
        VkSubpassDependency d[2]{};d[0].srcSubpass=VK_SUBPASS_EXTERNAL;d[0].dstSubpass=0;d[0].srcStageMask=d[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d[1].srcSubpass=0;d[1].dstSubpass=VK_SUBPASS_EXTERNAL;d[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d[1].dstStageMask=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;d[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&a;rp.subpassCount=1;rp.pSubpasses=&sp;rp.dependencyCount=2;rp.pDependencies=d;
        if((r=vkCreateRenderPass(device,&rp,nullptr,&passes[k]))){passes[k]={};fail("render pass",r);return false;}
    }
    VkShaderModule m[2]{};VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize=sizeof vert_code;mi.pCode=vert_code;if((r=vkCreateShaderModule(device,&mi,nullptr,&m[0]))){fail("vertex shader",r);return false;}
    mi.codeSize=sizeof frag_code;mi.pCode=frag_code;if((r=vkCreateShaderModule(device,&mi,nullptr,&m[1]))){vkDestroyShaderModule(device,m[0],nullptr);fail("GSR shader",r);return false;}
    VkPipelineShaderStageCreateInfo st[2]{};for(int k=0;k<2;k++){st[k].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[k].stage=k?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;st[k].module=m[k];st[k].pName="main";}
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&ba;
    VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=st;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;
    gp.pMultisampleState=&ms;gp.pColorBlendState=&cb;gp.pDynamicState=&ds;gp.layout=layout;gp.renderPass=passes[0];gp.subpass=0;
    r=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline);vkDestroyShaderModule(device,m[0],nullptr);vkDestroyShaderModule(device,m[1],nullptr);
    if(r){pipeline={};fail("GSR pipeline",r);return false;}
    target_format=format;return true;
}
// After a swapchain (re)creation: one view + framebuffer per image (create_swap; the old ones went with destroy_present_semaphores).
bool targets(const std::vector<VkImage> &imgs,VkFormat format,VkExtent2D ext){
    drop_targets();if(!enabled() || !setup() || !make_pipeline(format))return false;VkResult r;
    for(VkImage im:imgs){
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=im;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView v{};if((r=vkCreateImageView(device,&vi,nullptr,&v))){drop_targets();fail("swapchain image view",r);return false;}views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=passes[0];fi.attachmentCount=1;fi.pAttachments=&v;fi.width=ext.width;fi.height=ext.height;fi.layers=1;
        VkFramebuffer fb{};if((r=vkCreateFramebuffer(device,&fi,nullptr,&fb))){drop_targets();fail("framebuffer",r);return false;}framebuffers.push_back(fb);
    }
    fb_extent=ext;__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale gsr targets: %zu swapchain images %ux%u format %d",imgs.size(),ext.width,ext.height,int(format));
    return true;
}
// Once the flight's fence has signalled: its two timestamps; every ~3 s the average / max GPU time of the pass.
void collect(unsigned f){
    if(!stamped[f])return;stamped[f]=false;uint64_t t[2]{};
    if(vkGetQueryPoolResults(device,queries,f*2,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS || t[1]<t[0])return;
    const double ns=double(t[1]-t[0])*tick_ns;sum_ns+=ns;max_ns=std::max(max_ns,ns);++timed;
    const int64_t now=mono_ns();if(!window_t0)window_t0=now;
    if(now-window_t0<3000000000ll)return;
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale gsr %dx%d -> %dx%d gpu_us=%.0f max_us=%.0f presents=%u rcas=%s",last_sw,last_sh,last_dw,last_dh,sum_ns/timed/1e3,max_ns/1e3,timed,rcas::enabled()?"on":"off");
    sum_ns=max_ns=0;timed=0;window_t0=now;
}
// Records the pass into swapchain image `index`: render pass (clear only with letterbox bars), the source sampled in
// SHADER_READ_ONLY_OPTIMAL and returned to TRANSFER_SRC_OPTIMAL (the layout the engine tracks). Crop (x,y,w,h) of the upright
// frame goes to the rectangle (dx,dy,dw,dh); mirror = the source is stored bottom-up. false = nothing recorded (the caller blits).
bool record(VkCommandBuffer cmd,unsigned f,uint32_t index,VkImage source,VkFormat format,uint32_t width,uint32_t height,int x,int y,int w,int h,bool mirror,int dx,int dy,int dw,int dh,VkExtent2D ext,bool bars){
    if(!enabled() || !pipeline || index>=framebuffers.size() || fb_extent.width!=ext.width || fb_extent.height!=ext.height)return false;
    if(source_views[f])vkDestroyImageView(device,source_views[f],nullptr);source_views[f]={};   // this flight's previous frame has retired (its fence was waited)
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=source;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(VkResult r=vkCreateImageView(device,&vi,nullptr,&source_views[f])){source_views[f]={};fail("source view",r);return false;}
    VkDescriptorImageInfo ii{sampler,source_views[f],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wr.dstSet=sets[f];wr.dstBinding=0;wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&ii;vkUpdateDescriptorSets(device,1,&wr,0,nullptr);
    if(queries){vkCmdResetQueryPool(cmd,queries,f*2,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,f*2);}
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=source;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkClearValue black{};VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=passes[bars?1:0];rb.framebuffer=framebuffers[index];rb.renderArea={{0,0},ext};rb.clearValueCount=1;rb.pClearValues=&black;
    vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[f],0,nullptr);
    VkViewport v{float(dx),float(dy),float(dw),float(dh),0,1};VkRect2D sc{{dx,dy},{uint32_t(dw),uint32_t(dh)}};vkCmdSetViewport(cmd,0,1,&v);vkCmdSetScissor(cmd,0,1,&sc);
    // source position p = gl_FragCoord * scale + offset (pixels); mirrored: stored row = height - upright row (gsr.frag)
    const double sx=double(w)/dw,sy=double(h)/dh;
    struct{float sx,sy,ox,oy,tx,ty,rcas,edge;}pc{float(sx),float(mirror?-sy:sy),float(x-dx*sx),float(mirror?double(height)-y+dy*sy:y-dy*sy),1.f/float(width),1.f/float(height),rcas::enabled()?rcas::sharpness:0.f,edge_sharpness};
    vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof pc,&pc);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
    // back to the engine's layout; later writes to the source wait for these reads
    b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    if(queries){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,f*2+1);stamped[f]=true;}
    last_sw=w;last_sh=h;last_dw=dw;last_dh=dh;return true;
}
}
// [DOOM] Live widescreen switch (DM_DOOM_WIDE_LIVE=1, BUGS_AND_FIXES 2.12): the game always renders W x 720 and "wide off" shows its
// centre 16:9 (the Hor+ projection keeps the vertical FOV, so the centre crop is exactly the 16:9 view; the SWF HUD is centred at
// scale 1). -1 = not decided yet: at the first frame, crop when DM_DOOM_WIDE_LIVE=1 and the gear option is off (DM_DOOM_WIDE=0).
static std::atomic<int> crop169{-1};
// [4:3] hle_args.c (DOOM builds): the initial crop from the resolved aspect choice (DM_DOOM_ASPECT / DM_DOOM_WIDE; BUGS_AND_FIXES 2.20).
extern "C" int dm_doom_live_crop_initial(void) __attribute__((weak));
static std::atomic<int> mirror_next{0};   // [r3] DM_GFX_PRESENT_FLIP_FOLD: mirror the next present vertically (dm_present_mirror_next)
// [mt 2a] DM_GFX_PRESENT_THREAD (xbg_game.cpp reads it and calls dm_present_thread_set; docs/SWITCHES.md, BUGS_AND_FIXES 3.20). 1: the
// engine hands each present (source image, format, size, crop, the mirror flag captured on its thread) to a present thread that runs the
// same present_body as before: wait for the flight's fence, acquire, letterbox clear, RCAS, blit, submit, queue present, swapchain and
// surface recreation, window waits while backgrounded, ADPF report / frame-rate vote (dm_aperf_present). Order on the one VkQueue: the
// engine waits until every handed present has its blit submitted before it submits a slot (dm_present_wait_submitted), and every queue
// call of both threads takes queue_mutex, so the queue sees the same sequence as presenting inline. The translation thread keeps the core
// pin (pin_update at the handoff) and stays the ADPF session's frame thread (dm_aperf_frame_tid). 2: the same protocol (queue lock,
// captured mirror flag) run inline on the caller, no thread. 0 / unset: none of this runs; QLock is a no-op.
int pt_cfg=0;std::mutex queue_mutex;thread_local bool pt_on_thread=false;
struct QLock{bool on;QLock():on(pt_cfg!=0){if(on)queue_mutex.lock();}~QLock(){if(on)queue_mutex.unlock();}};
VkResult queue_wait_idle(){QLock l;return vkQueueWaitIdle(queue);}
namespace pt {
struct Job{VkImage src;VkFormat fmt;uint32_t w,h;int32_t crop[4];bool has_crop,mirror;uint64_t ticket;};
constexpr unsigned depth=4;Job ring[depth];
std::mutex mu;std::condition_variable cv;uint64_t enq=0,sub=0,done=0,cur=0;bool started=false;   // tickets: handed, blit submitted, finished; cur: present thread only
std::atomic<int> err{0};std::atomic<uint64_t> jobs{0},busy_ns{0},full_ns{0};
int64_t now_ns(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return int64_t(ts.tv_sec)*1000000000+ts.tv_nsec;}
void mark_submitted(){if(!pt_on_thread || !cur)return;{std::lock_guard<std::mutex> l(mu);if(sub<cur)sub=cur;}cv.notify_all();}
void loop();
}
int present_body(VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4],bool mirror);
int present(void*,void*,VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t){
    std::lock_guard<std::mutex> render_lock(render_mutex);
    pin_update();
    const bool mirror=mirror_next.exchange(0,std::memory_order_relaxed)!=0;
    return present_body(source,source_format,width,height,crop,mirror);
}
// The present itself; the caller holds render_mutex (present(), the present thread, or the inline protocol of DM_GFX_PRESENT_THREAD=2).
int present_body(VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4],bool mirror){
    VkFormatProperties sp;vkGetPhysicalDeviceFormatProperties(physical,source_format,&sp);if(!(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    int x=0,y=0,w=int(width),h=int(height);if(crop&&crop[2]>0&&crop[3]>0){x=std::clamp(crop[0],0,w);y=std::clamp(crop[1],0,h);w=std::min(crop[2],int(width)-x);h=std::min(crop[3],int(height)-y);}if(w<=0||h<=0)return VK_ERROR_INITIALIZATION_FAILED;
    {int c=crop169.load(std::memory_order_relaxed);
        if(c<0){const char*l=getenv("DM_DOOM_WIDE_LIVE"),*e=getenv("DM_DOOM_WIDE");c=dm_doom_live_crop_initial?dm_doom_live_crop_initial():(l&&*l=='1'&&(!e||!*e||(e[0]=='0'&&!e[1])))?1:0;int u=-1;crop169.compare_exchange_strong(u,c);c=crop169.load(std::memory_order_relaxed);}
        if(c>0&&int64_t(w)*9>int64_t(h)*16+9){int nw=int((int64_t(h)*16/9)&~1);x+=(w-nw)/2;w=nw;}}   // wider than 16:9 by more than a pixel
    for(;;){
    // [DOOM] The buffer keeps the window's aspect ratio: a 16:9 buffer on a 20:9 screen was stretched sideways by the
    // compositor (round HUD prompts looked squashed). It keeps the game's height, so the blit below adds side bars.
    // DM_PRESENT_STRETCH=1 restores the old fill-the-screen stretch.
    // [DOOM] DM_GFX_UPSCALE (namespace upscale): the buffer is the window's own size and the pass below fills it.
    const bool up_want=upscale::takes(source_format,sp.optimalTilingFeatures);bool stretched=false;
    int bw=w,bh=h;{static int stretch=-1;if(stretch<0){const char*e=getenv("DM_PRESENT_STRETCH");stretch=e&&*e=='1';}stretched=stretch>0;
        int ww=active?ANativeWindow_getWidth(active):0,wh=active?ANativeWindow_getHeight(active):0;
        if(!stretch&&ww>0&&wh>0){if(int64_t(ww)*h>int64_t(wh)*w)bw=int((int64_t(h)*ww/wh+1)&~1);else bh=int((int64_t(w)*wh/ww+1)&~1);}
        if(up_want&&ww>0&&wh>0){bw=ww;bh=wh;}}
    if(present_native && surface && active && (native_w!=bw || native_h!=bh)){   // the next swapchain takes this size (currentExtent follows the buffer geometry)
        int g=ANativeWindow_setBuffersGeometry(active,bw,bh,0);native_w=bw;native_h=bh;swap_dirty=true;
        if(up_want)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Present upscale: the game's output %dx%d into a %dx%d window-size buffer result=%d (DM_GFX_UPSCALE)",w,h,bw,bh,g);
        else __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Present at the game's output size %dx%d in a %dx%d buffer result=%d (marker gfx-present-native)",w,h,bw,bh,g);}
    // 后台等待释放UI锁；保留同一源帧，恢复并真正呈现后才返回成功。
    // Suspend without the UI lock; keep this source frame until it is presented.
    wait_window();
    VkResult r;
    if(!surface||window_changed()){
        if((r=queue_wait_idle()))return r;
        r=VkResult(create_surface());
        if(r==VK_NOT_READY)continue;
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return r;swap_dirty=true;
    }
    if(swap_dirty){
        if((r=queue_wait_idle()))return r;
        r=VkResult(create_swap());
        if(r==VK_NOT_READY||r==VK_ERROR_OUT_OF_DATE_KHR){wait_resize();continue;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return r;swap_dirty=false;
    }
    VkFormatProperties dp;vkGetPhysicalDeviceFormatProperties(physical,swap_format,&dp);if(!(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // 有限获取超时让渲染线程重新检查Surface代数，UI从不等待驱动。
    // A bounded acquire lets rendering observe lifecycle changes; UI never waits on the driver.
    unsigned f=overlap?flight:0;VkCommandPool pool=f?overlap_pool:command_pool;VkCommandBuffer cmd=f?overlap_command:command;VkSemaphore acq=f?overlap_acquired:acquired;VkFence done=f?overlap_fence:fence;
    if(in_flight[f]){XTRACE_BEGIN("dm-present-fence");r=vkWaitForFences(device,1,&done,VK_TRUE,10000000000ull);XTRACE_END();if(r)return r;if((r=vkResetFences(device,1,&done)))return r;in_flight[f]=false;
        upscale::collect(f);   // [DOOM] DM_GFX_UPSCALE: the pass's timestamps of this flight's previous present
        if(present_stamped[f]){uint64_t t[2]{};if(vkGetQueryPoolResults(device,present_queries,f*2,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS && t[1]>=t[0]){present_gpu_ns+=double(t[1]-t[0])*present_tick_ns;
            if(++present_timed==300){__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN PERF present_gpu_us=%.0f extent=%ux%u (marker gfx-present-timing)",present_gpu_ns/present_timed/1e3,extent.width,extent.height);present_gpu_ns=0;present_timed=0;}}present_stamped[f]=false;}}
    uint32_t index=0;
#ifdef DM_GAME_DOOM
    timespec aq0;clock_gettime(CLOCK_MONOTONIC,&aq0);
#endif
    XTRACE_BEGIN("dm-acquire");r=vkAcquireNextImageKHR(device,swapchain,50000000ull,acq,VK_NULL_HANDLE,&index);XTRACE_END();
#ifdef DM_GAME_DOOM
    {timespec aq1;clock_gettime(CLOCK_MONOTONIC,&aq1);if(!pt_on_thread)dm_aperf_note_wait((int64_t(aq1.tv_sec)-aq0.tv_sec)*1000000000+(aq1.tv_nsec-aq0.tv_nsec));}   // [r2] DM_ADPF_WORK=wall
#endif
    if(r==VK_TIMEOUT||r==VK_NOT_READY)continue;
    if(r==VK_ERROR_OUT_OF_DATE_KHR){swap_dirty=true;wait_resize();continue;}
    if(r==VK_ERROR_SURFACE_LOST_KHR){if((r=queue_wait_idle()))return r;uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
    bool rebuild=r==VK_SUBOPTIMAL_KHR;if(r!=VK_SUCCESS&&!rebuild)return r;
    r=vkResetCommandPool(device,pool,0);if(r)return r;VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;if((r=vkBeginCommandBuffer(cmd,&bi)))return r;
    bool stamp=present_timing && overlap;if(stamp && !present_queries){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=4;if(vkCreateQueryPool(device,&q,nullptr,&present_queries))stamp=false;
        VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);present_tick_ns=pp.limits.timestampPeriod;}
    if(stamp){vkCmdResetQueryPool(cmd,present_queries,f*2,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,present_queries,f*2);}
    double scale=std::min(double(extent.width)/w,double(extent.height)/h);int dw=std::max(1,int(w*scale)),dh=std::max(1,int(h*scale)),dx=(int(extent.width)-dw)/2,dy=(int(extent.height)-dh)/2;
    // [DOOM] DM_GFX_UPSCALE: the pass renders the swapchain image itself (letterbox clear, crop, mirror, RCAS); only when it enlarges.
    // DM_PRESENT_STRETCH=1: the whole buffer, unequal x / y scale (as the compositor stretched before).
    if(up_want && stretched){dx=dy=0;dw=int(extent.width);dh=int(extent.height);}
    const bool upscaled=up_want && (dw>w || dh>h) && upscale::record(cmd,f,index,source,source_format,width,height,x,y,w,h,mirror,dx,dy,dw,dh,extent,dx || dy || dw!=int(extent.width) || dh!=int(extent.height));
    if(!upscaled){
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    // 换布局须在获取信号量的等待阶段（下面提交的 TRANSFER）之后：源阶段写 TOP_OF_PIPE 时，换布局不等信号量，可能在呈现引擎
    // 读完这张图之前就发生（验证层同步检查：SYNC-HAZARD-WRITE-AFTER-READ）。
    // The layout transition must come after the acquire semaphore's wait stage (TRANSFER, in the submit below). With
    // TOP_OF_PIPE as the source stage it does not wait for the semaphore and can happen before the presentation engine has
    // finished reading the image (synchronization validation: SYNC-HAZARD-WRITE-AFTER-READ).
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    if(dx || dy || dw!=int(extent.width) || dh!=int(extent.height)){   // letterbox bars only where the blit leaves part of the image
        VkClearColorValue black{};vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&barrier.subresourceRange);
        barrier.oldLayout=barrier.newLayout;barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);}
    VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;blit.srcOffsets[0]={x,y,0};blit.srcOffsets[1]={x+w,y+h,1};blit.dstOffsets[0]={dx,dy,0};blit.dstOffsets[1]={dx+dw,dy+dh,1};
    // [r3] source stored bottom-up (DM_GFX_TAA_FIX lower-left): the crop rows [y, y+h) of the upright image are rows
    // [height-y-h, height-y) here; srcOffsets[0].y > srcOffsets[1].y makes the blit mirror them
    if(mirror){blit.srcOffsets[0].y=int(height)-y;blit.srcOffsets[1].y=int(height)-(y+h);}
    // 最近邻不要求额外线性过滤能力。鸿蒙：游戏出图比屏幕小、要放大，格式支持时用线性，不然像素发硬、斜边锯齿。
    // Nearest needs no extra linear-filter support. HarmonyOS: the game image is smaller than the screen and gets upscaled; use linear when the format supports it, otherwise pixels look blocky and diagonals jagged.
    VkFilter filter=VK_FILTER_NEAREST;
#ifdef __OHOS__
    if(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)filter=VK_FILTER_LINEAR;
#endif
    // [DOOM] DM_GFX_UPSCALE without its pass (format not taken, setup failed): the buffer is the window size, so scale linearly
    if(upscale::enabled() && (dw!=w || dh!=h) && (sp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))filter=VK_FILTER_LINEAR;
    const bool sharpened=rcas::record(cmd,f,source,source_format,width,height);
    vkCmdBlitImage(cmd,sharpened?rcas::image:source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    // 同队列后续写入必须等源图读取完成；即使下一次图像布局未改变也建立 WAR 执行依赖。
    // Order subsequent same-queue writes after the source read, including reuse without a layout change.
    VkMemoryBarrier source_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};source_read.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source_read.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&source_read,0,nullptr,0,nullptr);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    if(stamp){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,present_queries,f*2+1);present_stamped[f]=true;}
    // the acquire semaphore gates the first write to the swapchain image: the blit (TRANSFER) or the upscale pass's attachment
    if((r=vkEndCommandBuffer(cmd)))return r;VkPipelineStageFlags stage=upscaled?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acq;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&finished[index];
    {QLock ql;r=vkQueueSubmit(queue,1,&submit,done);}if(r)return r;in_flight[f]=true;pt::mark_submitted();VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&finished[index];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;
#ifdef __OHOS__
    // 期望上屏时刻（鸿蒙 SET_DESIRED_PRESENT_TIMESTAMP）：上屏节奏 = 上次期望时刻 + 一个出图档周期；早到的帧由合成器按时放出，翻译线程不等；
    // 落后时从当前时刻重新起拍。翻译完成时刻的抖动（±5 ms）不再让一帧在屏上停 3 或 5 个刷新。DM_PRESENT_TS=0 关。
    // Desired display time (HarmonyOS SET_DESIRED_PRESENT_TIMESTAMP): cadence = previous desired time + one present-tier period; an early frame is released on time by the compositor
    // without the translation thread waiting; when late, restart the cadence from now. Jitter in translation completion (±5 ms) no longer leaves a frame on screen for 3 or 5 refreshes.
    {static int on=-1;if(on<0){const char *e=getenv("DM_PRESENT_TS");on=!(e&&*e=='0');}static int64_t tgt;double per=dm_present_period();
     if(on&&per>0&&active){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);int64_t now=int64_t(ts.tv_sec)*1000000000+ts.tv_nsec,p=int64_t(per*1e9),next=tgt?tgt+p:now;
      if(tgt&&now>tgt+p+2000000)dm_present_late.fetch_add(1,std::memory_order_relaxed);if(next<now)next=now;if(next>now+2*p)next=now+p;tgt=next;OH_NativeWindow_NativeWindowHandleOpt(active,SET_DESIRED_PRESENT_TIMESTAMP,next);}}
#endif
#ifdef DM_GAME_DOOM
    dm_aperf_present(active,frame_rate_vote);   // ADPF work report + display rate vote (perf_android.c)
#endif
    XTRACE_BEGIN("dm-queue-present");VkResult pr;{QLock ql;pr=vkQueuePresentKHR(queue,&pi);}XTRACE_END();
    // 呈现失败时先排空，拆除路径要求队列空闲。
    // Drain a failed present before teardown, which requires an idle queue.
    if(overlap){flight^=1;if(pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR && (r=queue_wait_idle()))return r;}
    else{if((r=vkWaitForFences(device,1,&done,VK_TRUE,10000000000ull)))return r;if((r=vkResetFences(device,1,&done)))return r;in_flight[0]=false;upscale::collect(0);if((r=queue_wait_idle()))return r;}
    if(pr==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
    if(pr==VK_ERROR_OUT_OF_DATE_KHR){swap_dirty=true;wait_resize();continue;}
    if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR)return pr;
    // 已成功呈现的帧不因尺寸瞬变报错；下帧再重建或等待可用尺寸。
    // Defer suboptimal recreation to the next frame, which can wait for a usable extent.
    if((rebuild||pr==VK_SUBOPTIMAL_KHR)&&surface_properties_changed())swap_dirty=true;
    return 0;
    }
}
}
// [DOOM] JNI nativeWideLive (jni_android.c): 1 = show the centre 16:9 of the wide frame, 0 = the whole wide frame. Takes effect at the next present.
extern "C" void dm_present_crop169(int on){crop169.store(on?1:0,std::memory_order_relaxed);__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","[wide] live: %s",on?"centre 16:9 crop":"full wide frame");}
// [r3] DM_GFX_PRESENT_FLIP_FOLD (xbg_game.cpp xbg_present_texture_now): the engine asks the next present to mirror the source
// vertically in the swapchain blit instead of blitting it upside down into a flip image first (one full-screen blit and the
// 3 flip images less). Declined (0) while RCAS sharpening is on: its pass reads the source as given. [DOOM] Taken with DM_GFX_UPSCALE:
// its pass mirrors in its own mapping (RCAS included); its blit fallback mirrors from rcas::image, which keeps the source's orientation.
extern "C" int dm_present_mirror_next(int on){if(on && rcas::enabled() && !upscale::enabled())return 0;mirror_next.store(on?1:0,std::memory_order_relaxed);return 1;}
// [mt 2a] DM_GFX_PRESENT_THREAD: the present thread and the engine's side of the protocol (see pt_cfg above).
void pt::loop(){
    pt_on_thread=true;pthread_setname_np(pthread_self(),"dm-present");
    int pr=setpriority(PRIO_PROCESS,0,-8);if(pr)pr=setpriority(PRIO_PROCESS,0,-4);
    // Off the translation thread's pinned core(s): it is the one that must not be slowed.
    // [mtb] DM_GFX_PRESENT_UNPIN=1: start from every core, not from the creator's affinity. The thread is created by the pinned translation
    // thread and inherits its pin, so clearing the pin left an empty set and it stayed on the translation thread's core (affinity=1 in the log).
    const char *unpin_e=getenv("DM_GFX_PRESENT_UNPIN");const bool unpin=!(unpin_e && *unpin_e=='0');   // on by default since 02.11 (the present thread inherited the translator pin)
    // [DOOM] With the planned pin (cpu_topo.c) the plan's present mask: on flagships every core but the pin, as before; on one-big-cluster
    // SoCs the little cluster (the present thread is light: one blit and vkQueuePresentKHR per frame). DM_GFX_PRESENT_CPUS=<hex> sets it.
    const dm_cpu_plan_t *plan=dm_cpu_plan();unsigned planned=0;
    int ar=1;if(pin_mask && unpin && (pin_planned || getenv("DM_GFX_PRESENT_CPUS")) && plan->present){cpu_set_t set;CPU_ZERO(&set);for(int c=0;c<32;c++)if(plan->present>>c&1)CPU_SET(c,&set);ar=sched_setaffinity(0,sizeof set,&set);planned=plan->present;}
    else if(pin_mask){cpu_set_t set;CPU_ZERO(&set);if(unpin){long n=sysconf(_SC_NPROCESSORS_CONF);if(n<1 || n>32)n=n<1?8:32;for(long c=0;c<n;c++)CPU_SET(int(c),&set);}
     if(unpin || !sched_getaffinity(0,sizeof set,&set)){for(int c=0;c<32;c++)if(pin_mask>>c&1)CPU_CLR(c,&set);if(CPU_COUNT(&set))ar=sched_setaffinity(0,sizeof set,&set);}}
    unsigned now_mask=0;{cpu_set_t cur;CPU_ZERO(&cur);if(!sched_getaffinity(0,sizeof cur,&cur))for(int c=0;c<32;c++)if(CPU_ISSET(c,&cur))now_mask|=1u<<c;}
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present thread started tid=%d nice=%d (setpriority %d) off-pin-mask=%x affinity=%d cpus=%x (%s)",int(gettid()),getpriority(PRIO_PROCESS,0),pr,pin_mask,ar,now_mask,
        planned?"cpu plan":pin_mask?"all but the pin":"inherited");
    for(;;){Job j;{std::unique_lock<std::mutex> l(mu);cv.wait(l,[]{return done<enq;});j=ring[(done+1)%depth];cur=j.ticket;}
        const int64_t t0=now_ns();int r;{std::lock_guard<std::mutex> rl(render_mutex);r=present_body(j.src,j.fmt,j.w,j.h,j.has_crop?j.crop:nullptr,j.mirror);}
        busy_ns+=uint64_t(now_ns()-t0);++jobs;
        if(r){int z=0;if(err.compare_exchange_strong(z,r))__android_log_print(ANDROID_LOG_ERROR,"dm-vulkan","NVN present thread: present %llu failed: VkResult %d",(unsigned long long)j.ticket,r);}
        {std::lock_guard<std::mutex> l(mu);if(sub<j.ticket)sub=j.ticket;done=j.ticket;cur=0;}cv.notify_all();}
}
extern "C" void dm_present_thread_set(int mode){pt_cfg=mode>0?mode:0;if(pt_cfg)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present thread mode=%d (DM_GFX_PRESENT_THREAD: 1 own thread, 2 same protocol inline)",pt_cfg);}
// Returns 0, or the VkResult of an earlier failed present on the thread (the engine stops with it, as it did for an inline failure).
extern "C" int dm_present_async(VkImage source,VkFormat format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t){
    if(pt_cfg==2){std::lock_guard<std::mutex> render_lock(render_mutex);pin_update();const bool mirror=mirror_next.exchange(0,std::memory_order_relaxed)!=0;++pt::jobs;return present_body(source,format,width,height,crop,mirror);}
    pin_update();   // the translation thread (the caller), as present() pinned it
#ifdef DM_GAME_DOOM
    {static bool told=false;if(!told){told=true;dm_aperf_frame_tid(int(gettid()));}}
#endif
    const bool mirror=mirror_next.exchange(0,std::memory_order_relaxed)!=0;   // this present's flag, set by the engine just before
    std::unique_lock<std::mutex> l(pt::mu);
    if(!pt::started){pt::started=true;std::thread(pt::loop).detach();}
    if(pt::enq-pt::done>=pt::depth){const int64_t t0=pt::now_ns();pt::cv.wait(l,[]{return pt::enq-pt::done<pt::depth;});pt::full_ns+=uint64_t(pt::now_ns()-t0);}
    if(int e=pt::err.load())return e;
    pt::Job &j=pt::ring[(pt::enq+1)%pt::depth];j.src=source;j.fmt=format;j.w=width;j.h=height;j.has_crop=crop!=nullptr;if(crop)memcpy(j.crop,crop,sizeof j.crop);else memset(j.crop,0,sizeof j.crop);j.mirror=mirror;j.ticket=pt::enq+1;
    ++pt::enq;l.unlock();pt::cv.notify_all();return 0;
}
// [mtb] DM_GFX_RECORD_THREAD (xbg_game.cpp vkrec): the hand-off in two halves. dm_present_handoff_prepare runs on the translation thread where
// dm_present_async ran: the core pin, the ADPF frame thread, this present's mirror flag (set by the engine just before), and it starts the
// present thread there, so that thread's placement does not depend on the record thread. dm_present_async_m runs on the record thread after
// the frame's slot submission and hands the job over with that flag (DM_GFX_PRESENT_THREAD=2: presents inline on the caller, as dm_present_async).
extern "C" int dm_present_handoff_prepare(void){
    pin_update();
    if(pt_cfg==1){
#ifdef DM_GAME_DOOM
        {static bool told=false;if(!told){told=true;dm_aperf_frame_tid(int(gettid()));}}
#endif
        std::lock_guard<std::mutex> l(pt::mu);if(!pt::started){pt::started=true;std::thread(pt::loop).detach();}}
    return mirror_next.exchange(0,std::memory_order_relaxed)!=0;
}
extern "C" int dm_present_async_m(VkImage source,VkFormat format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t,int mirror){
    if(pt_cfg==2){std::lock_guard<std::mutex> render_lock(render_mutex);++pt::jobs;return present_body(source,format,width,height,crop,mirror!=0);}
    std::unique_lock<std::mutex> l(pt::mu);
    if(!pt::started){pt::started=true;std::thread(pt::loop).detach();}
    if(pt::enq-pt::done>=pt::depth){const int64_t t0=pt::now_ns();pt::cv.wait(l,[]{return pt::enq-pt::done<pt::depth;});pt::full_ns+=uint64_t(pt::now_ns()-t0);}
    if(int e=pt::err.load())return e;
    pt::Job &j=pt::ring[(pt::enq+1)%pt::depth];j.src=source;j.fmt=format;j.w=width;j.h=height;j.has_crop=crop!=nullptr;if(crop)memcpy(j.crop,crop,sizeof j.crop);else memset(j.crop,0,sizeof j.crop);j.mirror=mirror!=0;j.ticket=pt::enq+1;
    ++pt::enq;l.unlock();pt::cv.notify_all();return 0;
}
// [mtb] The translation thread's core pin (0 = none): the record thread keeps off it.
extern "C" unsigned dm_present_pin_mask(void){return pin_mask;}
extern "C" int dm_present_pin_tid(void){return pin_tid.load(std::memory_order_relaxed);}
// [DOOM] The record thread's mask from the CPU plan (cpu_topo.c) while the pin is the planned one; 0 = the old "every core but the pin".
extern "C" unsigned dm_present_record_mask(void){const unsigned p=pin_mask;return p && pin_planned && p==dm_cpu_plan()->translate?dm_cpu_plan()->record:0u;}
// Nanoseconds waited until every handed present has its blit submitted / has finished (0 = nothing pending).
extern "C" uint64_t dm_present_wait_submitted(void){std::unique_lock<std::mutex> l(pt::mu);if(pt::sub>=pt::enq)return 0;const int64_t t0=pt::now_ns();pt::cv.wait(l,[]{return pt::sub>=pt::enq;});return uint64_t(pt::now_ns()-t0);}
extern "C" uint64_t dm_present_drain(void){std::unique_lock<std::mutex> l(pt::mu);if(pt::done>=pt::enq)return 0;const int64_t t0=pt::now_ns();pt::cv.wait(l,[]{return pt::done>=pt::enq;});return uint64_t(pt::now_ns()-t0);}
extern "C" int dm_present_last_error(void){return pt::err.load();}
extern "C" void dm_present_queue_lock(void){if(pt_cfg)queue_mutex.lock();}
extern "C" void dm_present_queue_unlock(void){if(pt_cfg)queue_mutex.unlock();}
// Totals since start: presents run by the thread (or inline with =2), the thread's time in them, the engine's waits on a full hand-off ring.
extern "C" void dm_present_thread_stats(uint64_t out[4]){out[0]=pt::jobs.load();out[1]=pt::busy_ns.load();out[2]=pt::full_ns.load();out[3]=0;}
extern "C" void dm_android_vk_set_window(ANativeWindow *window){if(window)ANativeWindow_acquire(window);{std::lock_guard<std::mutex> l(window_mutex);if(desired)ANativeWindow_release(desired);desired=window;++generation;}window_cv.notify_all();}
extern "C" int dm_android_vk_has_window(){return has_window(nullptr);}
// [nonadreno] ASTC / ETC2 / EAC overlay ("variant A": tools/astc_overlay_doom.py, engine/xbg_astc_overlay.h, BUGS_AND_FIXES 2.26).
// Chosen here because both inputs exist only here: the device features (ASTC HDR is an extension that must be enabled at
// vkCreateDevice) and the moment before dm_main mounts the RomFS (hle_fs.c reads DM_ROMFS_OVERLAY at the mount). Overlay folder:
// DM_ASTC_OVERLAY_DIR, else the folder next to DM_ROMFS (Download/DOOM Mobile/astc). It is used only when its manifest parses,
// every overlay file has the size the manifest lists (an interrupted copy to the phone is caught here, not by a short read in
// the game), the RomFS files it was made from have their sizes, and xbg_astc_choose agrees (auto: only without device BC, so
// Adreno is untouched). Two steps around vkCreateDevice (BUGS_AND_FIXES 2.37):
//   astc_overlay_boot (before): the choice; returns whether ASTC HDR must be enabled on the device (variant data, or the
//     self-test forced with DM_GFX_ASTC_HDR_SELFTEST=force / fail on a device that has ASTC HDR).
//   astc_overlay_mount (after, before dm_main): variant data runs the ASTC HDR GPU self-test first (engine/xbg_astc_hdr_vk.h);
//     a failure mounts nohdr instead (BC6H keeps the original index entries and goes through the GPU BC decoder). Then
//     DM_ROMFS_OVERLAY = <dir>/data (nohdr: <dir>/nohdr:<dir>/data) in front of any existing value, and DM_GFX_ASTC_FORMATS =
//     the substituted formats for the engine (xbg_game.cpp bc_store).
// One line each: grep "NVN astc overlay", "NVN astc hdr selftest".
namespace astc_boot {
XbgAstcManifest m;XbgAstcChoice c;std::string dir;char why[256]="";bool have=false;const char *mode_env=nullptr;
int test_mode=XBG_AHS_AUTO;bool device_hdr=false;bool run_test=false;
}
static bool astc_overlay_boot(const VkPhysicalDeviceFeatures &f,bool hdr_feature){
    using namespace astc_boot;
    dir.clear();why[0]=0;have=false;run_test=false;   // a repeated initialization starts over
    mode_env=getenv("DM_GFX_ASTC_OVERLAY");const int mode=xbg_astc_mode(mode_env);
    test_mode=xbg_astc_hdr_selftest_mode(getenv("DM_GFX_ASTC_HDR_SELFTEST"));device_hdr=hdr_feature;
    if(const char *d=getenv("DM_ASTC_OVERLAY_DIR"))dir=d;
    else if(const char *r=getenv("DM_ROMFS")){std::string s=r;while(s.size()>1 && s.back()=='/')s.pop_back();size_t cut=s.find_last_of('/');dir=(cut==std::string::npos?std::string("."):s.substr(0,cut))+"/astc";}
    if(mode!=0 && !dir.empty()){
        if(FILE *mf=fopen((dir+"/manifest.json").c_str(),"rb")){
            std::string text;char buf[4096];size_t n;while(text.size()<(1u<<20) && (n=fread(buf,1,sizeof buf,mf))>0)text.append(buf,n);fclose(mf);
            if(!xbg_astc_manifest_parse(text.c_str(),&m,why,sizeof why)){
                have=true;const char *romfs=getenv("DM_ROMFS");struct stat st;
                for(int i=0;i<m.nfiles && have;i++){const std::string p=dir+"/"+m.file[i];const bool gone=stat(p.c_str(),&st)!=0;
                    if(gone || uint64_t(st.st_size)!=m.file_size[i]){snprintf(why,sizeof why,"%s is %lld bytes, the manifest says %llu (copy incomplete?)",m.file[i],gone?-1ll:(long long)st.st_size,(unsigned long long)m.file_size[i]);have=false;}}
                for(int i=0;i<m.nsources && have;i++){const std::string p=std::string(romfs?romfs:".")+"/"+m.source[i];
                    if(stat(p.c_str(),&st) || uint64_t(st.st_size)!=m.source_size[i]){snprintf(why,sizeof why,"made from another RomFS (%s differs)",m.source[i]);have=false;}}
            }
        }else snprintf(why,sizeof why,"no %s/manifest.json",dir.c_str());
    }
    c=xbg_astc_choose(mode,f.textureCompressionBC,f.textureCompressionASTC_LDR,f.textureCompressionETC2,hdr_feature,have?&m:nullptr);
    // BUGS_AND_FIXES 2.37: BC6H stays on the BC path by default (variant nohdr: native BC on Adreno, GPU BC decode on Mali). With
    // variant data the light-probe array is ASTC HDR, and on the Mi 14 Ultra (Turnip, self-test PASS) it showed white glows (Mali:
    // black rectangles) although every overlay block decodes right; the probe array also takes data that is not overlay data.
    // DM_GFX_ASTC_HDR=1 opts in to variant data again (then the self-test and the sanitizer below apply).
    {const char *he=getenv("DM_GFX_ASTC_HDR");const bool optin=he && *he && atoi(he)!=0;
     if(c.use && c.hdr && !optin){static char whyb[200];snprintf(whyb,sizeof whyb,"%s; BC6H kept on BC: DM_GFX_ASTC_HDR=0",c.why);
        c.hdr=0;c.mask=m.nohdr_mask;c.variant="nohdr";c.why=whyb;}}
    // The GPU self-test: before mounting variant data (auto), or on its own with force / fail (no overlay needed: the test blocks
    // are in the binary). It needs the device's ASTC HDR, which is then enabled even when no overlay uses it.
    const bool forced=test_mode==XBG_AHS_FORCE || test_mode==XBG_AHS_FAIL;
    run_test=test_mode!=XBG_AHS_OFF && hdr_feature && ((c.use && c.hdr) || forced);
    if(forced && !hdr_feature)
        fprintf(stderr,"NVN astc hdr selftest: not run: the device has no ASTC HDR (textureCompressionASTC_HDR=0 or no VK_EXT_texture_compression_astc_hdr) -> nothing to test (DM_GFX_ASTC_HDR_SELFTEST=%s)\n",
            xbg_astc_hdr_selftest_mode_name(test_mode));
    return (c.use && c.hdr) || run_test;
}
static void astc_overlay_mount(VkPhysicalDevice phys,VkDevice dev,VkQueue q,uint32_t fam,const VkPhysicalDeviceFeatures &f){
    using namespace astc_boot;
    if(run_test){
        const XbgAhsRun t=xbg_astc_hdr_selftest_run(phys,dev,q,fam,f.imageCubeArray!=VK_FALSE);
        const bool faked=test_mode==XBG_AHS_FAIL;
        const bool failed=faked || !t.ran || !t.verdict.pass;   // a test that could not run does not vouch for the decoder either
        const bool fallback=failed && c.use && c.hdr;
        char res[640];
        if(!t.ran)snprintf(res,sizeof res,"NOT RUN (%s)",t.why);
        else{const XbgAhsVerdict &v=t.verdict;char first[224]="";
            if(v.first_bad>=0)snprintf(first,sizeof first," first=\"%s\" kind=%s got=%g want=%g",xbg_astc_hdr_tests[v.first_bad].label,v.first_kind==XBG_AHS_FETCH?"fetch":v.first_kind==XBG_AHS_CUBE?"cube":"linear",v.first_got,v.first_want);
            snprintf(res,sizeof res,"%s blocks=%u vectors=%d queries=%u fetch_bad=%u cube_bad=%u linear_bad=%u nonfinite=%u max_rel=%.2g%s edge_bad=%u edge_nonfinite=%u zero_block=%s(%g,%g,%g,%g) ms=%.1f",
                v.pass?"PASS":"FAIL",t.blocks,int(XBG_ASTC_HDR_TEST_BLOCKS),t.queries,v.fetch_bad,v.cube_bad,v.linear_bad,v.nonfinite,v.max_rel,first,v.edge_bad,v.edge_nonfinite,
                v.zero_block_nan?"nan":"finite",v.zero_block[0],v.zero_block[1],v.zero_block[2],v.zero_block[3],t.ms);}
        fprintf(stderr,"NVN astc hdr selftest: %s%s -> %s (DM_GFX_ASTC_HDR_SELFTEST=%s)\n",res,faked?" [failure faked by DM_GFX_ASTC_HDR_SELFTEST=fail]":"",
            !(c.use && c.hdr)?"log only (no ASTC HDR overlay in use)":fallback?"variant nohdr":"variant data",xbg_astc_hdr_selftest_mode_name(test_mode));
        if(fallback){c.hdr=0;c.mask=m.nohdr_mask;c.variant="nohdr";c.why="ASTC HDR self-test failed";}
    }else if(c.use && c.hdr)fprintf(stderr,"NVN astc hdr selftest: skipped (DM_GFX_ASTC_HDR_SELFTEST=off) -> variant data\n");
    // BUGS_AND_FIXES 2.37 bisection: DM_GFX_ASTC_ONLY=<bc3|bc4|bc5|bc7> mounts <dir>/only-<fmt> (an index with only that format's
    // records pointing at the overlay: tools/astc_hdr_selftest_gen.py onlyindex, 14.7 MB) in front of <dir>/data and substitutes only
    // that format; everything else stays real BC. Needs the folder; without it the switch is logged and ignored.
    std::string only_layer;
    if(const char *oe=getenv("DM_GFX_ASTC_ONLY"))if(*oe && c.use){
        const uint32_t bit=xbg_astc_formats_mask(oe);struct stat st;const std::string pi=dir+"/only-"+oe+"/base/gameresources_003.pindex";
        const bool ok=bit && !(bit&(bit-1)) && (bit&c.mask) && !stat(pi.c_str(),&st) && m.nsources && uint64_t(st.st_size)==m.source_size[0];
        fprintf(stderr,"NVN astc overlay: DM_GFX_ASTC_ONLY=%s %s (%s)\n",oe,ok?"used: only this format substituted":"ignored",ok?pi.c_str():
            (!bit || (bit&(bit-1)))?"one of bc3 bc4 bc5 bc7":!(bit&c.mask)?"not substituted by this variant":"index missing or of another size: tools/astc_hdr_selftest_gen.py onlyindex");
        if(ok){c.mask=bit;only_layer=dir+"/only-"+oe;static std::string v;v=std::string("only-")+oe;c.variant=v.c_str();}}
    char fm[64];xbg_astc_formats_str(c.mask,fm,sizeof fm);
    if(c.use){
        std::string layers=only_layer.empty()?dir+"/"+c.variant:only_layer;if(!c.hdr || !only_layer.empty())layers+=":"+dir+"/data";
        if(const char *o=getenv("DM_ROMFS_OVERLAY"))if(*o){layers+=":";layers+=o;}
        setenv("DM_ROMFS_OVERLAY",layers.c_str(),1);setenv("DM_GFX_ASTC_FORMATS",fm,1);
    }else unsetenv("DM_GFX_ASTC_FORMATS");
    // stderr: the log file and logcat (tag dm), like the engine's lines
    fprintf(stderr,"NVN astc overlay: %s variant=%s formats=%s (%s%s%s) dir=%s device bc=%u astcLdr=%u etc2=%u astcHdr=%u DM_GFX_ASTC_OVERLAY=%s\n",
        c.use?"USED":"not used",c.use?c.variant:"-",c.use?fm:"-",c.why,*why?"; ":"",why,dir.empty()?"-":dir.c_str(),unsigned(f.textureCompressionBC),
        unsigned(f.textureCompressionASTC_LDR),unsigned(f.textureCompressionETC2),unsigned(device_hdr),mode_env?mode_env:"auto");
}
extern "C" int dm_android_vk_initialize(const char *path,char *error,size_t bytes){
    if(path){std::string dir=path;size_t cut=dir.find_last_of('/');if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-native").c_str(),"rb")){present_native=fgetc(m)!='0';fclose(m);}
        if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-timing").c_str(),"rb")){present_timing=true;fclose(m);}}
    std::lock_guard<std::mutex> render_lock(render_mutex);
    auto fail=[&](const char *what,VkResult r)->int{if(error&&bytes)snprintf(error,bytes,"%s: VkResult %d",what,int(r));if(!ready)cleanup_initialization();return int(r?r:VK_ERROR_INITIALIZATION_FAILED);};
    if(ready||!path||!*path)return fail("Initialization requires an external shader directory and an uninitialized renderer",VK_ERROR_INITIALIZATION_FAILED);
    // 每次初始化从缺省的 120 Hz 重新读标记。 | Each initialization starts again from the 120 Hz default before the marker.
    frame_rate_vote=120;
    upscale::enabled();   // [DOOM] DM_GFX_UPSCALE: decided (and logged) once, before the first swapchain and any present-thread caller
    // [DOOM] Frame rate limit above 60 (DM_PRESENT_MAX_HZ 90 / 120, DmActivity): vote that rate, so a panel offering 60/90/120 runs at 90
    // for a 90 cap (a 120 vote would leave 120 Hz, where the cap paces at 60). DmActivity also asks for that display mode.
    if(const char *cap=getenv("DM_PRESENT_MAX_HZ")){float c=float(atof(cap));if(c>60.5f && c<=240)frame_rate_vote=c;}
    {std::string dir=path;size_t cut=dir.find_last_of('/');if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-frame-rate").c_str(),"rb")){float v=0;frame_rate_vote=fscanf(m,"%f",&v)==1 && v>=0 && v<=240?v:0;fclose(m);}}

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.pApplicationName="dm-android";ai.apiVersion=VK_API_VERSION_1_1;
    const char *ext[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&ai;ic.enabledExtensionCount=2;ic.ppEnabledExtensionNames=ext;
    // DM_VK_VALIDATION=1 (diagnostics): the Khronos validation layer packaged in the APK (tools/build_apk.ps1 -Validation; a
    // debuggable app's own layers), with synchronization validation and best practices; messages go to logcat "dm-vvl", each
    // message id at most 20 times. =2: core validation only (faster).
    const char *vvl_env=getenv("DM_VK_VALIDATION");int vvl=vvl_env?atoi(vvl_env):0;const char *layer="VK_LAYER_KHRONOS_validation";
    VkValidationFeatureEnableEXT vf_on[]={VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
    VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};vf.enabledValidationFeatureCount=2;vf.pEnabledValidationFeatures=vf_on;
    if(vvl){auto enum_layers=(PFN_vkEnumerateInstanceLayerProperties)vkGetInstanceProcAddr(nullptr,"vkEnumerateInstanceLayerProperties");
        uint32_t ln=0;bool have=false;if(enum_layers){enum_layers(&ln,nullptr);std::vector<VkLayerProperties> lp(ln);enum_layers(&ln,lp.data());for(auto &l:lp)if(!strcmp(l.layerName,layer))have=true;}
        __android_log_print(ANDROID_LOG_INFO,"dm-vvl","validation layer %s (%u layers)",have?"found":"NOT FOUND",ln);
        if(have){ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=3;if(vvl==1)ic.pNext=&vf;}else vvl=0;}
    VkResult r=vkCreateInstance(&ic,nullptr,&instance);if(r)return fail("vkCreateInstance",r);
    if(vvl){static std::mutex vm;static std::unordered_map<int32_t,unsigned> seen;
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        mi.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback=[](VkDebugUtilsMessageSeverityFlagBitsEXT sev,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *d,void*)->VkBool32{
            unsigned c;{std::lock_guard<std::mutex> g(vm);c=++seen[d->messageIdNumber];}
            if(c<=20 || (c&(c-1))==0)__android_log_print(sev>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT?ANDROID_LOG_ERROR:ANDROID_LOG_WARN,"dm-vvl","#%u %s %s",c,d->pMessageIdName?d->pMessageIdName:"?",d->pMessage?d->pMessage:"");
            return VK_FALSE;};
        auto mk=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");VkDebugUtilsMessengerEXT m;
        if(mk)mk(instance,&mi,nullptr,&m);}
    uint32_t n=0;vkEnumeratePhysicalDevices(instance,&n,nullptr);std::vector<VkPhysicalDevice> devices(n);vkEnumeratePhysicalDevices(instance,&n,devices.data());if(!n)return fail("No Vulkan device",VK_ERROR_INITIALIZATION_FAILED);
    physical=devices[0];
    // 启动期间也可能旋转/切后台；等待新窗口，不把瞬时撤回误报为启动失败。
    // Rotation/backgrounding can race startup too; wait for a live window instead of failing boot.
    for(;;){wait_window();r=VkResult(create_surface());if(r==VK_NOT_READY)continue;if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}if(r)return fail("Android surface",r);break;}
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> q(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q.data());bool found=false;
    for(uint32_t i=0;i<n;i++){VkBool32 p=0;vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&p);if(p&&(q[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;found=true;break;}}if(!found)return fail("No graphics/compute/present family",VK_ERROR_FEATURE_NOT_PRESENT);
    VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&draw;vkGetPhysicalDeviceFeatures2(physical,&features);
    vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties> available(n);vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,available.data());bool mirror=false,minmax=false,indirect_count=false,ahb=false,foreign=false,format_list=false;for(auto e:available){if(std::string(e.extensionName)==VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)format_list=true;if(std::string(e.extensionName)==VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)ahb=true;if(std::string(e.extensionName)==VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)foreign=true;if(std::string(e.extensionName)==VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)indirect_count=true;if(std::string(e.extensionName)==VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)mirror=true;if(std::string(e.extensionName)==VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME)minmax=true;}
    {std::string all;for(auto &e:available){all+=e.extensionName;all+=' ';}   // 诊断：设备扩展全表（每次启动一行）| Diagnostics: the full device extension list (one line per launch)
     for(size_t at=0;at<all.size();at+=900)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Device extensions [%zu]: %s",at/900,all.substr(at,900).c_str());
     VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};VkPhysicalDeviceProperties2 pr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};pr.pNext=&host;vkGetPhysicalDeviceProperties2(physical,&pr);
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Device %s api=%u.%u.%u driver=%#x minImportedHostPointerAlignment=%llu",pr.properties.deviceName,VK_VERSION_MAJOR(pr.properties.apiVersion),VK_VERSION_MINOR(pr.properties.apiVersion),VK_VERSION_PATCH(pr.properties.apiVersion),pr.properties.driverVersion,(unsigned long long)host.minImportedHostPointerAlignment);}
    // [nonadreno] ASTC overlay (astc_overlay_boot above), decided before the device exists: ASTC HDR is queried here and enabled
    // only when the chosen overlay variant uses it or its self-test is forced (never on Adreno in auto mode, where the overlay is
    // not used at all); the variant is mounted after vkCreateDevice (astc_overlay_mount, after the ASTC HDR self-test).
    VkPhysicalDeviceTextureCompressionASTCHDRFeaturesEXT astc_hdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES_EXT};
    bool astc_hdr_ext=false;for(const auto &x:available)if(!strcmp(x.extensionName,VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME))astc_hdr_ext=true;
    if(astc_hdr_ext){VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};q.pNext=&astc_hdr;vkGetPhysicalDeviceFeatures2(physical,&q);}
    const bool want_astc_hdr=astc_overlay_boot(features.features,astc_hdr_ext && astc_hdr.textureCompressionASTC_HDR);
    // [DOOM] DM_VK_ROBUST=0: device without robustBufferAccess (diagnostic: out-of-range buffer reads read memory instead of 0;
    // DM_GFX_BONES_SSBO on the stock Adreno 740 driver, BUGS_AND_FIXES 2.34).
    {const char *e=getenv("DM_VK_ROBUST");if(e && *e && !atoi(e)){features.features.robustBufferAccess=VK_FALSE;__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","robustBufferAccess off (DM_VK_ROBUST=0)");}}
    std::vector<const char*> dext={VK_KHR_SWAPCHAIN_EXTENSION_NAME};if(mirror)dext.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    if(want_astc_hdr){dext.push_back(VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);astc_hdr.textureCompressionASTC_HDR=VK_TRUE;astc_hdr.pNext=draw.pNext;draw.pNext=&astc_hdr;}
    // NVN min/max reduction samplers (depth pyramids): extension only (the instance is Vulkan 1.1); without it reductions sample as average.
    if(minmax)dext.push_back(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
    // [DOOM] GPU-driven culling writes DrawElementsIndirect arguments and counts on the GPU; replayed as vkCmdDraw*IndirectCount.
    if(indirect_count)dext.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
    // [DOOM] format lists keep UBWC compression on images that need a second view format (xbg_game.cpp upgrade_mutable)
    if(format_list){dext.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);dm_vk_format_list=1;}
    // [nonadreno][afbc] VK_EXT_image_compression_control: query only (never requests a compression), so the engine can log whether the
    // driver compressed each image (AFBC / fixed rate / none; NVN afbc summary); plus a per-format probe here. VK_EXT/KHR_load_store_op_none:
    // STORE_OP_NONE for attachment aspects a render pass does not write (DM_GFX_STORE_NONE). Both off on Adreno unless DM_GFX_AFBC_QUERY=1 /
    // DM_GFX_STORE_NONE / DM_GFX_SIM_GPU ask, so the Adreno device is created exactly as before.
    VkPhysicalDeviceImageCompressionControlFeaturesEXT compression_ctl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT};
    {VkPhysicalDeviceProperties pp{};vkGetPhysicalDeviceProperties(physical,&pp);dm_vk_compression_control=dm_vk_store_none=0;
     auto env_on=[](const char *n){const char *e=getenv(n);return e && *e && strcmp(e,"0")!=0;};
     const bool other=pp.vendorID!=0x5143 || env_on("DM_GFX_SIM_GPU");
     bool cc=false,lsn_ext=false,lsn_khr=false;for(const auto &x:available){if(!strcmp(x.extensionName,VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME))cc=true;
        if(!strcmp(x.extensionName,VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))lsn_ext=true;if(!strcmp(x.extensionName,"VK_KHR_load_store_op_none"))lsn_khr=true;}
     if(cc && (other || env_on("DM_GFX_AFBC_QUERY"))){VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};q.pNext=&compression_ctl;vkGetPhysicalDeviceFeatures2(physical,&q);
        if(compression_ctl.imageCompressionControl){dext.push_back(VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME);compression_ctl.pNext=draw.pNext;draw.pNext=&compression_ctl;dm_vk_compression_control=1;}
        // Which formats / usages the driver would compress (the image-level answer comes from the engine per created image).
        auto probe_fn=(PFN_vkGetPhysicalDeviceImageFormatProperties2)vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceImageFormatProperties2");
        if(probe_fn){static const struct{VkFormat f;const char *n;bool ds;} fmts[]={{VK_FORMAT_R8G8B8A8_UNORM,"rgba8",false},{VK_FORMAT_R8G8B8A8_SRGB,"rgba8s",false},{VK_FORMAT_B8G8R8A8_UNORM,"bgra8",false},
                {VK_FORMAT_A2B10G10R10_UNORM_PACK32,"rgb10a2",false},{VK_FORMAT_B10G11R11_UFLOAT_PACK32,"r11g11b10f",false},{VK_FORMAT_R16G16_SFLOAT,"rg16f",false},{VK_FORMAT_R32_SFLOAT,"r32f",false},
                {VK_FORMAT_R8_UNORM,"r8",false},{VK_FORMAT_R8G8_UNORM,"rg8",false},{VK_FORMAT_R16G16B16A16_SFLOAT,"rgba16f",false},{VK_FORMAT_D16_UNORM,"d16",true},{VK_FORMAT_X8_D24_UNORM_PACK32,"x8d24",true},
                {VK_FORMAT_D24_UNORM_S8_UINT,"d24s8",true},{VK_FORMAT_D32_SFLOAT,"d32f",true},{VK_FORMAT_D32_SFLOAT_S8_UINT,"d32fs8",true}};
            std::string line;
            for(const auto &f:fmts){
                const VkImageUsageFlags base=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|(f.ds?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
                char r[3]={'-','-','-'};   // plain, +storage, +mutable (no list): c = compressed (default), f = fixed rate, n = none, - = unsupported
                for(int v=0;v<3;v++){if(f.ds && v)continue;
                    VkPhysicalDeviceImageFormatInfo2 in{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};in.format=f.f;in.type=VK_IMAGE_TYPE_2D;in.tiling=VK_IMAGE_TILING_OPTIMAL;in.usage=base|(v==1?VK_IMAGE_USAGE_STORAGE_BIT:0);in.flags=v==2?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:0;
                    VkImageCompressionPropertiesEXT cp{VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT};VkImageFormatProperties2 out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};out.pNext=&cp;
                    if(probe_fn(physical,&in,&out)==VK_SUCCESS)r[v]=(cp.imageCompressionFlags&VK_IMAGE_COMPRESSION_DISABLED_EXT)?'n':(cp.imageCompressionFlags&VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT)?'f':'c';}
                line+=' ';line+=f.n;line+='=';line.append(r,3);}
            __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN afbc probe (plain/+storage/+mutable: c compressed, f fixed-rate, n none, - unsupported):%s",line.c_str());}}
     else __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN afbc probe: VK_EXT_image_compression_control %s",cc?"present, not enabled on this GPU family (DM_GFX_AFBC_QUERY=1)":"not exposed by the driver");
     const char *sn=getenv("DM_GFX_STORE_NONE");
     if((lsn_ext || lsn_khr) && (sn && *sn?atoi(sn)!=0:other)){dext.push_back(lsn_khr?"VK_KHR_load_store_op_none":VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);dm_vk_store_none=1;}
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN store ops: load_store_op_none ext=%d khr=%d enabled=%d image_compression_control=%d enabled=%d",int(lsn_ext),int(lsn_khr),dm_vk_store_none,int(cc),dm_vk_compression_control);}
    // [DOOM] zero-copy guest memory imports AHardwareBuffer pages as buffer memory (xbg_game.cpp zc)
    if(ahb && foreign){dext.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);dext.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);}
    // Diagnostic (marker gfx-pipe-stats): the driver's per-pipeline statistics (VK_KHR_pipeline_executable_properties).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    // [perf4] also DM_GFX_PIPE_STATS (env) — the engine logs the statistics of the hottest pipelines per window.
    auto pipe_stats_env=[]{const char *e=getenv("DM_GFX_PIPE_STATS");return e && *e && *e!='0';};
    bool pe=false;for(const auto &x:available)if(!strcmp(x.extensionName,VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME))pe=true;
    if(FILE *ps=[&]{std::string dir=path;size_t cut=dir.find_last_of('/');FILE *f=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-pipe-stats").c_str(),"rb");if(!f && pe && pipe_stats_env())f=fopen("/proc/self/stat","rb");return f;}()){fclose(ps);dext.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);pef.pipelineExecutableInfo=VK_TRUE;pef.pNext=draw.pNext;draw.pNext=&pef;}
    float priority[2]={1,0.5f};VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=priority;
    {const char *e=getenv("DM_TEX_DISCARD");const bool want=e && atoi(e)>0;if(want && q[family].queueCount>=2)qc.queueCount=2;   // [DOOM] restore worker queue (default off)
     if(want)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","DM_TEX_DISCARD: family %u has %u queues -> aux queue %s",family,q[family].queueCount,qc.queueCount==2?"created":"unavailable (texture discard stays off)");}
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&draw;dc.pEnabledFeatures=&features.features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.enabledExtensionCount=uint32_t(dext.size());dc.ppEnabledExtensionNames=dext.data();if((r=vkCreateDevice(physical,&dc,nullptr,&device)))return fail("vkCreateDevice",r);vkGetDeviceQueue(device,family,0,&queue);if(qc.queueCount==2){vkGetDeviceQueue(device,family,1,&dm_vk_aux_queue);dm_vk_aux_family=family;}
    astc_overlay_mount(physical,device,queue,family,features.features);   // [nonadreno] before dm_main mounts the RomFS (BUGS_AND_FIXES 2.37)
    for(;;){
        wait_window();
        if(!surface||window_changed()){
            r=VkResult(create_surface());if(r==VK_NOT_READY)continue;
            if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
            if(r)return fail("Android surface",r);
        }
        r=VkResult(create_swap());
        if(r==VK_NOT_READY||r==VK_ERROR_OUT_OF_DATE_KHR){wait_resize();continue;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return fail("Swapchain",r);break;
    }
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;if((r=vkCreateCommandPool(device,&cp,nullptr,&command_pool)))return fail("Present command pool",r);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;if((r=vkAllocateCommandBuffers(device,&ca,&command)))return fail("Present command",r);
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};if((r=vkCreateSemaphore(device,&sc,nullptr,&acquired)))return fail("Acquire semaphore",r);VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if((r=vkCreateFence(device,&fc,nullptr,&fence)))return fail("Present fence",r);
    {std::string dir=path;size_t cut=dir.find_last_of('/');FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-overlap").c_str(),"rb");overlap=true;if(m){overlap=fgetc(m)!='0';fclose(m);}
     auto open_marker=[&](const char *name){return fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+name).c_str(),"rb");};
     pin_mask=dm_cpu_plan()->translate;pin_ab=0;pin_state=-1;pin_have_original=false;pin_presents=pin_drifts=0;
     if(FILE *p=open_marker("/gfx-pin-cpu")){if(fscanf(p,"%x",&pin_mask)!=1)pin_mask=0;fclose(p);}
     if(FILE *n=open_marker("/gfx-no-pin")){pin_mask=0;fclose(n);}
     // [perf4] DM_GFX_PIN_CPU=<hex mask> (env; 0 = no pin): same as the gfx-pin-cpu marker, e.g. f0 = X4 + all A720 instead of
     // the X4 alone (the present thread is also the translation thread with DM_NVN_SYNC=1).
     if(const char *e=getenv("DM_GFX_PIN_CPU");e && *e){unsigned v=0;if(sscanf(e,"%x",&v)==1)pin_mask=v;}
     pin_planned=pin_mask==dm_cpu_plan()->translate;
     if(FILE *t=open_marker("/gfx-ab-target")){char target[16]={0};if(fscanf(t,"%15s",target)==1 && !strcmp(target,"pin"))if(FILE *a=open_marker("/gfx-ab-toggle")){if(fscanf(a,"%d",&pin_ab)!=1)pin_ab=0;fclose(a);}fclose(t);}
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN pin mask=%x ab=%d %s (markers gfx-no-pin, gfx-pin-cpu, gfx-ab-target=pin)",pin_mask,pin_ab,pin_planned?"cpu plan":"set by hand");
     if(FILE *b=open_marker("/gfx-membench")){fclose(b);membench();}}
    if(overlap){if((r=vkCreateCommandPool(device,&cp,nullptr,&overlap_pool)))return fail("Present command pool",r);ca.commandPool=overlap_pool;if((r=vkAllocateCommandBuffers(device,&ca,&overlap_command)))return fail("Present command",r);
        if((r=vkCreateSemaphore(device,&sc,nullptr,&overlap_acquired)))return fail("Acquire semaphore",r);if((r=vkCreateFence(device,&fc,nullptr,&overlap_fence)))return fail("Present fence",r);}
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Present overlap=%d (marker gfx-present-overlap)",int(overlap));
    shader_path=path;dm_vk_config config{};config.size=sizeof(config);config.game.size=sizeof(config.game);config.game.physical=physical;config.game.device=device;config.game.queue=queue;config.game.queue_family=family;config.game.enabled_core=features.features;config.game.enabled_shader_draw_parameters=draw.shaderDrawParameters;config.game.fence_timeout_ns=10000000000ull;config.game.shader_directory=shader_path.c_str();config.game.present=present;config.game.log=log_message;config.game.fatal=fatal;config.sampler_mirror_clamp_to_edge=mirror;config.sampler_filter_minmax=minmax;config.has_window=has_window;config.display_hz=0; // Query actual display refresh in frontend; do not guess 60 Hz.
    int status=dm_vk_initialize(&config,error,bytes);if(!status)ready=true;else cleanup_initialization();return status;
}
