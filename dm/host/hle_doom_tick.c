// [DOOM] Game-speed diagnostic (DM_DOOM_TICKLOG=1; off by default). Called once per present from dm_guest_frame_tick (hle_os.c).
// Once per wall second it logs the present rate, the tick cvars as the game sees them, and an estimate of how much game time the
// game advanced in that second -- so "runs fast / slow" becomes a number (speed 1.00 = real time). See docs/research/game_speed.md.
// All addresses are update main (build id 2847991952b3d7ab) image offsets; a string check refuses any other main.
//   Frame loop 0x1ecfa80 / 0x1ed0f00: com_fixedTic != 0 -> one game frame per render frame. Inside it, com_adaptiveTick +
//   ImmediateMode -> 0x1ecf940, which keeps in static S = main+0x66bd7c0: S+0x90 last call time (us, Sys_Microseconds), S+0xa0 the
//   rounding carry (us); per call: us = clamp(now - last, 1e6/MaxHz, 1e6/MinHz) * 0.96 + carry; gameHz = ceil(1e6 / us);
//   carry = us - 1e6/gameHz. So a call with a changed S+0x90 is one adaptive game frame of 1e6/gameHz us, which we replay here.
//   Fixed-tic frames (menus) are counted where they run (mrt_set_game_hz below: 1e6/hz each, "fixed-tic frames" in the log), so a
//   present with no game frame (the level-load pacifier) adds no game time. With DM_DOOM_MENU_REALTIME=0 and DM_DOOM_HZ_SUPPRESS=0
//   the hook is not installed and the old model stays: a present without an adaptive call = one fixed tic of 1000/30 ms (0x1f2b630),
//   wrong while loading (with DM_DOOM_HZ_SUPPRESS on the hook is installed for its call count and passes the menu's 30 through).
// [DOOM] Game speed fix, the same per-present call (DM_DOOM_TICK_SCALE, default 1.0; docs/BUGS_AND_FIXES.md 4.7). The 0.96 above is the
//   game's own constant (main+0x358917c, `ldr s1` at 0x1ecf9b8; the float is shared by other code, so it is not patched), and the game
//   really runs 4 % slow with it: in adaptive mode exactly one game frame runs per render frame (0x1ecfa80 zeroes the residual), and
//   idTimeManager::SetGameHz(ceil(1e6/us)) makes every clock of the frame follow us, not the real frame time -- game ms per frame
//   (1000/hz with a fraction carry), GetRealMsPerFrame = 1000/hz, GetGameSecPerFrame = ms * 0.001, and TicksToSeconds = game ms /
//   gameTimePerSec (= ms * hz ~ 1000 here). So animations, timers and movement ran at 0.96 of real time (on the Switch, fixed 30 Hz:
//   1000/30 ms per 33.3 ms frame = 1.00). The fix adds the missing (scale - 0.96) x d (d = the clamped frame time the game saw) to the
//   game's rounding carry S+0xa0 once the host sees the frame (S+0x90 changed); the next call adds the carry to its us, so the game's
//   time sums to scale x the clamped real time. The add is a compare-and-swap against the game thread's own store of the carry: if the
//   game stored in between, the add waits for the next present (nothing lost); only a CAS inside the game's read -> store window
//   (0x1ecf9bc..0x1ecfa1c, a few virtual calls) loses that one frame's 4 %. Below MinHz (30) the game still slows down, as on the Switch.
//   DM_DOOM_TICK_SCALE=0 (or 0.96): the game's own 0.96, nothing written.
// [DOOM] Menu real time (DM_DOOM_MENU_REALTIME, default 1; docs/BUGS_AND_FIXES.md 3.21). Outside gameplay (game state != 6: main
//   menu, pause, load transitions; frame loops 0x1ecfa80 at 0x1ecfc88 and 0x1ed0f00 at 0x1ed106c) the frame loop resets the tick
//   object to 30 Hz (0x1f2b630: hz 30, ms 33, ms-fraction carry +0xc = 1/3, gameTimePerSec 990) and calls
//   idGameTimeManager::SetGameHz(tick->hz) (vtable 0x3f51a30 slot 0xa8 = 0x4bcd60: tick SetHz 0x1f2b5b0 -> ms = int(1000/hz + carry),
//   then the manager's clocks: hz, ms and 1000/hz times each clock's scale). One game frame runs per render frame, so at our 60 fps
//   presents (DM_PRESENT_MENU_HZ) menus ran at 2x. The reset is code (constants), so a write to the tick object from here would be
//   overwritten before the frame uses it; instead the vtable slot (main's data segment, writable) points at mrt_set_game_hz, which
//   runs on the game thread inside the game's own call. For the two fixed-tic call sites only (return address 0x1ecfcc8 /
//   0x1ed10ac; every other caller, e.g. the adaptive tick 0x1ecfa0c, goes straight to 0x4bcd60) it does what the adaptive tick
//   (0x1ecf940) does, without the 0.96: us = clamp(guest time since the last such frame, ..., 1e6/MinHz) + carry, hz = ceil(1e6/us)
//   clamped to [com_adaptiveTickMinHz, com_adaptiveTickMaxHz] (the range gameplay already runs in), carry = us - 1e6/hz; it keeps
//   SetHz's ms-fraction carry across frames (0x1f2b630 resets it each frame), calls the game's SetGameHz(hz) and then
//   0x1f2b6e0(tick, 1000) as the adaptive path does at 0x1ecfa30 (gameTimePerSec follows ms*hz), and zeroes the frame loop's
//   residual (S+0x14 / S+0x20), as that branch does with com_adaptiveTick 0 (the Switch), so the catch-up loop never adds a second
//   game frame. Gameplay is untouched: the adaptive branch (state 6 + allowHzChange) never reaches these call sites, and state 6
//   without allowHzChange (0x1ed012c) keeps its frozen hz without calling SetGameHz. With com_adaptiveTick or ImmediateMode off
//   (DM_DOOM_ADAPTIVE_TICK=0) the hook passes 30 through (gameplay itself is fixed-tic then). Frames still come one per present,
//   so logic that counts frames is as before; what follows real time is everything the game derives from the tick (ms, hz, clocks).
// [DOOM] Engine hz-change suppression (DM_DOOM_HZ_SUPPRESS, default 2; docs/BUGS_AND_FIXES.md 4.10, issue #55). allowHzChange is not a
//   map flag: idGameLocal::RunFrame (0x4e4c10) ends with 0x4f1450 (call at 0x4e97a0), which every game frame drops dead entities
//   from two lists of the game object -- hzChangeSuppressEnts_sync (data +0x49d408, count +0x49d410; filled by a sync interaction
//   for its participants at 0x55acf4 when the sync / the participant has disableAdaptiveTick or sync_noAdaptiveTick is set, and by
//   idTarget_AdaptiveTickToggle 0x37b1f0 for the player; removed at 0x3ead34 / 0x550b0c) and hzChangeSuppressEnts_anim (+0x49d4a0,
//   count +0x49d4a8; the anim event ae_setAllowHzChange -> 0x482bd0) -- and then sets the tick object's allowHzChange: 0 while either
//   list is not empty (or the game's own allowHzChange +0x49d538 is 0), and for a settle time after the last entry left
//   (nextHzChangeTime +0x49d404 = now + 500, or + 5000 game ms after a player sync). So allowHzChange = 0 in gameplay is the engine
//   asking for a constant game hz during a scripted, animation-synced sequence (the intro's armour pickup, glory kills, ...): the
//   frame loop then runs 0x1ed012c / 0x1ed170c -- the tick stays as it is, one game frame per render frame plus the residual's
//   catch-up frames -- and RunFrame skips its hz fix-up (g_fixupForAdaptiveTick "1" -> 0x4e9c30, only while allowHzChange is set).
//   DM_DOOM_FORCE_HZ_CHANGE (4.9) set the byte again after every frame, so each frame of such a sequence got a new hz from its own
//   frame time (60 / 61 / 62 / 47 ...) and the fix-up re-timed the player's animations each time: sequences drifted apart above 30 fps
//   (at a 30 limit Min = Max = 30, the hz is constant -- the only case that worked). Now (mode 1 / 2) the host leaves the byte alone.
//   A frozen gameplay frame is a frame loop pass (S+0x18 play / S+0x24 front end, Sys_Milliseconds of the pass) without a SetGameHz
//   call (the vtable hook below sees the adaptive and the fixed-tic calls) while allowHzChange is 0. While such frames run:
//   - the present pacer is capped at the held hz when that is below its own rate (dm_doom_frozen_hz -> hle_nvn.c present_period), so
//     one game frame per present is real time (the Switch way: render rate = tick rate); a held hz above the render rate is caught
//     up by the game's own residual loop;
//   - mode 2: when the held hz is off the real frame rate by more than 1 Hz (a hitch frame just before the sequence left 30, or the
//     level started on the menu's hz), the host asks for ONE adaptive frame (allowHzChange = 1 until S+0x90 moves) and the hook
//     gives that frame the real rate (the pacer's gameplay rate, or the smoothed frame rate when the device is slower) instead of
//     the jittery one; the game clears the byte again at the end of that frame and the sequence continues at that hz, uncapped.
//     At most one such change per window and per 2 s. Mode 1 never writes the byte (sequences may run at the held hz, e.g. 30 fps).
//   - the first adaptive frame after a held stretch keeps the held hz (hzs_resume_hz in the hook) instead of the 30 Hz its stale
//     frame time gives;
//   - the pacer's gameplay signal and the touch-look stamp count frozen frames as gameplay (they used S+0x90 only).
//   Mode 0 = the 4.9 behaviour (DM_DOOM_FORCE_HZ_CHANGE). Log: "[tick] suppress on / off", "[tick] re-pick", fields in the [tick] line.
#include "hle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DM_GAME_DOOM
enum { GOT_FIXEDTIC = 0x4885de8, GOT_ADAPTIVE = 0x4884948, GOT_IMMEDIATE = 0x48d95f0, GOT_MINHZ = 0x48d95d8, GOT_MAXHZ = 0x48d95d0,
       GOT_TIMESCALE = 0x48d7478, GOT_TICKOBJ = 0x487fc40, GOT_GAME = 0x487f170, STATIC_S = 0x66bd7c0, STR_FIXEDTIC = 0x36139c3 };
