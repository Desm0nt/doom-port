// 客体内存写保护脏页跟踪（nvn_dirty.c）
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // 登记一个内存池为跟踪区间（首次调用时注册缺页钩子与宿主写前钩子） | Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // 在册的内存池数、峰值、跟踪表容量（内存读数行用） | number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // 池销毁：受保护页还原可写并标失效 | Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // 覆盖页设只读 | Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // 当前代号：上传前取，作为该份内容的戳 | Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // 覆盖页在戳之后被写过 → 1 | Covered pages were written after the stamp -> 1
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累计：游戏写触发的缺页次数、宿主写前解保护的页数 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累计：本模块 mprotect 调用次数、覆盖页数、耗时 | Cumulative: this module's mprotect call count, pages covered, time spent
// [DOOM] issue #21, cumulative: failed mprotect calls (all / unprotect single page / protect or discard), unprotects rescued by
// widening to the whole PROT run, and the last errno (12 = ENOMEM: vm.max_map_count). Atomics only: callable from a signal handler.
void nvn_dirty_mp_fail(uint64_t *fails, uint64_t *unprot, uint64_t *prot, uint64_t *widened, int *last_errno);
// [DOOM] issue #21: VMA coalescing of zero-copy (dma-buf) chunks, DM_DIRTY_COALESCE (nvn_dirty.c). The engine registers each
// chunk right after mapping it at the guest address (MAP_SHARED|MAP_FIXED from fd at offset off); the tracker takes ownership of
// fd and re-maps the chunk in place from it when its split VMAs pile up. The veto (engine) answers 1 for a range that holds
// protection the tracker does not know about (wtrack storage snapshots); such a chunk is left alone.
void nvn_dirty_zc_chunk(void *lo, size_t n, int fd, uint64_t off);
void nvn_dirty_coalesce_veto_register(int (*fn)(uintptr_t lo, size_t len));
void nvn_dirty_coalesce_hold(int on);   // 1 / 0 around the engine publishing a vetoed region and its mprotect (no coalesce in between)
void nvn_dirty_coalesce_tick(void);                 // the "dm-vmacoal" thread every 500 ms; tests call it themselves (DM_DIRTY_COALESCE_THREAD=0)
int nvn_dirty_coalesce_at(const void *addr);        // re-map the chunk containing addr now (tests); 1 = done
void nvn_dirty_coalesce_stats(int *chunks, uint64_t *vmas, uint64_t *coalesces, uint64_t *aborts, uint64_t *ms);   // vmas: the model's total
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 当前存活槽、槽表高水位、登记总次数 | Currently live slots, slot table high-water mark, total number of registrations
// [DOOM] DM_TEX_DISCARD (engine: gfx/vulkan/engine/xbg_tex_discard.inc). Page-aligned ranges inside one tracked pool; only the
// engine's restore worker calls begin/abort/commit/restore_*. Page states: 0 none, 1 write-protected, 2 busy, 3 gone (released,
// PROT_NONE), 4 restoring.
int nvn_dirty_gone_begin(const void *lo, size_t len, uint64_t stamp);   // all pages PROT -> BUSY; 0 ok, 1 a page not protected, 2 written after stamp, 3 bad range
void nvn_dirty_gone_abort(const void *lo, size_t len);                  // BUSY -> PROT
void nvn_dirty_gone_commit(const void *lo, size_t len);                 // mprotect NONE + MADV_DONTNEED, BUSY -> GONE
size_t nvn_dirty_restore_begin(const void *lo, size_t len);             // GONE -> REST, returns pages taken
void nvn_dirty_restore_commit(const void *lo, size_t len);              // REST -> PROT (content already in place, read-only); wakes waiters
int nvn_dirty_page_state(const void *addr);                             // -1 untracked
int nvn_dirty_any_gone(const void *addr, size_t len);                   // a page of the range is GONE / REST
void nvn_dirty_gone_ask(const void *addr);                              // queue a restore request (signal-safe)
uintptr_t nvn_dirty_gone_take(void);                                    // worker: next requested page address, 0 = none
uint32_t nvn_dirty_gone_req_seq(void);
void nvn_dirty_gone_idle(uint32_t seen, int ms);                        // worker: sleep until a request / kick after `seen`, or ms
void nvn_dirty_gone_kick(void);
void nvn_dirty_gone_set_worker(void);                                   // the calling thread is the restore worker (never waits on GONE pages)
void nvn_dirty_gone_stats(uint64_t *read_faults, uint64_t *write_faults, uint64_t *host_writes, uint64_t *slow_waits, int *gave_up);
// [DOOM] DM_GFX_VT_ATOMIC=1: nvnTextureWriteTexels (prep + memcpy) and the renderer's dirty check + partial re-upload of a
// tracked texture (protect + stamp + read) exclude each other, so a VT page written by a transcode thread is never read
// half-new, and a page left unprotected because it was BUSY during protect cannot lose a write. Off: the old lock-free path.
int nvn_texel_sync_on(void);
void nvn_texel_lock(void);
void nvn_texel_unlock(void);
// [r4] DM_DIRTY_PROTECT_BGEN=1|2|3 (engine round 4b, docs/SWITCHES.md): re-protect a texture visiting only the 64-page windows
// whose newest generation passed the caller's floor (cookie, one per texture), with test-and-test-and-set and one range lookup.
// mode bit 0 = windows (needs the env set: unprotects are counted in flight, see nvn_dirty.c), bit 1 = verify with a full walk
// afterwards (st->missed = pages it still had to protect that the windows should have covered). A zero cookie walks every page.
typedef struct { uint64_t floor, reg; uintptr_t addr; size_t len; } nvn_dirty_cookie;
typedef struct { uint64_t calls, full, windows, pages, kept, missed; } nvn_dirty_pstat;   // cumulative; the caller resets
int nvn_dirty_pbgen_mode(void);                                    // DM_DIRTY_PROTECT_BGEN as read by the tracker (0 = off)
unsigned nvn_dirty_page_shift(void);
void nvn_dirty_protect_gen(const void *addr, size_t len, nvn_dirty_cookie *ck, int mode, nvn_dirty_pstat *st);
// Pages of [addr, addr+len) written after stamp as a bitmap (bit i = i-th page from the page holding addr); returns the page
// count, 0 when untracked or nwords is too small. *windows += block summaries above stamp that were read page by page.
size_t nvn_dirty_since_bits(const void *addr, size_t len, uint64_t stamp, uint64_t *bits, size_t nwords, uint64_t *windows);
