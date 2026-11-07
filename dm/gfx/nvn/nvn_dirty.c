// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

// 页状态与 mprotect 的次序（改保护的一方先把状态改成 BUSY，mprotect 落地后才改成终态；别的线程见 BUSY 就等）：
//   设只读：NONE → BUSY → mprotect(只读) → PROT；解保护：PROT → BUSY → mprotect(可写) → NONE。
// 不变式：状态是 NONE 的页一定可写、是 PROT 的页一定只读。原先设只读是「先记 PROT、攒一段再 mprotect」，这段时间里另一线程的写前解保护
// 会把状态改回 NONE，随后攒批的 mprotect 落地 ⇒ 页只读而状态 NONE：游戏线程写它时缺页处理只回「重执行」永远空转；
// 宿主用系统调用往里读文件时 prep_write 看状态 NONE 跳过，系统调用不走缺页处理，直接 EFAULT、数据读不全。
// BUSY 最长持续 = 一次 mprotect 的时间（设只读时再加上给这一段逐页改状态的几次原子写）。等待有上限 BUSY_WAIT_MS，超限报红：
// 缺页处理里超限按「不是我们的页」返回，交给崩溃处理如实报崩，不静默空转。等待只读原子量、不拿任何锁。
// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus, when setting read-only, the few atomic writes that change the state of the pages in the run). The wait has an upper limit BUSY_WAIT_MS and reports red beyond it:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// [DOOM] DM_TEX_DISCARD (default off; engine side xbg_tex_discard.inc): two more page states for static textures whose guest
// copy was released. GONE = PROT_NONE, physical pages dropped (MADV_DONTNEED), the content lives only in the Vulkan image;
// REST = the restore worker is rebuilding it. Transitions (all by the engine's restore worker):
//   discard: PROT -> BUSY (all pages, generation <= the upload stamp) -> mprotect(NONE) + DONTNEED -> GONE
//   restore: GONE -> REST -> content rebuilt off to the side, mremap'ed over the range read-only -> PROT (generation unchanged:
//            the content equals the uploaded one, so the engine's dirty stamps stay valid; a write then faults as usual)
// A guest fault (read or write) or a host pre-write on a GONE / REST page asks the worker for a restore and waits (futex);
// pages never go from GONE to writable directly.
enum { PG_GONE = 3, PG_REST = 4 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 4096   // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; _Atomic uint64_t *bgen; size_t bcap; uint64_t reg; } range_t;   // reg: [r4] registration number (nvn_dirty_protect_gen cookies)
// [perf6] DM_DIRTY_BLOCKS=1 (default 1 since 30.10): bgen[b] = newest page generation in block b (64 pages = 256 KB), kept with a CAS max on
// every unprotect, so nvn_dirty_since skips clean blocks instead of loading every page's generation. The engine asks once per
// guest submit per sampled texture (~7 submits a frame); DOOM's VT caches are 33-89 MB (8-22k pages each).
enum { BLK_SHIFT = 6 };
static int g_blocks = -1;
static int blocks_on(void) { if (g_blocks < 0) { const char *e = getenv("DM_DIRTY_BLOCKS"); g_blocks = e ? (atoi(e) ? 1 : 0) : 1; }   /* default on since the 30.10 A/B (descriptor path -1.2 us/draw) */ return g_blocks; }
static void bgen_note(range_t *r, uintptr_t p, uint64_t g) {
    if (!r->bgen) return;
    _Atomic uint64_t *b = &r->bgen[p >> BLK_SHIFT]; uint64_t cur = atomic_load(b);
    while (cur < g && !atomic_compare_exchange_weak(b, &cur, g)) {}
}
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
// [DOOM] issue #21 (POCO F7 / Galaxy S24: "fault loop" in write_texels at level transitions): mprotect can fail. A protection
// change inside a VMA splits it, and a split past vm.max_map_count (65530 on Android) fails with ENOMEM. With zero-copy
// (DM_GFX_ZEROCOPY, Adreno default) the pools are dma-buf mappings, which are VM_PFNMAP and never merge back, so every page
// this module ever protected or unprotected alone stays a VMA of its own and the count only grows over a session. mp() used to
// ignore the result: an unprotect that failed still set the state NONE -> a read-only page in state NONE -> on_fault answered
// "re-execute" forever (crash.c's fault-loop guard after 20000 faults). Now the result is returned and counted, and the callers
// keep the invariant above: unprotect_page widens to the whole PROT run (whole VMAs, no split) or reports a real crash; a failed
// protect leaves the pages NONE with a new generation (dirty: re-uploaded, never stale). errno is kept for the caller.
static _Atomic uint64_t g_mp_fail, g_mp_fail_unprot, g_mp_fail_prot, g_mp_widened, g_mp_red_ns;
static _Atomic int g_mp_errno;
// [DOOM] issue #21: zero-copy chunks (dma-buf, VM_PFNMAP: their VMAs never merge) registered by the engine with their fd
// (nvn_dirty_zc_chunk). Every mprotect of this module notes the VMA boundaries it can create in a per-chunk bitmap: a VMA of
// such a mapping only ever splits, so 1 + the set bits is the chunk's VMA count (exact for our own calls; failed calls count
// too, which can only overestimate). The coalescer (below, DM_DIRTY_COALESCE) re-maps a fragmented chunk in place from its fd.
enum { MAX_ZC = 512 };
typedef struct { uintptr_t lo, end; size_t np; int fd; uint64_t off; _Atomic uint64_t *bnd; _Atomic uint32_t vmas; } zc_chunk_t;
static zc_chunk_t g_zc[MAX_ZC];
static _Atomic int g_nzc;
static _Atomic uint64_t g_coal_n;   // chunks re-mapped so far (crash note, stats)
static _Atomic int g_coal_active;   // a coalesce holds its chunk's pages BUSY: protect waits for them instead of skipping them
static zc_chunk_t *zc_of(uintptr_t a) {
    int n = atomic_load_explicit(&g_nzc, memory_order_acquire);
    for (int i = 0; i < n; i++) if (a >= g_zc[i].lo && a < g_zc[i].end) return &g_zc[i];
    return NULL;
}
static void zc_bound(uintptr_t a) {   // a VMA boundary at page address a (signal-safe: atomics only)
    zc_chunk_t *c = zc_of(a); if (!c || a == c->lo) return;
    size_t i = (a - c->lo) >> g_shift; uint64_t bit = 1ull << (i & 63);
    if (!(atomic_fetch_or(&c->bnd[i >> 6], bit) & bit)) atomic_fetch_add(&c->vmas, 1);
}
static int mp(uintptr_t a, size_t len, int prot) {
    int saved = errno;
    if (atomic_load_explicit(&g_nzc, memory_order_relaxed)) { zc_bound(a); zc_bound(a + len); }
    uint64_t t = mach_absolute_time(); int rc = mprotect((void *)a, len, prot), e = rc ? errno : 0;
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    if (rc) { atomic_fetch_add(&g_mp_fail, 1); atomic_store(&g_mp_errno, e); }
    errno = saved;
    return rc;
}

static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
// One red line per 10 s at most (the fault path calls it; LOG as busy_wait does): what failed, errno, and the totals so far.
// ok: -1 = a protect, 1 / 0 = an unprotect whose widened retry worked / failed too.
static void mp_red(const char *what, uintptr_t a, size_t pages, int ok) {
    uint64_t now = g_tb.denom ? mach_absolute_time() * g_tb.numer / g_tb.denom : 1, last = atomic_load(&g_mp_red_ns);
    if (last && now - last < 10000000000ull) return;
    if (!atomic_compare_exchange_strong(&g_mp_red_ns, &last, now ? now : 1)) return;
    LOG("【红】脏页跟踪 / dirty tracker: mprotect failed (%s %p, %zu pages%s), errno %d (12 = ENOMEM: vm.max_map_count reached?); totals: failures %llu (unprotect %llu, protect %llu), widened %llu",
        what, (void *)a, pages, ok < 0 ? "" : ok ? ", widened to the PROT run: ok" : ", widened to the PROT run: FAILED", atomic_load(&g_mp_errno),
        (unsigned long long)atomic_load(&g_mp_fail), (unsigned long long)atomic_load(&g_mp_fail_unprot), (unsigned long long)atomic_load(&g_mp_fail_prot), (unsigned long long)atomic_load(&g_mp_widened));
}
void nvn_dirty_mp_fail(uint64_t *fails, uint64_t *unprot, uint64_t *prot, uint64_t *widened, int *last_errno) {
    *fails = atomic_load(&g_mp_fail); *unprot = atomic_load(&g_mp_fail_unprot); *prot = atomic_load(&g_mp_fail_prot);
    *widened = atomic_load(&g_mp_widened); *last_errno = atomic_load(&g_mp_errno);
}
// The crash report's line (crash.c dm_crash_note_register; in the signal handler, like the rest of the report)
static void crash_note(void) {
    int n = atomic_load_explicit(&g_nzc, memory_order_acquire); uint64_t v = 0; for (int i = 0; i < n; i++) v += atomic_load(&g_zc[i].vmas);
    fprintf(stderr, "[dm] dirty tracker: mprotect calls %llu, failures %llu (unprotect %llu, protect %llu), widened %llu, last errno %d; zero-copy chunks %d with %llu VMAs (model), coalesced %llu times\n",
            (unsigned long long)atomic_load(&g_mp_calls), (unsigned long long)atomic_load(&g_mp_fail), (unsigned long long)atomic_load(&g_mp_fail_unprot),
            (unsigned long long)atomic_load(&g_mp_fail_prot), (unsigned long long)atomic_load(&g_mp_widened), atomic_load(&g_mp_errno), n, (unsigned long long)v,
            (unsigned long long)atomic_load(&g_coal_n));
}

static _Atomic int g_busy_red;
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限（报红一次）。信号处理里可用：只有原子读、mach_absolute_time、sched_yield
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【红】脏页跟踪：页 %p 停在「忙」超过 %d 毫秒（改保护的线程没走完），不再等", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static range_t *find(uintptr_t a) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) return &g_r[i];
    return NULL;
}
// [r4] DM_DIRTY_PROTECT_BGEN (engine round 4b, read once at the first pool registration, before any page is protected): while
// set, every unprotect is counted in flight from before it takes its generation until its page is NONE (generation and block
// summary stored). nvn_dirty_protect_gen takes a new floor only when it saw none in flight right after reading g_now, so an
// unprotect with a generation <= that floor has its block summary visible to the walk. Unset: nothing below changes.
static int g_pbgen = -1;   // -1 = not read yet (pbgen_read)
static int pbgen_read(void) {
    if (g_pbgen < 0) { const char *e = getenv("DM_DIRTY_PROTECT_BGEN"); int v = e && *e ? atoi(e) : 1; if (v < 0 || v > 3) v = 0; g_pbgen = v;   /* on (1) by default since 01.11 */
        if (v) LOG("[nvn] DM_DIRTY_PROTECT_BGEN=%d (1 windows, 2 safe part only, 3 windows + verify)", v); }
    return g_pbgen;
}
static _Atomic int g_unprot_inflight;
// Pages s..e (BUSY, really writable now) -> a new generation each -> NONE, inside the in-flight count: a page becomes NONE only
// this way (unprotect, or a protect whose mprotect failed), which is what the window walk below relies on.
static void mark_writable(range_t *r, uintptr_t s, uintptr_t e) {
    const int count = g_pbgen > 0; if (count) atomic_fetch_add(&g_unprot_inflight, 1);
    for (uintptr_t k = s; k <= e; k++) { uint64_t g = atomic_fetch_add(&g_now, 1) + 1; atomic_store(&r->gen[k], g); bgen_note(r, k, g); atomic_store(&r->state[k], PG_NONE); }
    if (count) atomic_fetch_sub(&g_unprot_inflight, 1);
}
// 调用方已把状态从 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY.
// Returns 1 = p is writable (NONE); 0 = it is still read-only (state PROT again): the fault must not be reported as handled.
// [DOOM] issue #21: when the one-page mprotect fails (it needs a split, ENOMEM at vm.max_map_count), the whole run of PROT pages
// around p is unprotected at once: its neighbours are NONE (writable, so a VMA boundary lies there) or the range edge (a pool
// starts and ends a mapping), so the run consists of whole VMAs and mprotect needs no split. Every page of the run gets a new
// generation (its textures re-upload once). A neighbour another thread holds BUSY also ends the run; then the retry may fail too.
static int unprotect_page(range_t *r, uintptr_t p) {
    const int count = g_pbgen > 0; if (count) atomic_fetch_add(&g_unprot_inflight, 1);   // held across the mprotect, as before
    uintptr_t s = p, e = p;
    int ok = !mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    if (!ok) {
        const uintptr_t np = (r->end - r->base) >> g_shift;
        atomic_fetch_add(&g_mp_fail_unprot, 1);
        for (;;) { uint8_t st = PG_PROT; if (!s || !atomic_compare_exchange_strong(&r->state[s - 1], &st, PG_BUSY)) break; s--; }
        for (;;) { uint8_t st = PG_PROT; if (e + 1 >= np || !atomic_compare_exchange_strong(&r->state[e + 1], &st, PG_BUSY)) break; e++; }
        ok = !mp(r->base + (s << g_shift), (e - s + 1) << g_shift, PROT_READ | PROT_WRITE);
        if (ok) atomic_fetch_add(&g_mp_widened, 1);
        mp_red("unprotect", r->base + (p << g_shift), (size_t)(e - s + 1), ok);
    }
    if (ok) mark_writable(r, s, e);
    else for (uintptr_t k = s; k <= e; k++) atomic_store(&r->state[k], PG_PROT);   // still read-only: the state says so
    if (count) atomic_fetch_sub(&g_unprot_inflight, 1);
    return ok;
}
// ---------- [DOOM] DM_TEX_DISCARD: restore requests (signal-safe: atomics, futex, nanosleep only) ----------
enum { NREQ = 64, GONE_WAIT_MS = 60000 };
static _Atomic uintptr_t g_req[NREQ];          // page addresses waiting for a restore (0 = free slot)
static _Atomic uint32_t g_req_seq, g_gone_seq;  // futex words: new request (worker waits) / a restore landed (faulting threads wait)
static _Atomic uint64_t g_gone_rf, g_gone_wf, g_gone_pw, g_gone_slow;
static _Atomic int g_gone_worker_tid, g_gone_red;
#ifdef __linux__
static void fx_wait(_Atomic uint32_t *w, uint32_t v, int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    syscall(__NR_futex, (uint32_t *)w, FUTEX_WAIT_PRIVATE, v, &ts, NULL, 0);
}
static void fx_wake(_Atomic uint32_t *w) { syscall(__NR_futex, (uint32_t *)w, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0); }
static int my_tid(void) { return (int)syscall(__NR_gettid); }
#else   // no futex (Apple hosts): short sleeps; DM_TEX_DISCARD is an Android engine feature, GONE pages never exist here
static void fx_wait(_Atomic uint32_t *w, uint32_t v, int ms) { (void)w; (void)v; (void)ms; struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL); }
static void fx_wake(_Atomic uint32_t *w) { (void)w; }
static int my_tid(void) { return -1; }
#endif
static void gone_ask(uintptr_t a) {
    a &= ~(g_pg - 1);
    for (;;) {
        int i;
        for (i = 0; i < NREQ; i++) if (atomic_load(&g_req[i]) == a) break;   // already queued
        if (i == NREQ) for (i = 0; i < NREQ; i++) { uintptr_t z = 0; if (atomic_compare_exchange_strong(&g_req[i], &z, a)) break; }
        atomic_fetch_add(&g_req_seq, 1); fx_wake(&g_req_seq);
        if (i < NREQ) return;
        fx_wait(&g_gone_seq, atomic_load(&g_gone_seq), 2);   // all slots taken: let the worker drain some
    }
}
// Wait until page p leaves GONE / REST (asking the worker for a restore while it is GONE). 1 = re-execute / go on; 0 = gave up
// (worker thread itself, or no restore within GONE_WAIT_MS): the fault is reported as a crash instead of hanging silently.
static int gone_wait(range_t *r, uintptr_t p) {
    const uintptr_t a = r->base + (p << g_shift);
    if (my_tid() == atomic_load(&g_gone_worker_tid)) return 0;   // the restore worker must never touch GONE pages (it would wait for itself)
    uint64_t t0 = mach_absolute_time();
    for (;;) {
        uint32_t seq = atomic_load(&g_gone_seq);
        uint8_t st = atomic_load(&r->state[p]);
        if (st == PG_BUSY) { if (!busy_wait(r, p)) return 0; continue; }
        if (st != PG_GONE && st != PG_REST) return 1;
        if (st == PG_GONE) gone_ask(a);   // idempotent; re-asked every round so a dropped request cannot strand us
        fx_wait(&g_gone_seq, seq, 50);
        uint64_t ms = (mach_absolute_time() - t0) * g_tb.numer / g_tb.denom / 1000000ull;
        if (ms > 1000) atomic_fetch_add(&g_gone_slow, 1);
        if (ms > GONE_WAIT_MS) { atomic_store(&g_gone_red, 1); return 0; }
    }
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    range_t *r = find((uintptr_t)addr);
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
    {   // [DOOM] DM_TEX_DISCARD: a released page (read or write); a read fault on a page being released (BUSY, already PROT_NONE)
        uint8_t s = atomic_load(&r->state[p]);
        if (s == PG_GONE || s == PG_REST) { atomic_fetch_add(is_write ? &g_gone_wf : &g_gone_rf, 1); return gone_wait(r, p); }
        if (!is_write) return s == PG_BUSY ? busy_wait(r, p) : 0;
    }
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) {   // 0 when the page stays read-only: a real crash report instead of a fault loop (issue #21)
        if (!unprotect_page(r, p)) return 0;
        atomic_fetch_add(&g_faults, 1); return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q; p++) {
            for (;;) { uint8_t st = PG_PROT;   // 见 BUSY 等完要重看：对方可能是在设只读，等完这页是 PROT，照样得解 | After waiting out BUSY, look again: the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { if (unprotect_page(r, p)) atomic_fetch_add(&g_preps, 1); break; }   // failed: the host write then faults into on_fault, which reports it
                if (st == PG_GONE || st == PG_REST) { atomic_fetch_add(&g_gone_pw, 1); if (!gone_wait(r, p)) break; continue; }   // [DOOM] DM_TEX_DISCARD: restore first, then unprotect as usual
                if (st != PG_BUSY || !busy_wait(r, p)) break; }
        }
        a = stop;
    }
}

