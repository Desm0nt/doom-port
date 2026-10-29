// [DOOM] Portions (DM_GAME_DOOM paths: SetHeapSize result, MapMemory argument / source checks, QueryMemory over the alias page
// bitmap and module segments, MapPhysicalMemory page bitmap + zeroing, GetInfo 16/17/20/24) ported from nxnative
// (https://github.com/seedds/nxnative) dm/host/hle_svc.c, Copyright (c) 2026 seedds, MIT License.
// Horizon 内核系统调用（装 SDK 后由 SDK 运行时触发）。只实现 P1 启动链要的：内存布局 + 堆 + 查询 + 调试输出 + 计时。
// 寄存器约定：入参 w1.. / 出参 w0=Result、x1=值（GetInfo / SetHeapSize），与公开 Horizon ABI 一致。
// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include "hle.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

// 预留一整块客体地址空间，按 Horizon 分区（地址值直接用宿主 mmap 得到的真地址）。只占地址、不占内存。
// 大小从 5 GB 起降档重试（部分设备预留 5 GB 返回 ENOMEM(12)），下限 4864 MiB（游戏要 1 GB 别名+栈 + 3728 MB 堆，4864 − 1024 = 3840 > 3728 + 余量）。
// iOS 上不带「扩展虚拟寻址」权限时上限随设备型号不同（实测：有的 6 GB 成、有的 5120 MiB 不成，需要降到 4864 MiB）——
// 留到 8 GB 的话，签不了那项权限的开发者账号（免费账号）出的包在这一步就退出。
// Reserve one contiguous guest address space, partitioned like Horizon (address values are the real addresses returned by the host mmap). It only takes address space, not memory.
// The size starts at 5 GB and steps down on retry (on some devices reserving 5 GB returns ENOMEM(12)), floor 4864 MiB (the game needs 1 GB alias+stack + a 3728 MB heap; 4864 − 1024 = 3840 > 3728 + margin).
// On iOS without the extended virtual addressing entitlement the limit depends on the device model (measured: 6 GB works on some, 5120 MiB fails on others and must drop to 4864 MiB) —
// reserving up to 8 GB would make builds from developer accounts that cannot sign that entitlement (free accounts) exit at this step.
static size_t g_reserve;           // 实际预留大小（逐档降级后成功的值） | Actual reserved size (the value that succeeded after stepping down)
static uint8_t *g_as;              // ASLR 区基址 = 整块 | ASLR region base = the whole block
static uint8_t *g_heap; static size_t g_heap_cur, g_heap_cap; static int g_heap_rejects;
// 地址段整个落在客体堆当前已开放（可读写）的范围内：这种读不用陷内核核对，直接 memcpy（diag.c tk_rd 等每拍读客体内存的地方用）
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static uint8_t *g_stack; static size_t g_stack_cap;

static uint64_t app_pool_want(void);
static void mem_init(void) {
    if (g_as) return;
    // 从 5120 MiB 起逐档降级（每档减 128 MiB），下限 4864 MiB（堆容量 = reserve − 1 GB ≥ 3728 MB + 余量）
    // Step down from 5120 MiB (128 MiB per step), floor 4864 MiB (heap capacity = reserve − 1 GB ≥ 3728 MB + margin)
    size_t start = 5ull << 30; const size_t step = 128ull << 20, limit = 4864ull << 20;
#ifdef DM_GAME_DOOM
    // [DOOM] enlarged pools (hle_args.c DM_DOOM_POOL_MARGIN): the heap region (reserve - 1 GB) must hold pool - image; reserve that much
    // + 256 MB (address space only; Android arm64 has >= 39-bit user VA, so 5-6 GB is no issue). Steps down to the floor as before.
    { uint64_t need = app_pool_want() + (1ull << 30) + (256ull << 20); need = (need + step - 1) / step * step; if (need > start) start = (size_t)need; }
#endif
    for (size_t sz = start; sz >= limit; sz -= step) {
        g_as = mmap(NULL, sz, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_as != MAP_FAILED) { g_reserve = sz; break; }
        int err = errno; LOG("【红】reserve %zu MiB: mmap 失败 errno=%d (%s)，降档重试", sz >> 20, err, strerror(err));
        if (sz == limit) die("reserve guest AS: 试过 %zu–%zu MiB，全部失败", limit >> 20, start >> 20);
    }
    // 固定划分（都在预留块内）：alias 512MB、stack 256MB、heap 其余从 2GB 处起
    // Fixed partition (all inside the reserved block): alias 512MB, stack 256MB, heap the rest starting at 2GB
    g_alias = g_as + 0x10000000; g_alias_cap = 0x20000000;
    g_stack = g_as + 0x30000000; g_stack_cap = 0x10000000;
    g_heap  = g_as + 0x40000000; g_heap_cap  = g_reserve - 0x40000000;
#ifdef DM_GAME_DOOM
    { void doom_alias_init(void); doom_alias_init(); }
#endif
    LOG("guest AS %p +%zu MiB（降档后实际预留 %zu MiB）  heap %p alias %p stack %p", g_as, g_reserve >> 20, g_reserve >> 20, g_heap, g_alias, g_stack);
}

