#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UI线程仅排队；step必须挂在dm_pad_hook游戏采样线程。 | UI only queues; step must run on the guest sampling thread through dm_pad_hook. */
void dm_android_touch_resize(float width_dp, float height_dp, float left_dp, float right_dp, float bottom_dp);
void dm_android_touch_tablet(int tablet);
/* phase: 0 down, 1 move, 2 up, 3 cancel; slot: stable 0..15; coordinates in dp. | 阶段：按下/移动/抬起/取消；槽固定0..15；坐标dp。 */
void dm_android_touch_event(int phase, int slot, float x, float y);
void dm_android_touch_focus(int active);
void dm_android_touch_uimsg(const char *cat, const char *act, const char *arg);
void dm_android_touch_step(uint64_t guest_now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
/* 绘图快照：header[width,height,navigation,skill_set]，23个[x,y,r,enabled,pressed]；12键、摇杆、换组、难度及8选择格。 | Drawing snapshot: header followed by 23 records (12 keys, stick, set switch, difficulty, 8 choices). */
/* 后接六个技能图标记录（状态、图集、裁切矩形、副手/充能、次数）和16个HUD数值。
   Append six icon records (status, atlas, crop, secondary/charge flags, charges) and 16 HUD values. */
enum { DM_ANDROID_TOUCH_GLYPHS = 4 + 23 * 5, DM_ANDROID_TOUCH_HUD = DM_ANDROID_TOUCH_GLYPHS + 6 * 8,
       DM_ANDROID_TOUCH_DRAW_FLOATS = DM_ANDROID_TOUCH_HUD + 18 };
void dm_android_touch_layout(float out[DM_ANDROID_TOUCH_DRAW_FLOATS]);

/* Android platform services beside touch (front_android.c, haptics_android.c, perf_android.c). */
#include <jni.h>
/* Gamepad (DmActivity -> jni_android.c): Android key codes / axes of a controller; returns 1 when the key is the game's. */
int front_android_pad_key(int device, int keycode, int down);
void front_android_pad_axes(int device, const float axes[8]);   /* x y z rz ltrigger rtrigger hat_x hat_y */
void front_android_pad_removed(int device);
void front_android_set_rotation(int rotation);                   /* Display.getRotation(): 0..3 */
void front_android_sensors(int on);                              /* app in front: gyro / accelerometer sampling for the six-axis sensor */
/* Rumble: the application Context (global reference) before the host starts; start after the env file is loaded. */
void dm_haptics_android_context(JavaVM *vm, jobject ctx_global);
void dm_haptics_android_start(void);
void dm_haptics_android_tap(void);
void dm_haptics_android_device(int id);   /* the controller in use (InputDevice id), -1 = the phone */
void dm_haptics_android_set(int on);
void dm_haptics_android_foreground(int fg);
/* Performance (perf_android.c): ADPF hint session, thermal status, display rate vote. dm_aperf_present is the per-present hook
 * (vulkan_present.cpp present(), on the presenting thread, right before vkQueuePresentKHR). */
struct ANativeWindow;
void dm_aperf_present(struct ANativeWindow *window, float marker_vote_hz);
void dm_aperf_start(void);
int dm_thermal_now(void);
#ifdef __cplusplus
}
#endif
