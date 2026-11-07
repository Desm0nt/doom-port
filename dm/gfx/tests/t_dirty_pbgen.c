// [r4] Host test (Linux, e.g. from WSL: make t_dirty_pbgen && ./t_dirty_pbgen) of DM_DIRTY_PROTECT_BGEN in nvn_dirty.c:
// nvn_dirty_protect_gen (window walk above the cookie's floor, verify walk, kept floor while an unprotect is in flight) and
// nvn_dirty_since_bits (same answers as nvn_dirty_since per page); then writer threads fault on random pages while the main
// thread re-protects with verification: the verify walk must never find a page the windows skipped.
#define _GNU_SOURCE
#include "../nvn/nvn_dirty.c"
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>

static int (*g_hook)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_crash_note_register(void (*fn)(void)) { (void)fn; }   // issue #21: the crash report line (crash.c)
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    (void)sig; ucontext_t *uc = uc_; int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (g_hook && g_hook(si->si_addr, is_write, uc)) return;
    fprintf(stderr, "real crash at %p write=%d\n", si->si_addr, is_write); _exit(99);
}
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } else printf("ok   %s\n", #c); } while (0)

enum { PG = 4096, NPG = 512 };   // 8 windows of 64 pages
static uint8_t *g_mem;
static range_t *rng(void) { return find((uintptr_t)g_mem); }
static int all_prot(void) { range_t *r = rng(); for (int i = 0; i < NPG; i++) if (atomic_load(&r->state[i]) != PG_PROT) return 0; return 1; }
static volatile int g_stop, g_pause_us;
static void *writer(void *a) {
    unsigned seed = (unsigned)(uintptr_t)a; uint64_t n = 0;
    while (!g_stop) { unsigned pg = rand_r(&seed) % NPG; g_mem[(size_t)pg * PG + (rand_r(&seed) % PG)] = (uint8_t)n++;
        if (g_pause_us) usleep(g_pause_us); else if ((n & 63) == 0) sched_yield(); }
    return NULL;
}
// writers at a game-like rate (a pause after each write): the floor must advance in most calls and nothing may be missed
static void calm_stress(nvn_dirty_cookie *ck, nvn_dirty_pstat *acc) {
    g_stop = 0; g_pause_us = 20; pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, writer, (void *)(uintptr_t)(i * 104729 + 3));
    for (int k = 0; k < 5000; k++) { nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, ck, 3, acc); usleep(50); }
    g_stop = 1; for (int i = 0; i < 4; i++) pthread_join(t[i], NULL); g_pause_us = 0;
}

