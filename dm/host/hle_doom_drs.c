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
#include <math.h>
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

void doom_settings_frame(int player, int loaded);     // hle_doom_settings.c (motion aiming / its sensitivity / Aim Assist, issue #31)
void doom_settings_profile_state(int from, int to);
static void drs_profile_obs(const uint64_t *r) {   // idPlayerProfile::SetState(profile = x0, state = x1), before the store
    if (ptr_ok(r[0]) && in_main(*(const uint64_t *)(uintptr_t)r[0])) {
        uint8_t *p = (uint8_t *)(uintptr_t)r[0];
        uint8_t *old = atomic_exchange(&g_profile, p);
        int from = *(const int32_t *)(p + 8), to = (int)r[1];
        static int nlog; if (old != p || (from != to && nlog++ < 64))
            LOG("[drs] game profile %p state %d -> %d (requested %d)  [0 idle, 1 saving, 2 loading]", (void *)p, from, to, *(const int32_t *)(p + 0xc));
        if (to == 2) atomic_store(&g_loading, 1);
        else if (from == 2) { atomic_store(&g_loading, 0); atomic_store(&g_load_done, 5); }
        if (from != to) doom_settings_profile_state(from, to);
    }
    if (g_next_profile) g_next_profile(r);
}

// ---- [r6] Round 6 user options (BUGS_AND_FIXES 2.18): light mode, fixed 3D scale vs DRS, DRS target fps on our GPU time ----
// Game side = patch_nso.py CUTS "[r6]" (update main). Every option leaves the game untouched while it is off.
//   Light mode (DM_DOOM_LIGHT=1 / gear): idRenderSystemLocal::ProgramPerformanceModeConfiguration (main+0x1c064c0, run from SwapBuffers)
//     applies the handheld quality preset when w20 = (perfMode cache 0x4839c20 == 0) && r_nxUndockedPerf -- patched to also take
//     r_nxUndockedPerf == 2. r_nxUndockedPerf (bool idCVar 0x573d118, default 1, GOT 0x48d5078) is read only there, so the host writes
//     its integer (+0x30) raw (a Set would clamp a bool to 1) and sets the preset's dirty byte 0x4839c28 (GOT 0x48d5068): the next
//     SwapBuffers re-applies the 15 quality cvars and the handheld DRS caps flag 0x593e074 (GOT 0x48d5080) -- the game's own dock /
//     undock path. GetPerformanceMode / GetOperationMode (hle_oe.c) do not change: gamma boost (opMode, main+0x1c36164), Npad style
//     (hle_hid.c), touch layout and the handheld BDM clock branch (perfMode, main+0x1c276c4) stay docked.
//   Fixed 3D scale (DM_DOOM_RES < 1, gear 85/75/67 %): hle_args.c passes +set rs_forceFractionX/Y; the CUT at 0x1c26ec4.. makes the
//     fraction apply with rs_enable 0. The forced fraction also overrides the dynamic result (0x1c26eec runs after both paths), so
//     while DRS is on the host sets both fractions to 0 (DRS decides inside its caps) and back to DM_DOOM_RES when DRS is turned off.
//   DRS target (DM_DOOM_DRS_FPS=30|45|60 / gear): while rs_enable != 0, rs_dropMilliseconds = 1000 / fps and rs_raiseMilliseconds =
//     0.945 x that (re-applied whenever the cvars differ, e.g. after a profile load). The controller (main+0x1c27370) gets the frame's
//     GPU microseconds from host call 0x95 at main+0x1c5b27c (doom_drs_gpu_svc): our Vulkan GPU busy time (engine union of slot
//     spans, xbg_gpu_busy_total_ns) averaged over the last 4 game frames instead of the game's ReportValue timestamp pairs (0x1c71310),
//     which our ReportValue never fills. DM_DOOM_DRS_GPU=0 keeps the game's own value.
enum {
    GOT_UNDOCKED_PERF = 0x48d5078, GOT_PERF_DIRTY = 0x48d5068, GOT_HANDHELD_FLAG = 0x48d5080, GOT_PERF_CACHE = 0x48d5050,
    GOT_FORCE_X = 0x48d53b8, GOT_FORCE_Y = 0x48d53c0, GOT_DROP_MS = 0x48d5460, GOT_RAISE_MS = 0x48d5470, GOT_DRS_STATE = 0x48d40e8,
    FN_CVAR_SET = 0x120450, FN_GAME_GPU_TIME = 0x1c71310, CUT_LIGHT = 0x1c06568, CUT_RES = 0x1c26ee0,
};
static _Atomic int g_light_want = -1;      // 1 / 0 = light mode on / off asked (env or gear), -1 = never asked: nothing is touched
static _Atomic int g_drs_fps = 0;          // DRS target fps, 0 = keep the game's thresholds
static float g_res;                        // DM_DOOM_RES fraction < 1 (0 = 100 %, nothing to manage)
static int g_gpu_feed = 1;                 // DM_DOOM_DRS_GPU=0: the controller keeps the game's own GPU time
static int g_gpubound = 1;                 // [r6b] DM_DOOM_DRS_GPUBOUND=0: report the real GPU time even when the frame is CPU-bound
static int g_smart = 2;                    // [r6c] DM_DOOM_DRS_SMART: 0 off, 1 efficiency + reachability, 2 efficiency only (default)
static int g_cut_light = -1, g_cut_res = -1, g_set_ok = -1;
__attribute__((weak)) uint64_t xbg_gpu_busy_total_ns(void) { return 0; }   // weak definition (see hle_oe.c); the Vulkan engine has the strong one

