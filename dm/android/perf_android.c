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
// - [r2] Thread / clock monitor (always on, thread "dm-perfmon", wakes once a second): every 10 s "[perf] threads 10s:" lists the 6 threads
//   of the process that used the most CPU (tid:name % of a core, the CPU it last ran on; * = guest thread, B = DM_GUEST_BIGCORE thread) and
//   "[perf] clocks 10s:" the cpufreq policies (current / average / range of 1 s samples / scaling_max_freq, MHz) with the kgsl GPU clock and
//   busy % where readable. The guest threads get host names (hle_thread.c thr_host_name) so these lines and top can tell them apart.
// - [r2] DM_GUEST_BIGCORE=<hex mask>: the busiest guest thread (the game's main thread) is kept on that host CPU mask (sched_setaffinity,
//   re-checked every 2 s because HyperOS rewrites masks) and always belongs to the ADPF session.
// Switches (env file): DM_ADPF=0 or DM_NO_ADPF=1 (one meaning since r2: no hint session; the thermal frame-rate vote stays);
// DM_THERMAL_VOTE=0 keep the marker vote whatever the temperature; [r2] DM_ADPF_NOCLIP=1, DM_ADPF_WORK=wall, DM_ADPF_TARGET_MS=<ms>,
// DM_ADPF_HYST=1, DM_ADPF_GPU=0, DM_GUEST_BIGCORE=<hex>, DM_GUEST_BIGCORE_PICK=present (docs/SWITCHES.md).
// Hook: dm_aperf_present(window, frame_rate_vote) once per present on the presenting thread (vulkan_present.cpp present(), before
// vkQueuePresentKHR); dm_aperf_note_wait(ns) from the same present (its acquire wait); dm_aperf_guest_present() from nvnQueuePresentTexture
// (the guest thread that presents); dm_aperf_start() once at boot (jni_android.c).
// Portions ported from nx2android (https://github.com/seedds/nx2android) nxnative/dm/android/perf_android.c (ADPF session, thread scan),
// Copyright (c) 2026 seedds, MIT License.
#include "../host/dm.h"
#include "touch_android.h"
#include <android/native_window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

double dm_present_period(void);   // gfx/vulkan/nvn_vk.c: this present's pacing period (s)
__attribute__((weak)) double xbg_recent_gpu_ms(void);
__attribute__((weak)) uint64_t dm_nvn_pace_sleep_ns(void);         // gfx/nvn/hle_nvn.c: total pacing sleep inside AcquireTexture
__attribute__((weak)) int dm_nvn_present_menu_capped(void);         // gfx/nvn/hle_nvn.c [r6b]: 1 = presents capped at DM_PRESENT_MENU_HZ (menu / loading)
__attribute__((weak)) int thr_guest_tids(int32_t *out, int max);    // host/hle_thread.c: live guest threads' host tids
__attribute__((weak)) void thr_refresh_host_names(void);           // host/hle_thread.c: re-read the guest names into the host names

static int64_t perf_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }

