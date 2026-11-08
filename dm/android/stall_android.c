// [DOOM] 2.45 Frame stall watch, taken from DOOM Eternal's anrfix (72421d5, doom-eternal-port src-cmp16 dm/android/stall_android.c, docs/port-kb APP-02);
// the code below is Eternal's unchanged, only this header differs. Companion of frontend-common PadSurface.java (APP-01): PadSurface keeps a long
// GPU stall from becoming an ANR kill, this file says in boot.log that the stall happened and who was waiting on what.
// When no frame has been presented for DM_STALL_WATCH_MS (2000; 0 = off) while the game is in front and not held (pause menu / update notice =
// nativeHold, background = nativeFocus <= 1), a 【红】 report of every native thread of the process is logged while it happens, again every 2 s
// (5 times, then every 10 s), and one line when the presents resume (how long that frame took). Nothing is recovered or killed: it only reports.
// A report line per thread: tid, name, scheduler state (R running, S sleeping, D uninterruptible), CPU used since the previous sample (<= 1 s
// ago, or since the previous report), the system call it sits in (/proc/self/task/<tid>/syscall: futex with the lock's address as module+offset,
// ioctl / read / write / ppoll with the file behind the fd, e.g. /dev/kgsl-3d0 = waiting on the GPU driver) and the user pc as module+offset
// (llvm-symbolizer on the unstripped libdm.so). The first report lists all threads, the later ones only those that ran or changed.
// DM_STALL_SIGQUIT=1: with the first report of a stall (at most once a minute) the process also sends itself SIGQUIT: ART's signal catcher then
// writes the Java and native stacks of every thread to tombstoned (adb bugreport collects them under FS/data/anr/), as an ANR does. Off by default.
// Test: DM_TEST_STALL_MS=<ms> makes the presenting thread sleep that long at a present, first DM_TEST_STALL_AFTER_S (30) s after the first present,
// then every DM_TEST_STALL_EVERY_S (0 = once): the watch must report it, the UI must stay responsive (it does not wait for the game).
// Hooks: dm_stall_present() per present on the presenting thread (perf_android.c dm_aperf_present, first line: also under a translation layer),
// dm_stall_watch_start() after the host thread starts and dm_stall_quiet() on the pause menu / background (jni_android.c).
#include "../host/dm.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static _Atomic int64_t g_last_present, g_unquiet_at;   // CLOCK_MONOTONIC ns: the last present; the last time the game came back in front / out of the menu
static _Atomic uint64_t g_presents;
static _Atomic int g_present_tid, g_quiet;   // g_quiet bits: 1 the pause menu holds the game, 2 not in front

static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static int env_int(const char *name, int def) { const char *e = getenv(name); return e && *e ? atoi(e) : def; }

void dm_stall_quiet(int bit, int on) {
    int was = on ? atomic_fetch_or(&g_quiet, bit) : atomic_fetch_and(&g_quiet, ~bit);
    if (!on && (was & bit)) atomic_store(&g_unquiet_at, mono_ns());   // a hold is not a stall: the gap counts from here
}

void dm_stall_present(void) {
    static int test_ms = -1, after_s, every_s; static int64_t first, next;
    if (test_ms < 0) {
        test_ms = env_int("DM_TEST_STALL_MS", 0); after_s = env_int("DM_TEST_STALL_AFTER_S", 30); every_s = env_int("DM_TEST_STALL_EVERY_S", 0);
        if (test_ms > 0) LOG("[stall] test: the presenting thread sleeps %d ms at a present %d s after the first one%s (DM_TEST_STALL_MS)", test_ms, after_s,
                             every_s > 0 ? ", then again every DM_TEST_STALL_EVERY_S" : "");
    }
    if (test_ms > 0) {
        int64_t t = mono_ns();
        if (!first) { first = t; next = t + (int64_t)after_s * 1000000000; }
        if (next && t >= next) {
            LOG("[stall] test: presenting thread %d sleeps %d ms now (DM_TEST_STALL_MS)", (int)gettid(), test_ms);
            usleep((useconds_t)test_ms * 1000);
            next = every_s > 0 ? mono_ns() + (int64_t)every_s * 1000000000 : 0;
        }
    }
    atomic_store(&g_present_tid, (int)gettid());
    atomic_fetch_add(&g_presents, 1);
    atomic_store(&g_last_present, mono_ns());
}

