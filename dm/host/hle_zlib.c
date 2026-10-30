// [DOOM] The game's statically linked zlib 1.2.3 inflate (update main) -> host zlib-ng 2.3.3 (third_party/zlib-ng, zlib license).
//
// Where it is used (update main, build id 28479919..., docs/research/idtech6_resources.md): idResourceContainer::ReadData
// (main+0x1ef2710: z_stream on the stack, inflateInit2_(-15), one inflate(Z_FINISH) of the whole entry, then checks
// total_out == size), the background loader's decompress thread (main+0x1efc810: raw stream fed in 4 MB read blocks into a ring,
// Z_NO_FLUSH, Z_FINISH on the last block), libpng (inflateInit_ / inflate per row / inflateReset) and a few small users.
// Every caller reaches zlib by a direct bl / b to five entries; patch_nso.py ZLIB_GATES puts an entry gate on each
// (svc 0x90..0x94, hle.h). The gate either returns the host's result (regs[8] = 1) or runs the original function (regs[8] = 0).
//
// z_stream of zlib 1.2.3 on LP64 = 0x70 bytes (inflateInit2_ checks stream_size == 0x70); the guest's pointers are host pointers.
// The host never runs zlib-ng on the guest struct: each guest stream gets a host context (zctx) whose address goes into
// strm->state (only zlib itself reads that field); every call copies next_in / avail_in / total_in / next_out / avail_out /
// total_out in, runs zng_inflate*, and copies them back with msg, data_type and (for zlib / gzip wrappers) adler.
// Intentional differences from 1.2.3, none read by the game's callers: zalloc / zfree / opaque are left as the caller set them
// (1.2.3 writes its zcalloc / zcfree there when NULL) and the state memory comes from the host heap, not the caller's allocator;
// msg texts are zlib-ng's; data_type's bit 256 (Z_TREES only) can differ. Return codes, flush values and decoding are zlib's.
//
// Switches (read once, at the first gate call; docs/SWITCHES.md section 5):
//   DM_ZLIB_FAST=1     zlib-ng instead of the game's inflate (default 1 since 30.10; 0 = original code, the gates fall through)
//   DM_ZLIB_STATS=1    every 5 s (DM_ZLIB_STATS_SEC) a "[zlib]" line: calls, MB in / out, ms in inflate, busiest threads, by caller.
//                      With DM_ZLIB_FAST=0 the host calls the original inflate through the gate's bypass and times it.
//   DM_ZLIB_VERIFY=<n> (implies FAST) the first n streams (1 = 512) are also decoded by the game's own zlib on a shadow stream;
//                      every call compares return code, bytes consumed / produced and the output bytes ("[zlib] VERIFY ...").
//                      Calls with more than DM_ZLIB_VERIFY_MAX_MB (64) of output space end verification of that stream.
#ifdef DM_GAME_DOOM
#include "dm.h"
#include "hle.h"
#include <zlib-ng.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>

typedef struct {                       // zlib 1.2.3 z_stream, LP64
    const uint8_t *next_in; uint32_t avail_in; uint64_t total_in;
    uint8_t *next_out; uint32_t avail_out; uint64_t total_out;
    const char *msg; void *state;
    uint64_t zalloc, zfree, opaque;
    int32_t data_type; uint64_t adler, reserved;
} gz_stream;
_Static_assert(sizeof(gz_stream) == 0x70, "zlib 1.2.3 z_stream is 0x70 bytes on LP64");
_Static_assert(offsetof(gz_stream, avail_in) == 0x08 && offsetof(gz_stream, total_in) == 0x10 && offsetof(gz_stream, next_out) == 0x18 &&
               offsetof(gz_stream, avail_out) == 0x20 && offsetof(gz_stream, total_out) == 0x28 && offsetof(gz_stream, msg) == 0x30 &&
               offsetof(gz_stream, state) == 0x38 && offsetof(gz_stream, zalloc) == 0x40 && offsetof(gz_stream, data_type) == 0x58 &&
               offsetof(gz_stream, adler) == 0x60, "z_stream 1.2.3 layout");

enum { ZCTX_MAGIC = 0x5a4e4731 };      // "1GNZ"
typedef struct {
    uint32_t magic; int32_t wbits;     // windowBits as passed (negative = raw)
    uint64_t seq;                      // stream number (init order)
    zng_stream z;
    gz_stream *shadow;                 // DM_ZLIB_VERIFY: the same stream decoded by the game's zlib (NULL = not verified)
    uint8_t *vbuf; size_t vcap;
    uint64_t vcalls, vout; int vlogged;
} zctx;