// ---------------- [r2] switches ----------------
static struct {
    int hints_off;          // DM_ADPF=0 / DM_NO_ADPF (set, not "0"): no hint session
    int noclip, wall, hyst, gpu;
    int64_t target;         // DM_ADPF_TARGET_MS (ns; 0 = the present period)
    uint32_t big_mask;      // DM_GUEST_BIGCORE (0 = off)
    int big_present;        // DM_GUEST_BIGCORE_PICK=present: the guest thread that presents, not the busiest one
    char tag[96];           // the non-default ADPF modes, appended to the ADPF lines
} C;
static void cfg_read(void) {
    const char *e = getenv("DM_ADPF"), *n = getenv("DM_NO_ADPF");
    C.hints_off = (e && !strcmp(e, "0")) || (n && *n && strcmp(n, "0"));
    e = getenv("DM_ADPF_NOCLIP"); C.noclip = e && atoi(e) > 0;
    e = getenv("DM_ADPF_WORK"); C.wall = e && !strcmp(e, "wall");
    e = getenv("DM_ADPF_HYST"); C.hyst = e && atoi(e) > 0;
    e = getenv("DM_ADPF_GPU"); C.gpu = !(e && *e && !atoi(e));
    e = getenv("DM_ADPF_TARGET_MS"); { double ms = e ? atof(e) : 0; C.target = ms > 0.5 && ms < 250 ? (int64_t)(ms * 1e6) : 0; }
    e = getenv("DM_GUEST_BIGCORE"); C.big_mask = e && *e ? (uint32_t)strtoul(e, NULL, 16) : 0;
    e = getenv("DM_GUEST_BIGCORE_PICK"); C.big_present = e && !strcmp(e, "present");
    size_t o = 0; C.tag[0] = 0;
    if (C.wall) o += (size_t)snprintf(C.tag + o, sizeof C.tag - o, " work=wall");
    if (C.noclip && !C.wall) o += (size_t)snprintf(C.tag + o, sizeof C.tag - o, " noclip");
    if (C.target) o += (size_t)snprintf(C.tag + o, sizeof C.tag - o, " target=%.2fms", C.target / 1e6);
    if (C.hyst) o += (size_t)snprintf(C.tag + o, sizeof C.tag - o, " hyst");
    if (!C.gpu) o += (size_t)snprintf(C.tag + o, sizeof C.tag - o, " gpu=0");
    if (C.big_mask && o < sizeof C.tag) snprintf(C.tag + o, sizeof C.tag - o, " bigcore=%x", C.big_mask);
}
static pthread_once_t g_cfg_once = PTHREAD_ONCE_INIT;
static void cfg(void) { pthread_once(&g_cfg_once, cfg_read); }

// Shared between the monitor thread and the presenting thread
static _Atomic int g_guest_present_tid;   // the guest thread that called nvnQueuePresentTexture last
static _Atomic int g_big_tid;             // the DM_GUEST_BIGCORE thread (0 = none yet)
static pthread_mutex_t g_mon_mu = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_guest_set[256]; static int g_guest_n;   // guest tids at the monitor's last scan (under g_mon_mu)
void dm_aperf_guest_present(void) {
    int t = (int)gettid();
    if (atomic_load_explicit(&g_guest_present_tid, memory_order_relaxed) != t) atomic_store_explicit(&g_guest_present_tid, t, memory_order_relaxed);
}
static int64_t g_present_wait;   // presenting thread only: acquire waits since its last dm_aperf_present
void dm_aperf_note_wait(int64_t ns) { if (ns > 0) g_present_wait += ns; }

// /proc/self/task/<tid>/stat: comm, utime + stime (clock ticks), the CPU it last ran on (field 39)
typedef struct { uint64_t ticks; int cpu; char comm[16]; } tstat_t;
static int task_stat(int tid, tstat_t *o) {
    char p[64], b[1024]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    o->ticks = 0; o->cpu = -1; o->comm[0] = 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1;
    b[n] = 0;
    char *l = strchr(b, '('), *r = strrchr(b, ')'); if (!l || !r || r < l) return -1;
    size_t cl = (size_t)(r - l - 1); if (cl > 15) cl = 15;
    memcpy(o->comm, l + 1, cl); o->comm[cl] = 0;
    unsigned long long ut = 0, st = 0; int field = 2;
    for (char *t = r + 1; *t; t++) if (*t == ' ') { field++;
        if (field == 14) ut = strtoull(t + 1, NULL, 10); else if (field == 15) st = strtoull(t + 1, NULL, 10);
        else if (field == 39) { o->cpu = atoi(t + 1); break; } }
    o->ticks = ut + st; return 0;
}

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
static uint8_t g_hint_low[8];   // DM_ADPF_HYST: consecutive scans each member spent below 5% of a core
static AWorkDuration *g_wd;

