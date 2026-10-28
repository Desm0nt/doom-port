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
#include "hle.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef DM_GAME_DOOM
enum { GOT_FIXEDTIC = 0x4885de8, GOT_ADAPTIVE = 0x4884948, GOT_IMMEDIATE = 0x48d95f0, GOT_MINHZ = 0x48d95d8, GOT_MAXHZ = 0x48d95d0,
       GOT_TIMESCALE = 0x48d7478, GOT_TICKOBJ = 0x487fc40, STATIC_S = 0x66bd7c0, STR_FIXEDTIC = 0x36139c3 };
static uint8_t *g_mb; static size_t g_msz;
static int ok_ptr(uint64_t p, size_t n) { return p && p >= (uint64_t)g_mb && p + n <= (uint64_t)g_mb + g_msz; }
// idCVar: GOT slot -> object in main .bss; integer value at +0x30, float at +0x34 (how the frame loop reads them)
static int cv_i(uint64_t got) { uint64_t o = *(uint64_t *)(g_mb + got); return ok_ptr(o, 0x38) ? *(int32_t *)(o + 0x30) : -999; }
static float cv_f(uint64_t got) { uint64_t o = *(uint64_t *)(g_mb + got); return ok_ptr(o, 0x38) ? *(float *)(o + 0x34) : -1.f; }

void dm_doom_tick_diag(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_DOOM_TICKLOG"); on = e && *e == '1'; }
    if (!on) return;
    if (!g_mb) {
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_mb = g_mod[i].base; g_msz = g_mod[i].size; }
        if (!g_mb || g_msz < STATIC_S + 0x100 || memcmp(g_mb + STR_FIXEDTIC, "com_fixedTic", 13)) {
            LOG("[tick] DM_DOOM_TICKLOG: main is not the expected update build, diagnostic off"); on = 0; g_mb = NULL; return; }
    }
    uint8_t *S = g_mb + STATIC_S;
    uint64_t now = now_ns(), ts = *(volatile uint64_t *)(S + 0x90), carry = *(volatile uint64_t *)(S + 0xa0);
    static uint64_t t0, prev_ts, prev_carry, n_pres, n_adapt, last_hz; static double game_us;
    int mn = cv_i(GOT_MINHZ), mx = cv_i(GOT_MAXHZ);
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
        "com_fixedTic=%d com_adaptiveTick=%d immediate=%d minHz=%d maxHz=%d timescale=%.2f | tickobj hz=%d ms=%d allowHzChange=%d residual=%d",
        wall, (unsigned long long)n_pres, n_pres / wall, (unsigned long long)n_adapt, (unsigned long long)last_hz, game_us / 1e3,
        game_us / 1e6 / wall, cv_i(GOT_FIXEDTIC), cv_i(GOT_ADAPTIVE), cv_i(GOT_IMMEDIATE), mn, mx, cv_f(GOT_TIMESCALE), thz, tms, allow,
        *(int32_t *)(S + 0x14));
    t0 = now; n_pres = n_adapt = 0; game_us = 0;
}
#else
void dm_doom_tick_diag(void) {}
#endif
