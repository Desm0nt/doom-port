// 安卓前端（front.h 的安卓实现，对应 Mac 的 front_mac.m、iOS 的 front_ios.m）。
// 两种形态：
//   命令行（adb shell 直接跑 dmhost，开发验证用）：没有窗口，front_run 在当前线程直接跑客体，front_metal_layer 为 NULL（图形层走无窗口路径）。
//   App（Java 外壳经 JNI 交来 ANativeWindow，见 jni_android.c）：客体在独立线程跑，front_metal_layer 返回 ANativeWindow*（图形层出图的目标面）。
// Android front end (the Android implementation of front.h, counterpart of Mac's front_mac.m and iOS's front_ios.m).
// Two forms:
//   Command line (dmhost run directly from adb shell, for development checks): no window, front_run runs the guest directly on the current thread, front_metal_layer is NULL (the graphics layer takes the no-window path).
//   App (the Java shell hands over an ANativeWindow through JNI, see jni_android.c): the guest runs on its own thread, front_metal_layer returns the ANativeWindow* (the surface the graphics layer presents to).
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "touch_android.h"
#include "../ios/Sources/dm_touch_game_state.h"
#include <math.h>
#include <stdatomic.h>

static void *_Atomic g_window;      // ANativeWindow*；命令行形态为 NULL | ANativeWindow*; NULL in the command-line form
static _Atomic int g_screen_w, g_screen_h;
void front_android_set_window(void *win, int w, int h) { atomic_store(&g_window, win); atomic_store(&g_screen_w, w); atomic_store(&g_screen_h, h); }

// UI 线程从当前 Display 读实际刷新率；0 表示未知，不冒充实测 60 Hz。
// The UI thread reads the current Display refresh rate; 0 means unknown, not a measured 60 Hz.
static _Atomic int g_refresh_millihz;
void front_android_set_refresh(float hz) {
    atomic_store(&g_refresh_millihz, isfinite(hz) && hz > 0 && hz < 1000 ? (int)lroundf(hz * 1000) : 0);
}
double front_display_refresh(char *name, size_t n) {
    if (name && n) *name = 0;
    return atomic_load(&g_refresh_millihz) / 1000.0;
}

void *front_metal_layer(void) { return atomic_load(&g_window); }

void front_run(void (*guest)(void)) {
    // 观测门在装载后才可订阅；这里沿用 iOS 在 front_run 接入的时机。
    // Observation gates exist only after loading; subscribe at front_run as iOS does.
    if (front_metal_layer()) {
#ifndef DM_GAME_DOOM   // [DOOM] D2R UI-message observation (diag.c) and skill-art bridge (dm_touch_game_bridge.c) are not built
        void uimsg_subscribe(void (*)(const char *, const char *, const char *));
        uimsg_subscribe(dm_android_touch_uimsg);
#endif
        dm_pad_hook = dm_android_touch_step;
#ifndef DM_GAME_DOOM
        dm_touch_game_attach();
        dm_touch_game_live_check(1);
#endif
    }
    guest();   // 两种形态都在调用线程上跑：App 形态由 JNI 侧先起好客体线程再进 dm_main | Both forms run on the calling thread: in the App form the JNI side starts the guest thread first and then enters dm_main
}

// 游戏输出分辨率：DM_GAME_RES 钉住；有屏幕尺寸时按同高度放宽到屏幕比例（与 Mac 真全屏、iOS 同一规则，宽 ≤ 4096、8 对齐），否则原版 16:9
// Game output resolution: pinned by DM_GAME_RES; with a known screen size, widen at the same height to the screen aspect (the same rule as Mac true full screen and iOS, width ≤ 4096, aligned to 8), otherwise the original 16:9
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
    unsigned bh = docked ? 900 : 720;
    if (dm > 0 && sh > 0 && dm * 9 > sh * 16) {
        unsigned bw = (unsigned)(llround((double)bh * dm / sh / 8) * 8); if (bw > 4096) bw = 4096;
        return (uint64_t)bh << 32 | bw;
    }
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

