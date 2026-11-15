// [zc3mode] Host test (Linux, e.g. from WSL: make t_dirty_zc3 && ./t_dirty_zc3 && ./t_dirty_zc3 nowin) of nvn_dirty_take_written, the
// dirty-page tracker side of zero-copy mode 3 (BUGS_AND_FIXES 2.61). A memfd mapping (re-mappable from its fd, like a dma-buf chunk)
// stands in for a zero-copy chunk; a plain array stands in for the memory the GPU reads, and "cleaning" a page (DC CVAC on the device)
// copies it there. The contract: after take + clean, every write that happened before the take is in "memory".
//   1. basics: the first call is full and protects every page; no writes -> nothing; one write -> exactly that page, protected again;
//      a page written and then protected by someone else (texture re-upload) still counts; a re-registered pool -> full again.
//   2. stress: writer threads (plain stores into random pages), a "texture" thread protecting random ranges (nvn_dirty_protect),
//      a host pre-write thread (prep_write, like hle_fs.c file reads), the coalescer re-mapping the chunks, and the taker (= the
//      record thread before vkQueueSubmit) cleaning concurrently; writers stop every round, then one take + clean must leave
//      "memory" equal to the mapping, byte for byte. Rounds with the writers stopped mid-way check the same at many points.
//   "nowin" runs it all with DM_DIRTY_PROTECT_BGEN=0 (no window summaries: full walks of states and generations).
#define _GNU_SOURCE
#include "../nvn/nvn_dirty.c"
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>

