// [DOOM] Game-speed diagnostic (DM_DOOM_TICKLOG=1; off by default). Called once per present from dm_guest_frame_tick (hle_os.c).
// Once per wall second it logs the present rate, the tick cvars as the game sees them, and an estimate of how much game time the
// game advanced in that second -- so "runs fast / slow" becomes a number (speed 1.00 = real time). See docs/research/game_speed.md.
// All addresses are update main (build id 2847991952b3d7ab) image offsets; a string check refuses any other main.
//   Frame loop 0x1ecfa80 / 0x1ed0f00: com_fixedTic != 0 -> one game frame per render frame. Inside it, com_adaptiveTick +
//   ImmediateMode -> 0x1ecf940, which keeps in static S = main+0x66bd7c0: S+0x90 last call time (us, Sys_Microseconds), S+0xa0 the
//   rounding carry (us); per call: us = clamp(now - last, 1e6/MaxHz, 1e6/MinHz) * 0.96 + carry; gameHz = ceil(1e6 / us);
//   carry = us - 1e6/gameHz. So a call with a changed S+0x90 is one adaptive game frame of 1e6/gameHz us, which we replay here.
//   Fixed-tic frames (menus) are counted where they run (mrt_set_game_hz below: 1e6/hz each, "fixed-tic frames" in the log), so a
//   present with no game frame (the level-load pacifier) adds no game time. With DM_DOOM_MENU_REALTIME=0 the hook is not installed
//   and the old model stays: a present without an adaptive call = one fixed tic of 1000/30 ms (0x1f2b630), wrong while loading.
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
#include "hle.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef DM_GAME_DOOM
enum { GOT_FIXEDTIC = 0x4885de8, GOT_ADAPTIVE = 0x4884948, GOT_IMMEDIATE = 0x48d95f0, GOT_MINHZ = 0x48d95d8, GOT_MAXHZ = 0x48d95d0,
       GOT_TIMESCALE = 0x48d7478, GOT_TICKOBJ = 0x487fc40, STATIC_S = 0x66bd7c0, STR_FIXEDTIC = 0x36139c3 };
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
       RA_FIXED_PLAY = 0x1ecfcc8, RA_FIXED_FRONT = 0x1ed10ac };
typedef void (*mrt_fn)(void *, int);   // SetGameHz(manager, hz) and tick rate step 0x1f2b6e0(tick, maxRate): AAPCS64, called in place
static int mrt_state;                  // 0 = not installed yet, 1 = installed, -1 = refused (build / code check failed)
static uint64_t mrt_prev_us; static double mrt_carry; static float mrt_frac; static int mrt_have_frac, mrt_said;
static uint64_t mrt_frames, mrt_game_ns; static int mrt_last_hz;   // for [tick] (atomic: written on the game thread, read per present)
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
    if (ra != RA_FIXED_PLAY && ra != RA_FIXED_FRONT) { set_hz(mgr, hz); return; }   // adaptive tick and every other caller: as is
    const uint64_t now = guest_ns() / 1000u;
    const uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ); uint8_t *tick = ok_ptr(tg, 8) ? *(uint8_t **)tg : NULL;
    const int mn = cv_i(GOT_MINHZ), mx = cv_i(GOT_MAXHZ);
    const int act = tick && hz == 30 && cv_i(GOT_FIXEDTIC) > 0 && cv_i(GOT_ADAPTIVE) > 0 && cv_i(GOT_IMMEDIATE) > 0 && mn >= 10 && mx >= mn && mx <= 1000;
    const int gap = !mrt_prev_us || now < mrt_prev_us || now - mrt_prev_us > 250000u;   // first frame / after gameplay or a load
    double d = gap ? 1e9 : (double)(now - mrt_prev_us);
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

