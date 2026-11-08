// [DOOM] The game's controller settings shared with the gear (BUGS_AND_FIXES 2.40, GitHub issue #31): Motion Aiming, its sensitivity
// slider and Aim Assist. One state each -- the game's own value; the gear only reads it when its dialog opens and writes it through
// the game's own setters, so a change in either place shows in both and goes into the game profile.
// Update main (build id 2847991952b3d7ab), idMenuDataSource_ControllerSettings (LoadData main+0xfe8780.., SetField main+0xfe9xxx):
//   field 6  "Aim Assist"      = profile byte +0x4698 (LoadData main+0xfe8ac4, SetField strb main+0xfe91a4; profile JSON "useAimAssist",
//                                serialiser main+0x10d8df8 / loader main+0x10e0b14; ConstructDefaults main+0x10d086c sets 1). Read every
//                                frame by idPlayerAimAssist::Update (main+0x2a6064). The aim assist *strength* is ours (DM_AA_STRENGTH).
//   field 7  motion swing      = joy_sixAxisSwingToggle (not mirrored here)
//   field 8  "Motion Aiming"   = joy_sixAxisAimToggle (GOT 0x488cb78): getter main+0x10e7d10 (int +0x30), setter main+0x10e7d30 =
//                                idCVar::SetBool(cvar, v, force 1) (main+0x120650); profile key "joy_sixAxisAimToggle" (value string).
//   field 9  sensitivity slider= joy_sixAxisGlobalScale (GOT 0x4895828): slider = round(GlobalScale / 60 x 100), 0..100 in steps of 1
//                                (main+0xfe8b28..0xfe8b88); SetField (main+0xfe9754..0xfe9798): GlobalScale = clamp(slider x 0.01 x 60,
//                                0, 60) -> setter main+0x10ecc90 = idCVar::SetFloat(cvar, force 1, v) (main+0x1206f0). Default "30" = 50;
//                                profile key "joy_sixAxisGlobalScale" (value string).
// The profile serialiser stores each cvar's value *string* (+0x10), never the int / float fields, so touch look's temporary gate writes
// (hle_hid.c tl_gates: value fields only, restored from the string) never reach the profile. For the same reason the truth read here
// is the value string, not the int a touch-look gate may have opened.
//
// Why the settings were lost (#31): idPlayerProfile save requests (main+0x1eeb7e0 and the two inline copies at main+0x1eeb674 /
// main+0x1eebb04, autosave included) return without saving while profile_requireSaveExplanation (GOT 0x48d9a40, default "1",
// "Turn off to allow profile save without save indicator explanation UI") is set and the profile's seenSaveExplanation byte (+0x38,
// JSON "seenSaveExplanation") is 0. That byte is set by the save explanation screen, which our CUTS 0x1080118 skips -- so a profile
// created on this port was never written (no PROFILE/save.dat on a fresh install; a profile imported from a Switch already had the
// byte and saved fine). hle_args.c now passes +set profile_requireSaveExplanation 0 (DM_DOOM_PROFILE_SAVE=0 keeps the game's gate);
// settings_loaded() below checks it once more after the profile load and logs the save gate.
//
// Safety net (as hle_doom_drs.c's DRS choice): the host mirror files/save.ctl ("aim=1", "sens=43", "aa=1", each with a dirty flag
// "aim_dirty=1") keeps a change the player made -- in the game menu or with the gear -- until a profile save has completed after it
// (profile state 1 -> 0). On the next launch only a still-dirty value (the profile did not get to save it) that the loaded profile
// does not have is re-applied with the same setter + a profile save request; a clean mirror never overrides the profile (an imported
// profile keeps its own values). Everything is logged as "[gyro] ...".
#include "hle.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef DM_GAME_DOOM
enum { GS_AIM, GS_SENS, GS_AA, GS_N };
enum {
    GOT_AIM_TOGGLE = 0x488cb78, GOT_GLOBAL_SCALE = 0x4895828, GOT_REQ_EXPLAIN = 0x48d9a40,
    FN_SET_AIM_TOGGLE = 0x10e7d30, FN_SET_GLOBAL_SCALE = 0x10ecc90, FN_CVAR_SET_BOOL = 0x120650,
    PROF_SEEN_EXPLAIN = 0x38, PROF_AIM_ASSIST = 0x4698,
};
static const char *const k_key[GS_N] = {"aim", "sens", "aa"};
static const char *const k_what[GS_N] = {"motion aiming (joy_sixAxisAimToggle)", "motion aiming sensitivity (joy_sixAxisGlobalScale slider)", "Aim Assist (profile useAimAssist)"};

