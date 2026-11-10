package com.xiaoboluo.d2r;

import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.TextView;
import java.util.ArrayList;

// [front] The generic sections of the gear dialog (GearSettings), each one DOOM 2016's code moved out of its DmActivity.showDoomSettings /
// applyDoomSettings (same prefs, same environment, same texts), plus the generic parts other ports had (Switch mode, picture shape, menu
// language: XC2 / Diablo III / XC3). A port lists the ones it wants in GearHost.gearSections, in its order, between its own sections:
//   fpsLimit       frame rate limit 30 / 60 / 90 / 120 (GearDisplay; DM_PRESENT_MAX_HZ, + mirrors like DOOM's DM_DOOM_TICK_MAX)   next launch
//   renderScale    3D resolution as a fraction (pref "res"; DOOM / Eternal: DM_DOOM_RES)                                          next launch
//   sharpen        FSR RCAS sharpening (DM_GFX_SHARPEN, vulkan_present.cpp)                                                       next launch
//   upscale        GSR upscale to the screen (DM_GFX_UPSCALE=gsr; only DOOM 2016's vulkan_present.cpp has it so far)              next launch
//   driver         Vulkan driver list + what runs now + "Manage drivers…" (VkDrivers, DriverManager; DM_VK_DRIVER)                 next launch
//   safeMode       [safe-mode] safe graphics mode off / auto / on on Mali / PowerVR (pref "safe_mode", DM_SAFE_MODE; SafeMode.java)  next launch
//   gameLanguage   the game's language (DM_LANG -> hle_oe.c GetDesiredLanguage); the codes / names are the game's                  next launch
//   rumble         vibration (DM_RUMBLE, haptics_android.c; off by default)                                                       next launch
//   opmode         Switch mode docked / handheld (pref "front_opmode", DM_OPMODE=0; live through FrontMenu.Host.setDocked)         both
//   pictureShape   16:9 with bars / stretched / the game's widescreen (pref "aspect"; GearDisplay.fitSurface + the port's env)      both
//   look           look sensitivity (pad "look")                                                                                   immediately
//   deadzone       move stick dead-zone compensation (pad "move_dz")                                                               immediately
//   fpsCounter     the FPS / frame time counter (pad "fps")                                                                        immediately
//   gearButton     the menu (gear) button: normal / faded + the faded opacity slider (pad "gear_mode", "gear_alpha")               immediately
//   padControls    the on-screen controls' opacity and size (live sliders), "Edit the layout…", "Reset the layout"                 immediately
//   menuLanguage   the menu language (front ui_lang: this dialog, the pause menu, the pad dialogs; reopens in it)                   immediately
final class GearPages {
    private GearPages() {}
    // an environment step that may fail like Os.setenv
    interface Env { void set(GearSettings g, int value) throws ErrnoException; }

    // ---- frame rate limit: only the values this display can show evenly; above 60 the note "experimental"
    static GearSettings.Section fpsLimit(int[] limits, int def, String... mirrorEnv) {
        return new GearSettings.Section() {
            final ArrayList<Integer> lims = new ArrayList<>(); RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                for (int l : limits) if (GearDisplay.fpsLimitAvailable(s.a, l)) lims.add(l);
                String[] labels = new String[lims.size()]; int at = 0, cur = GearDisplay.fpsLimit(s.a, s.gamePrefs(), limits, def);
                for (int i = 0; i < labels.length; i++) { labels[i] = String.valueOf(lims.get(i)); if (lims.get(i) == cur) at = i; }
                s.heading(box, s.whenKey("gear_fps_limit", false));
                g = s.radios(box, labels, at, true);
                if (!lims.isEmpty() && lims.get(lims.size() - 1) > 60) s.text(box, s.s("gear_fps_note"));
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { if (!lims.isEmpty()) game.putInt("fps_limit", lims.get(GearSettings.picked(g))); }
            public void env(GearSettings s) throws ErrnoException {
                int lim = GearDisplay.fpsLimit(s.a, s.gamePrefs(), limits, def);
                Os.setenv("DM_PRESENT_MAX_HZ", String.valueOf(lim), true);
                for (String e : mirrorEnv) Os.setenv(e, String.valueOf(lim), true);
            }
        };
    }
    static GearSettings.Section fpsLimit(String... mirrorEnv) { return fpsLimit(GearDisplay.FPS_LIMITS, GearDisplay.FPS_LIMIT_DEFAULT, mirrorEnv); }

