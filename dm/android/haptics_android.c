// Android vibration: the game's rumble (hle_haptics.c's state for player 1) goes to android.os.Vibrator / VibratorManager through JNI, on a
// thread of its own: the haptics sink runs on the game's thread under hle_haptics.c's lock, and every vibrate call is a binder transaction.
// Rumble goes to the controller in use when it has a motor (InputDevice.getVibratorManager, Android 12), else to the phone. A controller with
// two motors (Xbox / DualShock style: a heavy and a light one) gets the Switch's low band on the first and the high band on the second, so the
// HD-rumble frequency split survives; one motor gets the mixed intensity. Android has no continuous rumble with a changing strength, so a
// strength is a one-shot of RUMBLE_MS renewed while the game keeps the motor running.
// Switches (env file): DM_RUMBLE=0 no rumble at all (the game then sees no vibration devices); DM_RUMBLE_PHONE=0 never on the phone's own motor;
// DM_RUMBLE_GAIN=<0..2> strength scale (default 1).
// Portions ported from nx2android (https://github.com/seedds/nx2android) nxnative/dm/android/haptics_android.c, Copyright (c) 2026 seedds, MIT License.
#include "../host/dm.h"
#include "../host/hle_haptics.h"
#include "touch_android.h"
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef DM_GAME_DOOM

#define RUMBLE_MS 600
#define RENEW_NS 400000000ll
#define GAP_NS 25000000ll   // at most one rumble change per this
static JavaVM *g_vm; static jobject g_ctx;   // the application Context (global reference)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static dm_haptic_state g_state; static int g_taps, g_dev = -1, g_dev_new; static _Atomic int g_on = 1, g_started;
static int g_phone = 1; static float g_gain = 1;
static int same(const dm_haptic_state *a, const dm_haptic_state *b) { return a->epoch == b->epoch && !memcmp(a->motor, b->motor, sizeof a->motor); }
static int64_t mono_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static void sink(dm_haptic_state s) {   // hle_haptics.c, under its lock: only hand the state over
    if (s.player) return;
    pthread_mutex_lock(&g_mu); g_state = s; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}
