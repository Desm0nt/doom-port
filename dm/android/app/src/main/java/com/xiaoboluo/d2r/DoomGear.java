package com.xiaoboluo.d2r;

import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioGroup;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

// [DOOM] DOOM 2016's own gear sections and their order, on the shared gear (frontend-common GearSettings / GearPages, docs/FRONTEND_UNIFIED.md
// "Gear"). This file belongs to the DOOM port (dm/android/app/src/main/java/com/xiaoboluo/d2r/DoomGear.java), not to frontend-common: the sync
// never touches it. DmActivity.gearSections returns sections(this, g). The dialog is the one DmActivity.showDoomSettings built before the
// move (BUGS_AND_FIXES 2.44, parity table frontend-common/migration/doom-parity.md: same rows, order, prefs, environment, live calls in the same
// order, texts from DOOM's own resources - GearHost.gearUsesPortResources); the generic rows are GearPages', the DOOM rows are here:
//   dynamic resolution  the game's rs_enable, read now and written live (nativeGetDrs / nativeSetDrs, hle_doom_drs.c; saved in the game profile,
//                       not here) + [r6] its target fps (pref drs_fps, DM_DOOM_DRS_FPS, live nativeSetDrsFps), shown while ticked
//   light mode          [r6] the game's Switch handheld quality preset (pref light, DM_DOOM_LIGHT, live nativeSetLight; the pause menu's mode row)
//   aspect ratio        [4:3] auto / 16:9 / 4:3 (pref aspect, DM_DOOM_ASPECT + DM_DOOM_WIDE; auto <-> 16:9 live with DM_DOOM_WIDE_LIVE=1)
//   aim assist          issue #31 (2.40): the game's Aim Assist / Motion Aiming / gyro sensitivity (live, the game profile keeps them) + our
//                       DM_AA_STRENGTH (hle_hid.c; 0 = the game's weak MP-method assist)
//   game folder         GameFolder (issue #11): the folder this run uses and "Choose…"
//   updates             2.41: GearPages.updates (status line + "Check for new versions at start"; "Check for updates" is the pause menu's row)
//   safe mode           2.57: GearPages.safeMode after the driver (Mali / PowerVR only: off / auto / on, DM_SAFE_MODE; SafeMode.java)
//   diagnostics         -PdmDiag=1 environment (no row)
//   live                no row: the Save's live calls in the order the old dialog made them (see live())
// New DOOM rows go here as further sections, in the list below where they belong.
final class DoomGear {
    private DoomGear() {}
    static final float[] RES_FRACTIONS = {1.0f, 0.85f, 0.75f, 0.67f};
    static final String[] RES_LABELS = {"100% (720p)", "85% (612p)", "75% (540p)", "67% (480p)"};
    static final int[] DRS_FPS = {30, 45, 60};   // [r6] dynamic resolution target: rs_dropMilliseconds = 1000 / fps (hle_doom_drs.c)
    static final int DRS_FPS_DEFAULT = 60;
    static final float[] AA_STRENGTHS = {0f, 0.5f, 1.0f, 1.5f, 2.0f};
    static final String[] AA_LABELS = {null /* gear_aa_switch */, "0.5", "1", "1.5", "2"};
    static final String[] LANG_CODES = {"", "ru", "en-US"};
    static final String[] LANG_LABELS = {null /* gear_lang_system */, "Русский", "English"};   // game languages keep their own names
    // [4:3] Aspect ratio (next launch; hle_args.c DM_DOOM_ASPECT, BUGS_AND_FIXES 2.20): 0 = auto (the screen's: wider -> Hor+ W x 720,
    // narrower -> down to 960x720), 1 = 16:9 (the original 1280x720), 2 = 4:3 (960x720). Replaces the "wide" checkbox: wide=false -> 16:9.
    static final String[] ASPECT_ENV = {"auto", "16:9", "4:3"};
    static int aspectPref(SharedPreferences p) {
        if (!p.contains("aspect")) {   // migrate the old checkbox once
            int a = p.getBoolean("wide", true) ? 0 : 1;
            p.edit().putInt("aspect", a).remove("wide").apply();
            return a;
        }
        return Math.min(Math.max(0, p.getInt("aspect", 0)), ASPECT_ENV.length - 1);
    }