// Original guest functions = gate ret point + 20 (patch_nso.py ZLIB_GATES: the original first word, then b entry+4)
typedef int (*g_init2_fn)(gz_stream *, int, const char *, int);
typedef int (*g_init_fn)(gz_stream *, const char *, int);
typedef int (*g_inflate_fn)(gz_stream *, int);
typedef int (*g_one_fn)(gz_stream *);
static inline uintptr_t bypass(const uint64_t *regs) { return (uintptr_t)regs[30] + 20; }
static inline uint64_t guest_lr(const uint64_t *regs) { return regs[101]; }   // gate block: stp x0,x8 / stp x29,x30 below dm_gate's 800-byte frame

static int g_fast, g_stats, g_verify_left_init; static unsigned g_stats_sec = 5; static size_t g_verify_max;
static _Atomic int g_verify_left;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static uintptr_t g_main_base;

// ---------- statistics ----------
typedef struct { _Atomic uint64_t calls, in, out, ns; } zcnt;
enum { NCALLER = 16, NTHR = 32 };
static struct { _Atomic uint64_t lr; zcnt c; } g_caller[NCALLER];
static struct { _Atomic int tid; _Atomic uint64_t ns, out; } g_thr[NTHR];
static zcnt g_win, g_tot;
static _Atomic uint64_t g_streams, g_ends, g_resets, g_live, g_errors, g_vstreams, g_vdone, g_vcalls, g_vbytes, g_vbad, g_vskip, g_vns;

