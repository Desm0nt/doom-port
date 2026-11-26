// [longframe-diag] PNH-C11 (docs/port-kb/peanuts/C-framerate-system.md; port-common/modules/longframe-diag): who ate a long frame.
// Off by default (a diagnostic report: owner rule "diagnostic reports stay off"). DM_LONGFRAME=1 turns it on. The tree's own long-frame line
// (hle_nvn.c, DM_LONGFRAME unset = on, 0 = off) is not touched: unset keeps it as it was, 0 still turns it off, 1 keeps it and adds this.
// When on, a sampler thread (dm-lfthreads) reads every thread's counters of this process from /proc/self/task/<tid>/schedstat:
// "<ns on a core> <ns runnable, waiting for a core> <timeslices>", every DM_LONGFRAME_THREADS ms (100) into a ring. The presenting thread
// only compares the frame's length with DM_LONGFRAME_X (2.0) x the present period: for a long frame it posts a request (one line at most every
// DM_LONGFRAME_GAP_MS, 1000) and goes on; it never reads /proc itself. The sampler then takes one fresh reading, uses the newest ring reading
// from before the frame's start as the base, and logs one line:
//   [longframe] frame N: F ms (Kx the T ms period); threads over W ms: 'a' cpu C wait Q, 'b' ..., 'c' ...; presenting 'p' cpu C wait Q;
//   most waiting 'm' wait Q cpu C; all N threads cpu C wait Q; K long frames not logged since the last line
// cpu = time on a core, wait = time runnable but waiting for a core (runqueue: the core was taken by someone else, or the thread was parked on
// a cluster that was full). W is the window: the frame plus at most one sampler period before it (the base reading is older than the frame's
// start), so the numbers are "over W ms", not exactly over the frame. Names are /proc's comm (guest threads carry the guest's name in most
// trees: hle_thread.c). Frames longer than 3 s (or than the ring's history) are holds (pause, background, load) and are not looked at. Kernels without schedstat: stat's
// utime + stime (clock ticks), the wait is '?'. Cost when off: one static int test per present. When on: ~10 readings a second on the
// sampler thread (one open / read / close per thread), and per long frame one more reading plus the comm of at most 5 threads.
// Adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: dm/gfx/vk_android/vka_perf.c (the [longframe] line's busiest threads:
// the dm-lfthreads sampler ts_read / ts_busiest / ts_main, DM_LONGFRAME_THREADS) and dm/gfx/vk_android/vka_pace.h (vkpace_schedstat: the run /
// wait fields). Changes from peanuts: off by default, on with DM_LONGFRAME=1 (peanuts: the sampler runs whenever DM_LONGFRAME_THREADS > 0,
// 250 ms); the sampler period default is 100 ms so the window is at most the frame + 0.1 s; the threshold is relative to the present period
// (peanuts: a fixed 100 ms); each thread's runqueue wait and the totals of all threads are in the line; the presenting thread and the thread
// that waited most are named even outside the top 3; the sampler prints the line itself (peanuts: the frame path 8 frames later, next to the
// GPU span); a rate limit; a tid reused by a new thread reads its whole counter instead of 0.
#pragma once
#include <stdint.h>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { LFD_TMAX = 384, LFD_RING = 40, LFD_TOP = 3 };   // 40 readings: 4 s of history at 100 ms
typedef struct { int tid; uint64_t run, wait; } lfd_thr;               // one thread's counters (ns), or its change over a window
typedef struct { uint64_t t; int n, has_wait; lfd_thr e[LFD_TMAX]; } lfd_snap;   // one reading of every thread; t = when it started (ns, CLOCK_MONOTONIC)
typedef struct { uint64_t frame, frame_ns, target_ns, since; int caller; unsigned skipped; } lfd_req;