static int (*g_hook)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_crash_note_register(void (*fn)(void)) { (void)fn; }
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    (void)sig; ucontext_t *uc = uc_; int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (g_hook && g_hook(si->si_addr, is_write, uc)) return;
    fprintf(stderr, "real crash at %p write=%d\n", si->si_addr, is_write); _exit(99);
}
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } else printf("ok   %s\n", #c); } while (0)

enum { PG = 4096, CPG = 1024, NCH = 2, NPG = CPG * NCH };   // two 4 MB chunks (16 windows each)
static uint8_t *g_mem, *g_dram;
static int g_fd;
static nvn_dirty_cookie g_ck[NCH]; static nvn_dirty_pstat g_st;
static pthread_mutex_t g_take_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_cleaned_pages, g_full_calls;
// take + clean one chunk (the engine's maint_clean for one chunk); returns the pages cleaned
static size_t take_clean(int c, int *full_out) {
    uint64_t bits[CPG / 64]; int full = 0; uint8_t *lo = g_mem + (size_t)c * CPG * PG;
    size_t np = nvn_dirty_take_written(lo, (size_t)CPG * PG, &g_ck[c], bits, CPG / 64, &full, &g_st);
    if (full_out) *full_out = full;
    if (!np) return 0;
    size_t n = 0;
    for (size_t i = 0; i < np; i++) if (full || (bits[i >> 6] >> (i & 63) & 1)) { memcpy(g_dram + (size_t)c * CPG * PG + i * PG, lo + i * PG, PG); n++; }
    g_cleaned_pages += n; g_full_calls += full;
    return n;
}
static void take_all(void) { pthread_mutex_lock(&g_take_mu); for (int c = 0; c < NCH; c++) take_clean(c, NULL); pthread_mutex_unlock(&g_take_mu); }
static size_t diff_pages(void) { size_t d = 0; for (int i = 0; i < NPG; i++) if (memcmp(g_mem + (size_t)i * PG, g_dram + (size_t)i * PG, PG)) d++; return d; }
static range_t *R(void) { return find((uintptr_t)g_mem); }
static int all_prot(int c) { for (int i = 0; i < CPG; i++) if (atomic_load(&R()->state[c * CPG + i]) != PG_PROT) return 0; return 1; }

static volatile int g_stop;
static _Atomic uint64_t g_seq = 1;
static void *writer(void *a) {
    unsigned seed = (unsigned)(uintptr_t)a;
    while (!g_stop) { unsigned pg = rand_r(&seed) % NPG; uint64_t v = atomic_fetch_add(&g_seq, 1);
        size_t off = (size_t)pg * PG + (rand_r(&seed) % (PG / 8)) * 8; memcpy(g_mem + off, &v, 8);
        if ((v & 15) == 0) sched_yield(); }
    return NULL;
}
static void *texture(void *a) {   // another tracker user: protects random ranges (texture re-uploads), CAS NONE -> BUSY -> PROT
    unsigned seed = (unsigned)(uintptr_t)a;
    while (!g_stop) { unsigned pg = rand_r(&seed) % NPG, n = 1 + rand_r(&seed) % 16; if (pg + n > NPG) n = NPG - pg;
        nvn_dirty_protect(g_mem + (size_t)pg * PG, (size_t)n * PG); usleep(50); }
    return NULL;
}
static void *hostw(void *a) {   // host writes (file reads into guest memory): prep_write, then the store
    unsigned seed = (unsigned)(uintptr_t)a;
    while (!g_stop) { unsigned pg = rand_r(&seed) % NPG, n = 1 + rand_r(&seed) % 4; if (pg + n > NPG) n = NPG - pg;
        uint8_t *p = g_mem + (size_t)pg * PG + 64; size_t len = (size_t)n * PG - 128; g_prep(p, len);
        uint64_t v = atomic_fetch_add(&g_seq, 1); for (size_t k = 0; k + 8 <= len; k += 512) memcpy(p + k, &v, 8); usleep(100); }
    return NULL;
}
static void *coal(void *a) {   // the coalescer re-maps the chunks now and then
    (void)a;
    while (!g_stop) { for (int c = 0; c < NCH; c++) nvn_dirty_coalesce_at(g_mem + (size_t)c * CPG * PG); usleep(2000); }
    return NULL;
}
static void *taker(void *a) { (void)a; while (!g_stop) { take_all(); usleep(30); } return NULL; }

int main(int argc, char **argv) {
    const int nowin = argc > 1 && !strcmp(argv[1], "nowin");
    setenv("DM_DIRTY_PROTECT_BGEN", nowin ? "0" : "1", 1);
    setenv("DM_DIRTY_COALESCE_THREAD", "0", 1);
    printf("t_dirty_zc3: %s\n", nowin ? "DM_DIRTY_PROTECT_BGEN=0 (full walks)" : "window walks");
    struct sigaction sa = {0}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSEGV, &sa, NULL);
    g_fd = (int)syscall(SYS_memfd_create, "zc3", 0); CHECK(g_fd >= 0); CHECK(ftruncate(g_fd, (off_t)NPG * PG) == 0);
    g_mem = mmap(NULL, (size_t)NPG * PG, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    g_dram = calloc(NPG, PG);
    for (int i = 0; i < NPG; i++) g_mem[(size_t)i * PG + 7] = (uint8_t)(i * 13 + 1);   // written before the tracker knows the pool
    nvn_dirty_add_range(g_mem, (size_t)NPG * PG);
    for (int c = 0; c < NCH; c++) nvn_dirty_zc_chunk(g_mem + (size_t)c * CPG * PG, (size_t)CPG * PG, dup(g_fd), (uint64_t)c * CPG * PG);
    CHECK(nvn_dirty_pbgen_mode() == (nowin ? 0 : 1));

    // ---- 1. basics
    int full = -1;
    CHECK(take_clean(0, &full) == CPG && full == 1);
    CHECK(all_prot(0));
    CHECK(take_clean(0, &full) == 0 && full == 0);   // nothing written since
    g_mem[5 * PG + 100] = 0x5a;                         // faults, page 5 writable again
    CHECK(atomic_load(&R()->state[5]) == PG_NONE);
    { uint64_t bits[CPG / 64]; int f = 0; size_t np = nvn_dirty_take_written(g_mem, (size_t)CPG * PG, &g_ck[0], bits, CPG / 64, &f, &g_st);
      int only5 = np == CPG && f == 0 && bits[0] == (1ull << 5); for (int w = 1; w < CPG / 64; w++) only5 = only5 && !bits[w];
      CHECK(only5); CHECK(atomic_load(&R()->state[5]) == PG_PROT);
      memcpy(g_dram + 5 * PG, g_mem + 5 * PG, PG); }
    g_mem[700 * PG] = 1; nvn_dirty_protect(g_mem + 700 * PG, PG);   // written, then protected by a texture re-upload before the take
    CHECK(atomic_load(&R()->state[700]) == PG_PROT);
    CHECK(take_clean(0, &full) == 1 && full == 0);
    CHECK(take_clean(1, &full) == CPG && full == 1);
    CHECK(diff_pages() == 0);
    // a page the tracker never protected because the pool was re-registered: full again
    nvn_dirty_remove_range(g_mem); nvn_dirty_add_range(g_mem, (size_t)NPG * PG);
    g_mem[3 * PG] = 9;   // writable now (removal unprotected everything), no fault, no generation
    CHECK(take_clean(0, &full) == CPG && full == 1 && all_prot(0));
    CHECK(take_clean(1, &full) == CPG && full == 1 && all_prot(1));
    CHECK(diff_pages() == 0);
    // nwords too small / outside a tracked range -> 0
    { uint64_t b[1]; int f; CHECK(nvn_dirty_take_written(g_mem, (size_t)CPG * PG, &g_ck[0], b, 1, &f, &g_st) == 0);
      static uint8_t other[2 * PG]; CHECK(nvn_dirty_take_written(other, PG, &g_ck[0], b, 1, &f, &g_st) == 0); g_ck[0] = (nvn_dirty_cookie){0}; take_clean(0, NULL); }

    // ---- 2. stress
    size_t bad_rounds = 0, worst = 0; uint64_t calls0 = g_st.calls, kept0 = g_st.kept;
    for (int round = 0; round < 60; round++) {
        g_stop = 0; pthread_t t[8]; int nt = 0;
        for (int i = 0; i < 3; i++) pthread_create(&t[nt++], NULL, writer, (void *)(uintptr_t)(round * 7919 + i * 104729 + 3));
        pthread_create(&t[nt++], NULL, texture, (void *)(uintptr_t)(round * 31 + 5));
        pthread_create(&t[nt++], NULL, hostw, (void *)(uintptr_t)(round * 37 + 11));
        if (round % 3 == 0) pthread_create(&t[nt++], NULL, coal, NULL);
        pthread_create(&t[nt++], NULL, taker, NULL);
        usleep(round % 5 == 0 ? 200000 : 30000);
        g_stop = 1; for (int i = 0; i < nt; i++) pthread_join(t[i], NULL);
        take_all();   // the submission after the last write: everything written must now be in memory
        size_t d = diff_pages(); if (d) { bad_rounds++; if (d > worst) worst = d; printf("  round %d: %zu stale page(s)\n", round, d); memcpy(g_dram, g_mem, (size_t)NPG * PG); }
    }
    uint64_t faults, preps; nvn_dirty_stats(&faults, &preps); int ch; uint64_t vmas, coalesces, aborts, ms; nvn_dirty_coalesce_stats(&ch, &vmas, &coalesces, &aborts, &ms);
    printf("  stress: %llu takes (%llu kept the floor), %llu pages cleaned, %llu faults, %llu host pre-writes, %llu coalesces, %llu windows visited\n",
           (unsigned long long)(g_st.calls - calls0), (unsigned long long)(g_st.kept - kept0), (unsigned long long)g_cleaned_pages,
           (unsigned long long)faults, (unsigned long long)preps, (unsigned long long)coalesces, (unsigned long long)g_st.windows);
    CHECK(bad_rounds == 0);
    CHECK(g_seq > 100000);   // the writers really ran
    printf(g_fail ? "t_dirty_zc3: FAILED\n" : "t_dirty_zc3: all passed\n");
    return g_fail;
}
