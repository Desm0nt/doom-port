// nn::audio AudioOut：游戏（FMOD nx 输出层）按块追加 PCM 缓冲，播完由系统放回「已释放」队列并触发事件。
// 宿主：真出声 = CoreAudio AudioUnit（Mac DefaultOutput / iOS RemoteIO）、安卓 AAudio；静音接收端 = 按真实时间消耗缓冲（不出声，节奏与真机一致）。
//       iOS 缺省真出声（产品必须有声音），测试要静音显式 DM_AUDIO=0；Mac 命令行宿主缺省静音接收端（回放 / 闸门不出声），DM_AUDIO=1 真出声；
//       Mac App（app_mac.m）是产品，启动时设 DM_AUDIO=1（「没有声音」：之前 App 也走了命令行的缺省，从来不出声）。
//       声卡打不开（没有输出设备等）不再退出：记红，这一路改由静音接收端按实时消耗，游戏照常跑。
// AudioOut / AudioOutBuffer / AudioOutParameter 的内容只经下列接口读写，宿主自定内部布局，只写结构体前部。
// nn::audio AudioOut: the game (FMOD's nx output layer) appends PCM buffers in blocks; once played the system puts them back on the "released" queue and signals an event.
// Host: real sound = CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO), Android AAudio; silent sink = buffers consumed in real time (no sound, same pacing as the device).
// iOS plays real sound by default (the product must have sound), tests set DM_AUDIO=0 explicitly for silence; the Mac command-line host defaults to the silent sink (replays / gates are silent), DM_AUDIO=1 for real sound;
// the Mac App (app_mac.m) is the product and sets DM_AUDIO=1 at launch ("no sound": the App used to take the command-line default and never played sound).
// If the sound card cannot be opened (no output device etc.) it no longer exits: logs red, and this channel is consumed in real time by the silent sink while the game runs on.
// The contents of AudioOut / AudioOutBuffer / AudioOutParameter are read and written only through the interfaces below; the host defines its own internal layout and writes only the front part of each struct.
#include "hle.h"
#include <pthread/qos.h>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
typedef AudioUnit dm_audev;            // 声卡（出声端）句柄 | Sound card (output side) handle
typedef dispatch_semaphore_t dm_sem;
static dm_sem sem_new(void) { return dispatch_semaphore_create(0); }
static void sem_up(dm_sem s) { dispatch_semaphore_signal(s); }
static void sem_down(dm_sem s) { dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER); }
static void sem_free(dm_sem s) { dispatch_release(s); }
#elif defined(__OHOS__)
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <semaphore.h>
typedef OH_AudioRenderer *dm_audev;
typedef sem_t *dm_sem;
static dm_sem sem_new(void) { dm_sem s = malloc(sizeof *s); if (!s || sem_init(s, 0, 0)) die("音频信号量"); return s; }
static void sem_up(dm_sem s) { sem_post(s); }
static void sem_down(dm_sem s) { while (sem_wait(s)) {} }
static void sem_free(dm_sem s) { sem_destroy(s); free(s); }
#else
#include <aaudio/AAudio.h>
#include <semaphore.h>
typedef AAudioStream *dm_audev;
typedef sem_t *dm_sem;
static dm_sem sem_new(void) { dm_sem s = malloc(sizeof *s); if (!s || sem_init(s, 0, 0)) die("音频信号量"); return s; }
static void sem_up(dm_sem s) { sem_post(s); }
static void sem_down(dm_sem s) { while (sem_wait(s)) {} }
static void sem_free(dm_sem s) { sem_destroy(s); free(s); }
#endif
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
// Timed wait on the release semaphore (dm_pcmout_wait); a timeout or a spurious return is fine, the caller checks again
static void sem_down_ms(dm_sem s, int ms) {
#ifdef __APPLE__
    dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000));
#else
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += (long)ms * 1000000L; dl.tv_sec += dl.tv_nsec / 1000000000L; dl.tv_nsec %= 1000000000L;
    while (sem_timedwait(s, &dl) && errno == EINTR) {}
#endif
}

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // 写进客体 AudioOutBuffer 的前 24 字节 | The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // 已释放最多 = 在途缓冲数（游戏收回缓冲才能再追加），与待播同上限 | Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
typedef struct {
    pthread_mutex_t m;
    void *event;                       // 客体 SystemEvent：有缓冲被释放就触发 | Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // 待播：游戏侧写，出声端读 | Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // 已释放：出声端写，游戏侧读 | Released: written by the output side, read by the game side
    uint64_t pos;                      // 当前块已播字节 | Bytes already played of the current block
    dm_audev au;                       // 声卡；NULL = 没开成（走静音接收端） | Sound card; NULL = not opened (the silent sink is used)
    int mix;                           // 1 = 游戏给 5.1（6 声道），交给 CoreAudio 前我们自己下混成双声道（au_render） | 1 = the game gives 5.1 (6 channels), downmixed to stereo by us before handing to CoreAudio (au_render)
    int16_t *mixbuf;                   // 下混用的 6 声道暂存（MIXMAX 帧） | 6-channel staging for the downmix (MIXMAX frames)
    pthread_t null_th; _Atomic int null_run;
    dm_sem notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    _Atomic uint64_t notify_ns;        // 最近一次「有缓冲释放、要通知」的时刻（量通知线程被叫醒的延迟） | Time of the most recent "buffer released, needs notifying" event (to measure the notification thread's wake-up latency)
    _Atomic int dry;                   // 这次声卡回调里待播队列空了（在播放态）= 一次欠载（断音） | The pending playback queue was empty in this audio callback (while playing) = one underrun (audio dropout)
    dm_audio_fill_fn src; void *src_ctx;
    dm_sem rel_sem;                    // host PCM outputs only (dm_pcmout_open): posted for each released block, so a producer blocks instead of polling   // 宿主音源（DOOM AudioRenderer 混音器）：非空时样本由它现算，不走客体缓冲队列 | Host source (DOOM AudioRenderer mixer): when set, samples come from it instead of the guest buffer queue
    // [DOOM] 声卡与墙钟两个消费者的交接（issues #12/#20）：dm 锁住 au / dev_on / null_live / 重开状态；owner = 1 时墙钟（静音接收端线程）
    // 消费客体队列，声卡回调只出静音；pulling 是 pull 的互斥（trylock，回调里永不阻塞），保证任何时刻只有一个消费者动队列
    // [DOOM] Handover between the two consumers, sound card and wall clock (issues #12/#20): dm guards au / dev_on / null_live / the reopen state; with owner = 1
    // the wall clock (silent sink thread) consumes the guest queue and the sound card callback only outputs silence; pulling makes pull exclusive (a trylock,
    // never blocking in the callback), so only one consumer touches the queue at any time
    pthread_mutex_t dm;
    _Atomic int owner, pulling;
    int null_live, dev_on;             // 静音接收端线程在跑 / 声卡流已启动 | silent sink thread running / sound card stream started
    _Atomic uint64_t renders, last_render_ns, rel_n;   // 声卡回调次数、最近一次回调时刻、累计释放块数（日志） | sound card callbacks, time of the latest one, blocks released so far (log)
    uint64_t dev_start_ns;
    // 安卓看护线程（aa_keeper）：卡死检测、桥接、关流重开 | Android keeper thread (aa_keeper): stall detection, bridging, close and reopen
    pthread_t keep_th; _Atomic int keep_run; dm_sem keep_sem; int keep_live;
    _Atomic int dev_err; _Atomic uintptr_t err_st;     // 错误回调报的错与报错的流（只认当前流的错） | error reported by the error callback and the stream that reported it (only the current stream's errors count)
    int ll, reopen, handover, fails, incidents;   // 当前流是低延迟 / 待重开 / 新流已启动、等它第一次回调再交还队列 / 连续重开失败次数 / 出事次数（日志限流） | current stream is low latency / reopen pending / new stream started, queue handed back at its first callback / consecutive reopen failures / incidents (log rate limit)
    uint64_t hand_renders, next_try_ns, bridge_ns, bridge_rel0, fake_t0; int fake_logged;
    // [DOOM] issue #32 diagnostic (aa_keeper, every 10 s): callbacks that found the queue empty while playing (the producer was late), and the
    // stream's own underruns (AAudioStream_getXRunCount: the callback was late for the device)
    char name[16]; _Atomic uint64_t drys;
    uint64_t dg_t0, dg_r0, dg_d0, dg_x; uintptr_t dg_st; int32_t dg_xlast; int dg_quiet;
} aout;
static void *null_sink(void *p);
static void null_on(aout *a);

