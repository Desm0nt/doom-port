// 安卓外壳：一个全屏横屏的 SurfaceView，原生层（libdm.so，dm/android/jni_android.c）跑宿主与游戏。
// 对应 iOS 的 main.m（启动链）与 front_ios.m（视图、触屏、生命周期）；本类只做系统对接，游戏逻辑与界面都在原生层。
// Android shell: a full-screen landscape SurfaceView; the native layer (libdm.so, dm/android/jni_android.c) runs the host and the game.
// Counterpart of iOS's main.m (startup chain) and front_ios.m (view, touch, lifecycle); this class only does system integration, game logic and UI are all in the native layer.
package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.text.Editable;
import android.text.InputType;
import android.text.SpanWatcher;
import android.text.Spannable;
import android.text.Spanned;
import android.text.TextWatcher;
import android.view.inputmethod.BaseInputConnection;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.content.SharedPreferences;
import android.widget.EditText;
import android.widget.FrameLayout;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

public class DmActivity extends Activity implements SurfaceHolder.Callback, DisplayManager.DisplayListener, InputManager.InputDeviceListener {
    static final boolean DOOM = "doom".equals(BuildConfig.DM_GAME);
    private DoomPadView doomPad;
    static native int nativeStart(String filesDir, String extDir, String romfsSrc, String lang);
    static native void nativeSurface(Object surface, int w, int h);
    static native void nativeRefresh(float hz);
    static native void nativeFrameStats(float[] out);   // [DOOM] {fps, mean ms, max ms, GPU ms} over the last 0.5 s
    static native void nativeTouch(int n, int[] ids, float[] xs, float[] ys);
    static native void nativeFocus(int ev);
    static native void nativeTextResult(long id, boolean accepted, String text);
    // Platform services (jni_android.c): rumble needs the application Context; the motion sensors the display rotation; game controllers
    // (gamepad / joystick / d-pad sources) go to the native side, which answers whether the game took a key.
    static native void nativeContext(Object context);
    static native void nativeRotation(int rotation);
    static native boolean nativePadKey(int device, int keyCode, boolean down);
    static native void nativePadAxes(int device, float x, float y, float z, float rz, float lt, float rt, float hatX, float hatY);
    static native void nativePadRemoved(int device);
    static native boolean nativeWideLive(boolean on);   // [DOOM] live widescreen switch (DM_DOOM_WIDE_LIVE=1); false = applies at the next launch
    static native int nativeGetDrs();                   // [DOOM] the game's rs_enable (hle_doom_drs.c): -1 not known yet, 0 off, 1 on
    static native boolean nativeSetDrs(boolean on);     // [DOOM] queue a live rs_enable change + profile save; false = cannot be written here
    static native String nativeDriverStatus();          // [DOOM] "<requested>\t<active>\t<reason>" (vk_driver.c), "" before the driver loads

    private static WeakReference<DmActivity> current = new WeakReference<>(null);
    private static volatile int bootFailure;
    private AlertDialog inputDialog;

