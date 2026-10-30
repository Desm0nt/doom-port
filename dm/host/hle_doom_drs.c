// [DOOM] Dynamic resolution: one shared state for the game's Video menu item and the gear checkbox (BUGS_AND_FIXES 2.14).
// The state is the game's own cvar rs_enable (update main, build id 2847991952b3d7ab):
//   GOT main+0x48d53b0 -> idCVar rs_enable (registered by the static initializer at main+0x1c2fea0; name char* at +0x50, value char*
//   at +0x10, integer at +0x30, float at +0x34 -- the same layout hle_hid.c uses for the aim assist cvars). The renderer reads the
//   integer every frame (main+0x1c26ec4, main+0x1c27898: cbz = only "nonzero" matters).
//   The Video menu's "Dynamic Resolution" (patch_nso.py: the Chromatic Aberration getter / setter / profile key moved to rs_enable)
//   calls idCVar::SetInteger(rs_enable, v > 0, force 1) = main+0x1206a0, and after applying the video settings the menu asks the
//   profile to save: main+0x1eeb7e0 (this = profile: returns when state +0x08 == 1 or a requested state +0x0c is pending, else
//   calls vtable[+0x168](this, 3)). The profile serialiser (main+0x1107cc0) stores the cvar's value *string* (+0x10) under the key
//   "r_chromaticAberration" and on load sets it back with force -- so a gear change must go through SetInteger (string + integer),
//   not a raw integer write, to survive in the profile.
// Here: the gear reads the integer (dm_doom_drs_get, any thread) and queues a value (dm_doom_drs_set); the write happens on the game's
// main thread between frames -- at the entry of idCommonLocal::Frame (observation gate 30) -- by calling SetInteger exactly like the
// menu, followed by the menu's profile save request (retried for up to ~600 frames while the profile is busy). The profile object
// comes from observation gate 34 (idPlayerProfile::SetState(profile, state), main+0x10fd260), which the profile passes through when
// it loads / saves. A fresh install has no saved value: rs_enable's default is patched to "0" (CUTS 0x1c2feb4, see hle_args.c).
// Every change of rs_enable (menu, profile load, gear) is logged as "[drs] rs_enable a -> b".
#include "hle.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#ifdef DM_GAME_DOOM
enum { GOT_RS_ENABLE = 0x48d53b0, FN_CVAR_SET_INT = 0x1206a0, FN_PROFILE_SAVE = 0x1eeb7e0, PROFILE_SAVE_TRIES = 600 };
static uint8_t *g_base; static size_t g_size;
static _Atomic(uint8_t *) g_cvar;          // validated rs_enable idCVar, NULL until found
static _Atomic(uint8_t *) g_profile;       // last profile seen at idPlayerProfile::SetState
static _Atomic int g_pending = -1;         // value queued by the gear (0 / 1), -1 = none
static int g_save_tries, g_fn_ok = -1;
static void (*g_next_frame)(const uint64_t *), (*g_next_profile)(const uint64_t *);

static int ptr_ok(uint64_t p) { return p > 0x10000 && p < (1ull << 48) && !(p & 7); }
static int in_main(uint64_t p) { return g_base && p >= (uint64_t)(uintptr_t)g_base && p < (uint64_t)(uintptr_t)g_base + g_size; }
static int find_main(void) {
    if (g_base) return 1;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_size = g_mod[i].size; g_base = g_mod[i].base; }
    return g_base != NULL && g_size > GOT_RS_ENABLE + 8;
}
static uint8_t *drs_cvar(void) {
    uint8_t *cv = atomic_load(&g_cvar);
    if (cv || !find_main()) return cv;
    uint64_t o = *(volatile uint64_t *)(g_base + GOT_RS_ENABLE);
    if (!ptr_ok(o)) return NULL;
    const char *nm = *(const char *const *)(uintptr_t)(o + 0x50);
    if (!nm || !ptr_ok((uint64_t)(uintptr_t)nm & ~7ull) || strcmp(nm, "rs_enable")) return NULL;   // not constructed yet / another build
    atomic_store(&g_cvar, (uint8_t *)(uintptr_t)o);
    return (uint8_t *)(uintptr_t)o;
}
// The two game functions this calls, checked by their first instruction words (update main) before the first call.
static int fns_ok(void) {
    if (g_fn_ok >= 0) return g_fn_ok;
    static const uint32_t set_int[4] = {0xd10143ffu, 0xa9034ff4u, 0xa9047bfdu, 0x910103fdu}, save[4] = {0xb9400808u, 0x7100051fu, 0x54000120u, 0xb9400c08u};
    g_fn_ok = find_main() && !memcmp(g_base + FN_CVAR_SET_INT, set_int, sizeof set_int) && !memcmp(g_base + FN_PROFILE_SAVE, save, sizeof save);
    if (!g_fn_ok) LOG("[drs] idCVar::SetInteger / profile save not at the expected addresses (another main build): the gear checkbox is read-only");
    return g_fn_ok;
}
static uint8_t *profile_ok(void) {
    uint8_t *p = atomic_load(&g_profile);
    if (!p) return NULL;
    uint64_t vt = *(const uint64_t *)p;
    return in_main(vt) && in_main(*(const uint64_t *)(uintptr_t)(vt + 0x168)) ? p : NULL;
}