static const char *caller_name(uint64_t off) {
    switch (off) {
    case 0x1ef2880: return "ReadData";                 // idResourceContainer::ReadData, whole entry, Z_FINISH
    case 0x1efcbf4: return "BGL-last";                 // idBackgroundLoader decompress thread, flush = last block ? Z_FINISH : 0
    case 0x1efcd18: return "BGL";                      // same, ring wrap piece, Z_NO_FLUSH
    case 0x1fea07c: case 0x1fea15c: case 0x1ff2f84: case 0x1ff30b4: case 0x1fef308: return "png";
    default: return NULL;
    }
}
static zcnt *caller_slot(uint64_t lr) {
    for (int i = 0; i < NCALLER; i++) {
        uint64_t v = atomic_load_explicit(&g_caller[i].lr, memory_order_relaxed);
        if (v == lr) return &g_caller[i].c;
        if (!v) { uint64_t z = 0; if (atomic_compare_exchange_strong(&g_caller[i].lr, &z, lr) || z == lr) return &g_caller[i].c; }
    }
    return NULL;                                        // table full: counted in totals only
}
static void thr_add(uint64_t ns, uint64_t out) {
    int tid = (int)gettid();
    for (int i = 0; i < NTHR; i++) {
        int v = atomic_load_explicit(&g_thr[i].tid, memory_order_relaxed);
        if (!v) { int z = 0; if (!atomic_compare_exchange_strong(&g_thr[i].tid, &z, tid) && z != tid) continue; v = tid; }
        if (v == tid) { atomic_fetch_add_explicit(&g_thr[i].ns, ns, memory_order_relaxed); atomic_fetch_add_explicit(&g_thr[i].out, out, memory_order_relaxed); return; }
    }
}
static inline void cnt_add(zcnt *c, uint64_t in, uint64_t out, uint64_t ns) {
    atomic_fetch_add_explicit(&c->calls, 1, memory_order_relaxed); atomic_fetch_add_explicit(&c->in, in, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->out, out, memory_order_relaxed); atomic_fetch_add_explicit(&c->ns, ns, memory_order_relaxed);
}
static void stat_call(uint64_t lr, uint64_t in, uint64_t out, uint64_t ns) {
    cnt_add(&g_win, in, out, ns); cnt_add(&g_tot, in, out, ns);
    zcnt *c = caller_slot(lr); if (c) cnt_add(c, in, out, ns);
    thr_add(ns, out);
}
static void thr_comm(int tid, char *buf, size_t n) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid); buf[0] = 0;
    FILE *f = fopen(p, "r"); if (!f) return;
    if (fgets(buf, (int)n, f)) buf[strcspn(buf, "\n")] = 0;
    fclose(f);
}
#define MB(x) ((double)(x) / 1048576.0)
static void verify_summary(void) {
    LOG("[zlib] VERIFY %llu streams (%llu verified to the end), %llu calls, %.1f MB compared, %llu mismatches, %llu stopped (too big), reference decode %.0f ms",
        (unsigned long long)atomic_load(&g_vstreams), (unsigned long long)atomic_load(&g_vdone), (unsigned long long)atomic_load(&g_vcalls),
        MB(atomic_load(&g_vbytes)), (unsigned long long)atomic_load(&g_vbad), (unsigned long long)atomic_load(&g_vskip), atomic_load(&g_vns) / 1e6);
}
static void *stats_main(void *a) {
    (void)a;
    uint64_t last = now_ns();
    for (;;) {
        struct timespec ts = {g_stats_sec, 0}; nanosleep(&ts, NULL);
        uint64_t now = now_ns(); double secs = (now - last) / 1e9; last = now;
        uint64_t calls = atomic_exchange(&g_win.calls, 0), in = atomic_exchange(&g_win.in, 0), out = atomic_exchange(&g_win.out, 0), ns = atomic_exchange(&g_win.ns, 0);
        // busiest threads in this window (always drained, so an idle window does not carry into the next)
        int tid[3] = {0}; uint64_t tns[3] = {0}, tout[3] = {0};
        for (int i = 0; i < NTHR; i++) {
            int t = atomic_load(&g_thr[i].tid); if (!t) continue;
            uint64_t n = atomic_exchange(&g_thr[i].ns, 0), o = atomic_exchange(&g_thr[i].out, 0);
            for (int k = 0; k < 3; k++) if (n > tns[k]) { for (int j = 2; j > k; j--) { tid[j] = tid[j - 1]; tns[j] = tns[j - 1]; tout[j] = tout[j - 1]; } tid[k] = t; tns[k] = n; tout[k] = o; break; }
        }
        if (!calls) continue;
        char thr[256]; int p = 0; thr[0] = 0;
        for (int k = 0; k < 3 && tid[k]; k++) {
            char nm[32]; thr_comm(tid[k], nm, sizeof nm);
            p += snprintf(thr + p, sizeof thr - p, "%s%d %s %.0f ms (%.0f%%) %.1f MB", k ? ", " : "", tid[k], nm, tns[k] / 1e6, tns[k] / 1e7 / secs, MB(tout[k]));
            if (p >= (int)sizeof thr) break;
        }
        LOG("[zlib] %.1fs %s: %llu calls, in %.1f MB, out %.1f MB, inflate %.0f ms (%.0f%% of one core), %.0f MB/s out | streams %llu live %llu | threads: %s",
            secs, g_fast ? "zlib-ng" : "game zlib 1.2.3", (unsigned long long)calls, MB(in), MB(out), ns / 1e6, ns / 1e7 / secs,
            ns ? MB(out) / (ns / 1e9) : 0.0, (unsigned long long)atomic_load(&g_streams), (unsigned long long)atomic_load(&g_live), thr);
        char by[1024]; p = 0; by[0] = 0;
        for (int i = 0; i < NCALLER; i++) {
            uint64_t lr = atomic_load(&g_caller[i].lr); if (!lr) break;
            uint64_t c = atomic_load(&g_caller[i].c.calls); if (!c) continue;
            uint64_t off = lr - g_main_base; const char *nm = caller_name(off);
            char at[48]; if (nm) snprintf(at, sizeof at, "%s", nm); else snprintf(at, sizeof at, "main+%#llx", (unsigned long long)off);
            p += snprintf(by + p, sizeof by - p, "%s%s %llu/%.1f MB/%.0f ms", p ? ", " : "", at, (unsigned long long)c,
                          MB(atomic_load(&g_caller[i].c.out)), atomic_load(&g_caller[i].c.ns) / 1e6);
            if (p >= (int)sizeof by) break;
        }
        LOG("[zlib] total %llu calls, in %.1f MB, out %.1f MB, inflate %.0f ms, errors %llu | by caller (calls/out/ms): %s",
            (unsigned long long)atomic_load(&g_tot.calls), MB(atomic_load(&g_tot.in)), MB(atomic_load(&g_tot.out)), atomic_load(&g_tot.ns) / 1e6,
            (unsigned long long)atomic_load(&g_errors), by);
        if (g_verify_left_init) verify_summary();
    }
    return NULL;
}