// 打开中的输出登记表：前端生命周期（切后台）一次性暂停 / 恢复
// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background)
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_suspended;        // 写在 g_aos_lock 里；看护线程只读，不拿 g_aos_lock（锁序 g_aos_lock → a->dm） | written under g_aos_lock; the keeper only reads it, without g_aos_lock (lock order g_aos_lock -> a->dm)
// [DOOM] 打开数超限时的 Result：audio 模块 153、描述 5 = 0xa99（SDK AddDeviceSink 无空槽同样回它，见 hle_doom_audio.c AR_SINK_FAIL；
// audout 服务「会话用完」按 yuzu / Ryujinx 也是 audio 5，固件里没核）。以前这里 die，issues #12/#20 的 FATAL 就是它
// [DOOM] Result when too many outputs are open: audio module 153, description 5 = 0xa99 (what the SDK's AddDeviceSink returns without a free slot, see
// hle_doom_audio.c AR_SINK_FAIL; audout's "out of sessions" is audio 5 too per yuzu / Ryujinx, not checked against firmware). This used to die: the FATAL of issues #12/#20
#define RES_AUDOUT_OUT_OF_SESSIONS 0xa99u
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p 不是宿主建的", g); return a; }

// 通知线程 QoS 对照（公开 iPhone 15 Pro Max 移动时断音，提议把这条线程提到交互级、与混音线程一致）。
// 缺省交互级（真机 iPhone18,2 野外移动 240 秒同进程交替：叫醒延迟平均约减半、超过 2 ms 的 16 → 0，7 段配对 7/7；欠载两相都是 0，没有可测的退步）。
// DM_AUDIO_NQOS：不设 / 1 = 交互级；0 = 旧行为（继承建线程者的 QoS）；alt:<秒> = 同一进程里每 <秒> 交替一次，每段结束记一行这段的欠载次数与通知延迟——
// 真机移动时同进程交替比，不用两次启动（热态、场景都不同）对比
// Notification thread QoS comparison (public report of audio dropouts while moving on iPhone 15 Pro Max; proposal: raise this thread to interactive level, same as the mixer thread).
// Default is interactive (device iPhone18,2, 240 s of wilderness movement with in-process alternation: wake-up latency roughly halved on average, cases over 2 ms 16 → 0, 7/7 paired segments; underruns 0 in both phases, no measurable regression).
// DM_AUDIO_NQOS: unset / 1 = interactive; 0 = old behavior (inherits the QoS of the thread's creator); alt:<seconds> = alternate every <seconds> within the same process, logging at the end of each segment its underrun count and notification latency;
// compare by in-process alternation while moving on the device, not across two launches (thermal state and scene both differ)
static int g_nq_mode = -1, g_nq_alt; static qos_class_t g_nq_base = QOS_CLASS_UNSPECIFIED;
static _Atomic uint64_t g_nq_dry[2], g_nq_calls[2], g_nq_wake[2], g_nq_lat_sum[2], g_nq_lat_max[2], g_nq_lat_2ms[2];
static int nq_phase(void) { if (g_nq_mode < 0) { const char *e = getenv("DM_AUDIO_NQOS"); g_nq_mode = e && !strcmp(e, "0") ? 0 : e && !strncmp(e, "alt:", 4) ? 2 : 1; if (g_nq_mode == 2) { g_nq_alt = atoi(e + 4); if (g_nq_alt < 1) g_nq_alt = 30; } }
    return g_nq_mode < 2 ? g_nq_mode : (int)((now_ns() / 1000000000ull / (uint64_t)g_nq_alt) & 1); }
static void nq_report(int ph) {
    uint64_t c = atomic_exchange(&g_nq_calls[ph], 0), d = atomic_exchange(&g_nq_dry[ph], 0), w = atomic_exchange(&g_nq_wake[ph], 0), ls = atomic_exchange(&g_nq_lat_sum[ph], 0), lm = atomic_exchange(&g_nq_lat_max[ph], 0), l2 = atomic_exchange(&g_nq_lat_2ms[ph], 0);
    if (!c) return;
    if (ph) LOG("音频通知线程 QoS 对照：相 交互级：声卡回调 %llu 次、欠载（断音）%llu 次；通知 %llu 次，叫醒延迟 平均 %.3f ms、最大 %.3f ms、超过 2 ms %llu 次", (unsigned long long)c, (unsigned long long)d, (unsigned long long)w, w ? ls / 1e6 / (double)w : 0.0, lm / 1e6, (unsigned long long)l2);
    else LOG("音频通知线程 QoS 对照：相 现状：声卡回调 %llu 次、欠载（断音）%llu 次；通知 %llu 次，叫醒延迟 平均 %.3f ms、最大 %.3f ms、超过 2 ms %llu 次", (unsigned long long)c, (unsigned long long)d, (unsigned long long)w, w ? ls / 1e6 / (double)w : 0.0, lm / 1e6, (unsigned long long)l2);
}
static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_ns, now_ns(), memory_order_relaxed);
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        sem_up(a->notify_sem);
}

static void *event_notify(void *p) {
    aout *a = p; int applied = -1;
    if (g_nq_base == QOS_CLASS_UNSPECIFIED) g_nq_base = qos_class_self();
#ifdef __OHOS__
    { void front_gtx_register(int); front_gtx_register(2); }   // OpenGTX 关键线程 | OpenGTX key thread
#endif
    for (;;) {
        int ph = nq_phase();
        if (ph != applied) {   // 换相：先报上一相这段的读数，再改本线程 QoS | Phase switch: first report the readings of the previous phase's segment, then change this thread's QoS
            if (applied >= 0) nq_report(applied);
            int e = pthread_set_qos_class_self_np(ph ? QOS_CLASS_USER_INTERACTIVE : (g_nq_base == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : g_nq_base), 0);
            if (e) LOG("【红】音频通知线程：设 QoS 失败（%d）", e);
            else if (g_nq_mode && ph) LOG("音频通知线程：QoS → 交互级"); else if (g_nq_mode) LOG("音频通知线程：QoS → 现状");
            applied = ph; }
        sem_down(a->notify_sem);
        { uint64_t t0 = atomic_load_explicit(&a->notify_ns, memory_order_relaxed), dt = t0 ? now_ns() - t0 : 0; int q = applied < 0 ? 0 : applied;
          atomic_fetch_add(&g_nq_wake[q], 1); atomic_fetch_add(&g_nq_lat_sum[q], dt); if (dt > 2000000) atomic_fetch_add(&g_nq_lat_2ms[q], 1);
          uint64_t mx = atomic_load(&g_nq_lat_max[q]); while (dt > mx && !atomic_compare_exchange_weak(&g_nq_lat_max[q], &mx, dt)) {} }
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            sem_up(a->notify_sem);
    }
    return NULL;
}

// 从待播队列取 n 字节交给出声端；整块播完就挪到已释放并通知普通线程。
// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    // [DOOM] 交接的边界上声卡回调与墙钟可能同时进来：抢不到的一方出静音 / 跳过这一拍（不阻塞声卡回调） | [DOOM] at a handover boundary the sound card callback and the wall clock may both enter: the loser outputs silence / skips this tick (never blocks the callback)
    if (atomic_exchange_explicit(&a->pulling, 1, memory_order_acquire)) { memset(dst, 0, n); return; }
    if (a->src) {   // 宿主音源：播放态现算，其余静音 | Host source: rendered on demand while started, silence otherwise
        if (atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) a->src(a->src_ctx, (int16_t *)dst, (unsigned)(n / (2u * (unsigned)a->ch)));
        else memset(dst, 0, n);
        atomic_store_explicit(&a->pulling, 0, memory_order_release);
        return;
    }
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) { if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) a->dry = 1; memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut 已释放队列溢出");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released++;
        }
    }
    atomic_store_explicit(&a->pulling, 0, memory_order_release);
    if (released) { atomic_fetch_add_explicit(&a->rel_n, (uint64_t)released, memory_order_relaxed); notify_release(a); if (a->rel_sem) sem_up(a->rel_sem); }
}

