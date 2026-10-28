// [DOOM] Command line for the game (nn::os::GetHostArgc / GetHostArgv). id Tech 6 parses "+set <cvar> <value>" from it at
// startup, which is how options reach the renderer without patching code:
//   DM_DOOM_RES=<0.5..1>   3D render fraction (rs_forceFractionX / Y): the game renders the 3D view at that fraction of
//                          1280x720 and upscales it itself (with its TAA); the UI stays at full resolution. 0.75 = 960x540.
//   DM_DOOM_DRS=1          keep DOOM's dynamic resolution (rs_enable) on; default off (+set rs_enable 0).
//   DM_DOOM_GEOMCULL=1     keep DOOM's triangle culling (default off: it drops visible triangles on our GPU path).
//   DM_DOOM_ADAPTIVE_TICK=0  keep the fixed 30 Hz game tic (default: adaptive tick, game time follows real frame time).
//   DM_DOOM_TICK_MIN / DM_DOOM_TICK_MAX  adaptive tick Hz range (default 30 / 60).
//   DM_DOOM_ARGS="..."     extra arguments, space separated, appended verbatim (e.g. "+set rs_showResolutionChanges 1").
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef DM_GAME_DOOM
static char *g_argv[128];   // ~60 used by the defaults below (sticks + tick cvars), room for DM_DOOM_ARGS
static int g_argc = -1;

static void add(const char *s) { if (g_argc < 127) g_argv[g_argc++] = strdup(s); }