#ifdef DM_GAME_DOOM
// ---------------- [DOOM] game controllers ----------------
// DmActivity forwards the keys and motion events of gamepad / joystick / d-pad sources (jni_android.c). The controller feeds the separate
// controller channel of hle_hid.c (dm_pad_set: buttons ORed with touch, a stick used when the touch stick is idle), reported to the game as
// the connected npad (full key docked / handheld, hle_oe.c's operation mode). Face buttons by position, as the Switch (and nx2android / iOS)
// map them: the bottom one is B, the right one A; DM_PAD_LAYOUT=label maps them by their printed letter instead (A -> A). Analog triggers
// become ZL / ZR with hysteresis (pressed above 0.35, released below 0.25); the hat and the d-pad keys both give the d-pad. Sticks: radial
// dead zone 0.10, rescaled so the full range stays reachable, y flipped (Android grows downward, the Switch upward).
// Portions ported from nx2android (https://github.com/seedds/nx2android) nxnative/dm/android/front_android.c (key_bit, pad_publish),
// Copyright (c) 2026 seedds, MIT License.
#include <android/keycodes.h>
#include <android/sensor.h>
#include <android/looper.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint64_t g_pad_keys, g_pad_trig; static float g_pad_ax[8]; static int g_pad_dev = -1;   // UI thread only
static uint64_t key_bit(int32_t k) {
    static int label = -1; if (label < 0) { const char *e = getenv("DM_PAD_LAYOUT"); label = e && !strcmp(e, "label"); }
    switch (k) {
    case AKEYCODE_BUTTON_A: return label ? DM_BTN_A : DM_BTN_B; case AKEYCODE_BUTTON_B: return label ? DM_BTN_B : DM_BTN_A;
    case AKEYCODE_BUTTON_X: return label ? DM_BTN_X : DM_BTN_Y; case AKEYCODE_BUTTON_Y: return label ? DM_BTN_Y : DM_BTN_X;
    case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R; case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
    case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS;
    case AKEYCODE_BUTTON_START: case AKEYCODE_MENU: return DM_BTN_PLUS;
#ifdef DM_GAME_DOOM
    // Keyboard (emulator / BT keyboard): Enter/Space confirm (Switch A), Esc/Backspace back (B), arrows/WASD d-pad,
    // Tab menu (+), Q/E bumpers, Shift/Ctrl triggers (aim / fire), F = Y, R = X, M = map (-).
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_SPACE: return DM_BTN_A;
    case AKEYCODE_ESCAPE: case AKEYCODE_DEL: return DM_BTN_B;
    case AKEYCODE_W: return DM_BTN_UP; case AKEYCODE_S: return DM_BTN_DOWN; case AKEYCODE_A: return DM_BTN_LEFT; case AKEYCODE_D: return DM_BTN_RIGHT;
    case AKEYCODE_TAB: return DM_BTN_PLUS; case AKEYCODE_M: return DM_BTN_MINUS;
    case AKEYCODE_Q: return DM_BTN_L; case AKEYCODE_E: return DM_BTN_R;
    case AKEYCODE_SHIFT_LEFT: return DM_BTN_ZL; case AKEYCODE_CTRL_LEFT: return DM_BTN_ZR;
    case AKEYCODE_F: return DM_BTN_Y; case AKEYCODE_R: return DM_BTN_X;
#endif
    case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return DM_BTN_MINUS;   // (Back only arrives here from a controller: DmActivity forwards controller sources only)
    case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN; case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT;
    default: return 0;
    }
}
// Physical sticks get a 0.10 dead zone against drift. The on-screen pad (DoomPadView, device 0x7d00) gets none: its look stick
// carries a finger's slow drag as small tilts, which that dead zone used to zero (slow aiming did not move the view at all).
static void stick(float x, float y, int32_t *ox, int32_t *oy) {
    float m = sqrtf(x * x + y * y), dz = g_pad_dev == 0x7d00 ? 0.0f : 0.10f;
    if (!(m > dz) || m == 0) { *ox = *oy = 0; return; }
    float s = (m > 1 ? 1 : (m - dz) / (1 - dz)) / m;
    float fx = x * s, fy = -y * s;
    *ox = (int32_t)lroundf(fmaxf(-1, fminf(1, fx)) * DM_STICK_MAX); *oy = (int32_t)lroundf(fmaxf(-1, fminf(1, fy)) * DM_STICK_MAX);
}
static void pad_publish(void) {
    uint64_t b = g_pad_keys;
    for (int i = 0; i < 2; i++) { uint64_t bit = i ? DM_BTN_ZR : DM_BTN_ZL; float v = g_pad_ax[4 + i];
        if (v > 0.35f) g_pad_trig |= bit; else if (v < 0.25f) g_pad_trig &= ~bit; }
    b |= g_pad_trig;
    if (g_pad_ax[6] < -0.5f) b |= DM_BTN_LEFT; if (g_pad_ax[6] > 0.5f) b |= DM_BTN_RIGHT;
    if (g_pad_ax[7] < -0.5f) b |= DM_BTN_UP; if (g_pad_ax[7] > 0.5f) b |= DM_BTN_DOWN;
    int32_t lx, ly, rx, ry; stick(g_pad_ax[0], g_pad_ax[1], &lx, &ly); stick(g_pad_ax[2], g_pad_ax[3], &rx, &ry);
    dm_pad_set(b, lx, ly, rx, ry);
}
static void pad_using(int device) {
    if (device == g_pad_dev) return;
    if (g_pad_dev >= 0) { g_pad_keys = g_pad_trig = 0; memset(g_pad_ax, 0, sizeof g_pad_ax); }   // another controller took over: nothing stays held from the old one
    g_pad_dev = device; dm_haptics_android_device(device);
    LOG("[input] controller in use: device %d", device);
}
int front_android_pad_key(int device, int keycode, int down) {
    uint64_t bit = key_bit(keycode);
    if (!bit) {   // the rest stays the system's; a controller key nothing maps is logged once, so a new controller's layout can be found
        static uint8_t seen[64];
        if (down && keycode >= 0 && keycode < 512 && !(seen[keycode >> 3] >> (keycode & 7) & 1)) { seen[keycode >> 3] |= (uint8_t)(1 << (keycode & 7)); LOG("[input] controller key %d (device %d): not mapped", keycode, device); }
        return 0;
    }
    pad_using(device);
    { static int n; if (down && n++ < 20) LOG("[input] key %d (device %d) -> button %#llx", keycode, device, (unsigned long long)bit); }
    if (down) g_pad_keys |= bit; else g_pad_keys &= ~bit;
    pad_publish();
    return 1;
}
void front_android_pad_axes(int device, const float axes[8]) {
    pad_using(device);
    memcpy(g_pad_ax, axes, sizeof g_pad_ax);
    pad_publish();
}
void front_android_pad_removed(int device) {
    if (device != g_pad_dev) return;
    g_pad_keys = g_pad_trig = 0; memset(g_pad_ax, 0, sizeof g_pad_ax); g_pad_dev = -1;
    dm_pad_set(0, 0, 0, 0, 0); dm_haptics_android_device(-1);
    LOG("[input] controller %d removed: rumble back on the phone", device);
}

