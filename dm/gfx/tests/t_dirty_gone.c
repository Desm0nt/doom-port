// [DOOM] Host test (Linux: make t_dirty_gone) of the DM_TEX_DISCARD page states in nvn_dirty.c: discard -> read fault restore (mremap) -> write fault ->
// dirty; host pre-write on a released page; begin refused after a write.
#define _GNU_SOURCE
#include "../nvn/nvn_dirty.c"
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

static int (*g_hook)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_; int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (g_hook && g_hook(si->si_addr, is_write, uc)) return;
    fprintf(stderr, "real crash at %p write=%d\n", si->si_addr, is_write); _exit(99);
}
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } else printf("ok   %s\n", #c); } while (0)

enum { PG = 4096, NPG = 64 };
static uint8_t *g_mem; static size_t g_len = NPG * PG;
static uint8_t pattern(size_t i) { return (uint8_t)(i * 131 + 7); }
static volatile int g_stop;
static _Atomic int g_restores;
// worker: serve restore requests by rebuilding the pattern off to the side and mremap'ing it over the range
static void *worker(void *a) {
    (void)a; nvn_dirty_gone_set_worker();
    while (!g_stop) {
        uint32_t seen = nvn_dirty_gone_req_seq(); uintptr_t r;
        int did = 0;
        while ((r = nvn_dirty_gone_take())) {
            did = 1;
            uint8_t *lo = g_mem + PG, *hi = g_mem + (NPG - 1) * PG; size_t len = hi - lo;
            if (!nvn_dirty_restore_begin(lo, len)) continue;
            uint8_t *t = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            for (size_t i = 0; i < len; i++) t[i] = pattern((size_t)(lo - g_mem) + i);
            usleep(20000);   // a slow GPU readback
            mprotect(t, len, PROT_READ);
            void *m = (void *)syscall(__NR_mremap, t, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, lo);
            if (m != lo) { printf("mremap failed %d\n", errno); _exit(98); }
            nvn_dirty_restore_commit(lo, len); g_restores++;
        }
        if (!did) nvn_dirty_gone_idle(seen, 100);
    }
    return NULL;
}
static void *reader(void *a) { volatile uint8_t *p = a; return (void *)(uintptr_t)*p; }
static void *writer(void *a) { volatile uint8_t *p = a; *p = 0x5a; return NULL; }
static void *hostw(void *a) { g_prep(a, 16); memset(a, 0xa5, 16); return NULL; }

int main(void) {
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_ONSTACK}; sigaction(SIGSEGV, &sa, 0);
    g_mem = mmap(NULL, g_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < g_len; i++) g_mem[i] = pattern(i);
    nvn_dirty_add_range(g_mem, g_len);
    nvn_dirty_protect(g_mem, g_len); uint64_t stamp = nvn_dirty_now();
    uint8_t *lo = g_mem + PG; size_t len = (NPG - 2) * PG;
    pthread_t w; pthread_create(&w, NULL, worker, NULL);
    // 1. discard
    CHECK(nvn_dirty_gone_begin(lo, len, stamp) == 0);
    nvn_dirty_gone_commit(lo, len);
    CHECK(nvn_dirty_page_state(lo + 5 * PG) == PG_GONE);
    CHECK(nvn_dirty_any_gone(g_mem, g_len) == 1);
    unsigned char v; mincore(lo, PG, &v); CHECK((v & 1) == 0);   // released
    // 2. read fault from two threads -> restore, content back, still write-protected with the old generation
    pthread_t r1, r2; void *x1, *x2; uint8_t *p = lo + 7 * PG + 123;
    pthread_create(&r1, NULL, reader, p); pthread_create(&r2, NULL, reader, p + 3 * PG); pthread_join(r1, &x1); pthread_join(r2, &x2);
    CHECK((uint8_t)(uintptr_t)x1 == pattern((size_t)(p - g_mem)));
    CHECK((uint8_t)(uintptr_t)x2 == pattern((size_t)(p + 3 * PG - g_mem)));
    CHECK(g_restores == 1);
    CHECK(nvn_dirty_page_state(p) == PG_PROT);
    CHECK(nvn_dirty_since(lo, len, stamp) == 0);
    // 3. write after restore -> ordinary dirty path
    pthread_t wt; pthread_create(&wt, NULL, writer, p); pthread_join(wt, NULL);
    CHECK(*p == 0x5a); CHECK(nvn_dirty_since(lo, len, stamp) == 1);
    // 4. begin refused: page written (NONE) / generation newer than the stamp
    CHECK(nvn_dirty_gone_begin(lo, len, stamp) == 1);
    nvn_dirty_protect(lo, len);
    CHECK(nvn_dirty_gone_begin(lo, len, stamp) == 2);
    uint64_t s2 = nvn_dirty_now();
    // 5. discard again, then a write fault straight on a released page, and a host pre-write on another
    CHECK(nvn_dirty_gone_begin(lo, len, s2) == 0); nvn_dirty_gone_commit(lo, len);
    pthread_create(&wt, NULL, writer, lo + 9 * PG); pthread_join(wt, NULL);
    CHECK(lo[9 * PG] == 0x5a); CHECK(lo[9 * PG + 1] == pattern((size_t)(lo + 9 * PG + 1 - g_mem)));
    CHECK(g_restores == 2); CHECK(nvn_dirty_since(lo, len, s2) == 1);
    nvn_dirty_protect(lo, len); uint64_t s3 = nvn_dirty_now();
    CHECK(nvn_dirty_gone_begin(lo, len, s3) == 0); nvn_dirty_gone_commit(lo, len);
    pthread_t hw; pthread_create(&hw, NULL, hostw, lo + 20 * PG); pthread_join(hw, NULL);
    CHECK(lo[20 * PG] == 0xa5 && lo[20 * PG + 16] == pattern((size_t)(lo + 20 * PG + 16 - g_mem)));
    CHECK(g_restores == 3);
    uint64_t rf, wf, pw, slow; int red; nvn_dirty_gone_stats(&rf, &wf, &pw, &slow, &red);
    printf("stats read %llu write %llu hostwrite %llu slow %llu red %d\n", (unsigned long long)rf, (unsigned long long)wf, (unsigned long long)pw, (unsigned long long)slow, red);
    CHECK(rf >= 2 && wf >= 1 && pw >= 1 && !red);
    g_stop = 1; nvn_dirty_gone_kick(); pthread_join(w, NULL);
    printf(g_fail ? "FAILED\n" : "ALL OK\n");
    return g_fail;
}