static void dirty_init(void) {   // first pool registration (or first zero-copy chunk, issue #21); guest threads that create pools, serialized by the game
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb);
        pbgen_read();   // before any page can be protected, so unprotect_page always sees the final value
        dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); dm_crash_note_register(crash_note); }
}
void nvn_dirty_add_range(const void *mem, size_t size) {
    dirty_init();
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    _Atomic uint64_t *bgen = r->bgen; size_t nb = (np >> BLK_SHIFT) + 1;
    if (blocks_on()) { if (r->bcap < nb) { bgen = calloc(nb, sizeof *bgen); if (!bgen) die("dirty block table allocation failed (%zu)", nb); r->bcap = nb; } else memset((void *)bgen, 0, nb * sizeof *bgen); }
    else bgen = NULL;
    r->base = base; r->end = end; r->state = state; r->gen = gen; r->bgen = bgen;
    { static uint64_t reg_seq; r->reg = ++reg_seq; }   // under g_slot_mu
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end;
}
// Pages s..e (BUSY, writable) -> one mprotect(read-only) -> PROT. [DOOM] issue #21: if it fails (the run needs a split past
// vm.max_map_count) they are still writable: NONE with a new generation each, so they count as written (re-uploaded at the next
// check, protected again then) and can never be clean but writable.
static void protect_flush(range_t *r, uintptr_t s, uintptr_t e) {
    if (!mp(r->base + (s << g_shift), (e - s + 1) << g_shift, PROT_READ)) { for (uintptr_t k = s; k <= e; k++) atomic_store(&r->state[k], PG_PROT); return; }
    atomic_fetch_add(&g_mp_fail_prot, 1); mark_writable(r, s, e); mp_red("protect", r->base + (s << g_shift), (size_t)(e - s + 1), -1);
}
// 设只读：先把页标成 BUSY，mprotect 落地后才标 PROT（次序与不变式见文件头的状态说明）
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 连续置成 PROT 的页合成一次 mprotect（逐页调用在首传大贴图时一帧上千次；选角页启动帧 5：4888 次）。
    // 已是 PROT / BUSY 的页断开连续段：不重复保护，也不碰别的线程正在改的页
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls ran to over a thousand per frame when large textures were first uploaded; character select, startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break a run: they are not protected again, and pages another thread is changing are left alone
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (!took && i <= q && st == PG_BUSY && atomic_load(&g_coal_active) && busy_wait(r, i)) { st = PG_NONE; took = atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY); }   // issue #21: a page the coalescer holds would stay unprotected
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { protect_flush(r, run, i - 1); open = 0; }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;   // 未跟踪时保守视为脏，宁可多传也不显示旧内容 | When untracked, conservatively treat as dirty: better to upload more than to show stale content
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    if (r->bgen) {   // [perf6] block summary first: a block whose newest generation is <= stamp has no newer page
        for (uintptr_t b = p >> BLK_SHIFT, bq = q >> BLK_SHIFT; b <= bq; b++) {
            if (atomic_load(&r->bgen[b]) <= stamp) continue;
            uintptr_t lo = b << BLK_SHIFT, hi = lo + (1u << BLK_SHIFT) - 1; if (lo < p) lo = p; if (hi > q) hi = q;
            for (uintptr_t i = lo; i <= hi; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;
        }
        return 0;
    }
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // 上传前最后一次写的代号 ≤ 戳；之后的写必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
// ---------- [r4] DM_DIRTY_PROTECT_BGEN (engine round 4b; nvn_dirty.h) ----------
int nvn_dirty_pbgen_mode(void) { return pbgen_read(); }   // the engine may ask before the first pool registration
unsigned nvn_dirty_page_shift(void) { return (unsigned)g_shift; }
// Pages p..q: NONE -> BUSY -> one mprotect per run -> PROT, like nvn_dirty_protect, but the state byte is loaded first and the CAS
// is tried only on NONE (test-and-test-and-set; a CAS on a PROT page fails anyway). verify_floor != UINT64_MAX: count the pages
// taken whose generation is <= verify_floor into *missed (a page the window walk should have protected).
static void protect_run(range_t *r, uintptr_t p, uintptr_t q, uint64_t verify_floor, uint64_t *missed) {
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        int took = 0;
        if (i <= q) { uint8_t st = atomic_load(&r->state[i]);
            if (st == PG_BUSY && atomic_load(&g_coal_active) && busy_wait(r, i)) st = atomic_load(&r->state[i]);   // issue #21: wait out the coalescer (see nvn_dirty_protect)
            if (st == PG_NONE) took = atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY); }
        if (took && verify_floor != UINT64_MAX && atomic_load(&r->gen[i]) <= verify_floor) ++*missed;
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { protect_flush(r, run, i - 1); open = 0; }
    }
}
// Why visiting only the windows (64-page blocks) whose summary bgen is above the cookie's floor protects every writable page:
// a page becomes writable (NONE) only through unprotect_page, which takes generation g from g_now and stores gen and bgen
// before it stores NONE, all inside the in-flight count. The walk reads s = g_now first, then the in-flight count, then the
// block summaries. If the count was 0, every unprotect with g <= s had finished (its bgen >= g is visible) before the summaries
// were read; any later unprotect gets g > s. So after a walk that visited every block with bgen > F, every NONE page has
// gen > s, and s is the next floor; by induction every NONE page has a generation above the floor at the next call, i.e.
// lies in a block whose summary is above it. A count != 0 keeps the old floor (still valid: protecting never adds NONE
// pages). The first call of a cookie (floor 0), another range or registration (pool re-created) and ranges without block
// summaries (DM_DIRTY_BLOCKS=0) walk every page.
void nvn_dirty_protect_gen(const void *addr, size_t len, nvn_dirty_cookie *ck, int mode, nvn_dirty_pstat *st) {
    if (!len) return;
    range_t *r = find((uintptr_t)addr);   // one lookup (nvn_dirty_protect does nvn_dirty_tracked and then find again)
    if (!r || (uintptr_t)addr + len > r->end) return;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    const int same = ck->reg == r->reg && ck->addr == a && ck->len == len;
    const int windows = (mode & 1) && g_pbgen > 0 && r->bgen;
    const uint64_t floor = windows && same ? ck->floor : 0;
    // s, then the in-flight count (both seq_cst, before any block summary is read); a few retries ride out a fault in progress
    uint64_t s = atomic_load(&g_now); int quiet = atomic_load(&g_unprot_inflight) == 0;
    for (int k = 0; !quiet && windows && k < 32; k++) { sched_yield(); s = atomic_load(&g_now); quiet = atomic_load(&g_unprot_inflight) == 0; }
    ++st->calls;
    if (!floor) { protect_run(r, p, q, UINT64_MAX, NULL); st->pages += q - p + 1; ++st->full; }
    else for (uintptr_t b = p >> BLK_SHIFT, bq = q >> BLK_SHIFT; b <= bq; b++) {
        if (atomic_load(&r->bgen[b]) <= floor) continue;
        uintptr_t lo = b << BLK_SHIFT, hi = lo + (1u << BLK_SHIFT) - 1; if (lo < p) lo = p; if (hi > q) hi = q;
        protect_run(r, lo, hi, UINT64_MAX, NULL); st->pages += hi - lo + 1; ++st->windows;
    }
    if (floor && (mode & 2)) protect_run(r, p, q, quiet ? s : floor, &st->missed);   // verify: a full walk after it must find nothing old
    if (windows) { if (quiet) ck->floor = s; else { if (!same) ck->floor = 0; ++st->kept; } }
    ck->reg = r->reg; ck->addr = a; ck->len = len;
}
size_t nvn_dirty_since_bits(const void *addr, size_t len, uint64_t stamp, uint64_t *bits, size_t nwords, uint64_t *windows) {
    if (!len) return 0;
    range_t *r = find((uintptr_t)addr);
    if (!r || (uintptr_t)addr + len > r->end) return 0;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    const size_t np = q - p + 1, nw = (np + 63) >> 6;
    if (nw > nwords) return 0;
    memset(bits, 0, nw * sizeof *bits);
    if (r->bgen) {   // the same block-summary shortcut as nvn_dirty_since
        for (uintptr_t b = p >> BLK_SHIFT, bq = q >> BLK_SHIFT; b <= bq; b++) {
            if (atomic_load(&r->bgen[b]) <= stamp) continue;
            ++*windows;
            uintptr_t lo = b << BLK_SHIFT, hi = lo + (1u << BLK_SHIFT) - 1; if (lo < p) lo = p; if (hi > q) hi = q;
            for (uintptr_t i = lo; i <= hi; i++) if (atomic_load(&r->gen[i]) > stamp) bits[(i - p) >> 6] |= 1ull << ((i - p) & 63);
        }
    } else for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) bits[(i - p) >> 6] |= 1ull << ((i - p) & 63);
    return np;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