void dm_haptics_android_tap(void) { if (!atomic_load(&g_on) || !atomic_load(&g_started)) return; pthread_mutex_lock(&g_mu); g_taps++; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
void dm_haptics_android_device(int id) {
    if (!atomic_load(&g_started)) return;
    pthread_mutex_lock(&g_mu); if (id != g_dev) { g_dev = id; g_dev_new = 1; pthread_cond_signal(&g_cv); } pthread_mutex_unlock(&g_mu);
}
void dm_haptics_android_set(int on) { atomic_store(&g_on, on != 0); pthread_mutex_lock(&g_mu); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
void dm_haptics_android_foreground(int fg) { if (atomic_load(&g_started)) dm_haptics_suspend(DM_HAPTICS_BACKGROUND, !fg); }

static JNIEnv *E;
static jclass cVE, cID, cCV; static jmethodID mOneShot, mPredef, mVibrate, mCancel, mHas, mAmp, mGetDevice, mGetVib, mGetVm;
static jmethodID mVmIds, mVmVib, mVmVibrate, mVmCancel, mCvStart, mPcAdd, mPcCombine;
// Game rumble must carry a usage: without one Android files it as TOUCH feedback and drops it when the phone's touch haptics
// setting is off ("ended with status IGNORED_FOR_SETTINGS", 28.10). VibrationAttributes USAGE_MEDIA (API 30+, the class games
// use) where available, else AudioAttributes USAGE_GAME on Vibrator.vibrate (API 26+).
static jobject g_vattrs, g_aattrs; static jmethodID mVibrateVA, mVibrateAA, mVmVibrateVA;
// A motor set: one Vibrator (v), or a VibratorManager with two motors (vm + ids) driven as low / high band. amp: amplitude control.
typedef struct { jobject v, vm; jint id[2]; int amp, dual; } vib_t;
static int jok(const char *what) {
    if (!(*E)->ExceptionCheck(E)) return 1;
    (*E)->ExceptionClear(E); LOG("[RED] [haptics] Java exception in %s", what); return 0;
}
static int vib_has_amp(jobject v) { jboolean a = (*E)->CallBooleanMethod(E, v, mAmp); return jok("hasAmplitudeControl") && a; }
static int vib_take(vib_t *v, jobject o) {   // o: a Vibrator, local reference, released here
    memset(v, 0, sizeof *v);
    if (!o) return 0;
    jboolean has = (*E)->CallBooleanMethod(E, o, mHas); if (!jok("hasVibrator")) has = 0;
    if (has) { v->v = (*E)->NewGlobalRef(E, o); v->amp = vib_has_amp(o); }
    (*E)->DeleteLocalRef(E, o);
    return v->v != NULL;
}
static int vm_take(vib_t *v, jobject vm) {   // vm: a VibratorManager (local reference, released here); two motors -> dual, one -> plain
    memset(v, 0, sizeof *v);
    if (!vm) return 0;
    jintArray ids = (*E)->CallObjectMethod(E, vm, mVmIds); jsize n = jok("getVibratorIds") && ids ? (*E)->GetArrayLength(E, ids) : 0;
    if (n >= 2) {
        (*E)->GetIntArrayRegion(E, ids, 0, 2, v->id);
        jobject a = (*E)->CallObjectMethod(E, vm, mVmVib, v->id[0]), b = jok("getVibrator") ? (*E)->CallObjectMethod(E, vm, mVmVib, v->id[1]) : NULL;
        if (jok("getVibrator") && a && b) { v->vm = (*E)->NewGlobalRef(E, vm); v->dual = 1; v->amp = vib_has_amp(a) && vib_has_amp(b); }
        if (a) (*E)->DeleteLocalRef(E, a); if (b) (*E)->DeleteLocalRef(E, b);
    } else if (n == 1) {
        jint id; (*E)->GetIntArrayRegion(E, ids, 0, 1, &id);
        jobject a = (*E)->CallObjectMethod(E, vm, mVmVib, id); if (jok("getVibrator")) vib_take(v, a);
    }
    if (ids) (*E)->DeleteLocalRef(E, ids);
    (*E)->DeleteLocalRef(E, vm);
    return v->v || v->vm;
}
static void vib_drop(vib_t *v) { if (v->v) (*E)->DeleteGlobalRef(E, v->v); if (v->vm) (*E)->DeleteGlobalRef(E, v->vm); memset(v, 0, sizeof *v); }
static int vib_ok(const vib_t *v) { return v->v || v->vm; }
static jobject one_shot(int ms, int amp) { jobject e = (*E)->CallStaticObjectMethod(E, cVE, mOneShot, (jlong)ms, (jint)amp); return jok("createOneShot") ? e : NULL; }
static void vib_effect(jobject v, jobject e) {
    if (!e) return;
    if (g_vattrs && mVibrateVA) (*E)->CallVoidMethod(E, v, mVibrateVA, e, g_vattrs);
    else if (g_aattrs && mVibrateAA) (*E)->CallVoidMethod(E, v, mVibrateAA, e, g_aattrs);
    else (*E)->CallVoidMethod(E, v, mVibrate, e);
    jok("vibrate"); (*E)->DeleteLocalRef(E, e);
}
static void vib_play(vib_t *v, int ms, int amp_lo, int amp_hi, int amp_mix) {
    if (!v->dual) { vib_effect(v->v, one_shot(ms, v->amp ? amp_mix : -1)); return; }
    jobject pc = (*E)->CallStaticObjectMethod(E, cCV, mCvStart); if (!jok("startParallel") || !pc) return;
    int a[2] = {amp_lo, amp_hi};
    for (int k = 0; k < 2; k++) {
        if (!a[k]) continue;   // a silent band: that motor stays off (a one-shot needs amplitude >= 1)
        jobject e = one_shot(ms, v->amp ? a[k] : -1); if (!e) continue;
        jobject r = (*E)->CallObjectMethod(E, pc, mPcAdd, v->id[k], e); jok("addVibrator");
        if (r) (*E)->DeleteLocalRef(E, r);
        (*E)->DeleteLocalRef(E, e);
    }
    jobject cv = (*E)->CallObjectMethod(E, pc, mPcCombine); (*E)->DeleteLocalRef(E, pc);
    if (!jok("combine") || !cv) return;   // (combine throws when nothing was added; amp_mix > 0 means at least one band is on)
    if (g_vattrs && mVmVibrateVA) (*E)->CallVoidMethod(E, v->vm, mVmVibrateVA, cv, g_vattrs); else (*E)->CallVoidMethod(E, v->vm, mVmVibrate, cv);
    jok("VibratorManager.vibrate"); (*E)->DeleteLocalRef(E, cv);
}
static void vib_tap(vib_t *v) {   // a short, light tick; without amplitude control the system's click
    if (v->dual || !v->v) return;
    if (v->amp) { vib_effect(v->v, one_shot(14, 140)); return; }
    jobject e = (*E)->CallStaticObjectMethod(E, cVE, mPredef, (jint)0 /* EFFECT_CLICK */); if (jok("createPredefined")) vib_effect(v->v, e);
}
static void vib_stop(vib_t *v) {
    if (v->dual) { (*E)->CallVoidMethod(E, v->vm, mVmCancel); jok("VibratorManager.cancel"); }
    else if (v->v) { (*E)->CallVoidMethod(E, v->v, mCancel); jok("cancel"); }
}
static int amp255(float x) { int a = (int)(x * g_gain * 255 + 0.5f); return a > 255 ? 255 : a < 0 ? 0 : a; }

static void *haptics_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "haptics");
    if ((*g_vm)->AttachCurrentThread(g_vm, &E, NULL) != JNI_OK) { LOG("[RED] [haptics] cannot attach to the Java VM"); return NULL; }
    jclass cCtx = (*E)->FindClass(E, "android/content/Context"), cVib = (*E)->FindClass(E, "android/os/Vibrator"), cVm = (*E)->FindClass(E, "android/os/VibratorManager");
    jclass cPc = (*E)->FindClass(E, "android/os/CombinedVibration$ParallelCombination");
    cVE = (*E)->NewGlobalRef(E, (*E)->FindClass(E, "android/os/VibrationEffect")); cID = (*E)->NewGlobalRef(E, (*E)->FindClass(E, "android/view/InputDevice"));
    cCV = (*E)->NewGlobalRef(E, (*E)->FindClass(E, "android/os/CombinedVibration"));
    if (!jok("FindClass") || !cCtx || !cVib || !cVm || !cPc || !cVE || !cID || !cCV) { LOG("[RED] [haptics] Vibrator classes missing: no rumble"); return NULL; }
    jmethodID mSvc = (*E)->GetMethodID(E, cCtx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    mOneShot = (*E)->GetStaticMethodID(E, cVE, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
    mPredef = (*E)->GetStaticMethodID(E, cVE, "createPredefined", "(I)Landroid/os/VibrationEffect;");
    mVibrate = (*E)->GetMethodID(E, cVib, "vibrate", "(Landroid/os/VibrationEffect;)V"); mCancel = (*E)->GetMethodID(E, cVib, "cancel", "()V");
    mHas = (*E)->GetMethodID(E, cVib, "hasVibrator", "()Z"); mAmp = (*E)->GetMethodID(E, cVib, "hasAmplitudeControl", "()Z");
    mGetDevice = (*E)->GetStaticMethodID(E, cID, "getDevice", "(I)Landroid/view/InputDevice;"); mGetVib = (*E)->GetMethodID(E, cID, "getVibrator", "()Landroid/os/Vibrator;");
    mGetVm = (*E)->GetMethodID(E, cID, "getVibratorManager", "()Landroid/os/VibratorManager;");
    mVmIds = (*E)->GetMethodID(E, cVm, "getVibratorIds", "()[I"); mVmVib = (*E)->GetMethodID(E, cVm, "getVibrator", "(I)Landroid/os/Vibrator;");
    mVmVibrate = (*E)->GetMethodID(E, cVm, "vibrate", "(Landroid/os/CombinedVibration;)V"); mVmCancel = (*E)->GetMethodID(E, cVm, "cancel", "()V");
    jmethodID mVmDefault = (*E)->GetMethodID(E, cVm, "getDefaultVibrator", "()Landroid/os/Vibrator;");
    mCvStart = (*E)->GetStaticMethodID(E, cCV, "startParallel", "()Landroid/os/CombinedVibration$ParallelCombination;");
    mPcAdd = (*E)->GetMethodID(E, cPc, "addVibrator", "(ILandroid/os/VibrationEffect;)Landroid/os/CombinedVibration$ParallelCombination;");
    mPcCombine = (*E)->GetMethodID(E, cPc, "combine", "()Landroid/os/CombinedVibration;");
    if (!jok("GetMethodID")) { LOG("[RED] [haptics] Vibrator methods missing: no rumble"); return NULL; }
    {   // usage attributes (optional: older Android lacks some; a missing one leaves the plain call)
        jclass cVA = (*E)->FindClass(E, "android/os/VibrationAttributes"); if (!jok("FindClass VibrationAttributes")) cVA = NULL;
        if (cVA) {
            jmethodID mFor = (*E)->GetStaticMethodID(E, cVA, "createForUsage", "(I)Landroid/os/VibrationAttributes;"); if (!jok("createForUsage")) mFor = NULL;
            jobject va = mFor ? (*E)->CallStaticObjectMethod(E, cVA, mFor, (jint)19) : NULL;   // USAGE_MEDIA
            if (va && jok("createForUsage call")) g_vattrs = (*E)->NewGlobalRef(E, va);
            mVibrateVA = (*E)->GetMethodID(E, cVib, "vibrate", "(Landroid/os/VibrationEffect;Landroid/os/VibrationAttributes;)V"); if (!jok("vibrate(VA)")) mVibrateVA = NULL;
            mVmVibrateVA = (*E)->GetMethodID(E, cVm, "vibrate", "(Landroid/os/CombinedVibration;Landroid/os/VibrationAttributes;)V"); if (!jok("vm vibrate(VA)")) mVmVibrateVA = NULL;
        }
        jclass cAB = (*E)->FindClass(E, "android/media/AudioAttributes$Builder"); if (!jok("FindClass AudioAttributes$Builder")) cAB = NULL;
        if (cAB) {
            jmethodID mNew = (*E)->GetMethodID(E, cAB, "<init>", "()V"), mUse = (*E)->GetMethodID(E, cAB, "setUsage", "(I)Landroid/media/AudioAttributes$Builder;"),
                      mBuild = (*E)->GetMethodID(E, cAB, "build", "()Landroid/media/AudioAttributes;");
            if (jok("AudioAttributes methods") && mNew && mUse && mBuild) {
                jobject b = (*E)->NewObject(E, cAB, mNew); if (b) (*E)->CallObjectMethod(E, b, mUse, (jint)14);   // USAGE_GAME
                jobject aa = b ? (*E)->CallObjectMethod(E, b, mBuild) : NULL; if (aa && jok("AudioAttributes build")) g_aattrs = (*E)->NewGlobalRef(E, aa);
            }
            mVibrateAA = (*E)->GetMethodID(E, cVib, "vibrate", "(Landroid/os/VibrationEffect;Landroid/media/AudioAttributes;)V"); if (!jok("vibrate(AA)")) mVibrateAA = NULL;
        }
        LOG("[haptics] rumble usage: %s", g_vattrs && mVibrateVA ? "VibrationAttributes MEDIA" : g_aattrs && mVibrateAA ? "AudioAttributes GAME" : "none (touch class)");
    }
    // The phone's motor: VibratorManager's default vibrator (Android 12), else the "vibrator" service
    vib_t dev = {0}, pad = {0};
    if (g_phone) {
        jstring name = (*E)->NewStringUTF(E, "vibrator_manager");
        jobject vm = (*E)->CallObjectMethod(E, g_ctx, mSvc, name); jok("getSystemService(vibrator_manager)"); (*E)->DeleteLocalRef(E, name);
        if (vm) { jobject d = (*E)->CallObjectMethod(E, vm, mVmDefault); if (jok("getDefaultVibrator")) vib_take(&dev, d); (*E)->DeleteLocalRef(E, vm); }
        if (!dev.v) { name = (*E)->NewStringUTF(E, "vibrator"); vib_take(&dev, (*E)->CallObjectMethod(E, g_ctx, mSvc, name)); jok("getSystemService(vibrator)"); (*E)->DeleteLocalRef(E, name); }
    }
    LOG("[haptics] phone vibrator: %s%s", dev.v ? "yes" : g_phone ? "none" : "off (DM_RUMBLE_PHONE=0)", dev.v ? dev.amp ? ", with amplitude control" : ", on/off only" : "");
    dm_haptics_route(0, 1);   // rumble has somewhere to go from now on (a controller's motor, or the phone's)
    vib_t *cur = NULL; int cur_amp = 0, cur_lo = 0, cur_hi = 0; int64_t last = 0; dm_haptic_state seen = {0};
    for (;;) {
        pthread_mutex_lock(&g_mu);
        for (;;) {
            int64_t t = mono_now(), due = cur_amp ? last + RENEW_NS : 0;
            if (g_taps || g_dev_new || !same(&seen, &g_state) || (!atomic_load(&g_on) && cur_amp) || (due && t >= due)) break;
            if (!due) { pthread_cond_wait(&g_cv, &g_mu); continue; }
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); int64_t w = due - t; dl.tv_sec += w / 1000000000; dl.tv_nsec += w % 1000000000;
            if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &dl);
        }
        int taps = g_taps, dn = g_dev_new, id = g_dev; dm_haptic_state s = g_state; g_taps = 0; g_dev_new = 0;
        pthread_mutex_unlock(&g_mu);
        int64_t t = mono_now();
        if (dn) {
            if (cur == &pad && cur_amp) { vib_stop(&pad); cur_amp = 0; }
            if (cur == &pad) cur = NULL;
            vib_drop(&pad);
            if (id >= 0) {
                jobject d = (*E)->CallStaticObjectMethod(E, cID, mGetDevice, (jint)id);
                if (jok("getDevice") && d) {
                    jobject vm = (*E)->CallObjectMethod(E, d, mGetVm); if (jok("getVibratorManager")) vm_take(&pad, vm);
                    if (!vib_ok(&pad)) { jobject v = (*E)->CallObjectMethod(E, d, mGetVib); if (jok("getVibrator")) vib_take(&pad, v); }
                    (*E)->DeleteLocalRef(E, d);
                }
            }
            LOG("[haptics] input device %d: rumble on %s%s", id, vib_ok(&pad) ? pad.dual ? "the controller's two motors" : "the controller's motor" : dev.v ? "the phone" : "nothing",
                vib_ok(&pad) ? pad.amp ? " (amplitude control)" : " (on/off)" : "");
        }
        if (taps && dev.v && atomic_load(&g_on)) { vib_tap(&dev); if (cur == &dev && cur_amp) last = t - RENEW_NS + GAP_NS; }   // a running rumble comes back right after the tick
        if (!same(&seen, &s) && t - last < GAP_NS) {   // too soon after the last change: come back for it
            struct timespec d = {0, GAP_NS - (t - last)}; nanosleep(&d, NULL); t = mono_now();
        }
        seen = s;
        vib_t *to = vib_ok(&pad) ? &pad : dev.v ? &dev : NULL;
        int on = atomic_load(&g_on) && to;
        int amp = on ? amp255(dm_haptics_level(s.motor, 2, 1.0f).intensity) : 0, lo = 0, hi = 0;
        if (on && to->dual) { float l, h; dm_haptics_bands(s.motor, 2, &l, &h); lo = amp255(l); hi = amp255(h); }
        if (to && !to->amp && amp) { amp = amp >= 64 ? 255 : 0; lo = lo >= 64 ? 255 : 0; hi = hi >= 64 ? 255 : 0; }   // on/off motors: a strong enough rumble only
        if (to && to->dual) { if (lo < 10) lo = 0; if (hi < 10) hi = 0; if (!lo && !hi) amp = 0; }
        if (amp < 10) { if (cur_amp) { vib_stop(cur); cur_amp = 0; } continue; }
        if (to != cur || abs(amp - cur_amp) >= 12 || abs(lo - cur_lo) >= 12 || abs(hi - cur_hi) >= 12 || t - last >= RENEW_NS) {
            static int first = 1; if (first) { first = 0; LOG("[haptics] first rumble from the game: strength %d/255 (low %d, high %d)", amp, lo, hi); }
            if (cur && cur != to && cur_amp) vib_stop(cur);
            vib_play(to, RUMBLE_MS, lo, hi, amp); cur = to; cur_amp = amp; cur_lo = lo; cur_hi = hi; last = t;
        }
    }
    return NULL;
}
void dm_haptics_android_context(JavaVM *vm, jobject ctx_global) { g_vm = vm; g_ctx = ctx_global; }
void dm_haptics_android_start(void) {
    if (atomic_load(&g_started) || !g_vm || !g_ctx) return;
    const char *e = getenv("DM_RUMBLE");
    if (e && !atoi(e)) { LOG("[haptics] DM_RUMBLE=0: no rumble (the game sees no vibration devices)"); return; }
    e = getenv("DM_RUMBLE_PHONE"); g_phone = !(e && !atoi(e));
    e = getenv("DM_RUMBLE_GAIN"); if (e) { float g = (float)atof(e); g_gain = g >= 0 && g <= 2 ? g : 1; }
    atomic_store(&g_started, 1);
    dm_haptics_init(sink, 1);
    pthread_t t; if (!pthread_create(&t, NULL, haptics_thread, NULL)) pthread_detach(t);
}
#else   // D2R: no game rumble on Android (its hid reports no vibration devices)
void dm_haptics_android_context(JavaVM *vm, jobject ctx_global) { (void)vm; (void)ctx_global; }
void dm_haptics_android_start(void) {}
void dm_haptics_android_tap(void) {}
void dm_haptics_android_device(int id) { (void)id; }
void dm_haptics_android_set(int on) { (void)on; }
void dm_haptics_android_foreground(int fg) { (void)fg; }
#endif