static int word_is(uint32_t off, uint32_t w) { uint32_t v; if (!find_main() || g_size < off + 4) return 0; memcpy(&v, g_base + off, 4); return v == w; }
static int cut_light(void) { if (g_cut_light < 0) g_cut_light = word_is(CUT_LIGHT, 0x7a400924u); return g_cut_light; }
static int cut_res(void) { if (g_cut_res < 0) g_cut_res = word_is(CUT_RES, 0x3400242eu); return g_cut_res; }
// JNI (DmActivity's game-files check, BUGS_AND_FIXES 2.45): the r6 patches (v0.4.5: light mode + fixed 3D scale) in the loaded main:
// 1 both, 0 one missing (game files from an older setup), -1 not checked yet (only the values the [r6] start line computed, never a fresh read).
int dm_doom_r6_patches(void) { return g_cut_light < 0 || g_cut_res < 0 ? -1 : g_cut_light && g_cut_res; }
static int set_ok(void) {   // idCVar internal Set(cvar, const char *, force) = the string path of SetInteger (main+0x1206a0 calls it)
    if (g_set_ok >= 0) return g_set_ok;
    static const uint32_t w[4] = {0xa9bc5ffcu, 0xa90157f6u, 0xa9024ff4u, 0xa9037bfdu};
    g_set_ok = find_main() && !memcmp(g_base + FN_CVAR_SET, w, sizeof w);
    if (!g_set_ok) LOG("[r6] idCVar::Set not at the expected address (another main build): fixed 3D scale / DRS target are not managed");
    return g_set_ok;
}
static uint8_t *cvar_at(uint32_t got, const char *name) {
    if (!find_main()) return NULL;
    uint64_t o = *(volatile uint64_t *)(g_base + got);
    if (!ptr_ok(o)) return NULL;
    const char *nm = *(const char *const *)(uintptr_t)(o + 0x50);
    if (!nm || !ptr_ok((uint64_t)(uintptr_t)nm & ~7ull) || strcmp(nm, name)) return NULL;
    return (uint8_t *)(uintptr_t)o;
}
static uint8_t *data_at(uint32_t got) {   // a main global through its GOT slot (validated to lie inside main)
    if (!find_main()) return NULL;
    uint64_t o = *(volatile uint64_t *)(g_base + got);
    return in_main(o) ? (uint8_t *)(uintptr_t)o : NULL;
}
static float cvar_f(const uint8_t *cv) { return *(volatile const float *)(cv + 0x34); }
// Set a float cvar through the game's own setter (string + int + float, like the console), only when it differs from what the
// setter would store; returns 1 when it wrote.
static int cvar_set_f(uint8_t *cv, const char *fmt, double v) {
    char s[32]; snprintf(s, sizeof s, fmt, v);
    if (fabsf(cvar_f(cv) - strtof(s, NULL)) < 1e-4f) return 0;
    ((void (*)(void *, const char *, int))(uintptr_t)(g_base + FN_CVAR_SET))(cv, s, 1);
    return 1;
}