// ---------- [DOOM] issue #21: VMA coalescing of zero-copy chunks (DM_DIRTY_COALESCE, default on) ----------
// Why: a protection change inside a VMA splits it, and dma-buf mappings (VM_PFNMAP) never merge again, so in a zero-copy pool
// every page this module ever unprotected alone stays a VMA of its own. The count only grows (it reached vm.max_map_count 65530
// after ~23 min on a POCO F7, then splits failed with ENOMEM). On anonymous memory the kernel merges neighbours with equal
// protection, so there the count stays near the minimum the protection pattern needs: 1 + the number of PROT / NONE changes.
// What: re-map the whole chunk in place from the same dma-buf fd and offset (mmap MAP_SHARED|MAP_FIXED: one fresh VMA, the same
// physical pages, the same address, so the content, guest pointers and the Vulkan import -- which holds the buffer itself, not
// this CPU mapping -- are untouched), then re-apply the tracker's page states as a few large runs. That is the anonymous-memory
// minimum again. Rejected alternatives: coarser protection granules for dma-buf pools bound the count, but a write makes a whole
// granule writable and dirty (larger partial re-uploads of the VT caches every frame, a permanent cost); DM_GFX_ZC_MAX_MB only
// moves the TextureBuffer pool off zero-copy (the vertex / UBO snapshots come back) and leaves the other pools growing.
// Protocol (the page-state rules at the top of the file): under g_slot_mu (no pool registration / removal meanwhile) every page of
// the chunk goes NONE / PROT -> BUSY (any other state: abort, retry at a later tick); mmap(PROT_READ, MAP_FIXED) replaces the
// mapping atomically (other threads block on the mm lock, never see a hole); runs of formerly NONE pages get PROT_READ|PROT_WRITE
// back; then each page gets its old state, generation unchanged. Meanwhile a game write faults, sees BUSY and waits (busy_wait),
// a host pre-write waits for BUSY to clear, a pread that gets EFAULT is prepared and retried (hle_fs.c ReadFile), and a protect
// waits for the coalescer's pages (g_coal_active) instead of skipping them. Writable pages are read-only only inside that window,
// protected pages never become writable, so no write goes unseen. The in-flight unprotect count is held for the whole window, so
// a concurrent window walk (DM_DIRTY_PROTECT_BGEN) keeps its old floor. Chunks with mprotect protection of the engine's own
// (wtrack storage snapshots) are skipped through the veto hook the engine registers.
// When: a thread ("dm-vmacoal") ticks every 500 ms. If the modelled zero-copy VMAs exceed DM_DIRTY_COALESCE_BUDGET (default
// 16384, a quarter of the limit), the chunks with the largest excess over their minimum are coalesced (excess >= 64 and >= a
// quarter of the chunk's count, so a chunk whose own protection pattern needs that many VMAs is not re-mapped in vain), up to
// 8 a tick, until the total is under 3/4 of the budget. DM_DIRTY_COALESCE_TEST=<s>: every <s> seconds every chunk with any
// excess is coalesced whatever the budget (phone test of the mechanics); DM_DIRTY_COALESCE=0: off (the model is still kept).
static int g_coal_cfg = -1, g_coal_budget = 16384, g_coal_test_s, g_coal_broken;
static pthread_mutex_t g_coal_mu = PTHREAD_MUTEX_INITIALIZER;
static int (*g_coal_veto)(uintptr_t lo, size_t len);
static _Atomic uint64_t g_coal_aborts, g_coal_vetoed, g_coal_ns, g_coal_reopen_fail, g_coal_before, g_coal_after;
static int coal_cfg(void) {
    if (g_coal_cfg < 0) {
        const char *e = getenv("DM_DIRTY_COALESCE"); g_coal_cfg = e && *e ? atoi(e) != 0 : 1;
        e = getenv("DM_DIRTY_COALESCE_BUDGET"); if (e && atoi(e) > 0) g_coal_budget = atoi(e);
        e = getenv("DM_DIRTY_COALESCE_TEST"); if (e && atoi(e) > 0) g_coal_test_s = atoi(e);
        LOG("[nvn] DM_DIRTY_COALESCE=%d budget %d VMAs%s (zero-copy chunks re-mapped in place when their split VMAs pile up, issue #21)", g_coal_cfg, g_coal_budget,
            g_coal_test_s ? ", TEST: every chunk with any excess, periodically" : "");
    }
    return g_coal_cfg;
}
void nvn_dirty_coalesce_veto_register(int (*fn)(uintptr_t lo, size_t len)) { g_coal_veto = fn; }
// The engine publishes a new protected region (and applies its mprotect) inside hold(1) / hold(0): a coalesce holds g_coal_mu from
// its veto check to the end of the re-map, so it either sees the region and leaves the chunk alone or runs entirely before it.
void nvn_dirty_coalesce_hold(int on) { if (on) pthread_mutex_lock(&g_coal_mu); else pthread_mutex_unlock(&g_coal_mu); }
// The minimum VMA count of the chunk's current protection pattern (PROT = read-only, the rest writable; BUSY counts as its
// neighbour), from the range covering it; 1 for an untracked chunk (all writable).
static uint32_t coal_minimal(const zc_chunk_t *c, const range_t *r) {
    if (!r) return 1;
    uintptr_t p0 = (c->lo - r->base) >> g_shift; uint32_t m = 1; int prev = -1;
    for (size_t k = 0; k < c->np; k++) { uint8_t s = atomic_load(&r->state[p0 + k]); if (s == PG_BUSY) continue; int ro = s == PG_PROT; if (prev >= 0 && ro != prev) m++; prev = ro; }
    return m;
}
static range_t *coal_range(const zc_chunk_t *c) { range_t *r = find(c->lo); return r && c->lo >= r->base && c->end <= r->end ? r : NULL; }
// Re-map one chunk (caller holds g_coal_mu). 1 = done, 0 = skipped (veto, failure), -1 = a page was busy (try again soon).
static int coalesce(zc_chunk_t *c) {
    if (g_coal_broken) return 0;
    if (g_coal_veto && g_coal_veto(c->lo, c->end - c->lo)) { atomic_fetch_add(&g_coal_vetoed, 1); return 0; }
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = coal_range(c);
    if (!r && find(c->lo)) { pthread_mutex_unlock(&g_slot_mu); return 0; }   // a range covering only part of the chunk: never the case for pools (chunks are cut from one pool)
    const uintptr_t p0 = r ? (c->lo - r->base) >> g_shift : 0, nw = (c->np + 63) >> 6;
    uint64_t *was = calloc(nw, sizeof *was);   // bit set = the page was PROT
    if (!was) { pthread_mutex_unlock(&g_slot_mu); return 0; }
    uint64_t t0 = mach_absolute_time(); uint32_t before = atomic_load(&c->vmas);
    const int count = g_pbgen > 0; if (count) atomic_fetch_add(&g_unprot_inflight, 1);
    atomic_fetch_add(&g_coal_active, 1);
    size_t taken = 0;
    if (r) for (; taken < c->np; taken++) {
        _Atomic uint8_t *s = &r->state[p0 + taken]; uint8_t st = atomic_load(s);
        if ((st != PG_NONE && st != PG_PROT) || !atomic_compare_exchange_strong(s, &st, PG_BUSY)) break;
        if (st == PG_PROT) was[taken >> 6] |= 1ull << (taken & 63);
    }
    int ok = !r || taken == c->np;
    if (ok) {
        void *m = mmap((void *)c->lo, c->end - c->lo, r ? PROT_READ : PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, c->fd, (off_t)c->off);
        if (m != (void *)c->lo) {   // a MAP_FIXED that failed may have left a hole: say so loudly, never try again
            g_coal_broken = 1; ok = 0;
            LOG("【红】[nvn] VMA coalesce: mmap MAP_FIXED of chunk %p+%#zx failed errno %d; coalescing off for this session", (void *)c->lo, (size_t)(c->end - c->lo), errno);
        } else {
            for (size_t w = 0; w < nw; w++) atomic_store(&c->bnd[w], 0);
            atomic_store(&c->vmas, 1);
            for (size_t k = 0; r && k < c->np;) {   // runs of formerly writable pages get write access back
                if (was[k >> 6] >> (k & 63) & 1) { k++; continue; }
                size_t e = k; while (e + 1 < c->np && !(was[(e + 1) >> 6] >> ((e + 1) & 63) & 1)) e++;
                if (mp(c->lo + (k << g_shift), (e - k + 1) << g_shift, PROT_READ | PROT_WRITE)) {   // still read-only: PROT, and written as far as anyone can tell
                    atomic_fetch_add(&g_coal_reopen_fail, 1);
                    for (size_t j = k; j <= e; j++) { uint64_t g = atomic_fetch_add(&g_now, 1) + 1; atomic_store(&r->gen[p0 + j], g); bgen_note(r, p0 + j, g); was[j >> 6] |= 1ull << (j & 63); }
                }
                k = e + 1;
            }
        }
    }
    for (size_t k = 0; k < taken; k++) atomic_store(&r->state[p0 + k], (was[k >> 6] >> (k & 63) & 1) ? PG_PROT : PG_NONE);   // old states, old generations
    atomic_fetch_sub(&g_coal_active, 1);
    if (count) atomic_fetch_sub(&g_unprot_inflight, 1);
    pthread_mutex_unlock(&g_slot_mu);
    free(was);
    if (!ok) { if (g_coal_broken) return 0; atomic_fetch_add(&g_coal_aborts, 1); return -1; }
    if (g_coal_veto && g_coal_veto(c->lo, c->end - c->lo))   // an engine protection appeared during the re-map (wtrack activates rarely, at its first use)
        LOG("【红】[nvn] VMA coalesce: chunk %p gained an engine-protected region while being re-mapped; its write tracking resyncs through its own spot checks", (void *)c->lo);
    uint64_t ns = (mach_absolute_time() - t0) * g_tb.numer / g_tb.denom; uint32_t after = atomic_load(&c->vmas);
    atomic_fetch_add(&g_coal_ns, ns); atomic_fetch_add(&g_coal_before, before); atomic_fetch_add(&g_coal_after, after);
    uint64_t n = atomic_fetch_add(&g_coal_n, 1) + 1;
    if (n <= 16 || !(n & (n - 1)))
        LOG("[nvn] VMA coalesce #%llu: chunk %p+%zu MB re-mapped, VMAs %u -> %u, %.2f ms", (unsigned long long)n, (void *)c->lo, (size_t)((c->end - c->lo) >> 20), before, after, ns / 1e6);
    return 1;
}
int nvn_dirty_coalesce_at(const void *addr) {   // tests / diagnostics: re-map the chunk containing addr now
    zc_chunk_t *c = zc_of((uintptr_t)addr); if (!c) return 0;
    pthread_mutex_lock(&g_coal_mu); int ok = coalesce(c); pthread_mutex_unlock(&g_coal_mu); return ok > 0;
}
void nvn_dirty_coalesce_tick(void) {
    if (!coal_cfg() || g_coal_broken) return;
    static uint64_t next_test;
    int n = atomic_load_explicit(&g_nzc, memory_order_acquire); if (!n) return;
    uint64_t total = 0; for (int i = 0; i < n; i++) total += atomic_load(&g_zc[i].vmas);
    uint64_t now = mach_absolute_time() * g_tb.numer / g_tb.denom;
    const int test = g_coal_test_s && now >= next_test;
    if (!test && total <= (uint64_t)g_coal_budget) return;
    if (test) next_test = now + (uint64_t)g_coal_test_s * 1000000000ull;
    pthread_mutex_lock(&g_coal_mu);
    static char tried[MAX_ZC]; memset(tried, 0, (size_t)n);   // this tick: skipped (vetoed / busy) or done
    for (int done = 0, pass = 0; pass < n && done < 8; pass++) {
        int best = -1; uint32_t best_ex = 0;
        for (int i = 0; i < n; i++) {   // the chunk with the largest excess over its minimum
            zc_chunk_t *c = &g_zc[i]; uint32_t v = atomic_load(&c->vmas); if (v <= 1 || tried[i]) continue;
            range_t *r = coal_range(c); uint32_t m = coal_minimal(c, r), ex = v > m ? v - m : 0;
            if (!test && (ex < 64 || ex < v / 4)) continue;
            if (ex > best_ex) { best_ex = ex; best = i; }
        }
        if (best < 0 || !best_ex) break;
        uint32_t v0 = atomic_load(&g_zc[best].vmas);
        int ok = coalesce(&g_zc[best]);
        for (int t = 0; ok < 0 && t < 4; t++) { struct timespec ts = {0, 200000L}; nanosleep(&ts, NULL); ok = coalesce(&g_zc[best]); }   // a page another thread had busy for a moment
        tried[best] = 1;
        if (ok <= 0) continue;   // still busy, or vetoed: the other chunks go on, this one waits for the next tick
        done++; uint32_t v1 = atomic_load(&g_zc[best].vmas); if (v1 < v0) total = total > v0 - v1 ? total - (v0 - v1) : 0;
        if (!test && total <= (uint64_t)g_coal_budget * 3 / 4) break;
    }
    pthread_mutex_unlock(&g_coal_mu);
}
static void *coal_thread(void *a) {
    (void)a;
#ifdef __APPLE__
    pthread_setname_np("dm-vmacoal");
#else
    pthread_setname_np(pthread_self(), "dm-vmacoal");
#endif
    for (;;) { struct timespec ts = {0, 500000000L}; nanosleep(&ts, NULL); nvn_dirty_coalesce_tick(); }
    return NULL;
}
void nvn_dirty_zc_chunk(void *lo, size_t n, int fd, uint64_t off) {
    dirty_init();
    if (fd < 0 || !n || ((uintptr_t)lo & (g_pg - 1)) || (n & (g_pg - 1))) { if (fd >= 0) close(fd); return; }
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
    int i = atomic_load(&g_nzc);
    if (i >= MAX_ZC) { pthread_mutex_unlock(&mu); close(fd); LOG("【红】[nvn] VMA coalesce: chunk table full (%d), %p not registered", MAX_ZC, lo); return; }
    zc_chunk_t *c = &g_zc[i]; c->lo = (uintptr_t)lo; c->end = c->lo + n; c->np = n >> g_shift; c->fd = fd; c->off = off;
    c->bnd = calloc((c->np + 63) >> 6, sizeof *c->bnd); if (!c->bnd) die("VMA coalesce: boundary bitmap (%zu pages)", c->np);
    atomic_store(&c->vmas, 1);
    atomic_store_explicit(&g_nzc, i + 1, memory_order_release);
    pthread_mutex_unlock(&mu);
    if (!i && coal_cfg()) { const char *e = getenv("DM_DIRTY_COALESCE_THREAD");   // tests: 0 = no thread, they tick themselves
        if (!e || atoi(e)) { pthread_t th; if (!pthread_create(&th, NULL, coal_thread, NULL)) pthread_detach(th); } }
}
void nvn_dirty_coalesce_stats(int *chunks, uint64_t *vmas, uint64_t *coalesces, uint64_t *aborts, uint64_t *ms) {
    int n = atomic_load_explicit(&g_nzc, memory_order_acquire); uint64_t v = 0; for (int i = 0; i < n; i++) v += atomic_load(&g_zc[i].vmas);
    *chunks = n; *vmas = v; *coalesces = atomic_load(&g_coal_n); *aborts = atomic_load(&g_coal_aborts) + atomic_load(&g_coal_vetoed);
    *ms = atomic_load(&g_coal_ns) / 1000000;
}
// ---------- [DOOM] DM_TEX_DISCARD: worker-side page transitions (see the PG_GONE notes at the top; nvn_dirty.h) ----------
static range_t *pages_of(const void *lo, size_t len, uintptr_t *p, uintptr_t *q) {
    if (!len || ((uintptr_t)lo & (g_pg - 1)) || (len & (g_pg - 1)) || !nvn_dirty_tracked(lo, len)) return NULL;
    range_t *r = find((uintptr_t)lo);
    *p = ((uintptr_t)lo - r->base) >> g_shift; *q = *p + (len >> g_shift);
    return r;
}
int nvn_dirty_gone_begin(const void *lo, size_t len, uint64_t stamp) {
    uintptr_t p, q; range_t *r = pages_of(lo, len, &p, &q);
    if (!r) return 3;
    uintptr_t i = p;
    for (; i < q; i++) { uint8_t st = PG_PROT; if (!atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY)) break; }
    int why = i < q ? 1 : 0;   // a page is not write-protected (written since, never protected, or busy)
    if (!why) for (uintptr_t k = p; k < q; k++) if (atomic_load(&r->gen[k]) > stamp) { why = 2; break; }   // written after the upload stamp
    if (why) for (uintptr_t k = p; k < i; k++) atomic_store(&r->state[k], PG_PROT);
    return why;
}
void nvn_dirty_gone_abort(const void *lo, size_t len) {
    uintptr_t p, q; range_t *r = pages_of(lo, len, &p, &q); if (!r) return;
    for (uintptr_t k = p; k < q; k++) atomic_store(&r->state[k], PG_PROT);
}
void nvn_dirty_gone_commit(const void *lo, size_t len) {
    uintptr_t p, q; range_t *r = pages_of(lo, len, &p, &q); if (!r) return;
    if (mp((uintptr_t)lo, len, PROT_NONE)) {   // issue #21: still readable -> no DONTNEED (it would zero live content); stays protected
        atomic_fetch_add(&g_mp_fail_prot, 1); for (uintptr_t k = p; k < q; k++) atomic_store(&r->state[k], PG_PROT);
        mp_red("discard", (uintptr_t)lo, len >> g_shift, -1); return; }
    madvise((void *)lo, len, MADV_DONTNEED);   // private anonymous memory: the pages are freed (a dma-buf / file mapping would keep them)
    for (uintptr_t k = p; k < q; k++) atomic_store(&r->state[k], PG_GONE);
    atomic_fetch_add(&g_gone_seq, 1); fx_wake(&g_gone_seq);
}
size_t nvn_dirty_restore_begin(const void *lo, size_t len) {
    uintptr_t p, q; range_t *r = pages_of(lo, len, &p, &q); if (!r) return 0;
    size_t n = 0;
    for (uintptr_t k = p; k < q; k++) { uint8_t st = PG_GONE; n += atomic_compare_exchange_strong(&r->state[k], &st, PG_REST); }
    return n;
}
void nvn_dirty_restore_commit(const void *lo, size_t len) {
    uintptr_t p, q; range_t *r = pages_of(lo, len, &p, &q); if (!r) return;
    for (uintptr_t k = p; k < q; k++) { uint8_t st = PG_REST; atomic_compare_exchange_strong(&r->state[k], &st, PG_PROT); }
    atomic_fetch_add(&g_gone_seq, 1); fx_wake(&g_gone_seq);
}
int nvn_dirty_page_state(const void *addr) {
    range_t *r = find((uintptr_t)addr); if (!r) return -1;
    return atomic_load(&r->state[((uintptr_t)addr - r->base) >> g_shift]);
}
int nvn_dirty_any_gone(const void *addr, size_t len) {
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr & ~(g_pg - 1), e = (uintptr_t)addr + len;
    for (; a < e; a += g_pg) { int s = nvn_dirty_page_state((const void *)a); if (s == PG_GONE || s == PG_REST) return 1; }
    return 0;
}
void nvn_dirty_gone_ask(const void *addr) { gone_ask((uintptr_t)addr); }
uintptr_t nvn_dirty_gone_take(void) { for (int i = 0; i < NREQ; i++) { uintptr_t a = atomic_exchange(&g_req[i], 0); if (a) return a; } return 0; }
uint32_t nvn_dirty_gone_req_seq(void) { return atomic_load(&g_req_seq); }
void nvn_dirty_gone_idle(uint32_t seen, int ms) { fx_wait(&g_req_seq, seen, ms); }
void nvn_dirty_gone_kick(void) { atomic_fetch_add(&g_req_seq, 1); fx_wake(&g_req_seq); }
void nvn_dirty_gone_set_worker(void) { atomic_store(&g_gone_worker_tid, my_tid()); }
void nvn_dirty_gone_stats(uint64_t *read_faults, uint64_t *write_faults, uint64_t *host_writes, uint64_t *slow_waits, int *gave_up) {
    *read_faults = atomic_load(&g_gone_rf); *write_faults = atomic_load(&g_gone_wf); *host_writes = atomic_load(&g_gone_pw);
    *slow_waits = atomic_load(&g_gone_slow); *gave_up = atomic_load(&g_gone_red);
}
// [DOOM] Writer/reader exclusion for CPU texel writes vs. the renderer's partial re-upload (see nvn_dirty.h). Without it a
// re-upload can race a WriteTexels on another thread: it protects and reads while the writer is mid-memcpy (torn page for
// one submission), and a page that was BUSY during protect stays writable with gen <= the new stamp, so the rest of that
// write is never re-uploaded. One plain mutex: WriteTexels calls are short (one VT page) and uncontended almost always.
static pthread_mutex_t g_texel_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_texel_on = -1;
int nvn_texel_sync_on(void) { if (g_texel_on < 0) { const char *e = getenv("DM_GFX_VT_ATOMIC"); g_texel_on = e && atoi(e) ? 1 : 0; LOG("[nvn] DM_GFX_VT_ATOMIC=%d (WriteTexels vs partial re-upload exclusion)", g_texel_on); } return g_texel_on; }
void nvn_texel_lock(void) { pthread_mutex_lock(&g_texel_mu); }
void nvn_texel_unlock(void) { pthread_mutex_unlock(&g_texel_mu); }
