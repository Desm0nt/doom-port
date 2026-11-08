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

public class DmActivity extends Activity implements SurfaceHolder.Callback, DisplayManager.DisplayListener, InputManager.InputDeviceListener, FrontMenu.Host, FrontMenu.Owner, GearHost {
    static final boolean DOOM = "doom".equals(BuildConfig.DM_GAME);
    private DoomPadView doomPad;
    static native int nativeStart(String filesDir, String extDir, String romfsSrc, String lang);
    static native void nativeSurface(Object surface, int w, int h);
    static native void nativeDisplaySize(int w, int h);   // [DOOM] the display's real size: the aspect fallback while the view has none (screen off at start)
    static native void nativeRefresh(float hz);
    static native void nativeFrameStats(float[] out);   // [DOOM] {fps, mean ms, max ms, GPU ms} over the last 0.5 s
    static native void nativeTouch(int n, int[] ids, float[] xs, float[] ys);
    static native void nativeFocus(int ev);
    static native void nativeTextResult(long id, boolean accepted, String text);
    // Platform services (jni_android.c): rumble needs the application Context; the motion sensors the display rotation; game controllers
    // (gamepad / joystick / d-pad sources) go to the native side, which answers whether the game took a key.
    // [front] The unified front end (frontend-common, docs/FRONTEND_UNIFIED.md; the same names and signatures as nx2android's front_android.c): the
    // pause menu holds the game (nativeHold), the graceful exit (nativeQuit), the Switch operation mode live (nativeOpmode / nativeSetOpmode)
    static native void nativeHold(boolean on);
    static native void nativeQuit();
    static native int nativeOpmode();
    static native void nativeSetOpmode(int mode);
    static final String FRONT_DATA_DIR = "DOOM Mobile";   // Download/<this>: the player's data folder; save exports go to its saves/ ([DOOM] found on any volume or chosen: GameFolder.java)
    private FrontMenu front;
    private SaveTransfer saves;
    static native void nativeContext(Object context);
    static native void nativeRotation(int rotation);
    static native boolean nativePadKey(int device, int keyCode, boolean down);
    static native void nativePadAxes(int device, float x, float y, float z, float rz, float lt, float rt, float hatX, float hatY);
    static native void nativePadRemoved(int device);
    static native boolean nativeLookDelta(float dxDp, float dyDp, float tiltSecPerDp);   // [DOOM] DoomPadView look -> the game's gyro aim channel (hle_hid.c); false = use the stick
    static native boolean nativeWideLive(boolean on);   // [DOOM] live widescreen switch (DM_DOOM_WIDE_LIVE=1); false = applies at the next launch
    static native int nativeGetDrs();                   // [DOOM] the game's rs_enable (hle_doom_drs.c): -1 not known yet, 0 off, 1 on
    static native boolean nativeSetDrs(boolean on);     // [DOOM] queue a live rs_enable change + profile save; false = cannot be written here
    static native boolean nativeSetLight(boolean on);   // [DOOM] r6 light mode (handheld quality preset) live; false = the game build lacks the patch
    static native int nativeR6Patches();                // [DOOM] 2.45: the loaded main has the v0.4.5 (r6) patches: 1 yes, 0 no (old game files), -1 not known yet
    static native void nativeSetDrsFps(int fps);        // [DOOM] r6 dynamic resolution target fps (hle_doom_drs.c)
    // [DOOM] the game's controller settings (hle_doom_settings.c, issue #31): GS_* ids; get -1 = not known yet; set false = not writable here
    static native int nativeGetGameSetting(int id);
    static native boolean nativeSetGameSetting(int id, int value);
    static final int GS_MOTION_AIM = 0, GS_GYRO_SENS = 1, GS_AIM_ASSIST = 2;
    static native String nativeDriverStatus();          // [DOOM] "<requested>\t<active>\t<reason>" (vk_driver.c), "" before the driver loads
    static native void nativeLog(String text);          // [DOOM] a "[java] ..." breadcrumb line into boot.log (jni_android.c)
    static native String nativeLogPath();               // [DOOM] this run's boot.log ("" before nativeStart)
    // [DOOM] crash logs (CrashLog.java, BUGS_AND_FIXES 2.22): breadcrumbs from before the native log exists wait in pendingCrumbs
    private static volatile boolean nativeLogReady;
    private static boolean crashHandlerInstalled;
    private static String lastStartError;
    private final java.util.ArrayList<String> pendingCrumbs = new java.util.ArrayList<>();

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
        if (BuildConfig.DM_DIAG) CrashLog.startLogcatCapture(getApplicationContext());   // [DOOM] diagnostics APK: logcat to a file from the first moment
        // 调试包：am start … --ez dmArtDump true 只把触屏控件底图写到 files/dm/artdump/ 就退出（dm/android/tools/art_compare.sh 用）
        // Debug builds: am start … --ez dmArtDump true only writes the touch control art to files/dm/artdump/ and exits (used by dm/android/tools/art_compare.sh)
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("dmArtDump", false)) {
            try { new TouchOverlay(this, null, null).dumpArt(new File(getFilesDir(), "dm/artdump")); } catch (IOException e) { android.util.Log.e("dm", "art dump: " + e); }
            finish(); return;
        }
        current = new WeakReference<>(this);
        if (DOOM) updates = new UpdateNotice(new Updates());   // [DOOM] 2.41: started at the first frames (VkDrivers.gameStarted callback)
        if (DOOM) checkLastRun();   // [DOOM] crash report of the last run, before anything of this one is written
        displays = getSystemService(DisplayManager.class);
        if (displays != null) displays.registerDisplayListener(this, null);
        inputs = getSystemService(InputManager.class);
        if (inputs != null) inputs.registerInputDeviceListener(this, null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // [DOOM] Android 10 (the minSdk 29 build) has no ALWAYS: SHORT_EDGES is the same in landscape
        getWindow().getAttributes().layoutInDisplayCutoutMode = android.os.Build.VERSION.SDK_INT >= 30
            ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        drivers = new VkDrivers(this);   // [front] the Vulkan driver choice, crash guard and the gear's driver list (frontend-common)
        if (DOOM) GearDisplay.applyDisplayMode(this, doomPrefs());   // [DOOM] frame rate limit above 60: ask for a display mode at a multiple of it
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        gameFrame = new FrameLayout(this);
        gameFrame.addView(view, new FrameLayout.LayoutParams(-1, -1));
        setContentView(gameFrame);
        immersiveBars();
        // [front] Back (the key, the gesture) opens / closes the pause menu; targetSdk 36 delivers it only through this callback on Android 13+
        if (DOOM && android.os.Build.VERSION.SDK_INT >= 33) getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::backPressed);
    }

    // ---- [front] FrontMenu.Host: what the pause menu asks of this port (frontend-common, docs/FRONTEND_UNIFIED.md)
    private void backPressed() { if (front != null && started) front.back(); else finish(); }
    @Override public void onBackPressed() { if (DOOM) backPressed(); else super.onBackPressed(); }
    @Override public FrontMenu frontMenu() { return front; }
    @Override public boolean ready() { return started; }
    @Override public void hold(boolean on) { if (!libraryLoaded) return; try { nativeHold(on); } catch (UnsatisfiedLinkError e) { android.util.Log.e("dm", "nativeHold missing: the game is not paused (" + e.getMessage() + ")"); } }
    @Override public void gameSettings() { showDoomSettings(); }
    @Override public boolean updates() { return updates != null; }   // [DOOM] 2.41: the pause menu's "Check for updates"
    @Override public void checkUpdates() { if (updates != null) updates.checkNow(); }
    @Override public FrontMenu.Saves saves() {
        if (saves == null && DOOM) {   // [DOOM] issue #11: exports go to <game folder>/saves (GameFolder), with SaveTransfer.forGame's fallback
            File pub = GameFolder.writable(this, "saves"), ext = getExternalFilesDir(null);
            saves = new SaveTransfer(this, new File(getFilesDir(), "save"), pub != null ? pub : ext != null ? new File(ext, "saves") : new File(GameFolder.defaultDir(), "saves"));
        }
        if (saves == null) saves = SaveTransfer.forGame(this, FRONT_DATA_DIR);
        return saves;
    }
    @Override public void quit() {
        android.util.Log.i("dm", "[java] quit from the pause menu");
        if (DOOM) CrashLog.sessionEnd(this);   // [DOOM] a clean end: no crash report at the next start
        try { if (libraryLoaded) nativeQuit(); } catch (UnsatisfiedLinkError e) { /* an older library: just end */ }
        if (DOOM && libraryLoaded) CrashLog.mirrorBootLogs(this);   // [DOOM] boot.log next to crash.txt (System.exit skips onStop)
        finishAndRemoveTask(); System.exit(0);
    }
    @Override public void immersive() { immersiveBars(); }
    // a save import asked for in the pause menu: done at the start, before the library loads and the game runs (SaveTransfer.applyPending)
    private void applyPendingImport() {
        String imp = ((SaveTransfer) saves()).applyPending();
        if (imp != null) android.widget.Toast.makeText(this, imp.startsWith("!") ? imp.substring(1) : imp, android.widget.Toast.LENGTH_LONG).show();
    }
    @Override public String gameName() { return "DOOM"; }
    // [DOOM] the mode row is the r6 light mode: the game's own Switch handheld quality preset, live (nativeSetLight); docked (full quality) stays the
    // default (BUGS_AND_FIXES 2.18); the gear dialog's "light mode" box is the same setting
    @Override public int modeKind() { return FrontMenu.MODE_LIGHT; }
    @Override public boolean docked() { return !doomPrefs().getBoolean("light", false); }
    @Override public boolean setDocked(boolean on) {
        boolean ok = false;
        try { ok = libraryLoaded && nativeSetLight(!on); } catch (Throwable t) { android.util.Log.e("dm", "nativeSetLight: " + t); }
        if (ok) doomPrefs().edit().putBoolean("light", !on).apply();
        android.util.Log.i("dm", "[java] mode " + (on ? "docked (full quality)" : "handheld (light)") + (ok ? "" : ": not switched"));
        return ok;
    }

    // ---- [front] GearHost: the shared gear dialog (frontend-common GearSettings / GearPages / VkDrivers / DriverManager, docs/FRONTEND_UNIFIED.md
    // "Gear", BUGS_AND_FIXES 2.44); DOOM's own rows and the order are DoomGear.java. The pause menu's "Game settings…" (gameSettings) still calls
    // showDoomSettings. gearUsesPortResources: every text is DOOM's own string resource in the system locale, as before the move.
    private VkDrivers drivers;   // made in onCreate (drivers = new VkDrivers(this)): the start's driver choice, the crash guard, the gear's list
    private final float[] gearStats = new float[4];
    @Override public android.app.Activity activity() { return this; }
    @Override public String dataDirName() { return FRONT_DATA_DIR; }
    @Override public SharedPreferences gamePrefs() { return doomPrefs(); }
    @Override public FrontMenu.Pad pad() { return doomPad; }
    @Override public FrontMenu front() { return front; }
    @Override public boolean libraryLoaded() { return libraryLoaded; }
    @Override public boolean started() { return started; }
    @Override public VkDrivers drivers() { return drivers; }
    @Override public java.util.List<GearSettings.Section> gearSections(GearSettings g) { return DoomGear.sections(this, g); }
    @Override public boolean gearUsesPortResources() { return true; }
    @Override public String driverStatus() { return nativeDriverStatus(); }
    @Override public float[] frameStats() { try { nativeFrameStats(gearStats); } catch (Throwable t) { gearStats[0] = 0; } return gearStats; }
    @Override public File driverImportDir() { return new File(GameFolder.dir(this), "drivers"); }   // [DOOM] <game folder>/drivers (issue #11)
    // [DOOM] diagnostics APK with -PdmDiagDriver=<pkg> (diag3/4): always that driver on an Adreno 6xx+, whatever was saved or the crash guard decided
    // before (VkDrivers.choose drops the guard file). Without it (diag6, owner 06.11) the saved choice and the crash guard work as in a release build.
    @Override public String forcedDriver() { return BuildConfig.DM_DIAG && !BuildConfig.DM_DIAG_DRIVER.isEmpty() && VkDrivers.turnipPossible() ? BuildConfig.DM_DIAG_DRIVER : null; }
    void showDoomSettings() { new GearSettings(this).show(); }
    UpdateNotice updateNotice() { return updates; }   // [DOOM] 2.41: the gear's "Updates" rows (GearPages.updates)
    // CrashLog.java's helpers, now VkDrivers'
    static String hardwareSummary() { return VkDrivers.hardwareSummary(); }
    static String readSmall(File f) throws IOException { return VkDrivers.readSmall(f); }

    // ---- [DOOM] crash logs and breadcrumbs (CrashLog.java, BUGS_AND_FIXES 2.22)
    // A "[java] ..." line in boot.log (and logcat); kept until the native log exists (nativeStart opens it).
    @Override public void crumb(String s) {
        if (nativeLogReady) { try { nativeLog(s); return; } catch (Throwable t) { /* an older library */ } }
        android.util.Log.i("dm", "[java] " + s);
        if (!nativeLogReady && pendingCrumbs.size() < 64) pendingCrumbs.add(s);
    }
    private void flushCrumbs() { java.util.ArrayList<String> l = new java.util.ArrayList<>(pendingCrumbs); pendingCrumbs.clear(); for (String s : l) crumb(s); }
    private static File currentLog(android.content.Context c) {
        if (nativeLogReady) try { String p = nativeLogPath(); if (p != null && !p.isEmpty()) return new File(p); } catch (Throwable t) { /* below */ }
        return CrashLog.bootLog(c);
    }
    // Once per process: the Java exception handler (report at once, then the system's handler) and the check of the last run.
    private void checkLastRun() {
        final android.content.Context app = getApplicationContext();
        if (!crashHandlerInstalled) {
            crashHandlerInstalled = true;
            final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
                try {
                    String what = "Java exception on thread \"" + t.getName() + "\": " + e;
                    if (nativeLogReady) try { nativeLog(what); } catch (Throwable x) { /* still write the report */ }
                    CrashLog.writeNow(app, what, CrashLog.stackTrace(e), currentLog(app));
                    CrashLog.sessionEnd(app);
                } catch (Throwable x) { android.util.Log.e("dm", "crash log: " + x); }
                if (prev != null) prev.uncaughtException(t, e); else { android.os.Process.killProcess(android.os.Process.myPid()); System.exit(10); }
            });
            if (BuildConfig.DM_DIAG && !diagWiped) { diagWiped = true; CrashLog.wipeForDiag(app); }   // [DOOM] diagnostics APK: a clean slate
            String last = CrashLog.checkPrevious(app, (f, why) -> runOnUiThread(() -> {
                if (f == null) return;
                crumb("crash report of the last run written: " + f.getAbsolutePath());
                android.widget.Toast.makeText(app, Ui.t(app, "Прошлый запуск DOOM завершился неожиданно. Отчёт: ", "The last DOOM run ended unexpectedly. Report: ")
                    + f.getAbsolutePath(), android.widget.Toast.LENGTH_LONG).show();
            }));
            if (last != null) crumb(last);
        }
    }
    // A start / boot failure screen: the report now (the screen names it), once per message and process.
    private File startErrorReport(String message) {
        File f = CrashLog.report(this);
        if (message.equals(lastStartError)) return f;
        lastStartError = message;
        crumb("start error shown: " + message.replace('\n', ' '));
        final android.content.Context app = getApplicationContext(); final File log = currentLog(this);
        CrashLog.sessionEnd(this);   // reported now, not again at the next start
        new Thread(() -> {
            try { Thread.sleep(300); } catch (InterruptedException e) { /* write now */ }   // the last native lines reach boot.log
            CrashLog.writeNow(app, "start error: " + message, null, log);
        }, "dm-crashlog").start();
        return f;
    }
    @Override public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        // 5 / 10 / 15 running moderate / low / critical, 20 UI hidden, 40+ background (the constants below 20 are deprecated in API 34)
        if (DOOM && level >= 5) crumb("trim memory level " + level + (started ? "" : " (game not started)"));
    }

    @SuppressWarnings("deprecation")
    private void immersiveBars() {
        if (android.os.Build.VERSION.SDK_INT < 30) {   // [DOOM] Android 10 (the minSdk 29 build): the flags the insets controller replaced
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }

    // 资源在玩家放的位置读（缺省 Download/S2X/D2R Mobile/data），要「所有文件访问」权限；没有就引导去系统设置页，回来再启动
    // Resources are read where the player put them (default Download/S2X/D2R Mobile/data), which needs "all files access"; without it, guide the player to the system settings page and start after returning
    // [DOOM] Android 10 (the minSdk 29 build) has no all files access: the storage permission with requestLegacyExternalStorage reads the same
    // folders. It is asked for once by itself; a tap asks again, or opens the app's settings page once the player chose "don't ask again"
    private boolean legacyStorageAsked;
    private static final String[] LEGACY_STORAGE = {android.Manifest.permission.READ_EXTERNAL_STORAGE, android.Manifest.permission.WRITE_EXTERNAL_STORAGE};
    private boolean ensureFileAccess() {
        if (SaveTransfer.allFilesAccess(this)) return true;
        boolean legacy = android.os.Build.VERSION.SDK_INT < 30;
        if (legacy && !legacyStorageAsked) { legacyStorageAsked = true; requestPermissions(LEGACY_STORAGE, 1); }
        TextView t = new TextView(this);
        t.setText(DOOM ? R.string.doom_start_permission : R.string.dmAndroidPermission);   // [DOOM] the shared text names d2r's Download/S2X/D2R Mobile
        t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        t.setOnClickListener(v -> {
            if (!legacy) startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
            else if (shouldShowRequestPermissionRationale(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)) requestPermissions(LEGACY_STORAGE, 1);
            else startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
        });
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

    static long treeSize(File f) {
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


    // [front] The gear (settings) dialog is the shared one (frontend-common GearSettings, docs/FRONTEND_UNIFIED.md "Gear"): the generic rows
    // are GearPages (frame rate limit, sharpening, upscale, Vulkan driver + DriverManager, game language, vibration, look, dead zone, FPS
    // counter, menu button, on-screen controls), the display modes GearDisplay, the driver choice / crash guard VkDrivers; DOOM's own rows
    // (dynamic resolution, light mode, aspect ratio, aim assist + the game's controller settings, game folder) and the order are DoomGear.java.
    SharedPreferences doomPrefs() { return getSharedPreferences("doom_graphics", MODE_PRIVATE); }

    private static boolean diagWiped;   // [DOOM] diagnostics APK: CrashLog.wipeForDiag looked at this process already
    // [DOOM] the diagnostics APK: boot.log copied to <game folder>/logs every minute while the game runs (a hard kill or a hang keeps the
    // last minute of the log reachable without adb), on a background thread
    private final Runnable diagMirror = new Runnable() { @Override public void run() {
        if (!started || isFinishing()) return;
        android.content.Context app = getApplicationContext(); new Thread(() -> CrashLog.mirrorBootLogs(app), "dm-logcopy").start();
        view.postDelayed(this, 60_000);
    } };

    private void startHost() {
        if (bootFailure != 0) { showBootFailure(); return; }
        if (started || importing || !resumed || !ensureFileAccess()) return;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!DOOM && !libraryLoaded && !ensureImported(new File(downloads, "S2X/D2R Mobile/android"))) return;
        // [DOOM] issue #11: find the game folder (GameFolder: internal storage, SD cards, USB drives, the player's choice) before anything
        // of this launch is set up; none complete = the start error with the searched paths and "Choose the game folder…", the library is
        // not loaded, so a folder chosen there simply runs this again.
        if (DOOM && !libraryLoaded) {
            GameFolder.Result r = GameFolder.resolve(this);
            logFolder(r);
            if (r.dir == null) { showFolderError(r.best() == GameFolder.NO_SHADERS ? -4 : r.best() == GameFolder.NO_MODULES ? -3 : -2, r); return; }
            if (!r.saved.isEmpty() && r.savedState != GameFolder.OK && !folderNoticeShown) {
                folderNoticeShown = true;
                android.widget.Toast.makeText(this, getString(R.string.folder_unavailable, r.saved, folderWhy(r.savedState), r.dir.getAbsolutePath()), android.widget.Toast.LENGTH_LONG).show();
            }
        }
        if (gameFrame.getParent() == null) setContentView(gameFrame);
        if (!libraryLoaded) {
            // 与 jni_android.c 选运行目录同一条规则：内部目录有主程序就用内部目录，否则用外部私有目录
            // Same rule as jni_android.c's choice of runtime directory: internal storage when it has the main module, otherwise external private storage
            File ext = getExternalFilesDir(null), internal = new File(getFilesDir(), "dm");
            File runtime = new File(internal, "out/main.patched.bin").canRead() || ext == null ? internal : new File(ext, "dm");
            try { Os.setenv("DM_DERIVED", new File(runtime, "derived.tsv").getAbsolutePath(), true); } catch (ErrnoException e) { showStartError(getString(R.string.dmAndroidStorage)); return; }
            if (DOOM) { applyPendingImport(); GearSettings.applyEnv(this); }   // [front] a requested save import first
            try {   // [DOOM] the app version for boot.log's first lines (jni_android.c log_header)
                android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
                Os.setenv("DM_APP_VERSION", getPackageName() + " " + pi.versionName + " (" + pi.getLongVersionCode() + ")" + (BuildConfig.DEBUG ? " debug" : ""), true);
            } catch (Exception e) { /* only a log line */ }
            try { System.loadLibrary("dm"); libraryLoaded = true; }
            catch (UnsatisfiedLinkError error) { showStartError(getString(R.string.dmAndroidLibrary)); return; }
            nativeContext(getApplicationContext());
        }
        if (DOOM && doomPad == null) { doomPad = new DoomPadView(this, DoomPadView.PRESET_GAME, PadSkin.CLASSIC, KeyEvent.KEYCODE_E);
            // [DOOM] 2.45 (from Eternal's anrfix, docs/port-kb APP-01): the pad is drawn on its own surface and thread (frontend-common PadSurface.java),
            // above the game's surface and below the window; the UI thread then draws nothing while the game runs and cannot be held by HWUI waiting
            // for a GPU our Vulkan work occupies (ANR "Input dispatching timed out"). Touches still go to doomPad on the window, as before.
            // Old drawing on the window: DM_PAD_SURFACE=0 in dm/env (applied after nativeStart, padSurfaceEnv) or --ez dmPadHwui true;
            // test: --ei dmPadStallMs N / DM_PAD_STALL_MS=N = the pad's draw thread sleeps N ms holding a buffer (at most every 15 s).
            boolean hwui = getIntent().getBooleanExtra("dmPadHwui", false);
            PadSurface.stallMs = Math.max(0, getIntent().getIntExtra("dmPadStallMs", 0));
            if (!hwui) gameFrame.addView(doomPad.attachSurface(), new FrameLayout.LayoutParams(-1, -1));
            crumb("pad drawn " + (hwui ? "on the window (HWUI, dmPadHwui)" : "on its own surface (draw thread dm-pad-draw)") + (PadSurface.stallMs > 0 ? ", test stall " + PadSurface.stallMs + " ms" : ""));
            gameFrame.addView(doomPad, new FrameLayout.LayoutParams(-1, -1)); updatePadVisibility();   // [DOOM] on-screen controller
            front = new FrontMenu(this, this, doomPad, gameFrame); }   // [front] the pause menu over it (the gear button opens it)
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
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.displayCutout());
                    touchOverlay.setSafeInsets(safe.left, safe.right, safe.bottom);
                } else {   // [DOOM] Android 10 (the minSdk 29 build)
                    android.view.DisplayCutout c = insets.getDisplayCutout();
                    if (c == null) touchOverlay.setSafeInsets(0, 0, 0); else touchOverlay.setSafeInsets(c.getSafeInsetLeft(), c.getSafeInsetRight(), c.getSafeInsetBottom());
                }
                return insets;
            });
            gameFrame.addView(touchOverlay, new FrameLayout.LayoutParams(-1, -1));
            touchOverlay.requestApplyInsets();
        }
        if (!view.getHolder().getSurface().isValid() || view.getWidth() <= 0 || view.getHeight() <= 0) return;
        File ext = getExternalFilesDir(null);
        if (ext == null) { showStartError(getString(R.string.dmAndroidStorage)); return; }
        if (DOOM) { android.view.Display d = GearDisplay.display(this);   // the real display size first: a game started with the screen off has a 0x0 view
            if (d != null) { android.graphics.Point p = new android.graphics.Point(); d.getRealSize(p); nativeDisplaySize(p.x, p.y); crumb("display " + p.x + "x" + p.y + ", view " + view.getWidth() + "x" + view.getHeight()); } }
        nativeSurface(view.getHolder().getSurface(), view.getWidth(), view.getHeight());
        updateRefresh();
        // 资源目录缺省 Download/S2X/D2R Mobile/data；没有这个目录、而早先的 Download/data 在时沿用早先的位置，已装好的手机不必搬数据。
        // 按目录判断而不按 .build.config：它也可以由导入的覆盖层提供，资源目录里就没有。
        // The resource directory defaults to Download/S2X/D2R Mobile/data; when that directory does not exist but the earlier
        // Download/data does, the earlier one is kept, so existing installs need not move their data. The test is the directory,
        // not .build.config: the imported overlay can provide that file, and then the resource directory has none.
        File data = new File(downloads, "S2X/D2R Mobile/data"), earlier = new File(downloads, "data");
        if (!data.isDirectory() && earlier.isDirectory()) data = earlier;
        String src = DOOM ? GameFolder.dir(this).getAbsolutePath() : data.getAbsolutePath();   // [DOOM] <game folder>/{romfs,dm} (GameFolder, issue #11)
        if (DOOM) CrashLog.session(this, "fg");   // [DOOM] the game runs from here: a leftover marker at the next start = it did not end cleanly
        int result = nativeStart(getFilesDir().getAbsolutePath(), ext.getAbsolutePath(), src, Locale.getDefault().toLanguageTag());
        if (DOOM) { nativeLogReady = true; flushCrumbs(); crumb("nativeStart " + result); }
        if (result < 0) {
            int resource = switch (result) {
                // [DOOM] its own texts: the shared ones name d2r's Download/S2X/D2R Mobile and d2rm_android.py
                case -2 -> DOOM ? R.string.doom_start_missing_data : R.string.dmAndroidMissingData;
                case -3 -> DOOM ? R.string.doom_start_missing_modules : R.string.dmAndroidMissingModules;
                case -4 -> DOOM ? R.string.doom_start_missing_shaders : R.string.dmAndroidMissingShaders;
                case -5 -> R.string.dmAndroidMissingConfig;
                case -9 -> R.string.dmAndroidMissingOverlay;
                case -10 -> R.string.dmAndroidInvalidOverlay;
                default -> 0;
            };
            // [DOOM] the Java check passed but the native one did not: the same screen with the searched paths and the folder button
            if (DOOM && result >= -4 && result <= -2) { showFolderError(result, GameFolder.resolve(this)); return; }
            showStartError(resource == 0 ? getString(R.string.dmAndroidStartFailed, result) : getString(resource)); return;
        }
        started = true;
        if (DOOM) padSurfaceEnv();   // [DOOM] 2.45: dm/env is read by nativeStart, so its pad switches apply from here
        if (touchOverlay != null) touchOverlay.setInputEnabled(true);
        if (DOOM) {   // [front] the driver notice from the start, then the crash guard / fallback / first frames watch (VkDrivers.gameStarted)
            drivers.gameStarted(view, () -> { if (updates != null) updates.start(); });   // [DOOM] 2.41: the update / game-files checks at the first frames
            if (BuildConfig.DM_DIAG) view.postDelayed(diagMirror, 60_000);
        }
        nativeFocus(3);
    }

    private void showStartError(String message) { showStartError(message, false); }
    private void showStartError(String message, boolean folderButton) {
        // [DOOM] every error screen writes a report at once and names it (CrashLog.java)
        if (DOOM) message += "\n\n" + Ui.t(this, "Журнал для отчёта об ошибке: ", "Log for a bug report: ") + startErrorReport(message).getAbsolutePath();
        TextView t = new TextView(this);
        t.setText(message); t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        if (!folderButton) { setContentView(t); return; }
        // [DOOM] issue #11: the game files are missing or incomplete: "Choose the game folder…" under the text
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.addView(t);
        android.widget.Button b = new android.widget.Button(this); b.setText(R.string.folder_choose_start); b.setOnClickListener(v -> showFolderDialog(null));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.setMargins(64, 0, 64, 64); box.addView(b, lp);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(box);
        setContentView(scroll);
    }

    // ---- [DOOM] the game folder (GameFolder.java, GitHub issue #11, BUGS_AND_FIXES 2.28)
    private static final int REQ_FOLDER = 4711;
    private static String folderLogged;      // the last resolution written to the log (once per change, startHost runs often)
    private boolean folderNoticeShown;
    private AlertDialog folderDialog;
    private Runnable folderRowUpdate;         // the gear dialog's "Game folder" line, refreshed after a choice
    private void logFolder(GameFolder.Result r) {
        String s = r.log();
        if (s.equals(folderLogged)) return;
        folderLogged = s;
        for (String l : s.split("\n")) crumb(l);
    }
    String folderWhy(int state) {
        switch (state) {
            case GameFolder.OK: return getString(R.string.folder_why_ok);
            case GameFolder.MISSING: return getString(R.string.folder_why_missing);
            case GameFolder.UNREADABLE: return getString(R.string.folder_why_unreadable);
            case GameFolder.NO_ROMFS: return getString(R.string.folder_why_romfs);
            case GameFolder.NO_MODULES: return getString(R.string.folder_why_modules);
            default: return getString(R.string.folder_why_shaders);
        }
    }
    // The start error for -2 / -3 / -4 with the folders that were searched and what each lacks
    private void showFolderError(int code, GameFolder.Result r) {
        StringBuilder sb = new StringBuilder(getString(code == -4 ? R.string.doom_start_missing_shaders : code == -3 ? R.string.doom_start_missing_modules : R.string.doom_start_missing_data));
        sb.append("\n\n").append(getString(R.string.folder_searched));
        for (GameFolder.Candidate c : r.all) sb.append("\n• ").append(c.dir.getAbsolutePath()).append(" — ").append(folderWhy(c.state));
        showStartError(sb.toString(), true);
    }
    // "<path> (found automatically / your choice)" and, when another folder was chosen for the next launch, a second line
    String folderLine() {
        GameFolder.Result r = GameFolder.last(); String saved = GameFolder.saved(this);
        String line = r == null || r.dir == null ? getString(R.string.folder_none)
            : getString(r.how.equals("saved choice") ? R.string.folder_custom_fmt : R.string.folder_auto_fmt, r.dir.getAbsolutePath());
        String used = r == null ? "" : r.saved;
        if (started && !saved.equals(used)) line += "\n" + getString(R.string.folder_next_fmt, saved.isEmpty() ? getString(R.string.folder_next_auto) : saved);
        return line;
    }
    // The chooser: the system folder picker, a typed path, or back to the automatic search. onDone refreshes the gear row (or null).
    void showFolderDialog(Runnable onDone) {
        folderRowUpdate = onDone;
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
        TextView info = new TextView(this);
        info.setText(folderLine() + "\n\n" + getString(R.string.folder_help) + "\n\n" + getString(R.string.folder_sd_warning));
        box.addView(info);
        android.widget.Button browse = new android.widget.Button(this); browse.setText(R.string.folder_browse); box.addView(browse);
        EditText path = new EditText(this); path.setSingleLine(true); path.setHint(R.string.folder_path_hint);
        path.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_VARIATION_URI);
        String saved = GameFolder.saved(this); GameFolder.Result r = GameFolder.last();
        path.setText(!saved.isEmpty() ? saved : r != null && r.dir != null ? r.dir.getAbsolutePath() : GameFolder.defaultDir().getAbsolutePath());
        box.addView(path);
        android.widget.Button use = new android.widget.Button(this); use.setText(R.string.folder_use_path); box.addView(use);
        android.widget.Button reset = new android.widget.Button(this); reset.setText(R.string.folder_reset); box.addView(reset);
        reset.setEnabled(!saved.isEmpty());
        android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(box);
        AlertDialog d = new AlertDialog.Builder(this).setTitle(R.string.folder_title).setView(scroll).setNegativeButton(R.string.gear_cancel, null).create();
        folderDialog = d;
        d.setOnDismissListener(x -> { if (folderDialog == d) folderDialog = null; if (resumed) immersiveBars(); });
        browse.setOnClickListener(v -> openFolderPicker());
        use.setOnClickListener(v -> {
            String p = path.getText().toString().trim();
            if (p.isEmpty()) return;
            if (!p.startsWith("/")) p = new File(android.os.Environment.getExternalStorageDirectory(), p).getAbsolutePath();   // "Download/DOOM Mobile"
            applyFolderChoice(new File(p));
        });
        reset.setOnClickListener(v -> {
            crumb("game folder: reset to the automatic search (was " + GameFolder.saved(this) + ")");
            GameFolder.prefs(this).edit().remove(GameFolder.PREF).apply();
            folderApplied(null);
        });
        d.show();
    }
    private void openFolderPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI,
            android.provider.DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:" + Environment.DIRECTORY_DOWNLOADS));
        try { startActivityForResult(i, REQ_FOLDER); }
        catch (android.content.ActivityNotFoundException e) { crumb("game folder picker: none on this phone"); android.widget.Toast.makeText(this, R.string.folder_no_picker, android.widget.Toast.LENGTH_LONG).show(); }
    }
    @SuppressWarnings("deprecation")
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == DriverManager.REQ_ZIP) { DriverManager.picked(this, result == RESULT_OK && data != null ? data.getData() : null); return; }   // [DOOM] 2.39
        if (request != REQ_FOLDER) return;
        Uri u = result == RESULT_OK && data != null ? data.getData() : null;
        if (u == null) { crumb("game folder picker: cancelled"); return; }
        File f = GameFolder.fromTreeUri(this, u);
        crumb("game folder picker: " + u + " -> " + (f != null ? f.getAbsolutePath() : "not a storage path"));
        if (f == null) { new AlertDialog.Builder(this).setTitle(R.string.folder_title).setMessage(R.string.folder_not_path).setPositiveButton(android.R.string.ok, null).show(); return; }
        applyFolderChoice(f);
    }
    // A chosen folder (or the folder that holds "DOOM Mobile"): saved only when complete; otherwise the player is told what is missing
    private void applyFolderChoice(File picked) {
        File d = GameFolder.normalize(picked);
        int s = GameFolder.check(this, d);
        crumb("game folder: the player chose " + picked.getAbsolutePath() + (d.equals(picked) ? "" : " -> " + d.getAbsolutePath()) + ": " + GameFolder.stateName(s));
        if (s != GameFolder.OK) {
            new AlertDialog.Builder(this).setTitle(R.string.folder_title).setMessage(getString(R.string.folder_bad, d.getAbsolutePath(), folderWhy(s))
                + (GameFolder.removable(d) ? "\n\n" + getString(R.string.folder_sd_warning) : "")).setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        GameFolder.prefs(this).edit().putString(GameFolder.PREF, d.getAbsolutePath()).apply();
        if (GameFolder.removable(d)) android.widget.Toast.makeText(this, R.string.folder_sd_warning, android.widget.Toast.LENGTH_LONG).show();
        folderApplied(d);
    }
    // After a change (d = the new folder, null = automatic): the running game keeps its folder until the next launch; before the library
    // loads the start simply runs again; after a failed nativeStart (native paths already set) the app restarts in a new process.
    private void folderApplied(File d) {
        if (folderDialog != null) folderDialog.dismiss();
        String what = d != null ? d.getAbsolutePath() : getString(R.string.folder_next_auto);
        if (started) {
            android.widget.Toast.makeText(this, getString(R.string.folder_set_next, what), android.widget.Toast.LENGTH_LONG).show();
            if (folderRowUpdate != null) folderRowUpdate.run();
            return;
        }
        android.widget.Toast.makeText(this, getString(R.string.folder_set_now, what), android.widget.Toast.LENGTH_SHORT).show();
        if (libraryLoaded) { restartApp(); return; }
        GameFolder.forget(); lastStartError = null;
        startHost();   // not resumed yet (back from the picker): onResume runs it
    }
    private void restartApp() {
        crumb("restart in a new process for the new game folder");
        CrashLog.sessionEnd(this);   // not a crash
        if (libraryLoaded) CrashLog.mirrorBootLogs(this);
        startActivity(new Intent(this, DmRestart.class).putExtra("pid", android.os.Process.myPid()));
    }

    // ---- [DOOM] 2.41: "a new version is out" and "re-run setup" notices (UpdateNotice.java, the same file in frontend-common/java). Started once
    // the game shows its first frames (VkDrivers.gameStarted's callback in startHost); the gear has the status line and "Check for new versions
    // at start" (GearPages.updates); "Check for updates" is the pause menu's row (FrontMenu.R_UPDATES).
    private UpdateNotice updates;
    private static boolean baselineLogged;
    static final String UPDATE_REPO = "Desm0nt/doom-port";
    // patch_nso.py SHA-256s (LF and CRLF checkouts) of every public patcher before v0.4.5, the last patch-set change: dm/out/build_src.sha256
    // naming one of them = game files from setup up to v0.4.4 = version 0 (game_files.json history); any other or none = version 1
    static final java.util.Set<String> PRE_045_PATCHERS = new java.util.HashSet<>(java.util.Arrays.asList(
        "68bccd226422ac8627da9d5a4be5091c2294fdf4e84b189fcf5e2cf61c340850", "ecc32011757987974214a7a044998b32b94296c6b3d30a8969fc12b3d8582a35",   // 1d41275 10-30
        "0e9ee9d4bf6dfe9788c1a4220c8b859d7101eab9d7215da87f29e86596d5d030", "4fc54a159fe592af64b47454452d0853bf201267a1a7cf64ec6059f765fef12c",   // c48e3d8 10-30
        "a645dfd9152f15046053286a7b344ddca91b988cfb3b8df4b249b5931bc5dd26", "fb6d73c43507933ec9651b34680e565dda46b77ca1dc0b9dd53e5e579a6391c9",   // df81554 10-30
        "4d8fa0d0013ce438c77f493022689b9bd732cd5aded41f7c943e968fe20bff3d", "5e0d2df184e3253ac6c0863749f7dbad58a678693bbcb372adce94eafcdc8b4e",   // 4a27fdb 10-29
        "df4229fc0c33f98c4c50ad45acc46bc9d9c9ecc173f29d60d752dba2a746a91b", "2b8b89f854e05b76421be7128292bc0839c9b9d8282ac298e0b5c6935ba89296")); // 7878bbb 10-29
    // gf_features_v<N> per game-files version (index = version; 0 has none): what re-running setup brings
    static final int[] GF_FEATURES = {0, R.string.gf_features_v1};
    private static final String[] UPD_KEYS = {"upd_heading", "upd_version_line", "upd_check_now", "upd_check_start", "upd_checking", "upd_title", "upd_message",
        "upd_whats_new", "upd_open", "upd_close", "upd_latest", "upd_failed", "upd_err_offline", "upd_err_limit", "gf_title", "gf_message", "gf_howto", "gf_note",
        "gf_guide", "gf_line_ok", "gf_line_nostamp", "gf_line_old", "gf_line_none"};
    private static final int[] UPD_IDS = {R.string.upd_heading, R.string.upd_version_line, R.string.upd_check_now, R.string.upd_check_start, R.string.upd_checking,
        R.string.upd_title, R.string.upd_message, R.string.upd_whats_new, R.string.upd_open, R.string.upd_close, R.string.upd_latest, R.string.upd_failed,
        R.string.upd_err_offline, R.string.upd_err_limit, R.string.gf_title, R.string.gf_message, R.string.gf_howto, R.string.gf_note, R.string.gf_guide,
        R.string.gf_line_ok, R.string.gf_line_nostamp, R.string.gf_line_old, R.string.gf_line_none};
    private final class Updates implements UpdateNotice.Host {
        @Override public Activity activity() { return DmActivity.this; }
        @Override public String appName() { return FRONT_DATA_DIR; }
        @Override public String repoSlug() { return UPDATE_REPO; }
        @Override public String currentVersion() { return BuildConfig.VERSION_NAME; }
        @Override public int requiredGameFiles() { return BuildConfig.DM_GAME_FILES; }
        @Override public File gameFilesStamp() { File d = GameFolder.dir(DmActivity.this); return d == null ? null : new File(GameFolder.runtime(DmActivity.this, d), "setup-stamp.json"); }
        @Override public int gameFilesBaseline() {
            File d = GameFolder.dir(DmActivity.this); if (d == null) return 1;
            File f = new File(GameFolder.runtime(DmActivity.this, d), "out/build_src.sha256");
            String s = readText(f), sha = s == null || s.trim().isEmpty() ? "" : s.trim().split("\\s+")[0].toLowerCase(Locale.ROOT);
            // [DOOM] 2.45: what the loaded game really has wins over the patcher hash (an unpublished patcher, e.g. a dev build, is in no list)
            int r6 = -1; try { r6 = nativeR6Patches(); } catch (UnsatisfiedLinkError e) { /* an older library */ }
            boolean old = r6 == 0 || (r6 < 0 && PRE_045_PATCHERS.contains(sha));
            if (!baselineLogged) { baselineLogged = true; crumb("game files: no stamp; " + f + (sha.isEmpty() ? " missing" : " patch_nso.py " + sha.substring(0, Math.min(12, sha.length())) + "…")
                + (r6 == 0 ? ": the loaded main lacks the v0.4.5 patches -> version 0" : r6 > 0 ? ": the v0.4.5 patches are in the loaded main -> version 1"
                : old ? ": a patcher from before v0.4.5 -> version 0" : " -> version 1")); }   // once per process (the gear asks again)
            return old ? 0 : 1;
        }
        @Override public String gameFilesFeatures(int have, int need) {
            StringBuilder sb = new StringBuilder();
            for (int v = Math.max(1, have + 1); v <= need && v < GF_FEATURES.length; v++) if (GF_FEATURES[v] != 0) sb.append(sb.length() > 0 ? "\n" : "").append(getString(GF_FEATURES[v]));
            return sb.toString();
        }
        @Override public SharedPreferences prefs() { return getSharedPreferences(Ui.PREFS, MODE_PRIVATE); }
        @Override public String text(String key, Object... args) {
            for (int i = 0; i < UPD_KEYS.length; i++) if (UPD_KEYS[i].equals(key)) return args.length == 0 ? getString(UPD_IDS[i]) : getString(UPD_IDS[i], args);
            return key;
        }
        @Override public void crumb(String line) { DmActivity.this.crumb(line); }
        @Override public void immersive() { immersiveBars(); }
        @Override public boolean canShowNotice() {
            return resumed && started && !isFinishing() && hasWindowFocus() && inputDialog == null && folderDialog == null
                && (front == null || !front.isOpen()) && (doomPad == null || !doomPad.editing());
        }
        @Override public boolean holdGame(boolean on) {
            if (on) { if (front != null && front.isOpen()) return false; if (doomPad != null) doomPad.releaseAll(); }
            hold(on); return true;
        }
        @Override public String setupGuideUrl() { return "https://github.com/" + UPDATE_REPO + "/blob/main/docs/" + (UpdateNotice.russianUi(language()) ? "INSTALL.ru.md#обновление" : "INSTALL.md#updating"); }
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
            if (resumed) immersiveBars();
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
        if (front != null && front.key(e)) return true;   // [front] the pause menu: Back / Home / Menu, its navigation; nothing reaches a held game
        if (DOOM && e.getAction() == KeyEvent.ACTION_DOWN) android.util.Log.i("dm", "[input] java key " + e.getKeyCode() + " source 0x" + Integer.toHexString(e.getSource()) + " device " + e.getDeviceId() + " started " + started + " dialog " + (inputDialog != null));
        if (started && inputDialog == null && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
            && (controller(e.getDevice(), e.getSource())
                || (DOOM && ((e.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD || e.getSource() == 0) && e.getKeyCode() != KeyEvent.KEYCODE_BACK))   // [DOOM] keyboard too
            && nativePadKey(e.getDeviceId(), e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN)) return true;
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (front != null && front.motion(e)) return true;   // [front] the menu's navigation by hat / stick; a held game gets no axes
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

    // [DOOM] A real controller connected: the on-screen pad hides all but the settings button, and comes back when the last one is
    // disconnected. Only a device that looks like a game controller counts: an external one (USB / Bluetooth) with face buttons or a
    // stick, or a built-in one (handheld consoles) with both. HyperOS phones list built-in input devices that call themselves gamepad /
    // joystick without being one (POCO F6, GitHub issue #5): with the old "any gamepad / joystick source" test the pad never showed there.
    private static boolean physicalController(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        if ((s & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD && (s & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return false;
        boolean[] k = d.hasKeys(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y);
        boolean buttons = k[0] && k[1] && (k[2] || k[3]);
        boolean stick = d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null && d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null;
        return d.isExternal() ? buttons || stick : buttons && stick;
    }
    // [DOOM] 2.45 the pad's surface switches from dm/env (set into the environment by nativeStart): DM_PAD_SURFACE=0 = back to drawing on the window
    // (HWUI, before 2.45), DM_PAD_STALL_MS=N = the draw thread's test stall (PadSurface.stallMs).
    private void padSurfaceEnv() {
        if (doomPad == null) return;
        String on = "", st = "";
        try { String v = Os.getenv("DM_PAD_SURFACE"); on = v == null ? "" : v.trim(); v = Os.getenv("DM_PAD_STALL_MS"); st = v == null ? "" : v.trim(); } catch (Throwable t) { return; }
        if ("0".equals(on) && doomPad.drawsOnSurface()) { doomPad.detachSurface(); crumb("pad drawn on the window (HWUI, DM_PAD_SURFACE=0)"); }
        if (!st.isEmpty()) try { PadSurface.stallMs = Math.max(0, Integer.parseInt(st)); crumb("pad draw thread test stall " + PadSurface.stallMs + " ms (DM_PAD_STALL_MS)"); } catch (NumberFormatException e) { /* ignored */ }
    }
    private String lastInputDevices;
    void updatePadVisibility() {
        if (doomPad == null) return;
        boolean any = false; StringBuilder list = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id); if (d == null) continue;
            boolean c = physicalController(d); any |= c;
            list.append("\n  ").append(id).append(" \"").append(d.getName()).append("\" sources 0x").append(Integer.toHexString(d.getSources()))
                .append(d.isVirtual() ? " virtual" : "").append(d.isExternal() ? " external" : " built-in").append(c ? " -> controller" : "");
        }
        String s = list.toString();   // one log per change: which device hides the on-screen pad is visible in boot.log
        if (!s.equals(lastInputDevices)) { lastInputDevices = s; android.util.Log.i("dm", "[input] devices (on-screen pad " + (any ? "hidden: controller" : "shown") + "):" + s); }
        doomPad.setControllerMode(any);
    }
    @Override public void onDisplayAdded(int id) { updateRefresh(); }
    @Override public void onDisplayRemoved(int id) { updateRefresh(); }
    @Override public void onDisplayChanged(int id) { updateRefresh(); }
    @Override protected void onResume() { super.onResume(); resumed = true; immersiveBars(); startHost(); updateRefresh(); if (started) { nativeFocus(3); if (touchOverlay != null && bootFailure == 0) touchOverlay.setInputEnabled(true); } }
    @Override protected void onPause() { resumed = false; if (inputDialog != null) inputDialog.dismiss(); releaseTouches(); if (touchOverlay != null) touchOverlay.setInputEnabled(false); if (started) nativeFocus(0); super.onPause(); }
    @Override protected void onStart() { super.onStart(); if (started) { nativeFocus(2); if (DOOM && bootFailure == 0) CrashLog.session(this, "fg"); } }   // [DOOM] session state for CrashLog
    @Override protected void onStop() { if (started) { nativeFocus(1); if (DOOM && bootFailure == 0) CrashLog.session(this, "bg"); }
        if (DOOM && libraryLoaded) { android.content.Context app = getApplicationContext(); new Thread(() -> CrashLog.mirrorBootLogs(app), "dm-logcopy").start(); }   // [DOOM] boot.log next to crash.txt
        com.xiaoboluo.d2r.widget.HeroWidgetProvider.requestUpdate(this); super.onStop(); }
    @Override protected void onDestroy() { if (updates != null) updates.dismiss(); if (DOOM && isFinishing()) { crumb("activity finishing"); CrashLog.sessionEnd(this); } if (displays != null) displays.unregisterDisplayListener(this); if (inputs != null) inputs.unregisterInputDeviceListener(this); if (inputDialog != null) inputDialog.dismiss(); if (current.get() == this) current.clear(); super.onDestroy(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersiveBars(); else releaseTouches(); }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
        if (libraryLoaded) nativeSurface(h.getSurface(), w, ht);
        updateRefresh();
        startHost();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { releaseTouches(); if (libraryLoaded) nativeSurface(null, 0, 0); }

}