static uint8_t *g_mb; static size_t g_msz;
static int g_main_state;   // 0 = not looked up yet, 1 = the expected update main, -1 = another build (nothing here reads it)
static int main_ok(void) {
    if (g_main_state) return g_main_state > 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_mb = g_mod[i].base; g_msz = g_mod[i].size; }
    if (!g_mb) return 0;   // modules not loaded yet: look again next time
    if (g_msz < STATIC_S + 0x100 || memcmp(g_mb + STR_FIXEDTIC, "com_fixedTic", 13)) { g_mb = NULL; g_main_state = -1; return 0; }
    g_main_state = 1; return 1;
}
static int ok_ptr(uint64_t p, size_t n) { return p && p >= (uint64_t)g_mb && p + n <= (uint64_t)g_mb + g_msz; }
// idCVar: GOT slot -> object in main .bss; integer value at +0x30, float at +0x34 (how the frame loop reads them)
static int cv_i(uint64_t got) { uint64_t o = *(uint64_t *)(g_mb + got); return ok_ptr(o, 0x38) ? *(int32_t *)(o + 0x30) : -999; }
static float cv_f(uint64_t got) { uint64_t o = *(uint64_t *)(g_mb + got); return ok_ptr(o, 0x38) ? *(float *)(o + 0x34) : -1.f; }