// ---- per-thread state
#define MAXT 256
typedef struct { int tid; int64_t cpu; char state; long nr; } tsnap;
static tsnap g_base[MAXT], g_last[MAXT]; static int g_nbase, g_nlast; static int64_t g_base_t;

// CPU time of a thread of this process: the kernel's per-thread CPU clock (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)), as perf_android.c
static int64_t thread_cpu_ns(int tid) {
    clockid_t c = (clockid_t)(((uint32_t)~(uint32_t)tid << 3) | 6u); struct timespec ts;
    if (clock_gettime(c, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int read_small(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) { out[0] = 0; return -1; }
    ssize_t r = read(fd, out, n - 1); close(fd); if (r < 0) r = 0; out[r] = 0;
    while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ')) out[--r] = 0;
    return (int)r;
}
static int list_tids(int *tids, int max) {
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) { int t = atoi(e->d_name); if (t > 0) tids[n++] = t; }
    closedir(d);
    for (int i = 1; i < n; i++) { int v = tids[i], j = i - 1; while (j >= 0 && tids[j] > v) { tids[j + 1] = tids[j]; j--; } tids[j + 1] = v; }
    return n;
}
static char task_state(int tid, char *comm, size_t cn) {
    char p[64], b[512]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    comm[0] = 0; if (read_small(p, b, sizeof b) <= 0) return '?';
    char *l = strchr(b, '('), *r = strrchr(b, ')'); if (!l || !r || r < l) return '?';
    size_t k = (size_t)(r - l - 1); if (k >= cn) k = cn - 1; memcpy(comm, l + 1, k); comm[k] = 0;
    return r[1] == ' ' ? r[2] : '?';
}
static const char *sys_name(long nr) {   // arm64 numbers
    switch (nr) {
    case 98: return "futex"; case 29: return "ioctl"; case 73: return "ppoll"; case 22: return "epoll_pwait"; case 101: return "nanosleep";
    case 115: return "clock_nanosleep"; case 63: return "read"; case 64: return "write"; case 72: return "pselect6"; case 124: return "sched_yield";
    case 207: return "recvfrom"; case 212: return "recvmsg"; case 211: return "sendmsg"; case 260: return "wait4"; case 95: return "waitid";
    case 133: return "rt_sigsuspend"; case 137: return "rt_sigtimedwait"; case 65: return "readv"; case 66: return "writev"; case 67: return "pread64";
    case 68: return "pwrite64"; case 226: return "mprotect"; case 222: return "mmap"; case 215: return "munmap"; case 233: return "madvise";
    case 56: return "openat"; case 57: return "close"; case 129: return "kill"; case 131: return "tgkill"; case 169: return "gettimeofday";
    case 113: return "clock_gettime"; case 278: return "getrandom"; case 281: return "execveat"; case 167: return "prctl";
    default: return NULL;
    }
}
static void where(uintptr_t a, char *out, size_t n) {   // module+offset (symbol) of an address in this process
    Dl_info di; memset(&di, 0, sizeof di);
    if (!a) { snprintf(out, n, "0"); return; }
    if (dladdr((void *)a, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/'); b = b ? b + 1 : di.dli_fname;
        if (di.dli_sname && di.dli_saddr) snprintf(out, n, "%s+%#lx(%s+%#lx)", b, (unsigned long)(a - (uintptr_t)di.dli_fbase), di.dli_sname, (unsigned long)(a - (uintptr_t)di.dli_saddr));
        else snprintf(out, n, "%s+%#lx", b, (unsigned long)(a - (uintptr_t)di.dli_fbase));
    } else snprintf(out, n, "%#lx", (unsigned long)a);
}
// /proc/self/task/<tid>/syscall decoded: "futex(WAIT libdm.so+0x...) pc libc.so+0x..." ; "running" ; "-" when unreadable
static long task_syscall(int tid, char *out, size_t n) {
    char p[64], b[256]; snprintf(p, sizeof p, "/proc/self/task/%d/syscall", tid);
    if (read_small(p, b, sizeof b) <= 0) { snprintf(out, n, "-"); return -2; }
    if (!strncmp(b, "running", 7)) { snprintf(out, n, "running"); return -3; }
    unsigned long long v[9] = {0}; char *s = b; long nr = strtol(s, &s, 10);
    for (int i = 0; i < 8 && *s; i++) v[i] = strtoull(s, &s, 16);
    if (nr < 0) { char pc[96]; where((uintptr_t)v[1], pc, sizeof pc); snprintf(out, n, "user pc %s", pc); return nr; }   // "-1 sp pc": not in a system call
    const char *name = sys_name(nr); char pc[96], arg[160] = "";
    where((uintptr_t)v[7], pc, sizeof pc);
    if (nr == 98) { static const char *ops[] = {"WAIT", "WAKE", "FD", "REQUEUE", "CMP_REQUEUE", "WAKE_OP", "LOCK_PI", "UNLOCK_PI", "TRYLOCK_PI", "WAIT_BITSET"};
        unsigned op = (unsigned)(v[1] & 0x7f); char w[96]; where((uintptr_t)v[0], w, sizeof w);
        snprintf(arg, sizeof arg, "%s %s", op < 10 ? ops[op] : "op?", w); }
    else if (nr == 29 || nr == 63 || nr == 64 || nr == 65 || nr == 66 || nr == 67 || nr == 68) {
        char fp[64], t[128]; snprintf(fp, sizeof fp, "/proc/self/fd/%llu", v[0]); ssize_t k = readlink(fp, t, sizeof t - 1); t[k > 0 ? k : 0] = 0;
        if (nr == 29) snprintf(arg, sizeof arg, "fd %llu %s, cmd %#llx", v[0], t, v[1]); else snprintf(arg, sizeof arg, "fd %llu %s", v[0], t); }
    if (name) snprintf(out, n, "%s(%s) pc %s", name, arg, pc); else snprintf(out, n, "syscall %ld(%#llx, %#llx) pc %s", nr, v[0], v[1], pc);
    return nr;
}
static int sample(tsnap *s, int max) {   // tid + CPU of every thread
    int tids[MAXT]; int n = list_tids(tids, max < MAXT ? max : MAXT);
    for (int i = 0; i < n; i++) { s[i].tid = tids[i]; s[i].cpu = thread_cpu_ns(tids[i]); s[i].state = 0; s[i].nr = -9; }
    return n;
}
static const tsnap *find(const tsnap *s, int n, int tid) { for (int i = 0; i < n; i++) if (s[i].tid == tid) return &s[i]; return NULL; }

static void report(int64_t gap, int nth, int64_t now) {
    int ptid = atomic_load(&g_present_tid); int quiet = atomic_load(&g_quiet);
    double win = g_base_t ? (now - g_base_t) / 1e9 : 0;
    LOG("【红】[stall] no frame presented for %.1f s (report %d; %llu frames so far; presenting thread %d; %s): native threads: tid name state, CPU in the last %.2f s, system call",
        gap / 1e9, nth, (unsigned long long)atomic_load(&g_presents), ptid, quiet ? "held / not in front" : "in front", win);
    int tids[MAXT]; int n = list_tids(tids, MAXT), shown = 0, quietn = 0;
    tsnap cur[MAXT]; int nc = 0;
    // the presenting thread first, then by tid
    for (int pass = 0; pass < 2; pass++) for (int i = 0; i < n; i++) {
        int tid = tids[i]; if ((pass == 0) != (tid == ptid)) continue;
        char comm[24], sc[320]; char st = task_state(tid, comm, sizeof comm); int64_t cpu = thread_cpu_ns(tid);
        long nr = task_syscall(tid, sc, sizeof sc);
        const tsnap *b = find(g_base, g_nbase, tid), *l = find(g_last, g_nlast, tid);
        double used = b && cpu >= 0 && b->cpu >= 0 ? (cpu - b->cpu) / 1e6 : -1;
        if (nc < MAXT) cur[nc++] = (tsnap){tid, cpu, st, nr};
        // later reports: only the threads that ran (>= 5 ms) or changed state / system call, and the presenting thread
        if (nth > 1 && tid != ptid && l && l->state == st && l->nr == nr && used < 5) { quietn++; continue; }
        char wc[64] = ""; { char p[64], w[48]; snprintf(p, sizeof p, "/proc/self/task/%d/wchan", tid); if (read_small(p, w, sizeof w) > 0 && strcmp(w, "0")) snprintf(wc, sizeof wc, " wchan %s", w); }
        LOG("【红】[stall]  %d %-15s %c cpu %6.1f ms  %s%s", tid, comm, st, used, sc, wc);
        shown++;
    }
    if (quietn) LOG("【红】[stall]  (%d more threads unchanged since report %d, under 5 ms CPU)", quietn, nth - 1);
    memcpy(g_last, cur, sizeof(tsnap) * (size_t)nc); g_nlast = nc;
    g_nbase = sample(g_base, MAXT); g_base_t = mono_ns();   // the next report's CPU column covers the time since this one
    (void)shown;
}

static void *watch_main(void *arg) {
    (void)arg; prctl(PR_SET_NAME, "dm-stallwatch");
    const int64_t limit = (int64_t)env_int("DM_STALL_WATCH_MS", 2000) * 1000000;
    const int sigquit = env_int("DM_STALL_SIGQUIT", 0);
    LOG("[stall] frame stall watch: a report of every native thread when no frame is presented for %lld ms in front (DM_STALL_WATCH_MS)%s",
        (long long)(limit / 1000000), sigquit ? "; SIGQUIT to self with the first report (DM_STALL_SIGQUIT=1)" : "");
    int64_t stall_lp = 0, next = 0, last_sigquit = 0; int nth = 0;
    for (;;) {
        usleep(250000);
        int64_t now = mono_ns(), lp = atomic_load(&g_last_present);
        if (!lp) continue;   // nothing presented yet: boot
        if (stall_lp && lp != stall_lp) {
            LOG("【红】[stall] over: presents resumed, that frame took %.2f s (%d reports)", (lp - stall_lp) / 1e9, nth);
            stall_lp = 0; nth = 0;
        }
        if (!g_base_t || now - g_base_t >= 1000000000) { g_nbase = sample(g_base, MAXT); g_base_t = now; }
        int64_t from = lp, uq = atomic_load(&g_unquiet_at); if (uq > from) from = uq;
        int64_t gap = now - from;
        if (atomic_load(&g_quiet) || gap < limit) { if (!stall_lp) continue; }
        if (!stall_lp) { stall_lp = lp; nth = 0; next = now; }
        if (now < next) continue;
        nth++;
        report(now - lp, nth, now);
        if (nth == 1 && sigquit && (!last_sigquit || now - last_sigquit >= 60000000000LL)) {
            last_sigquit = now; LOG("【红】[stall] SIGQUIT to self: ART writes every thread's stack to tombstoned (DM_STALL_SIGQUIT)"); kill(getpid(), SIGQUIT);
        }
        next = now + (nth < 5 ? limit : 10000000000LL);
    }
    return NULL;
}

void dm_stall_watch_start(void) {
    static int started;
    if (started || env_int("DM_STALL_WATCH_MS", 2000) <= 0) return;
    started = 1;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256u << 10);
    if (!pthread_create(&th, &a, watch_main, NULL)) pthread_detach(th);
    pthread_attr_destroy(&a);
}