// ---------- pure parts (host test: port-common/modules/longframe-diag/test) ----------
// schedstat: "<run ns> <wait ns> <slices>"
static inline int lfd_parse_schedstat(const char *s, uint64_t *run, uint64_t *wait) {
    unsigned long long r, w;
    if (!s || sscanf(s, "%llu %llu", &r, &w) != 2) return 0;
    *run = r; *wait = w; return 1;
}
// stat: utime + stime (clock ticks), fields 14 and 15; the comm in parentheses may hold spaces and ')' itself, so count from the last ')'
static inline int lfd_parse_stat(const char *b, uint64_t *ticks) {
    const char *q = b ? strrchr(b, ')') : NULL; unsigned long long ut, st;
    if (!q || !q[1] || sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
    *ticks = ut + st; return 1;
}
// Every thread of `now` against `base`: a thread born since counts all of its time; a counter that went back (the tid was reused by a new
// thread) counts its whole value. Returns the number of entries in d; *all_run / *all_wait = the totals.
static inline int lfd_deltas(const lfd_snap *base, const lfd_snap *now, lfd_thr *d, uint64_t *all_run, uint64_t *all_wait) {
    uint64_t ar = 0, aw = 0; int n = 0;
    for (int i = 0; i < now->n; i++) {
        const lfd_thr *c = &now->e[i], *p = NULL;
        for (int j = 0; j < base->n; j++) if (base->e[j].tid == c->tid) { p = &base->e[j]; break; }
        lfd_thr x = *c;
        if (p && c->run >= p->run) { x.run = c->run - p->run; x.wait = c->wait >= p->wait ? c->wait - p->wait : 0; }
        d[n++] = x; ar += x.run; aw += x.wait;
    }
    *all_run = ar; *all_wait = aw; return n;
}
// The k entries that ran most (descending; ties: lower tid first). Returns how many (entries with no run time are left out).
static inline int lfd_top(const lfd_thr *d, int n, int k, int *idx) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!d[i].run) continue;
        int at = m;
        while (at > 0 && (d[idx[at - 1]].run < d[i].run || (d[idx[at - 1]].run == d[i].run && d[idx[at - 1]].tid > d[i].tid))) at--;
        if (at >= k) continue;
        for (int s = (m < k ? m : k - 1); s > at; s--) idx[s] = idx[s - 1];
        idx[at] = i; if (m < k) m++;
    }
    return m;
}
// Should the presenting thread ask for a line? frame and target in ns; x = the factor; hold = frames at least this long are not looked at.
static inline int lfd_is_long(uint64_t frame_ns, uint64_t target_ns, double x, uint64_t hold_ns) {
    if (!target_ns) target_ns = 16666667;
    return frame_ns < hold_ns && (double)frame_ns >= x * (double)target_ns;
}
typedef const char *(*lfd_name_fn)(int tid, char *buf, size_t n);
// The line (without the "[longframe] " prefix). wait_known = 0: the kernel has no schedstat, waits are '?'.
static inline size_t lfd_format(char *out, size_t n, const lfd_req *r, const lfd_snap *base, const lfd_snap *now, lfd_thr *d, lfd_name_fn name) {
    uint64_t ar, aw; int nd = lfd_deltas(base, now, d, &ar, &aw), top[LFD_TOP], nt = lfd_top(d, nd, LFD_TOP, top);
    const int wk = base->has_wait && now->has_wait;
    char nm[40], w[32];
    double tgt = (r->target_ns ? r->target_ns : 16666667) / 1e6;
    size_t o = (size_t)snprintf(out, n, "frame %llu: %.1f ms (%.1fx the %.1f ms period); threads over %.0f ms:", (unsigned long long)r->frame,
                                r->frame_ns / 1e6, r->frame_ns / 1e6 / tgt, tgt, (now->t - base->t) / 1e6);
#define LFD_W(v) (wk ? (snprintf(w, sizeof w, "%.1f", (v) / 1e6), w) : "?")
    int caller_in = 0, mw = -1;
    for (int k = 0; k < nt && o < n; k++) {
        const lfd_thr *t = &d[top[k]]; if (t->tid == r->caller) caller_in = 1;
        o += (size_t)snprintf(out + o, n - o, "%s '%s' cpu %.1f wait %s", k ? "," : "", name(t->tid, nm, sizeof nm), t->run / 1e6, LFD_W(t->wait));
    }
    if (!nt && o < n) o += (size_t)snprintf(out + o, n - o, " none ran");
    if (!caller_in && r->caller && o < n) {
        for (int i = 0; i < nd; i++) if (d[i].tid == r->caller) {
            o += (size_t)snprintf(out + o, n - o, "; presenting '%s' cpu %.1f wait %s", name(d[i].tid, nm, sizeof nm), d[i].run / 1e6, LFD_W(d[i].wait)); break; }
    }
    if (wk) {
        for (int i = 0; i < nd; i++) if (d[i].wait && (mw < 0 || d[i].wait > d[mw].wait)) mw = i;
        int listed = mw >= 0 && d[mw].tid == r->caller;
        for (int k = 0; k < nt && mw >= 0; k++) if (top[k] == mw) listed = 1;
        if (mw >= 0 && !listed && o < n) o += (size_t)snprintf(out + o, n - o, "; most waiting '%s' wait %.1f cpu %.1f", name(d[mw].tid, nm, sizeof nm), d[mw].wait / 1e6, d[mw].run / 1e6);
    }
    if (o < n) o += (size_t)snprintf(out + o, n - o, "; all %d threads cpu %.1f wait %s ms", nd, ar / 1e6, LFD_W(aw));
    if (r->skipped && o < n) o += (size_t)snprintf(out + o, n - o, "; %u long frames not logged since the last line", r->skipped);
#undef LFD_W
    return o < n ? o : n - 1;
}