uint8_t *doom_profile(void);                 // hle_doom_drs.c: the validated profile from gate 34, NULL before it was seen
void doom_profile_save_request(void);        // hle_doom_drs.c: ask the profile to save (retried while it is busy)

static uint8_t *g_base; static size_t g_size;
static int g_fn_ok = -1;
static _Atomic int g_ready;                  // the profile has loaded: g_val is the game's value
static _Atomic int g_val[GS_N] = {-1, -1, -1};      // the game's current values (main thread), read by the gear
static _Atomic int g_pending[GS_N] = {-1, -1, -1};  // queued by the gear, written on the main thread
static _Atomic int g_save_phase;             // profile saves seen by gate 34: 1 = started (0 -> 1), 2 = finished (1 -> 0)

static int ptr_ok(uint64_t p) { return p > 0x10000 && p < (1ull << 48) && !(p & 7); }
static int find_main(void) {
    if (g_base) return 1;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_size = g_mod[i].size; g_base = g_mod[i].base; }
    return g_base != NULL && g_size > GOT_REQ_EXPLAIN + 8;
}
static uint8_t *cvar_at(uint32_t got, const char *name) {
    if (!find_main()) return NULL;
    uint64_t o = *(volatile uint64_t *)(g_base + got);
    if (!ptr_ok(o)) return NULL;
    const char *nm = *(const char *const *)(uintptr_t)(o + 0x50);
    if (!nm || !ptr_ok((uint64_t)(uintptr_t)nm & ~7ull) || strcmp(nm, name)) return NULL;
    return (uint8_t *)(uintptr_t)o;
}
// The game's setters, checked word by word (update main) before the first call: the menu's own SetSixAxisAimToggle / SetGlobalScale
// wrappers (they ignore `this` and go through the GOT slots above) and idCVar::SetBool for profile_requireSaveExplanation.
static int fns_ok(void) {
    if (g_fn_ok >= 0) return g_fn_ok;
    static const uint32_t aim[5] = {0xb001bd20u, 0xf945bc00u, 0x12000021u, 0x320003e2u, 0x17c0e244u};
    static const uint32_t gs[4] = {0xb001bd40u, 0xf9441400u, 0x320003e1u, 0x17c0ce95u};
    static const uint32_t setb[5] = {0xd10143ffu, 0xa9034ff4u, 0xa9047bfdu, 0x910103fdu, 0xaa0003f4u};
    g_fn_ok = find_main() && !memcmp(g_base + FN_SET_AIM_TOGGLE, aim, sizeof aim) && !memcmp(g_base + FN_SET_GLOBAL_SCALE, gs, sizeof gs)
              && !memcmp(g_base + FN_CVAR_SET_BOOL, setb, sizeof setb);
    if (!g_fn_ok) LOG("[gyro] the controller settings setters are not at the expected addresses (another main build): the gear shows them read-only");
    return g_fn_ok;
}