static _Atomic uint64_t g_render_calls;   // 声卡回调累计（生命周期验证：恢复后须继续增长） | Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
// 5.1 → 双声道（公开 的：原先把游戏的 6 声道原样交给 AudioUnit、不设声道布局，系统隐式转换只取前两路，中置——NPC 对白——整路丢掉）。
// 游戏的声道顺序（Mac App 走到阿卡拉跟前、对白开口那半秒按声道量能量实测：只有第 3 路跳起来）：左前、右前、中置、低音、左后、右后。
// 系数照 ITU-R BS.775 的 Lo/Ro：L = 左前 + 0.707·中置 + 0.707·左后，R 对称；低音不并入（标准下混不含 LFE，并进去会让低频发闷）；
// 整体再乘 0.707 留 3 dB 余量，仍超出 16 位的饱和截断。Q15 定点：0.707 = 23170，0.707 × 0.707 = 16384
// 5.1 -> stereo (previously: the game's 6 channels were handed to the AudioUnit as is without a channel layout; the system's implicit conversion took only the first two, and the centre - NPC dialogue - was dropped entirely).
// The game's channel order (measured in the Mac App by per-channel energy during the half second Akara starts speaking: only channel 3 jumps): front left, front right, centre, LFE, rear left, rear right.
// Coefficients from the ITU-R BS.775 Lo/Ro downmix: L = front left + 0.707*centre + 0.707*rear left, R symmetric; LFE is not mixed in (the standard downmix excludes LFE, and mixing it in muddies the low end);
// the whole is multiplied by 0.707 again for 3 dB of headroom, and anything still beyond 16 bits is saturated. Q15 fixed point: 0.707 = 23170, 0.707 x 0.707 = 16384
#define MIXMAX 4096
static void downmix51(const int16_t *in, int16_t *out, unsigned frames) {
    for (unsigned i = 0; i < frames; i++, in += 6, out += 2) {
        int32_t l = (23170 * in[0] + 16384 * in[2] + 16384 * in[4]) >> 15, r = (23170 * in[1] + 16384 * in[2] + 16384 * in[5]) >> 15;
        out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r); }
}
// 声卡回调的公共部分（两个平台同一份）：填满 bytes 字节的输出缓冲
// Common part of the sound card callback (one copy for both platforms): fill an output buffer of bytes bytes
static void render(aout *a, void *data, uint32_t bytes) {
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->renders, 1, memory_order_relaxed); atomic_store_explicit(&a->last_render_ns, now_ns(), memory_order_relaxed);   // 看护线程的进度读数 | progress reading for the keeper
    a->dry = 0;
    if (atomic_load_explicit(&a->owner, memory_order_acquire)) memset(data, 0, bytes);   // [DOOM] 墙钟在消费（桥接 / 暂停 / 交接中）：声卡只出静音 | [DOOM] the wall clock is consuming (bridge / suspend / handover): the sound card outputs silence
    else if (!a->mix) pull(a, data, bytes);
    else { int16_t *o = data; unsigned left = bytes / 4;   // 双声道 16 位：每帧 4 字节 | stereo 16-bit: 4 bytes per frame
        while (left) { unsigned k = left < MIXMAX ? left : MIXMAX; pull(a, (uint8_t *)a->mixbuf, (uint64_t)k * 12); downmix51(a->mixbuf, o, k); o += 2 * k; left -= k; } }
    // 诊断开关 DM_AUDIO_OUT_DUMP=<文件>：交给 CoreAudio 的那份数据原样续写（下混后是双声道）。本机没有回环设备录不到系统输出，判「对白有没有交出去」看这份
    // Diagnostic switch DM_AUDIO_OUT_DUMP=<file>: append the data handed to CoreAudio as is (stereo after the downmix). This machine has no loopback device and cannot record the system output; to tell whether the dialogue was handed over, look at this file
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_OUT_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDIO_OUT_DUMP 打不开 %s", e); }
      if (df) fwrite(data, 1, bytes, df); }
    { int q = nq_phase(); atomic_fetch_add_explicit(&g_nq_calls[q], 1, memory_order_relaxed); if (a->dry) atomic_fetch_add_explicit(&g_nq_dry[q], 1, memory_order_relaxed); }   // 通知线程 QoS 对照：按当前相记 | Notification thread QoS comparison: recorded per current phase
    if (a->dry) atomic_fetch_add_explicit(&a->drys, 1, memory_order_relaxed);   // [DOOM] issue #32: per-output count for aa_keeper's 10 s line
}
#ifdef __APPLE__
static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    render(ctx, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
    return noErr;
}
#if !TARGET_OS_IPHONE
static void dev_name(AudioObjectID id, char *o, size_t n) {   // 输出设备名（日志用） | output device name (for the log)
    CFStringRef nm = NULL; UInt32 sz = sizeof nm; AudioObjectPropertyAddress pa = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    o[0] = 0; if (!AudioObjectGetPropertyData(id, &pa, 0, NULL, &sz, &nm) && nm) { CFStringGetCString(nm, o, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(nm); } }
#endif
static int au_open(aout *a) {   // 0 = 声卡开好了；-1 = 开不了（a->au 置空，调用方走静音接收端） | 0 = the sound card is open; -1 = it cannot be opened (a->au set to null, the caller uses the silent sink)
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &a->au)) { a->au = NULL; LOG("【红】AudioUnit 建不出来：这一路改用静音接收端（不出声）"); return -1; }
    a->mix = a->ch == 6; if (a->mix && !a->mixbuf) a->mixbuf = malloc(MIXMAX * 12);   // 5.1：我们自己下混，AudioUnit 只收双声道（单 / 双声道照旧原样交） | 5.1: we downmix ourselves and the AudioUnit only takes stereo (mono / stereo are passed through as before)
    if (a->mix && !a->mixbuf) { AudioComponentInstanceDispose(a->au); a->au = NULL; LOG("【红】下混暂存申请不到：这一路改用静音接收端（不出声）"); return -1; }
    int och = a->mix ? 2 : a->ch;
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * och, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * och,
                                     .mChannelsPerFrame = (UInt32)och, .mBitsPerChannel = 16};
    if (a->mix) LOG("audio：游戏给 6 声道（5.1）→ 交给声卡前自己下混成双声道（中置 / 后环绕 ×0.707 并入左右，低音不并入，整体 −3 dB、饱和截断）");
    AURenderCallbackStruct cb = {au_render, a};
    if (AudioUnitSetProperty(a->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(a->au)) { AudioComponentInstanceDispose(a->au); a->au = NULL; LOG("【红】AudioUnit 设置失败（没有输出设备？）：这一路改用静音接收端（不出声）"); return -1; }
#if !TARGET_OS_IPHONE
    {   // 缺省输出单元跟随系统缺省输出设备：两个都记下来，核「声音从系统选的那个设备出」 | the default output unit follows the system default output device: record both, to check that "the sound comes out of the device the system chose"
        AudioObjectID cur = 0, def = 0; UInt32 sz = sizeof cur; char cn[128], dn[128];
        AudioUnitGetProperty(a->au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &cur, &sz);
        AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }; sz = sizeof def;
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, &def); dev_name(cur, cn, sizeof cn); dev_name(def, dn, sizeof dn);
        LOG("audio：输出设备「%s」（%u），系统缺省输出「%s」（%u）%s", cn, (unsigned)cur, dn, (unsigned)def, cur == def ? "，一致" : "【红】不一致"); }