// DM_DOOM_TICK_SCALE: game time per real time in the adaptive tick; 1.0 by default, at most 1.25; 0 / 0.96 or less = the game's own 0.96.
static float tick_scale(void) {
    static float s = -1.f;
    if (s < 0.f) {
        const char *e = getenv("DM_DOOM_TICK_SCALE"); float v = e && *e ? (float)atof(e) : 1.0f;
        if (v > 1.25f) v = 1.25f;
        if (!(v > 0.9601f)) v = 0.96f;
        s = v;
    }
    return s;
}
static uint64_t fix_prev_ts, fix_added_us, fix_retries; static double fix_pending;
// One present: credit (scale - 0.96) x the clamped frame time of each adaptive frame seen since the last present, then move whole
// microseconds of the credit into the game's carry S+0xa0 (compare-and-swap; on a race with the game's store, retry next present).
static void tick_fix(uint8_t *S, uint64_t ts, int mn, int mx, float scale) {
    if (ts != fix_prev_ts && fix_prev_ts && mn > 0 && mx > 0) {
        // The game counted 0.96 x clamp(d, 1e6/MaxHz, 1e6/MinHz); the target is scale x min(d, 1e6/MinHz): real time, keeping the
        // slow-down below MinHz (as on the Switch) but not the speed-up of frames shorter than 1e6/MaxHz (pacing jitter at the frame
        // cap) -- that part comes out negative and is taken from the next credits (the carry is never lowered).
        const double d = (double)(ts - fix_prev_ts), lo = (double)(1000000u / (unsigned)mx), hi = (double)(1000000u / (unsigned)mn);
        const double seen = d < lo ? lo : d > hi ? hi : d, real = d > hi ? hi : d;
        fix_pending += real * (double)scale - seen * (double)0.96f;   // the game multiplies by the float 0.96f
        if (fix_pending < -50000.0) fix_pending = -50000.0;          // a long run above MaxHz leaves at most 50 ms of debt
    }
    fix_prev_ts = ts;
    if (fix_pending < 1.0) return;
    uint64_t add = (uint64_t)fix_pending, *carry = (uint64_t *)(S + 0xa0), cur = __atomic_load_n(carry, __ATOMIC_RELAXED);
    if (__atomic_compare_exchange_n(carry, &cur, cur + add, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { fix_pending -= (double)add; fix_added_us += add; }
    else fix_retries++;
}

// ---- Menu real time (DM_DOOM_MENU_REALTIME; header comment) ----
enum { VT_SETGAMEHZ = 0x3f51ad8, FN_SETGAMEHZ = 0x4bcd60, FN_TICK_RESET = 0x1f2b630, FN_TICK_RATE = 0x1f2b6e0,
       RA_FIXED_PLAY = 0x1ecfcc8, RA_FIXED_FRONT = 0x1ed10ac, RA_ADAPTIVE = 0x1ecfa10 };
typedef void (*mrt_fn)(void *, int);   // SetGameHz(manager, hz) and tick rate step 0x1f2b6e0(tick, maxRate): AAPCS64, called in place
static int mrt_state;                  // 0 = not installed yet, 1 = installed, -1 = refused (build / code check failed)
static uint64_t mrt_prev_us; static double mrt_carry; static float mrt_frac; static int mrt_have_frac, mrt_said;
static uint64_t mrt_frames, mrt_game_ns; static int mrt_last_hz;   // for [tick] (atomic: written on the game thread, read per present)
// DM_DOOM_HZ_SUPPRESS (header comment): 0 = off (DM_DOOM_FORCE_HZ_CHANGE as before), 1 = respect the engine's suppression and cap the
// present rate at the held hz, 2 (default) = 1 + one re-pick of the held hz when it is off the real frame rate.
static int hzs_mode(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_DOOM_HZ_SUPPRESS"); v = e && *e ? atoi(e) : 2; if (v < 0 || v > 2) v = 2; } return v; }
static uint64_t sgh_calls;                          // atomic: SetGameHz calls of the game (any caller), counted by the hook
static int hzs_sub_hz; static uint64_t hzs_sub_until;   // atomic: the re-pick's hz for the adaptive tick's SetGameHz, valid until (now_ns)
// atomic, one-shot: the hz the engine held, for the first adaptive frame after a held stretch. That frame's own pick is not a frame
// time: 0x1ecf940 measures from its previous call (before the stretch), clamps to 1e6/MinHz and would run one 30 Hz frame (33 ms of
// game time in a 16 ms frame, and two hz changes); with the held hz it continues as it was and the next frame picks normally.
static int hzs_resume_hz;
static int mrt_enabled(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_DOOM_MENU_REALTIME"); v = !(e && *e == '0'); } return v; }
// The choice per fixed-tic frame (pure; the offline float32 model in BUGS_AND_FIXES 3.21 mirrors it):
// d = the frame's guest time (us, at most 1e6/MinHz), carry in/out (us); returns the game Hz for this frame.
static int mrt_pick_hz(double d, double *carry, int mn, int mx) {
    double hi = 1e6 / mn, acc = *carry + d;
    int hz = acc > 0 ? (int)ceil(1e6 / acc) : mx;
    if (hz > mx) hz = mx; if (hz < mn) hz = mn;
    double c = acc - 1e6 / hz;
    *carry = c < -50000.0 ? -50000.0 : c > hi ? hi : c;   // frames shorter than 1/MaxHz leave at most 50 ms of debt
    return hz;
}
static void mrt_set_game_hz(void *mgr, int hz) {
    const uint64_t ra = (uint64_t)__builtin_return_address(0) - (uint64_t)g_mb;
    const mrt_fn set_hz = (mrt_fn)(void *)(g_mb + FN_SETGAMEHZ);
    __atomic_add_fetch(&sgh_calls, 1, __ATOMIC_RELAXED);
    if (ra != RA_FIXED_PLAY && ra != RA_FIXED_FRONT) {   // adaptive tick and every other caller: as is, except the re-pick frame (4.10)
        if (ra == RA_ADAPTIVE) { const int s = __atomic_load_n(&hzs_sub_hz, __ATOMIC_RELAXED), r = __atomic_exchange_n(&hzs_resume_hz, 0, __ATOMIC_RELAXED);
                                 if (s > 0 && now_ns() < __atomic_load_n(&hzs_sub_until, __ATOMIC_RELAXED)) hz = s; else if (r > 0) hz = r; }
        set_hz(mgr, hz); return;
    }
    const uint64_t now = guest_ns() / 1000u;
    const uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ); uint8_t *tick = ok_ptr(tg, 8) ? *(uint8_t **)tg : NULL;
    const int mn = cv_i(GOT_MINHZ), mx = cv_i(GOT_MAXHZ);
    const int act = mrt_enabled() && tick && hz == 30 && cv_i(GOT_FIXEDTIC) > 0 && cv_i(GOT_ADAPTIVE) > 0 && cv_i(GOT_IMMEDIATE) > 0 && mn >= 10 && mx >= mn && mx <= 1000;
    const int gap = !mrt_prev_us || now < mrt_prev_us || now - mrt_prev_us > 250000u;   // first frame / after gameplay or a load
    double d = gap ? 1e9 : (double)(now - mrt_prev_us);
    // [4.10] The first frame after a gap has no frame time; it used to count as 1e6/MinHz (30 Hz). When gameplay starts right after
    // it inside a suppressed sequence, that 30 is the hz the engine holds, so take the last fixed-tic hz (the menu's rate) instead.
    if (gap && hzs_mode()) { const int l = __atomic_load_n(&mrt_last_hz, __ATOMIC_RELAXED); if (l >= mn && l <= mx && mn >= 10) d = 1e6 / l; }
    mrt_prev_us = now;
    if (!act) { set_hz(mgr, hz); mrt_have_frac = 0; mrt_carry = 0; }   // old behaviour (adaptive tick off: gameplay is fixed-tic too)
    else {
        if (gap) { mrt_carry = 0; mrt_have_frac = 0; }
        if (d > 1e6 / mn) d = 1e6 / mn;                       // below MinHz the game slows down, as on the Switch (one 1e6/MinHz tic)
        const int h = mrt_pick_hz(d, &mrt_carry, mn, mx);
        float *frac = (float *)(tick + 0xc);                  // SetHz's ms-fraction carry (0x1f2b5b0); 0x1f2b630 set it to 1/3
        if (mrt_have_frac) *frac = mrt_frac;
        set_hz(mgr, h);                                       // the game's own SetGameHz: tick hz / ms / 1000/hz and every clock
        mrt_frac = *frac; mrt_have_frac = 1;
        ((mrt_fn)(void *)(g_mb + FN_TICK_RATE))(tick, 1000);  // as the adaptive path after its SetGameHz (0x1ecfa30)
        *(int32_t *)(g_mb + STATIC_S + (ra == RA_FIXED_PLAY ? 0x14 : 0x20)) = 0;   // residual: one game frame per render frame
        if (!mrt_said) { mrt_said = 1; LOG("[tick] menu realtime: first fixed-tic frame at %d Hz (%d ms; real %.2f ms, Min/MaxHz %d/%d, loop %s)",
                                           h, *(int32_t *)(tick + 8), d / 1e3, mn, mx, ra == RA_FIXED_PLAY ? "0x1ecfa80" : "0x1ed0f00"); }
        hz = h;
    }
    __atomic_add_fetch(&mrt_game_ns, hz > 0 ? (uint64_t)(1e9 / hz) : 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&mrt_frames, 1, __ATOMIC_RELAXED); __atomic_store_n(&mrt_last_hz, hz, __ATOMIC_RELAXED);
}
// Once, from the per-present call: check the build's code at the hooked call sites and swap the vtable slot (one aligned 8-byte
// store into main's data segment; the game thread reads either the old or the new pointer, both valid).
static void mrt_install(void) {
    static const struct { uint32_t off, word; } chk[] = {
        {RA_FIXED_PLAY - 4, 0xd63f0100}, {RA_FIXED_FRONT - 4, 0xd63f0100},   // blr x8 (SetGameHz) at both fixed-tic sites
        {FN_SETGAMEHZ, 0xa9be4ff4}, {FN_TICK_RESET, 0xd28003c8}, {FN_TICK_RATE, 0xb9401008} };
    for (unsigned i = 0; i < sizeof chk / sizeof chk[0]; i++)
        if (*(uint32_t *)(g_mb + chk[i].off) != chk[i].word) { LOG("[tick] menu realtime off: code at main+%#x differs", chk[i].off); mrt_state = -1; return; }
    uint64_t *slot = (uint64_t *)(g_mb + VT_SETGAMEHZ), cur = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (cur != (uint64_t)g_mb + FN_SETGAMEHZ) { LOG("[tick] menu realtime off: vtable slot main+%#x = %#llx", VT_SETGAMEHZ, (unsigned long long)cur); mrt_state = -1; return; }
    __atomic_store_n(slot, (uint64_t)(void *)mrt_set_game_hz, __ATOMIC_RELEASE);
    mrt_state = 1;
    LOG("[tick] menu realtime on (DM_DOOM_MENU_REALTIME): outside gameplay the fixed tic follows real time (SetGameHz slot main+%#x)", VT_SETGAMEHZ);
}

// ---- Gameplay hz change (DM_DOOM_FORCE_HZ_CHANGE, default 1) ----
// [4.10] Used only with DM_DOOM_HZ_SUPPRESS=0 (or when the suppression code refused this main): the byte is the engine's request to
// keep the hz during an animation-synced sequence (0x4f1450), and setting it every frame broke those sequences above 30 fps (#55).
// In game state 6 the frame loop takes the adaptive branch only while the tick object's allowHzChange (+0x24) is set; with it clear
// (0x1ed012c) gameplay keeps the frozen 30 Hz tick (33 ms) and still runs one game frame per render frame, so at our 60 fps it ran at
// 2x (Mi 14, 06.11: after loading a save, allowHzChange=0, [tick] 0 adaptive frames at 45-60 fps, owner: "runs twice as fast" until
// a later script set it again). The map sets the byte at 0x4c6abc / 0x4d64dc, ae_setAllowHzChange toggles it, 0x4f15c8 clears it.
// On the Switch Min = Max = 30, so both branches give 30 Hz and the flag changes nothing; here it decides real time vs 2x. So the
// host keeps it set (once per present, the game thread only reads it in the frame loop); outside state 6 the branch is not taken.
static int hzc_enabled(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_DOOM_FORCE_HZ_CHANGE"); v = !(e && *e == '0'); } return v; }
static uint64_t hzc_sets;
static void hzc_keep(void) {
    if (cv_i(GOT_ADAPTIVE) <= 0 || cv_i(GOT_IMMEDIATE) <= 0) return;   // adaptive tick off: gameplay is fixed-tic by choice
    uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ), tobj = ok_ptr(tg, 8) ? *(uint64_t *)tg : 0;
    if (!tobj || !ok_ptr(tobj, 0x28)) return;
    volatile uint8_t *allow = (volatile uint8_t *)(tobj + 0x24);
    if (*allow) return;
    *allow = 1;
    if (++hzc_sets <= 5 || !(hzc_sets & (hzc_sets - 1)))
        LOG("[tick] allowHzChange was clear (tick hz %d): set again so gameplay follows real time (#%llu; DM_DOOM_FORCE_HZ_CHANGE=0 keeps the game's)",
            *(int32_t *)tobj, (unsigned long long)hzc_sets);
}

// ---- Engine hz-change suppression (DM_DOOM_HZ_SUPPRESS, default 2; header comment, docs/BUGS_AND_FIXES.md 4.10) ----
enum { G_NEXT = 0x49d404, G_SYNC_NUM = 0x49d410, G_ANIM_NUM = 0x49d4a8, G_ALLOW = 0x49d538, G_END = 0x49d540 };
__attribute__((weak)) double dm_nvn_present_play_hz(void) { return 0; }   // hle_nvn.c: the pacer's gameplay rate without the cap below
static struct {
    int init, run, active, forcing, repicked, want;
    uint64_t stamp, calls, ts, pass_ns, on_ns, frames, windows, repicks, force_ns, repick_ns, ema_ns, said;
    double ema_us;
} hzs;
static uint64_t hzs_passes;   // atomic: frozen gameplay frames seen (gameplay signal / stamp below, [tick] line)
static int hzs_cap_hz;        // atomic: > 0 = the game hz the engine holds right now, for the present pacer
// hle_nvn.c present_period: the hz to cap presents at (0 = none). Any thread.
double dm_doom_frozen_hz(void) { return (double)__atomic_load_n(&hzs_cap_hz, __ATOMIC_RELAXED); }
// Why the engine holds the hz: the fields 0x4f1450 decides on (the game object is a static of main; otherwise not read).
static const char *hzs_reason(char *b, size_t n) {
    const uint64_t gg = *(uint64_t *)(g_mb + GOT_GAME), gl = ok_ptr(gg, 8) ? *(uint64_t *)gg : 0;
    if (!gl || !ok_ptr(gl, G_END)) { snprintf(b, n, "reason unknown (game object %#llx outside main)", (unsigned long long)gl); return b; }
    const int ns = *(int32_t *)(gl + G_SYNC_NUM), na = *(int32_t *)(gl + G_ANIM_NUM), ga = *(uint8_t *)(gl + G_ALLOW), nx = *(int32_t *)(gl + G_NEXT);
    snprintf(b, n, "reason=%s sync_ents=%d anim_ents=%d game_allow=%d nextHzChangeTime=%d",
             ns > 0 ? (na > 0 ? "sync+anim" : "sync") : na > 0 ? "anim" : !ga ? "game_flag" : "settle", ns, na, ga, nx);
    return b;
}
// The hz a re-pick asks for: the pacer's gameplay rate, or the smoothed real frame rate when the device runs clearly below it (a held
// hz above the render rate makes the game's residual loop run catch-up game frames all the time). With no recent adaptive frame to
// measure (a level that starts inside a sequence) at most 60, the menu's rate: whether the device holds a 90 / 120 limit is unknown.
static int hzs_target(int mn, int mx, uint64_t now) {
    const double h = dm_nvn_present_play_hz(); double r = h;
    if (hzs.ema_us > 0 && now - hzs.ema_ns < 2000000000ull) { const double f = 1e6 / hzs.ema_us; if (!(h > 0) || f < h * 0.97) r = f; }
    else if (r > 60) r = 60;
    if (!(r > 0)) return 0;
    int t = (int)(r + 0.5); if (t > mx) t = mx; if (t < mn) t = mn;
    return t;
}
static int hzs_log_ok(void) { const uint64_t k = ++hzs.said; return k <= 40 || !(k & (k - 1)); }
// Once per present (the game's main thread presents). Publishes hzs_cap_hz / hzs_passes.
static void hzs_tick(uint8_t *S, int mode, int mn, int mx) {
    const uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ), tobj = ok_ptr(tg, 8) ? *(uint64_t *)tg : 0;
    if (!tobj || !ok_ptr(tobj, 0x28)) return;
    const uint64_t now = now_ns(), ts = *(volatile uint64_t *)(S + 0x90), calls = __atomic_load_n(&sgh_calls, __ATOMIC_RELAXED);
    const uint64_t stamp = (uint64_t)*(volatile uint32_t *)(S + 0x18) << 32 | *(volatile uint32_t *)(S + 0x24);
    if (!hzs.init) { hzs.init = 1; hzs.stamp = stamp; hzs.calls = calls; hzs.ts = ts; return; }
    const int pass = stamp != hzs.stamp, called = calls != hzs.calls, adapt = ts != hzs.ts;
    if (adapt && hzs.ts) {   // smoothed real time of an adaptive frame (us), for hzs_target
        const uint64_t d = ts - hzs.ts;
        if (d > 2000 && d < 100000) {   // each sample moves it by at most 25 % / 16, so the hitch frame that often precedes a sequence does not
            if (hzs.ema_us > 0 && now - hzs.ema_ns < 2000000000ull) { const double lo = hzs.ema_us * 0.75, hi = hzs.ema_us * 1.25, v = d < lo ? lo : d > hi ? hi : (double)d; hzs.ema_us += (v - hzs.ema_us) / 16; }
            else hzs.ema_us = (double)d;
            hzs.ema_ns = now;
        }
    }
    hzs.stamp = stamp; hzs.calls = calls; hzs.ts = ts;
    const int on = cv_i(GOT_FIXEDTIC) > 0 && cv_i(GOT_ADAPTIVE) > 0 && cv_i(GOT_IMMEDIATE) > 0 && mn >= 10 && mx >= mn;
    volatile uint8_t *allow = (volatile uint8_t *)(tobj + 0x24); const int thz = *(int32_t *)tobj; char rb[160];
    if (pass) hzs.pass_ns = now;
    if (hzs.forcing) {   // mode 2: one adaptive frame at hzs.want; the game clears allowHzChange again at the end of that frame
        if (!adapt && on && now - hzs.force_ns < 500000000ull) {
            *allow = 1; __atomic_store_n(&hzs_sub_until, now + 150000000ull, __ATOMIC_RELAXED); return;
        }
        hzs.forcing = 0; hzs.repicked = 1; if (adapt) hzs.repicks++;
        if (hzs_log_ok()) LOG("[tick] re-pick %s: tick hz %d (asked %d) #%llu", adapt ? "done" : "gave up (no adaptive frame in 500 ms)", thz, hzs.want, (unsigned long long)hzs.repicks);
        return;   // this present saw the adaptive frame: not a frozen one, and not the end of the window either
    }
    const int frozen = on && pass && !called && !adapt && !*allow && thz >= 10 && thz <= 1000;
    if (frozen) { hzs.run++; __atomic_add_fetch(&hzs_passes, 1, __ATOMIC_RELAXED); }
    else if (pass || now - hzs.pass_ns > 250000000ull) hzs.run = 0;   // an adaptive / fixed-tic frame, or no frame at all (loading)
    if (!hzs.active && hzs.run >= 2) {
        hzs.active = 1; hzs.on_ns = now; hzs.frames = 0; hzs.windows++; hzs.repicked = 0;
        if (hzs_log_ok()) LOG("[tick] suppress on: the game holds its hz at %d (allowHzChange=0, %s); pacer rate %.2f Hz, mode %d (DM_DOOM_HZ_SUPPRESS) #%llu",
                              thz, hzs_reason(rb, sizeof rb), dm_nvn_present_play_hz(), mode, (unsigned long long)hzs.windows);
    } else if (hzs.active && !hzs.run) {
        hzs.active = 0; __atomic_store_n(&hzs_cap_hz, 0, __ATOMIC_RELAXED); __atomic_store_n(&hzs_resume_hz, 0, __ATOMIC_RELAXED);
        if (hzs_log_ok()) LOG("[tick] suppress off after %.2f s, %llu frozen frames at %d Hz (%s): %s", (now - hzs.on_ns) / 1e9, (unsigned long long)hzs.frames, thz,
                              hzs_reason(rb, sizeof rb), *allow ? "adaptive tick again" : "no gameplay frame (menu / loading)");
    }
    if (!hzs.active) return;
    if (frozen) hzs.frames++;
    if (mode >= 2 && !hzs.repicked && now - hzs.repick_ns > 2000000000ull) {
        const int t = hzs_target(mn, mx, now);
        if (t > 0 && (thz < t - 1 || thz > t + 1)) {
            hzs.forcing = 1; hzs.want = t; hzs.force_ns = hzs.repick_ns = now;
            __atomic_store_n(&hzs_sub_hz, t, __ATOMIC_RELAXED); __atomic_store_n(&hzs_sub_until, now + 150000000ull, __ATOMIC_RELAXED);
            __atomic_store_n(&hzs_cap_hz, 0, __ATOMIC_RELAXED);   // the re-pick frame runs at the pacer's own rate
            *allow = 1;
            if (hzs_log_ok()) LOG("[tick] re-pick: held hz %d is off the real rate, asking for one adaptive frame at %d Hz (pacer %.2f, smoothed frame %.2f ms)",
                                  thz, t, dm_nvn_present_play_hz(), hzs.ema_us / 1e3);
            return;
        }
        hzs.repicked = 1;   // close enough: keep the engine's hz for this window
    }
    __atomic_store_n(&hzs_cap_hz, thz, __ATOMIC_RELAXED);
    __atomic_store_n(&hzs_resume_hz, thz, __ATOMIC_RELAXED);
}
// Once: the code this relies on (same build as the menu hook's checks).
static int hzs_check(void) {
    static const struct { uint32_t off, word; } chk[] = {
        {0x4f15c8, 0x3900911f},    // strb wzr, [x8, #0x24]: 0x4f1450 clears allowHzChange
        {0x1ecfc60, 0x34002668}, {0x1ed1040, 0x34003668},   // cbz w8 -> the frozen branch 0x1ed012c / 0x1ed170c in both frame loops
        {0x1ecfbc4, 0x2902e788},   // stp w8, w25, [x28, #0x14]: the play loop's residual and pass time (S+0x14 / S+0x18)
        {RA_ADAPTIVE - 4, 0xd63f0100} };   // blr x8: the adaptive tick's SetGameHz
    for (unsigned i = 0; i < sizeof chk / sizeof chk[0]; i++)
        if (*(uint32_t *)(g_mb + chk[i].off) != chk[i].word) { LOG("[tick] hz suppression off: code at main+%#x differs", chk[i].off); return 0; }
    return 1;
}

void dm_doom_tick_diag(void) {
    static int on = -1, mrt, hzc, hzsm; static float scale;
    if (on < 0) {
        const char *e = getenv("DM_DOOM_TICKLOG"); on = e && *e == '1'; scale = tick_scale(); mrt = mrt_enabled(); hzc = hzc_enabled(); hzsm = hzs_mode();
        LOG("[tick] game speed: adaptive tick at %.3fx real time (DM_DOOM_TICK_SCALE; %s); menus: %s; gameplay allowHzChange: %s", scale,
            scale > 0.9601f ? "the host tops up the game's own 0.96 through its carry" : "the game's own 0.96, nothing written",
            mrt ? "real time (DM_DOOM_MENU_REALTIME=1)" : "fixed 30 Hz tic per frame (DM_DOOM_MENU_REALTIME=0)",
            hzsm == 2 ? "the game's; held hz: present capped to it, one re-pick when off the real rate (DM_DOOM_HZ_SUPPRESS=2)" :
            hzsm == 1 ? "the game's; held hz: present capped to it (DM_DOOM_HZ_SUPPRESS=1)" :
            hzc ? "kept set (DM_DOOM_HZ_SUPPRESS=0, DM_DOOM_FORCE_HZ_CHANGE=1)" : "the game's, nothing done (DM_DOOM_HZ_SUPPRESS=0, DM_DOOM_FORCE_HZ_CHANGE=0)");
    }
    if (!on && !(scale > 0.9601f) && !(mrt && !mrt_state) && !hzc && !hzsm) return;
    if (!main_ok()) {
        if (g_main_state < 0) { LOG("[tick] main is not the expected update build: speed fix, menu realtime, hz suppression and diagnostic off"); on = 0; scale = 0.96f; mrt = 0; hzc = 0; hzsm = 0; }
        return;   // (modules not loaded yet: look again next present)
    }
    if ((mrt || hzsm) && !mrt_state) mrt_install();   // the hook also counts SetGameHz calls for hzs_tick (it passes 30 through with mrt off)
    if (hzsm && (mrt_state != 1 || !hzs_check())) {
        LOG("[tick] hz suppression (DM_DOOM_HZ_SUPPRESS) unavailable: back to DM_DOOM_FORCE_HZ_CHANGE=%d", hzc); hzsm = 0;
    }
    uint8_t *S = g_mb + STATIC_S;
    int mn = cv_i(GOT_MINHZ), mx = cv_i(GOT_MAXHZ);
    if (hzsm) hzs_tick(S, hzsm, mn, mx);
    else if (hzc) hzc_keep();
    uint64_t ts = *(volatile uint64_t *)(S + 0x90);
    if (scale > 0.9601f) tick_fix(S, ts, mn, mx, scale);
    if (!on) return;
    uint64_t now = now_ns(), carry = *(volatile uint64_t *)(S + 0xa0);   // after the fix: the carry the game's next call reads
    static uint64_t t0, prev_ts, prev_carry, n_pres, n_adapt, last_hz, fx_n0, fx_ns0, fz_p, fz_n0; static double game_us;
    const uint64_t fx_n = __atomic_load_n(&mrt_frames, __ATOMIC_RELAXED), fx_ns = __atomic_load_n(&mrt_game_ns, __ATOMIC_RELAXED);
    const uint64_t fz_n = __atomic_load_n(&hzs_passes, __ATOMIC_RELAXED);
    if (!t0) { t0 = now; prev_ts = ts; prev_carry = carry; fx_n0 = fx_n; fx_ns0 = fx_ns; fz_p = fz_n0 = fz_n; return; }
    n_pres++;
    {   // frozen gameplay frames (4.10): one game frame of 1e6 / tick hz each (the residual's catch-up frames are not seen from here)
        const uint64_t tgz = *(uint64_t *)(g_mb + GOT_TICKOBJ), toz = ok_ptr(tgz, 8) ? *(uint64_t *)tgz : 0; const int hzz = toz && ok_ptr(toz, 0x28) ? *(int32_t *)toz : 0;
        if (fz_n != fz_p && hzz > 0) game_us += (double)(fz_n - fz_p) * 1e6 / hzz;
        fz_p = fz_n;
    }
    if (ts != prev_ts && prev_ts && mn > 0 && mx > 0) {   // replay 0x1ecf940 for this frame (one adaptive call per present assumed)
        uint64_t d = ts - prev_ts, lo = 1000000u / (unsigned)mx, hi = 1000000u / (unsigned)mn;
        if (d < lo) d = lo; if (d > hi) d = hi;
        uint64_t us = (uint64_t)((float)d * 0.96f + (float)prev_carry);
        unsigned hz = us ? (unsigned)ceilf(1e6f / (float)us) : 30; if (!hz) hz = 30;
        game_us += 1e6 / hz; n_adapt++; last_hz = hz;
    } else if (mrt_state != 1 && !hzsm) game_us += 1e6 / 30.0;   // old model: a present without an adaptive call = one 30 Hz tic
    // (hook installed: fixed-tic frames are counted where they run, mrt_set_game_hz; a present with no game frame -- the level-load
    // pacifier -- adds no game time)
    prev_ts = ts; prev_carry = carry;
    double wall = (now - t0) / 1e9;
    if (wall < 1.0) return;
    uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ), tobj = ok_ptr(tg, 8) ? *(uint64_t *)tg : 0;
    int thz = -1, tms = -1, allow = -1; char rsn[160];
    if (tobj) { thz = *(int32_t *)tobj; tms = *(int32_t *)(tobj + 8); allow = *(uint8_t *)(tobj + 0x24); }   // tick object (0x1f2b630 layout)
    if (mrt_state == 1) game_us += (double)(fx_ns - fx_ns0) / 1e3;
    LOG("[tick] %.2fs wall: %llu presents (%.1f fps), %llu adaptive frames (last gameHz %llu), %llu fixed-tic frames (last hz %d, menu realtime %s); "
        "game time ~%.0f ms => speed %.2fx | "
        "com_fixedTic=%d com_adaptiveTick=%d immediate=%d minHz=%d maxHz=%d timescale=%.2f | tickobj hz=%d ms=%d allowHzChange=%d residual=%d | "
        "fix scale=%.3f added_us=%llu retries=%llu | suppress mode=%d active=%d frozen=%llu cap_hz=%d windows=%llu repicks=%llu %s",
        wall, (unsigned long long)n_pres, n_pres / wall, (unsigned long long)n_adapt, (unsigned long long)last_hz,
        (unsigned long long)(fx_n - fx_n0), __atomic_load_n(&mrt_last_hz, __ATOMIC_RELAXED),
        mrt_state == 1 ? (mrt ? "on" : "off") : mrt_state < 0 ? "refused" : "off, frames not counted", game_us / 1e3,
        game_us / 1e6 / wall, cv_i(GOT_FIXEDTIC), cv_i(GOT_ADAPTIVE), cv_i(GOT_IMMEDIATE), mn, mx, cv_f(GOT_TIMESCALE), thz, tms, allow,
        *(int32_t *)(S + 0x14), scale, (unsigned long long)fix_added_us, (unsigned long long)fix_retries,
        hzsm, hzs.active, (unsigned long long)(fz_n - fz_n0), __atomic_load_n(&hzs_cap_hz, __ATOMIC_RELAXED), (unsigned long long)hzs.windows,
        (unsigned long long)hzs.repicks, hzsm && !allow ? hzs_reason(rsn, sizeof rsn) : "");
    t0 = now; n_pres = n_adapt = 0; game_us = 0; fx_n0 = fx_n; fx_ns0 = fx_ns; fz_n0 = fz_n;
}

// [r6b] Gameplay signal for the present pacer (hle_nvn.c present_play_tick, DM_PRESENT_MENU_HZ). Called once per present (any
// thread; two cvar reads and one 8-byte read of the game's static S). Returns 1 when the game ran an adaptive game frame since the
// previous call, 0 when it did not, -1 when the signal is not available. The adaptive branch of the frame loop (0x1ecf940, which
// stores Sys_Microseconds at S+0x90 on every call) is taken only when com_adaptiveTick is on, the game state is 6 (in game) and the
// tick object's allowHzChange (+0x24) is set by the map -- i.e. exactly in gameplay, and exactly where game time follows the real
// frame time (com_adaptiveTickMaxHz = the frame rate limit). Menus run the fixed 30 Hz tic (no call); the level-load pacifier
// (loading screen) renders without game frames (no call). -1: another main build, or com_adaptiveTick 0 (DM_DOOM_ADAPTIVE_TICK=0),
// where the call never happens -- the pacer then keeps the old behaviour instead of capping gameplay as if it were a menu.
int dm_doom_play_signal(void) {
    if (!main_ok()) return -1;
    if (cv_i(GOT_ADAPTIVE) <= 0 || cv_i(GOT_IMMEDIATE) <= 0) return -1;
    static uint64_t prev, prev_fz;
    uint64_t ts = *(volatile uint64_t *)(g_mb + STATIC_S + 0x90), fz = __atomic_load_n(&hzs_passes, __ATOMIC_RELAXED);
    int ran = (ts && prev && ts != prev) || fz != prev_fz; prev = ts; prev_fz = fz;   // [4.10] or a frozen gameplay frame (the engine holds the hz)
    return ran;
}
// [issue #40] The same gameplay stamp for touch look (hle_hid.c tl_gameplay), stateless so it does not disturb the pacer's `prev` above:
// 1 and *ts = S+0x90 (Sys_Microseconds of the last adaptive game frame; it changes every gameplay frame), 0 when unavailable (another
// main build, com_adaptiveTick or ImmediateMode off). Any thread; one 8-byte read.
int dm_doom_play_stamp(uint64_t *ts) {
    if (!main_ok()) return 0;
    if (cv_i(GOT_ADAPTIVE) <= 0 || cv_i(GOT_IMMEDIATE) <= 0) return 0;
    *ts = *(volatile uint64_t *)(g_mb + STATIC_S + 0x90) + __atomic_load_n(&hzs_passes, __ATOMIC_RELAXED);   // [4.10] frozen gameplay frames move it too
    return 1;
}
#else
void dm_doom_tick_diag(void) {}
double dm_doom_frozen_hz(void) { return 0; }
#endif
