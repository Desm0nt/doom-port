// [DOOM] Host test (Linux: make t_dirty_coalesce) of issue #21's VMA coalescer in nvn_dirty.c (DM_DIRTY_COALESCE). A memfd
// mapping (MAP_SHARED, re-mappable from its fd at the same offset) stands in for a zero-copy dma-buf chunk; the kernel merges
// shmem VMAs where dma-buf ones never merge, so the VMA counts checked are the tracker's model, while protections are checked
// page by page against /proc/self/maps.
//   1. fragmentation: the model counts the splits our mprotects create; the budget tick re-maps the chunk; afterwards the model
//      is the minimum of the protection pattern, page states / generations / content are unchanged and every page's real
//      protection matches its state (PROT read-only, NONE writable).
//   2. veto: a chunk the engine vetoes is left alone.
//   3. stress: writer threads (plain stores), a host pread writer (prepare + EFAULT retry like hle_fs.c), an "uploader" (stamp,
//      protect, copy) and the coalescer on both chunks all at once: no crash, no write the tracker missed (content differs from
//      the uploader's copy but the page is not dirty since its stamp), protections match states at the end.
#define _GNU_SOURCE
#include "../nvn/nvn_dirty.c"
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>

static int (*g_hook)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_crash_note_register(void (*fn)(void)) { (void)fn; }
static _Atomic uint64_t g_segv;
static void on_segv(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_; int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    atomic_fetch_add(&g_segv, 1);
    if (g_hook && g_hook(si->si_addr, is_write, uc)) return;
    fprintf(stderr, "real crash at %p write=%d\n", si->si_addr, is_write); _exit(99);
}
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } else printf("ok   %s\n", #c); } while (0)

enum { PG = 4096, CPG = 256, NCH = 2, NPG = CPG * NCH };   // two 1 MB chunks
static uint8_t *g_mem;
static range_t *R(void) { return find((uintptr_t)g_mem); }
static uint8_t st(int i) { return atomic_load(&R()->state[i]); }
// Real protection of every page from /proc/self/maps: 1 = writable, 0 = read-only, -1 = not mapped
static void real_prot(int8_t *out) {
    for (int i = 0; i < NPG; i++) out[i] = -1;
    FILE *f = fopen("/proc/self/maps", "r"); char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
        for (unsigned long a = lo; a < hi; a += PG) if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + NPG * PG) out[(a - (uintptr_t)g_mem) / PG] = perm[1] == 'w';
    }
    fclose(f);
}
static int prot_matches(int lo, int hi) {   // PROT <-> read-only, NONE <-> writable
    int8_t p[NPG]; real_prot(p); int bad = 0;
    for (int i = lo; i < hi; i++) { uint8_t s = st(i); if ((s == PG_PROT && p[i] != 0) || (s == PG_NONE && p[i] != 1) || (s != PG_PROT && s != PG_NONE)) { if (bad++ < 5) printf("  page %d state %d real %d\n", i, s, p[i]); } }
    return !bad;
}
static uint32_t model(int c) { return atomic_load(&g_zc[c].vmas); }
static int veto_chunk1(uintptr_t lo, size_t len) { (void)len; return lo == (uintptr_t)g_mem + CPG * PG; }

// ---- stress
static volatile int g_stop;
static uint64_t g_stamp[NPG]; static uint8_t g_copy[NPG][64];   // the uploader's stamp and copy of the first 64 bytes of each page
static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static void *writer(void *a) {
    uint32_t s = (uint32_t)(uintptr_t)a; while (!g_stop) { int p = rnd(&s) % NPG; volatile uint8_t *b = g_mem + (size_t)p * PG + rnd(&s) % 64; *b = (uint8_t)(*b + 1); }
    return NULL;
}
static int g_tmpfd;
static _Atomic uint64_t g_efault;
static void *preader(void *a) {   // a host write: prepare, pread, EFAULT -> prepare again and retry (hle_fs.c ReadFile)
    uint32_t s = (uint32_t)(uintptr_t)a;
    while (!g_stop) { int p = rnd(&s) % NPG; uint8_t *dst = g_mem + (size_t)p * PG + 128; size_t sz = 256;
        g_prep(dst, sz);
        for (int tries = 0; tries < 64; tries++) { ssize_t r = pread(g_tmpfd, dst, sz, rnd(&s) % 1024); if (r >= 0) break; if (errno != EFAULT) { printf("pread errno %d\n", errno); _exit(95); } atomic_fetch_add(&g_efault, 1); g_prep(dst, sz); } }
    return NULL;
}
static _Atomic uint64_t g_missed, g_rounds;
static void *uploader(void *a) {
    (void)a;
    while (!g_stop) {
        for (int p = 0; p < NPG && !g_stop; p++) {
            uint8_t *pg = g_mem + (size_t)p * PG;
            if (g_stamp[p] && memcmp(g_copy[p], pg, 64) && !nvn_dirty_since(pg, PG, g_stamp[p])) {   // changed, but not dirty: a missed write
                if (atomic_fetch_add(&g_missed, 1) < 5) printf("  MISSED write page %d state %d\n", p, st(p)); }
            g_stamp[p] = nvn_dirty_now(); nvn_dirty_protect(pg, PG); memcpy(g_copy[p], pg, 64);   // stamp, protect, then read: a later write is > stamp
        }
        atomic_fetch_add(&g_rounds, 1);
    }
    return NULL;
}
static _Atomic uint64_t g_coal_ok, g_coal_try;
static void *coalescer(void *a) {
    (void)a; int c = 0;
    while (!g_stop) { atomic_fetch_add(&g_coal_try, 1); if (nvn_dirty_coalesce_at(g_mem + (size_t)(c & 1) * CPG * PG)) atomic_fetch_add(&g_coal_ok, 1); c++; usleep(200); }
    return NULL;
}

