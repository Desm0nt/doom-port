// [turnip-ubwc-hint] Turnip on an Adreno 740 next to a v7xx system driver: FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=<1|0>
// (port-common/modules/turnip-ubwc-hint, KB PNH-B-11; idea and the version rule from peanuts vka_driver.c ubwc_hint, DroidDeck #963,
// Mesa MR 29754). The texture unit's UBWC flag hint (TPL1_DBG_ECO_CNTL1.TP_UBWC_FLAG_HINT) is one register for every driver on the
// GPU and has to match the system driver's: Qualcomm's v6xx drivers leave it off, v7xx ones set it. Mesa's default for the 740 is off
// (most 740 phones shipped v6xx), so Turnip next to a v7xx system driver flips it while SurfaceFlinger and the system driver still use
// it, and what Android composites on the GPU from compressed buffers (a screenshot, an overlay, One UI's blur) comes out garbled or
// flickering ("the One UI bug"). Other models: Mesa's default matches their system drivers (peanuts sets it for the 740 only).
// Mesa reads FD_DEV_FEATURES once, at its first device, as name=value pairs separated by ':' (freedreno_dev_info.c), so the hint is
// appended to a value dm/env already set, never replaces it, and a value that already names the feature is kept as it is.
// Pure helpers (stdio only): tests/ubwc_hint_test.c calls them directly.
#pragma once
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DM_UBWC_FEATURE "enable_tp_ubwc_flag_hint"

// The kgsl gpu_model node ("Adreno740v2"), "" when there is none (not an Adreno, or unreadable).
static inline void dm_ubwc_model(char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0; FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0; fclose(f); }
}
// "Adreno740v2" / "Adreno740" (the kgsl gpu_model node): 1, anything else 0.
static inline int dm_ubwc_model_740(const char *model) { return model && !strncmp(model, "Adreno740", 9) && !isdigit((unsigned char)model[9]); }

// The system driver's version: the first "V@dddd" in the file (libGLESv2_adreno.so carries "V@0863.0" for 512.863), 0 when not found.
// Read in chunks; the last 8 bytes of a chunk are kept, so a marker across a chunk border is found.
static inline int dm_ubwc_system_version(const char *path) {
    FILE *f = path ? fopen(path, "rb") : NULL; if (!f) return 0;
    enum { CHUNK = 1 << 16, KEEP = 8 };
    char *buf = (char *)malloc(CHUNK + KEEP); if (!buf) { fclose(f); return 0; }
    size_t keep = 0; int v = 0; size_t n;
    while (!v && (n = fread(buf + keep, 1, CHUNK, f)) > 0) {
        const size_t len = keep + n;
        for (size_t i = 0; i + 6 <= len && !v; i++)
            if (buf[i] == 'V' && buf[i + 1] == '@' && isdigit((unsigned char)buf[i + 2]) && isdigit((unsigned char)buf[i + 3]) &&
                isdigit((unsigned char)buf[i + 4]) && isdigit((unsigned char)buf[i + 5])) v = atoi(buf + i + 2);
        keep = len < KEEP ? len : KEEP; memmove(buf, buf + len - keep, keep);
    }
    free(buf); fclose(f); return v;
}

// Whether a FD_DEV_FEATURES value already names the feature (as a whole name before '=' or ':').
static inline int dm_ubwc_named(const char *value) {
    const size_t k = strlen(DM_UBWC_FEATURE);
    for (const char *p = value; p && *p;) {
        if (!strncmp(p, DM_UBWC_FEATURE, k) && (p[k] == '=' || p[k] == ':' || !p[k])) return 1;
        p = strchr(p, ':'); if (p) p++;
    }
    return 0;
}

// The new FD_DEV_FEATURES value with the hint (on = 1 / 0) appended to existing (NULL or "" = none).
// Returns 1 = written to out, 0 = keep existing as it is (it names the feature already), -1 = out too small (nothing set).
static inline int dm_ubwc_compose(char *out, size_t bytes, const char *existing, int on) {
    if (dm_ubwc_named(existing)) return 0;
    const int n = existing && *existing ? snprintf(out, bytes, "%s%s" DM_UBWC_FEATURE "=%d", existing, existing[strlen(existing) - 1] == ':' ? "" : ":", on ? 1 : 0)
                                        : snprintf(out, bytes, DM_UBWC_FEATURE "=%d", on ? 1 : 0);
    return n < 0 || (size_t)n >= bytes ? -1 : 1;
}