static void cfg_init(void) {
    const char *e;
    g_fast = (e = getenv("DM_ZLIB_FAST")) ? atoi(e) > 0 : 1;   // default on since the 30.10 phone test (4x faster, 0 mismatches)
    g_stats = (e = getenv("DM_ZLIB_STATS")) && atoi(e) > 0;
    if ((e = getenv("DM_ZLIB_STATS_SEC")) && atoi(e) > 0) g_stats_sec = (unsigned)atoi(e);
    if ((e = getenv("DM_ZLIB_VERIFY")) && atoi(e) > 0) { g_verify_left_init = atoi(e) == 1 ? 512 : atoi(e); g_fast = 1; }
    atomic_store(&g_verify_left, g_verify_left_init);
    g_verify_max = (size_t)(((e = getenv("DM_ZLIB_VERIFY_MAX_MB")) && atoi(e) > 0) ? atoi(e) : 64) << 20;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main_base = (uintptr_t)g_mod[i].base;
    if (g_stats) { pthread_t t; if (!pthread_create(&t, NULL, stats_main, NULL)) pthread_detach(t); }
}
static inline void cfg(void) { pthread_once(&g_once, cfg_init); }

void dm_zlib_init(void) {                              // main.c, after the modules are loaded
    cfg();
    int have = gate_svc_present(SVC_HOST_ZLIB_INFLATE) && gate_svc_present(SVC_HOST_ZLIB_INIT2) && gate_svc_present(SVC_HOST_ZLIB_INIT) &&
               gate_svc_present(SVC_HOST_ZLIB_END) && gate_svc_present(SVC_HOST_ZLIB_RESET);
    if (g_fast || g_stats || g_verify_left_init || !have)
        LOG("[zlib] inflate gates %s; DM_ZLIB_FAST=%d (%s) DM_ZLIB_STATS=%d DM_ZLIB_VERIFY=%d", have ? "present" : "MISSING (main.patched.bin without ZLIB_GATES: switches have no effect)",
            g_fast, g_fast ? "zlib-ng " ZLIBNG_VERSION : "game zlib 1.2.3", g_stats, g_verify_left_init);
}

