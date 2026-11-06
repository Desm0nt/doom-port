// [DOOM] hle_audio.c 安卓路径单测（issues #12/#20），AAudio 用 aa_mock/aaudio/AAudio.h 的替身。Linux / WSL：
//   cd dm/host/tests && gcc -O1 -g -D_GNU_SOURCE -pthread -Wall -Wno-unused-function -Iaa_mock -I../../android/compat/include -I.. t_audio_aa.c -o /tmp/t_audio_aa && /tmp/t_audio_aa
//   （加 -fsanitize=thread 查竞争）
// 场景：AudioOut 头 8 字节写会话（Wwise Term 才会 Close）、打开数超限回 0xa99 不 die、照 Wwise 的 Term 反复建 / 拆不漏槽；
// 低延迟流卡死（Samsung：卡住 → 1 s 后 TIMEOUT）时客体缓冲照常释放、降级 NONE 重开并交还；只靠错误回调时同样恢复；
// requestStart 失败（「stolen」）、DM_AUDIO_FAKE_STALL、暂停 / 恢复、桥接中途 Close。
// [DOOM] Unit test of hle_audio.c's Android path (issues #12/#20), with AAudio replaced by aa_mock/aaudio/AAudio.h. Linux / WSL: see the command above
// (add -fsanitize=thread to look for races). Scenarios: the AudioOut's first 8 bytes hold the session (so Wwise's Term closes it), too many opens return 0xa99
// without dying, repeated build / tear-down the way Wwise's Term does it leaks no slot; when a low-latency stream stalls (Samsung: stuck -> TIMEOUT after 1 s)
// guest buffers keep being released, the stream is reopened as NONE and handed back; the same with error callbacks only; requestStart failure ("stolen"),
// DM_AUDIO_FAKE_STALL, suspend / resume, Close in the middle of a bridge.
#include "../hle_audio.c"
#include <stdio.h>
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static pthread_mutex_t om_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { const void *k; void *v; } om[32];
void *objmap_get(const void *k) { void *v = NULL; pthread_mutex_lock(&om_mu); for (int i = 0; i < 32; i++) if (om[i].k == k) { v = om[i].v; break; } pthread_mutex_unlock(&om_mu); return v; }
void objmap_put(const void *k, void *v) { pthread_mutex_lock(&om_mu); for (int i = 0; i < 32; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; break; } pthread_mutex_unlock(&om_mu); }
void *objmap_del(const void *k) { void *v = NULL; pthread_mutex_lock(&om_mu); for (int i = 0; i < 32; i++) if (om[i].k == k) { om[i].k = 0; v = om[i].v; break; } pthread_mutex_unlock(&om_mu); return v; }
static _Atomic int g_sig;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add(&g_sig, 1); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }

static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s ", ok_ ? "✓" : "✗"); printf(__VA_ARGS__); putchar('\n'); fails += !ok_; } while (0)

