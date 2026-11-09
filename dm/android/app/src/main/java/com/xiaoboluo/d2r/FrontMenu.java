package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// [front] The unified pause menu and what hangs off it (frontend-common, docs/FRONTEND_UNIFIED.md; the same file in every port). The port's DmActivity
// implements Host (and Owner, so the pad's gear button finds this), builds one FrontMenu after its pad and calls back() / key() / motion() from its
// input handlers. The menu holds the game "as the Switch does": FrontMenu.Host.hold -> nativeHold (the guest clock frozen, guest clock reads wait,
// graphics and audio suspended, see the port's hle_hold.c / nx2android's hold_sync), no input reaches the game, and on Resume everything goes on
// without a jump. The rows:
//   Resume · Game settings… (the port's own gear dialog) · Gamepad… (skin, opacity, size, layout, editor) · Edit layout · Mode (docked / handheld, when
//   the game has one) · Export save · Import save… · Check for updates (when the port has an update check) · Quit
final class FrontMenu implements PauseMenu.Listener {
    interface Owner { FrontMenu frontMenu(); }
    interface Host {
        String gameName();
        boolean ready();                 // the host runs (the game can be held)
        void hold(boolean on);           // nativeHold
        void gameSettings();             // the port's own settings dialog (shown over the menu; the game stays held)
        int modeKind();                  // MODE_*
        boolean docked();
        boolean setDocked(boolean on);   // false: not switched
        Saves saves();                   // null: no save transfer in this port
        void quit();
        void immersive();
        default boolean updates() { return false; }   // the port has an update check (UpdateNotice): the "Check for updates" row
        default void checkUpdates() { }
    }
    static final int MODE_NONE = 0, MODE_OPMODE = 1, MODE_LIGHT = 2;   // OPMODE: Switch dock / handheld (dm_set_opmode); LIGHT: DOOM's handheld quality preset
    interface Saves { String export(); String[] list(); String requestImport(String name); String where(); }
    interface Pad {
        void releaseAll(); void startEdit(Runnable done); boolean editing(); void endEdit(boolean save); void reloadSettings(); void resetLayout();
        String prefsName();
        String[] sliderKeys();           // {opacity key, size key} in prefsName(), null entries when the pad has none
        int[] sliderDefaults();          // their defaults (%)
        String[] presets();              // layout presets ("game", "switch"...), null when one
        View view();
    }

    private final Activity act; private final Host host; private final Pad pad; private final PauseMenu menu;
    private boolean open, dialog;
    private int[] ids = new int[0];
    static final int R_RESUME = 0, R_SETTINGS = 1, R_PAD = 2, R_EDIT = 3, R_MODE = 4, R_EXPORT = 5, R_IMPORT = 6, R_QUIT = 7, R_UPDATES = 8;

    FrontMenu(Activity a, Host h, Pad p, FrameLayout frame) {
        act = a; host = h; pad = p;
        menu = new PauseMenu(a, this); menu.setVisibility(View.GONE);
        frame.addView(menu, new FrameLayout.LayoutParams(-1, -1));
    }
    boolean isOpen() { return open; }
    private String t(String ru, String en) { return Ui.t(act, ru, en); }

