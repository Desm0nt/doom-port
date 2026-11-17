// [pc] port-common runtime report: pc_note (pc.h). The file is named hle_*.c so every dm/ tree's Android CMakeLists picks it up
// through its file(GLOB HLE_SRCS ${HOST}/hle_*.c) without a build edit; it has no HLE entries of its own.
#include "dm.h"
#include "pc.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { PC_MAX = 32 };
static const char *_Atomic g_said[PC_MAX];   // ids already reported (pointer compare first, then strcmp: callers pass literals)

void pc_note(const char *id, int applied, const char *fmt, ...) {
    if (!id) return;
    for (int i = 0; i < PC_MAX; i++) {
        const char *s = atomic_load(&g_said[i]);
        if (!s) {
            const char *exp = NULL;
            if (!atomic_compare_exchange_strong(&g_said[i], &exp, id)) { s = exp; if (s == id || !strcmp(s, id)) return; continue; }
            break;                                   // this slot is ours: report
        }
        if (s == id || !strcmp(s, id)) return;
        if (i == PC_MAX - 1) return;                 // table full: stay quiet rather than repeat
    }
    char why[256] = "";
    if (fmt) { va_list ap; va_start(ap, fmt); vsnprintf(why, sizeof why, fmt, ap); va_end(ap); }
    LOG("[pc] %s: %s%s%s%s", id, applied ? "applied" : "skipped", *why ? " (" : "", why, *why ? ")" : "");
}