// ---------- runtime ----------
static inline uint64_t lfd_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static struct {
    int on;                                  // -1 = not read yet, 0 = off, 1 = on
    int period_ms, gap_ms; double x;
    pthread_mutex_t mu; pthread_cond_t cv;
    int asked; lfd_req req;
} g_lfd = {-1, 100, 1000, 2.0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, {0}};
static inline int lfd_env_int(const char *k, int def, int lo, int hi) { const char *e = getenv(k); int v = e && *e ? atoi(e) : def; return v < lo ? lo : v > hi ? hi : v; }
static inline int dm_lfd_on(void) {
    if (g_lfd.on < 0) {
        const char *e = getenv("DM_LONGFRAME");
        g_lfd.period_ms = lfd_env_int("DM_LONGFRAME_THREADS", 100, 0, 5000);   // peanuts' name: 0 = no sampler (off)
        g_lfd.gap_ms = lfd_env_int("DM_LONGFRAME_GAP_MS", 1000, 0, 600000);
        const char *x = getenv("DM_LONGFRAME_X"); g_lfd.x = x && *x ? atof(x) : 2.0; if (!(g_lfd.x >= 1.0 && g_lfd.x <= 100.0)) g_lfd.x = 2.0;
        g_lfd.on = e && !strcmp(e, "1") && g_lfd.period_ms >= 10;
    }
    return g_lfd.on;
}
static inline const char *dm_lfd_why(void) {
    static char b[160];
    if (!dm_lfd_on()) return g_lfd.period_ms > 0 && g_lfd.period_ms < 10 ? "DM_LONGFRAME_THREADS under 10 ms: off" : "a diagnostic, off by default: DM_LONGFRAME=1 names the busiest threads of long frames";
    snprintf(b, sizeof b, "DM_LONGFRAME=1: busiest threads of frames >= %.1fx the present period, sampler every %d ms, a line at most every %d ms", g_lfd.x, g_lfd.period_ms, g_lfd.gap_ms);
    return b;
}
// Read one counter file of a thread (dir = /proc/self/task as an fd)
static inline int lfd_read_file(int dir, int tid, const char *what, char *b, size_t n) {
    char p[48]; snprintf(p, sizeof p, "%d/%s", tid, what);
    int fd = openat(dir, p, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t k = read(fd, b, n - 1); close(fd);
    if (k <= 0) return 0;
    b[k] = 0; return 1;
}
static inline int lfd_read(lfd_snap *s) {
    static long hz; if (!hz) { hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100; }
    s->t = lfd_now(); s->n = 0; s->has_wait = 1;
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    int dir = dirfd(d); struct dirent *de; char b[512];
    while ((de = readdir(d)) && s->n < LFD_TMAX) {
        int tid = atoi(de->d_name); if (tid <= 0) continue;
        lfd_thr *e = &s->e[s->n]; uint64_t tk;
        if (lfd_read_file(dir, tid, "schedstat", b, sizeof b) && lfd_parse_schedstat(b, &e->run, &e->wait)) { e->tid = tid; s->n++; continue; }
        if (lfd_read_file(dir, tid, "stat", b, sizeof b) && lfd_parse_stat(b, &tk)) { e->tid = tid; e->run = tk * (1000000000ull / (uint64_t)hz); e->wait = 0; s->has_wait = 0; s->n++; }
    }
    closedir(d);
    return s->n;
}
static inline const char *lfd_comm(int tid, char *buf, size_t n) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    int fd = open(p, O_RDONLY | O_CLOEXEC); ssize_t k = fd >= 0 ? read(fd, buf, n - 1) : -1; if (fd >= 0) close(fd);
    if (k <= 0) { snprintf(buf, n, "%d (exited)", tid); return buf; }
    buf[k] = 0; buf[strcspn(buf, "\n")] = 0; return buf;
}
static inline void *lfd_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "dm-lfthreads");
    lfd_snap *ring[LFD_RING] = {0}, *cur = (lfd_snap *)malloc(sizeof(lfd_snap)); lfd_thr *d = (lfd_thr *)malloc(LFD_TMAX * sizeof(lfd_thr));
    for (int i = 0; i < LFD_RING; i++) ring[i] = (lfd_snap *)calloc(1, sizeof(lfd_snap));
    for (int i = 0; i < LFD_RING; i++) if (!ring[i]) cur = NULL;
    if (!cur || !d) { LOG("[longframe] sampler: out of memory, off"); return NULL; }
    unsigned rn = 0; uint64_t next = 0; char line[640];
    pthread_mutex_lock(&g_lfd.mu);
    for (;;) {
        uint64_t now = lfd_now();
        if (!g_lfd.asked && now < next) {   // nothing asked and not yet time for a reading: wait for either
            uint64_t dl = next - now; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(dl / 1000000000ull); ts.tv_nsec += (long)(dl % 1000000000ull); if (ts.tv_nsec >= 1000000000l) { ts.tv_sec++; ts.tv_nsec -= 1000000000l; }
            pthread_cond_timedwait(&g_lfd.cv, &g_lfd.mu, &ts); continue;
        }
        const int asked = g_lfd.asked; const lfd_req r = g_lfd.req;
        pthread_mutex_unlock(&g_lfd.mu);
        const int ok = lfd_read(cur) > 0;
        if (asked) {
            const lfd_snap *base = NULL;
            for (unsigned k = 0; k < LFD_RING && k < rn; k++) { const lfd_snap *s = ring[(rn - 1 - k) % LFD_RING]; if (s->t <= r.since) { base = s; break; } }
            if (ok && base) { lfd_format(line, sizeof line, &r, base, cur, d, lfd_comm); LOG("[longframe] %s", line); }
            else LOG("[longframe] frame %llu: %.1f ms; threads: no reading from before the frame (%s)", (unsigned long long)r.frame, r.frame_ns / 1e6, ok ? "the sampler started later" : "/proc/self/task unreadable");
        }
        if (ok) { lfd_snap *t = ring[rn % LFD_RING]; ring[rn % LFD_RING] = cur; cur = t; rn++; next = lfd_now() + (uint64_t)g_lfd.period_ms * 1000000ull; }
        else next = lfd_now() + 1000000000ull;
        pthread_mutex_lock(&g_lfd.mu);
        if (asked) g_lfd.asked = 0;
    }
    return NULL;
}
// Once per present on the presenting thread: frame = its number, frame_ns = the time since the previous present, target_ns = the present
// period (0 = 60 fps). Cheap: no /proc reads here, at most one mutex trylock + signal per long frame.
static inline void dm_lfd_frame(uint64_t frame, uint64_t frame_ns, uint64_t target_ns) {
    static int started; static uint64_t last; static unsigned skipped;
    if (!dm_lfd_on()) return;
    if (!started) {
        started = 1; pthread_t th;
        if (pthread_create(&th, NULL, lfd_main, NULL)) { g_lfd.on = 0; LOG("[longframe] sampler thread not started: off"); return; }
        pthread_detach(th);
    }
    uint64_t hold = (uint64_t)(LFD_RING - 2) * (uint64_t)g_lfd.period_ms * 1000000ull; if (hold > 3000000000ull) hold = 3000000000ull;   // the ring must reach back past the frame
    if (!lfd_is_long(frame_ns, target_ns, g_lfd.x, hold)) return;
    const uint64_t now = lfd_now();
    if (last && now - last < (uint64_t)g_lfd.gap_ms * 1000000ull) { skipped++; return; }
    if (pthread_mutex_trylock(&g_lfd.mu)) { skipped++; return; }
    if (g_lfd.asked) { pthread_mutex_unlock(&g_lfd.mu); skipped++; return; }   // the sampler is still on the previous one
    g_lfd.req.frame = frame; g_lfd.req.frame_ns = frame_ns; g_lfd.req.target_ns = target_ns; g_lfd.req.since = now - frame_ns;
    g_lfd.req.caller = (int)syscall(SYS_gettid); g_lfd.req.skipped = skipped; g_lfd.asked = 1;
    pthread_cond_signal(&g_lfd.cv);
    pthread_mutex_unlock(&g_lfd.mu);
    last = now; skipped = 0;
}
#else   // no /proc: Apple builds of the same tree
static inline int dm_lfd_on(void) { return 0; }
static inline const char *dm_lfd_why(void) { return "no /proc on this system"; }
static inline void dm_lfd_frame(uint64_t frame, uint64_t frame_ns, uint64_t target_ns) { (void)frame; (void)frame_ns; (void)target_ns; }
#endif