// 应用内存池 = Switch（4 GB 机型）给应用的 3285 MB。游戏启动（main+0x1b66160）把 QueryMemoryInfo 报的总量整块设成堆、失败就减 2 MB 重试，
// 整块交给自己的 StandardAllocator（main+0x32137a0）——所以堆大小 = 池 − 映像占用，宿主报多少它就只有多少。
// 之前这里报 3 GB 且 SetHeapSize 从不失败：堆比真机少约 150 MB，进局高峰主堆只剩约 18 MB，本机负载高时时序一偏申请 128 KB 返回空、
// 客体不判空直接写（CRASH main+0x1b36138，合入后复核 inv_tap）。
// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL 0xCD500000ull
// 报给游戏的池 = Switch 的 3285 MB + 512 MB（DM_APP_POOL_ADD_MB=<整数> 可调，可为负——往小调用来造「主堆用尽」的现场）。
// 为什么多给：游戏自己固定划走约 3.08 GB，主堆余量进局只有 100–180 MB、换图时地板三四十 MB、最大可分配块二十几 MB；用尽时原版多处不判空直接写
// （公开仓库，另见下面 dm_guest_heap_stat）。实测多给的量一比一变成主堆余量（138 → 649 MB），游戏划走的不跟着变；
// 没碰过的堆页不计入常驻内存，余量真被用掉时常驻才涨。堆区上限是预留里的 4 GB（mem_init），3728 MB 放得下；再加要先动预留。
// 取 +512 而不是更大：给 6 GB 内存的机型留余地，真机上限有读数后再定。
// Pool reported to the game = the Switch's 3285 MB + 512 MB (adjustable with DM_APP_POOL_ADD_MB=<integer>, may be negative -- lowering it is how to stage a "main heap exhausted" scene).
// Why more is given: the game carves out a fixed 3.08 GB or so for itself, leaving the main heap only 100–180 MB of free space on entering a game, a floor of thirty to forty MB when changing maps and a largest allocatable block of twenty-odd MB; when it runs out the original game writes through null without checking in many places
// (see also dm_guest_heap_stat below). Measured: the extra amount turns one-to-one into main heap free space (138 -> 649 MB), while what the game carves out does not change with it;
// heap pages never touched do not count toward resident memory, which grows only when the free space is actually used. The heap region limit is the 4 GB in the reservation (mem_init), which fits 3728 MB; adding more means changing the reservation first.
// +512 rather than more: to leave room for devices with 6 GB of RAM; to be revisited once there are readings of the limit on real devices.
#ifdef DM_GAME_DOOM
#define APP_POOL_ADD_MB 0     // [DOOM] the console's pool as is (the +512 MB is a D2R main-heap workaround); DM_APP_POOL_ADD_MB still overrides
#else
#define APP_POOL_ADD_MB 512
#endif
#ifdef DM_GAME_DOOM
int dm_doom_pool_add_mb(void);   // hle_args.c: enlarged NVN VRAM pools (DM_DOOM_POOL_MARGIN / DM_DOOM_WIDE) + heap headroom; the pools come out of the game heap
#define APP_POOL_EXTRA_MB() dm_doom_pool_add_mb()
#else
#define APP_POOL_EXTRA_MB() 0
#endif
// The pool we would like to report (no cache: mem_init sizes the reservation from it before app_pool() exists).
static uint64_t app_pool_want(void) { const char *e = getenv("DM_APP_POOL_ADD_MB"); long add = e ? atol(e) : APP_POOL_ADD_MB + APP_POOL_EXTRA_MB(); return APP_POOL + ((uint64_t)(int64_t)add << 20); }
static size_t image_bytes(void);
// Reported pool, capped so that pool - image fits the heap region (reserve - 1 GB): above that SetHeapSize would answer OutOfMemory
// instead of LimitReached and the game's 2 MB step-down start would not apply.
static uint64_t app_pool(void) {
    static uint64_t v; if (v) return v;
    uint64_t want = app_pool_want(), cap = g_heap_cap ? g_heap_cap + image_bytes() : want;
    v = want < cap ? want : cap;
    const char *e = getenv("DM_APP_POOL_ADD_MB");
    LOG("app pool: %llu MB reported (Switch 3285 MB %+lld MB%s%s)", (unsigned long long)(v >> 20), (long long)((int64_t)(v >> 20) - 3285),
        e ? ", DM_APP_POOL_ADD_MB" : ", VRAM pools + headroom", want > cap ? "; capped by the guest heap region" : "");
    return v;
}
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size; return (n + 0x1fffff) & ~(size_t)0x1fffff; }

