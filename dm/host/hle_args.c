// [DOOM] Command line for the game (nn::os::GetHostArgc / GetHostArgv). id Tech 6 parses "+set <cvar> <value>" from it at
// startup, which is how options reach the renderer without patching code:
//   DM_DOOM_RES=<0.5..1>   3D render fraction (rs_forceFractionX / Y): the game renders the 3D view at that fraction of
//                          the output size (1280x720, or W x 720 with DM_DOOM_WIDE) and upscales it itself (with its TAA);
//                          the UI stays at full resolution. 0.75 = 960x540 at 16:9. [r6] Works with dynamic resolution off only with
//                          the CUT at main+0x1c26ec4 (before it the rs_enable 0 exit skipped the fraction); 1.0 adds nothing (a forced
//                          1.0 overrode DRS); while DRS is on hle_doom_drs.c sets the fraction to 0 so DRS decides (BUGS_AND_FIXES 2.18).
//   DM_DOOM_LIGHT=1, DM_DOOM_DRS_FPS=30|45|60, DM_DOOM_DRS_GPU=0  light mode / DRS target / DRS on the game's own GPU timer
//                          (hle_doom_drs.c, read by doom_drs_init below).
//   DM_DOOM_WIDE=1|0|<w>   widescreen Hor+ output W x 720 (default off; 1 = screen aspect; see doom_wide below).
//   DM_DOOM_ASPECT=auto|16:9|4:3|<a>:<b>|<w>  output aspect (the gear choice; auto = the screen's, also narrower; dm_doom_wide_w).
//   DM_DOOM_WIDE_LIVE=1    always render wide; the gear checkbox switches live (off = presenter 16:9 centre crop; BUGS 2.12).
//   DM_DOOM_DRS=<n>        debugging: +set rs_enable <n> at every launch. Unset (normal): nothing -- rs_enable's default is patched
//                          to "0" and the Video menu / gear checkbox (one shared state, saved in the game profile) decide.
//   DM_DOOM_GEOMCULL=1     keep DOOM's triangle culling (default off: it drops visible triangles on our GPU path).
//   DM_DOOM_ADAPTIVE_TICK=0  keep the fixed 30 Hz game tic (default: adaptive tick, game time follows real frame time).
//   DM_DOOM_TICK_MIN / DM_DOOM_TICK_MAX  adaptive tick Hz range (default 30 / 60; the gear's frame rate limit sets MAX = the limit,
//                          together with DM_PRESENT_MAX_HZ, so game time keeps real time at the capped present rate).
//   DM_DOOM_PROFILE_SAVE=0 keep the game's profile save gate (profile_requireSaveExplanation 1; default: +set ... 0, see build()).
//   DM_DOOM_ARGS="..."     extra arguments, space separated, appended verbatim (e.g. "+set rs_showResolutionChanges 1").
#include "hle.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef DM_GAME_DOOM
static char *g_argv[128];   // ~60 used by the defaults below (sticks + tick cvars), room for DM_DOOM_ARGS
static int g_argc = -1;

static void add(const char *s) { if (g_argc < 127) g_argv[g_argc++] = strdup(s); }