// ---- The player's last choice, mirrored by the host (files/save.drs, "0" / "1") ----
// Phone test diag45: the gear's SetInteger + profile save request (state 0, requested 3 -> the profile manager main+0x1df81a0 calls
// the save main+0x1dfc5f0, which queues an asynchronous save job) did not survive a restart: the next profile load set rs_enable
// back to 1. Whether the job wrote PROFILE/save.dat is not visible yet (now logged: profile state transitions here, save-file opens
// for writing in hle_fs.c). Independently of that, the host keeps the last value the player chose -- in the Video menu or with the
// gear, both are just changes of rs_enable after the profile has loaded -- and re-applies it (SetInteger + save request, exactly the
// menu's calls) a few frames after every profile load. No file yet (first launch with this build): "0", because a profile written
// before the Chromatic Aberration -> Dynamic Resolution repurpose carries CA's "1" under the same key, which is not a choice.
static char g_mirror_path[1100];
static int mirror_path(void) {
    if (!g_mirror_path[0]) { const char *sv = getenv("DM_SAVE"); if (!sv || !*sv) return 0; snprintf(g_mirror_path, sizeof g_mirror_path, "%s.drs", sv); }
    return 1;
}
static int mirror_read(void) {   // 0 / 1, or -1 when there is no file
    if (!mirror_path()) return -1;
    FILE *f = fopen(g_mirror_path, "rb"); if (!f) return -1;
    int c = fgetc(f); fclose(f);
    return c == '1' ? 1 : c == '0' ? 0 : -1;
}
static void mirror_write(int v) {
    if (!mirror_path()) return;
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", g_mirror_path);
    FILE *f = fopen(tmp, "wb"); if (!f) { LOG("[drs] cannot write %s (errno %d)", tmp, errno); return; }
    fputc(v ? '1' : '0', f); int ok = fflush(f) == 0; ok &= fsync(fileno(f)) == 0; fclose(f);
    if (!ok || rename(tmp, g_mirror_path)) { LOG("[drs] cannot save the choice to %s (errno %d)", g_mirror_path, errno); return; }
    LOG("[drs] choice %d saved to %s", v, g_mirror_path);
}
static _Atomic int g_load_done = 0;        // frames to wait before re-applying the mirrored choice after a profile load (0 = nothing to do)
static _Atomic int g_loading = 0;          // the profile is loading (state 2): rs_enable changes now come from the profile, not the player
static _Atomic int g_loaded_once = 0;      // a profile load has finished (or the fallback timeout passed): changes are the player's
static _Atomic int g_known = -1;           // the value the player chose last (mirror), -1 = not read yet

static void drs_profile_obs(const uint64_t *r) {   // idPlayerProfile::SetState(profile = x0, state = x1), before the store
    if (ptr_ok(r[0]) && in_main(*(const uint64_t *)(uintptr_t)r[0])) {
        uint8_t *p = (uint8_t *)(uintptr_t)r[0];
        uint8_t *old = atomic_exchange(&g_profile, p);
        int from = *(const int32_t *)(p + 8), to = (int)r[1];
        static int nlog; if (old != p || (from != to && nlog++ < 64))
            LOG("[drs] game profile %p state %d -> %d (requested %d)  [0 idle, 1 saving, 2 loading]", (void *)p, from, to, *(const int32_t *)(p + 0xc));
        if (to == 2) atomic_store(&g_loading, 1);
        else if (from == 2) { atomic_store(&g_loading, 0); atomic_store(&g_load_done, 5); }
    }
    if (g_next_profile) g_next_profile(r);
}

static void drs_write(uint8_t *cv, int v, const char *who) {
    int32_t cur = *(volatile int32_t *)(cv + 0x30);
    ((void (*)(void *, int, int))(g_base + FN_CVAR_SET_INT))(cv, v, 1);   // = SetChromaticAberration's call (menu setter)
    LOG("[drs] %s: rs_enable %d -> %d (value \"%s\"); requesting a profile save", who, cur, *(volatile int32_t *)(cv + 0x30), *(const char *const *)(cv + 0x10));
    g_save_tries = PROFILE_SAVE_TRIES;
}

