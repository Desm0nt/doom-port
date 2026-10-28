// Android performance plumbing for the presents: CPU performance hints (ADPF), thermal status, and the display refresh-rate vote.
// - ADPF (APerformanceHint, Android 13; resolved with dlsym so older systems still load): the frame's work is reported against the game's
//   present period (dm_present_period, the pacing tier hle_nvn.c picked), so the CPU governor raises clocks before a frame runs late instead
//   of after. The work of a frame is the largest CPU time any thread of the session spent between two presents (per-thread CPU clocks): a wall
//   interval would include the pacer's idle wait and ask for clocks the frame does not need (the same reasoning as hle_nvn.c pace_adapt_tick).
//   Every 300 presents the threads that used at least 15% of a core join the session (up to 5 besides the presenting one; Android 14
//   APerformanceHint_setThreads), the idle ones leave. Android 15 also gets the frame's GPU time (reportActualWorkDuration2) when the
//   Vulkan backend measures it (xbg_recent_gpu_ms).
// - Thermal (AThermal, Android 11/12): the status is followed with a listener, logged with the headroom, and returned by dm_thermal_now()
//   (hle_nvn.c's perf summary). At SEVERE or worse the window's frame-rate vote drops to the lowest rate that is a multiple of the game's
//   rate and >= 60 Hz (a 120 Hz panel costs power the 30 / 60 fps game does not use); it returns to the marker vote at MODERATE or better.
// - Display rate: the vote vulkan_present.cpp makes at surface creation (marker gfx-frame-rate, default 120 Hz) is re-applied here whenever the
//   window or the wanted rate changes, with ANativeWindow_setFrameRateWithChangeStrategy(ONLY_IF_SEAMLESS) where available (Android 12), so a
//   thermal step never blanks the screen for a mode switch.
// Switches (env file): DM_NO_ADPF=1 no hint session; DM_THERMAL_VOTE=0 keep the marker vote whatever the temperature.
// Hook: dm_aperf_present(window, frame_rate_vote) once per present on the presenting thread (vulkan_present.cpp present(), before
// vkQueuePresentKHR); dm_aperf_start() once at boot (jni_android.c).
// Portions ported from nx2android (https://github.com/seedds/nx2android) nxnative/dm/android/perf_android.c (ADPF session, thread scan),
// Copyright (c) 2026 seedds, MIT License.
#include "../host/dm.h"
#include "touch_android.h"
#include <android/native_window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double dm_present_period(void);   // gfx/vulkan/nvn_vk.c: this present's pacing period (s)
__attribute__((weak)) double xbg_recent_gpu_ms(void);

// ---------------- ADPF ----------------
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AWorkDuration AWorkDuration;
static struct {
    APerformanceHintManager *(*get)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*update)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);   // Android 14
    AWorkDuration *(*wd_create)(void);                                        // Android 15
    void (*wd_start)(AWorkDuration *, int64_t), (*wd_total)(AWorkDuration *, int64_t), (*wd_cpu)(AWorkDuration *, int64_t), (*wd_gpu)(AWorkDuration *, int64_t);
    int (*report2)(APerformanceHintSession *, AWorkDuration *);
} H;
static APerformanceHintSession *g_hint; static int64_t g_hint_target; static int32_t g_hint_tids[8]; static int g_hint_n;
static AWorkDuration *g_wd;