// 游戏主堆读数：主堆 = main+0x32137a0 那个 nn::mem::StandardAllocator（启动时把整块堆交给它）。申请失败返回空，原版多处不判空直接写
// （公开仓库 连战 82 分钟后异步读盘回调 main+0x27ecdc 要 765 KB 连续块得空，memset 写 0 地址；进程常驻内存看不出这件事）。
// 余量 / 最大可分配块调的是分配器自己的查询（sdk+0x2f9d78 / +0x2f9dbc，内部走 CentralHeap::Query、自己加锁），只许在客体线程上调。
// 读到的数留一份在 g_gheap_last 里，分配返回空时的报红直接带上次读数（那一刻不再进分配器）。单位：总量 / 余量 MB，最大块 KB
// Game main heap readout: the main heap = the nn::mem::StandardAllocator at main+0x32137a0 (the whole heap block is handed to it at startup). A failed allocation returns null, and the original game writes through it without a null check in many places
// (after 82 minutes of boss rush, the asynchronous disk-read callback main+0x27ecdc asked for a contiguous 765 KB block, got null, and memset wrote to address 0; the process resident memory does not show this).
// Free space / largest allocatable block call the allocator's own queries (sdk+0x2f9d78 / +0x2f9dbc, which go through CentralHeap::Query and take their own lock) and may only be called on a guest thread.
// The values read are kept in g_gheap_last, and the red report on a null allocation carries the last readout directly (the allocator is not entered again at that moment). Units: total / free in MB, largest block in KB
static module_t *mod_named(const char *n) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, n)) return &g_mod[i]; return NULL; }
static _Atomic unsigned long long g_gheap_last[3];
int dm_guest_heap_stat(unsigned long long *total_mb, unsigned long long *free_mb, unsigned long long *maxblk_kb) {
#ifdef DM_GAME_DOOM   // [DOOM] main+0x32137a0 / sdk+0x2f9d78 / sdk+0x2f9dbc are D2R main + SDK 11.4.3 offsets: no heap readout
    (void)total_mb; (void)free_mb; (void)maxblk_kb; (void)mod_named; return 0;
#else
    module_t *mm = mod_named("main"), *ms = mod_named("sdk"); if (!mm || !ms || !g_heap_cur) return 0;
    uint8_t *al = mm->base + 0x32137a0; if (!al[0]) return 0;   // 首字节 = 已初始化（sdk+0x2f9898 写） | First byte = initialized (written by sdk+0x2f9898)
    uint64_t fr = ((uint64_t (*)(void *))(ms->base + 0x2f9d78))(al), mx = ((uint64_t (*)(void *))(ms->base + 0x2f9dbc))(al);
    *total_mb = g_heap_cur >> 20; *free_mb = fr >> 20; *maxblk_kb = mx >> 10;
    g_gheap_last[0] = *total_mb; g_gheap_last[1] = *free_mb; g_gheap_last[2] = *maxblk_kb; return 1;
#endif
}
// 上次读到的主堆读数（总量 MB、余量 MB、最大可分配块 KB；没读过全 0）。任何线程可调——不进分配器（iOS 壳在主线程上记内存告警那一行用）
// The last main-heap readout (total MB, free MB, largest allocatable block KB; all 0 if never read). Callable from any thread - does not enter the allocator (the iOS shell uses it on the main thread for the memory-warning log line)
void dm_guest_heap_last(unsigned long long v[3]) { for (int i = 0; i < 3; i++) v[i] = atomic_load(&g_gheap_last[i]); }
#ifndef DM_GAME_DOOM   // [DOOM] orig = sdk+0x2f9974 (SDK 11.4.3 offset); Doom's SDK exports this symbol, so the takeover would jump into garbage
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEmm", "照原版分配，只在返回空时报红（原版调用方多处不判空）", void *, hle_stdalloc_allocate, void *self, size_t size, size_t align) {
    static void *(*orig)(void *, size_t, size_t); if (!orig) { module_t *ms = mod_named("sdk"); orig = (void *(*)(void *, size_t, size_t))(ms->base + 0x2f9974); }
    void *p = orig(self, size, align);
    if (!p && size) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("【红】游戏堆分配返回空：申请 %zu 字节（对齐 %zu），分配器 %p；上次读数 主堆 %llu MB、余量 %llu MB、最大可分配块 %llu KB", size, align, self,
        (unsigned long long)g_gheap_last[0], (unsigned long long)g_gheap_last[1], (unsigned long long)g_gheap_last[2]); }
    return p;
}
#endif

// 内存账（memstat.m）用：客体地址空间整块范围（未初始化时 base = NULL）
// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = g_reserve; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23 };