// ---------- host stream ----------
static zctx *ctx_of(const gz_stream *s) {
    zctx *c = s ? (zctx *)s->state : NULL;
    if (!c) return NULL;
    if (c->magic != ZCTX_MAGIC) { static int said; if (!said++) LOG("[zlib] stream %p: state %p is not a host stream (Z_STREAM_ERROR)", (void *)s, (void *)c); return NULL; }
    return c;
}
static void to_host(zctx *c, const gz_stream *s) {
    c->z.next_in = s->next_in; c->z.avail_in = s->avail_in; c->z.total_in = s->total_in;
    c->z.next_out = s->next_out; c->z.avail_out = s->avail_out; c->z.total_out = s->total_out;
}
static void to_guest(const zctx *c, gz_stream *s) {
    s->next_in = c->z.next_in; s->avail_in = c->z.avail_in; s->total_in = c->z.total_in;
    s->next_out = c->z.next_out; s->avail_out = c->z.avail_out; s->total_out = c->z.total_out;
    s->msg = c->z.msg; s->data_type = c->z.data_type;
    if (c->wbits > 0) s->adler = c->z.adler;           // raw streams: 1.2.3 leaves adler at 1 (set by init / reset)
}
// 1.2.3 inflateInit2_: version / size check first (Z_VERSION_ERROR), then strm, then windowBits 8..15 after the wrapper bits
static int host_init(gz_stream *s, int wbits, const char *version, int size, uint64_t *regs, int init2) {
    if (!version || version[0] != '1' || size != (int)sizeof(gz_stream)) return Z_VERSION_ERROR;
    if (!s) return Z_STREAM_ERROR;
    s->msg = NULL;
    int w = wbits < 0 ? -wbits : wbits < 48 ? (wbits & 15) : wbits;
    if (w < 8 || w > 15) { s->state = NULL; return Z_STREAM_ERROR; }
    zctx *c = calloc(1, sizeof *c);
    if (!c) return Z_MEM_ERROR;
    c->magic = ZCTX_MAGIC; c->wbits = wbits;
    int r = zng_inflateInit2(&c->z, wbits);
    if (r != Z_OK) { free(c); s->state = NULL; return r; }
    c->seq = atomic_fetch_add(&g_streams, 1); atomic_fetch_add(&g_live, 1);
    s->state = c; s->total_in = s->total_out = 0; s->adler = 1;
    if (atomic_load_explicit(&g_verify_left, memory_order_relaxed) > 0 && atomic_fetch_sub(&g_verify_left, 1) > 0) {
        gz_stream *sh = calloc(1, sizeof *sh);
        if (sh) {
            sh->zalloc = s->zalloc; sh->zfree = s->zfree; sh->opaque = s->opaque;   // the caller's allocator, as the game would
            int gr = init2 ? ((g_init2_fn)bypass(regs))(sh, wbits, version, size) : ((g_init_fn)bypass(regs))(sh, version, size);
            if (gr != Z_OK) { LOG("[zlib] VERIFY stream %llu: game inflateInit returned %d, host 0", (unsigned long long)c->seq, gr); atomic_fetch_add(&g_vbad, 1); free(sh); }
            else { c->shadow = sh; atomic_fetch_add(&g_vstreams, 1); }
        }
    }
    return Z_OK;
}
static void shadow_drop(zctx *c, uint64_t *regs_end) {
    if (!c->shadow) return;
    if (regs_end) ((g_one_fn)bypass(regs_end))(c->shadow);   // inside the inflateEnd gate: the game's inflateEnd frees its state
    // otherwise the game-side state of the shadow is leaked on purpose (only after a mismatch / oversize call, a few KB)
    free(c->shadow); c->shadow = NULL; free(c->vbuf); c->vbuf = NULL; c->vcap = 0;
}
// Same call on the game's zlib, output into vbuf; compare with what the host just did
static void verify_call(zctx *c, const gz_stream *before, const gz_stream *after, int flush, int hr, uint64_t *regs) {
    gz_stream *sh = c->shadow; size_t cap = before->avail_out;
    if (cap > g_verify_max) { atomic_fetch_add(&g_vskip, 1); shadow_drop(c, NULL); return; }
    if (cap > c->vcap) { free(c->vbuf); c->vcap = cap > 65536 ? cap : 65536; if (!(c->vbuf = malloc(c->vcap))) { c->vcap = 0; shadow_drop(c, NULL); return; } }
    sh->next_in = before->next_in; sh->avail_in = before->avail_in; sh->next_out = c->vbuf; sh->avail_out = (uint32_t)cap;
    uint64_t t0 = now_ns();
    int gr = ((g_inflate_fn)bypass(regs))(sh, flush);
    atomic_fetch_add(&g_vns, now_ns() - t0);
    size_t hin = before->avail_in - after->avail_in, hout = before->avail_out - after->avail_out;
    size_t gin = before->avail_in - sh->avail_in, gout = cap - sh->avail_out;
    // per call: return code, bytes consumed / produced, output bytes (totals only logged: a caller may rewrite its total_out)
    int bad = gr != hr || gin != hin || gout != hout || (hout && memcmp(c->vbuf, before->next_out, hout));
    atomic_fetch_add(&g_vcalls, 1); atomic_fetch_add(&g_vbytes, hout); c->vcalls++; c->vout += hout;
    if (bad) {
        size_t at = 0; while (at < hout && at < gout && c->vbuf[at] == before->next_out[at]) at++;
        LOG("[zlib] VERIFY MISMATCH stream %llu call %llu flush %d: ret host %d game %d, in host %zu game %zu, out host %zu game %zu, total_out host %llu game %llu, first differing byte %zu",
            (unsigned long long)c->seq, (unsigned long long)c->vcalls, flush, hr, gr, hin, gin, hout, gout,
            (unsigned long long)after->total_out, (unsigned long long)sh->total_out, at);
        atomic_fetch_add(&g_vbad, 1); shadow_drop(c, NULL);
    } else if (hr == Z_STREAM_END && c->seq < 8 && !c->vlogged++)
        LOG("[zlib] VERIFY stream %llu ok: %llu calls, %llu bytes", (unsigned long long)c->seq, (unsigned long long)c->vcalls, (unsigned long long)c->vout);
}