    // ---- 3D resolution: pref "res" = an index into fractions (0 = full: the variable unset); note = a line under it (null: none)
    static GearSettings.Section renderScale(float[] fractions, String[] labels, String envName, String note) {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.whenKey("gear_res", false));
                g = s.radios(box, labels, s.gamePrefs().getInt("res", 0), false);
                if (note != null) s.text(box, note);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt("res", GearSettings.picked(g)); }
            public void env(GearSettings s) throws ErrnoException {
                int res = s.gamePrefs().getInt("res", 0);
                if (res > 0 && res < fractions.length) Os.setenv(envName, String.valueOf(fractions[res]), true); else Os.unsetenv(envName);
            }
        };
    }

    // ---- sharpening (FSR RCAS in vulkan_present.cpp): off / low / medium / high = DM_GFX_SHARPEN unset / 1.0 / 0.5 / 0.2
    static final String[] SHARPEN_STOPS = {"", "1.0", "0.5", "0.2"};
    static GearSettings.Section sharpen() {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.whenKey("gear_sharpen", false));
                g = s.radios(box, s.keys("gear_off", "gear_low", "gear_medium", "gear_high"), s.gamePrefs().getInt("sharpen", 0), true);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt("sharpen", GearSettings.picked(g)); }
            public void env(GearSettings s) throws ErrnoException {
                int sh = s.gamePrefs().getInt("sharpen", 0);
                if (sh > 0 && sh < SHARPEN_STOPS.length) Os.setenv("DM_GFX_SHARPEN", SHARPEN_STOPS[sh], true); else Os.unsetenv("DM_GFX_SHARPEN");
            }
        };
    }
    // ---- upscale to the screen resolution (vulkan_present.cpp DM_GFX_UPSCALE): the swapchain at the window size + a GSR pass; off by default
    static GearSettings.Section upscale() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings s, LinearLayout box) { c = s.check(box, s.whenKey("gear_upscale", false), s.gamePrefs().getBoolean("upscale", false)); }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("upscale", c.isChecked()); }
            public void env(GearSettings s) throws ErrnoException {
                if (s.gamePrefs().getBoolean("upscale", false)) Os.setenv("DM_GFX_UPSCALE", "gsr", true); else Os.unsetenv("DM_GFX_UPSCALE");
            }
        };
    }

    // ---- [safe-mode] safe graphics mode (DOOM BUGS_AND_FIXES 2.57, SafeMode.java): off = the 0.5.x speed-ups always (DM_SAFE_MODE=0), auto
    // (default) = steps after graphics crashes (unset), on = always at least step 1 (DM_SAFE_MODE=on: the speed-ups off by hand, for picture
    // problems no crash reveals). Shown only on Mali / PowerVR (simulated included) and only for a port with GearHost.safeMode; the row says
    // the step of the running game. A changed choice starts the crash count again.
    static GearSettings.Section safeMode(GearHost h) {
        return new GearSettings.Section() {
            RadioGroup g; int was = SafeMode.AUTO;
            public void build(GearSettings s, LinearLayout box) {
                SafeMode sm = h.safeMode(); if (sm == null || !sm.eligible()) return;
                s.heading(box, s.whenKey("gear_safe_mode", false));
                was = s.gamePrefs().getInt("safe_mode", SafeMode.AUTO);
                g = s.radios(box, s.keys("gear_safe_off", "gear_safe_auto", "gear_safe_on"), was, true);
                String now = sm.nowLine(); if (now != null) s.text(box, now);
                s.text(box, s.s("gear_safe_note"));
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) {
                if (g == null) return;
                int p = GearSettings.picked(g); game.putInt("safe_mode", p);
                SafeMode sm = h.safeMode(); if (p != was && sm != null) sm.choiceChanged(was, p);
                was = p;
            }
            public void env(GearSettings s) throws ErrnoException { if (h.safeMode() != null) SafeMode.env(s.gamePrefs()); }
        };
    }

    // ---- the Vulkan driver: the system one, or any adrenotools package under files/drivers/<name>/ (bundled, downloaded, installed in the
    // manager or imported from the data folder's drivers/), labelled with the name from its meta.json; below it, what the running game
    // actually uses. Packages that cannot run here (VkDrivers.problem: another GPU family, a glibc build, a missing library, minApi) are not
    // offered; a line lists them with the reason. The list is rebuilt when the driver manager closes (a package installed or deleted there
    // shows up at once), keeping the choice when it still exists. env = VkDrivers.prepare + choose (the crash guard, DM_VK_DRIVER).
    static GearSettings.Section driver(VkDrivers d) {
        return new GearSettings.Section() {
            LinearLayout drvBox; String[] names = new String[0]; RadioGroup g;
            void fill(GearSettings s, String want) {
                drvBox.removeAllViews();
                ArrayList<String> offered = new ArrayList<>(); StringBuilder hiddenWhy = new StringBuilder();
                for (String n : d.packages()) { String why = d.problem(n); if (why == null) offered.add(n); else hiddenWhy.append("\n- ").append(d.label(n)).append(": ").append(d.reasonText(why)); }
                names = offered.toArray(new String[0]);
                if (hiddenWhy.length() > 0) s.text(drvBox, s.t("Скрыты (не подходят этому телефону, " + VkDrivers.hardwareSummary() + "):",
                    "Hidden (cannot run on this phone, " + VkDrivers.hardwareSummary() + "):") + hiddenWhy);
                String[] labels = new String[names.length + 1]; labels[0] = s.s("gear_driver_system");
                for (int i = 0; i < names.length; i++) labels[i + 1] = d.label(names[i]);
                String cur = want != null && (want.isEmpty() || offered.contains(want)) ? want : s.gamePrefs().getString("driver", d.defaultDriver());
                int at = 0; for (int i = 0; i < names.length; i++) if (names[i].equals(cur)) at = i + 1;
                g = s.radios(drvBox, labels, at, false);
            }
            String picked() { int k = GearSettings.picked(g); return k > 0 && k <= names.length ? names[k - 1] : ""; }
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.whenKey("gear_driver", false));
                drvBox = s.vbox(box); fill(s, null);
                String[] st = d.status();
                if (st != null) {
                    boolean fell = !st[0].isEmpty() && "system".equals(st[1]);
                    s.text(box, fell ? s.s("gear_driver_active_fallback", d.label(st[0]))
                                     : s.s("gear_driver_active", st[0].isEmpty() || "system".equals(st[1]) ? s.s("gear_driver_system_short") : d.label(st[0])));
                    // (2.25) why it fell back, or what the GPU reports for the custom driver (vk_driver.c verified it)
                    if (fell || (st.length > 3 && !st[0].isEmpty())) s.text(box, (fell ? d.reasonText(st[2]) + (st.length > 3 ? "; " : "") : "") + (st.length > 3 ? st[3] : ""));
                }
                // (2.39) the driver manager (installed packages, install from a file, download from GitHub releases: DriverManager.java)
                Button mgr = s.button(box, s.t("Драйверы…", "Manage drivers…"));
                mgr.setOnClickListener(v -> new DriverManager(d, () -> fill(s, picked())).show());
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putString("driver", picked()); }
            public void env(GearSettings s) throws ErrnoException { d.prepare(); d.choose(); }
        };
    }

    // ---- the game's language (text and voice): codes[0] = "" = as in the system; labels[0] null = gear_lang_system; DM_LANG only when picked
    // (jni sets it from the phone's locale otherwise)
    static GearSettings.Section gameLanguage(String[] codes, String[] labels, boolean horizontal) {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.whenKey("gear_lang", false));
                String[] l = labels.clone(); if (l[0] == null) l[0] = s.s("gear_lang_system");
                g = s.radios(box, l, GearSettings.indexOf(codes, s.gamePrefs().getString("lang", "")), horizontal);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putString("lang", codes[GearSettings.picked(g)]); }
            public void env(GearSettings s) throws ErrnoException { String lang = s.gamePrefs().getString("lang", ""); if (!lang.isEmpty()) Os.setenv("DM_LANG", lang, true); }
        };
    }
    // ---- vibration (haptics_android.c): off by default (DOOM 28.10: it only drains the battery); DM_RUMBLE=0 hides the vibration devices
    static GearSettings.Section rumble() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings s, LinearLayout box) { c = s.check(box, s.whenKey("gear_rumble", false), s.gamePrefs().getBoolean("rumble", false)); }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("rumble", c.isChecked()); }
            public void env(GearSettings s) throws ErrnoException { Os.setenv("DM_RUMBLE", s.gamePrefs().getBoolean("rumble", false) ? "1" : "0", true); }
        };
    }

    // ---- the Switch operation mode (Eternal / XC2 / D3 / XC3): docked (the default) / handheld, live through the port's FrontMenu.Host.setDocked
    // (nativeSetOpmode -> hle_oe.c dm_set_opmode: the game gets notifications 30 / 31, as a Switch put into / taken out of its dock); a handheld
    // pick is kept ("front_opmode" 0) and set as DM_OPMODE=0 at the next start. Labels: null = "Docked (TV)" / "Handheld". The pause menu's
    // "Mode" row is the same setting. (DOOM 2016's mode is its light mode instead: its own section.)
    static GearSettings.Section opmode(FrontMenu.Host fh, String docked, String handheld) {
        return new GearSettings.Section() {
            RadioGroup g; boolean was;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.when(s.t("Режим Switch", "Switch mode"), true));
                was = fh.docked();
                g = s.radios(box, new String[] {docked != null ? docked : s.t("Док (TV)", "Docked (TV)"), handheld != null ? handheld : s.t("Портативный", "Handheld")}, was ? 0 : 1, false);
                s.note(box, s.t("Переключается сразу, как Switch при установке в док; выбор сохраняется", "Switches at once, as a Switch put in its dock; the choice is kept"));
            }
            public void after(GearSettings s) {
                boolean to = GearSettings.picked(g) == 0;
                if (to != was && !fh.setDocked(to)) s.toast(s.t("Режим не переключился", "The mode did not switch"), false);
            }
            public void env(GearSettings s) throws ErrnoException { if (s.gamePrefs().getInt("front_opmode", 1) == 0) Os.setenv("DM_OPMODE", "0", true); }
        };
    }
    // The pause menu's setDocked for MODE_OPMODE ports (Eternal / XC2 / D3): the live switch, then the pick kept for the next start
    static boolean setOpmode(GearHost h, boolean docked, java.util.function.IntConsumer nativeSetOpmode) {
        if (!h.libraryLoaded()) return false;
        try { nativeSetOpmode.accept(docked ? 1 : 0); } catch (UnsatisfiedLinkError e) { android.util.Log.e("dm", "nativeSetOpmode missing: " + e.getMessage()); return false; }
        h.gamePrefs().edit().putInt("front_opmode", docked ? 1 : 0).apply();
        android.util.Log.i("dm", "[java] Switch mode -> " + (docked ? "docked" : "handheld"));
        return true;
    }

    // ---- picture shape (XC2 / D3 / TL2): pref "aspect" = an index into labels; bars (the 16:9 one) and stretched apply at once (the port's
    // relayout, which calls GearDisplay.fitSurface), a widescreen mode needs a restart (the port's env says so to its game: env(value))
    static GearSettings.Section pictureShape(String[] labels, int def, String note, Runnable relayout, Env env) { return pictureShape(labels, def, note, relayout, env, null); }
    // onPick (null = none): called with the index whenever the player picks another shape in the open dialog, before Save (TL2: the Switch mode
    // rows above it show the render size of the picked shape)
    static GearSettings.Section pictureShape(String[] labels, int def, String note, Runnable relayout, Env env, java.util.function.IntConsumer onPick) {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.t("Формат картинки:", "Picture shape:"));
                g = s.radios(box, labels, s.gamePrefs().getInt("aspect", def), false);
                if (onPick != null) g.setOnCheckedChangeListener((rg, id) -> onPick.accept(GearSettings.picked(rg)));
                if (note != null) s.note(box, note);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt("aspect", GearSettings.picked(g)); }
            public void after(GearSettings s) { if (relayout != null) relayout.run(); }
            public void env(GearSettings s) throws ErrnoException { if (env != null) env.set(s, s.gamePrefs().getInt("aspect", def)); }
        };
    }

    // ---- the pad's settings (FrontMenu.Pad.prefsName(), "doom_controls"; the pad reloads after Save)
    static final float[] LOOK_GAINS = {0.6f, 0.8f, 1.0f, 1.3f, 1.7f};
    static final String[] LOOK_LABELS = {"0.6", "0.8", "1.0", "1.3", "1.7"};
    static final float[] DEADZONES = {0f, 0.08f, 0.15f, 0.25f};
    // look sensitivity (swipe and fire button); label = null: gear_look (DOOM's words), else the port's (XC2: "Camera sensitivity (swipe)")
    static GearSettings.Section look(String label) {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.when(label != null ? label : s.s("gear_look"), true));
                g = s.radios(box, LOOK_LABELS, GearSettings.nearest(LOOK_GAINS, s.padPrefs().getFloat("look", 1.0f)), true);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { pad.putFloat("look", LOOK_GAINS[GearSettings.picked(g)]); }
        };
    }
    static GearSettings.Section deadzone() {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                s.heading(box, s.whenKey("gear_deadzone", true));
                g = s.radios(box, s.keys("gear_none", "gear_small", "gear_medium_dz", "gear_large"), GearSettings.nearest(DEADZONES, s.padPrefs().getFloat("move_dz", 0f)), true);
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { pad.putFloat("move_dz", DEADZONES[GearSettings.picked(g)]); }
        };
    }
    static GearSettings.Section fpsCounter() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings s, LinearLayout box) { c = s.check(box, s.whenKey("gear_fps_counter", true), s.padPrefs().getBoolean("fps", true)); }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { pad.putBoolean("fps", c.isChecked()); }
        };
    }
    // the menu (settings) button (DoomPadView.GEAR_*): normal / faded, with the faded opacity slider below (1..100 % of the usual alpha; saved and
    // shown on the pad while sliding: the pad is visible behind the dialog)
    static GearSettings.Section gearButton() {
        return new GearSettings.Section() {
            RadioGroup g;
            public void build(GearSettings s, LinearLayout box) {
                SharedPreferences cp = s.padPrefs();
                s.heading(box, s.whenKey("gear_button", true));
                g = s.radios(box, s.keys("gear_button_normal", "gear_button_faded"), Math.min(Math.max(0, cp.getInt("gear_mode", DoomPadView.GEAR_NORMAL)), 1), true);
                s.text(box, s.s("gear_button_note"));
                final int was = Math.max(1, Math.min(100, cp.getInt("gear_alpha", DoomPadView.GEAR_ALPHA_DEFAULT)));
                final TextView t = s.text(box, s.s("gear_button_alpha", was));
                android.widget.SeekBar sb = new android.widget.SeekBar(s.a); sb.setMax(99); sb.setProgress(was - 1); box.addView(sb);
                sb.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
                    public void onProgressChanged(android.widget.SeekBar b, int v, boolean user) { t.setText(s.s("gear_button_alpha", v + 1)); cp.edit().putInt("gear_alpha", v + 1).apply(); s.reloadPad(); }
                    public void onStartTrackingTouch(android.widget.SeekBar b) {}
                    public void onStopTrackingTouch(android.widget.SeekBar b) {}
                });
            }
            public void save(GearSettings s, SharedPreferences.Editor game, SharedPreferences.Editor pad) { pad.putInt("gear_mode", GearSettings.picked(g)); }
        };
    }
    // [front] the on-screen controls, as in Diablo III's gear dialog: opacity and size (live, the pad shows them behind the dialog), the layout
    // editor (the ABXY diamond moves as one; the dialog closes, the game stays held by the pause menu) and its reset; the same settings as the
    // pause menu's "On-screen gamepad…"
    static GearSettings.Section padControls() {
        return new GearSettings.Section() {
            public void build(GearSettings s, LinearLayout box) {
                FrontMenu.Pad pad = s.host.pad(); SharedPreferences cp = s.padPrefs();
                s.heading(box, s.t("Экранные контролы", "On-screen controls"));
                String[] k = pad != null ? pad.sliderKeys() : new String[] {"opacity", "size"};
                int[] d = pad != null ? pad.sliderDefaults() : new int[] {DoomPadView.OPACITY_DEFAULT, DoomPadView.SIZE_DEFAULT};
                if (k[0] != null) s.slider(box, cp, s.t("Прозрачность контролов: %d%%", "Controls' opacity: %d%%"), k[0], 5, 100, d[0], s::reloadPad);
                if (k[1] != null) s.slider(box, cp, s.t("Размер контролов: %d%%", "Controls' size: %d%%"), k[1], 60, 160, d[1], s::reloadPad);
                Button edit = s.button(box, s.t("Изменить раскладку…", "Edit the layout…"));
                Button reset = s.button(box, s.t("Сбросить раскладку", "Reset the layout"));
                reset.setOnClickListener(v -> { FrontMenu.Pad p = s.host.pad(); if (p != null) { p.resetLayout(); s.toast(s.t("Раскладка сброшена", "Layout reset"), false); } });
                edit.setOnClickListener(v -> { s.dismiss(); FrontMenu f = s.host.front(); FrontMenu.Pad p = s.host.pad(); if (f != null) f.edit(); else if (p != null) p.startEdit(null); });
            }
        };
    }
    // the menu language (Diablo III's gear option, now the shared "front" ui_lang the pause menu's gamepad dialog sets too): applies at once,
    // the dialog reopens in it
    static GearSettings.Section menuLanguage() {
        return new GearSettings.Section() {
            RadioGroup g; String was;
            final String[] codes = {"", "ru", "en"};
            public void build(GearSettings s, LinearLayout box) {
                was = s.a.getSharedPreferences(Ui.PREFS, android.content.Context.MODE_PRIVATE).getString("ui_lang", "");
                s.heading(box, s.when(s.t("Язык меню", "Menu language"), true));
                g = s.radios(box, new String[] {s.s("gear_lang_system"), "Русский", "English"}, GearSettings.indexOf(codes, was), true);
            }
            public void after(GearSettings s) {
                String l = codes[GearSettings.picked(g)];
                if (l.equals(was)) return;
                s.a.getSharedPreferences(Ui.PREFS, android.content.Context.MODE_PRIVATE).edit().putString("ui_lang", l).apply();
                s.reopen();
            }
        };
    }

    // ---- updates (DOOM 2016 BUGS_AND_FIXES 2.41, UpdateNotice.java): the app version + the game files' setup version and "Check for new versions
    // at start" (front prefs upd_check, next launch). "Check for updates" (GitHub now, the result shown either way) is not here any more: it is
    // the pause menu's row (FrontMenu.R_UPDATES, shown when FrontMenu.Host.updates(); the port's checkUpdates() calls u.checkNow()), as in DOOM
    // v0.4.22. u = the port's one UpdateNotice (made in onCreate with its UpdateNotice.Host; null = nothing shown). The port also calls u.start()
    // once the game runs (DOOM: VkDrivers.gameStarted's first-frames callback).
    static GearSettings.Section updates(UpdateNotice u) {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings s, LinearLayout box) {
                if (u == null) return;
                s.heading(box, s.s("upd_heading"));
                s.text(box, u.statusLine());
                c = s.check(box, s.whenKey("upd_check_start", false), u.startCheckOn());
            }
            // UpdateNotice keeps its own pref (front upd_check) and logs the change ("update check: at start on / off (gear)"): done in after(), so a
            // port can place it among its live calls (DOOM: between light mode and dynamic resolution, as its old dialog did)
            public void after(GearSettings s) { if (c != null) u.setStartCheck(c.isChecked()); }
        };
    }
}