static uint8_t g_seen[256];

// 已映射区间登记（MapMemory 栈、MapPhysicalMemory alias 页），QueryMemory 据此回报；SDK 靠它找空位
// Registry of mapped ranges (MapMemory stacks, MapPhysicalMemory alias pages), QueryMemory reports from it; the SDK relies on it to find free space
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // 返回 1 基下标，0 = 无重叠 | returns a 1-based index, 0 = no overlap
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nmap >= MAX_MAP) die("映射表满");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
    pthread_mutex_unlock(&g_map_lock);
}
static void map_del(int i) { pthread_mutex_lock(&g_map_lock); g_map[i] = g_map[--g_nmap]; pthread_mutex_unlock(&g_map_lock); }

#ifdef DM_GAME_DOOM
// ---- [DOOM] nxnative memory semantics (hle_svc.c there): all registry access under g_map_lock ----
static void map_add_l(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {   // caller holds g_map_lock
    if (g_nmap >= MAX_MAP) die("mapping table full");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
}
static void map_del_l(int i) { g_map[i] = g_map[--g_nmap]; }   // caller holds g_map_lock
// Alias region page bitmap (4 KiB Horizon pages): which pages MapPhysicalMemory has committed. QueryMemory reports mapped runs as
// Heap and free runs as Free (the SDK walks the alias region with QueryMemory to find space); MapPhysicalMemory hands out zeroed
// pages as Horizon does (a page unmapped with MADV_FREE can keep its old contents until the kernel reclaims it).
static _Atomic uint64_t *g_alias_bits;
static uint64_t *g_alias_exposed;   // one bit per host page ever opened (g_map_lock)
static _Atomic int64_t g_alias_mapped;
void doom_alias_init(void) {
    size_t pg = (size_t)getpagesize();
    if (!(g_alias_bits = calloc((g_alias_cap >> 18) + 1, sizeof *g_alias_bits))) die("alias region page bitmap calloc");
    if (!(g_alias_exposed = calloc(g_alias_cap / pg / 64 + 1, sizeof *g_alias_exposed))) die("alias exposure bitmap calloc");
}
static int64_t alias_pages(uint64_t off, uint64_t n, int map) {
    int64_t ch = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        uint64_t old = map ? atomic_fetch_or(&g_alias_bits[p >> 6], m) : atomic_fetch_and(&g_alias_bits[p >> 6], ~m);
        ch += __builtin_popcountll((map ? ~old : old) & m);
        p += k;
    }
    return map ? ch : -ch;
}
static int64_t alias_unmapped(uint64_t off, uint64_t n) {
    int64_t c = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        c += __builtin_popcountll(~atomic_load(&g_alias_bits[p >> 6]) & m);
        p += k;
    }
    return c;
}
static uint64_t alias_mask(unsigned n) { return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
static uint64_t alias_run_end(uint64_t p, uint64_t end, int mapped) {
    for (uint64_t first = p + 1; first < end;) {
        unsigned shift = first & 63, n = (unsigned)(end - first < 64 - shift ? end - first : 64 - shift);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (mapped ? ~bits : bits) & (alias_mask(n) << shift);
        if (different) return (first & ~UINT64_C(63)) + __builtin_ctzll(different);
        first += n;
    }
    return end;
}
static void alias_query(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped) {
    uint64_t p = (addr - (uintptr_t)g_alias) >> 12, lo = p;
    *mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
    while (lo) {
        uint64_t first = (lo - 1) & ~UINT64_C(63);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (*mapped ? ~bits : bits) & alias_mask((unsigned)(lo - first));
        if (different) { lo = first + 64 - __builtin_clzll(different); break; }
        lo = first;
    }
    uint64_t hi = alias_run_end(p, g_alias_cap >> 12, *mapped);
    *base = (uintptr_t)g_alias + (lo << 12); *size = (hi - lo) << 12;
}
// Zero the pages of [off, off+size) that are not mapped yet but whose host page was opened before (never-opened pages are still the
// mmap's zero pages)
static void alias_initialize(uint64_t off, size_t size) {
    size_t pg = (size_t)getpagesize(), ratio = pg >> 12;
    uint64_t end = (off + size) >> 12;
    for (uint64_t p = off >> 12; p < end;) {
        int mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
        uint64_t next = alias_run_end(p, end, mapped);
        if (!mapped) {
            uint64_t clear = p;
            for (uint64_t q = p; q < next;) {
                uint64_t host = q / ratio, stop = (host + 1) * ratio;
                if (stop > next) stop = next;
                if (!(g_alias_exposed[host >> 6] & (UINT64_C(1) << (host & 63)))) {
                    if (clear < q) memset(g_alias + (clear << 12), 0, (q - clear) << 12);
                    clear = stop;
                }
                q = stop;
            }
            if (clear < next) memset(g_alias + (clear << 12), 0, (next - clear) << 12);
        }
        p = next;
    }
    for (uint64_t h = off / pg, last = (off + size - 1) / pg; h <= last; ++h)
        g_alias_exposed[h >> 6] |= UINT64_C(1) << (h & 63);
}
static int memory_contains(uint64_t base, uint64_t capacity, uint64_t addr, uint64_t size) {
    return addr >= base && size <= capacity && addr - base <= capacity - size;
}
// MapMemory / UnmapMemory argument checks as the kernel makes them: 4 KiB alignment (InvalidAddress 0xCC01), nonzero aligned size
// (InvalidSize 0xCA01), no wrap (InvalidCurrentMemory 0xD401), destination inside the stack region (InvalidMemoryRegion 0xDC01)
static uint32_t map_arguments(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src) & 0xfff) return 0xCC01;
    if (!size || (size & 0xfff)) return 0xCA01;
    if (size > UINT64_MAX - src || size > UINT64_MAX - dst) return 0xD401;
    return memory_contains((uintptr_t)g_stack, g_stack_cap, dst, size) ? 0 : 0xDC01;
}
static int map_source_busy(uint64_t addr, uint64_t size) {   // g_map_lock held: the range is (part of) a source already lent to MapMemory
    for (int i = 0; i < g_nmap; ++i)
        if (addr < g_map[i].src + g_map[i].size && g_map[i].src < addr + size) return 1;
    return 0;
}
// A borrowed source (MapMemory) reads as Locked with no permission (Horizon: IsBorrowed, the range is inaccessible while mapped)
static void map_source_query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *attr, uint64_t *perm) {
    uint64_t lo = *base, remaining = *size;
    for (int i = 0; i < g_nmap; ++i) {
        uint64_t src = g_map[i].src, end = src + g_map[i].size;
        uint64_t lower = lo, upper = remaining;
        if (addr >= src && addr < end) {
            if (src > lower) lower = src;
            if (end - lo < upper) upper = end - lo;
            *attr = 1; *perm = 0;
        } else if (end <= addr && end > lower) lower = end;
        else if (src > addr && src - lo < upper) upper = src - lo;
        remaining = upper - (lower - lo); lo = lower;
    }
    *base = lo; *size = remaining;
}
// Module segments as the loader maps them: .text r-x (Code), .rodata r-- (Code), .data/.bss rw- (CodeData), the rewriter's patch
// area r-x; the loader's key page inside the data segment is read-only
static void module_readonly_run(uint64_t off, uint64_t start, uint64_t end, uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    if (start >= end) return;
    if (off < start) { if (start < *hi) *hi = start; }
    else if (off >= end) { if (end > *lo) *lo = end; }
    else { if (start > *lo) *lo = start; if (end < *hi) *hi = end; *perm &= ~UINT64_C(2); }
}
static void module_query(const module_t *mm, uint64_t addr, uint64_t *base, uint64_t *size, uint64_t *state, uint64_t *perm) {
    uint64_t off = addr - (uint64_t)mm->base, mb = (uint64_t)mm->base;
    uint64_t ro = mm->ro_off ? mm->ro_off : mm->text_end, da = mm->data_off ? mm->data_off : ro, pa = mm->patch_off ? mm->patch_off : mm->size;
    if (off < ro)      { *base = mb;      *size = ro;            *state = 3/*Code*/;     *perm = 5; }
    else if (off < da) { *base = mb + ro; *size = da - ro;       *state = 3/*Code*/;     *perm = 1; }
    else if (off < pa) { *base = mb + da; *size = pa - da;       *state = 4/*CodeData*/; *perm = 3; }
    else               { *base = mb + pa; *size = mm->size - pa; *state = 4/*CodeData*/; *perm = 5; }
    if (*state == 4 && *perm == 3 && mm->key_page_size) {
        uint64_t lo = *base - mb, hi = lo + *size;
        module_readonly_run(off, mm->key_page_off, mm->key_page_off + mm->key_page_size, &lo, &hi, perm);
        *base = mb + lo; *size = hi - lo;
    }
}
#endif

