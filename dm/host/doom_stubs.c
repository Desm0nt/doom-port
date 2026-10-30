// [DOOM] Neutral stand-ins for D2R-only host features (built only with -DDM_GAME=doom, see dm/android/CMakeLists.txt).
// The lean Doom core excludes diag.c, dm_*.c game features (cursor / actions / world / HUD / pad mode / custom settings / i18n
// / text input / update), hle_tact.c, hle_granny.c, hle_charname.c, hle_private_aim.c, hle_target.c and the ios touch-game
// bridge + generated skill-art table. The kept core (hle_hid.c, hle_fs.c, hle_oe.c, hle_os.c, hle_svc.c, hle_swkbd.c,
// hle_thread.c, thread.c, gfx, android/touch_android.c, text_input_jni.c) still references these symbols.
// Every stub is a no-op that reports "nothing there": no UI, not in a run, no panel, no item, no target, no host call.
// None of them touches guest memory.
#ifndef DM_GAME_DOOM
#error "doom_stubs.c is only for DM_GAME_DOOM builds"
#endif
#include "dm.h"
#include "hle.h"
#include "front.h"
#include "dm_actions.h"
#include "dm_cursor.h"
#include "dm_cursor_item.h"
#include "dm_custom.h"
#include "dm_hud.h"
#include "dm_i18n.h"
#include "dm_uiscreen.h"
#include "dm_world.h"
#include "../ios/Sources/dm_touch_native_art.h"
#include <stdatomic.h>

// ---- diag.c: boot profiler, run state, UI-message observation, world freeze ----
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
void dm_prof_mark(const char *seg) { if (g_flag.bootprof) LOG("boot: %s (%.1f ms)", seg, (now_ns() - g_t0_ns) / 1e6); }
int dm_ingame_now(void) { return 0; }
uint64_t dm_server_tick(void) { return UINT64_MAX; }          // not in a run
_Atomic int g_dm_frozen;                                       // never frozen
// No OBS_UIMSG gate exists in Doom's patched modules: subscribers never fire.
void uimsg_subscribe(void (*fn)(const char *cat, const char *act, const char *arg)) { (void)fn; }
int dm_loadscreen_up(void) { return 0; }
void dm_hold_for_loadscreen(void) {}

// ---- hle_tact.c: host calls 0x80+ (cuts). Doom has only the zlib entry gates 0x90..0x94 (hle_zlib.c, hle.h registry) ----
uint64_t svc_host_call(int num, uint64_t *regs) { int dm_zlib_host_call(int, uint64_t *); return (uint64_t)dm_zlib_host_call(num, regs); }

// ---- hle_private_aim.c / dm_padmode.c / dm_runeword.c / dm_textinput.c / dm_update.c pumps and filters ----
void dm_aim_cone_pump(void) {}
int dm_padmode_input_gate(uint64_t *buttons) { (void)buttons; return 0; }
uint64_t dm_padmode_page_filter(uint64_t buttons) { return buttons; }
uint64_t dm_runeword_pad_filter(uint64_t buttons) { return buttons; }
void dm_textinput_pump(void) {}
void dm_update_pump(void) {}
void dm_tp_request(void) {}

