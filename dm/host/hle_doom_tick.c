// [DOOM] Game-speed diagnostic (DM_DOOM_TICKLOG=1; off by default). Called once per present from dm_guest_frame_tick (hle_os.c).
// Once per wall second it logs the present rate, the tick cvars as the game sees them, and an estimate of how much game time the
// game advanced in that second -- so "runs fast / slow" becomes a number (speed 1.00 = real time). See docs/research/game_speed.md.
// All addresses are update main (build id 2847991952b3d7ab) image offsets; a string check refuses any other main.
//   Frame loop 0x1ecfa80 / 0x1ed0f00: com_fixedTic != 0 -> one game frame per render frame. Inside it, com_adaptiveTick +
//   ImmediateMode -> 0x1ecf940, which keeps in static S = main+0x66bd7c0: S+0x90 last call time (us, Sys_Microseconds), S+0xa0 the
//   rounding carry (us); per call: us = clamp(now - last, 1e6/MaxHz, 1e6/MinHz) * 0.96 + carry; gameHz = ceil(1e6 / us);
//   carry = us - 1e6/gameHz. So a call with a changed S+0x90 is one adaptive game frame of 1e6/gameHz us, which we replay here.
//   A present without an adaptive call is counted as one fixed tic of 1000/30 ms (the Switch default, 0x1f2b630) -- this model is
//   wrong while paused / loading, so read the numbers in gameplay.
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

void dm_doom_tick_diag(void) {
    static int on = -1; static float scale;
    if (on < 0) {
        const char *e = getenv("DM_DOOM_TICKLOG"); on = e && *e == '1'; scale = tick_scale();
        LOG("[tick] game speed: adaptive tick at %.3fx real time (DM_DOOM_TICK_SCALE; %s)", scale,
            scale > 0.9601f ? "the host tops up the game's own 0.96 through its carry" : "the game's own 0.96, nothing written");
    }
    if (!on && !(scale > 0.9601f)) return;
    if (!main_ok()) {
        if (g_main_state < 0) { LOG("[tick] main is not the expected update build: speed fix and diagnostic off"); on = 0; scale = 0.96f; }
        return;   // (modules not loaded yet: look again next present)
    }
    uint8_t *S = g_mb + STATIC_S;
    uint64_t ts = *(volatile uint64_t *)(S + 0x90);
    int mn = cv_i(GOT_MINHZ), mx = cv_i(GOT_MAXHZ);
    if (scale > 0.9601f) tick_fix(S, ts, mn, mx, scale);
    if (!on) return;
    uint64_t now = now_ns(), carry = *(volatile uint64_t *)(S + 0xa0);   // after the fix: the carry the game's next call reads
    static uint64_t t0, prev_ts, prev_carry, n_pres, n_adapt, last_hz; static double game_us;
    if (!t0) { t0 = now; prev_ts = ts; prev_carry = carry; return; }
    n_pres++;
    if (ts != prev_ts && prev_ts && mn > 0 && mx > 0) {   // replay 0x1ecf940 for this frame (one adaptive call per present assumed)
        uint64_t d = ts - prev_ts, lo = 1000000u / (unsigned)mx, hi = 1000000u / (unsigned)mn;
        if (d < lo) d = lo; if (d > hi) d = hi;
        uint64_t us = (uint64_t)((float)d * 0.96f + (float)prev_carry);
        unsigned hz = us ? (unsigned)ceilf(1e6f / (float)us) : 30; if (!hz) hz = 30;
        game_us += 1e6 / hz; n_adapt++; last_hz = hz;
    } else game_us += 1e6 / 30.0;
    prev_ts = ts; prev_carry = carry;
    double wall = (now - t0) / 1e9;
    if (wall < 1.0) return;
    uint64_t tg = *(uint64_t *)(g_mb + GOT_TICKOBJ), tobj = ok_ptr(tg, 8) ? *(uint64_t *)tg : 0;
    int thz = -1, tms = -1, allow = -1;
    if (tobj) { thz = *(int32_t *)tobj; tms = *(int32_t *)(tobj + 8); allow = *(uint8_t *)(tobj + 0x24); }   // tick object (0x1f2b630 layout)
    LOG("[tick] %.2fs wall: %llu presents (%.1f fps), %llu adaptive frames (last gameHz %llu); game time ~%.0f ms => speed %.2fx | "
        "com_fixedTic=%d com_adaptiveTick=%d immediate=%d minHz=%d maxHz=%d timescale=%.2f | tickobj hz=%d ms=%d allowHzChange=%d residual=%d | "
        "fix scale=%.3f added_us=%llu retries=%llu",
        wall, (unsigned long long)n_pres, n_pres / wall, (unsigned long long)n_adapt, (unsigned long long)last_hz, game_us / 1e3,
        game_us / 1e6 / wall, cv_i(GOT_FIXEDTIC), cv_i(GOT_ADAPTIVE), cv_i(GOT_IMMEDIATE), mn, mx, cv_f(GOT_TIMESCALE), thz, tms, allow,
        *(int32_t *)(S + 0x14), scale, (unsigned long long)fix_added_us, (unsigned long long)fix_retries);
    t0 = now; n_pres = n_adapt = 0; game_us = 0;
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