int svc_thread(int num, uint64_t *regs);   // hle_thread.c：线程与同步类 | hle_thread.c: thread and synchronization kind

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT：svc 按号计数 | DM_HLECOUNT: svc counts by number
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    { uint64_t svc_host_call(int, uint64_t *); if (num >= 0x80 && svc_host_call(num, regs)) return regs[0]; }   // 0x80 起：剪口约定的宿主调用（hle_tact.c） | from 0x80: host calls by cut convention (hle_tact.c)
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
#ifdef DM_GAME_DOOM
        // Beyond the heap region: ResultOutOfMemory (0xD001) as KPageTable::SetHeapSize returns, which the SDK's SetMemoryHeapSize turns into
        // os::ResultOutOfMemory; InvalidCurrentMemory made the SDK abort (nxnative memory.md). Shrinking over a range lent to MapMemory: 0xD401
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD001; return 0; }
        { pthread_mutex_lock(&g_map_lock); int busy = size < g_heap_cur && map_source_busy((uintptr_t)g_heap + size, g_heap_cur - size); pthread_mutex_unlock(&g_map_lock);
          if (busy) { regs[0] = 0xD401; return 0; } }
#else
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
#endif
        if (size + image_bytes() > app_pool()) { g_heap_rejects++; regs[0] = 0x10801; return 0; }   // 超出应用内存池：ResultLimitReached。游戏启动时按 2 MB 递减重试直到放得下（main+0x1b66184） | exceeds the application memory pool: ResultLimitReached. At startup the game retries in 2 MB decrements until it fits (main+0x1b66184)
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        { static int said; if (!said && size) { said = 1; LOG("SetHeapSize：首次设堆成功 %zu MB（应用内存池 %llu MB − 映像 %zu MB；此前被拒 %d 次）", size >> 20, (unsigned long long)(app_pool() >> 20), image_bytes() >> 20, g_heap_rejects); } }
        g_heap_cur = size;
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
#ifdef DM_GAME_DOOM
    case 0x02: case 0x03: {   // SetMemoryPermission / SetMemoryAttribute: bookkeeping only, but a range lent to MapMemory cannot be changed (0xD401)
        uint64_t addr = regs[0], size = regs[1];
        pthread_mutex_lock(&g_map_lock);
        int busy = size > UINT64_MAX - addr || (size && map_source_busy(addr, size));
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = busy ? 0xD401 : 0; return 0;
    }
    case 0x04: {  // MapMemory(dst, src, size): the SDK moves a thread's stack into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { LOG("[RED] MapMemory refused (%#x): %#llx <- %#llx +%#llx", result, (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size); regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        // The source is not checked for guest backing (nxnative's map_source_backed): host-spawned guest threads (hle_guestthread.c)
        // lend host-allocated stacks
        if (map_find(dst, size) || map_source_busy(src, size)) result = 0xD401;
        else if (g_nmap == MAX_MAP) result = 0xCE01;
        if (result) { pthread_mutex_unlock(&g_map_lock); LOG("[RED] MapMemory refused (%#x): %#llx <- %#llx +%#llx", result, (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size); regs[0] = result; return 0; }
        uint64_t mask = (uint64_t)getpagesize() - 1;   // host page (4 KiB or 16 KiB on Android): the extra bytes opened are unused pages of the same region
        uint64_t lo = dst & ~mask, hi = (dst + size + mask) & ~mask;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);   // move semantics: the source is inaccessible while mapped (QueryMemory reports it Locked)
        map_add_l(dst, size, src, 0x0B /*MappedMemory*/);
        pthread_mutex_unlock(&g_map_lock);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size): contents move back to the source
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            pthread_mutex_unlock(&g_map_lock); LOG("UnmapMemory does not match a mapping %#llx+%#llx", (unsigned long long)dst, (unsigned long long)size); regs[0] = 0xD401; return 0; }
        memcpy((void *)src, (void *)dst, size);
        map_del_l(k - 1);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm; uint32_t attr = 0;
        int k; module_t *mm;
        pthread_mutex_lock(&g_map_lock);
        uint64_t ab = (uintptr_t)g_alias, ae = ab + g_alias_cap, rb = 0, rn = 0;
        int mapped = 0, in_alias = addr >= ab && addr < ae;
        if (in_alias) alias_query(addr, &rb, &rn, &mapped);
        if (a >= g_heap && a < g_heap + g_heap_cur) { base = (uint64_t)g_heap; size = g_heap_cur; state = 5/*Heap*/; perm = 3; }
        else if ((k = map_find(addr, 1))) { base = g_map[k-1].base; size = g_map[k-1].size; state = g_map[k-1].state; perm = 3; }
        else if (in_alias && mapped) { base = rb; size = rn; state = 5; perm = 3; }
        else if (a >= g_as && a < g_as + g_reserve) {   // free: up to the adjacent mapping / mapped alias run / heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + g_reserve;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            if (in_alias) {
                if (rb > ab && rb > lo) lo = rb;
                if (rb + rn < ae && rb + rn < hi) hi = rb + rn;
            } else if (addr < ab) {
                alias_query(ab, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ab : rb + rn;
                if ((mapped || edge < ae) && edge < hi) hi = edge;
            } else {
                alias_query(ae - 1, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ae : rb;
                if ((mapped || edge > ab) && edge > lo) lo = edge;
            }
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base = lo; size = hi - lo; state = 0; perm = 0; }
        else if ((mm = module_of(a)) && a >= mm->base) module_query(mm, addr, &base, &size, &state, &perm);
        else {
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            else { base = addr & ~0xFFFull; size = 0x1000; state = 0/*Free*/; perm = 0; } }
        map_source_query(addr, &base, &size, &attr, &perm);
        pthread_mutex_unlock(&g_map_lock);
        if (mi) {
            struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; } out = {base, size, (uint32_t)state, attr, (uint32_t)perm, 0, 0, 0};
            _Static_assert(sizeof out == 40, "MemoryInfo ABI size");
            memcpy((void *)(uintptr_t)mi, &out, sizeof out);
        }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size): alias region only, tracked per 4 KiB page
        uintptr_t a = regs[0]; size_t size = regs[1];
        if (!size || ((a | size) & 0xfff) || !memory_contains((uintptr_t)g_alias, g_alias_cap, a, size)) {
            LOG("SVC %#x outside the alias region %p+%#zx", num, (void *)a, size); regs[0] = 0xD401; return 0; }
        pthread_mutex_lock(&g_map_lock);
        if (num == 0x2D && map_source_busy(a, size)) { pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0; }
        uint64_t off = a - (uintptr_t)g_alias;
        int64_t need = alias_unmapped(off, size) * 4096;
        uintptr_t pg = (uintptr_t)getpagesize(), lo = a, hi = a + size, mlo = lo & ~(pg - 1), mhi = (hi + pg - 1) & ~(pg - 1);
        int commit = num == 0x2C;
        int64_t changed = commit ? need : (int64_t)size - need;
        if (!commit) {   // a host page shared with a still-mapped neighbour stays open
            if ((lo - mlo) / 4096 != (uint64_t)alias_unmapped(mlo - (uintptr_t)g_alias, lo - mlo)) mlo += pg;
            if ((mhi - hi) / 4096 != (uint64_t)alias_unmapped(hi - (uintptr_t)g_alias, mhi - hi)) mhi -= pg;
        }
        if (changed && mhi > mlo) {
            if (mprotect((void *)mlo, mhi - mlo, commit ? PROT_READ | PROT_WRITE : PROT_NONE)) die("SVC %#x mprotect %p+%#zx errno=%d", num, (void *)a, size, errno);
            if (!commit) (void)madvise((void *)mlo, mhi - mlo, MADV_FREE);
        }
        if (commit && changed) alias_initialize(off, size);   // newly committed pages read as zero, as on Horizon
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", commit ? "Map" : "Unmap", (void *)a, size);
        { int64_t d = alias_pages(off, size, commit) * 4096, before = atomic_fetch_add(&g_alias_mapped, d), after = before + d;
          pthread_mutex_unlock(&g_map_lock);
          if (before >> 28 != after >> 28) LOG("[mem] alias region mapped %.2f GB (alias region %.2f GB)", after / 1073741824.0, g_alias_cap / 1073741824.0); }
        regs[0] = 0; return 0;
    }