static void drs_frame_obs(const uint64_t *r) {     // idCommonLocal::Frame entry: the game's main thread, between frames
    uint8_t *cv = drs_cvar();
    if (cv) {
        static int last = -2, nlog; static unsigned frames;
        if (atomic_load(&g_known) < 0) {
            int m = mirror_read(); atomic_store(&g_known, m < 0 ? 0 : m); if (m < 0) mirror_write(0);
            LOG("[drs] last choice %d (%s)", atomic_load(&g_known), m < 0 ? "no file yet: off" : g_mirror_path);
        }
        // fallback: no profile load seen in ~1 minute (gate missed it / no profile at all) -> apply the choice, later changes are the player's
        if (!atomic_load(&g_loaded_once) && ++frames == 3600 && !atomic_load(&g_loading) && !atomic_load(&g_load_done)) {
            atomic_store(&g_load_done, 1); LOG("[drs] no profile load seen: applying the last choice now");
        }
        int ld = atomic_load(&g_load_done);
        if (ld > 0 && atomic_compare_exchange_strong(&g_load_done, &ld, ld - 1) && ld == 1) {   // a profile load finished a few frames ago
            atomic_store(&g_loaded_once, 1);
            int want = atomic_load(&g_known), cur = *(volatile int32_t *)(cv + 0x30) != 0;
            if (cur != want && fns_ok()) drs_write(cv, want, "profile loaded with another value, restoring the last choice");
            else LOG("[drs] profile loaded: rs_enable %d = last choice", cur);
            last = *(volatile int32_t *)(cv + 0x30);
        }
        int v = atomic_exchange(&g_pending, -1);
        if (v >= 0) {
            if ((*(volatile int32_t *)(cv + 0x30) != 0) != v && fns_ok()) drs_write(cv, v, "gear");
            if (atomic_load(&g_known) != v) { atomic_store(&g_known, v); mirror_write(v); }
            last = *(volatile int32_t *)(cv + 0x30);
        }
        if (g_save_tries > 0) {
            uint8_t *p = profile_ok();
            if (p && *(const int32_t *)(p + 8) == 0 && *(const int32_t *)(p + 0xc) == 0) {
                ((void (*)(void *))(g_base + FN_PROFILE_SAVE))(p);
                LOG("[drs] profile %p: save requested (state %d, requested %d)", (void *)p, *(const int32_t *)(p + 8), *(const int32_t *)(p + 0xc));
                g_save_tries = 0;
            } else if (--g_save_tries == 0)
                LOG("[drs] profile %s: no save request (the host's choice file still restores it)", p ? "busy" : "not seen yet");
        }
        int now = *(volatile int32_t *)(cv + 0x30);
        if (now != last) {
            int player = atomic_load(&g_loaded_once) && !atomic_load(&g_loading) && atomic_load(&g_load_done) == 0;
            if (nlog++ < 64) LOG("[drs] rs_enable %d -> %d (value \"%s\")%s", last, now, *(const char *const *)(cv + 0x10), player ? " by the player (Video menu)" : "");
            if (player && (now != 0) != atomic_load(&g_known)) { atomic_store(&g_known, now != 0); mirror_write(now != 0); }
            last = now;
        }
    }
    if (g_next_frame) g_next_frame(r);
}

// hle_hid.c (DM_INPUT_OBS=1) hands its own callbacks for gates 30 / 34 here, so both run.
void doom_drs_obs_chain(int id, void (*fn)(const uint64_t *)) {
    if (id == OBS_DOOM_FRAME) g_next_frame = fn; else if (id == OBS_DOOM_PROFILE_STATE) g_next_profile = fn;
}
// hle_args.c build() (GetHostArgc, after the modules are loaded, before the game's main loop).
void doom_drs_init(void) {
    static int done; if (done) return; done = 1;
    int f = gate_obs_present(OBS_DOOM_FRAME), p = gate_obs_present(OBS_DOOM_PROFILE_STATE);
    if (f) gate_obs_set(OBS_DOOM_FRAME, drs_frame_obs);
    if (p) gate_obs_set(OBS_DOOM_PROFILE_STATE, drs_profile_obs);
    LOG("[drs] live bridge: Frame gate %s, profile gate %s", f ? "on" : "MISSING (gear checkbox read-only)", p ? "on" : "missing (no profile save request)");
    if (!f) g_fn_ok = 0;
}
// JNI (jni_android.c): 1 / 0 = rs_enable on / off (a queued gear value counts as current), -1 = not known yet (game not started).
// Until the profile has loaded (and the choice was re-applied), the last choice (mirror) is what the game will run with: show that.
int dm_doom_drs_get(void) {
    uint8_t *cv = drs_cvar(); if (!cv) return -1;
    int v = atomic_load(&g_pending);
    if (v >= 0) return v;
    if (!atomic_load(&g_loaded_once) || atomic_load(&g_loading) || atomic_load(&g_load_done)) {
        int k = atomic_load(&g_known); if (k < 0) k = mirror_read(); if (k >= 0) return k;
    }
    return *(volatile int32_t *)(cv + 0x30) != 0;
}
// Queue the gear's value; 0 = it cannot be written (no Frame gate or another main build), 1 = queued for the next frame.
int dm_doom_drs_set(int on) {
    if (!drs_cvar() || g_fn_ok == 0 || !gate_obs_present(OBS_DOOM_FRAME)) return 0;
    atomic_store(&g_pending, on ? 1 : 0);
    return 1;
}
#else
int dm_doom_drs_get(void) { return -1; }
int dm_doom_drs_set(int on) { (void)on; return 0; }
#endif