static void light_frame(void) {
    int want = atomic_load(&g_light_want);
    if (want < 0 || !cut_light()) return;
    uint8_t *cv = cvar_at(GOT_UNDOCKED_PERF, "r_nxUndockedPerf"), *dirty = data_at(GOT_PERF_DIRTY), *flag = data_at(GOT_HANDHELD_FLAG), *perf = data_at(GOT_PERF_CACHE);
    if (!cv || !dirty || !flag || !perf) return;
    static int pending, waited, tries;
    volatile int32_t *iv = (volatile int32_t *)(cv + 0x30);
    int cur = *iv, target = want ? 2 : cur == 2 ? 1 : cur;
    if (cur != target) {
        *iv = target; __atomic_store_n(dirty, 1, __ATOMIC_RELEASE);
        pending = 1; waited = 0; tries = 0;
        LOG("[light] %s: r_nxUndockedPerf %d -> %d, preset re-apply requested (perfMode %d)", want ? "on (Switch handheld quality preset)" : "off (docked preset)", cur, target, *(volatile int32_t *)perf);
    }
    if (pending) {
        int expect = want ? 1 : (*(volatile int32_t *)perf == 0 && *iv != 0);
        if (*(volatile uint8_t *)flag == expect) { pending = 0; LOG("[light] preset applied: handheld flag %d (%s caps for the 3D resolution)", expect, expect ? "handheld 0.8x0.9 max" : "docked 1.0 max"); }
        else if (++waited > 30) {   // the store raced the function's own clear of the dirty byte: ask again
            waited = 0; __atomic_store_n(dirty, 1, __ATOMIC_RELEASE);
            if (++tries == 10) { pending = 0; LOG("[light] preset not applied after 10 requests (handheld flag %d, want %d)", *(volatile uint8_t *)flag, expect); }
        }
    }
}

static void res_drs_frame(int drs_on) {
    if (!set_ok()) return;
    if (g_res > 0 && cut_res()) {
        uint8_t *fx = cvar_at(GOT_FORCE_X, "rs_forceFractionX"), *fy = cvar_at(GOT_FORCE_Y, "rs_forceFractionY");
        if (fx && fy) {
            double want = drs_on ? 0.0 : g_res;
            int w = cvar_set_f(fx, "%.3f", want); w |= cvar_set_f(fy, "%.3f", want);
            if (w) LOG("[res] 3D scale %s: rs_forceFractionX/Y %.3f", drs_on ? "handed to dynamic resolution" : "fixed", want);
        }
    }
    int fps = atomic_load(&g_drs_fps);
    if (drs_on && fps > 0) {
        uint8_t *dr = cvar_at(GOT_DROP_MS, "rs_dropMilliseconds"), *ra = cvar_at(GOT_RAISE_MS, "rs_raiseMilliseconds");
        if (dr && ra) {
            double drop = 1000.0 / fps;
            int w = cvar_set_f(dr, "%.2f", drop); w |= cvar_set_f(ra, "%.2f", drop * 0.945);
            if (w) LOG("[drs] target %d fps: rs_dropMilliseconds %.2f rs_raiseMilliseconds %.2f", fps, cvar_f(dr), cvar_f(ra));
        }
    }
}