// [DOOM] Widescreen output (DM_DOOM_WIDE, default OFF until the EmergencyRender at 1600x720 is solved, BUGS_AND_FIXES 2.10). How the game picks its resolution (update main, build id 2847991952b3d7ab):
// there is no display-resolution query (no nn::oe::GetDefaultDisplayResolution import). Renderer init (0x1c05450, and the cold-start
// branch of EmergencyRender 0x1c06c70 at 0x1c071e4, taken only when the renderer is not up yet) reads r_mode ("0 == 720p, 1 == 1080p - Forces a certain resolution", default "0", registered at
// 0x1c2ea24, idCVar at GOT 0x48d4fe0) and, for r_mode 0 / 1, takes {u32 width, u32 height} from the rodata table at 0x39f1b10
// ({1280,720},{1920,1080}; any other r_mode -> immediate 1280x720), storing it in the one display-size global 0x573d1b0 (GOT 0x48d4fc0).
// Every size getter of the render system reads that global (0x1c04aa0 / 0x1c04ab0 GetWidth/GetHeight, 0x1c04ac0 / 0x1c04ad0 scanout
// width/height; GetPixelAspect is a constant 1.0), and from them come the window textures, the render targets, the per-frame
// gameFrameParms_t width/height (idCommonLocal::GameFrameParms -> idGameLocal+0x2bb398) and the GUI virtual size.
// Projection is Hor+ by construction: idView::SetFovXandY / CalcFovXandY / CalculateFovXandY take g_fov as the horizontal FOV *at
// 16:9* (vfov = 2*atan(tan(g_fov/2) * 0.5625)), then fovX = 2*atan(tan(vfov/2) * w/h) with w/h clamped to [1, 3] -- a wider output
// keeps the vertical FOV and shows more on the sides. idSWF::RenderInternal scales the 1280x720 Flash stage uniformly by
// min(w/stageW, h/stageH) and centres it, so menus / HUD are not stretched (centred, with the side space for anchored elements).
// So widening needs only the table: entry 0 becomes {W, 720} (and entry 1 {1.5 W, 1080} for an explicit r_mode 1), written before
// the game's renderer init reads it (the command line is fetched first). The table lives in .rodata, which the Android loader
// leaves writable (only [0, text_end) and the patch area are sealed). The presenter then gets a W x 720 image that fills the
// screen without side bars.
//   DM_DOOM_WIDE=1           W = 720 x screen aspect (front_game_res(0), rounded to 16), only when the screen is wider than 16:9
//   DM_DOOM_WIDE unset / 0   original 1280x720 (the presenter letterboxes it with side bars)
//   DM_DOOM_WIDE=<width>     explicit width (>= 720, rounded to 16)
// W is clamped to [720, 2160] (Auto: [960, 2160]; [4:3] narrower outputs below dm_doom_wide_w): 720 x 1 .. 720 x 3, the game's aspect
// clamp in its FOV math.
uint64_t front_game_res(int docked);
// [DOOM] DM_DOOM_WIDE_LIVE=1 (default off until phone-tested, BUGS_AND_FIXES 2.12): the gear dialog's "wide" checkbox switches live.
// The game has no runtime path that changes its output size (vid_restart = empty stub 0x1c06c40, the operation-mode message 30
// only re-tunes quality cvars in 0x1c064c0), so with this switch the game always renders W x 720 and "wide off" is the presenter
// cropping the centre 16:9 of it (vulkan_present.cpp dm_present_crop169) -- identical geometry, since the projection is Hor+ (same
// vertical FOV) and the SWF stage is centred at scale 1.
static int dm_doom_wide_live_env(void) { const char *e = getenv("DM_DOOM_WIDE_LIVE"); return e && *e == '1'; }
static int g_wide_patched;   // the r_mode table really holds the wide size (doom_wide succeeded)
// [4:3] Aspect ratio choice (DM_DOOM_ASPECT, gear "Aspect ratio", BUGS_AND_FIXES 2.20). The same r_mode table rewrite also gives
// outputs NARROWER than 16:9 (W < 1280 at height 720); checked in the base main (symbols):
//   * CalculateFovXandY 0x32c888: vfov = 2*atan(tan(clamp(g_fov,1,120)/2) * 0.5625), fovX = 2*atan(tan(vfov/2) * clamp(w*pa/h, 1, 3))
//     -> vertical FOV is kept, a 4:3 output shows less on the sides (the native 4:3 look); the aspect floor 1.0 = W 720.
//   * idSWF::RenderInternal 0x556d0: scale = min(vw/stageW, vh/stageH) * swf_safeFrame factor (+0x1cc), the stage centred on both
//     axes (vw/vh = idRenderModelGui::GetVirtualWidth/Height 0x15667e4 = the GUI viewport = output size). At 960x720 the 1280x720 HUD /
//     menus are scaled to 0.75 (960x540) and centred with 90 px empty above and below (3D fills them; SWFs with the bars flag +0x220
//     draw black bars there) -- nothing is cut off. Only the horizontal offset is exported to the scripts (+0x1c8 = 0 here).
//   * DRS / rs_forceFraction are fractions of the output size; the presenter (letterbox / GSR) takes any source size.
// So the narrow width only needs a floor: Auto never goes below 960 (4:3; below it the HUD shrinks under 0.75 on 3.5-4" screens);
// an explicit ratio / width may go down to 720 (the FOV aspect floor). The RenderTarget pool keeps its stock 117 MB minimum.
int front_screen_size(int *w, int *h);   // front_android.c: the game view's size as Java reported it (also below 16:9)
__attribute__((weak)) int front_screen_size(int *w, int *h) { (void)w; (void)h; return 0; }   // other front ends: unknown
enum { ASPECT_169, ASPECT_AUTO, ASPECT_FIXED };
static int g_aspect_choice = -1;   // what dm_doom_wide_w decided from (ASPECT_*)
static int g_live_wide;            // DM_DOOM_WIDE_LIVE=1 and the output is wider than 16:9: the presenter crop can switch it to 16:9
// "Match the screen": output width at height 720, unrounded and unclamped. Wider than 16:9: front_game_res(0) as before (exactly the
// widths of the old DM_DOOM_WIDE=1). Narrower: the view size itself (front_game_res reports 1280x720 for those); a screen within 5 %
// of 16:9 (e.g. a 16:9 handheld whose window lost a few pixels to a bar) stays 1280.
static unsigned auto_width(void) {
    const char *gr = getenv("DM_GAME_RES"); unsigned gw, gh, v = 1280;
    if (gr && sscanf(gr, "%ux%u", &gw, &gh) == 2 && gh) v = (unsigned)(720.0 * gw / gh + 0.5);   // DM_GAME_RES only as an aspect here
    else {
        uint64_t r = front_game_res(0); int sw = 0, sh = 0;
        if ((uint32_t)(r >> 32) == 720 && (uint32_t)r > 1280) v = (uint32_t)r;
        else if (front_screen_size(&sw, &sh) && sw > 0 && sh > 0 && (int64_t)sw * 9 < (int64_t)sh * 16) v = (unsigned)(720.0 * sw / sh + 0.5);
    }
    return v < 1280 && v >= 1216 ? 1280 : v;   // 1216 = 720 x 16/9 x 0.95
}
// Output width, or 0 = the original 1280x720. Pure computation from env + screen size, cached; also used by hle_svc.c app_pool()
// (called earlier than the command line) through doom_pool_plan. Inputs, first match wins:
//   DM_DOOM_WIDE=<width>  (>= 720; dm/env)  explicit width
//   DM_DOOM_ASPECT=auto | 16:9 | 4:3 | <a>:<b> | <width>  (the APK sets it from the gear choice)
//   DM_DOOM_WIDE=1 (compatibility) = auto; unset / 0 = 16:9
// With DM_DOOM_WIDE_LIVE=1 a 16:9 choice still renders at the screen aspect (wider than 16:9 only) and the presenter crops it (BUGS 2.12).
unsigned dm_doom_wide_w(void) {
    static int done; static unsigned w;
    if (done) return w;
    done = 1;
    const char *e = getenv("DM_DOOM_WIDE"), *a = getenv("DM_DOOM_ASPECT");
    long ew = e && *e ? atol(e) : 0;
    unsigned v = 1280; int ch = ASPECT_169;
    if (ew >= 720) { v = (unsigned)ew; ch = ASPECT_FIXED; }
    else if (a && *a) {
        double x = 0, y = 0;
        if (!strcmp(a, "auto")) ch = ASPECT_AUTO;
        else if (!strcmp(a, "16:9")) ch = ASPECT_169;
        else if ((sscanf(a, "%lf:%lf", &x, &y) == 2 || sscanf(a, "%lf/%lf", &x, &y) == 2) && x > 0 && y > 0) { v = (unsigned)(720.0 * x / y + 0.5); ch = ASPECT_FIXED; }
        else if (atol(a) >= 720) { v = (unsigned)atol(a); ch = ASPECT_FIXED; }
        else { ch = ASPECT_AUTO; LOG("[aspect] DM_DOOM_ASPECT=%s not understood (auto | 16:9 | 4:3 | <a>:<b> | <width>): auto", a); }
    } else if (e && *e && !(*e == '0' && !e[1])) ch = ASPECT_AUTO;
    if (ch == ASPECT_AUTO) v = auto_width();
    else if (ch == ASPECT_169 && dm_doom_wide_live_env()) { unsigned s = auto_width(); if (s > 1280) v = s; }   // render wide, crop to 16:9
    v = (v + 8) / 16 * 16;
    const unsigned lo = ch == ASPECT_AUTO ? 960 : 720;
    if (v < lo) v = lo; if (v > 2160) v = 2160;
    g_aspect_choice = ch; g_live_wide = dm_doom_wide_live_env() && v > 1280;
    LOG("[aspect] %s -> output %ux720 (DM_DOOM_ASPECT=%s, DM_DOOM_WIDE=%s%s)", ch == ASPECT_AUTO ? "auto (screen aspect)" : ch == ASPECT_169 ? "16:9" : "fixed",
        v, a ? a : "-", e ? e : "-", g_live_wide ? ", live crop available" : "");
    return w = v == 1280 ? 0 : v;
}
// vulkan_present.cpp (weak reference, first frame): 1 = start with the centre 16:9 crop (DM_DOOM_WIDE_LIVE=1, the choice is 16:9 and
// the game really renders wider than 16:9), 0 = show the whole frame.
int dm_doom_live_crop_initial(void) { dm_doom_wide_w(); return g_wide_patched && g_live_wide && g_aspect_choice == ASPECT_169; }
// [DOOM] The game's NVN VRAM pools (idNvnVRamAllocator; cvars registered at 0x1c4a040..0x1c4a150, read once by the VRAM allocator
// init 0x1c305e0, which renderer init calls at 0x1c054dc -- after the command line's +set are applied). Each pool is carved out of the
// game heap (heap = app pool - image, hle_svc.c) and zeroed; with zero-copy (xbg_game.cpp map_pool) it becomes AHardwareBuffer
// (dma-buf) memory, i.e. resident from the start. Stock sizes (MB) and what fills them:
//   TextureBuffer 1162.5  textures, VT pages, vertex/index/constant buffers -- the yellow EmergencyRender of BUGS 2.9
//   RenderTarget  117     screen-sized targets (~133 bytes per output pixel at 1280x720) + fixed atlases -- white EmergencyRender at 1600
//   Uncached      22.5    CPU-visible uncached (readbacks, queries)        -- magenta
//   Shader        5.5     shader code                                      -- blue
//   Dynamic       23      per-frame dynamic data                           -- green
// DM_DOOM_POOL_MARGIN (default 1; 16 GB phone): TextureBuffer 1536, RenderTarget 1.35 x max(117, 117*W/1280+4) rounded up to 4 MB,
// the three small pools x1.5 rounded up; hle_svc.c app_pool() adds the total increase + 512 MB of plain game-heap headroom (unless
// DM_APP_POOL_ADD_MB is set). DM_DOOM_POOL_MARGIN=0: stock sizes; DM_DOOM_WIDE then still raises RenderTarget to 117*W/1280+4 (the minimum
// that fits the wider targets) and the app pool by that difference only.
typedef struct { double tb, rt, unc, sh, dyn; int pool_add_mb; } doom_pools;
static const doom_pools g_stock_pools = {1162.5, 117, 22.5, 5.5, 23, 0};
// [mem] Margin level: 2 = full (above), 1 = half (TextureBuffer 1350, small pools x1.25, 256 MB heap headroom), 0 = stock.
// DM_DOOM_POOL_MARGIN=0/1/2 forces a level; unset = by RAM: MemTotal >= 14 GB (16 GB phones) -> 2, >= 10 GB (12 GB phones,
// MemTotal ~11 GB) -> 1, below -> 0. The full margin (+956 MB app pool, ~+444 MB resident) pushed a 12 GB Xiaomi 13 Pro into
// 4.9 GB of swap (MemFree 70-135 MB) and a 30 s level reload after death. Old values: "1" = full (now 2 also accepted).
static long mem_total_mb(void) {
    static long mb = -2; if (mb != -2) return mb; mb = -1;
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return mb;
    char line[128]; while (fgets(line, sizeof line, f)) { long kb; if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) { mb = kb / 1024; break; } }
    fclose(f); return mb;
}
static int margin_level(void) {
    static int lv = -1; if (lv >= 0) return lv;
    const char *e = getenv("DM_DOOM_POOL_MARGIN");
    if (e && *e) lv = *e == '0' ? 0 : *e == '1' || *e == '2' ? 2 - (*e == '1' && e[1] == 'h') : 2;   // "0" stock, "1"/"2" full, "1h" half
    else { long mb = mem_total_mb(); lv = mb < 0 ? 2 : mb >= 14 * 1024 ? 2 : mb >= 10 * 1024 ? 1 : 0; }
    LOG("[pools] margin level %d (%s, MemTotal %ld MB)", lv, e && *e ? "DM_DOOM_POOL_MARGIN" : "by RAM", mem_total_mb());
    return lv;
}
static int margin_on(void) { return margin_level() > 0; }
static doom_pools doom_pool_plan(void) {
    doom_pools p = g_stock_pools; unsigned w = dm_doom_wide_w();
    double rt_min = w > 1280 ? (117u * w + 1279) / 1280 + 4 : 117;   // minimum for the output width ([4:3] narrower: the stock 117)
    const int lv = margin_level();
    if (lv == 2) {
        p.tb = 1536; p.rt = ceil(rt_min * 1.35 / 4) * 4; p.unc = ceil(22.5 * 1.5); p.sh = ceil(5.5 * 1.5); p.dyn = ceil(23 * 1.5);
    } else if (lv == 1) {
        p.tb = 1350; p.rt = ceil(rt_min * 1.15 / 4) * 4; p.unc = ceil(22.5 * 1.25); p.sh = ceil(5.5 * 1.25); p.dyn = ceil(23 * 1.25);
    } else p.rt = rt_min;
    double extra = (p.tb - g_stock_pools.tb) + (p.rt - g_stock_pools.rt) + (p.unc - g_stock_pools.unc) + (p.sh - g_stock_pools.sh) + (p.dyn - g_stock_pools.dyn);
    p.pool_add_mb = (int)ceil(extra) + (lv == 2 ? 512 : lv == 1 ? 256 : 0);
    return p;
}
// hle_svc.c app_pool(): MB to add to the reported application pool (VRAM pool increase + heap headroom).
int dm_doom_pool_add_mb(void) { return doom_pool_plan().pool_add_mb; }
static void doom_pools_args(void) {
    doom_pools p = doom_pool_plan(), s = g_stock_pools;
    static const char *const names[5] = {"nvn_VRAMTextureBufferHeapSizeMB", "nvn_VRAMRenderTargetHeapSizeMB", "nvn_VRAMUncachedHeapSizeMB",
                                         "nvn_VRAMShaderHeapSizeMB", "nvn_VRAMDynamicHeapSizeMB"};
    const double v[5] = {p.tb, p.rt, p.unc, p.sh, p.dyn}, o[5] = {s.tb, s.rt, s.unc, s.sh, s.dyn};
    for (int i = 0; i < 5; i++) if (v[i] != o[i]) { char b[24]; snprintf(b, sizeof b, "%g", v[i]); add("+set"); add(names[i]); add(b); }
    LOG("[pools] VRAM MB: TextureBuffer %g RenderTarget %g Uncached %g Shader %g Dynamic %g (stock 1162.5/117/22.5/5.5/23; "
        "DM_DOOM_POOL_MARGIN=%d, wide %u); app pool +%d MB%s", p.tb, p.rt, p.unc, p.sh, p.dyn, margin_on(), dm_doom_wide_w(), p.pool_add_mb,
        getenv("DM_APP_POOL_ADD_MB") ? " (overridden by DM_APP_POOL_ADD_MB)" : "");
}
static void doom_wide(void) {
    unsigned w = dm_doom_wide_w();
    if (!w) { LOG("[wide] 16:9 output 1280x720 (aspect 16:9, or the screen is within 5 %% of 16:9)"); return; }
    uint8_t *mb = NULL; size_t msz = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { mb = g_mod[i].base; msz = g_mod[i].size; }
    enum { TAB = 0x39f1b10, STR_RMODE = 0x39a107e, STR_RTPOOL = 0x395b436 };   // (the pool cvar name is checked so a wrong build does not widen without its pool)
    static const uint32_t orig[4] = {1280, 720, 1920, 1080};
    if (!mb || msz < TAB + 16 || memcmp(mb + STR_RMODE, "r_mode", 7) || memcmp(mb + STR_RTPOOL, "nvn_VRAMRenderTargetHeapSizeMB", 31) ||
        memcmp(mb + TAB, orig, sizeof orig)) {
        LOG("[wide] main is not the expected update build (r_mode table not found): output stays 1280x720"); return; }
    uint32_t t[4] = {w, 720, (w * 3 / 2 + 8) / 16 * 16, 1080};
    memcpy(mb + TAB, t, sizeof t);
    g_wide_patched = 1;
    LOG("[wide] r_mode table main+%#x -> r_mode 0 = %ux720 (r_mode 1 = %ux1080, %s than 16:9); RenderTarget pool: see [pools]", TAB, t[0], t[2],
        w > 1280 ? "wider" : "narrower");
}

