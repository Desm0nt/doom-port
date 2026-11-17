// [pc] Verified game-code patches (README.md "Relevance checks", rule 3). Any module that rewrites game instructions at run time
// goes through these: every site is compared word for word with the bytes the patch was written against BEFORE anything is
// written; one mismatch (another game build, an update) and nothing is touched, with one '[pc] <id>: skipped (<module>+<off> differs)'
// line. The writer is TL2's tw_poke (tl2-port hle_tl2_wide.c, "all sites are checked word for word: another build -> nothing
// changes"): the loader seals code in 16 KiB granules, so the granules around the site go RW, the words are written, the granules
// go back to RX and the instruction cache is flushed. Header-only (static inline) so a module needs no extra build entry.
//   static const uint32_t k_want[2] = {0x52808708 /* mov w8,#1080 */, 0x14000004};
//   static const pc_site k_sites[] = {{0x1994180, 2, k_want}};
//   if (pc_sites_match("my-fix", m->name, m->base, m->text_end, k_sites, 1)) { ... pc_poke(m->base, 0x1994180, repl, 2) ... }
// pc_patch() does both for one site. The NSO-side twin is module.json "relevance.nso" {"at": {"module", "off", "words"}} (pc_check.py).
#pragma once
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "pc.h"
void die(const char *fmt, ...);

typedef struct { uint32_t off, n; const uint32_t *want; } pc_site;

// 1 = every site lies inside the text ([0, text_end)) and holds exactly its words; 0 = not, logged once as skipped
static inline int pc_sites_match(const char *id, const char *mod, const uint8_t *base, uint64_t text_end, const pc_site *s, int n) {
    for (int i = 0; i < n; i++) {
        if ((uint64_t)s[i].off + 4ull * s[i].n > text_end) { pc_note(id, 0, "%s+%#x is past its text: not this build", mod, s[i].off); return 0; }
        if (memcmp(base + s[i].off, s[i].want, 4ull * s[i].n)) { pc_note(id, 0, "%s+%#x differs from the expected instructions: not this build, nothing patched", mod, s[i].off); return 0; }
    }
    return 1;
}
// Write n instruction words at base+off (code sealed RX by the loader). 0 = done, -1 = the RW mprotect failed (nothing written).
static inline int pc_poke(uint8_t *base, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(void *)(base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[pc] mprotect RX after a code patch at %p", (void *)at);
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}
// Verify one site, then write it. 1 = patched (logged as applied), 0 = skipped (logged), -1 = mprotect refused (logged, nothing written)
static inline int pc_patch(const char *id, const char *mod, uint8_t *base, uint64_t text_end, uint32_t off, const uint32_t *want, const uint32_t *repl, int n) {
    const pc_site s = {off, (uint32_t)n, want};
    if (!pc_sites_match(id, mod, base, text_end, &s, 1)) return 0;
    if (pc_poke(base, off, repl, n)) { pc_note(id, 0, "%s+%#x: mprotect RW refused, nothing patched", mod, off); return -1; }
    pc_note(id, 1, "%s+%#x, %d instructions", mod, off, n);
    return 1;
}
