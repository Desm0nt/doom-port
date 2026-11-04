// [front] Holding the game while the pause menu is open, "as the Switch does" (frontend-common/native/hle_hold.c, the same file in every d2r-mobile
// based port; docs/FRONTEND_UNIFIED.md). On the Switch a game behind the HOME menu is suspended: its threads stop, its sound stops, and when it comes
// back its clock goes on where it stopped. Here, while held (JNI DmActivity.nativeHold(true), jni_android.c):
//   1. graphics: dm_gfx_suspend(1) - the game's next NVN submit / present waits (in-flight GPU work drains first); the SurfaceView keeps the last frame,
//      the Java menu draws over it;
//   2. audio: dm_audio_suspend(1) - the silent sink takes over (the game's audio pacing does not stall, nothing is heard);
//   3. the guest clock: guest_clock_freeze_for(3) - frozen, and guest reads of the system counter (the cntpct gate, svc GetSystemTick) wait while
//      held (hle_os.c guest_clock_read_wait, after nx2android's), so threads that never touch NVN stop too; at the release the frozen time is
//      subtracted (no jump: the game does not see the pause as one long frame - adaptive tick, dynamic resolution, physics);
//   movies: the guest schedules frames by their PTS against this clock (dm/host/MOVIE.md), so they stop with it.
// The release goes the other way round: the clock first, then audio and graphics back to what the app's lifecycle wants (the background suspends
// them too: dm_hold_bg_audio / dm_hold_bg_graphics are the lifecycle dispatcher's audio / graphics ops, so "menu open, app to the background and
// back" keeps the game held). Holding and releasing run on their own thread (dm-hold): dm_gfx_suspend may wait for the GPU, the UI never does.
#include "hle.h"
#include <pthread.h>
#include <stdatomic.h>
void dm_gfx_suspend(int on);
void dm_audio_suspend(int on);
void guest_clock_freeze_for(int reason, int on);

enum { HOLD_REASON = 3 };   // hle_os.c: 0 tests / loading screen, 1 background, 2 system keyboard, 3 the pause menu
static pthread_mutex_t g_apply = PTHREAD_MUTEX_INITIALIZER;     // the applies (they may wait for the GPU)
static int g_bg_audio, g_bg_gfx, g_menu, g_on_audio, g_on_gfx;   // under g_apply: wanted by the lifecycle / the menu, applied
static pthread_mutex_t g_req_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_req_cv = PTHREAD_COND_INITIALIZER;
static int g_req = -1;                                           // under g_req_mu: the menu state the UI asked for last (-1 none pending)
static _Atomic int g_held;                                       // the menu hold as applied (dm_hold_active)

static void apply_locked(void) {
    int gfx = g_bg_gfx || g_menu, audio = g_bg_audio || g_menu;
    if (gfx != g_on_gfx) { dm_gfx_suspend(gfx); g_on_gfx = gfx; }
    if (audio != g_on_audio) { dm_audio_suspend(audio); g_on_audio = audio; }
}
void dm_hold_bg_audio(int on) { pthread_mutex_lock(&g_apply); g_bg_audio = on != 0; apply_locked(); pthread_mutex_unlock(&g_apply); }
void dm_hold_bg_graphics(int on) { pthread_mutex_lock(&g_apply); g_bg_gfx = on != 0; apply_locked(); pthread_mutex_unlock(&g_apply); }
int dm_hold_active(void) { return atomic_load(&g_held); }

static void *hold_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_req_mu);
        while (g_req < 0) pthread_cond_wait(&g_req_cv, &g_req_mu);
        int on = g_req; g_req = -1;
        pthread_mutex_unlock(&g_req_mu);
        pthread_mutex_lock(&g_apply);
        if (on && !g_menu) {
            uint64_t t0 = now_ns();
            g_menu = 1; apply_locked();                  // graphics first: no thread is left inside an NVN call when clock reads start to wait
            guest_clock_freeze_for(HOLD_REASON, 1);
            atomic_store(&g_held, 1);
            LOG("[java] menu open: the game is paused (graphics held, audio to the silent sink, guest clock frozen) in %.1f ms", (now_ns() - t0) / 1e6);
        } else if (!on && g_menu) {
            guest_clock_freeze_for(HOLD_REASON, 0);       // the clock goes on first: the threads waiting in a clock read see it continue
            g_menu = 0; apply_locked();
            atomic_store(&g_held, 0);
            LOG("[java] menu closed: the game goes on");
        }
        pthread_mutex_unlock(&g_apply);
    }
    return NULL;
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void hold_start(void) { pthread_t th; if (!pthread_create(&th, NULL, hold_main, NULL)) { pthread_setname_np(th, "dm-hold"); pthread_detach(th); } }
// The UI's request: latest state wins, never waits
void dm_hold_menu(int on) {
    pthread_once(&g_once, hold_start);
    pthread_mutex_lock(&g_req_mu); g_req = on != 0; pthread_cond_signal(&g_req_cv); pthread_mutex_unlock(&g_req_mu);
}