// CPU time of any thread of this process: the kernel's per-thread CPU clock id (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
static int64_t thread_cpu_ns(int32_t tid) {
    clockid_t c = (clockid_t)(((uint32_t)~(uint32_t)tid << 3) | 6u); struct timespec ts;
    if (clock_gettime(c, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void hint_init(int64_t target) {
    static int tried; if (tried) return; tried = 1;
    if (C.hints_off) { LOG("[perf] CPU hints off (DM_ADPF=0 / DM_NO_ADPF): no ADPF session"); return; }
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
    LOG("[perf] ADPF session %s (target %.2f ms, thread %d%s%s%s)", g_hint ? "open" : "refused", target / 1e6, g_hint_tids[0],
        H.set_threads ? ", threads join by load" : "", g_wd ? (C.gpu ? ", GPU time reported" : ", GPU time not reported (DM_ADPF_GPU=0)") : "", C.tag);
}
// DM_ADPF_HYST: who may join by load. Guest threads (the monitor's list) and our own named host threads; not the UI thread
// (tid = pid), the Java / HWUI / binder threads or the vendor's (libMiGL), whose load is not the frame's.
static int hyst_allowed(int tid, const char *comm, const int32_t *guests, int ng) {
    if (tid == (int)getpid()) return 0;
    if (!strncmp(comm, "dm-", 3) || !strncmp(comm, "nvn-", 4) || !strncmp(comm, "xbg-", 4) || !strcmp(comm, "guest-main")) return 1;
    for (int i = 0; i < ng; i++) if (guests[i] == tid) return 1;
    return 0;
}
// The other threads that do frame work: the presenting thread is the Vulkan backend's, the game's main thread (simulation) and its job workers
// set the pace as much. Every 300 presents the threads that used at least 15% of a core since the last look join the session (up to 5 besides
// the presenting one), the ones that went idle leave it (nx2android: without them the main thread stayed on mid cores and its bursts made long frames).
// [r2] A DM_GUEST_BIGCORE thread is always a member. DM_ADPF_HYST=1: join at >= 15% of a core, leave only after two scans in a row
// below 5%; the presenting thread, the bigcore thread and the guest thread that presents are never dropped; see hyst_allowed.
static void hint_threads_scan(int32_t self) {
    static struct { int tid; uint64_t ticks; } prev[512]; static int np; static int64_t last;
    int64_t now = perf_now(); double secs = last ? (now - last) / 1e9 : 0; last = now;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    static struct { int tid; uint64_t used, u; int was, ok; char comm[16]; } cur[512]; int nc = 0; struct dirent *e;
    while ((e = readdir(d)) && nc < 512) { int tid = atoi(e->d_name); if (tid <= 0) continue;
        tstat_t ts; task_stat(tid, &ts); cur[nc].tid = tid; cur[nc].used = ts.ticks; memcpy(cur[nc].comm, ts.comm, sizeof ts.comm); nc++; }
    closedir(d);
    static int32_t guests[256]; int ng = 0;
    if (C.hyst) { pthread_mutex_lock(&g_mon_mu); ng = g_guest_n; memcpy(guests, g_guest_set, sizeof(int32_t) * (size_t)ng); pthread_mutex_unlock(&g_mon_mu); }
    for (int i = 0; i < nc; i++) {
        uint64_t was = 0; for (int j = 0; j < np; j++) if (prev[j].tid == cur[i].tid) { was = prev[j].ticks; break; }
        cur[i].was = was != 0; cur[i].u = cur[i].used > was ? cur[i].used - was : 0;
        cur[i].ok = !C.hyst || hyst_allowed(cur[i].tid, cur[i].comm, guests, ng);
    }
    long hz = sysconf(_SC_CLK_TCK); double tick_s = secs * (double)(hz > 0 ? hz : 100), need = 0.15 * tick_s, low = 0.05 * tick_s;
    int big = C.big_mask ? atomic_load(&g_big_tid) : 0, gp = atomic_load(&g_guest_present_tid);
    int32_t pick[8]; uint8_t plow[8] = {0}; int npick = 0; pick[npick++] = self;
    int big_alive = 0; for (int i = 0; i < nc; i++) big_alive |= big && cur[i].tid == big;
    if (big_alive && big != self) pick[npick++] = big;
    if (secs > 0 && C.hyst) {
        for (int m = 0; m < g_hint_n && npick < 6; m++) {   // members stay unless idle on two scans in a row
            int tid = g_hint_tids[m], k = -1; if (tid == self || tid == big) continue;
            for (int i = 0; i < nc; i++) if (cur[i].tid == tid) { k = i; break; }
            if (k < 0) continue;   // the thread ended
            int l = cur[k].u < low ? g_hint_low[m] + 1 : 0;
            if (l >= 2 && tid != gp) continue;
            plow[npick] = (uint8_t)(l > 2 ? 2 : l); pick[npick++] = tid;
        }
    }
    if (secs > 0) {
        while (npick < 6) {   // the busiest first
            int best = -1; uint64_t bu = 0;
            for (int i = 0; i < nc; i++) {
                if (cur[i].tid == self) continue;
                int taken = 0; for (int j = 0; j < npick; j++) taken |= pick[j] == cur[i].tid;
                if (taken) continue;
                if (!cur[i].ok) continue;
                if (cur[i].was && cur[i].u >= need && cur[i].u > bu) { bu = cur[i].u; best = i; }
            }
            if (best < 0) break;
            pick[npick++] = cur[best].tid;
        }
    }
    np = 0; for (int i = 0; i < nc; i++) { prev[np].tid = cur[i].tid; prev[np].ticks = cur[i].used; np++; }
    if (!g_hint || !H.set_threads) return;
    int same = npick == g_hint_n;
    for (int i = 0; same && i < npick; i++) { int in = 0; for (int j = 0; j < g_hint_n; j++) in |= g_hint_tids[j] == pick[i]; same = in; }
    if (same) { for (int i = 0; i < npick; i++) for (int j = 0; j < g_hint_n; j++) if (g_hint_tids[j] == pick[i]) g_hint_low[j] = plow[i]; return; }
    if (H.set_threads(g_hint, pick, (size_t)npick)) { LOG("[perf] ADPF: the session refused threads"); return; }
    memcpy(g_hint_tids, pick, sizeof(int32_t) * (size_t)npick); memcpy(g_hint_low, plow, (size_t)npick); g_hint_n = npick;
    char l[200]; size_t o = 0; for (int i = 0; i < npick && o < sizeof l; i++) o += (size_t)snprintf(l + o, sizeof l - o, " %d", pick[i]);
    LOG("[perf] ADPF session threads:%s%s", l, big_alive ? " (bigcore member)" : "");
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

// ---------------- [r2] thread / clock monitor ----------------
// Sysfs values that may be missing or denied (SELinux: kgsl for untrusted apps): a path that failed once is not tried again, so a
// denial is logged by the kernel once, not every second.
typedef struct { char path[96]; int dead; } sysval_t;
static long long sys_read(sysval_t *v) {
    if (v->dead || !v->path[0]) return -1;
    int fd = open(v->path, O_RDONLY | O_CLOEXEC); if (fd < 0) { v->dead = 1; return -1; }
    char b[32]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) { v->dead = 1; return -1; }
    b[n] = 0; return strtoll(b, NULL, 10);
}
#define MAXPOL 8
static struct { int id; sysval_t cur, max; long long sum, lo, hi, last; int n; } g_pol[MAXPOL]; static int g_npol;
static struct { sysval_t clk, dcur, dmax, busy; long long sum, last, bsum; int n, bn; } g_gpu;
static void clocks_init(void) {
    for (int i = 0; i < 16 && g_npol < MAXPOL; i++) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", i);
        if (access(p, R_OK)) continue;
        g_pol[g_npol].id = i; snprintf(g_pol[g_npol].cur.path, sizeof g_pol[0].cur.path, "%s", p);
        snprintf(g_pol[g_npol].max.path, sizeof g_pol[0].max.path, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", i);
        g_pol[g_npol].lo = -1; g_npol++;
    }
    snprintf(g_gpu.clk.path, sizeof g_gpu.clk.path, "/sys/class/kgsl/kgsl-3d0/gpuclk");
    snprintf(g_gpu.dcur.path, sizeof g_gpu.dcur.path, "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq");
    snprintf(g_gpu.dmax.path, sizeof g_gpu.dmax.path, "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq");
    snprintf(g_gpu.busy.path, sizeof g_gpu.busy.path, "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage");
}
static void clocks_sample(void) {   // once a second
    for (int i = 0; i < g_npol; i++) { long long v = sys_read(&g_pol[i].cur); if (v <= 0) continue;
        g_pol[i].last = v; g_pol[i].sum += v; g_pol[i].n++; if (g_pol[i].lo < 0 || v < g_pol[i].lo) g_pol[i].lo = v; if (v > g_pol[i].hi) g_pol[i].hi = v; }
    long long c = sys_read(&g_gpu.clk); if (c <= 0) c = sys_read(&g_gpu.dcur);
    if (c > 0) { g_gpu.last = c; g_gpu.sum += c; g_gpu.n++; }
    long long b = sys_read(&g_gpu.busy); if (b >= 0) { g_gpu.bsum += b; g_gpu.bn++; }
}
static void clocks_log(void) {
    char l[512]; size_t o = (size_t)snprintf(l, sizeof l, "[perf] clocks 10s (MHz cur/avg [min-max] cap):");
    for (int i = 0; i < g_npol && o < sizeof l; i++) {
        long long mx = sys_read(&g_pol[i].max);
        if (!g_pol[i].n) o += (size_t)snprintf(l + o, sizeof l - o, " p%d n/a", g_pol[i].id);
        else o += (size_t)snprintf(l + o, sizeof l - o, " p%d %lld/%lld [%lld-%lld] %lld", g_pol[i].id, g_pol[i].last / 1000, g_pol[i].sum / g_pol[i].n / 1000,
                                   g_pol[i].lo / 1000, g_pol[i].hi / 1000, mx > 0 ? mx / 1000 : -1);
        g_pol[i].sum = g_pol[i].n = 0; g_pol[i].lo = -1; g_pol[i].hi = 0;
    }
    if (o < sizeof l) {
        long long dm = sys_read(&g_gpu.dmax);
        if (g_gpu.n) o += (size_t)snprintf(l + o, sizeof l - o, "; gpu %lld/%lld cap %lld", g_gpu.last / 1000000, g_gpu.sum / g_gpu.n / 1000000, dm > 0 ? dm / 1000000 : -1);
        else o += (size_t)snprintf(l + o, sizeof l - o, "; gpu n/a");
        if (o < sizeof l && g_gpu.bn) o += (size_t)snprintf(l + o, sizeof l - o, " busy %lld%%", g_gpu.bsum / g_gpu.bn);
        if (o < sizeof l) snprintf(l + o, sizeof l - o, "; thermal %s", th_name(atomic_load(&g_tstatus)));
    }
    g_gpu.sum = g_gpu.n = 0; g_gpu.bsum = g_gpu.bn = 0;
    LOG("%s", l);
}
// DM_GUEST_BIGCORE: pin / re-pin / release. Raw sched_{set,get}affinity on another thread's tid (bionic hides cpu_set_t behind
// _GNU_SOURCE, which the force-included compat header has already decided against).
typedef struct { unsigned long b[4]; } cpumask_t;   // 256 CPUs
static int aff_set(int tid, const cpumask_t *m) { return syscall(__NR_sched_setaffinity, tid, sizeof *m, m) < 0 ? -1 : 0; }
static int aff_get(int tid, cpumask_t *m) { memset(m, 0, sizeof *m); return syscall(__NR_sched_getaffinity, tid, sizeof *m, m) < 0 ? -1 : 0; }
static cpumask_t g_big_set, g_all_set; static int g_big_applied; static unsigned g_big_resets, g_big_resets_w, g_big_fail;
static void big_check(void) {
    int want = atomic_load(&g_big_tid);
    if (g_big_applied && g_big_applied != want) {   // the pick moved: give the old thread every CPU back
        int r = aff_set(g_big_applied, &g_all_set);
        LOG("[perf] bigcore: tid %d released (result %d)", g_big_applied, r ? errno : 0); g_big_applied = 0;
    }
    if (!want) return;
    if (want != g_big_applied) {
        int r = aff_set(want, &g_big_set), err = r ? errno : 0; if (r) ++g_big_fail;
        tstat_t ts; task_stat(want, &ts);
        LOG("[perf] bigcore: tid %d (%s) pinned to mask %x result %d (%s)", want, ts.comm, C.big_mask, err, C.big_present ? "the guest thread that presents" : "the busiest guest thread");
        g_big_applied = want; return;
    }
    cpumask_t cur;
    if (aff_get(want, &cur)) { if (errno == ESRCH) { g_big_applied = 0; atomic_compare_exchange_strong(&g_big_tid, &want, 0); } return; }
    if (!memcmp(&cur, &g_big_set, sizeof cur)) return;
    ++g_big_resets; ++g_big_resets_w;   // HyperOS (or the cpuset) rewrote the mask
    int r = aff_set(want, &g_big_set), err = r ? errno : 0; if (r) ++g_big_fail;
    if (g_big_resets <= 4 || (r && g_big_fail <= 4)) LOG("[perf] bigcore: tid %d mask found reset, re-pinned (result %d, resets %u)", want, err, g_big_resets);
}
static void monitor_scan(int64_t now) {
    static struct { int tid; uint64_t ticks; } prev[512]; static int np; static int64_t last; static int scans;
    static struct { int tid; uint64_t used, u; int cpu; char comm[16]; } cur[512]; int nc = 0;
    double secs = last ? (now - last) / 1e9 : 0; last = now;
    int32_t gt[256]; int ng = thr_guest_tids ? thr_guest_tids(gt, 256) : 0;
    pthread_mutex_lock(&g_mon_mu); memcpy(g_guest_set, gt, sizeof(int32_t) * (size_t)ng); g_guest_n = ng; pthread_mutex_unlock(&g_mon_mu);
    if (thr_refresh_host_names) thr_refresh_host_names();
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nc < 512) { int tid = atoi(e->d_name); if (tid <= 0) continue;
        tstat_t ts; if (task_stat(tid, &ts)) continue;
        cur[nc].tid = tid; cur[nc].used = ts.ticks; cur[nc].cpu = ts.cpu; memcpy(cur[nc].comm, ts.comm, sizeof ts.comm); nc++; }
    closedir(d);
    uint64_t total = 0;
    for (int i = 0; i < nc; i++) {
        int found = 0; uint64_t was = 0; for (int j = 0; j < np; j++) if (prev[j].tid == cur[i].tid) { was = prev[j].ticks; found = 1; break; }
        cur[i].u = found ? (cur[i].used > was ? cur[i].used - was : 0) : (scans ? cur[i].used : 0);   // a thread born in the window: all its time is in it
        total += cur[i].u;
    }
    np = 0; for (int i = 0; i < nc; i++) { prev[np].tid = cur[i].tid; prev[np].ticks = cur[i].used; np++; }
    int first = !scans++;
    long hz = sysconf(_SC_CLK_TCK); double per = secs > 0 ? 100.0 / (secs * (double)(hz > 0 ? hz : 100)) : 0;   // ticks -> % of a core
    int big = atomic_load(&g_big_tid);
    // DM_GUEST_BIGCORE pick: the busiest guest thread (keeps the current one unless another used 25% more), or the presenting one
    if (C.big_mask) {
        int want = big;
        if (C.big_present) want = atomic_load(&g_guest_present_tid);
        else if (!first) {
            int bi = -1, ci = -1;
            for (int i = 0; i < nc; i++) { int g = 0; for (int j = 0; j < ng && !g; j++) g = gt[j] == cur[i].tid; if (!g) continue;
                if (cur[i].tid == big) ci = i; if (bi < 0 || cur[i].u > cur[bi].u) bi = i; }
            if (bi >= 0 && cur[bi].u * per >= 20 && (ci < 0 || (double)cur[bi].u > 1.25 * (double)cur[ci].u)) want = cur[bi].tid;
        }
        if (!want) want = atomic_load(&g_guest_present_tid);   // until the first full window
        if (want != big) { atomic_store(&g_big_tid, want); big = want; big_check(); }
    }
    if (first || secs <= 0) return;
    char l[600]; size_t o = (size_t)snprintf(l, sizeof l, "[perf] threads 10s (%.1f s, %d threads, process %.0f%%):", secs, nc, (double)total * per);
    for (int k = 0; k < 6 && o < sizeof l; k++) {
        int b = -1; for (int i = 0; i < nc; i++) if (cur[i].u && (b < 0 || cur[i].u > cur[b].u)) b = i;
        if (b < 0) break;
        int g = 0; for (int j = 0; j < ng && !g; j++) g = gt[j] == cur[b].tid;
        o += (size_t)snprintf(l + o, sizeof l - o, " %d:%s%s%s %.0f%% c%d", cur[b].tid, cur[b].comm, g ? "*" : "", cur[b].tid == big && C.big_mask ? "B" : "", (double)cur[b].u * per, cur[b].cpu);
        cur[b].u = 0;
    }
    LOG("%s", l);
    clocks_log();
    if (C.big_mask) {
        tstat_t ts; task_stat(big, &ts);
        LOG("[perf] bigcore 10s: tid %d (%s) mask %x last cpu %d, mask found reset %u times (total %u), failures %u", big, big ? ts.comm : "-", C.big_mask, big ? ts.cpu : -1, g_big_resets_w, g_big_resets, g_big_fail);
        g_big_resets_w = 0;
    }
}
static void *perf_monitor(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "dm-perfmon");
    cfg(); clocks_init();
    memset(&g_big_set, 0, sizeof g_big_set); memset(&g_all_set, 0, sizeof g_all_set);
    long ncpu = sysconf(_SC_NPROCESSORS_CONF); if (ncpu <= 0 || ncpu > 32) ncpu = 8;
    for (int c = 0; c < ncpu; c++) { g_all_set.b[0] |= 1ul << c; if (C.big_mask >> c & 1) g_big_set.b[0] |= 1ul << c; }
    if (C.big_mask) LOG("[perf] DM_GUEST_BIGCORE=%x: the %s guest thread is kept on these CPUs (checked every 2 s)", C.big_mask, C.big_present ? "presenting" : "busiest");
    int64_t t0 = perf_now(), next_scan = t0 + 2000000000ll, next_big = t0 + 2000000000ll;
    for (;;) {
        struct timespec s = {1, 0}; nanosleep(&s, NULL);
        int64_t now = perf_now();
        clocks_sample();
        if (C.big_mask && now >= next_big) { big_check(); next_big = now + 2000000000ll; }
        if (now >= next_scan) { monitor_scan(now); next_scan = now + 10000000000ll; }
    }
    return NULL;
}

void dm_aperf_start(void) {
    static int done; if (done) return; done = 1;
    cfg();
    { pthread_t th; if (!pthread_create(&th, NULL, perf_monitor, NULL)) pthread_detach(th); }
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
// Everything here is skipped when the process runs under an ARM translation layer (LDPlayer / emulators: libhoudini /
// libndk_translation bridge these platform calls and crash in them); real devices keep it. DM_ADPF=0 / DM_NO_ADPF only drop the
// hint session (before r2 DM_ADPF=0 also skipped the thermal frame-rate vote).
static int translation_layer(void) {
    static int off = -1;
    if (off >= 0) return off;
    off = 0;
    FILE *f = fopen("/proc/self/maps", "r"); char line[512];
    if (f) { while (fgets(line, sizeof line, f)) if (strstr(line, "libhoudini") || strstr(line, "libndk_translation")) { off = 1; break; } fclose(f); }
    if (off) LOG("[perf] performance hints off (ARM translation layer detected)");
    return off;
}
void dm_aperf_present(ANativeWindow *w, float marker_vote) {
    if (translation_layer()) return;
    cfg();
    double period = dm_present_period();
    int64_t p = period > 0 && period < 1 ? (int64_t)(period * 1e9) : 16666667, t = perf_now();
    if (C.target) p = C.target;   // DM_ADPF_TARGET_MS
    static unsigned frames; static int64_t prev_t; static uint64_t prev_sleep;
    // the sleeps of this interval (DM_ADPF_WORK=wall): the guest's pacing sleep in AcquireTexture and this thread's acquire waits
    uint64_t sl_total = dm_nvn_pace_sleep_ns ? dm_nvn_pace_sleep_ns() : 0;
    int64_t sleeps = (int64_t)(sl_total - prev_sleep) + g_present_wait; prev_sleep = sl_total; g_present_wait = 0;
    if (!C.hints_off) {
        hint_init(p);
        static int seen_big;
        int b = C.big_mask ? atomic_load(&g_big_tid) : 0;
        if (frames++ % 300 == 0 || b != seen_big) { seen_big = b; hint_threads_scan((int32_t)gettid()); }
        if (g_hint) {
            if (!g_hint_n) { g_hint_tids[0] = (int32_t)gettid(); g_hint_n = 1; }
            int64_t cpu = frame_cpu_work(), iv = prev_t ? t - prev_t : 0, work = cpu;
            if (C.wall) { int64_t c = cpu < iv ? cpu : iv; work = iv - sleeps; if (work < c) work = c; }   // wall less the sleeps, never below the CPU work
            else if (!C.noclip && work > iv && iv > 0) work = iv;
            if (work > 0 && iv > 0 && iv < 250000000) {   // (a longer span: the game was held, by a load, the pause menu or the background)
                double gms = xbg_recent_gpu_ms && C.gpu ? xbg_recent_gpu_ms() : -1;
                if (g_wd && gms > 0) {
                    int64_t g = (int64_t)(gms * 1e6);
                    H.wd_start(g_wd, prev_t); H.wd_total(g_wd, work > g ? work : g); H.wd_cpu(g_wd, work); H.wd_gpu(g_wd, g);
                    if (H.report2(g_hint, g_wd)) H.report(g_hint, work);
                } else H.report(g_hint, work);
                static int64_t wsum, wmax; static unsigned wn; wsum += work; if (work > wmax) wmax = work;
                if (++wn == 600) { LOG("[perf] ADPF: frame %s work mean %.1f / max %.1f ms against %.1f ms, thermal %s%s", C.wall ? "wall" : "CPU", wsum / 600 / 1e6, wmax / 1e6, p / 1e6, th_name(atomic_load(&g_tstatus)), C.tag); wsum = wmax = 0; wn = 0; }
            }
            if (p != g_hint_target && H.update) { H.update(g_hint, p); g_hint_target = p; LOG("[perf] ADPF target %.2f ms", p / 1e6); }
        }
    }
    prev_t = t;
    static ANativeWindow *voted_w; static float voted;
    float want = vote_for(marker_vote, period);
    // [r6b] The menu / loading present cap (DM_PRESENT_MENU_HZ) does not change the window's vote: with the thermal vote it would drop
    // the panel to 60 Hz in a menu, and the gameplay tier (the largest fraction of the *current* refresh under the limit) would then
    // stay at 60 until the phone cooled down. Display mode switches on every menu / gameplay change are avoided too.
    if (voted > 0 && w == voted_w && dm_nvn_present_menu_capped && dm_nvn_present_menu_capped()) want = voted;
    if (w && want > 0 && (w != voted_w || fabsf(want - voted) > 0.5f)) { vote(w, want); voted_w = w; voted = want; }
}