// Host call 0x95 (main+0x1c5b27c, in place of bl 0x1c71310; render frame, once per game frame): x0 = GPU microseconds of the last
// frame for the DRS controller -- the original value while DRS is off, ours while it is on. While on, what is reported depends on
// the mode (logged as mode=gpu|cpu|geom):
//   gpu  -- the real GPU time: our Vulkan GPU busy time averaged over the last 4 game frames.
//   cpu  -- [r6b, smoothed in r6c] the frame is not GPU-bound: min(GPU, 0.97 x rs_raiseMilliseconds), so DRS holds or climbs back.
//           The controller only knows "GPU time vs drop / raise ms"; at a CPU-bound spot (Mi 13, ~1800 draws: translator 14.5 ms,
//           frame 24 ms, GPU busy 16.7 ms) it dropped to its 0.5x0.5 minimum and gained nothing. idle = wall time per game frame (the
//           interval between these calls) - GPU busy, both averaged over the last DRS_BOUND_WIN frames (~0.4 s at 60 fps; r6b used
//           per-frame streaks on a 4-frame window and flapped, r6b logcat). bound=cpu when the averaged idle > max(2 ms, 15 % of the
//           frame), back to bound=gpu when it is < max(1 ms, 8 %) (hysteresis), at least DRS_DWELL_NS between two flips. Pause /
//           loading (long frames) count as cpu. DM_DOOM_DRS_GPUBOUND=0: never cpu (the r6 behaviour).
//   geom -- [r6c] lowering the resolution does not pay here (DM_DOOM_DRS_SMART, default 1, 0 = off). Field (Mi 13 Pro, r6b logcat):
//           in the geometry-heavy spot the scale fell to 0.5x0.5 with the GPU still ~17 ms > drop 16.67 -- the part of the frame that
//           does not scale with pixels (geometry, shadows, compute) is near the target by itself, so DRS mostly blurred.
// [r6c] Efficiency model ("does lowering help?"): GPU ms = a + b x s, s = scale area X*Y. Every frame the pair (mean area of the frames
// inside the GPU window -- DRS_LAG..DRS_LAG+3 frames back, our busy time retires ~3 frames late --, 4-frame GPU ms) enters a window of
// DRS_FIT_N frames (~1.6 s); with >= DRS_FIT_MIN pairs spread over >= DRS_FIT_RANGE of area a least-squares fit renews a and b
// (b < 0 -> 0, a < 0 -> 0); a fit whose rms exceeds DRS_FIT_RESID of the mean GPU time means two scenes in the window (a scene change
// met a scale change): the older half is dropped and the model is not renewed. Without spread (the scale holds) the model is kept, but
// when the GPU time differs from a + b x s by more than DRS_STALE for DRS_DWELL_NS the scene changed: the window is cleared, the model
// dropped and the mode goes back to gpu at once, so the controller may lower again and a fresh fit is made on the new scene.
// The model gives the lowest area worth having ("floor"):
//   - an area cut at s buys b*s / (a + b*s) of what a fully pixel-proportional frame would save; below s_e = EFF_MIN*a / ((1-EFF_MIN)*b)
//     that is < DRS_EFF_MIN (28 %): floor = s_e;
//   - DM_DOOM_DRS_SMART=1 also: a + b x DRS_S_MIN >= rs_dropMilliseconds (even the controller's 0.5x0.5 minimum does not reach the
//     target; cleared below 0.94 x drop) -> floor = 1.0: keep full resolution, the fps is set by the fixed part anyway.
//     DM_DOOM_DRS_SMART=2: the efficiency floor only (keep lowering while it buys >= 28 %, even with the target out of reach).
// gpu -> geom when the controller's area is < floor - 0.05 and GPU > raise for DRS_SUSTAIN_NS (no model yet: area < 0.9 and GPU > drop
// for 2 x DRS_DWELL_NS -- stuck low, so climb to measure); geom -> gpu when GPU < 0.97 x drop or the floor fell below area - 0.12 for
// DRS_SUSTAIN_NS (0.12 > one raise step, so a hold that overshoots does not flip it), or at once when the model is dropped; at least
// DRS_DWELL_NS after the previous decision. In geom the controller gets min(GPU, 0.92 x raise) while the area is below floor - 0.02 (it
// raises the area x1.087 per 3 frames: 0.5x0.5 -> 1.0 in ~1 s) and min(GPU, (raise + drop) / 2) at / above it (the controller's dead
// band: it holds). A same-cost scene with another pixel share (a fight full of effects where the geometry was) looks the same at a
// held scale, so every DRS_PROBE_NS in geom with GPU > drop one frame reports DRS_PROBE_STEP x drop (the controller cuts the area by
// ~1/1.2) and the next DRS_DWELL_NS hold it: the window gets spread, the fit is renewed and the floor follows the scene.
// Priority: cpu (bound=cpu) > geom > gpu. Python model of these rules (toy controller, BUGS_AND_FIXES 2.18 r6c): geometry-bound
// (a 14.4, b 10.4) -> geom, back to 1.0; pixel-bound (a 8, b 14) -> gpu, area ~0.59 at the target; CPU-bound -> cpu, 1.0.
#define DRS_RING        128                 // per-frame rings (GPU busy total, wall time, scale area); power of two
#define DRS_BOUND_WIN   24                  // frames averaged for the CPU-bound flag (~0.4 s at 60 fps)
#define DRS_DWELL_NS    500000000ull        // minimum time between two decisions (bound flag, efficiency mode) and for a stale model
#define DRS_SUSTAIN_NS  250000000ull        // an efficiency-mode condition must hold this long
#define DRS_LAG         3                   // frames between a frame's scale and the GPU busy time that contains it
#define DRS_FIT_N       96                  // (area, GPU ms) pairs in the fit window (~1.6 s at 60 fps)
#define DRS_FIT_MIN     24                  // pairs needed for a fit
#define DRS_FIT_RANGE   0.12                // area spread needed for a fit
#define DRS_EFF_MIN     0.28                // a further area cut must still buy >= 28 % of the pixel-proportional saving
#define DRS_S_MIN       0.25                // the controller's lowest area (docked minimum 0.5 x 0.5)
#define DRS_STALE       0.20                // |GPU - model| / model above this for DRS_DWELL_NS = the scene changed
#define DRS_FIT_RESID   0.12                // fit rms above this share of the mean GPU time = two scenes in the window
#define DRS_PROBE_NS    3000000000ull       // geom: re-measure (one area step down) at most this often
#define DRS_PROBE_STEP  1.20                // geom probe: reported drop x this for one frame = the controller cuts the area by ~1/1.2
enum { MODE_GPU, MODE_CPU, MODE_GEOM };
static const char *const g_mode_name[3] = {"gpu", "cpu", "geom"};

