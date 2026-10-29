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


    // [DOOM] Graphics options (gear button on the on-screen pad). Kept in SharedPreferences and handed to the native host as
    // environment variables before the library loads (hle_args.c turns them into the game's +set arguments, vulkan_present.cpp
    // reads the sharpening), so a change applies at the next start of the game.
    static final float[] RES_FRACTIONS = {1.0f, 0.85f, 0.75f, 0.67f};
    static final String[] RES_LABELS = {"100% (720p)", "85% (612p)", "75% (540p)", "67% (480p)"};
    static final String[] SHARPEN_STOPS = {"", "1.0", "0.5", "0.2"};
    static final String[] SHARPEN_LABELS = {"Выкл", "Слабая", "Средняя", "Сильная"};
    static final float[] AA_STRENGTHS = {0f, 0.5f, 1.0f, 1.5f, 2.0f};
    static final String[] AA_LABELS = {"Как на Switch", "0.5", "1", "1.5", "2"};
    static final String[] LANG_CODES = {"", "ru", "en-US"};
    static final String[] LANG_LABELS = {"Как в системе", "Русский", "English"};
    static final float[] LOOK_GAINS = {0.6f, 0.8f, 1.0f, 1.3f, 1.7f};
    static final String[] LOOK_LABELS = {"0.6", "0.8", "1.0", "1.3", "1.7"};
    static final float[] DEADZONES = {0f, 0.08f, 0.15f, 0.25f};
    static final String[] DEADZONE_LABELS = {"Нет", "Малая", "Средняя", "Большая"};
    static int nearest(float[] v, float x) { int b = 0; for (int i = 1; i < v.length; i++) if (Math.abs(v[i] - x) < Math.abs(v[b] - x)) b = i; return b; }

    private SharedPreferences doomPrefs() { return getSharedPreferences("doom_graphics", MODE_PRIVATE); }

    private void applyDoomSettings() {
        SharedPreferences p = doomPrefs();
        try {
            if (p.getBoolean("drs", false)) Os.setenv("DM_DOOM_DRS", "1", true); else Os.unsetenv("DM_DOOM_DRS");
            int res = p.getInt("res", 0);
            if (res > 0 && res < RES_FRACTIONS.length) Os.setenv("DM_DOOM_RES", String.valueOf(RES_FRACTIONS[res]), true); else Os.unsetenv("DM_DOOM_RES");
            int sh = p.getInt("sharpen", 0);
            if (sh > 0 && sh < SHARPEN_STOPS.length) Os.setenv("DM_GFX_SHARPEN", SHARPEN_STOPS[sh], true); else Os.unsetenv("DM_GFX_SHARPEN");
            // Vulkan driver (vk_driver.c): adrenotools packages under <data>/drivers/<name>/ (meta.json + the library) are copied to
            // private storage (a driver must be loaded from there); "turnip" chosen in the dialog sets DM_VK_DRIVER, which dm/env
            // can also set (DM_VK_DRIVER=<name>). The hooks adrenotools loads live in nativeLibraryDir.
            Os.setenv("DM_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            File drivers = new File(getFilesDir(), "drivers");
            Os.setenv("DM_DRIVERS_DIR", drivers.getAbsolutePath(), true);
            importDrivers(new File(Environment.getExternalStorageDirectory(), "Download/DOOM Mobile/drivers"), drivers);
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
            String drv = p.getString("driver", "");
            if (!drv.isEmpty()) Os.setenv("DM_VK_DRIVER", drv, true); else Os.unsetenv("DM_VK_DRIVER");
        } catch (ErrnoException e) { android.util.Log.e("dm", "doom settings: " + e); }
    }

    // Copy every driver package <src>/<name>/{meta.json, library} into <dst>/<name>/ when missing or of another size.
    private static void importDrivers(File src, File dst) {
        File[] dirs = src.listFiles(); if (dirs == null) return;
        for (File d : dirs) {
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

    void showDoomSettings() {
        runOnUiThread(() -> {
            SharedPreferences p = doomPrefs();
            LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
            CheckBox drs = new CheckBox(this); drs.setText("Динамическое разрешение DOOM (снижает 3D при нагрузке)"); drs.setChecked(p.getBoolean("drs", false)); box.addView(drs);
            CheckBox turnip = new CheckBox(this); turnip.setText("Драйвер Turnip (Mesa) вместо драйвера Qualcomm");
            turnip.setChecked("turnip".equals(p.getString("driver", ""))); box.addView(turnip);
            TextView langL = new TextView(this); langL.setText("Язык игры (тексты и озвучка, со следующего запуска):"); langL.setPadding(0, 24, 0, 0); box.addView(langL);
            RadioGroup langG = new RadioGroup(this); langG.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < LANG_CODES.length; i++) { RadioButton r = new RadioButton(this); r.setText(LANG_LABELS[i]); r.setId(5000 + i); langG.addView(r); }
            { String cur = p.getString("lang", ""); int li = 0; for (int i = 0; i < LANG_CODES.length; i++) if (LANG_CODES[i].equals(cur)) li = i; langG.check(5000 + li); }
            box.addView(langG);
            TextView aaL = new TextView(this); aaL.setText("Помощь в прицеливании (со следующего запуска):"); aaL.setPadding(0, 24, 0, 0); box.addView(aaL);
            RadioGroup aaG = new RadioGroup(this); aaG.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < AA_STRENGTHS.length; i++) { RadioButton r = new RadioButton(this); r.setText(AA_LABELS[i]); r.setId(6000 + i); aaG.addView(r); }
            aaG.check(6000 + nearest(AA_STRENGTHS, p.getFloat("aa", 1.0f))); box.addView(aaG);
            CheckBox wide = new CheckBox(this); wide.setText("Широкий экран: обзор шире вместо полос (со следующего запуска)"); wide.setChecked(p.getBoolean("wide", true)); box.addView(wide);
            CheckBox rumble = new CheckBox(this); rumble.setText("Вибрация (со следующего запуска)"); rumble.setChecked(p.getBoolean("rumble", false)); box.addView(rumble);
            CheckBox fps = new CheckBox(this); fps.setText("Счётчик FPS и времени кадра");
            fps.setChecked(getSharedPreferences("doom_controls", MODE_PRIVATE).getBoolean("fps", true)); box.addView(fps);
            TextView rl = new TextView(this); rl.setText("Разрешение 3D (интерфейс остаётся 720p):"); rl.setPadding(0, 24, 0, 0); box.addView(rl);
            RadioGroup res = new RadioGroup(this);
            for (int i = 0; i < RES_LABELS.length; i++) { RadioButton r = new RadioButton(this); r.setText(RES_LABELS[i]); r.setId(1000 + i); res.addView(r); }
            res.check(1000 + p.getInt("res", 0)); box.addView(res);
            TextView sl = new TextView(this); sl.setText("Резкость (FSR RCAS):"); sl.setPadding(0, 24, 0, 0); box.addView(sl);
            RadioGroup sh = new RadioGroup(this); sh.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < SHARPEN_LABELS.length; i++) { RadioButton r = new RadioButton(this); r.setText(SHARPEN_LABELS[i]); r.setId(2000 + i); sh.addView(r); }
            sh.check(2000 + p.getInt("sharpen", 0)); box.addView(sh);
            TextView note = new TextView(this); note.setText("Графика применится при следующем запуске игры, управление — сразу."); note.setPadding(0, 24, 0, 0);
            SharedPreferences cp = getSharedPreferences("doom_controls", MODE_PRIVATE);
            TextView ll = new TextView(this); ll.setText("Чувствительность обзора (свайп и кнопка огня):"); ll.setPadding(0, 24, 0, 0); box.addView(ll);
            RadioGroup look = new RadioGroup(this); look.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < LOOK_GAINS.length; i++) { RadioButton r = new RadioButton(this); r.setText(LOOK_LABELS[i]); r.setId(3000 + i); look.addView(r); }
            look.check(3000 + nearest(LOOK_GAINS, cp.getFloat("look", 1.0f))); box.addView(look);
            TextView dl = new TextView(this); dl.setText("Стик движения: компенсация мёртвой зоны игры:"); dl.setPadding(0, 24, 0, 0); box.addView(dl);
            RadioGroup dz = new RadioGroup(this); dz.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < DEADZONES.length; i++) { RadioButton r = new RadioButton(this); r.setText(DEADZONE_LABELS[i]); r.setId(4000 + i); dz.addView(r); }
            dz.check(4000 + nearest(DEADZONES, cp.getFloat("move_dz", 0f))); box.addView(dz);
            box.addView(note);
            android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(box);
            new AlertDialog.Builder(this).setTitle("Графика").setView(scroll)
                .setPositiveButton("Сохранить", (d, w) -> {
                    boolean wideChanged = wide.isChecked() != p.getBoolean("wide", true);
                    p.edit().putBoolean("drs", drs.isChecked()).putString("driver", turnip.isChecked() ? "turnip" : "").putBoolean("rumble", rumble.isChecked()).putBoolean("wide", wide.isChecked()).putFloat("aa", AA_STRENGTHS[Math.min(Math.max(0, aaG.getCheckedRadioButtonId() - 6000), AA_STRENGTHS.length - 1)]).putString("lang", LANG_CODES[Math.min(Math.max(0, langG.getCheckedRadioButtonId() - 5000), LANG_CODES.length - 1)])
                        .putInt("res", Math.max(0, res.getCheckedRadioButtonId() - 1000)).putInt("sharpen", Math.max(0, sh.getCheckedRadioButtonId() - 2000)).apply();
                    int li = Math.max(0, look.getCheckedRadioButtonId() - 3000), di = Math.max(0, dz.getCheckedRadioButtonId() - 4000);
                    getSharedPreferences("doom_controls", MODE_PRIVATE).edit().putBoolean("fps", fps.isChecked()).putFloat("look", LOOK_GAINS[Math.min(li, LOOK_GAINS.length - 1)])
                        .putFloat("move_dz", DEADZONES[Math.min(di, DEADZONES.length - 1)]).apply();
                    if (doomPad != null) doomPad.reloadSettings();
                    if (wideChanged) {   // [DOOM] BUGS_AND_FIXES 2.12: native answers whether it switched live (needs DM_DOOM_WIDE_LIVE=1)
                        boolean live = false;
                        try { live = libraryLoaded && nativeWideLive(wide.isChecked()); } catch (Throwable t) { android.util.Log.e("dm", "nativeWideLive: " + t); }
                        android.widget.Toast.makeText(this, live ? "Широкий экран: применено" : "Широкий экран: со следующего запуска", android.widget.Toast.LENGTH_SHORT).show();
                    }
                })
                .setNegativeButton("Отмена", null).show();
        });
    }

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