#else
    case 0x02: regs[0] = 0; return 0;   // SetMemoryPermission
    case 0x03: regs[0] = 0; return 0;   // SetMemoryAttribute
    case 0x04: {  // MapMemory(dst, src, size)：SDK 建线程时把堆上的栈搬进栈区 | MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if ((uint8_t *)dst < g_stack || (uint8_t *)dst + size > g_stack + g_stack_cap || map_find(dst, size)) {
            LOG("MapMemory 目标非法或已占用 %#llx+%#llx", dst, size); regs[0] = 0xDC01; return 0; }
        // 苹果页 16K、Horizon 页 4K：可访问区扩到 16K 边界（多放开的是同区未用页，不影响语义）
        // Apple pages are 16K, Horizon pages 4K: the accessible region is extended to a 16K boundary (the extra pages opened are unused pages of the same region, semantics unaffected)
        uint64_t lo = dst & ~0x3FFFull, hi = (dst + size + 0x3FFF) & ~0x3FFFull;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);   // 搬移语义：映射期间源区不被访问（Horizon 把它设为不可访问） | move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)：内容搬回源区；16K 页可能与相邻映射共用，不收回权限 | UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            LOG("UnmapMemory 与映射不符 %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        int k;
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (a >= g_as && a < g_as + g_reserve) {   // 空闲：到相邻映射/堆边界为止 | free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + g_reserve;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else {
            // 地址落在「当前宿主线程的栈」里：回报整段栈（状态 0xB = 栈）。SDK 初始化主线程记录时拿当时的 sp 查这里，把回报的区间当主线程栈；
            // 之后 nn::os::GetCurrentStackInfo 会断言 sp 仍在那个区间里，不在就中止。原先对客体地址空间之外的地址一律回报「它所在的 4 KiB 页」，
            // 主线程栈于是被记成一页——初始化时的 sp 与后来的 sp 是否同页，取决于宿主栈顶的绝对位置，而它随环境变量 / 启动参数总长移动（换个环境偶发启动即中止）。
            // 暗黑这一版不调 GetCurrentStackInfo（main / subsdk0 没有导入它，SDK 内部也没有调用点；宿主垫 12 档环境变量长度都起得到主菜单），所以没踩到；回报「一页」本身是错的，照改。
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            // This Diablo build does not call GetCurrentStackInfo (main / subsdk0 do not import it and the SDK has no internal call site; the host reaches the main menu with 12 different environment padding lengths), so it was never hit; reporting "one page" is wrong in itself, so fix it anyway.
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            else { base=addr & ~0xFFFull; size=0x1000; state=3/*Code*/; perm=5; } }
        if (mi) { uint64_t *m=(uint64_t*)mi; m[0]=base; m[1]=size; ((uint32_t*)mi)[4]=state; ((uint32_t*)mi)[5]=0; ((uint32_t*)mi)[6]=perm; ((uint32_t*)mi)[7]=0; ((uint32_t*)mi)[8]=0; }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size)：只许在 alias 区内（SDK 的 VammManager 用） | MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uint8_t *a = (uint8_t *)regs[0]; size_t size = regs[1];
        if (a < g_alias || a + size > g_alias + g_alias_cap) { LOG("SVC %#x 越出 alias 区 %p+%#zx", num, a, size); regs[0] = 0xD401; return 0; }
        int rc = num == 0x2C ? mprotect(a, size, PROT_READ | PROT_WRITE)
                             : (madvise(a, size, MADV_FREE), mprotect(a, size, PROT_NONE));
        if (rc) die("SVC %#x mprotect %p+%#zx errno=%d", num, a, size, errno);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", num == 0x2C ? "Map" : "Unmap", a, size);
        regs[0] = 0; return 0;
    }