    private void rebuild() {
        int[] all = {R_RESUME, R_SETTINGS, R_PAD, R_EDIT, R_MODE, R_EXPORT, R_IMPORT, R_UPDATES, R_QUIT};
        int n = 0; int[] use = new int[all.length];
        for (int r : all) { if (r == R_MODE && host.modeKind() == MODE_NONE) continue; if ((r == R_EXPORT || r == R_IMPORT) && host.saves() == null) continue; if (r == R_EDIT && pad == null) continue; if (r == R_UPDATES && !host.updates()) continue; use[n++] = r; }
        ids = java.util.Arrays.copyOf(use, n);
        String[] labels = new String[n]; int danger = -1;
        for (int i = 0; i < n; i++) { labels[i] = label(ids[i]); if (ids[i] == R_QUIT) danger = i; }
        menu.setRows(t("Пауза", "Paused"), host.gameName(), labels, danger);
    }
    private String label(int r) {
        switch (r) {
            case R_RESUME: return t("Продолжить", "Resume");
            case R_SETTINGS: return t("Настройки игры…", "Game settings…");
            case R_PAD: return t("Экранный геймпад…", "On-screen gamepad…");
            case R_EDIT: return t("Изменить раскладку", "Edit the layout");
            case R_MODE: { boolean d = host.docked();
                if (host.modeKind() == MODE_LIGHT) return d ? t("Режим: док (полное качество)", "Mode: docked (full quality)") : t("Режим: портативный (лёгкий)", "Mode: handheld (light)");
                return d ? t("Режим Switch: док (TV)", "Switch mode: docked (TV)") : t("Режим Switch: портативный", "Switch mode: handheld"); }
            case R_EXPORT: return t("Экспорт сохранения", "Export save");
            case R_IMPORT: return t("Импорт сохранения…", "Import save…");
            case R_UPDATES: return t("Проверить обновления", "Check for updates");
            default: return t("Выйти из игры", "Quit game");
        }
    }

    // ---- open / close
    void open() {
        if (open || !host.ready()) return;
        android.util.Log.i("dm", "[java] menu open");
        open = true; if (pad != null) pad.releaseAll(); host.hold(true);
        rebuild(); menu.open(); menu.requestFocus();
    }
    void close() {
        if (!open) return;
        android.util.Log.i("dm", "[java] menu close");
        open = false; menu.setVisibility(View.GONE);
        if (!dialog) host.hold(false);
        host.immersive();
    }
    @Override public void onResume() { close(); }
    @Override public void onPick(int row) {
        int r = row >= 0 && row < ids.length ? ids[row] : R_RESUME;
        switch (r) {
            case R_RESUME: close(); break;
            case R_SETTINGS: host.gameSettings(); break;
            case R_PAD: padSettings(); break;
            case R_EDIT: edit(); break;
            case R_MODE: { boolean to = !host.docked();
                if (host.setDocked(to)) menu.setNote(t("Игра переключит режим, когда продолжите", "The game switches when it goes on"));
                else menu.setNote(t("Режим не переключился", "The mode did not switch"));
                rebuild(); break; }
            case R_EXPORT: { Saves s = host.saves(); menu.setNote(t("Экспорт…", "Exporting…"));
                new Thread(() -> { String res = s.export(); act.runOnUiThread(() -> menu.setNote(res.startsWith("!")
                    ? t("Не вышло: ", "Failed: ") + (res.equals("!no save yet") ? t("сохранения ещё нет", "no save yet") : res.substring(1))
                    : t("Сохранено: saves/", "Saved: saves/") + res)); }, "front-export").start(); break; }
            case R_IMPORT: importSave(); break;
            case R_UPDATES: host.checkUpdates(); break;
            case R_QUIT:
                dialog(new AlertDialog.Builder(act).setMessage(t("Выйти из игры? Несохранённый прогресс пропадёт.", "Quit the game? Unsaved progress is lost."))
                    .setPositiveButton(t("Выйти", "Quit"), (d, w) -> host.quit()).setNegativeButton(t("Отмена", "Cancel"), null).create());
                break;
        }
    }
    // a dialog over the menu: the game stays held until both are gone
    private void dialog(AlertDialog d) {
        dialog = true;
        d.setOnDismissListener(x -> { dialog = false; lastBack = SystemClock.uptimeMillis(); if (!open) host.hold(false); host.immersive(); menu.invalidate(); });
        d.show();
    }

    // ---- the layout editor (the game stays held while editing)
    void edit() {   // also from a port's own gear dialog (DOOM 2016: "Edit the layout…")
        if (pad == null) return;
        menu.setVisibility(View.GONE);
        pad.startEdit(() -> { if (open) { menu.setVisibility(View.VISIBLE); rebuild(); menu.requestFocus(); } });
    }