int main(void) {
    setenv("DM_DIRTY_COALESCE_THREAD", "0", 1); setenv("DM_DIRTY_COALESCE_BUDGET", "64", 1);
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    struct sigaction sa = {.sa_sigaction = on_segv, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER}; sigaction(SIGSEGV, &sa, 0);
    int fd = (int)syscall(SYS_memfd_create, "zc", 0); CHECK(fd >= 0); CHECK(ftruncate(fd, NPG * PG) == 0);
    g_mem = mmap(NULL, NPG * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < NPG * PG; i++) g_mem[i] = (uint8_t)(i * 7 + 3);
    nvn_dirty_add_range(g_mem, NPG * PG);
    for (int c = 0; c < NCH; c++) nvn_dirty_zc_chunk(g_mem + (size_t)c * CPG * PG, CPG * PG, dup(fd), (uint64_t)c * CPG * PG);
    // ---- 1. fragmentation and the budget tick
    nvn_dirty_protect(g_mem, NPG * PG);
    for (int i = 0; i < NPG; i += 3) g_mem[(size_t)i * PG + 1] ^= 0xff;   // faults: single-page unprotects split the read-only VMA
    for (int i = 0; i < NPG; i += 6) nvn_dirty_protect(g_mem + (size_t)i * PG, PG);   // half of them protected again
    uint32_t v0 = model(0), m0 = coal_minimal(&g_zc[0], R());
    printf("chunk 0: model %u VMAs, minimum %u\n", v0, m0);
    CHECK(v0 > m0 && v0 > 64);
    CHECK(prot_matches(0, NPG));
    uint8_t s_before[NPG]; uint64_t g_before[NPG]; for (int i = 0; i < NPG; i++) { s_before[i] = st(i); g_before[i] = atomic_load(&R()->gen[i]); }
    uint8_t *content = malloc(NPG * PG); memcpy(content, g_mem, NPG * PG);
    nvn_dirty_coalesce_tick();   // budget 64 < model total
    int zn; uint64_t zv, zcount, za, zms; nvn_dirty_coalesce_stats(&zn, &zv, &zcount, &za, &zms);
    printf("after tick: chunks %d, model %llu VMAs, coalesced %llu, skipped %llu\n", zn, (unsigned long long)zv, (unsigned long long)zcount, (unsigned long long)za);
    CHECK(zcount == 2 && model(0) == m0 && model(1) == coal_minimal(&g_zc[1], R()));
    int same = 1; for (int i = 0; i < NPG; i++) same &= st(i) == s_before[i] && atomic_load(&R()->gen[i]) == g_before[i];
    CHECK(same);
    CHECK(memcmp(content, g_mem, NPG * PG) == 0);
    CHECK(prot_matches(0, NPG));
    { uint64_t s = nvn_dirty_now(), f0 = atomic_load(&g_segv); int pp = 6, pn = 3;   // page 6: PROT, page 3: NONE
      CHECK(st(pp) == PG_PROT && st(pn) == PG_NONE);
      g_mem[(size_t)pn * PG] = 1; CHECK(atomic_load(&g_segv) == f0);          // writable: no fault
      g_mem[(size_t)pp * PG] = 1; CHECK(atomic_load(&g_segv) == f0 + 1);      // protected: faults, dirty
      CHECK(nvn_dirty_since(g_mem + (size_t)pp * PG, PG, s) == 1 && st(pp) == PG_NONE); }
    // ---- 2. veto
    nvn_dirty_coalesce_veto_register(veto_chunk1);
    CHECK(nvn_dirty_coalesce_at(g_mem + CPG * PG) == 0);
    CHECK(nvn_dirty_coalesce_at(g_mem) == 1);
    nvn_dirty_coalesce_veto_register(NULL);
    // ---- 3. stress
    char tmpl[] = "/tmp/t_dirty_coalesce_XXXXXX"; g_tmpfd = mkstemp(tmpl); unlink(tmpl); { uint8_t b[2048]; memset(b, 0x3c, sizeof b); CHECK(write(g_tmpfd, b, sizeof b) == (ssize_t)sizeof b); }
    pthread_t th[7]; for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)(i * 7919 + 1));
    pthread_create(&th[3], NULL, preader, (void *)(uintptr_t)12345); pthread_create(&th[4], NULL, uploader, NULL);
    pthread_create(&th[5], NULL, coalescer, NULL); pthread_create(&th[6], NULL, writer, (void *)(uintptr_t)4242);   // one uploader: it owns g_stamp / g_copy
    sleep(4); g_stop = 1; for (int i = 0; i < 7; i++) pthread_join(th[i], NULL);
    nvn_dirty_coalesce_stats(&zn, &zv, &zcount, &za, &zms);
    printf("stress: coalesces %llu of %llu tries (busy skips %llu), uploader rounds %llu, faults %llu, pread EFAULT retries %llu, coalesce time %llu ms\n",
        (unsigned long long)g_coal_ok, (unsigned long long)g_coal_try, (unsigned long long)za, (unsigned long long)g_rounds, (unsigned long long)g_segv, (unsigned long long)g_efault, (unsigned long long)zms);
    CHECK(g_coal_ok > 50 && g_rounds > 20);
    CHECK(g_missed == 0);
    CHECK(prot_matches(0, NPG));
    uint64_t f, fu, fp, w; int e; nvn_dirty_mp_fail(&f, &fu, &fp, &w, &e); CHECK(f == 0);
    printf(g_fail ? "FAILED\n" : "ALL OK\n");
    return g_fail;
}