int main(void) {
    setenv("DM_DIRTY_PROTECT_BGEN", "3", 1);
    struct sigaction sa = {0}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSEGV, &sa, NULL);
    g_mem = mmap(NULL, (size_t)NPG * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    nvn_dirty_add_range(g_mem, (size_t)NPG * PG);
    CHECK(nvn_dirty_pbgen_mode() == 3);
    CHECK(rng() && rng()->bgen);

    // first call: zero cookie -> every page, floor set (nothing in flight)
    nvn_dirty_cookie ck = {0}; nvn_dirty_pstat st = {0};
    nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 1, &st);
    CHECK(st.full == 1 && st.pages == NPG && st.windows == 0);
    CHECK(ck.floor == nvn_dirty_now() && ck.floor != 0);
    CHECK(all_prot());

    // writes in windows 2 and 5 -> only those two windows are walked, and they are protected again
    uint64_t f0, p0; nvn_dirty_stats(&f0, &p0);
    g_mem[(size_t)(2 * 64 + 3) * PG] = 1; g_mem[(size_t)(2 * 64 + 40) * PG] = 2; g_mem[(size_t)(5 * 64 + 63) * PG + 7] = 3;
    uint64_t f1; nvn_dirty_stats(&f1, &p0); CHECK(f1 - f0 == 3);
    memset(&st, 0, sizeof st);
    nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 3, &st);
    CHECK(st.full == 0 && st.windows == 2 && st.pages == 128 && st.missed == 0);
    CHECK(all_prot());
    g_mem[(size_t)(2 * 64 + 3) * PG] = 9; uint64_t f2; nvn_dirty_stats(&f2, &p0); CHECK(f2 - f1 == 1);   // write-protected again: faults again

    // nothing written -> nothing walked
    nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 1, &st);
    memset(&st, 0, sizeof st); nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 1, &st);
    CHECK(st.windows == 0 && st.pages == 0 && all_prot());

    // the verify walk notices a page the windows could not see (made writable behind the tracker's back, old generation)
    { range_t *r = rng(); size_t pg = 7 * 64 + 1; mprotect(g_mem + pg * PG, PG, PROT_READ | PROT_WRITE); atomic_store(&r->state[pg], PG_NONE);
      memset(&st, 0, sizeof st); nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 3, &st);
      CHECK(st.missed == 1 && st.windows == 0 && all_prot()); }

    // an unprotect in flight while the walk starts: the floor is kept, not advanced
    { const uint64_t old = ck.floor; g_mem[(size_t)(1 * 64) * PG] = 5; atomic_fetch_add(&g_unprot_inflight, 1);
      memset(&st, 0, sizeof st); nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 1, &st); atomic_fetch_sub(&g_unprot_inflight, 1);
      CHECK(st.kept == 1 && ck.floor == old && st.windows == 1 && all_prot()); }

    // another range (or a re-created pool) -> full walk
    { nvn_dirty_cookie c2 = ck; memset(&st, 0, sizeof st);
      nvn_dirty_protect_gen(g_mem, (size_t)(NPG - 1) * PG, &c2, 1, &st); CHECK(st.full == 1 && st.pages == NPG - 1); }

    // since_bits == nvn_dirty_since page by page
    { const uint64_t stamp = nvn_dirty_now(); g_mem[(size_t)10 * PG] = 1; g_mem[(size_t)300 * PG + 99] = 1; g_mem[(size_t)511 * PG] = 1;
      uint64_t bits[NPG / 64], w = 0; size_t np = nvn_dirty_since_bits(g_mem + 5, (size_t)NPG * PG - 5, stamp, bits, NPG / 64, &w);
      int same = np == NPG, set = 0;
      for (int i = 0; i < NPG; i++) { int b = (bits[i >> 6] >> (i & 63)) & 1; set += b; if (b != nvn_dirty_since(g_mem + (size_t)i * PG, PG, stamp)) same = 0; }
      CHECK(same && set == 3 && w == 3);
      CHECK(nvn_dirty_since_bits(g_mem, (size_t)NPG * PG, stamp, bits, 1, &w) == 0);   // bitmap too small
      nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 1, &st); }

    // stress: 4 writers fault on random pages while we re-protect with verification
    { pthread_t t[4]; for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, writer, (void *)(uintptr_t)(i * 7919 + 1));
      nvn_dirty_pstat acc = {0};
      for (int k = 0; k < 20000; k++) { nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 3, &acc); if ((k & 7) == 0) sched_yield(); }
      g_stop = 1; for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
      printf("stress calls=%llu full=%llu windows=%llu pages=%llu kept=%llu missed=%llu\n", (unsigned long long)acc.calls, (unsigned long long)acc.full,
             (unsigned long long)acc.windows, (unsigned long long)acc.pages, (unsigned long long)acc.kept, (unsigned long long)acc.missed);
      CHECK(acc.missed == 0);
      CHECK(acc.windows > 0);
      nvn_dirty_pstat calm = {0}; calm_stress(&ck, &calm);
      printf("calm calls=%llu windows=%llu pages=%llu kept=%llu missed=%llu\n", (unsigned long long)calm.calls, (unsigned long long)calm.windows,
             (unsigned long long)calm.pages, (unsigned long long)calm.kept, (unsigned long long)calm.missed);
      CHECK(calm.missed == 0);
      CHECK(calm.kept * 2 < calm.calls);   // the floor advanced in most calls
      // writers stopped, nothing in flight: two calls leave every page protected
      memset(&st, 0, sizeof st); nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 3, &st); nvn_dirty_protect_gen(g_mem, (size_t)NPG * PG, &ck, 3, &st);
      CHECK(st.missed == 0 && all_prot()); }

    puts(g_fail ? "FAILED" : "ALL OK");
    return g_fail;
}