// JNI (jni_android.c nativeWideLive): 1 = the live switch is available (DM_DOOM_WIDE_LIVE=1 and the game renders wide), so the
// presenter crop can follow the checkbox; 0 = the change applies at the next launch.
int dm_doom_wide_live(void) {
    int ok = dm_doom_wide_live_env() && g_wide_patched && g_live_wide;   // [4:3] not for an output at or below 16:9
    LOG("[wide] live switch %s (DM_DOOM_WIDE_LIVE=%d, wide output %u)", ok ? "available" : "not available: next launch", dm_doom_wide_live_env(), g_wide_patched ? dm_doom_wide_w() : 0);
    return ok;
}

void doom_drs_init(void);   // hle_doom_drs.c
// 1 = the loaded main registers rs_enable with the default "0" (patch_nso.py CUTS 0x1c2feb4: add x2,x2,#0x154 instead of #0xb99).
static int doom_drs_default_off(void) {
    enum { REG_ADD = 0x1c2feb4 };
    const uint8_t *mb = NULL; size_t msz = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { mb = g_mod[i].base; msz = g_mod[i].size; }
    uint32_t w = 0; if (mb && msz > REG_ADD + 4) memcpy(&w, mb + REG_ADD, 4);
    int off = w == 0x91055042u;
    LOG("[drs] rs_enable default %s (main+%#x = %#x); %s", off ? "\"0\" (patched)" : w == 0x912e6442u ? "\"4\" (main without the default cut)" : "unknown",
        REG_ADD, w, off ? "the profile / Video menu / gear decide" : "adding +set rs_enable 0 (remake main.patched.bin with the current patch_nso.py)");
    return off;
}