// ---- dm_actions.c / dm_namegen.c ----
void dm_actions_pump(void) {}
void dm_belt_expand(int on) { (void)on; }
void dm_belt_put(uint32_t item_id) { (void)item_id; }
int dm_belt_rows(int *expanded) { if (expanded) *expanded = 0; return 0; }
void dm_belt_use(int col, int row) { (void)col; (void)row; }
void dm_held_stow(void) {}
void dm_inv_sort(int page) { (void)page; }
void dm_item_compare(int on) { (void)on; }
int dm_name_gen(char *out, int cap, int (*taken)(const char *)) { (void)taken; if (out && cap > 0) out[0] = 0; return 0; }
void dm_name_random(void) {}
int dm_name_taken(const char *name) { (void)name; return 0; }
void dm_num_set(int n) { (void)n; }
void dm_points_get(dm_points_info *out) { if (out) memset(out, 0, sizeof *out); }
void dm_shop_buy(void) {}
void dm_shop_sell_cursor(void) {}
void dm_skill_add_selected(void) {}
void dm_stash_take(int count) { (void)count; }
void dm_stat_add(int stat, int n) { (void)stat; (void)n; }
uint64_t dm_swapskill_filter(uint64_t buttons) { return buttons; }
int dm_swapskill_slot(int slot, int *charges) { (void)slot; if (charges) *charges = 0; return 0; }
// Test hooks driven by DM_UI_TAPS (hle_hid.c)
void dm_test_belt_first_potion(void) {}
void dm_test_equip(int a) { (void)a; }
void dm_test_hold_first_sellable(void) {}
void dm_test_hover_at(float x, float y) { (void)x; (void)y; }
void dm_test_identify(int a) { (void)a; }
void dm_test_sell_first(void) {}
void dm_test_stash_take(int a) { (void)a; }
void dm_test_swap_held(void) {}
void dm_test_use_first(int a) { (void)a; }

// ---- dm_cursor_item.c ----
int dm_cursor_item(dm_cursor_item_info *out) { if (out) memset(out, 0, sizeof *out); return 0; }
int dm_held_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; return 0; }
int dm_item_targeting(void) { return 0; }

// ---- dm_cursor.c (D2R UI driver) ----
_Atomic int g_fe_name_no_accept;
void dm_cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly) { (void)buttons; (void)lx; (void)ly; }
void dm_cursor_clear(void) {}
int dm_cursor_pending(void) { return 0; }
void dm_cursor_set(int x, int y) { (void)x; (void)y; }
void dm_cursor_set_screen(float x, float y, float screen_w, float screen_h) { (void)x; (void)y; (void)screen_w; (void)screen_h; }
uint64_t dm_ui_alone_take(uint64_t buttons) { (void)buttons; return 0; }
uint64_t dm_ui_auto_buttons(void) { return 0; }
uint64_t dm_ui_auto_release(void) { return 0; }
int dm_ui_bind_active(void) { return 0; }
void dm_ui_bind_begin(uint64_t key_mask) { (void)key_mask; }
int dm_ui_bind_skill_hit(float *x, float *y) { (void)x; (void)y; return 0; }
uint32_t dm_ui_cur_stack_id(void) { return 0; }
void dm_ui_drag_begin(int kind, float ox, float oy, float tol_ny) { (void)kind; (void)ox; (void)oy; (void)tol_ny; }
void dm_ui_drag_end(void) {}
void dm_ui_drag_lift(float ny) { (void)ny; }
void dm_ui_drag_move(float x, float y) { (void)x; (void)y; }
int dm_ui_drop_at_now(float nx, float ny_lifted, float ny_finger) { (void)nx; (void)ny_lifted; (void)ny_finger; return 0; }
int dm_ui_drop_bag(void) { return 0; }
int dm_ui_drop_point(float *nx, float *ny) { (void)nx; (void)ny; return 0; }
int dm_ui_fe_drag_ok(void) { return 0; }
void dm_ui_frontend_pump(void) {}
void dm_ui_gen_bump(int why) { (void)why; }
void dm_ui_hand_idle(int idle) { (void)idle; }
uint32_t dm_ui_hit_id(void) { return 0; }
uint32_t dm_ui_hover_unid(void) { return 0; }
void dm_ui_identify_hovered(void) {}
int dm_ui_item_at_now(float nx, float ny) { (void)nx; (void)ny; return 3; }      // 3 = outside any panel
unsigned dm_ui_item_query(float nx, float ny) { (void)nx; (void)ny; return 0; }
int dm_ui_item_result(unsigned seq) { (void)seq; return 3; }                    // 3 = outside any panel
void dm_ui_item_tap(void) {}
void dm_ui_long_press(void) {}
void dm_ui_map_full(int on) { (void)on; }
void dm_ui_map_hold(int on) { (void)on; }
int dm_ui_map_is_full(void) { return 0; }
int dm_ui_minimap_rect(float r[4]) { (void)r; return 0; }
void dm_ui_mouse_pump(void) {}
int dm_ui_origin_point(float *nx, float *ny) { (void)nx; (void)ny; return 0; }
int dm_ui_panel_state(void) { return 0; }
void dm_ui_ptr(int kind, float nx, float ny, int arg) { (void)kind; (void)nx; (void)ny; (void)arg; }
void dm_ui_safe_left(int units) { (void)units; }
void dm_ui_safe_right(int units) { (void)units; }
void dm_ui_send_queue(const char *spec) { (void)spec; }
void dm_ui_shop_buy_at(float nx, float ny) { (void)nx; (void)ny; }
void dm_ui_stack_cube_at(float nx, float ny) { (void)nx; (void)ny; }
void dm_ui_stack_take_at(float nx, float ny) { (void)nx; (void)ny; }
void dm_ui_tab_go(void) {}
int dm_ui_tap_box(void) { return 0; }
uint64_t dm_ui_taps_base(void) { return 0; }
int dm_ui_targeting(void) { return 0; }
void dm_ui_text_entered(const char *text) { (void)text; }
void dm_ui_touch(int phase, float nx, float ny) { (void)phase; (void)nx; (void)ny; }
int dm_ui_touch_active(void) { return 0; }
void dm_ui_use_item(void) {}
void dm_ui_world_tap(float ux, float uy, float wx, float wy) { (void)ux; (void)uy; (void)wx; (void)wy; }

