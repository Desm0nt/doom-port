// [DOOM] Live texture classes (DM_TEX_CLASS) and release of static guest texture copies (DM_TEX_DISCARD, default off).
// Included by xbg_game.cpp inside its anonymous namespace, after Image / images / pools / zc.
//
// Problem: every sampled texture lives twice in RAM -- the game's block-linear copy in its NVN pool (guest memory, the game's
// CPU writes it and may read it) and our optimal-tiled Vulkan image. DM_TEX_DISCARD=1 releases the guest copy of textures
// that were uploaded and then left alone (static): the pages are made PROT_NONE and freed (MADV_DONTNEED), and are rebuilt
// from the Vulkan image on the first touch.
//
//   select (translation thread, every 10 s, engine lock held): uploaded >= DM_TEX_DISCARD_AGE s ago and never since (no full
//     re-upload, never a partial one: VT caches excluded), not GPU-written (render targets / storage / copy destinations),
//     plain colour image (not CPU-decoded BC, not depth, not linear, single-sampled, NVN flags without display / video /
//     image / sparse / linear-RT), write-protected by the dirty tracker and clean since its upload stamp, layout GENERAL,
//     not on a zero-copy (dma-buf) pool, DM_TEX_DISCARD_MIN_KB..DM_TEX_DISCARD_MAX_MB. Only the pages fully inside the
//     texture storage are released (edge pages are shared with neighbours).
//   discard (worker thread "dm-texdiscard"): round-trip check -- swizzle(deswizzle(guest)) over zeroed storage must equal the
//     guest bytes of those pages (padding not zero => not reproducible => never discarded); then nvn_dirty_gone_begin (all
//     pages still write-protected and not written after the stamp) -> commit (PROT_NONE + DONTNEED).
//   restore (worker): a guest fault (read or write; crash.c fault hook -> nvn_dirty.c on_fault), a host pre-write
//     (dm_guest_write_prepare) or an engine undo asks for it. vkCmdCopyImageToBuffer on the aux queue (dm_vk_aux_queue, a
//     second queue of the graphics family created by vulkan_present.cpp only with DM_TEX_DISCARD=1) into a host-visible
//     buffer, nvn_swizzle into a fresh anonymous mapping, made read-only and mremap'ed over the range in one step (no thread
//     ever sees a half-restored page), pages back to write-protected with their old generation. A write then faults as before
//     and the texture is re-uploaded as dirty, exactly as without the discard.
//   undo (engine, synchronous): before the image is destroyed, replaced (mutable upgrade), moved out of GENERAL or written by
//     the GPU / a new upload (provenance_write) its guest copy is restored. The worker never takes the engine lock, so waiting
//     for it under that lock (or from a signal handler) cannot deadlock.
// Logs: "NVN TEXCLASS ..." every DM_TEX_CLASS s (default 60 with DM_TEX_DISCARD=1, else off) and "NVN TEXDISCARD ..." every 10 s.
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif
namespace texdiscard {
inline uint64_t now_ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
long env_long(const char *name,long def){const char *e=getenv(name);return e && *e?atol(e):def;}
int state_on=-1;uint64_t age_ns=60000000000ull,min_bytes=256u<<10,max_bytes=64u<<20,class_period_ns=0,scan_budget=256u<<20;size_t zc_min=512u<<20;
uintptr_t pg=4096;
bool on(){
    if(state_on>=0)return state_on>0;
    pg=uintptr_t(getpagesize());
    const char *e=getenv("DM_TEX_DISCARD");const bool want=e && atoi(e)>0;
    age_ns=uint64_t(std::max(5L,env_long("DM_TEX_DISCARD_AGE",60)))*1000000000ull;
    min_bytes=uint64_t(std::max(16L,env_long("DM_TEX_DISCARD_MIN_KB",256)))<<10;
    max_bytes=uint64_t(std::max(1L,env_long("DM_TEX_DISCARD_MAX_MB",64)))<<20;
    zc_min=size_t(std::max(1L,env_long("DM_TEX_DISCARD_ZC_MIN_MB",512)))<<20;
    scan_budget=uint64_t(std::max(1L,env_long("DM_TEX_DISCARD_SCAN_MB",256)))<<20;
    const long cp=env_long("DM_TEX_CLASS",want?60:0);class_period_ns=cp>0?uint64_t(cp)*1000000000ull:0;
    const char *why=nullptr;
    if(want){
        if(!nvn_dirty_gone_begin || !nvn_dirty_gone_commit || !nvn_dirty_restore_begin || !nvn_dirty_restore_commit || !nvn_dirty_gone_take || !nvn_dirty_any_gone || !nvn_dirty_since || !nvn_dirty_tracked)why="dirty tracker without discard support";
        else if(!&dm_vk_aux_queue || !&dm_vk_aux_family || dm_vk_aux_queue==VK_NULL_HANDLE)why="no second queue in the graphics queue family (vulkan_present.cpp)";
        else if(!tex_dirty)why="texture dirty tracking is off (marker gfx-tex-dirty / DM_GFX_TEX_DIRTY)";
    }
    state_on=want && !why;
    if(want || class_period_ns)xbg_logf("NVN DM_TEX_DISCARD=%d%s%s (age %llu s, %llu KB..%llu MB, zero-copy kept off pools >= %zu MB, %llu MB per scan); DM_TEX_CLASS every %llu s",
        state_on,why?" OFF: ":"",why?why:"",(unsigned long long)(age_ns/1000000000ull),(unsigned long long)(min_bytes>>10),(unsigned long long)(max_bytes>>20),zc_min>>20,
        (unsigned long long)(scan_budget>>20),(unsigned long long)(class_period_ns/1000000000ull));
    return state_on>0;
}
size_t zc_min_bytes(){on();return zc_min;}
void note_upload(Image &im,bool partial){const uint64_t n=now_ns();if(!im.up_first_ns)im.up_first_ns=n;im.up_last_ns=n;if(partial)++im.up_partial;else ++im.up_full;}

enum St{PENDING,WORKING,GONE,RESTORING,RESTORED,FAILED,CANCELLED};
struct Rec{uint64_t id=0;uintptr_t mem=0;uint64_t size=0;uintptr_t lo=0,hi=0;uint64_t stamp=0;nvn_texdesc d{};VkImage image{};
    uint32_t w=1,h=1,depth=1,levels=1,layers=1,bpp=0;int bw=1,bh=1;bool blocks=false;St st=PENDING;bool permanent=false,by_undo=false;};
std::mutex mu;std::condition_variable cv;
std::unordered_map<uint64_t,std::shared_ptr<Rec>> recs;   // engine-owned (scan / undo), under mu
std::map<uintptr_t,std::shared_ptr<Rec>> gone;            // released ranges by lo, under mu
std::deque<std::shared_ptr<Rec>> jobs;uint64_t next_id=1;bool worker_started=false;std::atomic<bool> worker_failed{false};
std::atomic<uint64_t> discards{0},released_total{0},gone_bytes{0},gone_count{0},restores{0},restores_undo{0},restore_bytes{0},restore_ns{0},discard_ns{0},
    undos{0},cancels{0},fail_verify{0},fail_written{0},fail_busy{0},fail_gen{0},fail_overlap{0};

// Per-subresource layout of the linear staging data, identical to the full upload in get_image_slow (layer-major, 16-aligned).
struct Sub{uint32_t level,layer,w,h,d;size_t off,pitch,bytes;};
size_t plan(const Rec &r,std::vector<Sub> &subs){
    size_t total=0;
    for(uint32_t layer=0;layer<r.layers;layer++)for(uint32_t level=0;level<r.levels;level++){
        const uint32_t w=std::max(1u,r.w>>level),h=std::max(1u,r.h>>level),d=std::max(1u,r.depth>>level);
        const size_t begin=(total+15)&~size_t(15);size_t pitch,bytes;
        if(r.blocks){const uint32_t bx=(w+r.bw-1)/r.bw,by=(h+r.bh-1)/r.bh;pitch=size_t(bx)*r.bpp;bytes=pitch*by*d;}
        else{pitch=size_t(w)*r.bpp;bytes=pitch*h*d;}
        subs.push_back({level,layer,w,h,d,begin,pitch,bytes});total=begin+bytes;}
    return total;
}

// ---- worker: Vulkan objects on the aux queue (only the worker uses them) ----
VkCommandPool wpool{};VkCommandBuffer wcmd{};VkFence wfence{};
bool vk_init(){
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=dm_vk_aux_family;
    if(vkCreateCommandPool(cfg.device,&cp,nullptr,&wpool))return false;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=wpool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
    if(vkAllocateCommandBuffers(cfg.device,&ca,&wcmd))return false;
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};return vkCreateFence(cfg.device,&fc,nullptr,&wfence)==VK_SUCCESS;
}
uint32_t host_type(uint32_t bits,bool &coherent){
    const VkMemoryPropertyFlags want[2]={VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    for(auto f:want)for(uint32_t i=0;i<memories.memoryTypeCount;i++)if((bits>>i&1) && (memories.memoryTypes[i].propertyFlags&f)==f){coherent=(memories.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;return i;}
    return UINT32_MAX;
}
// Image -> host-visible buffer in the upload layout; returns the mapping (caller frees buf/mem). Fatal on GPU errors: the guest
// pages are gone, there is nothing correct to fall back to.
void *readback(const Rec &r,const std::vector<Sub> &subs,size_t total,VkBuffer &buf,VkDeviceMemory &mem){
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=std::max<size_t>(total,16);bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(cfg.device,&bc,nullptr,&buf))xbg_fatalf("DM_TEX_DISCARD: restore buffer (%zu bytes) failed",total);
    VkMemoryRequirements q;vkGetBufferMemoryRequirements(cfg.device,buf,&q);bool coherent=false;const uint32_t type=host_type(q.memoryTypeBits,coherent);
    if(type==UINT32_MAX)xbg_fatalf("DM_TEX_DISCARD: no host-visible memory type for the restore buffer");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=q.size;ai.memoryTypeIndex=type;
    if(vkAllocateMemory(cfg.device,&ai,nullptr,&mem) || vkBindBufferMemory(cfg.device,buf,mem,0))xbg_fatalf("DM_TEX_DISCARD: restore memory (%zu bytes) failed",total);
    std::vector<VkBufferImageCopy> regions;regions.reserve(subs.size());
    for(auto &s:subs){VkBufferImageCopy c{};c.bufferOffset=s.off;c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,s.level,s.layer,1};c.imageExtent={s.w,s.h,s.d};regions.push_back(c);}
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(wcmd,0);vkBeginCommandBuffer(wcmd,&bi);
    // the image stays GENERAL; this only makes earlier writes (its upload, long completed on the main queue) visible to the copy
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;ib.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    ib.oldLayout=ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.image=r.image;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,r.levels,0,r.layers};
    vkCmdPipelineBarrier(wcmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    vkCmdCopyImageToBuffer(wcmd,r.image,VK_IMAGE_LAYOUT_GENERAL,buf,uint32_t(regions.size()),regions.data());
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb.buffer=buf;bb.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(wcmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&bb,0,nullptr);
    vkEndCommandBuffer(wcmd);vkResetFences(cfg.device,1,&wfence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&wcmd;
    VkResult res=vkQueueSubmit(dm_vk_aux_queue,1,&si,wfence);
    if(!res)res=vkWaitForFences(cfg.device,1,&wfence,VK_TRUE,10000000000ull);
    if(res)xbg_fatalf("DM_TEX_DISCARD: image readback failed (VkResult %d) for guest %#llx+%llu",int(res),(unsigned long long)r.mem,(unsigned long long)r.size);
    void *map=nullptr;if(vkMapMemory(cfg.device,mem,0,VK_WHOLE_SIZE,0,&map) || !map)xbg_fatalf("DM_TEX_DISCARD: restore buffer map failed");
    if(!coherent){VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=mem;mr.size=VK_WHOLE_SIZE;vkInvalidateMappedMemoryRanges(cfg.device,1,&mr);}
    return map;
}
// Rebuild [lo,hi) from the image: zeroed storage + swizzle (the discard check proved this reproduces the guest bytes), read-only,
// then mremap'ed over the PROT_NONE range in one step.
void restore(Rec &r){
    const size_t len=r.hi-r.lo;
    if(!nvn_dirty_restore_begin((const void*)r.lo,len)){return;}   // nothing GONE any more
    std::vector<Sub> subs;const size_t total=plan(r,subs);VkBuffer buf{};VkDeviceMemory mem{};
    const uint8_t *lin=static_cast<const uint8_t*>(readback(r,subs,total,buf,mem));
    const uintptr_t floor=r.mem&~(pg-1),ceil=(r.mem+r.size+pg-1)&~(pg-1);const size_t tlen=ceil-floor;
    uint8_t *t=static_cast<uint8_t*>(mmap(nullptr,tlen,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    if(t==MAP_FAILED)xbg_fatalf("DM_TEX_DISCARD: restore mapping of %zu bytes failed errno=%d",tlen,errno);
    uint8_t *storage=t+(r.mem-floor);
    for(auto &s:subs)if(nvn_swizzle(&r.d,s.level,s.layer,lin+s.off,s.pitch,storage))xbg_fatalf("DM_TEX_DISCARD: swizzle failed level %u layer %u",s.level,s.layer);
    vkUnmapMemory(cfg.device,mem);vkDestroyBuffer(cfg.device,buf,nullptr);vkFreeMemory(cfg.device,mem,nullptr);
    uint8_t *inner=t+(r.lo-floor);
    mprotect(inner,len,PROT_READ);
    void *moved=reinterpret_cast<void*>(syscall(__NR_mremap,inner,len,len,MREMAP_MAYMOVE|MREMAP_FIXED,reinterpret_cast<void*>(r.lo)));
    if(moved!=reinterpret_cast<void*>(r.lo)){   // should not happen; copy in place (other threads may glimpse the page while it fills)
        static std::atomic<int> told{0};if(!told++)xbg_logf("【红】NVN TEXDISCARD mremap over %#llx+%zu failed errno=%d: restoring by copy",(unsigned long long)r.lo,len,errno);
        mprotect(reinterpret_cast<void*>(r.lo),len,PROT_READ|PROT_WRITE);memcpy(reinterpret_cast<void*>(r.lo),inner,len);mprotect(reinterpret_cast<void*>(r.lo),len,PROT_READ);
        munmap(t,tlen);
    }else{if(r.lo>floor)munmap(t,r.lo-floor);if(ceil>r.hi)munmap(inner+len,ceil-r.hi);}
    nvn_dirty_restore_commit((const void*)r.lo,len);
}
// The round trip the restore will do, on the current guest bytes (the pages are still readable and write-protected).
bool reproducible(const Rec &r){
    std::vector<Sub> subs;const size_t total=plan(r,subs);std::vector<uint8_t> lin(total),z(size_t(r.size),0);
    for(auto &s:subs){if(nvn_deswizzle(&r.d,s.level,s.layer,reinterpret_cast<const void*>(r.mem),lin.data()+s.off,s.pitch))return false;
        if(nvn_swizzle(&r.d,s.level,s.layer,lin.data()+s.off,s.pitch,z.data()))return false;}
    return memcmp(reinterpret_cast<const void*>(r.lo),z.data()+(r.lo-r.mem),r.hi-r.lo)==0;
}
void finish(const std::shared_ptr<Rec> &j,St st,bool permanent,std::atomic<uint64_t> *counter){
    if(counter)++*counter;{std::lock_guard<std::mutex> l(mu);j->st=st;j->permanent=permanent;if(st==GONE)gone[j->lo]=j;}cv.notify_all();
}
void do_discard(const std::shared_ptr<Rec> &j){
    const uint64_t t0=now_ns();const size_t len=j->hi-j->lo;
    // a page of the storage already released through an overlapping texture: the worker must not read it (it would wait for itself)
    if(nvn_dirty_any_gone(reinterpret_cast<const void*>(j->mem),size_t(j->size))){finish(j,FAILED,true,&fail_overlap);return;}
    if(!reproducible(*j)){const bool written=nvn_dirty_since(reinterpret_cast<const void*>(j->mem),size_t(j->size),j->stamp)!=0;
        finish(j,FAILED,!written,written?&fail_written:&fail_verify);return;}
    const int why=nvn_dirty_gone_begin(reinterpret_cast<const void*>(j->lo),len,j->stamp);
    if(why){finish(j,FAILED,false,why==2?&fail_gen:&fail_busy);return;}
    nvn_dirty_gone_commit(reinterpret_cast<const void*>(j->lo),len);
    released_total+=len;gone_bytes+=len;++gone_count;++discards;discard_ns+=now_ns()-t0;
    finish(j,GONE,false,nullptr);
}
void restore_at(uintptr_t a){
    std::shared_ptr<Rec> r;
    {std::lock_guard<std::mutex> l(mu);auto it=gone.upper_bound(a);if(it!=gone.begin()){--it;if(a>=it->second->lo && a<it->second->hi && it->second->st==GONE){r=it->second;r->st=RESTORING;}}}
    if(!r){if(nvn_dirty_page_state && nvn_dirty_page_state(reinterpret_cast<const void*>(a))==3){static std::atomic<int> told{0};if(!told++)xbg_logf("【红】NVN TEXDISCARD released page %#llx has no record",(unsigned long long)a);}return;}
    const uint64_t t0=now_ns();restore(*r);const size_t len=r->hi-r->lo;
    gone_bytes-=len;--gone_count;++restores;if(r->by_undo)++restores_undo;restore_bytes+=len;restore_ns+=now_ns()-t0;
    {std::lock_guard<std::mutex> l(mu);r->st=RESTORED;gone.erase(r->lo);}cv.notify_all();
}
void worker_main(){
    pthread_setname_np(pthread_self(),"dm-texdiscard");nvn_dirty_gone_set_worker();
    if(!vk_init()){worker_failed=true;xbg_logf("【红】NVN TEXDISCARD aux queue command pool / fence creation failed: no discards");}
    for(;;){
        const uint32_t seen=nvn_dirty_gone_req_seq();bool did=false;
        while(uintptr_t a=nvn_dirty_gone_take()){restore_at(a);did=true;}   // restores first: a thread is waiting for each
        std::shared_ptr<Rec> j;
        {std::lock_guard<std::mutex> l(mu);while(!jobs.empty()){auto f=jobs.front();jobs.pop_front();if(f->st==PENDING){f->st=WORKING;j=f;break;}}}
        if(j){if(worker_failed)finish(j,FAILED,false,nullptr);else do_discard(j);did=true;}
        if(!did)nvn_dirty_gone_idle(seen,200);
    }
}
// Engine side (engine lock held): take the guest copy back before the image changes. Blocks until restored or cancelled.
void wait_back(const std::shared_ptr<Rec> &r){
    std::unique_lock<std::mutex> l(mu);
    for(;;){
        if(r->st==PENDING){r->st=CANCELLED;++cancels;return;}
        if(r->st==WORKING || r->st==RESTORING){cv.wait_for(l,std::chrono::milliseconds(50));continue;}
        if(r->st==GONE){r->by_undo=true;l.unlock();nvn_dirty_gone_ask(reinterpret_cast<const void*>(r->lo));l.lock();cv.wait_for(l,std::chrono::milliseconds(50));continue;}
        return;
    }
}
void undo(Image &im){
    const uint64_t id=im.discard_id;im.discard_id=0;if(!id)return;
    std::shared_ptr<Rec> r;bool live;{std::lock_guard<std::mutex> l(mu);auto it=recs.find(id);if(it==recs.end())return;r=it->second;recs.erase(it);live=r->st==GONE || r->st==PENDING || r->st==WORKING;}
    wait_back(r);
    if(live){++undos;if(im.discard_undos<255)++im.discard_undos;}
}
// Why a static image cannot be discarded (0 = it can). Bits double as the TEXCLASS buckets.
enum{W_ZC=1,W_DECODED=2,W_UNTRACKED=4,W_FORMAT=8,W_SMALL=16,W_BIG=32,W_LAYOUT=64,W_DIRTY=128,W_UNDONE=256};
unsigned why_not(const Image &im,uintptr_t &lo,uintptr_t &hi){
    const tex_t &t=im.source;if(!t.mem || !t.L.size)return W_FORMAT;
    if(im.decoded)return W_DECODED;
    if(im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT || im.samples!=VK_SAMPLE_COUNT_1_BIT || (t.d.flags&(0x1|0x2|0x4|0x10|0x20|0x40)) || t.d.target==5 || t.d.target==6 || t.d.target==10 || im.bpp!=t.L.bpb || !im.bpp)return W_FORMAT;
    const uintptr_t mem=uintptr_t(t.mem);lo=(mem+pg-1)&~(pg-1);hi=(mem+t.L.size)&~(pg-1);
    if(hi<=lo || hi-lo<min_bytes)return W_SMALL;
    if(t.L.size>max_bytes)return W_BIG;
    if(zc::find(lo,1) || zc::find(hi-1,1))return W_ZC;
    if(!im.dirty_stamp || !nvn_dirty_tracked || !nvn_dirty_tracked(t.mem,size_t(t.L.size)))return W_UNTRACKED;
    if(im.layout!=VK_IMAGE_LAYOUT_GENERAL)return W_LAYOUT;
    if(!nvn_dirty_since || nvn_dirty_since(t.mem,size_t(t.L.size),im.dirty_stamp))return W_DIRTY;
    if(im.discard_undos>=3)return W_UNDONE;
    return 0;
}
enum Cls{C_RT,C_VT,C_HOT,C_YOUNG,C_STATIC,C_UNUSED,C_N};
Cls classify(const Image &im,uint64_t now){
    if(im.gpu_written)return C_RT;
    if(im.up_partial)return C_VT;
    if(!im.up_full)return C_UNUSED;
    if(now-im.up_last_ns<age_ns || !im.uploaded)return im.up_full>1 || !im.uploaded?C_HOT:C_YOUNG;
    return C_STATIC;
}
void class_log(uint64_t now){
    static const char *const names[C_N]={"rt","vt","hot","young","static","unused"};
    struct Acc{uint64_t n=0,guest=0,vk=0;}acc[C_N];uint64_t gone_g=0,gone_n=0,disc_n=0,disc_pages=0,disc_guest=0,bucket[10]={};
    std::unordered_set<const Image*> seen;std::map<uintptr_t,uint64_t> pool_tex;
    for(auto &e:images){const Image &im=*e.second;if(!seen.insert(&im).second || !im.source.mem)continue;
        const Cls c=classify(im,now);acc[c].n++;acc[c].guest+=im.source.L.size;acc[c].vk+=uint64_t(std::max<int64_t>(0,im.mem_bytes));
        {auto p=pools.upper_bound(uintptr_t(im.source.mem));if(p!=pools.begin()){--p;if(uintptr_t(im.source.mem)<p->first+p->second)pool_tex[p->first]+=im.source.L.size;}}
        if(im.discard_id){gone_n++;gone_g+=im.source.L.size;continue;}
        if(c!=C_STATIC)continue;
        uintptr_t lo=0,hi=0;const unsigned w=why_not(im,lo,hi);
        if(!w){disc_n++;disc_pages+=hi-lo;disc_guest+=im.source.L.size;}else for(int b=0;b<10;b++)if(w>>b&1){bucket[b]+=im.source.L.size;break;}}
    std::string line;char b[160];
    for(int c=0;c<C_N;c++){snprintf(b,sizeof b," %s %llu/%.1f/%.1f",names[c],(unsigned long long)acc[c].n,acc[c].guest/1048576.0,acc[c].vk/1048576.0);line+=b;}
    xbg_logf("NVN TEXCLASS (class images/guest MB/vulkan MB, static = uploaded >= %llu s ago and not since)%s",(unsigned long long)(age_ns/1000000000ull),line.c_str());
    xbg_logf("NVN TEXCLASS static discardable %llu images %.1f MB guest (%.1f MB whole pages) | not: zc %.1f decoded %.1f untracked %.1f format/flags %.1f small %.1f big %.1f layout %.1f dirty %.1f undone %.1f | released or queued %llu images %.1f MB",
        (unsigned long long)disc_n,disc_guest/1048576.0,disc_pages/1048576.0,bucket[0]/1048576.0,bucket[1]/1048576.0,bucket[2]/1048576.0,bucket[3]/1048576.0,bucket[4]/1048576.0,
        bucket[5]/1048576.0,bucket[6]/1048576.0,bucket[7]/1048576.0,bucket[8]/1048576.0,(unsigned long long)gone_n,gone_g/1048576.0);
    for(auto &p:pools){if(p.second<(64u<<20))continue;
        const uintptr_t a=p.first&~(pg-1),e=(p.first+p.second+pg-1)&~(pg-1);std::vector<unsigned char> v((e-a)/pg);uint64_t res=0;
        const bool ok=mincore(reinterpret_cast<void*>(a),e-a,v.data())==0;if(ok)for(unsigned char x:v)res+=x&1;
        xbg_logf("NVN TEXCLASS pool %#llx %.1f MB: resident %s%.1f MB, textures %.1f MB, zero-copy %d",(unsigned long long)p.first,p.second/1048576.0,ok?"":"? ",res*double(pg)/1048576.0,
            pool_tex[p.first]/1048576.0,int(zc::find(p.first,1)!=nullptr));}
}
void stats_log(){
    uint64_t rf=0,wf=0,pw=0,slow=0;int red=0;if(nvn_dirty_gone_stats)nvn_dirty_gone_stats(&rf,&wf,&pw,&slow,&red);
    size_t q=0;{std::lock_guard<std::mutex> l(mu);q=jobs.size();}
    const uint64_t n=restores.load();
    xbg_logf("NVN TEXDISCARD released now %.1f MB (%llu textures), total %.1f MB | faults read %llu write %llu host-write %llu slow %llu%s | restores %llu (undo %llu) %.1f MB avg %.2f ms | discard avg %.2f ms queue %zu | undo %llu cancel %llu | not: verify %llu written %llu busy %llu newer %llu overlap %llu",
        gone_bytes.load()/1048576.0,(unsigned long long)gone_count.load(),released_total.load()/1048576.0,(unsigned long long)rf,(unsigned long long)wf,(unsigned long long)pw,(unsigned long long)slow,
        red?" GAVE UP":"",(unsigned long long)n,(unsigned long long)restores_undo.load(),restore_bytes.load()/1048576.0,n?restore_ns.load()/1e6/n:0.0,
        discards.load()?discard_ns.load()/1e6/double(discards.load()):0.0,q,(unsigned long long)undos.load(),(unsigned long long)cancels.load(),
        (unsigned long long)fail_verify.load(),(unsigned long long)fail_written.load(),(unsigned long long)fail_busy.load(),(unsigned long long)fail_gen.load(),(unsigned long long)fail_overlap.load());
}
// Pick static textures (engine lock held). Settles finished records first.
void scan(uint64_t now){
    uint64_t budget=scan_budget;std::unordered_set<Image*> seen;
    for(auto &e:images){Image &im=*e.second;if(!seen.insert(&im).second)continue;
        if(im.discard_id){std::shared_ptr<Rec> r;{std::lock_guard<std::mutex> l(mu);auto it=recs.find(im.discard_id);if(it!=recs.end())r=it->second;}
            if(!r){im.discard_id=0;continue;}
            St st;bool perm;{std::lock_guard<std::mutex> l(mu);st=r->st;perm=r->permanent;}
            if(st==RESTORED || st==FAILED || st==CANCELLED){{std::lock_guard<std::mutex> l(mu);recs.erase(im.discard_id);}im.discard_id=0;
                if(st==RESTORED && im.discard_undos<255)++im.discard_undos;if(st==FAILED && perm)im.discard_undos=255;}
            continue;}
        if(!budget || classify(im,now)!=C_STATIC)continue;
        uintptr_t lo=0,hi=0;if(why_not(im,lo,hi))continue;
        auto r=std::make_shared<Rec>();r->mem=uintptr_t(im.source.mem);r->size=im.source.L.size;r->lo=lo;r->hi=hi;r->stamp=im.dirty_stamp;r->d=im.source.d;r->image=im.image;
        r->w=im.width;r->h=im.height;r->depth=im.depth;r->levels=im.levels;r->layers=im.layers;r->bpp=im.bpp;
        nvn_fmt_bpb(im.source.d.format,&r->bw,&r->bh);r->blocks=r->bw>1 || r->bh>1;
        {std::lock_guard<std::mutex> l(mu);r->id=next_id++;recs[r->id]=r;jobs.push_back(r);}
        im.discard_id=r->id;budget=hi-lo>=budget?0:budget-(hi-lo);}
    if(nvn_dirty_gone_kick)nvn_dirty_gone_kick();
}
void tick(){
    static uint64_t next_scan=0,next_class=0,next_stats=0;
    if(state_on<0)on();
    if(state_on<=0 && !class_period_ns)return;
    const uint64_t now=now_ns();
    if(!next_scan){next_scan=now+10000000000ull;next_class=class_period_ns?now+class_period_ns:UINT64_MAX;next_stats=now+10000000000ull;return;}
    if(state_on>0 && now>=next_scan){next_scan=now+10000000000ull;
        if(!worker_started){worker_started=true;std::thread(worker_main).detach();}
        scan(now);}
    if(now>=next_class){next_class=now+class_period_ns;class_log(now);}
    if(state_on>0 && now>=next_stats){next_stats=now+10000000000ull;stats_log();}
}
}