    private SurfaceView view;
    private FrameLayout gameFrame;
    private TouchOverlay touchOverlay;
    private boolean started;
    private boolean importing;
    private boolean libraryLoaded;
    private boolean resumed;
    private DisplayManager displays;
    private InputManager inputs;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        // 调试包：am start … --ez dmArtDump true 只把触屏控件底图写到 files/dm/artdump/ 就退出（dm/android/tools/art_compare.sh 用）
        // Debug builds: am start … --ez dmArtDump true only writes the touch control art to files/dm/artdump/ and exits (used by dm/android/tools/art_compare.sh)
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("dmArtDump", false)) {
            try { new TouchOverlay(this, null, null).dumpArt(new File(getFilesDir(), "dm/artdump")); } catch (IOException e) { android.util.Log.e("dm", "art dump: " + e); }
            finish(); return;
        }
        current = new WeakReference<>(this);
        displays = getSystemService(DisplayManager.class);
        if (displays != null) displays.registerDisplayListener(this, null);
        inputs = getSystemService(InputManager.class);
        if (inputs != null) inputs.registerInputDeviceListener(this, null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        if (DOOM) applyDisplayMode();   // [DOOM] frame rate limit above 60: ask for a display mode at a multiple of it
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        gameFrame = new FrameLayout(this);
        gameFrame.addView(view, new FrameLayout.LayoutParams(-1, -1));
        setContentView(gameFrame);
        immersive();
    }

    private void immersive() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }

    // 资源在玩家放的位置读（缺省 Download/S2X/D2R Mobile/data），要「所有文件访问」权限；没有就引导去系统设置页，回来再启动
    // Resources are read where the player put them (default Download/S2X/D2R Mobile/data), which needs "all files access"; without it, guide the player to the system settings page and start after returning
    private boolean ensureFileAccess() {
        if (Environment.isExternalStorageManager()) return true;
        TextView t = new TextView(this);
        t.setText(R.string.dmAndroidPermission);
        t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        t.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
        setContentView(t);
        return false;
    }

    // 导入包：玩家在电脑上用 dm/tools/d2rm_android.py 和自己的游戏文件做出 android 文件夹，拷到 Download/S2X/D2R Mobile/ 下；
    // 这里把它拷进内部目录 files/dm/（out、shaders、overlay、derived.tsv、pack_info.txt）。pack_info.txt 与已导入的逐字相同就不再拷。
    // 先拷到 files/dm/import.part/，齐了再逐项换上，pack_info.txt 最后放：中途被杀或空间不够，下次打开从头再导，不会用到半份。
    // 原生库加载之前做：不含游戏内容的发布包，几张表在库加载时就从 derived.tsv 读（dm/host/dm_derived_rt.h）。
    // Import pack: the player makes the android folder on a computer with dm/tools/d2rm_android.py and their own game files and copies it to
    // Download/S2X/D2R Mobile/; this copies it into the internal files/dm/ (out, shaders, overlay, derived.tsv, pack_info.txt). Nothing is copied
    // when pack_info.txt is the same as the imported one. It goes to files/dm/import.part/ first and replaces the items one by one once complete,
    // pack_info.txt last: if the app is killed or runs out of space, the next start imports again and half a copy is never used.
    // Done before the native library loads: in the release APK without game content, a few tables are read from derived.tsv at load time.
    private static final String[] IMPORT_ITEMS = {"out", "shaders", "overlay", "derived.tsv", "pack_info.txt"};

    private static String readText(File f) {
        try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return null; }
    }

    private static String packProduct(String info) {
        if (info != null) for (String line : info.split("\n")) if (line.startsWith("PRODUCT=")) return line.substring(8).trim();
        return null;
    }

    // true = 可以接着启动；false = 正在导入或已显示错误
    // true = startup can continue; false = importing, or an error is shown
    private boolean ensureImported(File source) {
        if (importing) return false;
        File runtime = new File(getFilesDir(), "dm");
        String want = readText(new File(source, "pack_info.txt")), have = readText(new File(runtime, "pack_info.txt"));
        if (want != null && !want.equals(have)) {
            String product = packProduct(want);
            if (!BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
            importing = true;
            TextView t = new TextView(this);
            t.setTextSize(20); t.setPadding(64, 64, 64, 64); t.setText(getString(R.string.dmAndroidImporting, 0));
            setContentView(t);
            new Thread(() -> {
                String error = null;
                try { importPack(source, runtime, percent -> runOnUiThread(() -> t.setText(getString(R.string.dmAndroidImporting, percent)))); }
                catch (IOException e) { error = e.getMessage() == null ? e.toString() : e.getMessage(); }
                String failure = error;
                runOnUiThread(() -> {
                    importing = false;
                    if (failure != null) showStartError(getString(R.string.dmAndroidImportFailed, failure));
                    else startHost();
                });
            }, "dm-import").start();
            return false;
        }
        // 已导入的是给别的版本做的（App 升级了、导入包没重做）：同样要重做导入包。开发时直接推进来的没有 pack_info.txt，不核
        // The imported pack was made for another version (the app was updated, the pack was not remade): it has to be remade too.
        // Files pushed directly during development have no pack_info.txt and are not checked
        String product = packProduct(have);
        if (have != null && !BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
        return true;
    }

    private interface Progress { void percent(int p); }

    private static void importPack(File source, File runtime, Progress progress) throws IOException {
        for (String item : IMPORT_ITEMS) if (!new File(source, item).exists()) throw new IOException("missing " + item);
        File part = new File(runtime, "import.part");
        deleteTree(part);
        long sum = 0;
        for (String item : IMPORT_ITEMS) sum += treeSize(new File(source, item));
        final long total = sum;
        long[] done = {0}; int[] shown = {-1};
        byte[] buffer = new byte[1 << 20];
        for (String item : IMPORT_ITEMS) copyTree(new File(source, item), new File(part, item), buffer, bytes -> {
            done[0] += bytes;
            int p = (int) (done[0] * 100 / Math.max(1, total));
            if (p != shown[0]) { shown[0] = p; progress.percent(p); }
        });
        for (String item : IMPORT_ITEMS) {
            File from = new File(part, item), to = new File(runtime, item);
            deleteTree(to);
            if (!from.renameTo(to)) throw new IOException("rename " + item);
        }
        deleteTree(part);
    }

    private interface Copied { void bytes(long n); }

    private static long treeSize(File f) {
        if (!f.isDirectory()) return f.length();
        long n = 0; File[] list = f.listFiles();
        if (list != null) for (File c : list) n += treeSize(c);
        return n;
    }

    private static void copyTree(File from, File to, byte[] buffer, Copied copied) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to.getName());
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from.getName());
            for (File c : list) copyTree(c, new File(to, c.getName()), buffer, copied);
            return;
        }
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent.getName());
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buffer)) > 0; ) { out.write(buffer, 0, n); copied.bytes(n); }
        }
    }

    private static void deleteTree(File f) throws IOException {
        File[] list = f.isDirectory() ? f.listFiles() : null;
        if (list != null) for (File c : list) deleteTree(c);
        if (f.exists() && !f.delete()) throw new IOException("delete " + f.getName());
    }


    // [DOOM] Gear (settings) dialog on the on-screen pad. Texts are string resources (res/values*/doom_settings.xml: English default,
    // ru uk de fr es pt it pl tr zh ja ko by the system locale). Two kinds of options, each labelled with when it applies:
    //   next launch: kept in SharedPreferences "doom_graphics" and handed to the native host as environment variables before the
    //     library loads (hle_args.c turns them into the game's +set arguments; vulkan_present.cpp, vk_driver.c, haptics_android.c,
    //     hle_hid.c, hle_nvn.c read them once at start) -- 3D resolution, sharpening, frame rate limit, widescreen (unless the live
    //     switch DM_DOOM_WIDE_LIVE=1 is on), Vulkan driver, game language, aim assist, vibration;
    //   immediately: the controls in "doom_controls" (DoomPadView.reloadSettings: look sensitivity, dead zone, FPS counter) and
    //     dynamic resolution, which is the game's own rs_enable cvar shared with Video -> Dynamic Resolution in the game menu
    //     (nativeGetDrs / nativeSetDrs, hle_doom_drs.c; saved in the game profile, not here).
    static final float[] RES_FRACTIONS = {1.0f, 0.85f, 0.75f, 0.67f};
    static final String[] RES_LABELS = {"100% (720p)", "85% (612p)", "75% (540p)", "67% (480p)"};
    static final String[] SHARPEN_STOPS = {"", "1.0", "0.5", "0.2"};
    static final int[] SHARPEN_LABELS = {R.string.gear_off, R.string.gear_low, R.string.gear_medium, R.string.gear_high};
    static final float[] AA_STRENGTHS = {0f, 0.5f, 1.0f, 1.5f, 2.0f};
    static final String[] AA_LABELS = {null /* gear_aa_switch */, "0.5", "1", "1.5", "2"};
    static final String[] LANG_CODES = {"", "ru", "en-US"};
    static final String[] LANG_LABELS = {null /* gear_lang_system */, "Русский", "English"};   // game languages keep their own names
    static final float[] LOOK_GAINS = {0.6f, 0.8f, 1.0f, 1.3f, 1.7f};
    static final String[] LOOK_LABELS = {"0.6", "0.8", "1.0", "1.3", "1.7"};
    static final float[] DEADZONES = {0f, 0.08f, 0.15f, 0.25f};
    static final int[] DEADZONE_LABELS = {R.string.gear_none, R.string.gear_small, R.string.gear_medium_dz, R.string.gear_large};
    // Frame rate limit (next launch): DM_PRESENT_MAX_HZ caps the present pacing (hle_nvn.c: the largest integer fraction of the refresh
    // rate not above it) and DM_DOOM_TICK_MAX the game's adaptive tick (hle_args.c) to the same value, so game time stays real time.
    // 90 / 120 are offered only when the display has a mode at a multiple of that rate; the window then asks for that mode
    // (applyDisplayMode) and the surface votes for it (vulkan_present.cpp). Above 60 is untested (frame-counted UI logic risk).
    static final int[] FPS_LIMITS = {30, 60, 90, 120};
    static final int FPS_LIMIT_DEFAULT = 60;
    static int nearest(float[] v, float x) { int b = 0; for (int i = 1; i < v.length; i++) if (Math.abs(v[i] - x) < Math.abs(v[b] - x)) b = i; return b; }

    private SharedPreferences doomPrefs() { return getSharedPreferences("doom_graphics", MODE_PRIVATE); }

    private android.view.Display display() {
        android.view.Display d = null;
        try { d = getDisplay(); } catch (RuntimeException e) { /* not attached yet */ }
        if (d == null && displays != null) d = displays.getDisplay(android.view.Display.DEFAULT_DISPLAY);
        return d;
    }
    // A display mode whose refresh rate is an integer multiple of hz (within 1 Hz), preferring the current resolution, then the
    // lowest such rate; null = the display has none.
    private android.view.Display.Mode modeFor(int hz) {
        android.view.Display d = display(); if (d == null) return null;
        android.view.Display.Mode cur = d.getMode(), best = null; boolean bestSame = false;
        for (android.view.Display.Mode m : d.getSupportedModes()) {
            float r = m.getRefreshRate(); int k = Math.round(r / hz);
            if (k < 1 || Math.abs(r - k * hz) > 1.0f) continue;
            boolean same = cur != null && m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight();
            if (best == null || (same && !bestSame) || (same == bestSame && r < best.getRefreshRate())) { best = m; bestSame = same; }
        }
        return best;
    }
    private boolean fpsLimitAvailable(int hz) { return hz <= 60 || modeFor(hz) != null; }
    // The saved limit, lowered to the highest one this display supports (another screen / the system's refresh setting changed).
    private int fpsLimit() {
        int want = doomPrefs().getInt("fps_limit", FPS_LIMIT_DEFAULT);
        if (want <= 60) return want == 30 ? 30 : 60;
        int got = 60; for (int l : FPS_LIMITS) if (l > got && l <= want && fpsLimitAvailable(l)) got = l;
        return got;
    }
    // Above 60 the window asks the system for a display mode at a multiple of the limit (e.g. 90 Hz for 90, 120 Hz for 120); at 30 / 60
    // nothing is requested (the surface's own 120 Hz vote and the system decide, as before).
    private void applyDisplayMode() {
        int lim = fpsLimit(); if (lim <= 60) return;
        android.view.Display.Mode m = modeFor(lim); if (m == null) return;
        WindowManager.LayoutParams lp = getWindow().getAttributes(); lp.preferredDisplayModeId = m.getModeId(); getWindow().setAttributes(lp);
        android.util.Log.i("dm", "frame rate limit " + lim + ": display mode " + m.getModeId() + " " + m.getPhysicalWidth() + "x" + m.getPhysicalHeight() + " @ " + m.getRefreshRate() + " Hz");
    }

    // [DOOM] Driver crash guard: before a non-system driver is loaded its name goes to files/driver-trying ("<name>\n<failed launches>");
    // the file is removed once the game has presented a frame or the native side reports it fell back to the system driver
    // (driverWatch). A file left over means the last launch died before its first frame with that driver: after DRIVER_GUARD_STRIKES
    // such launches in a row the system driver is chosen (saved in the settings, so the player sees it and can pick the package again)
    // and a message says why. Two strikes, not one: a launch killed by hand (or by a debug script) before its first frame is not a crash.
    static final int DRIVER_GUARD_STRIKES = 2;
    private String driverNotice;   // shown once the game view is up
    private File driverGuard() { return new File(getFilesDir(), "driver-trying"); }

    private void applyDoomSettings() {
        SharedPreferences p = doomPrefs();
        try {
            // Dynamic resolution is the game's own setting now (hle_doom_drs.c); the old gear preference is dropped.
            if (p.contains("drs")) p.edit().remove("drs").apply();
            int lim = fpsLimit();
            Os.setenv("DM_PRESENT_MAX_HZ", String.valueOf(lim), true);
            Os.setenv("DM_DOOM_TICK_MAX", String.valueOf(lim), true);
            int res = p.getInt("res", 0);
            if (res > 0 && res < RES_FRACTIONS.length) Os.setenv("DM_DOOM_RES", String.valueOf(RES_FRACTIONS[res]), true); else Os.unsetenv("DM_DOOM_RES");
            int sh = p.getInt("sharpen", 0);
            if (sh > 0 && sh < SHARPEN_STOPS.length) Os.setenv("DM_GFX_SHARPEN", SHARPEN_STOPS[sh], true); else Os.unsetenv("DM_GFX_SHARPEN");
            // Vulkan driver (vk_driver.c): adrenotools packages under <data>/drivers/<name>/ (meta.json + the library) are copied to
            // private storage (a driver must be loaded from there); the package chosen in the dialog sets DM_VK_DRIVER, which dm/env
            // can also set (DM_VK_DRIVER=<name>). The hooks adrenotools loads live in nativeLibraryDir.
            Os.setenv("DM_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            File drivers = new File(getFilesDir(), "drivers");
            Os.setenv("DM_DRIVERS_DIR", drivers.getAbsolutePath(), true);
            extractBundledDrivers(drivers);   // the APK's default driver (Turnip, assets/drivers/<name>/)
            importDrivers(new File(Environment.getExternalStorageDirectory(), "Download/DOOM Mobile/drivers"), drivers);
            updateDriversInBackground(drivers);   // other packages from the repository's drivers/manifest.json when online
            // Game language (DOOM takes it from the system like on the Switch: hle_oe.c GetDesiredLanguage reads DM_LANG; jni sets
            // DM_LANG from the phone locale only when it is unset). "" = follow the phone.
            // Aim assist strength (hle_hid.c DM_AA_STRENGTH: the game's single-player assist scaled; 0 = the game's weak MP-method assist)
            Os.setenv("DM_AA_STRENGTH", String.valueOf(p.getFloat("aa", 1.0f)), true);
            // Rumble (haptics_android.c): off by default since 28.10 (owner: it only drains the battery); DM_RUMBLE=0 hides the vibration devices.
            Os.setenv("DM_RUMBLE", p.getBoolean("rumble", false) ? "1" : "0", true);
            // Widescreen (hle_args.c doom_wide): the game renders at the screen aspect (e.g. 1600x720) with a wider Hor+ view; on by default.
            Os.setenv("DM_DOOM_WIDE", p.getBoolean("wide", true) ? "1" : "0", true);
            String lang = p.getString("lang", "");
            if (!lang.isEmpty()) Os.setenv("DM_LANG", lang, true);
            String drv = p.getString("driver", defaultDriver());
            File guard = driverGuard(); String left = readText(guard);
            int strikes = 0;
            if (left != null) {
                String[] g = left.split("\n"); String name = g[0].trim();
                try { strikes = g.length > 1 ? Integer.parseInt(g[1].trim()) : 1; } catch (NumberFormatException e) { strikes = 1; }
                if (!name.equals(drv)) strikes = 0;   // about another driver: the player changed it since
                android.util.Log.w("dm", "driver guard: last launch with " + name + " ended before its first frame (" + strikes + " in a row)");
                if (strikes >= DRIVER_GUARD_STRIKES) {
                    p.edit().putString("driver", "").apply(); drv = ""; strikes = 0;
                    driverNotice = getString(R.string.toast_driver_crashed, driverLabel(name));
                }
                guard.delete();
            }
            if (!drv.isEmpty()) {
                try (OutputStream os = new FileOutputStream(guard)) { os.write((drv + "\n" + (strikes + 1) + "\n").getBytes(StandardCharsets.UTF_8)); }
                catch (IOException e) { android.util.Log.e("dm", "driver guard: " + e); }
            }
            if (!drv.isEmpty()) Os.setenv("DM_VK_DRIVER", drv, true); else Os.unsetenv("DM_VK_DRIVER");
        } catch (ErrnoException e) { android.util.Log.e("dm", "doom settings: " + e); }
    }

    // [DOOM] Default Vulkan driver per GPU (docs/research/non_adreno_drivers.md 4.1); the player's choice (pref "driver") always wins.
    // This is the hook for vendor rules: it returns a package name under files/drivers/ or "" = the system driver.
    //   Adreno 6xx / 7xx            -> turnip27 (bundled)
    //   Adreno 710 / 720 / 722      -> turnip27-a710 when downloaded, else system
    //   Adreno 8xx                  -> turnip27-a8xx when downloaded, else system
    //   Adreno 5xx and older        -> system (Turnip does not support a5xx)
    //   Mali / PowerVR / Xclipse / Maleoon (no kgsl gpu_model) -> system; their optional packages (PanVK, RADV-Xclipse) stay
    //                                  selectable in the dialog when the player supplies them
    // A manifest entry with an "auto" regex that matches this Adreno (drivers/manifest.json) still comes first for Adreno >= 600.
    // When gpu_model is unreadable on a Qualcomm SoC, turnip27 is tried (vk_driver.c falls back to the system driver without a GPU).
    static final String DEFAULT_DRIVER = "turnip27", DRIVER_A710 = "turnip27-a710", DRIVER_A8XX = "turnip27-a8xx";
    private String defaultDriver() {
        File dir = new File(getFilesDir(), "drivers");
        int m = adrenoModel(gpuModel());
        if (m == 0) return qualcommSoc() ? pkgIfPresent(dir, DEFAULT_DRIVER) : "";
        if (m < 600) return "";
        String auto = autoDriverForGpu(dir); if (auto != null) return auto;
        if (m == 710 || m == 720 || m == 722) return pkgIfPresent(dir, DRIVER_A710);
        if (m >= 800 && m < 900) return pkgIfPresent(dir, DRIVER_A8XX);
        return pkgIfPresent(dir, DEFAULT_DRIVER);
    }
    private static String pkgIfPresent(File dir, String name) { return hasDriverLib(new File(dir, name)) ? name : ""; }
    // "Adreno750v2" -> 750; 0 = no readable model; -1 = a model that is not an Adreno.
    static int adrenoModel(String gpu) {
        if (gpu == null || gpu.isEmpty()) return 0;
        String g = gpu.toLowerCase(Locale.ROOT); int at = g.indexOf("adreno"); if (at < 0) return -1;
        int i = at + 6, v = 0, n = 0;
        while (i < g.length() && !Character.isDigit(g.charAt(i)) && n == 0 && i < at + 12) i++;
        while (i < g.length() && Character.isDigit(g.charAt(i)) && n < 4) { v = v * 10 + (g.charAt(i++) - '0'); n++; }
        return n > 0 ? v : -1;
    }
    static boolean qualcommSoc() {
        String s = (android.os.Build.SOC_MANUFACTURER + " " + android.os.Build.HARDWARE).toLowerCase(Locale.ROOT);
        return s.contains("qti") || s.contains("qualcomm") || s.contains("qcom");
    }
    // The GPU model as the kernel reports it, e.g. "Adreno750v2" (empty when unreadable or not an Adreno).
    static String gpuModel() {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/sys/class/kgsl/kgsl-3d0/gpu_model"))) {
            String l = r.readLine(); return l == null ? "" : l.trim();
        } catch (java.io.IOException e) { return ""; }
    }
    // A downloaded package whose manifest entry has an "auto" regex matching this GPU (drivers/manifest.json in the repository,
    // cached as <dir>/manifest.json), e.g. the A710/A720 Turnip on an Adreno 720; null when none applies.
    private static String autoDriverForGpu(File dir) {
        String gpu = gpuModel(); if (gpu.isEmpty()) return null;
        try {
            org.json.JSONArray pk = new org.json.JSONObject(readSmall(new File(dir, "manifest.json"))).getJSONArray("packages");
            for (int i = 0; i < pk.length(); i++) {
                org.json.JSONObject o = pk.getJSONObject(i); String re = o.optString("auto", ""), name = o.getString("name");
                if (!re.isEmpty() && java.util.regex.Pattern.compile(re).matcher(gpu).find() && hasDriverLib(new File(dir, name))) return name;
            }
        } catch (Exception e) { /* no manifest yet */ }
        return null;
    }
    static boolean hasDriverLib(File d) { File[] fs = d.listFiles(); if (fs != null) for (File f : fs) if (f.getName().endsWith(".so")) return true; return false; }
    static String readSmall(File f) throws java.io.IOException {
        try (java.io.InputStream in = new java.io.FileInputStream(f)) { java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[8192]; int n; while ((n = in.read(buf)) > 0) b.write(buf, 0, n); return b.toString("UTF-8"); }
    }
    // [DOOM] Driver packages from the internet (when there is a connection): the repository's drivers/manifest.json lists
    // adrenotools packages (name, url, sha256, optional "auto" GPU regex). Missing ones are downloaded in the background, checked
    // against their SHA-256 and unpacked to <dir>/<name>/; they show up in the settings and, when "auto" matches this GPU, are
    // used by default from the next launch. Offline nothing happens and the bundled Turnip / system driver are used.
    static final String DRIVER_MANIFEST_URL = "https://raw.githubusercontent.com/Desm0nt/doom-port/main/drivers/manifest.json";
    private void updateDriversInBackground(final File dir) {
        new Thread(() -> {
            try {
                byte[] man = httpGet(DRIVER_MANIFEST_URL, 1 << 20);
                org.json.JSONArray pk = new org.json.JSONObject(new String(man, "UTF-8")).getJSONArray("packages");
                dir.mkdirs(); try (java.io.FileOutputStream os = new java.io.FileOutputStream(new File(dir, "manifest.json"))) { os.write(man); }
                for (int i = 0; i < pk.length(); i++) {
                    org.json.JSONObject o = pk.getJSONObject(i); String name = o.getString("name");
                    if (!name.matches("[A-Za-z0-9._-]+") || hasDriverLib(new File(dir, name))) continue;
                    byte[] zip = httpGet(o.getString("url"), 64 << 20);
                    String got = sha256(zip);
                    if (!got.equalsIgnoreCase(o.getString("sha256"))) { android.util.Log.e("dm", "driver " + name + ": SHA-256 mismatch " + got + ", skipped"); continue; }
                    unzipDriver(new java.io.ByteArrayInputStream(zip), new File(dir, name));
                    android.util.Log.i("dm", "driver " + name + ": downloaded (" + zip.length + " bytes)");
                }
            } catch (Exception e) { android.util.Log.i("dm", "driver update skipped: " + e); }
        }, "dm-driver-update").start();
    }
    static byte[] httpGet(String url, int max) throws java.io.IOException {
        for (int hop = 0; hop < 5; hop++) {   // GitHub release assets redirect to another host
            java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            c.setConnectTimeout(10000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(false);
            int code = c.getResponseCode();
            if (code / 100 == 3) { url = c.getHeaderField("Location"); c.disconnect(); if (url == null) break; continue; }
            if (code != 200) { c.disconnect(); throw new java.io.IOException("HTTP " + code + " " + url); }
            try (java.io.InputStream in = c.getInputStream()) {
                java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[1 << 16]; int n;
                while ((n = in.read(buf)) > 0) { b.write(buf, 0, n); if (b.size() > max) throw new java.io.IOException("too large: " + url); }
                return b.toByteArray();
            } finally { c.disconnect(); }
        }
        throw new java.io.IOException("too many redirects");
    }
    static String sha256(byte[] d) {
        try { byte[] h = java.security.MessageDigest.getInstance("SHA-256").digest(d); StringBuilder sb = new StringBuilder(); for (byte x : h) sb.append(String.format("%02x", x)); return sb.toString(); }
        catch (java.security.NoSuchAlgorithmException e) { return ""; }
    }
    // Unpack an adrenotools package zip (meta.json + the driver library, at the zip root or in one folder) into out/.
    static void unzipDriver(java.io.InputStream src, File out) throws java.io.IOException {
        File tmp = new File(out.getPath() + ".tmp"); deleteDriverTree(tmp); tmp.mkdirs();
        try (java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(src)) {
            java.util.zip.ZipEntry e; byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String n = new File(e.getName()).getName(); if (!(n.endsWith(".so") || n.equals("meta.json"))) continue;
                try (java.io.OutputStream os = new java.io.FileOutputStream(new File(tmp, n))) { int k; while ((k = z.read(buf)) > 0) os.write(buf, 0, k); }
            }
        }
        if (!hasDriverLib(tmp)) { deleteDriverTree(tmp); throw new java.io.IOException("no driver library in the package"); }
        deleteDriverTree(out); if (!tmp.renameTo(out)) throw new java.io.IOException("rename failed: " + out);
    }
    static void deleteDriverTree(File f) { File[] fs = f.listFiles(); if (fs != null) for (File c : fs) deleteDriverTree(c); f.delete(); }
    // Copy assets/drivers/<name>/* to <dst>/<name>/ when missing or of another size.
    private void extractBundledDrivers(File dst) {
        try {
            String[] names = getAssets().list("drivers"); if (names == null) return;
            for (String n : names) {
                String[] files = getAssets().list("drivers/" + n); if (files == null || files.length == 0) continue;
                File out = new File(dst, n); out.mkdirs();
                for (String f : files) {
                    File o = new File(out, f);
                    try (java.io.InputStream in = getAssets().open("drivers/" + n + "/" + f)) {
                        long size = -1; try (android.content.res.AssetFileDescriptor fd = getAssets().openFd("drivers/" + n + "/" + f)) { size = fd.getLength(); } catch (java.io.IOException e) { /* compressed asset: size unknown */ }
                        if (o.isFile() && (size < 0 ? o.length() > 0 : o.length() == size)) continue;
                        try (java.io.OutputStream os = new java.io.FileOutputStream(o)) { byte[] buf = new byte[1 << 16]; int k; while ((k = in.read(buf)) > 0) os.write(buf, 0, k); }
                        android.util.Log.i("dm", "bundled driver " + n + ": " + f + " extracted");
                    }
                }
            }
        } catch (java.io.IOException e) { android.util.Log.e("dm", "bundled drivers: " + e); }
    }

    // Driver packages in private storage (imported at launch from Download/DOOM Mobile/drivers), sorted by folder name.
    private String[] driverPackages() {
        File[] dirs = new File(getFilesDir(), "drivers").listFiles(); java.util.ArrayList<String> out = new java.util.ArrayList<>();
        if (dirs != null) for (File d : dirs) { File[] fs = d.isDirectory() ? d.listFiles() : null; if (fs == null) continue;
            for (File f : fs) if (f.getName().endsWith(".so")) { out.add(d.getName()); break; } }
        java.util.Collections.sort(out); return out.toArray(new String[0]);
    }
    // "<meta.json name> (<folder>)", or just the folder name when there is no readable meta.json.
    private String driverLabel(String name) {
        File m = new File(new File(new File(getFilesDir(), "drivers"), name), "meta.json");
        try (java.io.FileInputStream in = new java.io.FileInputStream(m)) {
            byte[] b = new byte[(int) Math.min(m.length(), 65536)]; int n = in.read(b);
            String t = new org.json.JSONObject(new String(b, 0, Math.max(n, 0), "UTF-8")).optString("name", "");
            if (!t.isEmpty()) return t + " (" + name + ")";
        } catch (Exception e) { /* no or unreadable meta.json */ }
        return name;
    }

    // Copy every driver package <src>/<name>/{meta.json, library} into <dst>/<name>/ when missing or of another size.
    private static void importDrivers(File src, File dst) {
        File[] dirs = src.listFiles(); if (dirs == null) return;
        for (File d : dirs) {
            if (d.isFile() && d.getName().endsWith(".zip")) {   // a package zip dropped as is: unpack to <dst>/<zip name>/ once
                String name = d.getName().substring(0, d.getName().length() - 4); File out = new File(dst, name);
                if (!hasDriverLib(out)) try (java.io.InputStream in = new java.io.FileInputStream(d)) { unzipDriver(in, out); android.util.Log.i("dm", "driver package " + name + ": unpacked from zip"); }
                catch (java.io.IOException e) { android.util.Log.e("dm", "driver package " + d.getName() + ": " + e); }
                continue;
            }
            File[] files = d.isDirectory() ? d.listFiles() : null; if (files == null) continue;
            File out = new File(dst, d.getName()); out.mkdirs();
            for (File f : files) {
                File o = new File(out, f.getName());
                if (o.length() == f.length()) continue;
                try (java.io.InputStream in = new java.io.FileInputStream(f); java.io.OutputStream os = new java.io.FileOutputStream(o)) {
                    byte[] buf = new byte[1 << 16]; int n; while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                    android.util.Log.i("dm", "driver package " + d.getName() + ": " + f.getName() + " copied");
                } catch (java.io.IOException e) { android.util.Log.e("dm", "driver package " + d.getName() + ": " + e); }
            }
        }
    }

    // "<label> (applies immediately)" / "<label> (applies on next launch)", in the locale's own punctuation (gear_label_fmt).
    private String when(int label, boolean now) {
        return getString(R.string.gear_label_fmt, getString(label), getString(now ? R.string.gear_when_now : R.string.gear_when_next));
    }
    private TextView heading(LinearLayout box, String text) {
        TextView t = new TextView(this); t.setText(text); t.setPadding(0, 24, 0, 0); box.addView(t); return t;
    }
    private RadioGroup radios(LinearLayout box, String[] labels, int idBase, int checked, boolean horizontal) {
        RadioGroup g = new RadioGroup(this); g.setOrientation(horizontal ? RadioGroup.HORIZONTAL : RadioGroup.VERTICAL);
        for (int i = 0; i < labels.length; i++) { RadioButton r = new RadioButton(this); r.setText(labels[i]); r.setId(idBase + i); g.addView(r); }
        g.check(idBase + checked); box.addView(g); return g;
    }
    private String[] texts(int[] ids) { String[] s = new String[ids.length]; for (int i = 0; i < ids.length; i++) s[i] = getString(ids[i]); return s; }
    private static int picked(RadioGroup g, int idBase, int n) { return Math.min(Math.max(0, g.getCheckedRadioButtonId() - idBase), n - 1); }
    // "<requested>\t<active>\t<reason>" from vk_driver.c, or null before the driver is loaded.
    private String[] driverStatus() {
        if (!libraryLoaded) return null;
        String s; try { s = nativeDriverStatus(); } catch (Throwable t) { return null; }
        String[] f = s == null ? new String[0] : s.split("\t", -1);
        return f.length >= 3 ? f : null;
    }

    void showDoomSettings() {
        runOnUiThread(() -> {
            SharedPreferences p = doomPrefs(), cp = getSharedPreferences("doom_controls", MODE_PRIVATE);
            LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
            // Dynamic resolution: the game's rs_enable, read now and written live (the Video menu's "Dynamic Resolution" is the same state).
            int drsNow = -1; try { if (libraryLoaded) drsNow = nativeGetDrs(); } catch (Throwable t) { android.util.Log.e("dm", "nativeGetDrs: " + t); }
            final int drsWas = drsNow;
            CheckBox drs = new CheckBox(this);
            if (drsNow >= 0) { drs.setText(when(R.string.gear_drs, true)); drs.setChecked(drsNow == 1); }
            else { drs.setText(R.string.gear_drs_unknown); drs.setEnabled(false); }
            box.addView(drs);
            heading(box, when(R.string.gear_res, false));
            RadioGroup res = radios(box, RES_LABELS, 1000, Math.min(Math.max(0, p.getInt("res", 0)), RES_LABELS.length - 1), false);
            heading(box, when(R.string.gear_sharpen, false));
            RadioGroup sh = radios(box, texts(SHARPEN_LABELS), 2000, Math.min(Math.max(0, p.getInt("sharpen", 0)), SHARPEN_LABELS.length - 1), true);
            // Frame rate limit: only the values this display can show evenly.
            java.util.ArrayList<Integer> lims = new java.util.ArrayList<>();
            for (int l : FPS_LIMITS) if (fpsLimitAvailable(l)) lims.add(l);
            String[] limLabels = new String[lims.size()]; int limAt = 0, limCur = fpsLimit();
            for (int i = 0; i < limLabels.length; i++) { limLabels[i] = String.valueOf(lims.get(i)); if (lims.get(i) == limCur) limAt = i; }
            heading(box, when(R.string.gear_fps_limit, false));
            RadioGroup fpsLim = radios(box, limLabels, 8000, limAt, true);
            if (lims.get(lims.size() - 1) > 60) { TextView n = new TextView(this); n.setText(R.string.gear_fps_note); box.addView(n); }
            CheckBox wide = new CheckBox(this); wide.setText(when(R.string.gear_wide, false)); wide.setChecked(p.getBoolean("wide", true)); box.addView(wide);
            // Vulkan driver: the system one, or any adrenotools package under files/drivers/<name>/ (bundled, downloaded or imported from
            // Download/DOOM Mobile/drivers), labelled with the name from its meta.json; below it, what the running game actually uses.
            heading(box, when(R.string.gear_driver, false));
            final String[] drvNames = driverPackages();
            String[] drvLabels = new String[drvNames.length + 1]; drvLabels[0] = getString(R.string.gear_driver_system);
            for (int i = 0; i < drvNames.length; i++) drvLabels[i + 1] = driverLabel(drvNames[i]);
            int drvAt = 0; { String cur = p.getString("driver", defaultDriver()); for (int i = 0; i < drvNames.length; i++) if (drvNames[i].equals(cur)) drvAt = i + 1; }
            RadioGroup drvG = radios(box, drvLabels, 7000, drvAt, false);
            String[] st = driverStatus();
            if (st != null) {
                boolean fell = !st[0].isEmpty() && "system".equals(st[1]);
                TextView a = new TextView(this);
                a.setText(fell ? getString(R.string.gear_driver_active_fallback, driverLabel(st[0]))
                               : getString(R.string.gear_driver_active, st[0].isEmpty() || "system".equals(st[1]) ? getString(R.string.gear_driver_system_short) : driverLabel(st[0])));
                box.addView(a);
            }
            heading(box, when(R.string.gear_lang, false));
            String[] langLabels = LANG_LABELS.clone(); langLabels[0] = getString(R.string.gear_lang_system);
            int langAt = 0; { String cur = p.getString("lang", ""); for (int i = 0; i < LANG_CODES.length; i++) if (LANG_CODES[i].equals(cur)) langAt = i; }
            RadioGroup langG = radios(box, langLabels, 5000, langAt, true);
            heading(box, when(R.string.gear_aa, false));
            String[] aaLabels = AA_LABELS.clone(); aaLabels[0] = getString(R.string.gear_aa_switch);
            RadioGroup aaG = radios(box, aaLabels, 6000, nearest(AA_STRENGTHS, p.getFloat("aa", 1.0f)), true);
            CheckBox rumble = new CheckBox(this); rumble.setText(when(R.string.gear_rumble, false)); rumble.setChecked(p.getBoolean("rumble", false)); box.addView(rumble);
            heading(box, when(R.string.gear_look, true));
            RadioGroup look = radios(box, LOOK_LABELS, 3000, nearest(LOOK_GAINS, cp.getFloat("look", 1.0f)), true);
            heading(box, when(R.string.gear_deadzone, true));
            RadioGroup dz = radios(box, texts(DEADZONE_LABELS), 4000, nearest(DEADZONES, cp.getFloat("move_dz", 0f)), true);
            CheckBox fps = new CheckBox(this); fps.setText(when(R.string.gear_fps_counter, true)); fps.setChecked(cp.getBoolean("fps", true)); box.addView(fps);
            android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(box);
            new AlertDialog.Builder(this).setTitle(R.string.gear_title).setView(scroll)
                .setPositiveButton(R.string.gear_save, (d, w) -> {
                    boolean wideChanged = wide.isChecked() != p.getBoolean("wide", true);
                    int drvPick = picked(drvG, 7000, drvNames.length + 1);
                    p.edit().putString("driver", drvPick > 0 ? drvNames[drvPick - 1] : "").putBoolean("rumble", rumble.isChecked()).putBoolean("wide", wide.isChecked())
                        .putFloat("aa", AA_STRENGTHS[picked(aaG, 6000, AA_STRENGTHS.length)]).putString("lang", LANG_CODES[picked(langG, 5000, LANG_CODES.length)])
                        .putInt("res", picked(res, 1000, RES_LABELS.length)).putInt("sharpen", picked(sh, 2000, SHARPEN_LABELS.length))
                        .putInt("fps_limit", lims.get(picked(fpsLim, 8000, lims.size()))).apply();
                    cp.edit().putBoolean("fps", fps.isChecked()).putFloat("look", LOOK_GAINS[picked(look, 3000, LOOK_GAINS.length)])
                        .putFloat("move_dz", DEADZONES[picked(dz, 4000, DEADZONES.length)]).apply();
                    if (doomPad != null) doomPad.reloadSettings();
                    if (drsWas >= 0 && drs.isChecked() != (drsWas == 1)) {
                        boolean ok = false;
                        try { ok = nativeSetDrs(drs.isChecked()); } catch (Throwable t) { android.util.Log.e("dm", "nativeSetDrs: " + t); }
                        if (!ok) android.widget.Toast.makeText(this, R.string.toast_drs_failed, android.widget.Toast.LENGTH_LONG).show();
                    }
                    if (wideChanged) {   // [DOOM] BUGS_AND_FIXES 2.12: native answers whether it switched live (needs DM_DOOM_WIDE_LIVE=1)
                        boolean live = false;
                        try { live = libraryLoaded && nativeWideLive(wide.isChecked()); } catch (Throwable t) { android.util.Log.e("dm", "nativeWideLive: " + t); }
                        android.widget.Toast.makeText(this, live ? R.string.toast_wide_now : R.string.toast_wide_next, android.widget.Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton(R.string.gear_cancel, null).show();
        });
    }

    // [DOOM] After the game starts: removes the driver crash guard once a frame was presented, and tells the player once when the
    // chosen driver did not start and the native side fell back to the system driver (vk_driver.c status). Polls every second until
    // both are settled.
    private boolean driverGuardDone, driverStatusDone;
    private final float[] guardStats = new float[4];
    private final Runnable driverWatch = new Runnable() { @Override public void run() {
        if (!started || isDestroyed()) return;
        String[] st = driverStatus();
        if (!driverStatusDone && st != null) {
            driverStatusDone = true;
            if (!st[0].isEmpty() && "system".equals(st[1])) {
                android.util.Log.w("dm", "driver " + st[0] + " did not start (" + st[2] + "): system driver");
                android.widget.Toast.makeText(DmActivity.this, getString(R.string.toast_driver_fallback, driverLabel(st[0]), st[2]), android.widget.Toast.LENGTH_LONG).show();
                driverGuard().delete(); driverGuardDone = true;   // not a crash: the native side already chose the system driver
            }
        }
        if (!driverGuardDone) {
            try { nativeFrameStats(guardStats); } catch (Throwable t) { guardStats[0] = 0; }
            if (guardStats[0] > 0) { driverGuard().delete(); driverGuardDone = true; android.util.Log.i("dm", "driver guard: first frames presented, guard cleared"); }
        }
        if (!(driverGuardDone && driverStatusDone)) view.postDelayed(this, 1000);
    } };

    private void startHost() {
        if (bootFailure != 0) { showBootFailure(); return; }
        if (started || importing || !resumed || !ensureFileAccess()) return;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!DOOM && !libraryLoaded && !ensureImported(new File(downloads, "S2X/D2R Mobile/android"))) return;
        if (gameFrame.getParent() == null) setContentView(gameFrame);
        if (!libraryLoaded) {
            // 与 jni_android.c 选运行目录同一条规则：内部目录有主程序就用内部目录，否则用外部私有目录
            // Same rule as jni_android.c's choice of runtime directory: internal storage when it has the main module, otherwise external private storage
            File ext = getExternalFilesDir(null), internal = new File(getFilesDir(), "dm");
            File runtime = new File(internal, "out/main.patched.bin").canRead() || ext == null ? internal : new File(ext, "dm");
            try { Os.setenv("DM_DERIVED", new File(runtime, "derived.tsv").getAbsolutePath(), true); } catch (ErrnoException e) { showStartError(getString(R.string.dmAndroidStorage)); return; }
            if (DOOM) applyDoomSettings();
            try { System.loadLibrary("dm"); libraryLoaded = true; }
            catch (UnsatisfiedLinkError error) { showStartError(getString(R.string.dmAndroidLibrary)); return; }
            nativeContext(getApplicationContext());
        }
        if (DOOM && doomPad == null) { doomPad = new DoomPadView(this); gameFrame.addView(doomPad, new FrameLayout.LayoutParams(-1, -1)); updatePadVisibility(); }   // [DOOM] on-screen controller
        if (touchOverlay == null && !DOOM) {   // [DOOM] the D2R touch layout does not apply; a DOOM layout comes later
            int[] gameIds = {R.string.dmAndroidAttack, R.string.dmAndroidSkill1, R.string.dmAndroidSkill2, R.string.dmAndroidSkill3, R.string.dmAndroidSkill4,
                R.string.dmAndroidPortal, R.string.dmAndroidPotion1, R.string.dmAndroidPotion2, R.string.dmAndroidPotion3, R.string.dmAndroidPotion4,
                R.string.dmAndroidMenu, R.string.dmAndroidMap, R.string.dmAndroidMove, R.string.dmAndroidSwap, R.string.dmAndroidDifficulty};
            int[] navIds = {R.string.dmConfirm, R.string.dmAndroidBack, R.string.dmAndroidAction, R.string.dmAndroidBind, R.string.dmAndroidNext};
            String[] gameLabels = new String[gameIds.length], navLabels = new String[navIds.length];
            for (int i = 0; i < gameIds.length; i++) gameLabels[i] = getString(gameIds[i]);
            for (int i = 0; i < navIds.length; i++) navLabels[i] = getString(navIds[i]);
            touchOverlay = new TouchOverlay(this, gameLabels, navLabels);
            touchOverlay.setOnApplyWindowInsetsListener((v, insets) -> {
                android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.displayCutout());
                touchOverlay.setSafeInsets(safe.left, safe.right, safe.bottom);
                return insets;
            });
            gameFrame.addView(touchOverlay, new FrameLayout.LayoutParams(-1, -1));
            touchOverlay.requestApplyInsets();
        }
        if (!view.getHolder().getSurface().isValid() || view.getWidth() <= 0 || view.getHeight() <= 0) return;
        File ext = getExternalFilesDir(null);
        if (ext == null) { showStartError(getString(R.string.dmAndroidStorage)); return; }
        nativeSurface(view.getHolder().getSurface(), view.getWidth(), view.getHeight());
        updateRefresh();
        // 资源目录缺省 Download/S2X/D2R Mobile/data；没有这个目录、而早先的 Download/data 在时沿用早先的位置，已装好的手机不必搬数据。
        // 按目录判断而不按 .build.config：它也可以由导入的覆盖层提供，资源目录里就没有。
        // The resource directory defaults to Download/S2X/D2R Mobile/data; when that directory does not exist but the earlier
        // Download/data does, the earlier one is kept, so existing installs need not move their data. The test is the directory,
        // not .build.config: the imported overlay can provide that file, and then the resource directory has none.
        File data = new File(downloads, "S2X/D2R Mobile/data"), earlier = new File(downloads, "data");
        if (!data.isDirectory() && earlier.isDirectory()) data = earlier;
        String src = DOOM ? new File(downloads, "DOOM Mobile").getAbsolutePath() : data.getAbsolutePath();   // [DOOM] Download/DOOM Mobile/{romfs,dm}
        int result = nativeStart(getFilesDir().getAbsolutePath(), ext.getAbsolutePath(), src, Locale.getDefault().toLanguageTag());
        if (result < 0) {
            int resource = switch (result) {
                case -2 -> R.string.dmAndroidMissingData;
                case -3 -> R.string.dmAndroidMissingModules;
                case -4 -> R.string.dmAndroidMissingShaders;
                case -5 -> R.string.dmAndroidMissingConfig;
                case -9 -> R.string.dmAndroidMissingOverlay;
                case -10 -> R.string.dmAndroidInvalidOverlay;
                default -> 0;
            };
            showStartError(resource == 0 ? getString(R.string.dmAndroidStartFailed, result) : getString(resource)); return;
        }
        started = true;
        if (touchOverlay != null) touchOverlay.setInputEnabled(true);
        if (DOOM) {   // [DOOM] driver crash guard / fallback message (driverWatch), and the notice from the guard at this launch
            if (driverNotice != null) { android.widget.Toast.makeText(this, driverNotice, android.widget.Toast.LENGTH_LONG).show(); driverNotice = null; }
            view.postDelayed(driverWatch, 1000);
        }
        nativeFocus(3);
    }

    private void showStartError(String message) {
        TextView t = new TextView(this);
        t.setText(message); t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        setContentView(t);
    }

    // 原生线程只投递主线程工作，不持有 Activity，也不直接操作视图。
    // Native threads only post UI work; they neither retain the Activity nor touch views.
    private static void showNativeFailure(int code) {
        bootFailure = code;
        DmActivity activity = current.get();
        if (activity != null) activity.runOnUiThread(() -> {
            if (!activity.isDestroyed()) activity.showBootFailure();
        });
    }

    private void showBootFailure() {
        releaseTouches();
        showStartError(getString(bootFailure == -6 ? R.string.dmAndroidGraphicsFailed : R.string.dmAndroidHostStopped));
    }

    private static void showNativeInput(long id, String title, String sub, String guide, String initial,
                                        int maxChars, int nameRule, int numericMax) {
        DmActivity activity = current.get();
        if (activity == null) { nativeTextResult(id, false, ""); return; }
        activity.runOnUiThread(() -> activity.openInput(id, title, sub, guide, initial, maxChars, nameRule != 0, numericMax));
    }

    private void openInput(long id, String title, String sub, String guide, String initial,
                           int maxChars, boolean nameRule, int numericMax) {
        if (!resumed || isFinishing() || isDestroyed() || inputDialog != null) { nativeTextResult(id, false, ""); return; }
        releaseTouches();
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setInputType(numericMax >= 0 ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setHint(nameRule ? getString(R.string.dmNameBlankRandom) : guide);
        input.setText(TextInputRules.normalize(initial, maxChars, nameRule, numericMax));
        input.setSelection(input.length());
        String message = sub + (sub.isEmpty() || guide.isEmpty() ? "" : "\n") + guide;
        if (nameRule) message += (message.isEmpty() ? "" : "\n") + getString(R.string.dmNameRule);
        if (numericMax >= 0) message = getString(R.string.dmAndroidQuantityRange, numericMax);
        if (title.isEmpty()) title = getString(nameRule ? R.string.dmNameTitle : numericMax >= 0 ? R.string.dmQtyTitle : R.string.dmInputTitle);
        final boolean[] finished = {false};
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(title).setMessage(message).setView(input)
            .setPositiveButton(R.string.dmConfirm, null).setNegativeButton(R.string.dmCancel, (d, which) -> {})
            .create();
        inputDialog = dialog;
        dialog.setOnDismissListener(d -> {
            if (inputDialog == dialog) inputDialog = null;
            if (!finished[0]) { finished[0] = true; nativeTextResult(id, false, ""); }
            if (resumed) immersive();
        });
        dialog.setOnShowListener(d -> {
            Runnable check = () -> {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                int error = nameRule ? TextInputRules.nameError(input.getText().toString()) : 0;
                input.setError(composing ? getString(R.string.dmNamePickIme) : error == 1 ? getString(R.string.dmNameTooShort) : error == 2 ? getString(R.string.dmNameEdge) : null);
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(!composing && error == 0);
            };
            Runnable settle = () -> {
                Editable text = input.getText();
                if (BaseInputConnection.getComposingSpanStart(text) < 0) {
                    String normalized = TextInputRules.normalize(text.toString(), maxChars, nameRule, numericMax);
                    if (!normalized.contentEquals(text)) { input.setText(normalized); input.setSelection(input.length()); return; }
                }
                check.run();
            };
            // 输入法结束组字（点候选、按完成）时常常只去掉组字标记、文字不变：TextWatcher 收不到，「确定」一直灰、提示停在「先选完字」。
            // 组字状态一变就和文字变化一样处理（规整 + 重判）：SpanWatcher 挂在文字上，Editable.Factory 让每次 setText 换上的新文字也挂着它。
            // When the IME finishes composing (picking a candidate, Done) it often only removes the composing span and keeps the text: TextWatcher is not
            // called, so OK stays disabled with "finish choosing characters". A change of composing state is handled like a text change (normalize + check):
            // a SpanWatcher sits on the text, and an Editable.Factory attaches it to the new text after every setText.
            SpanWatcher composingWatch = new SpanWatcher() {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                private void changed(Spannable text) {
                    boolean now = BaseInputConnection.getComposingSpanStart(text) >= 0;
                    if (now != composing) { composing = now; input.post(settle); }
                }
                public void onSpanAdded(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanRemoved(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanChanged(Spannable text, Object what, int ostart, int oend, int nstart, int nend) { changed(text); }
            };
            input.setEditableFactory(new Editable.Factory() {
                @Override public Editable newEditable(CharSequence source) {
                    Editable e = super.newEditable(source);
                    e.setSpan(composingWatch, 0, e.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
                    return e;
                }
            });
            input.getText().setSpan(composingWatch, 0, input.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
            input.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                public void onTextChanged(CharSequence s, int start, int before, int count) {}
                public void afterTextChanged(Editable text) { settle.run(); }
            });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                finished[0] = true;
                nativeTextResult(id, true, input.getText().toString());
                dialog.dismiss();
            });
            check.run();
            input.requestFocus();
            if (dialog.getWindow() != null) dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        });
        dialog.show();
    }

    private void releaseTouches() {
        if (touchOverlay != null) touchOverlay.cancelTouches();
        if (started) nativeTouch(0, new int[0], new float[0], new float[0]);
    }

    private void updateRefresh() {
        if (!libraryLoaded) return;
        android.view.Display d = view.getDisplay();
        nativeRefresh(d == null ? 0 : d.getRefreshRate());
        if (d != null) nativeRotation(d.getRotation());
    }

    // 游戏手柄：手柄 / 摇杆 / 十字键来源（非虚拟设备）的按键与摇杆交原生层；原生层不认的键照常交给系统。
    // Game controllers: keys and sticks from gamepad / joystick / d-pad sources of a real (not virtual) device go to the native layer; a key
    // the native layer does not map goes on to the system as usual. The phone's own Back (a virtual keyboard source) is never taken.
    private static boolean controller(InputDevice d, int source) {
        if (d == null || d.isVirtual()) return false;
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
            || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (DOOM && e.getAction() == KeyEvent.ACTION_DOWN) android.util.Log.i("dm", "[input] java key " + e.getKeyCode() + " source 0x" + Integer.toHexString(e.getSource()) + " device " + e.getDeviceId() + " started " + started + " dialog " + (inputDialog != null));
        if (started && inputDialog == null && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
            && (controller(e.getDevice(), e.getSource())
                || (DOOM && ((e.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD || e.getSource() == 0) && e.getKeyCode() != KeyEvent.KEYCODE_BACK))   // [DOOM] keyboard too
            && nativePadKey(e.getDeviceId(), e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN)) return true;
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        int src = e.getSource();
        if (started && inputDialog == null && e.getAction() == MotionEvent.ACTION_MOVE && e.getDevice() != null && !e.getDevice().isVirtual()
            && (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            nativePadAxes(e.getDeviceId(), e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
    @Override public void onInputDeviceAdded(int id) { updatePadVisibility(); }
    @Override public void onInputDeviceChanged(int id) { updatePadVisibility(); }
    @Override public void onInputDeviceRemoved(int id) { if (libraryLoaded) nativePadRemoved(id); updatePadVisibility(); }

    // [DOOM] A real controller connected (external gamepad / joystick): the on-screen pad hides all but the settings button,
    // and comes back when the last one is disconnected.
    private static boolean physicalController(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }
    void updatePadVisibility() {
        if (doomPad == null) return;
        boolean any = false;
        for (int id : InputDevice.getDeviceIds()) if (physicalController(InputDevice.getDevice(id))) { any = true; break; }
        doomPad.setControllerMode(any);
    }
    @Override public void onDisplayAdded(int id) { updateRefresh(); }
    @Override public void onDisplayRemoved(int id) { updateRefresh(); }
    @Override public void onDisplayChanged(int id) { updateRefresh(); }
    @Override protected void onResume() { super.onResume(); resumed = true; immersive(); startHost(); updateRefresh(); if (started) { nativeFocus(3); if (touchOverlay != null && bootFailure == 0) touchOverlay.setInputEnabled(true); } }
    @Override protected void onPause() { resumed = false; if (inputDialog != null) inputDialog.dismiss(); releaseTouches(); if (touchOverlay != null) touchOverlay.setInputEnabled(false); if (started) nativeFocus(0); super.onPause(); }
    @Override protected void onStart() { super.onStart(); if (started) nativeFocus(2); }
    @Override protected void onStop() { if (started) nativeFocus(1); com.xiaoboluo.d2r.widget.HeroWidgetProvider.requestUpdate(this); super.onStop(); }
    @Override protected void onDestroy() { if (displays != null) displays.unregisterDisplayListener(this); if (inputs != null) inputs.unregisterInputDeviceListener(this); if (inputDialog != null) inputDialog.dismiss(); if (current.get() == this) current.clear(); super.onDestroy(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); else releaseTouches(); }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
        if (libraryLoaded) nativeSurface(h.getSurface(), w, ht);
        updateRefresh();
        startHost();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { releaseTouches(); if (libraryLoaded) nativeSurface(null, 0, 0); }

}