// ---------------- [DOOM] six-axis sensor from the phone ----------------
// The gyroscope (200 Hz) and the accelerometer (100 Hz) are sampled on a thread of their own while the app is in front (front_android_sensors,
// jni_android.c nativeFocus), turned from the phone's natural frame to the screen's (Display.getRotation: x right, y up, z out of the screen)
// and handed to hle_hid.c (dm_sixaxis_push), which reports them as the npad's six-axis sensor. DM_GYRO=0: no sensor reported.
void dm_sixaxis_push(const float gyro_rad[3], const float acc_ms2[3], int64_t t_ns);   // hle_hid.c
void dm_sixaxis_available(int have);
static _Atomic int g_rotation, g_sensors_want = -1;
static ALooper *_Atomic g_sensor_looper;
void front_android_set_rotation(int rotation) { if (atomic_exchange(&g_rotation, rotation & 3) != (rotation & 3)) LOG("[input] display rotation %d", (rotation & 3) * 90); }
static void to_screen(const float *v, float *o) {   // natural (portrait) device axes -> screen axes for the current rotation
    switch (atomic_load(&g_rotation)) {
    case 1: o[0] = -v[1]; o[1] = v[0]; break;
    case 2: o[0] = -v[0]; o[1] = -v[1]; break;
    case 3: o[0] = v[1]; o[1] = -v[0]; break;
    default: o[0] = v[0]; o[1] = v[1]; break;
    }
    o[2] = v[2];
}
static void *sensor_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "dm-sensors");
    char pkg[256] = "org.doom.port";
    { FILE *f = fopen("/proc/self/cmdline", "r"); if (f) { size_t n = fread(pkg, 1, sizeof pkg - 1, f); fclose(f); pkg[n] = 0; if (!pkg[0]) strcpy(pkg, "org.doom.port"); } }
    // ALLOW_NON_CALLBACKS: the event queue is added without a callback (ident 1); with ALooper_prepare(0) such an fd never wakes
    // pollOnce, so events were only drained by the 2 s timeout -- the game saw the gyro in 2 s bursts, i.e. a still camera (28.10).
    ALooper *lp = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorManager *sm = ASensorManager_getInstanceForPackage(pkg);
    const ASensor *gy = sm ? ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_GYROSCOPE) : NULL, *ac = sm ? ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_ACCELEROMETER) : NULL;
    ASensorEventQueue *q = gy ? ASensorManager_createEventQueue(sm, lp, 1, NULL, NULL) : NULL;
    if (!q) { LOG("[input] no gyroscope: no six-axis sensor"); dm_sixaxis_available(0); return NULL; }
    LOG("[input] gyroscope %s (%s), accelerometer %s", ASensor_getName(gy), ASensor_getVendor(gy), ac ? ASensor_getName(ac) : "none");
    dm_sixaxis_available(1);
    atomic_store(&g_sensor_looper, lp);
    int on = 0;
    for (;;) {
        int want = atomic_load(&g_sensors_want) > 0;
        if (want != on) {
            on = want;
            int rg = 0, ra = 0;
            if (on) {
                // registerSensor's result was not checked: a refused rate (e.g. above the app's sensor-rate limit) left the queue silent
                // with "sampling" logged. On failure fall back to enableSensor + setEventRate (at least the sensor's min delay, 200 Hz otherwise).
                rg = ASensorEventQueue_registerSensor(q, gy, 5000, 0);
                if (rg < 0) { int md = ASensor_getMinDelay(gy); rg = ASensorEventQueue_enableSensor(q, gy); if (rg >= 0) ASensorEventQueue_setEventRate(q, gy, md > 5000 ? md : 5000); }
                if (ac) { ra = ASensorEventQueue_registerSensor(q, ac, 10000, 0); if (ra < 0) { ra = ASensorEventQueue_enableSensor(q, ac); if (ra >= 0) ASensorEventQueue_setEventRate(q, ac, 10000); } }
            } else { ASensorEventQueue_disableSensor(q, gy); if (ac) ASensorEventQueue_disableSensor(q, ac); }
            LOG("[input] motion sensors %s (gyro rc %d, accel rc %d)", on ? "sampling" : "paused", rg, ra);
        }
        ALooper_pollOnce(on ? 2000 : -1, NULL, NULL, NULL);   // 2 s timeout while sampling: a silent gyro is reported below
        static unsigned ng, nall, nlog; static int64_t tl; static int vlog = -1; if (vlog < 0) vlog = getenv("DM_GYRO_LOG") != NULL;
        ASensorEvent ev[32]; ssize_t n;
        while ((n = ASensorEventQueue_getEvents(q, ev, 32)) > 0)
            for (ssize_t i = 0; i < n; i++) {
                float v[3] = {ev[i].data[0], ev[i].data[1], ev[i].data[2]}, s[3]; to_screen(v, s);
                if (ev[i].type == ASENSOR_TYPE_GYROSCOPE) { dm_sixaxis_push(s, NULL, ev[i].timestamp);
                    ng++; if (!nall++) LOG("[input] first gyro sample: %.3f %.3f %.3f rad/s (screen frame, rotation %d)", s[0], s[1], s[2], atomic_load(&g_rotation) * 90); }
                else if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) dm_sixaxis_push(NULL, s, ev[i].timestamp);
            }
        if (on) {   // gyro event count every 5 s with DM_GYRO_LOG; without it only "no events" is reported (twice at most)
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); int64_t t = ts.tv_sec;
            if (!tl) tl = t;
            if (t - tl >= 5) { if (vlog || (!ng && nlog < 2 && ++nlog)) LOG("[input] gyro: %u events in %lld s", ng, (long long)(t - tl)); ng = 0; tl = t; }
        } else tl = 0;
    }
    return NULL;
}
static void sensor_start(void) { pthread_t t; if (!pthread_create(&t, NULL, sensor_thread, NULL)) pthread_detach(t); }
void front_android_sensors(int on) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    static int disabled = -1; if (disabled < 0) { const char *e = getenv("DM_GYRO"); disabled = e && !atoi(e); if (disabled) dm_sixaxis_available(0); }
    if (disabled) return;
    atomic_store(&g_sensors_want, on != 0);
    pthread_once(&once, sensor_start);
    ALooper *lp = atomic_load(&g_sensor_looper); if (lp) ALooper_wake(lp);
}
#else   // D2R: controllers and motion sensors stay as before (not forwarded on Android)
int front_android_pad_key(int device, int keycode, int down) { (void)device; (void)keycode; (void)down; return 0; }
void front_android_pad_axes(int device, const float axes[8]) { (void)device; (void)axes; }
void front_android_pad_removed(int device) { (void)device; }
void front_android_set_rotation(int rotation) { (void)rotation; }
void front_android_sensors(int on) { (void)on; }
#endif

// 文本输入框：App 用 text_input_jni.c 的强实现弹系统输入框；命令行无窗口，返回 -1。
// Text input: the App overrides this with text_input_jni.c; windowless command-line runs return -1.
__attribute__((weak)) int front_android_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!front_metal_layer()) return -1;
    return front_android_text_input(title, sub, guide, initial, max_chars, out, out_cap);
}
