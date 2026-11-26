// [gpu-floor] The DM_GPU_FLOOR=auto decision as a plain function (no Android, no I/O), so a host test can drive it
// (port-common/modules/gpu-floor/test/t_gpu_floor_auto.c). gpu_floor.c owns the clock levers, the clocks and the log.
//
// Per present (presenting thread): the frame's interval (present to present, wall clock) against the pacing target (the period the pacer
// aims at, dm_present_period). The floor goes ON when the frames run late: `on_run` frames in a row late (default 3), or `on_window` of
// the last 60 frames late (default 6; the pacer's own "6 missed of 60" step-down rule in hle_nvn.c, so the floor reacts before the pacer
// gives up a tier). A frame is late when its interval is more than `late_pct` % over the target (default 10: 18.3 ms against 16.67).
// It goes OFF again only with a margin: no late frame for `off_ns` (default 10 s) and, when the GPU time of a frame is known, every frame
// of that stretch with its GPU time under `idle_pct` % of the target (default 50: at the highest clock the frame would fit even at half
// of it). A release that is followed by a late run within `relapse_ns` (5 s) doubles the next calm stretch (up to `off_max_ns`, 120 s), so
// a scene that needs the floor does not flap on and off; a release that held `held_ns` (30 s) resets it.
// Not late, not calm: an interval over `hold_ns` (250 ms: a load hitch, a pause) clears the late run. Idle: no present for `gap_ns` (1 s:
// paused, in the background) releases at once (no back-off: the floor would lapse in the kernel anyway once the pulses stop).
#pragma once
#include <stdint.h>

typedef struct {
    // settings (gf_auto_init sets the defaults; gpu_floor.c reads the DM_GPU_FLOOR_* overrides)
    int late_pct, on_run, on_window, idle_pct;
    int64_t off_ns, off_max_ns, relapse_ns, held_ns, hold_ns, gap_ns;
    // state
    int on, run, win_n;
    uint64_t win;                       // the last 60 frames, bit 0 = newest, 1 = late
    int64_t calm_since, off_at, cur_off_ns, last_t;
    // counters since start (the status line takes differences)
    uint64_t frames, late, engages, releases, relapses, idle_releases;
} gf_auto_t;

enum { GF_WIN = 60 };

static inline void gf_auto_init(gf_auto_t *a) {
    a->late_pct = 10; a->on_run = 3; a->on_window = 6; a->idle_pct = 50;
    a->off_ns = 10000000000LL; a->off_max_ns = 120000000000LL; a->relapse_ns = 5000000000LL; a->held_ns = 30000000000LL;
    a->hold_ns = 250000000LL; a->gap_ns = 1000000000LL;
    a->on = 0; a->run = 0; a->win_n = 0; a->win = 0; a->calm_since = 0; a->off_at = 0; a->cur_off_ns = a->off_ns; a->last_t = 0;
    a->frames = a->late = a->engages = a->releases = a->relapses = a->idle_releases = 0;
}

static inline int gf_popcount64(uint64_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

static inline void gf_auto_clear_window(gf_auto_t *a) { a->run = 0; a->win = 0; a->win_n = 0; }

// One present at time `now` (ns, monotonic). target: the pacing period in ns (<= 0: unknown, the frame is not judged). gpu_ms: the GPU time
// of a recent frame, < 0 when unknown. Returns the floor's new state (1 on, 0 off).
static inline int gf_auto_frame(gf_auto_t *a, int64_t now, int64_t target, double gpu_ms) {
    int64_t iv = a->last_t ? now - a->last_t : 0;
    a->last_t = now;
    if (a->off_at && !a->on && now - a->off_at >= a->held_ns) { a->cur_off_ns = a->off_ns; a->off_at = 0; }   // the last release held
    if (iv <= 0) return a->on;                                         // the first present: nothing to judge yet
    if (iv >= a->gap_ns) {                                             // idle (pause, background): off at once, no back-off
        if (a->on) { a->on = 0; a->idle_releases++; a->off_at = 0; }
        gf_auto_clear_window(a);
        return a->on;
    }
    if (target <= 0) return a->on;
    a->frames++;
    if (iv > a->hold_ns) { a->run = 0; return a->on; }                 // a load hitch: neither late nor calm
    int late = iv * 100 > target * (100 + a->late_pct);
    if (late) a->late++;
    a->run = late ? a->run + 1 : 0;
    a->win = (a->win << 1) | (uint64_t)(late ? 1 : 0);
    if (a->win_n < GF_WIN) a->win_n++;
    uint64_t mask = GF_WIN >= 64 ? ~0ull : ((1ull << GF_WIN) - 1);
    if (!a->on) {
        if (a->run >= a->on_run || gf_popcount64(a->win & mask) >= a->on_window) {
            a->on = 1; a->engages++; a->calm_since = now;
            if (a->off_at && now - a->off_at < a->relapse_ns) {        // released too early: the next calm stretch is twice as long
                a->relapses++;
                a->cur_off_ns = a->cur_off_ns * 2 > a->off_max_ns ? a->off_max_ns : a->cur_off_ns * 2;
            }
            a->off_at = 0;
        }
        return a->on;
    }
    // on: a late frame, or a frame without GPU headroom, restarts the calm stretch
    if (late || (gpu_ms >= 0 && gpu_ms * 1e6 * 100 > (double)target * a->idle_pct)) a->calm_since = now;
    if (now - a->calm_since >= a->cur_off_ns) {
        a->on = 0; a->releases++; a->off_at = now;
        gf_auto_clear_window(a);
    }
    return a->on;
}