static void build(void) {
    if (g_argc >= 0) return;
    g_argc = 0; add("doom");
    doom_drs_init();     // live rs_enable bridge for the gear checkbox (hle_doom_drs.c)
    doom_wide();         // r_mode table (DM_DOOM_WIDE)
    doom_pools_args();   // +set nvn_VRAM*HeapSizeMB (DM_DOOM_POOL_MARGIN, and the wide RenderTarget minimum)
    const char *res = getenv("DM_DOOM_RES");
    if (res && *res) {
        double f = atof(res);
        if (f >= 0.25 && f < 0.999) {   // [r6] 100 % = no +set: a forced 1.0 would also pin dynamic resolution at full size
            char v[32]; snprintf(v, sizeof v, "%.3f", f);
            add("+set"); add("rs_forceFractionX"); add(v);
            add("+set"); add("rs_forceFractionY"); add(v);
        }
    }
    // DOOM's own dynamic resolution (rs_enable) reacts to the GPU time it measures; with correct timestamps it scales the 3D view down
    // on this GPU. One control, one state: the Video menu item "Dynamic Resolution" (the repurposed Chromatic Aberration entry,
    // patch_nso.py) saves rs_enable in the game profile, and the gear checkbox reads / writes the same cvar live (hle_doom_drs.c).
    // The profile is loaded after the command line and sets the saved value with force (profile binder main+0x1107cc0), so a +set here
    // would only matter until then; a fresh install has no saved value, so rs_enable's registered default decides -- patched from "4"
    // to "0" (CUTS 0x1c2feb4). No +set is added when that cut is in the loaded main; an older main.patched.bin without it still gets
    // +set rs_enable 0 so DRS never turns itself on. DM_DOOM_DRS=<n> (dm/env, debugging) forces +set rs_enable <n> at every launch.
    const char *drs = getenv("DM_DOOM_DRS");
    if (drs && *drs) { add("+set"); add("rs_enable"); add(atoi(drs) ? drs : "0"); LOG("[drs] DM_DOOM_DRS=%s: +set rs_enable %s at every launch", drs, atoi(drs) ? drs : "0"); }
    else if (!doom_drs_default_off()) { add("+set"); add("rs_enable"); add("0"); }
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
    // Profile saves (BUGS_AND_FIXES 2.40, issue #31): every idPlayerProfile save request (main+0x1eeb7e0 and its inline copies, the
    // autosave's included) returns without saving while profile_requireSaveExplanation (default "1") is set and the profile's
    // seenSaveExplanation byte (+0x38) is 0 -- that byte is set by the save explanation screen, which CUTS 0x1080118 skips. So a profile
    // made on this port was never written: menu settings (motion aiming, sensitivities, aim assist, dynamic resolution...) were lost on
    // every restart. The cvar is the game's own switch for exactly this ("allow profile save without save indicator explanation UI");
    // the profile does not store it. DM_DOOM_PROFILE_SAVE=0 keeps the game's gate (hle_doom_settings.c logs "[gyro] profile save gate").
    const char *ps = getenv("DM_DOOM_PROFILE_SAVE");
    if (!(ps && *ps == '0')) { add("+set"); add("profile_requireSaveExplanation"); add("0"); }
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
#else
int dm_doom_wide_live(void) { return 0; }
#endif