#endif
    return 0;
}
static int dev_start(aout *a) { return AudioOutputUnitStart(a->au) ? -1 : 0; }
static void dev_stop(aout *a) { AudioOutputUnitStop(a->au); }
static void dev_close(aout *a) { AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); a->au = NULL; }
#elif defined(__OHOS__)
// 鸿蒙：OHAudio 低时延渲染流（16 位、48 kHz、游戏用途），写数据回调直接调 render。
// 系统报错（音频服务重启等）时与安卓一样交给普通线程关掉重开，原来在播就接着播；路由切换（耳机、蓝牙）由 OHAudio 自行跟随。
// HarmonyOS: OHAudio low-latency renderer (16-bit, 48 kHz, game usage); the write-data callback calls render directly.
// On a system error (e.g. the audio service restarting) an ordinary thread closes and reopens it, as on Android, resuming if it
// was playing; route changes (headphones, Bluetooth) are followed by OHAudio itself.
static int32_t oh_write(OH_AudioRenderer *r, void *ctx, void *data, int32_t length) {
    (void)r; render(ctx, data, (uint32_t)length); return 0;
}
static int32_t oh_event(OH_AudioRenderer *r, void *ctx, OH_AudioStream_Event e) { (void)r; (void)ctx; LOG("audio：OHAudio 流事件 %d", (int)e); return 0; }
static int32_t oh_interrupt(OH_AudioRenderer *r, void *ctx, OH_AudioInterrupt_ForceType t, OH_AudioInterrupt_Hint h) {
    (void)r; (void)ctx; LOG("audio：音频焦点中断 type=%d hint=%d", (int)t, (int)h); return 0;
}
static int au_open(aout *a);
static void dev_drop(aout *a); static int dev_go(aout *a);
static void *oh_reopen(void *p) {
    aout *a = p;
    pthread_mutex_lock(&g_aos_lock); pthread_mutex_lock(&a->dm);
    if (a->au) {
        int playing = !g_suspended && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED;
        dev_drop(a);
        if (au_open(a)) { LOG("【红】音频流出错后重开失败：这一路改用静音接收端（不出声）"); if (playing) null_on(a); }
        else if (playing && dev_go(a)) LOG("【红】音频流重开后启动失败");
        else LOG("audio：音频流出错，已重开");
    }
    pthread_mutex_unlock(&a->dm); pthread_mutex_unlock(&g_aos_lock);
    return NULL;
}
static int32_t oh_error(OH_AudioRenderer *r, void *ctx, OH_AudioStream_Result err) {
    (void)r; LOG("【红】OHAudio 流错误 %d，交给线程重开", (int)err);
    pthread_t th; if (!pthread_create(&th, NULL, oh_reopen, ctx)) pthread_detach(th);
    return 0;
}
static int au_open(aout *a) {   // 0 = 声卡开好了；-1 = 开不了（a->au 置空，调用方走静音接收端） | 0 = the sound card is open; -1 = it cannot be opened (a->au set to null, the caller uses the silent sink)
    a->mix = a->ch == 6; if (a->mix && !a->mixbuf) a->mixbuf = malloc(MIXMAX * 12);   // 5.1：我们自己下混，声卡只收双声道 | 5.1: we downmix ourselves, the sound card only takes stereo
    if (a->mix && !a->mixbuf) { a->au = NULL; LOG("【红】下混暂存申请不到：这一路改用静音接收端（不出声）"); return -1; }
    int och = a->mix ? 2 : a->ch;
    OH_AudioStreamBuilder *b = NULL;
    if (OH_AudioStreamBuilder_Create(&b, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS) { a->au = NULL; LOG("【红】OHAudio 建不出来：这一路改用静音接收端（不出声）"); return -1; }
    OH_AudioStreamBuilder_SetSamplingRate(b, RATE);
    OH_AudioStreamBuilder_SetChannelCount(b, och);
    OH_AudioStreamBuilder_SetSampleFormat(b, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(b, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetRendererInfo(b, AUDIOSTREAM_USAGE_GAME);
    OH_AudioStreamBuilder_SetLatencyMode(b, AUDIOSTREAM_LATENCY_MODE_FAST);
    OH_AudioRenderer_Callbacks cb = {.OH_AudioRenderer_OnWriteData = oh_write, .OH_AudioRenderer_OnStreamEvent = oh_event,
                                     .OH_AudioRenderer_OnInterruptEvent = oh_interrupt, .OH_AudioRenderer_OnError = oh_error};
    OH_AudioStreamBuilder_SetRendererCallback(b, cb, a);
    OH_AudioStream_Result r = OH_AudioStreamBuilder_GenerateRenderer(b, &a->au);
    OH_AudioStreamBuilder_Destroy(b);
    if (r != AUDIOSTREAM_SUCCESS) { a->au = NULL; LOG("【红】OHAudio 打开失败 %d：这一路改用静音接收端（不出声）", (int)r); return -1; }
    if (a->mix) LOG("audio：游戏给 6 声道（5.1）→ 交给声卡前自己下混成双声道（中置 / 后环绕 ×0.707 并入左右，低音不并入，整体 −3 dB、饱和截断）");
    int32_t rate = 0, chn = 0, fpc = 0; OH_AudioRenderer_GetSamplingRate(a->au, &rate); OH_AudioRenderer_GetChannelCount(a->au, &chn); OH_AudioRenderer_GetFrameSizeInCallback(a->au, &fpc);
    LOG("audio：OHAudio 流 %d Hz %d 声道，每次回调 %d 帧", (int)rate, (int)chn, (int)fpc);
    return 0;
}
static int dev_start(aout *a) { return OH_AudioRenderer_Start(a->au) == AUDIOSTREAM_SUCCESS ? 0 : -1; }
static void dev_stop(aout *a) { OH_AudioRenderer_Stop(a->au); }
static void dev_close(aout *a) { OH_AudioRenderer_Release(a->au); a->au = NULL; }
#else
// 安卓：AAudio 输出流（16 位、48 kHz、游戏用途），回调直接调 render。缺省低延迟（性能模式 12）；出过卡死 / 超时 / 启动失败后本进程改用
// 性能模式 NONE（AudioTrack 那条路，不走 MMAP），见 aa_keeper。错误回调里不能关流：只记下错误、叫醒这一路的看护线程，由它关掉重开。
// Android: AAudio output stream (16-bit, 48 kHz, game usage), the callback calls render directly. Low latency by default (performance mode 12); after a
// stall / timeout / start failure this process switches to performance mode NONE (the AudioTrack path, no MMAP), see aa_keeper. The stream must not be
// closed inside the error callback: it only records the error and wakes this output's keeper thread, which closes and reopens it.
//
// [DOOM] issues #12/#20（Samsung S23+ SM-S916B，Android 17）：两路流同时开成 MMAP 低延迟，一路 requestStart 报「stream was probably stolen」，
// 之后每一路 MMAP 流第一次 write 就卡在 processData，1 s 后 AAUDIO_ERROR_TIMEOUT、回调线程退出；拿到 Legacy（perf = NO）的运行全都正常。
// 以前这里只记一行红字，流就死了：客体缓冲不再释放，游戏的 Wwise 输出（main+0x21d7180 每个设备一轮，状态 +0xb4 = 2 时约每秒一次
// 0x21d68d0 → 0x21fec30 → 汇点工厂 0x21ff1f0 → 0x21ff33c OpenDefaultAudioOut）每秒重建一次汇点，8 次后 h_au_Open die。
// [DOOM] issues #12/#20 (Samsung S23+ SM-S916B, Android 17): two streams opened together as MMAP low latency, one requestStart reported "stream was
// probably stolen", after which every MMAP stream stalled in processData on its first write, got AAUDIO_ERROR_TIMEOUT after 1 s and its callback thread
// exited; runs that got Legacy (perf = NO) were all fine. This used to log one red line and leave the stream dead: guest buffers were no longer released,
// and the game's Wwise output (main+0x21d7180, one round per device; with state +0xb4 = 2 about once a second 0x21d68d0 -> 0x21fec30 -> sink factory
// 0x21ff1f0 -> 0x21ff33c OpenDefaultAudioOut) rebuilt its sink every second until h_au_Open died on the 9th open.
// DM_AUDIO_LOWLATENCY：不设 / 1 = 低延迟（出事后自动降级）；0 = 一开始就用 NONE | unset / 1 = low latency (automatic fallback after a failure); 0 = NONE from the start
static _Atomic int g_aa_safe = -1;
static int aa_safe(void) {
    int s = atomic_load(&g_aa_safe);
    if (s < 0) { const char *e = getenv("DM_AUDIO_LOWLATENCY"); s = e && !atoi(e);
                 int exp = -1; if (atomic_compare_exchange_strong(&g_aa_safe, &exp, s) && s) LOG("audio：DM_AUDIO_LOWLATENCY=0：AAudio 流用性能模式 NONE（不走 MMAP 低延迟）"); s = atomic_load(&g_aa_safe); }
    return s;
}
static void aa_go_safe(const char *why) {
    aa_safe();
    if (atomic_exchange(&g_aa_safe, 1) == 0) LOG("【红】audio：低延迟 AAudio 流%s：本进程之后的流改用性能模式 NONE（AudioTrack 路径，不走 MMAP；issues #12/#20）", why);
}
// DM_AUDIO_STALL_MS：声卡在播却这么久没有回调就算卡死（缺省 250；0 = 只认错误回调） | DM_AUDIO_STALL_MS: a playing stream without a callback for this long counts as stalled (default 250; 0 = error callbacks only)
static _Atomic int g_stall_ms = -1;
static uint64_t aa_stall_ns(void) {
    int s = atomic_load(&g_stall_ms);
    if (s < 0) { const char *e = getenv("DM_AUDIO_STALL_MS"); s = e ? atoi(e) : 250; if (s < 0) s = 0; if (e) LOG("audio：DM_AUDIO_STALL_MS=%d", s); atomic_store(&g_stall_ms, s); }
    return (uint64_t)s * 1000000ull;
}
// 诊断开关 DM_AUDIO_FAKE_STALL=<毫秒>：每路低延迟流回调满这么久就假装卡死（回调回 STOP，流停住、不报错），用来在任何手机上走一遍
// 「卡死检测 → 墙钟桥接 → 降级 NONE 重开 → 交还」；NONE 流不受影响，所以降级后声音应当回来
// Diagnostic switch DM_AUDIO_FAKE_STALL=<ms>: every low-latency stream pretends to stall after its callbacks have run this long (the callback returns STOP,
// the stream stops without an error), to exercise "stall detection -> wall-clock bridge -> reopen as NONE -> hand back" on any phone; NONE streams are not
// affected, so the sound should come back after the fallback
// [DOOM] issue #32: DM_AUDIO_BUFFER_MS=<ms> floor of every AAudio stream's buffer (default 32; 0 = keep the system's size), see aa_open
static _Atomic int g_buf_ms = -1;
static int aa_buf_ms(void) {
    int s = atomic_load(&g_buf_ms);
    if (s < 0) { const char *e = getenv("DM_AUDIO_BUFFER_MS"); s = e && *e ? atoi(e) : 32; if (s < 0) s = 0; if (s > 200) s = 200; atomic_store(&g_buf_ms, s); }
    return s;
}
static int g_fake_ms = -1;
static int aa_fake_ms(void) { if (g_fake_ms < 0) { const char *e = getenv("DM_AUDIO_FAKE_STALL"); g_fake_ms = e ? atoi(e) : 0; if (g_fake_ms < 0) g_fake_ms = 0; } return g_fake_ms; }
static aaudio_data_callback_result_t aa_render(AAudioStream *st, void *ctx, void *data, int32_t frames) {
    aout *a = ctx;
    if (aa_fake_ms() && AAudioStream_getPerformanceMode(st) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY) {
        uint64_t t = now_ns();
        if (!a->fake_t0) a->fake_t0 = t;
        else if (t - a->fake_t0 > (uint64_t)aa_fake_ms() * 1000000ull) {
            if (!a->fake_logged) { a->fake_logged = 1; LOG("【红】audio：DM_AUDIO_FAKE_STALL：低延迟流回调 %d ms 后假装卡死（回调回 STOP）", aa_fake_ms()); }
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
    }
    render(a, data, (uint32_t)frames * (uint32_t)AAudioStream_getChannelCount(st) * 2u);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void aa_error(AAudioStream *st, void *ctx, aaudio_result_t err) {
    aout *a = ctx;
    if (err != AAUDIO_ERROR_DISCONNECTED) LOG("【红】音频流错误 %d（%s）：交给看护线程关掉重开", (int)err, AAudio_convertResultToText(err));
    atomic_store(&a->err_st, (uintptr_t)st); atomic_store(&a->dev_err, err ? (int)err : -1);
    if (a->keep_sem) sem_up(a->keep_sem);
}
// 开一路流写进 *out（不碰 a->au：看护线程在锁外开新流）；*ll = 拿到的是低延迟。0 = 开好了 | Open one stream into *out (a->au untouched: the keeper opens new streams outside the lock); *ll = it got low latency. 0 = opened
static int aa_open(aout *a, dm_audev *out, int *ll) {
    a->mix = a->ch == 6; if (a->mix && !a->mixbuf) a->mixbuf = malloc(MIXMAX * 12);   // 5.1：我们自己下混，声卡只收双声道 | 5.1: we downmix ourselves, the sound card only takes stereo
    if (a->mix && !a->mixbuf) { *out = NULL; LOG("【红】下混暂存申请不到：这一路改用静音接收端（不出声）"); return -1; }
    int och = a->mix ? 2 : a->ch, safe = aa_safe();
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { *out = NULL; LOG("【红】AAudio 建不出来：这一路改用静音接收端（不出声）"); return -1; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, RATE);
    AAudioStreamBuilder_setChannelCount(b, och);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, safe ? AAUDIO_PERFORMANCE_MODE_NONE : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);   // 本来就是缺省；写明：从不要 MMAP 独占 | the default anyway; spelled out: never ask for MMAP exclusive
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, aa_render, a);
    AAudioStreamBuilder_setErrorCallback(b, aa_error, a);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, out);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { *out = NULL; LOG("【红】AAudio 打开失败 %d（%s）", (int)r, AAudio_convertResultToText(r)); return -1; }
    {   // [DOOM] issue #32: buffer floor DM_AUDIO_BUFFER_MS (default 32; 0 = the system's size). The Mi 14 Ultra's low-latency streams come up
        // at 1536 / 1536 frames (32 ms), the Samsung S23's MMAP streams at 768 / 1536 (16 ms, 8 bursts of 96): raise a smaller buffer to
        // the floor (at most the capacity), so a late callback has the same margin on both. Latency +16 ms at most, where it was raised
        int ms = aa_buf_ms(); int32_t cur = AAudioStream_getBufferSizeInFrames(*out), cap = AAudioStream_getBufferCapacityInFrames(*out), want = ms * (RATE / 1000);
        if (want > cap) want = cap;
        if (ms > 0 && cur > 0 && cur < want) {
            int32_t got = AAudioStream_setBufferSizeInFrames(*out, want);
            static _Atomic int told; if (atomic_fetch_add(&told, 1) < 4) LOG("audio: AAudio buffer %d -> %d frames (floor %d ms, DM_AUDIO_BUFFER_MS; capacity %d)", (int)cur, (int)(got > 0 ? got : cur), ms, (int)cap);
        }
    }
    if (a->mix) LOG("audio：游戏给 6 声道（5.1）→ 交给声卡前自己下混成双声道（中置 / 后环绕 ×0.707 并入左右，低音不并入，整体 −3 dB、饱和截断）");
    *ll = AAudioStream_getPerformanceMode(*out) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    LOG("audio：AAudio 流 %d Hz %d 声道，性能模式 %d%s，每次回调 %d 帧，缓冲 %d / %d 帧", AAudioStream_getSampleRate(*out), AAudioStream_getChannelCount(*out),
        (int)AAudioStream_getPerformanceMode(*out), safe ? "（NONE：安全模式）" : "", AAudioStream_getFramesPerBurst(*out), AAudioStream_getBufferSizeInFrames(*out), AAudioStream_getBufferCapacityInFrames(*out));
    return 0;
}
static int au_open(aout *a) {   // 0 = 声卡开好了；-1 = 开不了（a->au 置空，调用方走静音接收端） | 0 = the sound card is open; -1 = it cannot be opened (a->au set to null, the caller uses the silent sink)
    int ll = 0, r = aa_open(a, &a->au, &ll); a->ll = ll; a->fake_t0 = 0;
    if (r) LOG("【红】这一路改用静音接收端（不出声）");
    return r;
}
static int dev_start(aout *a) { return AAudioStream_requestStart(a->au) == AAUDIO_OK ? 0 : -1; }
static void dev_stop(aout *a) { AAudioStream_requestStop(a->au); }
static void dev_close(aout *a) { AAudioStream_close(a->au); a->au = NULL; }
#endif
// 静音接收端：每 5 ms 按 48 kHz 实时消耗
// Silent sink: consumes in real time at 48 kHz every 5 ms
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8];
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns(); if (t > now) usleep((useconds_t)((t - now) / 1000));
        pull(a, scratch, bytes);
    }
    return NULL;
}
// 墙钟消费者开 / 关（a->dm 里调；重复调无害）。开着时 owner = 1，声卡回调只出静音；关掉（join 完）才把队列交还声卡
// Wall-clock consumer on / off (called under a->dm; repeated calls are harmless). While on, owner = 1 and the sound card callback outputs silence; the queue
// goes back to the sound card only after it is off (joined)
static void null_on(aout *a) {
    if (a->null_live) return;
    atomic_store_explicit(&a->owner, 1, memory_order_release); atomic_store_explicit(&a->null_run, 1, memory_order_release);
    if (pthread_create(&a->null_th, NULL, null_sink, a)) { atomic_store(&a->null_run, 0); atomic_store(&a->owner, 0); LOG("【红】静音接收端线程建不出来"); return; }
    a->null_live = 1;
}
static void null_off(aout *a) {
    if (!a->null_live) return;
    atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); a->null_live = 0;
    atomic_store_explicit(&a->owner, 0, memory_order_release);
}
// 声卡流启动 / 停 / 关的记账包装（a->dm 里调） | Bookkeeping wrappers for starting / stopping / closing the sound card stream (called under a->dm)
static int dev_go(aout *a) { if (a->dev_on) return 0; int r = dev_start(a); if (!r) { a->dev_on = 1; a->dev_start_ns = now_ns(); } return r; }
static void dev_halt(aout *a) { if (a->dev_on) { dev_stop(a); a->dev_on = 0; } }
static void dev_drop(aout *a) { dev_close(a); a->dev_on = 0; }
#if !defined(__APPLE__) && !defined(__OHOS__)
// [DOOM] 看护线程（每路真出声的 AudioOut 一条，issues #12/#20）。Switch 上 audout 不管下游怎样都按采样率把缓冲放回，客体从来看不到
// 「设备停了」；这里照做：声卡流报错（错误回调）或在播却 DM_AUDIO_STALL_MS 没有回调时，先把这一路交给墙钟（静音接收端按 48 kHz 释放
// 客体缓冲，游戏照常），再在锁外关掉旧流（卡在 processData 的回调线程最多再等 1 s）、开新流（低延迟出的事 → 本进程改用 NONE），
// 新流第一次回调后把队列交还声卡。重开连续失败 3 次（每次隔 1 s）就放弃：这一路留在静音接收端（不出声，节奏不断）。
// [DOOM] Keeper thread (one per real-sound AudioOut, issues #12/#20). On the Switch audout hands buffers back at the sample rate whatever happens downstream,
// and the guest never sees "the device stopped"; this does the same: when the sound card stream reports an error (error callback) or is playing yet has had
// no callback for DM_AUDIO_STALL_MS, the output first goes to the wall clock (the silent sink releases guest buffers at 48 kHz, the game carries on), then
// the old stream is closed outside the lock (a callback thread stuck in processData may take up to 1 more second), a new one is opened (trouble on a low-latency
// stream -> this process switches to NONE), and the queue goes back to the sound card at the new stream's first callback. After 3 failed reopens in a row
// (1 s apart) it gives up: the output stays on the silent sink (no sound, pacing unbroken).
enum { AA_STALL = 1 };   // 伪错误码：在播但没有回调 | pseudo error code: playing but no callbacks
static void *aa_keeper(void *p) {
    aout *a = p;
    pthread_setname_np(pthread_self(), "dm-audio-keep");
    while (atomic_load_explicit(&a->keep_run, memory_order_acquire)) {
        sem_down_ms(a->keep_sem, 50);
        if (!atomic_load_explicit(&a->keep_run, memory_order_acquire)) break;
        uint64_t now = now_ns(), stall = aa_stall_ns();
        pthread_mutex_lock(&a->dm);
        int started = atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED, susp = g_suspended, err = 0; uint64_t quiet = 0;
        { int e = atomic_exchange(&a->dev_err, 0); if (e && a->au && atomic_load(&a->err_st) == (uintptr_t)a->au) err = e; }   // 已关掉的旧流晚到的错不算 | a late error from an already closed stream does not count
        if (!err && a->au && a->dev_on && started && !susp) {
            if (a->handover) {   // 新流已启动、墙钟还在顶：第一次回调就交还；2 s 都没有回调 = 新流也不动 | new stream started, the wall clock still covering: hand back at its first callback; none for 2 s = the new stream does not run either
                if (atomic_load(&a->renders) != a->hand_renders) {
                    null_off(a); a->handover = 0; a->fails = 0;
                    if (a->incidents <= 10 || a->incidents % 100 == 0) LOG("audio：音频流已重开并接上：墙钟桥接 %.0f ms，期间按实时释放客体缓冲 %llu 块", (double)(now - a->bridge_ns) / 1e6, (unsigned long long)(atomic_load(&a->rel_n) - a->bridge_rel0));
                } else if (now - a->dev_start_ns > 2000000000ull) { err = AA_STALL; quiet = now - a->dev_start_ns; }
            } else if (stall && !atomic_load(&a->owner)) {
                uint64_t last = atomic_load(&a->last_render_ns); if (last < a->dev_start_ns) last = a->dev_start_ns;
                if (now > last && now - last > stall) { err = AA_STALL; quiet = now - last; }
            }
        }
        if (err) {   // 先桥接，再锁外关旧流 | bridge first, then close the old stream outside the lock
            dm_audev old = a->au; int was_ll = a->ll, hand = a->handover;
            a->au = NULL; a->dev_on = 0; a->handover = 0; a->reopen = 1; a->next_try_ns = 0;
            if (!hand) { a->bridge_ns = now; a->bridge_rel0 = atomic_load(&a->rel_n); } else a->fails++;   // 重开的流一直不回调也算一次失败 | a reopened stream that never calls back counts as a failure
            if (started && !susp) null_on(a);   // 客体缓冲照常按 48 kHz 放回（Switch 的 audout 也是这样） | guest buffers keep coming back at 48 kHz (as the Switch's audout does)
            int nth = ++a->incidents, loud = nth <= 10 || nth % 100 == 0;   // 设备反复出事时日志限流；这一路出事 50 次就不再重开 | rate-limit the log when the device keeps failing; after 50 incidents on this output it is not reopened any more
            if (nth >= 50) a->fails = 3;
            pthread_mutex_unlock(&a->dm);
            if (loud) {
                if (err == AAUDIO_ERROR_DISCONNECTED) LOG("audio：音频流断开（输出设备变化）：墙钟接手，重开");
                else if (err == AA_STALL) LOG("【红】audio：音频流%s卡死（%s %.0f ms 没有回调）：墙钟接手释放客体缓冲，关掉重开（第 %d 次；issues #12/#20）", was_ll ? "（低延迟）" : "",
                                             hand ? "重开的流启动后" : "在播却", (double)quiet / 1e6, nth);
                else LOG("audio：音频流错误 %d：墙钟接手释放客体缓冲，关掉重开（第 %d 次）", err, nth);
            }
            if (err != AAUDIO_ERROR_DISCONNECTED && was_ll) aa_go_safe(err == AA_STALL ? "卡死" : "报错");
            AAudioStream_close(old);
            pthread_mutex_lock(&a->dm);
        }
        if (a->reopen && !a->au && a->fails >= 3) { a->reopen = 0; LOG("【红】audio：音频流连续 %d 次重开失败：这一路留在静音接收端（不出声，节奏照常）", a->fails); }
        if (a->reopen && !a->au && atomic_load_explicit(&a->keep_run, memory_order_acquire) && now_ns() >= a->next_try_ns) {
            pthread_mutex_unlock(&a->dm);
            dm_audev nu = NULL; int ll = 0, r = aa_open(a, &nu, &ll);
            pthread_mutex_lock(&a->dm);
            started = atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED; susp = g_suspended;
            if (!r && atomic_load_explicit(&a->keep_run, memory_order_acquire)) {
                a->au = nu; a->ll = ll; a->reopen = 0; a->fake_t0 = 0; a->fake_logged = 0;
                if (started && !susp) {   // 没在播：留着空闲的新流，Start / 恢复时再启动 | not playing: keep the new stream idle, Start / resume starts it
                    if (dev_go(a)) { LOG("【红】audio：重开的音频流启动失败"); dev_drop(a); a->reopen = 1; a->fails++; if (a->ll) aa_go_safe("启动失败"); }
                    else if (a->null_live) { a->handover = 1; a->hand_renders = atomic_load(&a->renders); }
                }
            } else { if (!r) AAudioStream_close(nu); a->fails++; }
            if (a->reopen) a->next_try_ns = now_ns() + 1000000000ull;
        }
        // [DOOM] issue #32: every 10 s while the sound card plays: callbacks, how many found the queue empty (the producer was late: the
        // render thread for HostPcmOut, Wwise for the 6-channel one) and the stream's own underruns (the callback was late). Logged when
        // either is non-zero, otherwise once a minute
        if (a->au && a->dev_on && !a->handover && !atomic_load(&a->owner) && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED && !g_suspended) {
            uint64_t t = now_ns(), rn = atomic_load(&a->renders), dn = atomic_load(&a->drys);
            int32_t xr = AAudioStream_getXRunCount(a->au);
            if ((uintptr_t)a->au != a->dg_st) { a->dg_st = (uintptr_t)a->au; a->dg_xlast = xr > 0 ? xr : 0; }
            else if (xr > a->dg_xlast) { a->dg_x += (uint64_t)(xr - a->dg_xlast); a->dg_xlast = xr; }
            if (!a->dg_t0) { a->dg_t0 = t; a->dg_r0 = rn; a->dg_d0 = dn; a->dg_x = 0; }
            else if (t - a->dg_t0 >= 10000000000ull) {
                uint64_t d = dn - a->dg_d0, calls = rn - a->dg_r0;
                // every callback empty = an idle stream (the game opened it and feeds nothing: DOOM's 2-channel default out in the menus),
                // not a late producer; only a window with some data and some empty callbacks is a gap in the sound
                int idle = d && d == calls;
                if ((d && !idle) || a->dg_x || ++a->dg_quiet >= 6) {
                    LOG("%saudio: %s %dch last %.1f s: %llu callbacks, %llu found the queue empty (%s), %llu device underruns (callback late); buffer %d / %d frames, burst %d, perf %d",
                        (d && !idle) || a->dg_x ? "[RED] " : "", a->name, a->ch, (t - a->dg_t0) / 1e9, (unsigned long long)calls, (unsigned long long)d,
                        idle ? "idle: nothing written" : "producer late", (unsigned long long)a->dg_x,
                        (int)AAudioStream_getBufferSizeInFrames(a->au), (int)AAudioStream_getBufferCapacityInFrames(a->au), (int)AAudioStream_getFramesPerBurst(a->au), (int)AAudioStream_getPerformanceMode(a->au));
                    a->dg_quiet = 0;
                }
                a->dg_t0 = t; a->dg_r0 = rn; a->dg_d0 = dn; a->dg_x = 0;
            }
        } else a->dg_t0 = 0;   // paused / bridging / stopped: the next window starts when it plays again
        pthread_mutex_unlock(&a->dm);
    }
    return NULL;
}
#endif
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : DM_PHONE; }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }，0 = 系统默认（48 kHz / 2 声道）
// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "音频输出参数默认值", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "音频输出设备列表；宿主 1 个", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "连 audout 服务；宿主 AudioUnit / 静音接收端",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) {
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) die("AudioOut 采样率 %d 不支持（只做 48 kHz）", rate);
    aout *a = calloc(1, sizeof *a);
    if (!a) die("AudioOut alloc");
    pthread_mutex_init(&a->m, NULL); pthread_mutex_init(&a->dm, NULL); a->event = ev; a->ch = ch;
    snprintf(a->name, sizeof a->name, "%s", name ? name : "default");   // [DOOM] issue #32: for aa_keeper's 10 s line
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) {   // [DOOM] 不再 die：回 Horizon 的错误码，游戏（Wwise）留在它的哑汇点、过一秒再试 | [DOOM] no longer dies: return Horizon's error code; the game (Wwise) stays on its dummy sink and retries a second later
        pthread_mutex_unlock(&g_aos_lock);
        static _Atomic unsigned n_full; unsigned k = atomic_fetch_add(&n_full, 1);
        if (k < 3 || k % 60 == 0) LOG("【红】audio::OpenAudioOut：同时打开已有 %d 个，回 %#x（out of sessions；第 %u 次；issues #12/#20）", AOMAX, RES_AUDOUT_OUT_OF_SESSIONS, k + 1);
        pthread_mutex_destroy(&a->dm); pthread_mutex_destroy(&a->m); free(a);
        return RES_AUDOUT_OUT_OF_SESSIONS;
    }
    g_aos[slot] = a;   // 先登记（a->au 为空，暂停 / 恢复跳过它），再开声卡 | registered first (a->au is null, suspend / resume skip it), then the sound card is opened
    pthread_mutex_unlock(&g_aos_lock);
    a->notify_sem = sem_new();
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // 缓冲释放事件：客体线程上绑内核事件（自动清除），之后由音频线程触发 | Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    objmap_put(g, a);
    // [DOOM] Horizon 的 OpenAudioOut 把会话对象写进 AudioOut 头 8 字节；游戏的 Wwise 汇点 Term（main+0x21ff4f0）只在这个字非 0 时
    // 才 GetAudioOutState / StopAudioOut / CloseAudioOut。以前这里什么都不写（汇点构造时把它清 0，main+0x21ff290），
    // 每次重建汇点旧的 AudioOut 都不关：这就是 issues #12/#20 里「8 次打开没有一次关闭」。写宿主对象指针（游戏只判非 0），Close 时清掉
    // [DOOM] Horizon's OpenAudioOut writes the session object into the first 8 bytes of the AudioOut; the game's Wwise sink Term (main+0x21ff4f0) calls
    // GetAudioOutState / StopAudioOut / CloseAudioOut only when that word is non-zero. Nothing was written here (the sink zeroes it at construction,
    // main+0x21ff290), so every sink rebuild left the old AudioOut open: the "8 opens without a single close" of issues #12/#20. Write the host object
    // pointer (the game only tests it for non-zero); Close clears it
    *(void **)g = a;
    LOG("audio::OpenAudioOut %s 声道 %d %s", name ? name : "(默认)", ch, real_audio() ? "真出声" : "静音接收端");
    if (real_audio()) {
#if !defined(__APPLE__) && !defined(__OHOS__)
        a->keep_sem = sem_new();   // 错误回调可能在流一开就来，先建好 | the error callback may come as soon as the stream is open: create it first
#endif
        pthread_mutex_lock(&a->dm); int r = au_open(a); pthread_mutex_unlock(&a->dm);   // 开不了就 a->au 为空，开始播放时走静音接收端 | if it cannot be opened, a->au is null, and playback goes to the silent sink when it starts
#if !defined(__APPLE__) && !defined(__OHOS__)
        if (!r) { atomic_store(&a->keep_run, 1); if (!pthread_create(&a->keep_th, NULL, aa_keeper, a)) a->keep_live = 1; else LOG("【红】audio：看护线程建不出来（流卡死时不会自动恢复）"); }
#else
        (void)r;
#endif
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "开始播放", Result, h_au_Start, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STARTED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was == STATE_STARTED) return RES_OK;
    pthread_mutex_lock(&a->dm);
    if (a->au && !g_suspended && dev_go(a)) {
#if !defined(__APPLE__) && !defined(__OHOS__)
        // [DOOM] issues #12/#20：MMAP 低延迟流 requestStart 报「stolen」。交给看护线程重开（低延迟出的事 → NONE），其间墙钟消费 | [DOOM] issues #12/#20: an MMAP low-latency stream's requestStart reported "stolen". The keeper reopens it (trouble on low latency -> NONE), the wall clock consumes meanwhile
        LOG("【红】声卡启动失败：墙钟接手，交给看护线程重开");
        if (a->ll) aa_go_safe("启动失败");
        dev_drop(a);
        if (a->keep_live) { a->reopen = 1; a->next_try_ns = 0; a->bridge_ns = now_ns(); a->bridge_rel0 = atomic_load(&a->rel_n); sem_up(a->keep_sem); }
#else
        LOG("【红】声卡启动失败：这一路改用静音接收端（不出声）"); dev_drop(a);
#endif
    }
    if (!a->au || g_suspended) null_on(a);
    pthread_mutex_unlock(&a->dm);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "停止播放", void, h_au_Stop, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m); int was = atomic_load_explicit(&a->state, memory_order_relaxed); atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release); pthread_mutex_unlock(&a->m);
    if (was != STATE_STARTED) return;
    pthread_mutex_lock(&a->dm);
    if (a->au) dev_halt(a);
    null_off(a); a->handover = 0;   // 静音接收端（没有声卡、暂停期间接手、桥接中的） | the silent sink (no sound card, taking over during suspension, bridging)
    pthread_mutex_unlock(&a->dm);
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "关闭音频输出", void, h_au_Close, void *g) {
    h_au_Stop(g);
    aout *a = objmap_del(g);
    LOG("audio::CloseAudioOut 声道 %d", a->ch);   // [DOOM] 对照 OpenAudioOut：issues #12/#20 里从来没有 Close | [DOOM] to pair with OpenAudioOut: issues #12/#20 never had a Close
    if (a->keep_live) { atomic_store_explicit(&a->keep_run, 0, memory_order_release); sem_up(a->keep_sem); pthread_join(a->keep_th, NULL); a->keep_live = 0; }
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    sem_up(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    pthread_mutex_lock(&a->dm); if (a->au) dev_drop(a); null_off(a); pthread_mutex_unlock(&a->dm);
    *(void **)g = NULL;   // [DOOM] AudioOut 不再持有会话（见 h_au_Open） | [DOOM] the AudioOut no longer holds a session (see h_au_Open)
    sem_free(a->notify_sem);
    if (a->rel_sem) sem_free(a->rel_sem);
    if (a->keep_sem) sem_free(a->keep_sem);
    free(a->mixbuf); pthread_mutex_destroy(&a->dm); pthread_mutex_destroy(&a->m); free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "播放状态", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "采样率", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "声道数", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "采样格式；只做 PCM16", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "缓冲描述；写进客体结构体前 24 字节", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "缓冲数据指针", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "缓冲数据大小", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "追加待播缓冲", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    // 诊断开关 DM_AUDIO_DUMP=<文件>：游戏追加的每块 PCM 原样续写（16 位、48 kHz、声道数见 OpenAudioOut 日志，常见 6）。判「某个音播没播」用：
    // 拿原版音频对它做互相关（一档掉落音就这么验的）。只在设了变量时打开文件，没设时这里只多一次已缓存的判断
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("【红】DM_AUDIO_DUMP 打不开 %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool：队列满时 false（真机同样有上限） | bool: false when the queue is full (the real device has a limit too)
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "取已播完的缓冲；没有则 NULL", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// 前端生命周期 / 音频会话打断：暂停真出声，期间由静音接收端接手按实时消耗（节奏不断）；恢复时切回 AudioUnit。
// 前端在 切后台 / 打断开始 时调 1，回前台 / 打断结束（shouldResume）时调 0。
// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on; int failed = 0;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a) continue;
            pthread_mutex_lock(&a->dm);   // 锁序 g_aos_lock → a->dm | lock order g_aos_lock -> a->dm
            if (!a->au || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) { pthread_mutex_unlock(&a->dm); continue; }
            // 暂停期间改由静音接收端按实时消耗：否则缓冲不再释放，FMOD 输出线程永久等待、游戏音频节奏卡死（来电打断同理）
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) { dev_halt(a); a->handover = 0; null_on(a); }
            else { null_off(a);
                   int st = dev_go(a);
                   if (st) {   // 会话还被别人占着（通话没挂就切回游戏、打断结束时人还在别的 App）：不致命，继续由静音接收端顶着，等下一次恢复再试 | The session is still held by someone else (switched back to the game with a call not yet hung up, or the user is still in another app when the interruption ends): not fatal, the silent sink keeps covering, and we retry at the next resume
                       null_on(a);
#if !defined(__APPLE__) && !defined(__OHOS__)
                       if (a->keep_live) {   // [DOOM] 安卓：看护线程关掉重开（隔 1 s 再试，最多 3 次），不等下一次恢复 | [DOOM] Android: the keeper closes and reopens it (retries 1 s apart, at most 3), without waiting for the next resume
                           LOG("【红】声卡恢复失败（状态 %d）：保持静音接收端，交给看护线程重开", st);
                           dm_audev old = a->au; a->au = NULL; a->dev_on = 0; a->reopen = 1; a->next_try_ns = 0; a->bridge_ns = now_ns(); a->bridge_rel0 = atomic_load(&a->rel_n);
                           AAudioStream_close(old); sem_up(a->keep_sem);
                           pthread_mutex_unlock(&a->dm); continue; }
#endif
                       LOG("【红】声卡恢复失败（状态 %d）：保持静音接收端，等下次恢复重试", st);
                       failed = 1; } }
            pthread_mutex_unlock(&a->dm);
        }
        if (failed) g_suspended = 1;   // 没恢复成：状态留在「暂停」，下次调 0 会再试 | Resume did not succeed: the state stays "suspended", and the next call with 0 will retry
        LOG("audio %s", on ? "暂停（切后台 / 被打断）" : failed ? "恢复未成" : "恢复（回前台 / 打断结束）");
    }
    pthread_mutex_unlock(&g_aos_lock);
}