    // The dialog, top to bottom (and the start environment in the same order)
    static List<GearSettings.Section> sections(DmActivity a, GearSettings g) {
        Drs drs = new Drs(a); Light light = new Light(a); Aspect aspect = new Aspect(a); AimAssist aim = new AimAssist(a);
        List<GearSettings.Section> l = new ArrayList<>();
        l.add(diag());
        l.add(drs);
        l.add(GearPages.renderScale(RES_FRACTIONS, RES_LABELS, "DM_DOOM_RES", g.str(R.string.gear_res_drs_note)));
        l.add(light);
        l.add(GearPages.sharpen());
        l.add(GearPages.upscale());
        l.add(GearPages.fpsLimit("DM_DOOM_TICK_MAX"));   // the game's adaptive tick (hle_args.c) at the same value: game time stays real time
        l.add(aspect);
        if (a.drivers() != null) l.add(GearPages.driver(a.drivers()));
        l.add(GearPages.safeMode(a));   // [safe-mode] 2.57: off / auto / on on Mali / PowerVR only (no row elsewhere); DM_SAFE_MODE
        l.add(GearPages.gameLanguage(LANG_CODES, LANG_LABELS, true));
        l.add(aim);   // issue #31: the game's aim assist, our aim assist strength, the game's motion aiming / gyro sensitivity
        l.add(GearPages.rumble());
        l.add(GearPages.look(null));
        l.add(GearPages.deadzone());
        l.add(GearPages.fpsCounter());
        l.add(GearPages.gearButton());
        l.add(GearPages.padControls());
        l.add(folder(a));
        // the Save's live calls in the old dialog's order (its toasts and boot.log lines come in that order): DRS target + light mode (while the
        // library is loaded), the updates' "check at start" (GearPages.updates.after), then dynamic resolution, the game's controller settings,
        // the aspect ratio. The two row-less sections around the updates rows run them; the sections' own after() stay empty.
        l.add(live(s -> { if (a.libraryLoaded()) { drs.liveFps(); light.live(s); } }));
        l.add(GearPages.updates(a.updateNotice()));   // 2.41
        l.add(live(s -> { drs.live(s); aim.live(s); aspect.live(s); }));
        return l;
    }
    static GearSettings.Section live(java.util.function.Consumer<GearSettings> calls) {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {}
            public void after(GearSettings g) { calls.accept(g); }
        };
    }

    // ---- dynamic resolution: the game's rs_enable (the Video menu's "Dynamic Resolution" is the same state) + [r6] its target fps
    static final class Drs implements GearSettings.Section {
        final DmActivity a; int was = -1; CheckBox drs; RadioGroup fps;
        Drs(DmActivity a) { this.a = a; }
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences p = g.gamePrefs();
            int now = -1; try { if (a.libraryLoaded()) now = DmActivity.nativeGetDrs(); } catch (Throwable t) { android.util.Log.e("dm", "nativeGetDrs: " + t); }
            was = now;
            drs = new CheckBox(g.a);
            if (now >= 0) { drs.setText(g.when(g.str(R.string.gear_drs), true)); drs.setChecked(now == 1); }
            else { drs.setText(g.str(R.string.gear_drs_unknown)); drs.setEnabled(false); }
            box.addView(drs);
            TextView fpsH = g.heading(box, g.whenKey("gear_drs_target", true));
            String[] labels = new String[DRS_FPS.length]; for (int i = 0; i < DRS_FPS.length; i++) labels[i] = String.valueOf(DRS_FPS[i]);
            fps = g.radios(box, labels, GearSettings.indexOf(DRS_FPS, p.getInt("drs_fps", DRS_FPS_DEFAULT), DRS_FPS.length - 1), true);
            Runnable show = () -> { int v = drs.isChecked() ? View.VISIBLE : View.GONE; fpsH.setVisibility(v); fps.setVisibility(v); };
            show.run(); drs.setOnCheckedChangeListener((b, c) -> show.run());
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt("drs_fps", DRS_FPS[GearSettings.picked(fps)]); }
        void liveFps() { try { DmActivity.nativeSetDrsFps(DRS_FPS[GearSettings.picked(fps)]); } catch (Throwable t) { android.util.Log.e("dm", "nativeSetDrsFps: " + t); } }
        void live(GearSettings g) {
            if (was < 0 || drs.isChecked() == (was == 1)) return;
            boolean ok = false;
            try { ok = DmActivity.nativeSetDrs(drs.isChecked()); } catch (Throwable t) { android.util.Log.e("dm", "nativeSetDrs: " + t); }
            if (!ok) g.toast(g.str(R.string.toast_drs_failed), true);
        }
        public void env(GearSettings g) throws ErrnoException {
            SharedPreferences p = g.gamePrefs();
            if (p.contains("drs")) p.edit().remove("drs").apply();   // dynamic resolution is the game's own setting now; the old gear pref is dropped
            Os.setenv("DM_DOOM_DRS_FPS", String.valueOf(p.getInt("drs_fps", DRS_FPS_DEFAULT)), true);   // used only while DRS is on
        }
    }
    // ---- [r6] light mode (off: unset = the game is not touched); live through nativeSetLight, also at the next start
    static final class Light implements GearSettings.Section {
        final DmActivity a; boolean was; CheckBox c;
        Light(DmActivity a) { this.a = a; }
        public void build(GearSettings g, LinearLayout box) { was = g.gamePrefs().getBoolean("light", false); c = g.check(box, g.when(g.str(R.string.gear_light), true), was); }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("light", c.isChecked()); }
        void live(GearSettings g) {
            if (c.isChecked() == was) return;
            boolean ok = false;
            try { ok = DmActivity.nativeSetLight(c.isChecked()); } catch (Throwable t) { android.util.Log.e("dm", "nativeSetLight: " + t); }
            if (!ok) g.toast(g.str(R.string.toast_light_failed), true);
        }
        public void env(GearSettings g) throws ErrnoException { if (g.gamePrefs().getBoolean("light", false)) Os.setenv("DM_DOOM_LIGHT", "1", true); else Os.unsetenv("DM_DOOM_LIGHT"); }
    }
    // ---- [4:3] aspect ratio; BUGS_AND_FIXES 2.12: native answers whether it switched live (needs DM_DOOM_WIDE_LIVE=1); only auto <-> 16:9 on a
    // screen wider than 16:9 (a presenter crop); 4:3 needs a restart. DM_DOOM_WIDE mirrors it for the presenter's old crop rule and older tools.
    static final class Aspect implements GearSettings.Section {
        final DmActivity a; int was; RadioGroup r;
        Aspect(DmActivity a) { this.a = a; }
        public void build(GearSettings g, LinearLayout box) {
            g.heading(box, g.whenKey("gear_aspect", false));
            was = aspectPref(g.gamePrefs());
            r = g.radios(box, new String[] {g.s("gear_aspect_auto"), "16:9", "4:3"}, was, true);
            g.text(box, g.s("gear_aspect_note"));
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt("aspect", GearSettings.picked(r)); }
        void live(GearSettings g) {
            int pick = GearSettings.picked(r); if (pick == was) return;
            boolean live = false;
            if (pick != 2 && was != 2) try { live = a.libraryLoaded() && DmActivity.nativeWideLive(pick == 0); } catch (Throwable t) { android.util.Log.e("dm", "nativeWideLive: " + t); }
            g.toast(g.s(live ? "toast_wide_now" : "toast_wide_next"), false);
        }
        public void env(GearSettings g) throws ErrnoException {
            int asp = aspectPref(g.gamePrefs());
            Os.setenv("DM_DOOM_ASPECT", ASPECT_ENV[asp], true);
            Os.setenv("DM_DOOM_WIDE", asp == 0 ? "1" : "0", true);
        }
    }
    // ---- issue #31 (2.40): the game's own Aim Assist / Motion Aiming / motion sensitivity (its controller settings menu, hle_doom_settings.c),
    // read now from the game and written live through its setters on Save; not kept in our preferences (the game profile keeps them); only what
    // was changed in this dialog goes to the game (a change made in the game menu stays). Between them our aim assist strength (hle_hid.c
    // DM_AA_STRENGTH: the game's single-player assist scaled; 0 = the game's weak MP-method assist).
    static final class AimAssist implements GearSettings.Section {
        final DmActivity a; RadioGroup r; final int[] was = {-1, -1, -1}; CheckBox aaMenu, motionAim; android.widget.SeekBar gyroS;
        AimAssist(DmActivity a) { this.a = a; }
        public void build(GearSettings g, LinearLayout box) {
            for (int i = 0; i < 3; i++) { was[i] = -1; try { if (a.libraryLoaded()) was[i] = DmActivity.nativeGetGameSetting(i); } catch (Throwable t) { android.util.Log.e("dm", "nativeGetGameSetting: " + t); } }
            aaMenu = gameBox(g, box, R.string.gear_aa_menu, was[DmActivity.GS_AIM_ASSIST]);
            g.heading(box, g.whenKey("gear_aa", false));
            String[] l = AA_LABELS.clone(); l[0] = g.s("gear_aa_switch");
            r = g.radios(box, l, GearSettings.nearest(AA_STRENGTHS, g.gamePrefs().getFloat("aa", 1.0f)), true);
            motionAim = gameBox(g, box, R.string.gear_motion_aim, was[DmActivity.GS_MOTION_AIM]);
            // the game's slider: 0..100 in steps of 1 (joy_sixAxisGlobalScale = value x 0.6)
            final TextView gyroT = new TextView(g.a); box.addView(gyroT);
            gyroS = new android.widget.SeekBar(g.a); gyroS.setMax(100); box.addView(gyroS);
            int gs = was[DmActivity.GS_GYRO_SENS];
            if (gs >= 0) {
                gyroS.setProgress(gs); gyroT.setText(g.str(R.string.gear_gyro_sens_fmt, g.when(g.str(R.string.gear_gyro_sens), true), gs));
                gyroS.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
                    public void onProgressChanged(android.widget.SeekBar sb, int v, boolean user) { gyroT.setText(g.str(R.string.gear_gyro_sens_fmt, g.when(g.str(R.string.gear_gyro_sens), true), v)); }
                    public void onStartTrackingTouch(android.widget.SeekBar sb) {}
                    public void onStopTrackingTouch(android.widget.SeekBar sb) {}
                });
            } else { gyroT.setText(g.str(R.string.gear_game_unknown_fmt, g.str(R.string.gear_gyro_sens))); gyroS.setEnabled(false); }
            g.text(box, g.str(R.string.gear_game_settings_note));
        }
        CheckBox gameBox(GearSettings g, LinearLayout box, int label, int now) {
            CheckBox c = new CheckBox(g.a);
            if (now >= 0) { c.setText(g.when(g.str(label), true)); c.setChecked(now == 1); }
            else { c.setText(g.str(R.string.gear_game_unknown_fmt, g.str(label))); c.setEnabled(false); }
            box.addView(c); return c;
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putFloat("aa", AA_STRENGTHS[GearSettings.picked(r)]); }
        void live(GearSettings g) {
            int[] now = {motionAim.isChecked() ? 1 : 0, gyroS.getProgress(), aaMenu.isChecked() ? 1 : 0};   // GS_MOTION_AIM, GS_GYRO_SENS, GS_AIM_ASSIST
            boolean failed = false;
            for (int i = 0; i < 3; i++) if (was[i] >= 0 && now[i] != was[i]) {
                boolean ok = false;
                try { ok = DmActivity.nativeSetGameSetting(i, now[i]); } catch (Throwable t) { android.util.Log.e("dm", "nativeSetGameSetting: " + t); }
                failed |= !ok;
            }
            if (failed) g.toast(g.str(R.string.toast_game_setting_failed), true);
        }
        public void env(GearSettings g) throws ErrnoException { Os.setenv("DM_AA_STRENGTH", String.valueOf(g.gamePrefs().getFloat("aa", 1.0f)), true); }
    }
    // ---- issue #11: the game folder this run uses, and "Choose…" (the chooser saves at once; the running game keeps its folder)
    static GearSettings.Section folder(DmActivity a) {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {
                g.heading(box, g.when(g.str(R.string.gear_folder), false));
                TextView t = g.text(box, a.folderLine());
                Button b = g.button(box, g.str(R.string.folder_choose));
                b.setOnClickListener(v -> a.showFolderDialog(() -> t.setText(a.folderLine())));
            }
        };
    }
    // ---- the diagnostics APK (-PdmDiag=1, for testers who cannot edit dm/env): per-pass GPU timestamps (NVN GPASS) and the whole NVN PERF block
    // in boot.log; dm/env still wins (not overwritten). DM_LOG_TIME=1: every boot.log line starts with the wall-clock time it was written
    // (jni_android.c log_file_line); DM_GFX_PERF_SAMPLE=4: the NVN PERF parts timed every frame (windows of 60). No row.
    static GearSettings.Section diag() {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {}
            public void env(GearSettings g) throws ErrnoException {
                if (!BuildConfig.DM_DIAG) return;
                Os.setenv("DM_GFX_PASS_PROF", "1", false); Os.setenv("DM_GFX_PERF_VERBOSE", "1", false);
                Os.setenv("DM_LOG_TIME", "1", false); Os.setenv("DM_GFX_PERF_SAMPLE", "4", false);
                if (!BuildConfig.DM_DIAG_TU_DEBUG.isEmpty()) Os.setenv("TU_DEBUG", BuildConfig.DM_DIAG_TU_DEBUG, true);   // vk_driver.c keeps a set TU_DEBUG
            }
        };
    }
}