enum { BLK = 480, NB = 4 };   // 10 ms 一块，在途 4 块（Wwise 的 NX 汇点也是几块 5–10 ms 的环） | 10 ms per block, 4 in flight (Wwise's NX sink is a ring of a few 5-10 ms blocks too)
typedef struct { uint64_t maxgap_ms, frames, ms; } play_t;
// 像游戏那样喂：在途不足 NB 块就追加，有释放就收回；记最长的「没有释放」间隔 | feed it like the game: append while fewer than NB blocks are in flight, take released ones back; record the longest gap without a release
static play_t play(void *out, int ch, int ms, void (*mid)(void *), int mid_ms) {
    static int16_t pcm[NB][BLK * 6]; static gbuf b[NB]; gbuf *fr[NB]; int nf = NB; play_t r = {0, 0, 0};
    for (int i = 0; i < NB; i++) { h_au_SetBuf(&b[i], pcm[i], sizeof pcm[i], (uint64_t)BLK * 2 * (uint64_t)ch); fr[i] = &b[i]; }
    uint64_t t0 = now_ns(), last = t0; int mid_done = 0;
    while (now_ns() - t0 < (uint64_t)ms * 1000000ull) {
        while (nf && h_au_Append(out, fr[nf - 1])) nf--;
        gbuf *g; int got = 0; while ((g = h_au_Released(out))) { fr[nf++] = g; r.frames += BLK; got = 1; }
        uint64_t now = now_ns();
        if (got) { if ((now - last) / 1000000 > r.maxgap_ms) r.maxgap_ms = (now - last) / 1000000; last = now; }
        if (mid && !mid_done && now - t0 > (uint64_t)mid_ms * 1000000ull) { mid(out); mid_done = 1; }
        usleep(1000);
    }
    uint64_t now = now_ns(); if ((now - last) / 1000000 > r.maxgap_ms) r.maxgap_ms = (now - last) / 1000000;
    r.ms = (now - t0) / 1000000;
    // 收回在途的块，测试结束时静态缓冲不能还挂在队列里 | take the in-flight blocks back: the static buffers must not stay queued after the test
    h_au_Stop(out);
    { aout *a = ao(out); atomic_store(&a->q_head, atomic_load(&a->q_tail)); atomic_store(&a->rel_head, atomic_load(&a->rel_tail)); a->pos = 0; }   // 已停：没有消费者 | stopped: no consumer
    return r;
}
static void wwise_term(void *ao_) {   // main+0x21ff4f0：只有 AudioOut 头一个字非 0 才 Stop / Close | only when the AudioOut's first word is non-zero does it Stop / Close
    if (*(uint32_t *)ao_ || ((uint32_t *)ao_)[1]) { if (h_au_State(ao_) == STATE_STARTED) h_au_Stop(ao_); h_au_Close(ao_); }
}
static aout *A(void *o) { return ao(o); }
static pthread_t g_susp_th;
static void *susp_main(void *p) { (void)p; dm_audio_suspend(1); usleep(300000); dm_audio_suspend(0); return NULL; }
static void do_suspend(void *o) { (void)o; pthread_create(&g_susp_th, NULL, susp_main, NULL); }   // 另起线程：喂数据的循环不能停 | on another thread: the feeding loop must not stop

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t prm[16]; char ev[64]; h_au_InitParam(prm); *(uint16_t *)(prm + 4) = 6;
    {   // 1. 会话字、上限、Wwise 式反复建拆 | session word, limit, Wwise-style rebuild loop
        setenv("DM_AUDIO", "0", 1);
        static char outs[AOMAX + 1][64]; int ok = 1, nz = 1;
        for (int i = 0; i < AOMAX; i++) { memset(outs[i], 0, 64); ok &= h_au_Open(outs[i], ev, NULL, prm) == RES_OK; nz &= *(void **)outs[i] != NULL; }
        CHECK(ok && nz, "打开 %d 个都成功，AudioOut 头 8 字节非 0（Horizon 写会话对象）| %d opens ok, first 8 bytes non-zero", AOMAX, AOMAX);
        memset(outs[AOMAX], 0, 64);
        Result r = h_au_Open(outs[AOMAX], ev, NULL, prm);
        CHECK(r == RES_AUDOUT_OUT_OF_SESSIONS && !*(void **)outs[AOMAX], "第 %d 个回 %#x（期望 0xa99），不 die，头字仍为 0 | open #%d -> %#x, no die", AOMAX + 1, r, AOMAX + 1, r);
        for (int i = 0; i < AOMAX; i++) { h_au_Start(outs[i]); wwise_term(outs[i]); }
        int z = 1; for (int i = 0; i < AOMAX; i++) z &= !*(void **)outs[i];
        CHECK(z, "Wwise 式 Term 关掉全部，头字清 0 | Wwise-style Term closed all, word cleared");
        char w[64] = {0}; int all = 1;
        for (int i = 0; i < 40; i++) { all &= h_au_Open(w, ev, NULL, prm) == RES_OK; h_au_Start(w); wwise_term(w); }
        CHECK(all, "照 Wwise 每秒重建汇点 40 次不漏槽 | 40 Wwise-style sink rebuilds leak no slot");
    }
    setenv("DM_AUDIO", "1", 1);
    {   // 2. Samsung：低延迟流 150 ms 后卡死，1 s 后 TIMEOUT | Samsung: the low-latency stream stalls after 150 ms, TIMEOUT 1 s later
        atomic_store(&g_aa_safe, 0); atomic_store(&g_stall_ms, 250); atomic_store(&mock_stall_ll_ms, 150);
        char o[64] = {0}; h_au_Open(o, ev, NULL, prm); int ll0 = A(o)->ll; h_au_Start(o);
        int none0 = mock_opens_none; uint64_t cbn0;
        play_t p = play(o, 6, 1500, NULL, 0); (void)p;
        cbn0 = atomic_load(&mock_cb_none);
        h_au_Start(o); play_t q = play(o, 6, 500, NULL, 0);
        aout *a = A(o);
        CHECK(ll0, "起初是低延迟流 | starts as low latency");
        CHECK(p.maxgap_ms < 450, "卡死期间客体最长 %llu ms 没有释放（期望 < 450：250 ms 检测 + 余量）| longest gap without a release %llu ms", (unsigned long long)p.maxgap_ms, (unsigned long long)p.maxgap_ms);
        CHECK(p.frames * 1000 / 48000 >= p.ms * 80 / 100, "1.5 s 内释放 %llu ms 的音频（期望 ≥ 80%%）| released %llu ms of audio in %llu ms", (unsigned long long)(p.frames * 1000 / 48000), (unsigned long long)(p.frames * 1000 / 48000), (unsigned long long)p.ms);
        CHECK(atomic_load(&g_aa_safe) == 1 && mock_opens_none > none0 && a->au && !a->ll, "降级：重开成 NONE（NONE 打开 %d 次）| fell back to NONE", mock_opens_none - none0);
        CHECK(!a->null_live && !atomic_load(&a->owner) && !a->handover && atomic_load(&mock_cb_none) > cbn0 && q.maxgap_ms < 100,
              "交还：声卡（NONE）在消费，墙钟已停，之后最长间隔 %llu ms | handed back to the sound card, gap after %llu ms", (unsigned long long)q.maxgap_ms, (unsigned long long)q.maxgap_ms);
        h_au_Close(o);
    }
    {   // 3. 只靠错误回调（DM_AUDIO_STALL_MS=0）：TIMEOUT 来了才恢复 | error callback only (DM_AUDIO_STALL_MS=0): recovery only when the TIMEOUT comes
        atomic_store(&g_aa_safe, 0); atomic_store(&g_stall_ms, 0); atomic_store(&mock_stall_ll_ms, 150); int to0 = mock_timeouts;
        char o[64] = {0}; h_au_Open(o, ev, NULL, prm); h_au_Start(o);
        play_t p = play(o, 6, 2500, NULL, 0); aout *a = A(o);
        CHECK(mock_timeouts > to0 && atomic_load(&g_aa_safe) == 1 && a->au && !a->ll && !a->null_live, "TIMEOUT 错误回调 → 重开成 NONE 并交还 | TIMEOUT error callback -> reopened as NONE and handed back");
        CHECK(p.maxgap_ms < 1400, "最长无释放 %llu ms（只靠错误：卡 1 s + 余量，期望 < 1400）| longest gap %llu ms", (unsigned long long)p.maxgap_ms, (unsigned long long)p.maxgap_ms);
        h_au_Close(o); atomic_store(&g_stall_ms, 250);
    }
    {   // 4. requestStart 失败（「stolen」）| requestStart fails ("stolen")
        atomic_store(&g_aa_safe, 0); atomic_store(&mock_stall_ll_ms, 0); atomic_store(&mock_fail_start_ll, 1);
        char o[64] = {0}; h_au_Open(o, ev, NULL, prm); h_au_Start(o);
        play_t p = play(o, 6, 800, NULL, 0); aout *a = A(o);
        CHECK(p.maxgap_ms < 100 && a->au && !a->ll && !a->null_live && atomic_load(&g_aa_safe) == 1, "启动失败 → 墙钟立刻接手（最长间隔 %llu ms），NONE 重开后交还 | start failure -> wall clock at once (gap %llu ms), NONE reopen handed back", (unsigned long long)p.maxgap_ms, (unsigned long long)p.maxgap_ms);
        h_au_Close(o);
    }
    {   // 5. DM_AUDIO_FAKE_STALL | DM_AUDIO_FAKE_STALL
        atomic_store(&g_aa_safe, 0); g_fake_ms = 200;
        char o[64] = {0}; h_au_Open(o, ev, NULL, prm); h_au_Start(o);
        play_t p = play(o, 6, 1200, NULL, 0); aout *a = A(o);
        CHECK(p.maxgap_ms < 450 && a->au && !a->ll && !a->null_live && a->fake_logged == 0, "假卡死 200 ms → 检测、桥接（最长间隔 %llu ms）、NONE 重开交还 | fake stall -> bridged (gap %llu ms), NONE reopen handed back", (unsigned long long)p.maxgap_ms, (unsigned long long)p.maxgap_ms);
        h_au_Close(o); g_fake_ms = 0;
    }
    {   // 6. 暂停 / 恢复（NONE 流）| suspend / resume (NONE stream)
        char o[64] = {0}; h_au_Open(o, ev, NULL, prm); h_au_Start(o);
        play_t p = play(o, 6, 1000, do_suspend, 300); pthread_join(g_susp_th, NULL); h_au_Start(o); usleep(100000); aout *a = A(o);
        CHECK(p.maxgap_ms < 100 && a->dev_on && !a->null_live && !atomic_load(&a->owner), "暂停 300 ms 中节奏不断（最长间隔 %llu ms），恢复后回到声卡 | pacing unbroken over a 300 ms suspend (gap %llu ms), back on the sound card", (unsigned long long)p.maxgap_ms, (unsigned long long)p.maxgap_ms);
        h_au_Close(o);
    }
    {   // 7. 桥接 / 重开中途 Close（不挂、不崩，流全关）| Close in the middle of a bridge / reopen (no hang, no crash, every stream closed)
        int ok = 1;
        for (int k = 0; k < 6; k++) {
            atomic_store(&g_aa_safe, 0); atomic_store(&mock_stall_ll_ms, 60);
            char o[64] = {0}; h_au_Open(o, ev, NULL, prm); h_au_Start(o);
            usleep((useconds_t)(250000 + 40000 * k));
            uint64_t t = now_ns(); h_au_Close(o); ok &= now_ns() - t < 1500000000ull;
        }
        atomic_store(&mock_stall_ll_ms, 0);
        CHECK(ok && atomic_load(&mock_live) == 0, "桥接中途 Close 6 次：都在 1.5 s 内返回，残留流 %d 个 | 6 Closes mid-bridge returned in time, %d streams left", atomic_load(&mock_live), atomic_load(&mock_live));
    }
    printf(fails ? "失败 %d 项 | %d FAILED\n" : "全部通过 | all passed\n", fails, fails);
    return fails != 0;
}