    // ---- Back (the key, the gesture, a controller's Home / Menu): the editor cancels, the menu closes, else it opens. One action per press: a second call
    // within 400 ms (the same press seen twice: onBackPressed and the OnBackInvokedCallback) does nothing
    private long lastBack;
    void back() {
        long now = SystemClock.uptimeMillis(); if (now - lastBack < 400) return; lastBack = now;
        if (dialog) return;
        android.util.Log.i("dm", "[java] back: " + (pad != null && pad.editing() ? "editor cancelled" : open ? "menu closed" : "menu opened"));
        if (pad != null && pad.editing()) pad.endEdit(false); else if (open) close(); else open();
    }
    // a key event from DmActivity.dispatchKeyEvent before the game sees it; true when taken
    boolean key(KeyEvent e) {
        int k = e.getKeyCode(); boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (!host.ready()) return false;
        if (k == KeyEvent.KEYCODE_BUTTON_MODE || k == KeyEvent.KEYCODE_MENU || (k == KeyEvent.KEYCODE_BACK && android.os.Build.VERSION.SDK_INT < 33)) { if (down && e.getRepeatCount() == 0) back(); return true; }
        if (open && !dialog && menu.getVisibility() == View.VISIBLE && down && menu.key(k)) return true;
        if (!(open || (pad != null && pad.editing()))) return false;
        switch (k) {   // nothing reaches the game while it is held; the volume keys go to the system, Back to the OnBackInvokedCallback (Android 13+)
            case KeyEvent.KEYCODE_VOLUME_UP: case KeyEvent.KEYCODE_VOLUME_DOWN: case KeyEvent.KEYCODE_VOLUME_MUTE: case KeyEvent.KEYCODE_BACK: return false;
            default: return true;
        }
    }
    private long lastHat;
    boolean motion(MotionEvent e) {
        if (!(open || (pad != null && pad.editing()))) return false;
        if (open && !dialog && menu.getVisibility() == View.VISIBLE && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK && e.getAction() == MotionEvent.ACTION_MOVE) {
            float hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y), sy = e.getAxisValue(MotionEvent.AXIS_Y); long now = SystemClock.uptimeMillis();
            float v = Math.abs(hy) > 0.5f ? hy : Math.abs(sy) > 0.7f ? sy : 0;
            if (v != 0 && now - lastHat > 180) { lastHat = now; menu.key(v < 0 ? KeyEvent.KEYCODE_DPAD_UP : KeyEvent.KEYCODE_DPAD_DOWN); }
        }
        return true;
    }
    static boolean isController(InputDevice d, int source) {
        if (d == null || d.isVirtual()) return false;
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    // ---- import: pick an export, confirm, ask for it at the next start (SaveTransfer.applyPending / nx2android dm_saves_request_import) and close the app
    private void importSave() {
        Saves s = host.saves(); String[] saves = s.list();
        if (saves.length == 0) { menu.setNote(t("Нет экспортов в ", "No exports in ") + s.where()); return; }
        String[] labels = new String[saves.length];
        for (int i = 0; i < saves.length; i++) { String[] f = saves[i].split("\t"); labels[i] = f[0] + (f.length > 2 ? String.format(Locale.ROOT, "  (%s %s, %.1f MB)", f[1], t("файл.", "files"), Long.parseLong(f[2]) / 1048576.0) : ""); }
        dialog(new AlertDialog.Builder(act).setTitle(t("Импорт сохранения", "Import a save")).setItems(labels, (d, w) -> {
            String name = saves[w].split("\t")[0];
            dialog(new AlertDialog.Builder(act).setMessage(t("Импортировать " + name + "?\nТекущее сохранение сначала экспортируется (…-before-import), игра закроется — запустите её снова.",
                "Import " + name + "?\nThe current save is exported first (…-before-import), then the game closes: start it again."))
                .setPositiveButton(t("Импорт", "Import"), (d2, w2) -> {
                    String err = s.requestImport(name);
                    if (err != null) { menu.setNote(t("Не вышло: ", "Failed: ") + err); return; }
                    Toast.makeText(act, t("Игра закрывается для импорта — запустите её снова", "Closing for the import: start the game again"), Toast.LENGTH_LONG).show();
                    menu.postDelayed(host::quit, 1500); })
                .setNegativeButton(t("Отмена", "Cancel"), null).create());
        }).setNegativeButton(t("Отмена", "Cancel"), null).create());
    }

    // ---- the gamepad dialog: skin, opacity, size, layout preset, look by swiping, editor, reset (all live)
    private void padSettings() {
        if (pad == null) return;
        SharedPreferences cp = act.getSharedPreferences(pad.prefsName(), Activity.MODE_PRIVATE);
        LinearLayout box = new LinearLayout(act); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(56, 16, 56, 16);
        Map<String, String> packs = PadTheme.packs(act.getAssets());
        List<String> keys = PadSkin.packKeys(act, packs);
        String[] skinLabels = new String[keys.size()]; int skinAt = 0;
        String cur = cp.getString("skin", padDefaultSkin());
        for (int i = 0; i < keys.size(); i++) { skinLabels[i] = PadSkin.label(act, packs, keys.get(i)); if (keys.get(i).equals(cur)) skinAt = i; }
        TextView skinT = new TextView(act); skinT.setTextSize(17); skinT.setPadding(0, 16, 0, 8); skinT.setTextColor(PauseMenu.ACCENT);
        skinT.setText(t("Скин: ", "Skin: ") + skinLabels[skinAt] + "  ▸"); box.addView(skinT);
        final int[] pick = {skinAt};
        skinT.setOnClickListener(v -> new AlertDialog.Builder(act).setTitle(t("Скин геймпада", "Gamepad skin")).setSingleChoiceItems(skinLabels, pick[0], (d, w) -> {
            pick[0] = w; cp.edit().putString("skin", keys.get(w)).apply(); pad.reloadSettings(); skinT.setText(t("Скин: ", "Skin: ") + skinLabels[w] + "  ▸"); d.dismiss(); }).show());
        note(box, t(keys.size() - 1 + " скинов из nx2android (Themed Virtual Controllers, MIT)", keys.size() - 1 + " skins from nx2android (Themed Virtual Controllers, MIT)"));
        String[] sk = pad.sliderKeys(); int[] sd = pad.sliderDefaults();
        if (sk[0] != null) slider(box, cp, t("Прозрачность контролов", "Controls' opacity"), sk[0], 5, 100, sd[0]);
        if (sk[1] != null) slider(box, cp, t("Размер контролов", "Controls' size"), sk[1], 60, 160, sd[1]);
        String[] pr = pad.presets();
        if (pr != null && pr.length > 1) {
            TextView h = new TextView(act); h.setText(t("Раскладка:", "Layout:")); h.setPadding(0, 16, 0, 0); box.addView(h);
            RadioGroup g = new RadioGroup(act); g.setOrientation(RadioGroup.VERTICAL); String was = cp.getString("preset", pr[0]);
            for (int i = 0; i < pr.length; i++) { RadioButton b = new RadioButton(act); b.setId(6100 + i); b.setText(presetLabel(pr[i])); g.addView(b); if (pr[i].equals(was)) g.check(6100 + i); }
            if (g.getCheckedRadioButtonId() == -1) g.check(6100);
            g.setOnCheckedChangeListener((gg, id) -> { cp.edit().putString("preset", pr[Math.max(0, Math.min(pr.length - 1, id - 6100))]).apply(); pad.reloadSettings(); });
            box.addView(g);
        }
        if (cp.contains("look_on") || pad instanceof DoomPadView) {
            CheckBox look = new CheckBox(act); look.setText(t("Камера свайпом по свободному месту экрана", "Turn the camera by swiping a free spot")); look.setChecked(cp.getBoolean("look_on", true));
            look.setOnCheckedChangeListener((c, on) -> { cp.edit().putBoolean("look_on", on).apply(); pad.reloadSettings(); }); box.addView(look);
        }
        // [issue #40] the floating move stick and its zone, only for a port whose pad offers it (DOOM 2016)
        if (pad instanceof DoomPadView && ((DoomPadView) pad).floatingOffered()) {
            CheckBox fl = new CheckBox(act); fl.setText(t("Плавающий стик движения: появляется под пальцем в левой части экрана", "Floating move stick: appears under the finger on the left part of the screen"));
            fl.setChecked(cp.getBoolean("float_stick", ((DoomPadView) pad).floatingDefault()));
            fl.setOnCheckedChangeListener((c, on) -> { cp.edit().putBoolean("float_stick", on).apply(); pad.reloadSettings(); }); box.addView(fl);
            slider(box, cp, t("Зона плавающего стика (доля ширины экрана)", "Floating stick zone (share of the screen width)"), "float_zone", DoomPadView.FLOAT_ZONE_MIN, DoomPadView.FLOAT_ZONE_MAX, DoomPadView.FLOAT_ZONE_DEFAULT);
        }
        CheckBox shown = new CheckBox(act); shown.setText(t("Показывать геймпад (кнопка меню видна всегда)", "Show the gamepad (the menu button always shows)")); shown.setChecked(cp.getBoolean("shown", true));
        shown.setOnCheckedChangeListener((c, on) -> { cp.edit().putBoolean("shown", on).apply(); pad.reloadSettings(); }); box.addView(shown);
        android.widget.Button edit = new android.widget.Button(act); edit.setText(t("Изменить раскладку…", "Edit the layout…")); box.addView(edit);
        android.widget.Button reset = new android.widget.Button(act); reset.setText(t("Сбросить раскладку", "Reset the layout")); box.addView(reset);
        reset.setOnClickListener(v -> { pad.resetLayout(); Toast.makeText(act, t("Раскладка сброшена", "Layout reset"), Toast.LENGTH_SHORT).show(); });
        TextView lang = new TextView(act); lang.setPadding(0, 24, 0, 0); lang.setText(t("Язык меню: ", "Menu language: ")); box.addView(lang);
        RadioGroup lg = new RadioGroup(act); lg.setOrientation(RadioGroup.HORIZONTAL);
        String[] ln = {t("Как в системе", "System"), "Русский", "English"}, lc = {"", "ru", "en"}; String lw = act.getSharedPreferences(Ui.PREFS, Activity.MODE_PRIVATE).getString("ui_lang", "");
        for (int i = 0; i < 3; i++) { RadioButton b = new RadioButton(act); b.setId(6200 + i); b.setText(ln[i]); lg.addView(b); if (lc[i].equals(lw)) lg.check(6200 + i); }
        lg.setOnCheckedChangeListener((gg, id) -> { act.getSharedPreferences(Ui.PREFS, Activity.MODE_PRIVATE).edit().putString("ui_lang", lc[Math.max(0, Math.min(2, id - 6200))]).apply(); rebuild(); });
        box.addView(lg);
        ScrollView scroll = new ScrollView(act); scroll.addView(box);
        AlertDialog d = new AlertDialog.Builder(act).setTitle(t("Экранный геймпад", "On-screen gamepad")).setView(scroll).setPositiveButton(t("Готово", "Done"), null).create();
        edit.setOnClickListener(v -> { d.dismiss(); edit(); });
        dialog(d);
    }
    private String presetLabel(String p) {
        switch (p) {
            case "game": return t("Раскладка игры (как была)", "The game's own layout (as before)");
            case "switch": return t("Switch: два стика, раскладка nx2android", "Switch: two sticks, nx2android's layout");
            default: return p;
        }
    }
    private String padDefaultSkin() { return pad instanceof DoomPadView ? ((DoomPadView) pad).defaultSkin() : PadSkin.CLASSIC; }
    private void note(LinearLayout box, String s) { TextView t = new TextView(act); t.setText(s); t.setAlpha(0.7f); box.addView(t); }
    private void slider(LinearLayout box, SharedPreferences cp, String label, String key, int min, int max, int def) {
        int was = Math.max(min, Math.min(max, cp.getInt(key, def)));
        TextView t = new TextView(act); t.setText(label + ": " + was + "%"); t.setPadding(0, 12, 0, 0); box.addView(t);
        SeekBar s = new SeekBar(act); s.setMax(max - min); s.setProgress(was - min); box.addView(s);
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar sb, int v, boolean user) { t.setText(label + ": " + (v + min) + "%"); cp.edit().putInt(key, v + min).apply(); pad.reloadSettings(); }
            public void onStartTrackingTouch(SeekBar sb) {} public void onStopTrackingTouch(SeekBar sb) {}
        });
    }
}