static void build(void) {
    if (g_argc >= 0) return;
    g_argc = 0; add("doom");
    const char *res = getenv("DM_DOOM_RES");
    if (res && *res) {
        double f = atof(res);
        if (f >= 0.25 && f <= 1.0) {
            char v[32]; snprintf(v, sizeof v, "%.3f", f);
            add("+set"); add("rs_forceFractionX"); add(v);
            add("+set"); add("rs_forceFractionY"); add(v);
        }
    }
    // DOOM's own dynamic resolution (rs_enable) reacts to the GPU time it measures; with correct timestamps it now scales the 3D view
    // down on this GPU. Off unless DM_DOOM_DRS=1 (a fixed DM_DOOM_RES fraction is the explicit way to trade sharpness for speed).
    const char *drs = getenv("DM_DOOM_DRS");
    if (!(drs && atoi(drs)) && !(res && *res)) { add("+set"); add("rs_enable"); add("0"); }
    // Linear sticks for the on-screen controls (DM_DOOM_LINEAR_STICKS=0 keeps the game's response): DOOM defaults to a 0.35 dead
    // zone (joy_deadZone, joy_nxDefaultDeadzone), a power curve, a look slew limit (joy_dampenLook with joy_deltaPerMSLook 0.003 per ms,
    // ~330 ms to full tilt), edge acceleration (+40% past 0.9) and stick smoothing. With these off the right stick's tilt is the turn
    // rate, so the touch look (DoomPadView feeds a finger's displacement as tilt x time) turns "a finger offset" into "a view angle",
    // independent of the swipe speed. Aim assist stays as the game sets it. On a controller change idNXJoystick copies
    // joy_nxDefaultDeadzone into joy_deadZone and joy_nxDefaultJoystickCurve into joy_gammaLook (the curve: 0 linear, 1 squared,
    // 2 power; main+0x1f2c800), so those defaults are set too.
    const char *lin = getenv("DM_DOOM_LINEAR_STICKS");
    if (!(lin && *lin == '0')) {
        static const char *const sets[][2] = {{"joy_deadZone", "0"}, {"joy_nxDefaultDeadzone", "0"}, {"joy_nxProDeadzone", "0"},
            {"joy_powerScale", "1"}, {"joy_gammaLook", "0"}, {"joy_nxDefaultJoystickCurve", "0"}, {"joy_dampenLook", "0"}, {"joy_deltaPerMSLook", "1000"},
            {"joy_edgeAccelerationScalar", "0"}, {"joy_mergedDeadZoneAngle", "0"},
            {"joy_smoothingEnabled", "0"}};
        for (unsigned i = 0; i < sizeof sets / sizeof sets[0]; i++) { add("+set"); add(sets[i][0]); add(sets[i][1]); }
    }
    // DOOM's triangle culling (r_geometryCulling / r_smallTriangleCulling: the geometry shader of the depth pre-pass) drops visible
    // triangles here -- black holes in models (28.10, owner's play test: almost gone with both off, ~1 fps / ~1 ms GPU cost). Off
    // until that shader runs correctly; DM_DOOM_GEOMCULL=1 keeps the game's culling.
    const char *gc = getenv("DM_DOOM_GEOMCULL");
    if (!(gc && atoi(gc))) { add("+set"); add("r_geometryCulling"); add("0"); add("+set"); add("r_smallTriangleCulling"); add("0"); }
    // Game speed vs our frame rate (docs/research/game_speed.md). The game frame loop (update main 0x1ecfa80 / 0x1ed0f00) runs
    // with com_fixedTic 1 (registered default "1"): residual time is zeroed and exactly one game frame of 1000/gameHz ms runs per
    // render frame, gameHz pinned to 30 (0x1f2b630). On the Switch render = 30 fps (present interval 2), so that is real time; we
    // present every 60 Hz beat, so at 45-60 fps the game ran 1.5-2x fast (and at 15 fps 0.5x). com_fixedTic 0 does not help above
    // 30 fps: the residual loop always runs >= 1 game frame per render frame, it only adds catch-up frames. The engine's own fix is
    // the adaptive tick inside the fixedTic branch: com_adaptiveTick 1 + com_adaptiveTickImmediateMode 1 (0x1ecf940) measures the
    // real render frame time each frame (Sys_Microseconds, i.e. our GetSystemTick), clamps it to [1/MaxHz, 1/MinHz] and sets
    // gameHz = ceil(1e6 / us), carrying the rounding error -- one game frame per render frame, each as long as the frame took.
    // The Switch shipped MinHz = MaxHz = 30 (adaptive in name only). Applies in gameplay (game state 6 with allowHzChange set by
    // the map); menus keep the fixed 30 Hz tic. DM_DOOM_ADAPTIVE_TICK=0 keeps the original fixed 30 Hz behaviour;
    // DM_DOOM_TICK_MIN / DM_DOOM_TICK_MAX set the Hz range (default 30..60: our pacing caps presents at 60; below 30 fps the game
    // slows down as on the Switch rather than running untested sub-30 Hz tics).
    const char *adt = getenv("DM_DOOM_ADAPTIVE_TICK");
    if (!(adt && *adt == '0')) {
        const char *mn = getenv("DM_DOOM_TICK_MIN"), *mx = getenv("DM_DOOM_TICK_MAX");
        int lo = mn && *mn ? atoi(mn) : 30, hi = mx && *mx ? atoi(mx) : 60;
        if (lo < 10) lo = 10; if (hi > 240) hi = 240; if (hi < lo) hi = lo;
        char vl[16], vh[16]; snprintf(vl, sizeof vl, "%d", lo); snprintf(vh, sizeof vh, "%d", hi);
        add("+set"); add("com_fixedTic"); add("1");   // the adaptive branch lives inside the fixedTic branch
        add("+set"); add("com_adaptiveTick"); add("1");
        add("+set"); add("com_adaptiveTickImmediateMode"); add("1");
        add("+set"); add("com_adaptiveTickMinHz"); add(vl);
        add("+set"); add("com_adaptiveTickMaxHz"); add(vh);
    }
    // Aim assist on the single-player methods (hle_hid.c DM_AA_STRENGTH, default 1): with aa_DESIGN_override 1 its base values are the
    // weapon's SP aim assist data (docs/research/aim_assist.md section 8). DM_AA_STRENGTH=0 keeps the game's (weak) MP-method assist.
    const char *aas = getenv("DM_AA_STRENGTH");
    if (!(aas && atof(aas) <= 0)) { add("+set"); add("aa_DESIGN_override"); add("1"); }
    const char *extra = getenv("DM_DOOM_ARGS");
    if (extra && *extra) {
        char *copy = strdup(extra), *save = NULL;
        for (char *t = strtok_r(copy, " ", &save); t; t = strtok_r(NULL, " ", &save)) add(t);
        free(copy);
    }
    g_argv[g_argc] = NULL;
    char line[2048]; int at = 0;
    for (int i = 0; i < g_argc && at < (int)sizeof line - 2; i++) at += snprintf(line + at, sizeof line - at, "%s%s", i ? " " : "", g_argv[i]);
    LOG("命令行 | command line: %s", line);
}

HLE_FORCE("_ZN2nn2os11GetHostArgcEv", "命令行：DM_DOOM_RES / DM_DOOM_ARGS 变成 id Tech 的 +set 参数", int, h_os_GetHostArgc, void) { build(); return g_argc; }
HLE_FORCE("_ZN2nn2os11GetHostArgvEv", "命令行：与 GetHostArgc 配对", char **, h_os_GetHostArgv, void) { build(); return g_argv; }
#endif