// A cvar's value as the game last set it: the value string (+0x10; "1" / "0" / "true" / "false" / a number), else the field.
static float cvar_value(const uint8_t *cv, int as_int) {
    uint64_t p = *(const volatile uint64_t *)(cv + 0x10);
    if (p > 0x10000 && p < (1ull << 48)) {
        const char *s = (const char *)(uintptr_t)p; char *e; float v = strtof(s, &e);
        if (e != s) return v;
        if (!strcmp(s, "true")) return 1;
        if (!strcmp(s, "false")) return 0;
    }
    return as_int ? (float)*(const volatile int32_t *)(cv + 0x30) : *(const volatile float *)(cv + 0x34);
}
static int slider_of(float gs) {   // LoadData main+0xfe8b28: round(GlobalScale / 60 x 100), halves up
    float x = gs / 60.0f * 100.0f; int n = (int)floorf(x + 0.5f);
    return n < 0 ? 0 : n > 100 ? 100 : n;
}
static float scale_of(int n) {     // SetField main+0xfe9754: slider x 0.01 x 60 in float, clamped to [0, 60]
    volatile float a = (float)n * 0.01f; float v = a * 60.0f;
    return v < 0 ? 0 : v > 60 ? 60 : v;
}
static uint8_t *prof_ok(void) {   // the profile, with the two bytes used here holding a bool
    uint8_t *p = doom_profile();
    return p && p[PROF_AIM_ASSIST] <= 1 && p[PROF_SEEN_EXPLAIN] <= 1 ? p : NULL;
}
// Current values (main thread): -1 = unknown
static int read_val(int k) {
    if (k == GS_AIM) { uint8_t *cv = cvar_at(GOT_AIM_TOGGLE, "joy_sixAxisAimToggle"); return cv ? cvar_value(cv, 1) != 0 : -1; }
    if (k == GS_SENS) { uint8_t *cv = cvar_at(GOT_GLOBAL_SCALE, "joy_sixAxisGlobalScale"); return cv ? slider_of(cvar_value(cv, 0)) : -1; }
    uint8_t *p = prof_ok(); return p ? p[PROF_AIM_ASSIST] : -1;
}
static int write_val(int k, int v, const char *who) {   // main thread, through the game's setters; 1 = written
    if (!fns_ok()) return 0;
    int was = read_val(k);
    if (k == GS_AIM) {
        if (!cvar_at(GOT_AIM_TOGGLE, "joy_sixAxisAimToggle")) return 0;
        ((void (*)(void *, int))(uintptr_t)(g_base + FN_SET_AIM_TOGGLE))(doom_profile(), v ? 1 : 0);
    } else if (k == GS_SENS) {
        if (!cvar_at(GOT_GLOBAL_SCALE, "joy_sixAxisGlobalScale")) return 0;
        ((void (*)(void *, float))(uintptr_t)(g_base + FN_SET_GLOBAL_SCALE))(doom_profile(), scale_of(v));
    } else {
        uint8_t *p = prof_ok(); if (!p) return 0;
        *(volatile uint8_t *)(p + PROF_AIM_ASSIST) = v ? 1 : 0;   // = SetField's strb (main+0xfe91a4)
    }
    int now = read_val(k);
    if (k == GS_SENS) { uint8_t *cv = cvar_at(GOT_GLOBAL_SCALE, "joy_sixAxisGlobalScale");
        LOG("[gyro] %s: %s %d -> %d (joy_sixAxisGlobalScale \"%s\"); requesting a profile save", who, k_what[k], was, now, cv ? *(const char *const *)(cv + 0x10) : "?"); }
    else LOG("[gyro] %s: %s %d -> %d; requesting a profile save", who, k_what[k], was, now);
    doom_profile_save_request();
    return 1;
}