void doom_drs_gpu_svc(uint64_t *regs) {
    uint64_t game = find_main() ? ((uint64_t (*)(void))(uintptr_t)(g_base + FN_GAME_GPU_TIME))() : 0;
    uint8_t *cv = drs_cvar();
    int on = cv && *(volatile int32_t *)(cv + 0x30) != 0;
    uint8_t *st = data_at(GOT_DRS_STATE), *dr = cvar_at(GOT_DROP_MS, "rs_dropMilliseconds"), *ra = cvar_at(GOT_RAISE_MS, "rs_raiseMilliseconds");
    double drop = dr ? cvar_f(dr) : 0, raise = ra ? cvar_f(ra) : 0;
    static uint64_t ring[DRS_RING], tring[DRS_RING], next_log; static float sring[DRS_RING]; static unsigned n;
    uint64_t tot = xbg_gpu_busy_total_ns(), us = game, tnow = now_ns();
    double s_now = on && st ? (double)*(const float *)st * *(const float *)(st + 4) : -1;   // the controller's current area
    ring[n & (DRS_RING - 1)] = tot; tring[n & (DRS_RING - 1)] = tnow; sring[n & (DRS_RING - 1)] = (float)s_now;
    double ours = -1, frame = -1, gpu_b = -1, frame_b = -1, s_gpu = -1;
    if (n >= 4 && tot) { ours = (double)(tot - ring[(n - 4) & (DRS_RING - 1)]) / 4e6; frame = (double)(tnow - tring[(n - 4) & (DRS_RING - 1)]) / 4e6; }   // ms per game frame over the last 4 frames
    if (n >= DRS_BOUND_WIN && tot && ring[(n - DRS_BOUND_WIN) & (DRS_RING - 1)]) {
        unsigned j = (n - DRS_BOUND_WIN) & (DRS_RING - 1);
        gpu_b = (double)(tot - ring[j]) / (DRS_BOUND_WIN * 1e6); frame_b = (double)(tnow - tring[j]) / (DRS_BOUND_WIN * 1e6);
    }
    if (n >= DRS_LAG + 4) {   // mean area of the frames whose GPU work is in the 4-frame window
        double sum = 0; int k;
        for (k = 0; k < 4; k++) { float v = sring[(n - DRS_LAG - (unsigned)k) & (DRS_RING - 1)]; if (v <= 0) break; sum += v; }
        if (k == 4) s_gpu = sum / 4;
    }
    n++;

    // CPU-bound flag on the averaged idle
    static int cpu_bound; static uint64_t bound_t;
    double idle = frame_b > 0 && gpu_b >= 0 ? frame_b - gpu_b : 0;   // may be slightly negative (busy time lags the wall window)
    if (frame_b > 0 && gpu_b >= 0 && tnow - bound_t >= DRS_DWELL_NS) {
        double hi = frame_b * 0.15 > 2 ? frame_b * 0.15 : 2, lo = frame_b * 0.08 > 1 ? frame_b * 0.08 : 1;
        if (cpu_bound ? idle < lo : idle > hi) { cpu_bound = !cpu_bound; bound_t = tnow; }
    }

    // efficiency model and mode (see above)
    static float fs[DRS_FIT_N], fg[DRS_FIT_N]; static unsigned fn, fw;   // fit window: ring, the newest fn pairs end before fw
    static int model, unreach, smode = MODE_GPU, was_on = -1; static double ma, mb; static uint64_t stale_t, cond_t, mode_t, probe_t;
    static unsigned nmodel, nprobe;
    int smart = on && g_gpu_feed && g_smart && drop > 0 && raise > 0;
    if (on != was_on || !smart) { was_on = on; fn = fw = 0; model = unreach = 0; smode = MODE_GPU; stale_t = cond_t = 0; }
    double floor_s = 0, pred = -1;
    if (smart && ours > 0.5 && frame < 100 && s_gpu > 0) {
        fs[fw] = (float)s_gpu; fg[fw] = (float)ours; fw = (fw + 1) % DRS_FIT_N; if (fn < DRS_FIT_N) fn++;
        if (fn >= DRS_FIT_MIN) {
            double smin = 9, smax = -9, ss = 0, sg = 0, sss = 0, ssg = 0;
            for (unsigned k = 0; k < fn; k++) {
                unsigned q = (fw + DRS_FIT_N - fn + k) % DRS_FIT_N;
                double s = fs[q], g = fg[q];
                if (s < smin) smin = s;
                if (s > smax) smax = s;
                ss += s; sg += g; sss += s * s; ssg += s * g;
            }
            if (smax - smin >= DRS_FIT_RANGE) {
                double ms = ss / fn, mg = sg / fn, var = sss / fn - ms * ms, cov = ssg / fn - ms * mg;
                double b = var > 1e-6 ? cov / var : 0, a = mg - b * ms, e2 = 0;
                if (b < 0) { b = 0; a = mg; }
                if (a < 0) { a = 0; b = mg / ms; }
                for (unsigned k = 0; k < fn; k++) { unsigned q = (fw + DRS_FIT_N - fn + k) % DRS_FIT_N; double e = fg[q] - a - b * fs[q]; e2 += e * e; }
                if (sqrt(e2 / fn) > DRS_FIT_RESID * mg) fn /= 2;   // two scenes in the window (a scene change met a scale change): keep the newer half
                else {
                    if (!model && nmodel++ < 64)
                        LOG("[drs] eff model: gpu = %.2f + %.2f x area ms (%u frames, area %.2f..%.2f, rms %.2f ms): full resolution %.2f ms, pixel share %.2f",
                            a, b, fn, smin, smax, sqrt(e2 / fn), a + b, a + b > 0 ? b / (a + b) : 0);
                    ma = a; mb = b; model = 1; stale_t = 0;
                }
            }
        }
        if (model) {
            pred = ma + mb * s_gpu;
            if (pred > 0.5 && fabs(ours - pred) > DRS_STALE * pred) {
                if (!stale_t) stale_t = tnow;
                else if (tnow - stale_t >= DRS_DWELL_NS) {
                    if (nmodel++ < 64)
                        LOG("[drs] eff model dropped: gpu %.2f ms vs %.2f predicted at area %.2f for %.1f s (scene changed)%s", ours, pred, s_gpu,
                            (tnow - stale_t) / 1e9, smode == MODE_GEOM ? ": mode gpu, DRS may lower again" : "");
                    fn = fw = 0; model = unreach = 0; stale_t = cond_t = 0;
                    if (smode == MODE_GEOM) { smode = MODE_GPU; mode_t = tnow; }
                }
            } else stale_t = 0;
        }
    }
    if (smart && model) {
        double se = mb > 1e-3 ? DRS_EFF_MIN * ma / ((1 - DRS_EFF_MIN) * mb) : 1.0;
        if (se > 1) se = 1;
        if (g_smart == 1) {
            double gmin = ma + mb * DRS_S_MIN;
            if (!unreach && gmin >= drop) unreach = 1;
            else if (unreach && gmin < 0.94 * drop) unreach = 0;
        }
        floor_s = unreach ? 1.0 : se;
    } else if (smart && smode == MODE_GEOM) floor_s = 1.0;   // probe climb without a model
    if (smart && ours >= 0 && s_now > 0) {
        int want = smode; uint64_t need = DRS_SUSTAIN_NS;
        if (smode == MODE_GPU) {
            if (model ? s_now < floor_s - 0.05 && ours > raise : s_now < 0.9 && ours > drop) want = MODE_GEOM;
            if (!model) need = 2 * DRS_DWELL_NS;   // stuck low and over the target with no model: climb to measure
        } else if (ours < 0.97 * drop || (model && floor_s < s_now - 0.12)) want = MODE_GPU;
        if (want != smode) {
            if (!cond_t) cond_t = tnow;
            if (tnow - cond_t >= need && tnow - mode_t >= DRS_DWELL_NS) { smode = want; mode_t = tnow; cond_t = 0; }
        } else cond_t = 0;
    }

    int feed = on && g_gpu_feed && ours >= 0;
    // The CPU hold applies only while the GPU itself is within the target: when GPU time is over the drop threshold, lowering the
    // resolution still shortens the frame (02.11 mt_drs2: bound=cpu at gpu 23.3 ms / frame 24.8 ms held 1.00x1.00 at ~40 fps).
    int mode = feed ? (g_gpubound && cpu_bound && ours < drop ? MODE_CPU : smode) : MODE_GPU;
    double reported = ours;
    if (feed) {
        double r = 0, mid = 0.5 * (raise + drop);
        if (mode == MODE_CPU) r = 0.97 * raise;
        else if (mode == MODE_GEOM) {
            uint64_t since = probe_t > mode_t ? probe_t : mode_t;
            if (ours > drop && tnow - since >= DRS_PROBE_NS) {   // re-measure: one step down, then hold it (see above)
                reported = r = DRS_PROBE_STEP * drop; probe_t = tnow;
                if (nprobe++ < 32) LOG("[drs] geom probe: one area step down from %.2f to re-measure the pixel share (gpu %.2f ms, eff %.2f)", s_now, ours,
                                       model && ma + mb > 0 ? mb / (ma + mb) : -1.0);
            } else r = tnow - probe_t < DRS_DWELL_NS || s_now >= floor_s - 0.02 ? mid : 0.92 * raise;
        }
        if (r > 0 && reported > r) reported = r;
        us = (uint64_t)(reported * 1000.0 + 0.5);
    }
    regs[0] = us;

    char effs[96];
    if (model) snprintf(effs, sizeof effs, "%.2f (%.1f+%.1fxS ms, floor %.2f%s)", ma + mb > 0 ? mb / (ma + mb) : 0, ma, mb, floor_s, unreach ? " target out of reach" : "");
    else snprintf(effs, sizeof effs, "n/a");
    static int last_mode = MODE_GPU; static uint64_t mlog_t; static unsigned mlog_n, mlog_skip;
    if (mode != last_mode) {   // transitions, at most one line per second (the skipped ones are counted)
        if (feed && tnow - mlog_t >= 1000000000ull && mlog_n < 300) {
            char skip[48] = "";
            if (mlog_skip) snprintf(skip, sizeof skip, " [%u changes not logged]", mlog_skip);
            LOG("[drs] mode %s -> %s (gpu %.2f ms, frame %.2f, idle %.2f, area %.2f, eff %s)%s", g_mode_name[last_mode], g_mode_name[mode], ours, frame_b, idle,
                s_now, effs, skip);
            mlog_t = tnow; mlog_n++; mlog_skip = 0;
        } else if (feed) mlog_skip++;
        last_mode = mode;
    }
    uint64_t now = now_ns();
    if (now >= next_log) {
        next_log = now + 3000000000ull;
        static int said_off;
        if (on) {
            said_off = 0;
            LOG("[drs] gpu=%.2f ms target=%d fps (drop %.2f / raise %.2f ms) scale=%.2fx%.2f (controller %.3f) feed=%s game-timer=%.2f ms "
                "frame=%.2f idle=%.2f bound=%s eff=%s mode=%s reported=%.2f",
                ours, atomic_load(&g_drs_fps), dr ? cvar_f(dr) : -1.f, ra ? cvar_f(ra) : -1.f, st ? *(const float *)st : -1.f, st ? *(const float *)(st + 4) : -1.f,
                st ? *(const float *)(st + 0x14) : -1.f, feed ? "ours" : "game", game / 1000.0,
                frame_b, idle, cpu_bound ? "cpu" : "gpu", effs, g_mode_name[mode], us / 1000.0);
        } else if (!said_off) {
            said_off = 1;
            LOG("[drs] off (rs_enable 0): the controller keeps the game's own GPU time (%.2f ms); ours %.2f ms/frame, scale %.2fx%.2f", game / 1000.0, ours,
                st ? *(const float *)st : -1.f, st ? *(const float *)(st + 4) : -1.f);
        }
    }
}