// ---- dm_world.c ----
void dm_world_pump(void) {}
unsigned dm_world_tap(float nx, float ny) { (void)nx; (void)ny; return 0; }
int dm_world_tap_result(unsigned seq) { (void)seq; return 0; }                 // 0 = no target

// ---- dm_hud.c / dm_cursor.c panel stack ----
void dm_hud_get(dm_hud_info *out) { if (out) memset(out, 0, sizeof *out); }    // valid = 0
void dm_hud_request_players(int n) { (void)n; }
int dm_hud_stack_msg(const char *act, const char *arg) { (void)act; (void)arg; return 0; }

// ---- dm_custom.c / dm_i18n.c ----
int dm_custom_get(const char *key) { (void)key; return 0; }
int dm_custom_lang_explicit(void) { return -1; }
int dm_custom_voice_explicit(void) { return -1; }
int dm_sys_lang(void) { return 0; }

// ---- dm_uiscreen.c: resolution parameter slots = the front end's value as is (no D2R UI-rect adjustment) ----
uint64_t dm_game_res(int docked) { return front_game_res(docked); }
float dm_ui_ny(float ny_real) { return ny_real; }
void dm_uiscreen_enable(int mode) { (void)mode; }
void dm_uiscreen_res_reread(void) {}
void dm_uiscreen_set_panel(int on) { (void)on; }

// ---- ios/Sources dm_touch_game_* bridge + generated dm_touch_native_art.c (D2R skill art) ----
dm_ta_glyph dm_ta_skill(uint32_t raw_skill_id) { dm_ta_glyph g = {0}; g.raw_skill_id = raw_skill_id; g.reason = "doom"; return g; }
dm_ta_glyph dm_ta_current_set(const dm_tg_snapshot *s, uint64_t sdk_button, uint64_t host_now_ns, int skill_set) {
    (void)s; (void)sdk_button; (void)host_now_ns; (void)skill_set; dm_ta_glyph g = {0}; g.reason = "doom"; return g; }
int dm_ta_frame_at(int atlas, int frame, dm_ta_frame *out) { (void)atlas; (void)frame; if (out) memset(out, 0, sizeof *out); return 0; }
int dm_touch_game_snapshot(dm_tg_snapshot *out) { if (out) memset(out, 0, sizeof *out); return 0; }
void dm_touch_game_set_scene(int game_no_panel, uint64_t scene_epoch, uint64_t world_epoch) { (void)game_no_panel; (void)scene_epoch; (void)world_epoch; }