// ---------- gates (svc 0x90..0x94) ----------
int dm_zlib_host_call(int num, uint64_t *regs) {
    if (num < SVC_HOST_ZLIB_INIT2 || num > SVC_HOST_ZLIB_RESET) return 0;
    cfg();
    regs[8] = 0;                                       // default: the gate runs the original zlib
    gz_stream *s = (gz_stream *)regs[0];
    if (!g_fast) {
        if (g_stats && (num == SVC_HOST_ZLIB_INIT2 || num == SVC_HOST_ZLIB_INIT)) { atomic_fetch_add(&g_streams, 1); atomic_fetch_add(&g_live, 1); }
        if (g_stats && num == SVC_HOST_ZLIB_END && s && s->state) atomic_fetch_sub(&g_live, 1);
        if (num != SVC_HOST_ZLIB_INFLATE || !g_stats || !s) return 1;
        // DM_ZLIB_STATS on the game's own inflate: call it through the bypass and time it
        uint32_t ai = s->avail_in, ao = s->avail_out;
        uint64_t t0 = now_ns();
        int r = ((g_inflate_fn)bypass(regs))(s, (int)regs[1]);
        uint64_t dt = now_ns() - t0;
        if (r < 0 && r != Z_BUF_ERROR) atomic_fetch_add(&g_errors, 1);
        stat_call(guest_lr(regs), ai - s->avail_in, ao - s->avail_out, dt);
        regs[0] = (uint64_t)(int64_t)r; regs[8] = 1;
        return 1;
    }
    int r;
    switch (num) {
    case SVC_HOST_ZLIB_INIT2: r = host_init(s, (int)regs[1], (const char *)regs[2], (int)regs[3], regs, 1); break;
    case SVC_HOST_ZLIB_INIT:  r = host_init(s, 15, (const char *)regs[1], (int)regs[2], regs, 0); break;
    case SVC_HOST_ZLIB_INFLATE: {
        zctx *c = ctx_of(s);
        if (!c) { r = Z_STREAM_ERROR; break; }
        int flush = (int)regs[1];
        gz_stream before = *s;
        to_host(c, s);
        uint64_t t0 = now_ns();
        r = zng_inflate(&c->z, flush);
        uint64_t dt = now_ns() - t0;
        to_guest(c, s);
        if (r < 0 && r != Z_BUF_ERROR) atomic_fetch_add(&g_errors, 1);
        if (g_stats) stat_call(guest_lr(regs), before.avail_in - s->avail_in, before.avail_out - s->avail_out, dt);
        if (c->shadow) verify_call(c, &before, s, flush, r, regs);
        break;
    }
    case SVC_HOST_ZLIB_END: {                          // 1.2.3: frees window + state, state = NULL, Z_OK
        zctx *c = ctx_of(s);
        if (!c) { r = Z_STREAM_ERROR; break; }
        if (c->shadow) {                               // verified to the end; without DM_ZLIB_STATS a summary every 64 streams and at the last one
            uint64_t d = atomic_fetch_add(&g_vdone, 1) + 1;
            if (!g_stats && (d % 64 == 0 || c->seq + 1 == (uint64_t)g_verify_left_init)) verify_summary();
        }
        shadow_drop(c, regs);
        zng_inflateEnd(&c->z); c->magic = 0; free(c);
        s->state = NULL; atomic_fetch_add(&g_ends, 1); atomic_fetch_sub(&g_live, 1);
        r = Z_OK; break;
    }
    case SVC_HOST_ZLIB_RESET: {                        // 1.2.3: total_in = total_out = 0, msg = NULL, adler = 1, mode HEAD, window kept
        zctx *c = ctx_of(s);
        if (!c) { r = Z_STREAM_ERROR; break; }
        r = zng_inflateReset(&c->z);
        s->total_in = s->total_out = 0; s->msg = NULL; s->adler = 1;
        atomic_fetch_add(&g_resets, 1);
        if (c->shadow) {
            int gr = ((g_one_fn)bypass(regs))(c->shadow);
            if (gr != r) { LOG("[zlib] VERIFY MISMATCH stream %llu: inflateReset host %d game %d", (unsigned long long)c->seq, r, gr); atomic_fetch_add(&g_vbad, 1); shadow_drop(c, NULL); }
        }
        break;
    }
    default: return 0;
    }
    regs[0] = (uint64_t)(int64_t)r; regs[8] = 1;
    return 1;
}
#endif