static void r6_init(void) {
    // A gear change made after the environment was set (dialog opened while the game was still loading) is newer: keep it.
    const char *e = getenv("DM_DOOM_LIGHT");
    if (e && *e && atomic_load(&g_light_want) < 0) atomic_store(&g_light_want, *e == '1');
    e = getenv("DM_DOOM_DRS_FPS");
    if (e && *e && !atomic_load(&g_drs_fps)) { int f = atoi(e); atomic_store(&g_drs_fps, f >= 20 && f <= 240 ? f : 0); }
    e = getenv("DM_DOOM_RES");
    if (e && *e) { double f = atof(e); g_res = f >= 0.25 && f < 0.999 ? (float)f : 0; }
    e = getenv("DM_DOOM_DRS_GPU"); g_gpu_feed = !(e && *e == '0');
    e = getenv("DM_DOOM_DRS_GPUBOUND"); g_gpubound = !(e && *e == '0');
    e = getenv("DM_DOOM_DRS_SMART"); g_smart = e && *e == '0' ? 0 : e && *e == '1' ? 1 : 2;   // default 2 since 02.11: with 1 a heavy spot whose target is out of reach went back to full res (37 fps instead of ~50)
    LOG("[r6] light mode %s (patch %s), fixed 3D scale %.3f (patch %s), DRS target %d fps, GPU time for DRS: %s (host call 0x95 %s), "
        "CPU-bound hold %s (DM_DOOM_DRS_GPUBOUND), efficiency check %s (DM_DOOM_DRS_SMART)",
        atomic_load(&g_light_want) == 1 ? "on" : "off", cut_light() ? "present" : "MISSING", g_res, cut_res() ? "present" : "MISSING", atomic_load(&g_drs_fps),
        g_gpu_feed ? "ours" : "the game's", gate_svc_present(SVC_HOST_DOOM_GPU_TIME) ? "present" : "MISSING", g_gpubound ? "on" : "off",
        g_smart == 0 ? "off" : g_smart == 2 ? "on (efficiency only)" : "on");
}
// JNI (jni_android.c): light mode live switch; 0 = the loaded main lacks the patch (remake main.patched.bin), 1 = applies now / at start.
int dm_doom_light_set(int on) {
    atomic_store(&g_light_want, on ? 1 : 0);
    LOG("[light] gear: %s", on ? "on" : "off");
    return !find_main() || cut_light();
}
// JNI: DRS target fps (30 / 45 / 60), applied on the next frames while DRS is on.
void dm_doom_drs_fps_set(int fps) { atomic_store(&g_drs_fps, fps >= 20 && fps <= 240 ? fps : 0); LOG("[drs] gear: target %d fps", fps); }