#endif
    case 0x26: {  // Break(reason, ptr, size)：SDK 致命中止 | Break(reason, ptr, size): SDK fatal abort
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc 位于 %s+%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // 仅通知，可继续 | notification only, can continue
        _exit(42);
    }
    case 0x1E: regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = g_reserve;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = app_pool(); break;
        case I_UsedMem:  case I_UsedNonSys:  v = g_heap_cur + image_bytes(); break;   // 已用 = 堆 + 映像，与 SetHeapSize 的核对同一口径（原先写死 64 MB）。sdk 的 QueryMemoryInfo（sdk+0x34f4ec）只拿它算「程序占用」那一格； | Used = heap + image, the same accounting as the check in SetHeapSize (it used to be hard-coded to 64 MB). The sdk's QueryMemoryInfo (sdk+0x34f4ec) only uses it to compute the "program usage" field;
                                                 // 「可用总量」那一格直接是池总量，所以游戏启动设堆仍要从整池起按 2 MB 往下减到 池 − 映像（约 34 次被拒）——这是原版在真机上的走法，不是我们的口径错
                                                 // The "total available" field is simply the pool total, so at startup the game still has to step down from the whole pool by 2 MB at a time until pool - image (about 34 refusals) when setting its heap -- this is how the original behaves on the real device, not a mistake in our accounting
