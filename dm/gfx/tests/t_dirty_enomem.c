// [DOOM] Host test (Linux: make t_dirty_enomem) of issue #21 in nvn_dirty.c: mprotect failing with ENOMEM once the process has
// vm.max_map_count VMAs. A filler mapping with alternating page protections brings the real kernel to the limit (no mocks), then:
//   A. a write fault on a page inside a read-only run: the one-page unprotect needs a split and fails; the whole PROT run (whole
//      VMAs) is unprotected instead, the write lands, every page of the run is dirty;
//   B. a protect that needs a split fails: the pages stay writable and are NONE with a new generation (dirty, never clean+writable);
//   C. the widened run cannot be unprotected either (a BUSY neighbour inside the same VMA, no mergeable neighbour): the hook answers 0 (a real crash report,
//      not the old fault loop) and the page stays PROT;
//   D. below the limit again: the ordinary one-page path, no widening.
#define _GNU_SOURCE
#include "../nvn/nvn_dirty.c"
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

static int (*g_hook)(void *, int, void *);
static void (*g_prep)(void *, size_t);
static void (*g_note)(void);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_crash_note_register(void (*fn)(void)) { g_note = fn; }
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_; int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (g_hook && g_hook(si->si_addr, is_write, uc)) return;
    fprintf(stderr, "real crash at %p write=%d\n", si->si_addr, is_write); _exit(99);
}
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } else printf("ok   %s\n", #c); } while (0)

enum { PG = 4096, NPG = 64, NFILL = 90000 };
static uint8_t *g_mem, *g_fill; static size_t g_next = 1;
static int maps_now(void) {   // open/read only: no allocation (nothing may mmap while the process sits at the limit)
    char b[4096]; ssize_t n; int fd = open("/proc/self/maps", O_RDONLY), lines = 0; if (fd < 0) return -1;
    while ((n = read(fd, b, sizeof b)) > 0) for (ssize_t i = 0; i < n; i++) lines += b[i] == '\n';
    close(fd); return lines;
}
// Protect every other filler page (each one a VMA of its own) until three in a row fail with ENOMEM: the process is at the limit.
static void saturate(void) {
    int miss = 0;
    while (miss < 3 && g_next < NFILL) { if (mprotect(g_fill + g_next * PG, PG, PROT_READ)) { if (errno != ENOMEM) { printf("filler mprotect errno %d\n", errno); _exit(97); } miss++; } else miss = 0; g_next += 2; }
    if (g_next >= NFILL) { printf("filler too small\n"); _exit(96); }
}
static uint8_t st(int i) { return atomic_load(&g_r[0].state[i]); }
static int states(int lo, int hi, uint8_t want) { for (int i = lo; i <= hi; i++) if (st(i) != want) return 0; return 1; }

int main(void) {
    printf("t_dirty_enomem: vm.max_map_count test\n"); fflush(stdout);
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_ONSTACK}; sigaction(SIGSEGV, &sa, 0);
    // tracked pool: 64 pages between two PROT_NONE guards (so its ends are VMA boundaries, as a pool mapping's are)
    uint8_t *base = mmap(NULL, (NPG + 2) * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(base, PG, PROT_NONE); mprotect(base + (NPG + 1) * PG, PG, PROT_NONE);
    g_mem = base + PG; memset(g_mem, 0x11, NPG * PG);
    g_fill = mmap(NULL, (size_t)NFILL * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(g_fill != MAP_FAILED);
    nvn_dirty_add_range(g_mem, NPG * PG);
    nvn_dirty_protect(g_mem, 32 * PG);   // pages 0..31 read-only (one VMA), 32..63 writable
    uint64_t s0 = nvn_dirty_now();
    CHECK(states(0, 31, PG_PROT) && states(32, 63, PG_NONE));
    int before = maps_now();
    // ---- A. write fault inside the read-only run at the limit
    saturate(); int at = maps_now();
    printf("maps %d -> %d (filler pages used %zu)\n", before, at, g_next / 2);
    uint64_t f0, f0u, f0p, w0; int e0; nvn_dirty_mp_fail(&f0, &f0u, &f0p, &w0, &e0);
    volatile uint8_t *pa = g_mem + 10 * PG + 5; *pa = 0x5a;   // faults; the hook must widen
    uint64_t f1, f1u, f1p, w1; int e1; nvn_dirty_mp_fail(&f1, &f1u, &f1p, &w1, &e1);
    CHECK(*pa == 0x5a);
    CHECK(f1u == f0u + 1 && w1 == w0 + 1 && e1 == ENOMEM);
    CHECK(states(0, 31, PG_NONE));                 // the whole run was opened
    CHECK(nvn_dirty_since(g_mem, PG, s0) == 1);    // and counts as written (page 0, never touched)
    CHECK(nvn_dirty_since(g_mem + 31 * PG, PG, s0) == 1);
    // ---- B. a protect that needs a split at the limit
    saturate();
    uint64_t s1 = nvn_dirty_now(); uint64_t fa0, pr0; nvn_dirty_stats(&fa0, &pr0);
    nvn_dirty_protect(g_mem + 8 * PG, 8 * PG);
    uint64_t f2, f2u, f2p, w2; int e2; nvn_dirty_mp_fail(&f2, &f2u, &f2p, &w2, &e2);
    CHECK(f2p == f1p + 1);
    CHECK(states(8, 15, PG_NONE));                 // still writable, and the state says so
    CHECK(nvn_dirty_since(g_mem + 8 * PG, 8 * PG, s1) == 1);   // dirty: re-uploaded, never clean but writable
    g_mem[9 * PG] = 0x77; uint64_t fa1, pr1; nvn_dirty_stats(&fa1, &pr1);
    CHECK(g_mem[9 * PG] == 0x77 && fa1 == fa0);    // no fault: the page really is writable
    // ---- C. widening impossible: run 53..63 inside the VMA 52..63 (its other end is the guard, so the kernel cannot merge the
    //      change into a neighbour instead of splitting; dma-buf VMAs never merge at all), page 52 held BUSY by "another thread"
    for (int k = 0; k < 16; k++) munmap(g_fill + (1 + 2 * k) * PG, PG);   // free some VMAs so the protect below can split
    nvn_dirty_protect(g_mem + 52 * PG, 12 * PG);
    CHECK(states(52, 63, PG_PROT));
    atomic_store(&g_r[0].state[52], PG_BUSY);
    saturate();
    uint64_t f3, f3u, f3p, w3; int e3; nvn_dirty_mp_fail(&f3, &f3u, &f3p, &w3, &e3);
    int r = g_hook(g_mem + 58 * PG, 1, NULL);
    uint64_t f4, f4u, f4p, w4; int e4; nvn_dirty_mp_fail(&f4, &f4u, &f4p, &w4, &e4);
    CHECK(r == 0);                                 // a real crash report, not "handled" forever
    CHECK(states(53, 63, PG_PROT) && st(52) == PG_BUSY);
    CHECK(f4u == f3u + 1 && w4 == w3);
    if (g_note) g_note();                          // the crash report's counter line
    atomic_store(&g_r[0].state[52], PG_PROT);
    // ---- D. below the limit: the ordinary one-page unprotect
    munmap(g_fill, (size_t)NFILL * PG);
    printf("maps after releasing the filler: %d\n", maps_now());
    nvn_dirty_protect(g_mem + 40 * PG, 11 * PG);
    nvn_dirty_stats(&fa0, &pr0);
    volatile uint8_t *pd = g_mem + 45 * PG; *pd = 0x42;
    uint64_t f5, f5u, f5p, w5; int e5; nvn_dirty_mp_fail(&f5, &f5u, &f5p, &w5, &e5); nvn_dirty_stats(&fa1, &pr1);
    CHECK(*pd == 0x42 && fa1 == fa0 + 1 && f5 == f4 && w5 == w4);
    CHECK(st(45) == PG_NONE && st(44) == PG_PROT && st(46) == PG_PROT);
    g_prep(g_mem + 47 * PG, 16); memset(g_mem + 47 * PG, 1, 16); nvn_dirty_stats(&fa0, &pr0);
    CHECK(pr0 == pr1 + 1 && st(47) == PG_NONE);
    // ---- E. the VMA coalescer at the limit: a zero-copy chunk (a memfd stands in for the dma-buf) re-mapped in place while the
    //      process sits at vm.max_map_count (MAP_FIXED replaces whole VMAs, so it needs no split), then a write fault works
    setenv("DM_DIRTY_COALESCE_THREAD", "0", 1);
    int mfd = (int)syscall(SYS_memfd_create, "zc", 0); CHECK(mfd >= 0 && ftruncate(mfd, NPG * PG) == 0);
    uint8_t *res = mmap(NULL, (NPG + 2) * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *z = mmap(res + PG, NPG * PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mfd, 0); CHECK(z == res + PG);
    memset(z, 0x22, NPG * PG);
    nvn_dirty_add_range(z, NPG * PG); nvn_dirty_zc_chunk(z, NPG * PG, dup(mfd), 0);
    range_t *rz = find((uintptr_t)z);
    nvn_dirty_protect(z, NPG * PG);
    for (int i = 0; i < NPG; i += 4) z[(size_t)i * PG] = 1;   // single-page unprotects: the model counts the splits
    uint32_t zv = atomic_load(&g_zc[0].vmas); printf("chunk model %u VMAs\n", zv); CHECK(zv > 2);
    g_fill = mmap(NULL, (size_t)NFILL * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); g_next = 1;
    saturate(); printf("maps %d\n", maps_now());
    CHECK(nvn_dirty_coalesce_at(z) == 1);
    CHECK(atomic_load(&g_zc[0].vmas) == coal_minimal(&g_zc[0], rz));
    CHECK(atomic_load(&rz->state[0]) == PG_NONE && atomic_load(&rz->state[1]) == PG_PROT);
    CHECK(z[0] == 1 && z[1] == 0x22 && z[PG] == 0x22);   // content kept
    nvn_dirty_stats(&fa0, &pr0);
    z[(size_t)1 * PG + 9] = 0x33;                        // a protected page: faults and is opened (one page or the widened run)
    nvn_dirty_stats(&fa1, &pr1);
    CHECK(z[(size_t)PG + 9] == 0x33 && fa1 == fa0 + 1 && atomic_load(&rz->state[1]) == PG_NONE);
    munmap(g_fill, (size_t)NFILL * PG);
    printf(g_fail ? "FAILED\n" : "ALL OK\n");
    return g_fail;
}