// ---- host mirror files/save.ctl ----
static struct { int val, dirty; } g_m[GS_N] = {{-1, 0}, {-1, 0}, {-1, 0}};
static char g_mpath[1100];
static int mpath(void) {
    if (!g_mpath[0]) { const char *sv = getenv("DM_SAVE"); if (!sv || !*sv) return 0; snprintf(g_mpath, sizeof g_mpath, "%s.ctl", sv); }
    return 1;
}
static void mirror_read(void) {
    if (!mpath()) return;
    FILE *f = fopen(g_mpath, "rb"); if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        for (int k = 0; k < GS_N; k++) {
            char dk[32]; snprintf(dk, sizeof dk, "%s_dirty", k_key[k]);
            if (!strcmp(line, k_key[k])) g_m[k].val = atoi(eq + 1);
            else if (!strcmp(line, dk)) g_m[k].dirty = atoi(eq + 1) != 0;
        }
    }
    fclose(f);
}
static void mirror_write(void) {
    if (!mpath()) return;
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", g_mpath);
    FILE *f = fopen(tmp, "wb"); if (!f) { LOG("[gyro] cannot write %s (errno %d)", tmp, errno); return; }
    for (int k = 0; k < GS_N; k++) if (g_m[k].val >= 0) fprintf(f, "%s=%d\n%s_dirty=%d\n", k_key[k], g_m[k].val, k_key[k], g_m[k].dirty);
    int ok = fflush(f) == 0; ok &= fsync(fileno(f)) == 0; fclose(f);
    if (!ok || rename(tmp, g_mpath)) LOG("[gyro] cannot save the mirror %s (errno %d)", g_mpath, errno);
}
static void mirror_note(int k, int v) {   // a change the player made (game menu or gear): dirty until a profile save completes
    if (g_m[k].val == v && g_m[k].dirty) return;
    g_m[k].val = v; g_m[k].dirty = 1; mirror_write();
}

// After every profile load (hle_doom_drs.c: a few frames after gate 34 saw state 2 -> 0, or its no-load fallback).
static void settings_loaded(int last[GS_N]) {
    static int read_once; if (!read_once) { read_once = 1; mirror_read(); fns_ok(); }
    uint8_t *rq = cvar_at(GOT_REQ_EXPLAIN, "profile_requireSaveExplanation"), *p = prof_ok();
    int req = rq ? *(const volatile int32_t *)(rq + 0x30) : -1, seen = p ? p[PROF_SEEN_EXPLAIN] : -1;
    const char *e = getenv("DM_DOOM_PROFILE_SAVE");
    if (req > 0 && seen == 0 && !(e && *e == '0') && fns_ok()) {   // the +set did not reach it (hle_args.c): the game's own setter
        ((void (*)(void *, int, int))(uintptr_t)(g_base + FN_CVAR_SET_BOOL))(rq, 0, 1);
        LOG("[gyro] profile_requireSaveExplanation was still 1: set to 0 now");
        req = *(const volatile int32_t *)(rq + 0x30);
    }
    LOG("[gyro] profile save gate: profile_requireSaveExplanation %d, seenSaveExplanation %d -> profile saves %s", req, seen,
        req == 0 || seen == 1 ? "allowed" : req < 0 || seen < 0 ? "unknown" : "BLOCKED (the game skips every profile save request)");
    for (int k = 0; k < GS_N; k++) {
        int v = read_val(k);
        if (g_m[k].dirty && g_m[k].val >= 0 && v >= 0 && v != g_m[k].val) {
            LOG("[gyro] profile loaded %s %d, but the last choice %d was not saved in the profile yet (%s): restoring it", k_what[k], v, g_m[k].val, g_mpath);
            write_val(k, g_m[k].val, "restore");
            v = read_val(k);
        } else LOG("[gyro] profile loaded: %s %d%s", k_what[k], v, g_m[k].val >= 0 ? (g_m[k].dirty ? " (= the unsaved last choice)" : " (mirror clean)") : "");
        last[k] = v; atomic_store(&g_val[k], v);
    }
    atomic_store(&g_ready, 1);
}