static int64_t perf_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
// CPU time of any thread of this process: the kernel's per-thread CPU clock id (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
static int64_t thread_cpu_ns(int32_t tid) {
    clockid_t c = (clockid_t)(((uint32_t)~(uint32_t)tid << 3) | 6u); struct timespec ts;
    if (clock_gettime(c, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void hint_init(int64_t target) {
    static int tried; if (tried) return; tried = 1;
    if (getenv("DM_NO_ADPF")) { LOG("[perf] DM_NO_ADPF: CPU hints off"); return; }
    void *lib = dlopen("libandroid.so", RTLD_NOW); if (!lib) return;
    H.get = dlsym(lib, "APerformanceHint_getManager"); H.create = dlsym(lib, "APerformanceHint_createSession");
    H.update = dlsym(lib, "APerformanceHint_updateTargetWorkDuration"); H.report = dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    H.set_threads = dlsym(lib, "APerformanceHint_setThreads");
    H.wd_create = dlsym(lib, "AWorkDuration_create"); H.wd_start = dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
    H.wd_total = dlsym(lib, "AWorkDuration_setActualTotalDurationNanos"); H.wd_cpu = dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
    H.wd_gpu = dlsym(lib, "AWorkDuration_setActualGpuDurationNanos"); H.report2 = dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
    if (!H.get || !H.create || !H.report) { LOG("[perf] no APerformanceHint on this Android: CPU hints off"); return; }
    APerformanceHintManager *m = H.get(); if (!m) return;
    g_hint_tids[0] = (int32_t)gettid(); g_hint_n = 1;
    g_hint = H.create(m, g_hint_tids, (size_t)g_hint_n, target); g_hint_target = target;
    if (g_hint && H.wd_create && H.wd_start && H.wd_total && H.wd_cpu && H.wd_gpu && H.report2) g_wd = H.wd_create();
    LOG("[perf] ADPF session %s (target %.2f ms, thread %d%s%s)", g_hint ? "open" : "refused", target / 1e6, g_hint_tids[0],
        H.set_threads ? ", threads join by load" : "", g_wd ? ", GPU time reported" : "");
}
// The other threads that do frame work: the presenting thread is the Vulkan backend's, the game's main thread (simulation) and its job workers
// set the pace as much. Every 300 presents the threads that used at least 15% of a core since the last look join the session (up to 5 besides
// the presenting one), the ones that went idle leave it (nx2android: without them the main thread stayed on mid cores and its bursts made long frames).
static uint64_t task_ticks(int tid) {
    char p[64], b[512]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *c = strrchr(b, ')'); if (!c) return 0;
    unsigned long long ut = 0, st = 0; int field = 2;
    for (char *t = c + 1; *t && field < 15; t++) if (*t == ' ') { field++; if (field == 14) ut = strtoull(t + 1, NULL, 10); else if (field == 15) st = strtoull(t + 1, NULL, 10); }
    return ut + st;
}
static void hint_threads_scan(int32_t self) {
    static struct { int tid; uint64_t ticks; } prev[512]; static int np; static int64_t last;
    int64_t now = perf_now(); double secs = last ? (now - last) / 1e9 : 0; last = now;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    static struct { int tid; uint64_t used; } cur[512]; int nc = 0; struct dirent *e;
    while ((e = readdir(d)) && nc < 512) { int tid = atoi(e->d_name); if (tid <= 0) continue; cur[nc].tid = tid; cur[nc].used = task_ticks(tid); nc++; }
    closedir(d);
    int32_t pick[8]; int npick = 0; pick[npick++] = self;
    if (secs > 0) {
        long hz = sysconf(_SC_CLK_TCK); double need = 0.15 * secs * (double)(hz > 0 ? hz : 100);
        for (int k = 0; k < 5; k++) {   // the busiest first
            int best = -1; uint64_t bu = 0;
            for (int i = 0; i < nc; i++) {
                if (cur[i].tid == self) continue;
                int taken = 0; for (int j = 0; j < npick; j++) taken |= pick[j] == cur[i].tid;
                if (taken) continue;
                uint64_t was = 0; for (int j = 0; j < np; j++) if (prev[j].tid == cur[i].tid) { was = prev[j].ticks; break; }
                uint64_t u = cur[i].used > was ? cur[i].used - was : 0;
                if (was && u >= need && u > bu) { bu = u; best = i; }
            }
            if (best < 0) break;
            pick[npick++] = cur[best].tid;
        }
    }
    np = 0; for (int i = 0; i < nc; i++) { prev[np].tid = cur[i].tid; prev[np].ticks = cur[i].used; np++; }
    if (!g_hint || !H.set_threads) return;
    int same = npick == g_hint_n;
    for (int i = 0; same && i < npick; i++) { int in = 0; for (int j = 0; j < g_hint_n; j++) in |= g_hint_tids[j] == pick[i]; same = in; }
    if (same) return;
    if (H.set_threads(g_hint, pick, (size_t)npick)) { LOG("[perf] ADPF: the session refused threads"); return; }
    memcpy(g_hint_tids, pick, sizeof(int32_t) * (size_t)npick); g_hint_n = npick;
    char l[160]; size_t o = 0; for (int i = 0; i < npick && o < sizeof l; i++) o += (size_t)snprintf(l + o, sizeof l - o, " %d", pick[i]);
    LOG("[perf] ADPF session threads:%s", l);
}
// The session threads' CPU time since the previous present: the largest one is the frame's work (the critical path's CPU share).
static int64_t g_tid_cpu[8]; static int32_t g_tid_seen[8];
static int64_t frame_cpu_work(void) {
    int64_t worst = 0;
    for (int i = 0; i < g_hint_n; i++) {
        int64_t c = thread_cpu_ns(g_hint_tids[i]);
        if (c < 0) { g_tid_seen[i] = 0; continue; }   // the thread ended: the next scan drops it
        if (g_tid_seen[i] == g_hint_tids[i] && c > g_tid_cpu[i] && c - g_tid_cpu[i] > worst) worst = c - g_tid_cpu[i];
        g_tid_cpu[i] = c; g_tid_seen[i] = g_hint_tids[i];
    }
    return worst;
}

// ---------------- thermal ----------------
typedef struct AThermalManager AThermalManager;
static struct {
    AThermalManager *(*acquire)(void);
    int (*status)(AThermalManager *);
    int (*listen)(AThermalManager *, void (*)(void *, int), void *);
    float (*headroom)(AThermalManager *, int);
} T;
static AThermalManager *g_thermal; static _Atomic int g_tstatus = -1;
enum { TH_NONE, TH_LIGHT, TH_MODERATE, TH_SEVERE, TH_CRITICAL, TH_EMERGENCY, TH_SHUTDOWN };
static const char *th_name(int s) { static const char *n[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"}; return s >= 0 && s <= 6 ? n[s] : "unknown"; }
static void thermal_changed(void *data, int status) {   // a binder thread of the system's
    (void)data;
    int old = atomic_exchange(&g_tstatus, status);
    if (old != status) {
        float h = T.headroom && g_thermal ? T.headroom(g_thermal, 10) : NAN;   // forecast 10 s ahead; NaN when asked too often
        LOG("[perf] thermal status %s -> %s (headroom in 10 s %.2f)", th_name(old), th_name(status), (double)h);
    }
}
int dm_thermal_now(void) { return atomic_load(&g_tstatus); }   // strong over hle_nvn.c's weak default (-1): the perf summary's thermal column
void dm_aperf_start(void) {
    static int done; if (done) return; done = 1;
    void *lib = dlopen("libandroid.so", RTLD_NOW); if (!lib) return;
    T.acquire = dlsym(lib, "AThermal_acquireManager"); T.status = dlsym(lib, "AThermal_getCurrentThermalStatus");
    T.listen = dlsym(lib, "AThermal_registerThermalStatusListener"); T.headroom = dlsym(lib, "AThermal_getThermalHeadroom");
    if (!T.acquire || !T.status) { LOG("[perf] no AThermal on this Android: thermal status unknown"); return; }
    g_thermal = T.acquire(); if (!g_thermal) return;
    atomic_store(&g_tstatus, T.status(g_thermal));
    if (T.listen && T.listen(g_thermal, thermal_changed, NULL)) LOG("[perf] thermal listener refused: status read at start only");
    LOG("[perf] thermal status at start: %s", th_name(atomic_load(&g_tstatus)));
}

// ---------------- display rate vote ----------------
static int32_t (*g_set_rate)(ANativeWindow *, float, int8_t);
static int32_t (*g_set_rate2)(ANativeWindow *, float, int8_t, int8_t);   // Android 12: with a change strategy
static float vote_for(float marker, double period) {
    if (marker <= 0) return 0;   // marker gfx-frame-rate = 0: no vote at all
    static int thermal_vote = -1; if (thermal_vote < 0) { const char *e = getenv("DM_THERMAL_VOTE"); thermal_vote = !(e && !atoi(e)); }
    static int hot; int s = atomic_load(&g_tstatus);
    if (s >= TH_SEVERE) hot = 1; else if (s >= 0 && s <= TH_MODERATE) hot = 0;
    if (!thermal_vote || !hot) return marker;
    double fps = period > 0 ? 1 / period : 60; if (fps < 1) fps = 60;
    double v = fps; while (v < 59.5) v += fps;   // the lowest multiple of the game's rate that is at least 60 Hz
    return v < marker ? (float)v : marker;
}
static void vote(ANativeWindow *w, float hz) {
    static int resolved; if (!resolved) { resolved = 1; void *lib = dlopen("libnativewindow.so", RTLD_NOW);
        if (lib) { g_set_rate = dlsym(lib, "ANativeWindow_setFrameRate"); g_set_rate2 = dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy"); } }
    int r = g_set_rate2 ? g_set_rate2(w, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT, 0 /* ONLY_IF_SEAMLESS */)
          : g_set_rate ? g_set_rate(w, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT) : -1;
    LOG("[perf] window frame-rate vote %.0f Hz%s (thermal %s)", (double)hz, r ? " refused" : "", th_name(atomic_load(&g_tstatus)));
}

// ---------------- per present ----------------
// Performance hints are skipped when DM_ADPF=0 or when the process runs under an ARM translation layer (LDPlayer /
// emulators: libhoudini / libndk_translation bridge these platform calls and crash in them); real devices keep them.
static int perf_off(void) {
    static int off = -1;
    if (off >= 0) return off;
    const char *e = getenv("DM_ADPF"); off = e && !strcmp(e, "0");
    if (!off) {
        FILE *f = fopen("/proc/self/maps", "r"); char line[512];
        if (f) { while (fgets(line, sizeof line, f)) if (strstr(line, "libhoudini") || strstr(line, "libndk_translation")) { off = 1; break; } fclose(f); }
    }
    if (off) LOG("[perf] performance hints off (%s)", e && !strcmp(e, "0") ? "DM_ADPF=0" : "ARM translation layer detected");
    return off;
}
void dm_aperf_present(ANativeWindow *w, float marker_vote) {
    if (perf_off()) return;
    double period = dm_present_period();
    int64_t p = period > 0 && period < 1 ? (int64_t)(period * 1e9) : 16666667, t = perf_now();
    hint_init(p);
    static unsigned frames; static int64_t prev_t;
    if (frames++ % 300 == 0) hint_threads_scan((int32_t)gettid());
    if (g_hint) {
        if (!g_hint_n) { g_hint_tids[0] = (int32_t)gettid(); g_hint_n = 1; }
        int64_t work = frame_cpu_work(), iv = prev_t ? t - prev_t : 0;
        if (work > iv && iv > 0) work = iv;
        if (work > 0 && iv > 0 && iv < 250000000) {   // (a longer span: the game was held, by a load, the pause menu or the background)
            double gms = xbg_recent_gpu_ms ? xbg_recent_gpu_ms() : -1;
            if (g_wd && gms > 0) {
                int64_t g = (int64_t)(gms * 1e6);
                H.wd_start(g_wd, prev_t); H.wd_total(g_wd, work > g ? work : g); H.wd_cpu(g_wd, work); H.wd_gpu(g_wd, g);
                if (H.report2(g_hint, g_wd)) H.report(g_hint, work);
            } else H.report(g_hint, work);
            static int64_t wsum, wmax; static unsigned wn; wsum += work; if (work > wmax) wmax = work;
            if (++wn == 600) { LOG("[perf] ADPF: frame CPU work mean %.1f / max %.1f ms against %.1f ms, thermal %s", wsum / 600 / 1e6, wmax / 1e6, p / 1e6, th_name(atomic_load(&g_tstatus))); wsum = wmax = 0; wn = 0; }
        }
        if (p != g_hint_target && H.update) { H.update(g_hint, p); g_hint_target = p; LOG("[perf] ADPF target %.2f ms", p / 1e6); }
    }
    prev_t = t;
    static ANativeWindow *voted_w; static float voted;
    float want = vote_for(marker_vote, period);
    if (w && want > 0 && (w != voted_w || fabsf(want - voted) > 0.5f)) { vote(w, want); voted_w = w; voted = want; }
}