static void drs_write(uint8_t *cv, int v, const char *who) {
    int32_t cur = *(volatile int32_t *)(cv + 0x30);
    ((void (*)(void *, int, int))(g_base + FN_CVAR_SET_INT))(cv, v, 1);   // = SetChromaticAberration's call (menu setter)
    LOG("[drs] %s: rs_enable %d -> %d (value \"%s\"); requesting a profile save", who, cur, *(volatile int32_t *)(cv + 0x30), *(const char *const *)(cv + 0x10));
    g_save_tries = PROFILE_SAVE_TRIES;
}

static void drs_frame_obs(const uint64_t *r) {     // idCommonLocal::Frame entry: the game's main thread, between frames
    uint8_t *cv = drs_cvar();
    light_frame();                                 // [r6]
    res_drs_frame(cv && *(volatile int32_t *)(cv + 0x30) != 0);
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
        int ld = atomic_load(&g_load_done), loaded = 0;
        if (ld > 0 && atomic_compare_exchange_strong(&g_load_done, &ld, ld - 1) && ld == 1) {   // a profile load finished a few frames ago
            atomic_store(&g_loaded_once, 1); loaded = 1;
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
        // the controller settings (hle_doom_settings.c) on the same load tracking; their save requests are served just below
        doom_settings_frame(atomic_load(&g_loaded_once) && !atomic_load(&g_loading) && atomic_load(&g_load_done) == 0, loaded);
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

// hle_doom_settings.c: the profile seen at gate 34 (NULL before) and a profile save request, served between frames above (the menu's
// call main+0x1eeb7e0; skipped while the profile is busy, retried for ~600 frames).
uint8_t *doom_profile(void) { return profile_ok(); }
void doom_profile_save_request(void) { if (fns_ok()) g_save_tries = PROFILE_SAVE_TRIES; }
// hle_hid.c (DM_INPUT_OBS=1) hands its own callbacks for gates 30 / 34 here, so both run.
void doom_drs_obs_chain(int id, void (*fn)(const uint64_t *)) {
    if (id == OBS_DOOM_FRAME) g_next_frame = fn; else if (id == OBS_DOOM_PROFILE_STATE) g_next_profile = fn;
}
// hle_args.c build() (GetHostArgc, after the modules are loaded, before the game's main loop).
void doom_drs_init(void) {
    static int done; if (done) return; done = 1;
    r6_init();
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
int dm_doom_light_set(int on) { (void)on; return 0; }
void dm_doom_drs_fps_set(int fps) { (void)fps; }
#endif