// hle_doom_drs.c drs_frame_obs (idCommonLocal::Frame entry, the game's main thread). player = the profile has loaded and is idle, so a
// change now is the player's (the game menu); loaded = a profile load finished a few frames ago.
void doom_settings_frame(int player, int loaded) {
    static int last[GS_N] = {-2, -2, -2}, nlog; static unsigned frames, save_at;
    if (!find_main()) return;
    frames++;
    if (loaded) settings_loaded(last);
    if (!atomic_load(&g_ready)) return;
    for (int k = 0; k < GS_N; k++) {
        int v = atomic_exchange(&g_pending[k], -1);
        if (v >= 0) {
            if (read_val(k) == v || write_val(k, v, "gear")) mirror_note(k, v);
            else LOG("[gyro] gear: %s could not be written", k_what[k]);
            last[k] = read_val(k);
        }
        int now = read_val(k);
        if (now != last[k]) {
            if (nlog++ < 64) LOG("[gyro] %s %d -> %d%s", k_what[k], last[k], now, player ? " by the player (game menu)" : " (not by the player: profile busy)");
            // the menu writes each slider step at once: one save request ~1.5 s after the last change (the menu may ask itself, too)
            if (player && now >= 0) { mirror_note(k, now); save_at = frames + 90; }
            last[k] = now;
        }
        atomic_store(&g_val[k], now);
    }
    if (save_at && frames >= save_at) { save_at = 0; doom_profile_save_request(); }
    // a profile save finished after a change: the profile has it now, the mirror is clean
    static int saving[GS_N];
    int ph = atomic_exchange(&g_save_phase, 0);
    if (ph & 1) for (int k = 0; k < GS_N; k++) saving[k] = g_m[k].dirty ? g_m[k].val : -1;
    if (ph & 2) {
        int any = 0;
        for (int k = 0; k < GS_N; k++) if (saving[k] >= 0 && g_m[k].dirty && g_m[k].val == saving[k]) { g_m[k].dirty = 0; any = 1; }
        if (any) { mirror_write(); LOG("[gyro] profile saved: aim %d sens %d aa %d are in the profile now (mirror clean)", g_m[0].val, g_m[1].val, g_m[2].val); }
        for (int k = 0; k < GS_N; k++) saving[k] = -1;
    }
}
// hle_doom_drs.c drs_profile_obs: idPlayerProfile::SetState (from -> to; 0 idle, 1 saving, 2 loading).
void doom_settings_profile_state(int from, int to) {
    if (from == 0 && to == 1) atomic_fetch_or(&g_save_phase, 1);
    else if (from == 1 && to == 0) atomic_fetch_or(&g_save_phase, 2);
}

// JNI (jni_android.c nativeGetGameSetting / nativeSetGameSetting). id 0 motion aiming (0 / 1), 1 sensitivity slider (0..100),
// 2 Aim Assist (0 / 1). get: -1 = not known yet (the profile has not loaded); a queued gear value counts as current.
int dm_doom_game_setting_get(int id) {
    if (id < 0 || id >= GS_N) return -1;
    int v = atomic_load(&g_pending[id]);
    if (v >= 0) return v;
    return atomic_load(&g_ready) ? atomic_load(&g_val[id]) : -1;
}
// 1 = queued for the next frame; 0 = it cannot be written here (profile not loaded, no Frame gate, another main build).
int dm_doom_game_setting_set(int id, int v) {
    if (id < 0 || id >= GS_N || !atomic_load(&g_ready) || g_fn_ok == 0 || atomic_load(&g_val[id]) < 0) return 0;
    if (id == GS_SENS) v = v < 0 ? 0 : v > 100 ? 100 : v; else v = v != 0;
    atomic_store(&g_pending[id], v);
    LOG("[gyro] gear: %s -> %d queued", k_what[id], v);
    return 1;
}
#else
void doom_settings_frame(int player, int loaded) { (void)player; (void)loaded; }
void doom_settings_profile_state(int from, int to) { (void)from; (void)to; }
int dm_doom_game_setting_get(int id) { (void)id; return -1; }
int dm_doom_game_setting_set(int id, int v) { (void)id; (void)v; return 0; }
#endif