// ---- 给其他 HLE 文件用的入口 | Entry points for other HLE files ----
// OpenAudioOut 的宿主实现（DOOM 的 OpenDefaultAudioOut 转到这里）
// Host implementation of OpenAudioOut (DOOM's OpenDefaultAudioOut forwards here)
Result dm_audio_open_out(void *g, void *ev, const char *name, const void *param) { return h_au_Open(g, ev, name, param); }
// 宿主音源输出：与 AudioOut 同一条出声路径（AAudio / 静音接收端 / 5.1 下混 / 切后台），样本由 fill 回调现算。
// fill 在声卡回调线程（或静音接收端线程）上调，一次只有一个线程。返回的句柄只给下面三个函数用。
// Host source output: the same output path as AudioOut (AAudio / silent sink / 5.1 downmix / backgrounding), samples rendered by the fill callback.
// fill is called on the audio callback thread (or the silent sink thread), one thread at a time. The returned handle is only for the three functions below.
void *dm_audio_source_open(int ch, dm_audio_fill_fn fill, void *ctx, const char *name) {
    void *key = malloc(16); if (!key) die("audio source key");
    int32_t param[2] = { RATE, ch };   // AudioOutParameter：采样率 + 声道数（低 16 位） | AudioOutParameter: sample rate + channel count (low 16 bits)
    h_au_Open(key, NULL, name, (const uint8_t *)param);
    aout *a = ao(key); a->src_ctx = ctx; a->src = fill;
    return key;
}
void dm_audio_source_start(void *h) { h_au_Start(h); }
void dm_audio_source_stop(void *h) { h_au_Stop(h); }
void dm_audio_source_close(void *h) { h_au_Close(h); free(h); }

// Host PCM output (nxnative dm/host/hle_audio.c dm_pcmout_*, Copyright (c) 2026 seedds, MIT): an AudioOut the host feeds itself with
// AudioOutBuffer-shaped blocks ({ptr, cap, size}) of 48 kHz int16 interleaved PCM; same output path (AAudio / silent sink /
// backgrounding). The producer appends blocks, takes released ones back and sleeps in dm_pcmout_wait until the output releases one,
// so its pacing follows the sound card clock (hle_audren.c: the SDK's own audio renderer)
void *dm_pcmout_open(int ch) {
    void *key = malloc(16); if (!key) die("pcm out key");
    int32_t param[2] = { RATE, ch };
    h_au_Open(key, NULL, "HostPcmOut", (const uint8_t *)param);
    ao(key)->rel_sem = sem_new();
    return key;
}
void dm_pcmout_wait(void *k, int ms) { sem_down_ms(ao(k)->rel_sem, ms); }
void dm_pcmout_start(void *k) { h_au_Start(k); }
void dm_pcmout_close(void *k) { h_au_Close(k); free(k); }
int dm_pcmout_append(void *k, void *b) { return (int)h_au_Append(k, b); }
void *dm_pcmout_released(void *k) { return h_au_Released(k); }