#ifdef DM_GAME_DOOM
        // [DOOM] the system resource DOOM's main.npdm declares (META +0x14) is 0: no virtual address memory, as on hardware
        // (the SDK's IsVirtualAddressMemoryEnabled is "total != 0"; nxnative memory.md)
        case I_SysResTotal: v = 0; break; case I_SysResUsed: v = 0; break;
        // FreeThreadCount (Horizon 11+): the application's 96-thread limit less the game's live threads (Atmosphère kern_svc_info.cpp)
        case 24: { int thr_limit_used(void); int u = thr_limit_used(); v = u < 96 ? 96 - u : 0; break; }
#else
        case I_SysResTotal: v = 0x2000000; break; case I_SysResUsed: v = 0x400000; break;
#endif
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // DM_DETCLOCK 开时为确定流（hle_os.c） | a deterministic stream when DM_DETCLOCK is on (hle_os.c)
#ifdef DM_GAME_DOOM
        case I_ProgramId: v = 0x0100416004C00000ull; break;   // [DOOM] program id from main.npdm (ACI0)
#else
        case I_ProgramId: v = 0x0100726014352000ull; break;
#endif
        case I_IsApplication: v = 1; break;
#ifdef DM_GAME_DOOM
        case I_UserExcAddr: { static _Alignas(0x1000) uint8_t ctx[0x1000]; v = (uint64_t)ctx; break; }   // user exception context area (SetDyingMessageRegion writes into it; 0 would be a null write)
#else
        case I_UserExcAddr: v = 0; break;
#endif
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (占位返回成功)", num, regs[0], regs[1], regs[2]); }
        regs[0] = 0; return 0;
    }
}