void dm_doom_tick_diag(void) {
    static int on = -1, mrt; static float scale;
    if (on < 0) {
        const char *e = getenv("DM_DOOM_TICKLOG"); on = e && *e == '1'; scale = tick_scale(); mrt = mrt_enabled();
        LOG("[tick] game speed: adaptive tick at %.3fx real time (DM_DOOM_TICK_SCALE; %s); menus: %s", scale,
            scale > 0.9601f ? "the host tops up the game's own 0.96 through its carry" : "the game's own 0.96, nothing written",
            mrt ? "real time (DM_DOOM_MENU_REALTIME=1)" : "fixed 30 Hz tic per frame (DM_DOOM_MENU_REALTIME=0)");
    }
    if (!on && !(scale > 0.9601f) && !(mrt && !mrt_state)) return;
    if (!main_ok()) {
        if (g_main_state < 0) { LOG("[tick] main is not the expected update build: speed fix, menu realtime and diagnostic off"); on = 0; scale = 0.96f; mrt = 0; }
        return;   // (modules not loaded yet: look again next present)
    }
    if (mrt && !mrt_state) mrt_install();
    uint8_t *S = g_mb + STATIC_S;
    uint64_t ts = *(volatile uint64_t *)(S + 0x90);
    int mn = cv_i(GOT_MINHZ), mx = cv_i(GOT_MAXHZ);
    if (scale > 0.9601f) tick_fix(S, ts, mn, mx, scale);
    if (!on) return;
    uint64_t now = now_ns(), carry = *(volatile uint64_t *)(S + 0xa0);   // after the fix: the carry the game's next call reads
    static uint64_t t0, prev_ts, prev_carry, n_pres, n_adapt, last_hz, fx_n0, fx_ns0; static double game_us;
    const uint64_t fx_n = __atomic_load_n(&mrt_frames, __ATOMIC_RELAXED), fx_ns = __atomic_load_n(&mrt_game_ns, __ATOMIC_RELAXED);
    if (!t0) { t0 = now; prev_ts = ts; prev_carry = carry; fx_n0 = fx_n; fx_ns0 = fx_ns; return; }
    n_pres++;
    if (ts != prev_ts && prev_ts && mn > 0 && mx > 0) {   // replay 0x1ecf940 for this frame (one adaptive call per present assumed)
        uint64_t d = ts - prev_ts, lo = 1000000u / (unsigned)mx, hi = 1000000u / (unsigned)mn;
        if (d < lo) d = lo; if (d > hi) d = hi;
        uint64_t us = (uint64_t)((float)d * 0.96f + (float)prev_carry);
        unsigned hz = us ? (unsigned)ceilf(1e6f / (float)us) : 30; if (!hz) hz = 30;
        game_us += 1e6 / hz; n_adapt++; last_hz = hz;
    } else if (mrt_state != 1) game_us += 1e6 / 30.0;   // old model: a present without an adaptive call = one 30 Hz tic
    // (hook installed: fixed-tic frames are counted where they run, mrt_set_game_hz; a present with no game frame -- the level-load
    // pacifier -- adds no game time)
    prev_ts = ts; prev_carry = carry;
    double wall = (now - t0) / 1e9;
    if (wall < 1.0) return;
    uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ), tobj = ok_ptr(tg, 8) ? *(uint64_t *)tg : 0;
    int thz = -1, tms = -1, allow = -1;
    if (tobj) { thz = *(int32_t *)tobj; tms = *(int32_t *)(tobj + 8); allow = *(uint8_t *)(tobj + 0x24); }   // tick object (0x1f2b630 layout)
    if (mrt_state == 1) game_us += (double)(fx_ns - fx_ns0) / 1e3;
    LOG("[tick] %.2fs wall: %llu presents (%.1f fps), %llu adaptive frames (last gameHz %llu), %llu fixed-tic frames (last hz %d, menu realtime %s); "
        "game time ~%.0f ms => speed %.2fx | "
        "com_fixedTic=%d com_adaptiveTick=%d immediate=%d minHz=%d maxHz=%d timescale=%.2f | tickobj hz=%d ms=%d allowHzChange=%d residual=%d | "
        "fix scale=%.3f added_us=%llu retries=%llu",
        wall, (unsigned long long)n_pres, n_pres / wall, (unsigned long long)n_adapt, (unsigned long long)last_hz,
        (unsigned long long)(fx_n - fx_n0), __atomic_load_n(&mrt_last_hz, __ATOMIC_RELAXED),
        mrt_state == 1 ? "on" : mrt_state < 0 ? "refused" : "off, frames not counted", game_us / 1e3,
        game_us / 1e6 / wall, cv_i(GOT_FIXEDTIC), cv_i(GOT_ADAPTIVE), cv_i(GOT_IMMEDIATE), mn, mx, cv_f(GOT_TIMESCALE), thz, tms, allow,
        *(int32_t *)(S + 0x14), scale, (unsigned long long)fix_added_us, (unsigned long long)fix_retries);
    t0 = now; n_pres = n_adapt = 0; game_us = 0; fx_n0 = fx_n; fx_ns0 = fx_ns;
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
    static uint64_t prev;
    uint64_t ts = *(volatile uint64_t *)(g_mb + STATIC_S + 0x90);
    int ran = ts && prev && ts != prev; prev = ts;
    return ran;
}
#else
void dm_doom_tick_diag(void) {}
#endif
