package com.xiaoboluo.d2r;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

// [DOOM] Crash reports the player can open with a file manager (BUGS_AND_FIXES 2.22), no button, nothing per frame.
//   where: <game folder>/logs/crash.txt (Download/DOOM Mobile by default, GameFolder.java) when the app can write there (all files access; the storage permission on the Android 10
//     build), else Android/data/<package>/files/logs/crash.txt. Overwritten each time; the two before are kept as crash.prev.txt and
//     crash.prev2.txt (2.56). Next to them: boot.log / boot.prev.log / boot.prev2.log (the last three runs) and boot-<driver>.log, the latest
//     run with each Vulkan driver (mirrorBootLogs, archiveAtStart).
//   when: (1) a Java exception (DmActivity's uncaught exception handler: written at once); (2) a start / boot failure that shows an error
//     screen (written at once, the screen names the file); (3) at the next start, when the last run did not end cleanly: the native crash
//     handler (crash.c -> jni_android.c) left files/dm-crash.txt, or the session marker files/dm-session.txt (written while the game runs,
//     state fg / bg) is still there and the system's exit reason (Android 11+) or the state (Android 10: in front) says it was not a
//     normal kill in the background.
//   what: a header (device, SoC, GPU, Vulkan driver, Android, app version), the reason, the native log of that run (boot.log: its head
//     with the device / driver / GPU lines and its last 512 KB) and the logcat lines of that process (an app reads its own lines without
//     any permission; at the next start they are still in the log buffer unless the phone logged a lot meanwhile).
final class CrashLog {
    static final String SESSION = "dm-session.txt", NATIVE_MARK = "dm-crash.txt";
    static final int HEAD_BYTES = 48 << 10, TAIL_BYTES = 512 << 10, LOGCAT_LINES = 4000;

    private CrashLog() {}

    // <game folder>/logs (GameFolder: Download/DOOM Mobile or the player's folder, e.g. on an SD card; issue #11), else Download/DOOM Mobile/logs
    // (writable), else <external files>/logs, else <files>/logs.
    static File dir(Context c) {
        File pub = GameFolder.writable(c, "logs");
        if (pub != null) return pub;
        File ext = c.getExternalFilesDir(null);
        File d = ext != null ? new File(ext, "logs") : new File(c.getFilesDir(), "logs");
        if (!d.isDirectory() && !d.mkdirs()) d = new File(c.getFilesDir(), "logs");
        d.mkdirs();
        return d;
    }
    static File report(Context c) { return new File(dir(c), "crash.txt"); }

    // ---- the diagnostics APK: a logcat child process writes every line of this app (an app sees only its own uid's lines: Java, native,
    // the driver, the crash buffer) to <logs>/logcat.txt from the first moment of the process, the buffer's earlier lines of this start
    // included. It writes as the lines come, so whatever kills the game silently (SIGSEGV outside our handler, a kernel kill), the file
    // keeps everything up to that moment. The last start's file becomes logcat.prev.txt; 8 MB per file, one rotation (logcat.txt.1).
    private static Process logcat;
    static synchronized void startLogcatCapture(Context c) {
        if (logcat != null) return;
        try {
            File d = dir(c), now = new File(d, "logcat.txt"), prev = new File(d, "logcat.prev.txt");
            wipeForDiag(c);   // first, so a fresh install does not keep the old file as "previous"
            if (now.isFile()) { prev.delete(); now.renameTo(prev); }
            new File(d, "logcat.txt.1").delete();
            logcat = new ProcessBuilder("logcat", "-v", "threadtime", "-b", "main,system,crash", "-f", now.getAbsolutePath(), "-r", "8192", "-n", "1")
                .redirectErrorStream(true).start();
            android.util.Log.i("dm", "[diag] logcat capture to " + now + " (pid " + android.os.Process.myPid() + ")");
        } catch (Exception e) { android.util.Log.w("dm", "[diag] logcat capture failed: " + e); }
    }

    // ---- the diagnostics APK (BuildConfig.DM_DIAG): once per install / update (files/diag-wiped holds the package's lastUpdateTime), the
    // logs and crash markers left by earlier versions are deleted, so every report the tester sends comes from this build. Later starts
    // keep them: a crash of this build is still reported at the next start.
    static void wipeForDiag(Context c) {
        long stamp = 0;
        try { stamp = c.getPackageManager().getPackageInfo(c.getPackageName(), 0).lastUpdateTime; } catch (Exception e) { /* keep 0 */ }
        File mark = new File(c.getFilesDir(), "diag-wiped");
        if (String.valueOf(stamp).equals(readSmall(mark))) return;
        File d = dir(c), ext = c.getExternalFilesDir(null); int n = 0;
        List<File> gone = new ArrayList<>();
        for (String f : new String[] {"crash.txt", "crash.prev.txt", "crash.prev2.txt", "boot.log", "boot.prev.log", "boot.prev2.log", "logcat.txt", "logcat.txt.1", "logcat.prev.txt"}) gone.add(new File(d, f));
        File[] perDriver = d.listFiles((x, nm) -> nm.startsWith("boot-") && nm.endsWith(".log")); if (perDriver != null) for (File f : perDriver) gone.add(f);   // 2.56
        for (File root : new File[] {c.getFilesDir(), ext}) if (root != null) {
            gone.add(new File(root, "dm/boot.log")); gone.add(new File(root, "dm/boot.prev.log")); gone.add(new File(root, "dm/boot.prev2.log"));
            gone.add(new File(root, "logs/crash.txt")); gone.add(new File(root, "logs/crash.prev.txt")); gone.add(new File(root, "logs/crash.prev2.txt")); }
        gone.add(new File(c.getFilesDir(), NATIVE_MARK)); gone.add(new File(c.getFilesDir(), SESSION));
        for (String sm : new String[] {SafeMode.MARKER, SafeMode.SEEN, SafeMode.FIRST, SafeMode.RELAUNCH, SafeMode.GUARD_LOG}) gone.add(new File(c.getFilesDir(), sm));   // [safe-mode] 2.57: the count starts again
        for (File f : gone) if (f.delete()) n++;
        try (OutputStream os = new FileOutputStream(mark)) { os.write(String.valueOf(stamp).getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { /* wipes again next time */ }
        android.util.Log.i("dm", "[diag] old logs and crash markers deleted: " + n + " files");
    }

    // ---- the native logs next to the crash reports (GitHub issue #3: Android/data is out of a file manager's reach on Android 11+):
    // boot.log, boot.prev.log and boot.prev2.log (2.56) of <files or external files>/dm are copied to dir() as they are, overwriting the
    // copies; a log above MIRROR_MAX keeps its head and its last MIRROR_MAX bytes. Called when the app goes to the background and before it
    // quits. (2.56, issue #33) this run's log also goes to boot-<driver>.log (driverLogName): one file per Vulkan driver, replaced only by a
    // later run with the same driver, so a run with another driver never overwrites it.
    static final long MIRROR_MAX = 8L << 20;
    static final String[] RUNS = {"boot.prev2.log", "boot.prev.log", "boot.log"};   // oldest first: a newer run of a driver is copied last
    static synchronized void mirrorBootLogs(Context c) {
        File log = bootLog(c), d = dir(c);
        for (String n : RUNS) copyLog(new File(log.getParentFile(), n), new File(d, n));
        copyPerDriver(log, d);
    }
    // 2.56: at the start, before the native side moves boot.log to boot.prev.log: the last three runs (a crashed one included, it never reached
    // onStop) to their boot-<driver>.log. The files are opened here, at once (a rename does not touch an open file), and copied on a
    // background thread; a copy already newer than its run is left alone.
    static void archiveAtStart(Context c) {
        File log = bootLog(c); final File d = dir(c);
        final List<RandomAccessFile> open = new ArrayList<>(); final List<File> src = new ArrayList<>();
        for (String n : RUNS) { File f = new File(log.getParentFile(), n); if (!f.isFile()) continue;
            try { open.add(new RandomAccessFile(f, "r")); src.add(f); } catch (IOException e) { /* gone meanwhile */ } }
        if (open.isEmpty()) return;
        final long[] mod = new long[src.size()]; for (int i = 0; i < mod.length; i++) mod[i] = src.get(i).lastModified();
        new Thread(() -> {
            synchronized (CrashLog.class) {
                for (int i = 0; i < open.size(); i++) {
                    try (RandomAccessFile in = open.get(i)) {
                        String name = driverLogName(in); if (name == null) continue;
                        File to = new File(d, name);
                        if (to.isFile() && to.lastModified() >= mod[i]) continue;   // already a copy of this run or of a later one
                        copyLog(in, to);
                        android.util.Log.i("dm", "crash log: " + src.get(i).getName() + " -> " + to);
                    } catch (IOException e) { android.util.Log.w("dm", "crash log: per-driver copy: " + e); }
                }
            }
        }, "dm-logarchive").start();
    }
    private static void copyPerDriver(File log, File d) {
        if (!log.isFile()) return;
        try (RandomAccessFile in = new RandomAccessFile(log, "r")) { String name = driverLogName(in); if (name != null) copyLog(in, new File(d, name)); }
        catch (IOException e) { android.util.Log.w("dm", "crash log: per-driver copy " + log + ": " + e); }
    }
    // "boot-<driver>.log" for a native log: the driver the run asked for, from the first "[vk] driver choice: requested=<name>" line
    // (vk_driver.c; after a fallback to the system driver the run stays the failed driver's), else boot.log's header line "Vulkan driver
    // asked: <name>" (jni_android.c log_header: a run that ended before the driver loaded); "system" for the system driver. A path keeps its
    // last part; characters other than [A-Za-z0-9._-] become '_'. null when the head names no driver.
    static String driverLogName(RandomAccessFile in) throws IOException {
        long n = Math.min(in.length(), 1L << 20); byte[] b = new byte[(int) n]; in.seek(0); in.readFully(b);
        String head = new String(b, StandardCharsets.UTF_8), drv = null;
        final String choice = "[vk] driver choice: requested=", asked = "Vulkan driver asked: ";
        int at = head.indexOf(choice);
        if (at >= 0) { int s0 = at + choice.length(), e = s0; while (e < head.length() && head.charAt(e) > ' ') e++; drv = head.substring(s0, e); }
        else if ((at = head.indexOf(asked)) >= 0) { int s0 = at + asked.length(), e = s0; while (e < head.length() && head.charAt(e) > ' ') e++; drv = head.substring(s0, e); }
        if (drv == null || drv.isEmpty()) return null;
        int sl = drv.lastIndexOf('/'); if (sl >= 0 && sl < drv.length() - 1) drv = drv.substring(sl + 1);
        drv = drv.replaceAll("[^A-Za-z0-9._-]", "_"); if (drv.length() > 96) drv = drv.substring(0, 96);
        return "boot-" + drv + ".log";
    }
    private static void copyLog(File from, File to) {
        if (!from.isFile()) return;
        try (RandomAccessFile in = new RandomAccessFile(from, "r")) { copyLog(in, to); }
        catch (IOException e) { android.util.Log.w("dm", "crash log: copy " + from + ": " + e); }
    }
    private static void copyLog(RandomAccessFile in, File to) {
        try (OutputStream out = new FileOutputStream(to)) {
            in.seek(0); long n = in.length(); byte[] buf = new byte[1 << 16];
            if (n > MIRROR_MAX) {   // head (device / driver / GPU lines), a marker, then the tail
                byte[] head = new byte[HEAD_BYTES]; in.readFully(head); out.write(head);
                out.write(("\n---- [" + (n - HEAD_BYTES - MIRROR_MAX) + " bytes left out] ----\n").getBytes(StandardCharsets.UTF_8));
                in.seek(n - MIRROR_MAX);
            }
            for (int k; (k = in.read(buf)) > 0; ) out.write(buf, 0, k);
        } catch (IOException e) { android.util.Log.w("dm", "crash log: copy to " + to + ": " + e); }
    }

    // The native log of the last run: the newer of <files>/dm/boot.log and <external files>/dm/boot.log (jni_android.c picks one by the
    // same rule as DmActivity.startHost; both are looked at so a change of that rule does not lose the log).
    static File bootLog(Context c) {
        File a = new File(c.getFilesDir(), "dm/boot.log"), ext = c.getExternalFilesDir(null), b = ext == null ? null : new File(ext, "dm/boot.log");
        if (b == null || !b.isFile()) return a;
        if (!a.isFile()) return b;
        return a.lastModified() >= b.lastModified() ? a : b;
    }

    // ---- the session marker: the game is running (pid, state fg / bg / boot); removed on a clean end
    static void session(Context c, String state) {
        try (OutputStream os = new FileOutputStream(new File(c.getFilesDir(), SESSION))) {
            os.write(("pid=" + android.os.Process.myPid() + "\nstate=" + state + "\ntime=" + System.currentTimeMillis() + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) { android.util.Log.w("dm", "crash log: session marker: " + e); }
    }
    static void sessionEnd(Context c) { new File(c.getFilesDir(), SESSION).delete(); }

    // ---- a report from what is known now (Java exception, start error): synchronous, a few hundred ms (logcat)
    static File writeNow(Context c, String reason, String details, File log) {
        byte[][] snap = snapshot(log);
        return write(c, reason, details, log, snap, android.os.Process.myPid(), null);
    }

    // ---- at the start: did the last run end badly? Returns a one-line description of what was found (for the boot log), or null.
    // The report itself is written on a background thread; done receives its path (null when nothing was written).
    interface Done { void written(File report, String why); }
    static String checkPrevious(Context c, Done done) {
        File marker = new File(c.getFilesDir(), NATIVE_MARK), sess = new File(c.getFilesDir(), SESSION);
        String mark = readSmall(marker), session = readSmall(sess);
        marker.delete(); sess.delete();
        if (mark == null && session == null) return null;
        int pid = 0; String state = "?"; long started = 0;
        if (session != null) for (String l : session.split("\n")) {
            try {
                if (l.startsWith("pid=")) pid = Integer.parseInt(l.substring(4).trim());
                else if (l.startsWith("state=")) state = l.substring(6).trim();
                else if (l.startsWith("time=")) started = Long.parseLong(l.substring(5).trim());
            } catch (NumberFormatException e) { /* keep the defaults */ }
        }
        String exit = null; int reason = -1, status = 0; String trace = null;
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                ActivityManager am = c.getSystemService(ActivityManager.class);
                List<ApplicationExitInfo> l = am == null ? null : am.getHistoricalProcessExitReasons(c.getPackageName(), pid, 1);
                if (l != null && !l.isEmpty() && (pid == 0 || l.get(0).getPid() == pid)) {
                    ApplicationExitInfo x = l.get(0); reason = x.getReason(); status = x.getStatus();
                    exit = "exit reason " + exitName(reason) + " (" + reason + "), status " + status + ", importance " + x.getImportance()
                        + ", pss " + (x.getPss() >> 10) + " MB, rss " + (x.getRss() >> 10) + " MB" + (x.getDescription() != null ? ", \"" + x.getDescription() + "\"" : "");
                    if (reason == ApplicationExitInfo.REASON_ANR) trace = readTrace(x);
                }
            } catch (RuntimeException e) { exit = "exit reason unavailable: " + e; }
        }
        boolean front = !"bg".equals(state);
        String why;
        if (mark != null) why = "native crash (" + mark.trim().replace('\n', ' ') + ")";
        else if (reason >= 0) {
            boolean bad = reason == ApplicationExitInfo.REASON_CRASH || reason == ApplicationExitInfo.REASON_CRASH_NATIVE || reason == ApplicationExitInfo.REASON_ANR
                || reason == ApplicationExitInfo.REASON_INITIALIZATION_FAILURE || reason == ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE
                || reason == ApplicationExitInfo.REASON_DEPENDENCY_DIED
                || (front && (reason == ApplicationExitInfo.REASON_SIGNALED || reason == ApplicationExitInfo.REASON_LOW_MEMORY || reason == ApplicationExitInfo.REASON_UNKNOWN
                    || (reason == ApplicationExitInfo.REASON_EXIT_SELF && status != 0)));
            if (!bad) return "last run (pid " + pid + ", " + state + ") ended normally: " + exit;
            why = "the app was ended while the game was " + (front ? "in front" : "in the background") + ": " + exit;
        } else {
            if (!front) return "last run (pid " + pid + ") ended in the background (Android 10: no exit reason), not reported";
            why = "the app ended while the game was in front (state " + state + "; " + (exit != null ? exit : "no exit reason on Android 10") + ")";
        }
        if (started > 0) why += "; that run started " + stamp(started);
        final String reasonText = why, details = trace == null ? null : "ANR trace:\n" + trace;
        final File log = bootLog(c);
        final byte[][] snap = snapshot(log);   // now: the native side moves boot.log to boot.prev.log when the game starts again
        final int fpid = pid; final String exitText = exit;
        final Context app = c.getApplicationContext();
        new Thread(() -> {
            File f = write(app, reasonText, details, log, snap, fpid, exitText);
            if (done != null) done.written(f, reasonText);
        }, "dm-crashlog").start();
        return "last run: " + why;
    }

    // ---- writing
    private static File write(Context c, String reason, String details, File log, byte[][] snap, int pid, String exit) {
        File f = report(c), prev = new File(f.getParentFile(), "crash.prev.txt"), prev2 = new File(f.getParentFile(), "crash.prev2.txt");
        StringBuilder sb = new StringBuilder(1 << 20);
        sb.append("DOOM Mobile crash report\n").append("reason: ").append(reason).append('\n');
        if (exit != null && !reason.contains(exit)) sb.append("exit: ").append(exit).append('\n');
        sb.append("written: ").append(stamp(System.currentTimeMillis())).append('\n');
        header(c, sb);
        if (details != null) sb.append("\n---- details ----\n").append(details).append('\n');
        sb.append("\n---- native log ").append(log.getAbsolutePath()).append(" (").append(log.length()).append(" bytes) ----\n");
        if (snap == null) sb.append("(no log)\n");
        else {
            sb.append(new String(snap[0], StandardCharsets.UTF_8));
            if (snap[1] != null) sb.append("\n...\n---- native log, last ").append(snap[1].length >> 10).append(" KB ----\n").append(new String(snap[1], StandardCharsets.UTF_8));
        }
        sb.append("\n---- logcat of the process").append(pid > 0 ? " " + pid : "").append(" (main + crash buffers, last ").append(LOGCAT_LINES).append(" lines) ----\n");
        sb.append(logcat(pid));
        try {
            if (prev.isFile()) { prev2.delete(); if (!prev.renameTo(prev2)) prev.delete(); }   // 2.56: two earlier reports kept
            if (f.isFile()) { prev.delete(); if (!f.renameTo(prev)) f.delete(); }
            try (OutputStream os = new FileOutputStream(f)) { os.write(sb.toString().getBytes(StandardCharsets.UTF_8)); }
            try { android.media.MediaScannerConnection.scanFile(c, new String[] {f.getAbsolutePath()}, null, null); } catch (RuntimeException e) { /* only for MTP views */ }
            android.util.Log.w("dm", "crash log: " + reason + " -> " + f.getAbsolutePath());
            return f;
        } catch (IOException | RuntimeException e) { android.util.Log.e("dm", "crash log: cannot write " + f + ": " + e); return null; }
    }

    static void header(Context c, StringBuilder sb) {
        String ver = "?";
        try { android.content.pm.PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0); ver = pi.versionName + " (" + pi.getLongVersionCode() + ")"; }
        catch (Exception e) { /* keep ? */ }
        sb.append("app: ").append(c.getPackageName()).append(' ').append(ver).append(", product ").append(BuildConfig.DM_PRODUCT).append(BuildConfig.DEBUG ? ", debug build" : "").append('\n');
        sb.append("device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE).append(", ").append(Build.PRODUCT).append(")\n");
        sb.append("SoC: ").append(Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + ", " : "").append("hardware ").append(Build.HARDWARE).append(", board ").append(Build.BOARD).append('\n');
        sb.append("GPU: ").append(DmActivity.hardwareSummary()).append('\n');
        String drv = c.getSharedPreferences("doom_graphics", Context.MODE_PRIVATE).getString("driver", "(default)");
        sb.append("Vulkan driver setting: ").append(drv.isEmpty() ? "system" : drv).append('\n');
        sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (SDK ").append(Build.VERSION.SDK_INT).append("), patch ").append(Build.VERSION.SECURITY_PATCH).append(", build ").append(Build.DISPLAY).append('\n');
        sb.append("fingerprint: ").append(Build.FINGERPRINT).append('\n');
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am != null) { ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo(); am.getMemoryInfo(mi);
            sb.append("memory: ").append(mi.totalMem >> 20).append(" MB, available now ").append(mi.availMem >> 20).append(" MB").append(mi.lowMemory ? " (low)" : "").append('\n'); }
        sb.append("ABI: ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
    }

    // The log's head and tail ({whole} when small), read now.
    static byte[][] snapshot(File log) {
        if (log == null || !log.isFile()) return null;
        try (RandomAccessFile r = new RandomAccessFile(log, "r")) {
            long n = r.length();
            if (n <= HEAD_BYTES + TAIL_BYTES) { byte[] all = new byte[(int) n]; r.readFully(all); return new byte[][] {all, null}; }
            byte[] head = new byte[HEAD_BYTES], tail = new byte[TAIL_BYTES];
            r.readFully(head); r.seek(n - TAIL_BYTES); r.readFully(tail);
            return new byte[][] {head, tail};
        } catch (IOException e) { return null; }
    }

    // logcat -d of one process (pid 0: everything this app may read, i.e. its own lines)
    static String logcat(int pid) {
        List<String> cmd = new ArrayList<>();
        cmd.add("logcat"); cmd.add("-d"); cmd.add("-v"); cmd.add("threadtime"); cmd.add("-b"); cmd.add("main"); cmd.add("-b"); cmd.add("crash");
        cmd.add("-t"); cmd.add(String.valueOf(LOGCAT_LINES));
        if (pid > 0) cmd.add("--pid=" + pid);
        Process p = null;
        try {
            p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] buf = new byte[1 << 16];
            try (InputStream in = p.getInputStream()) { for (int k; (k = in.read(buf)) > 0 && b.size() < (4 << 20); ) b.write(buf, 0, k); }
            p.waitFor(3, TimeUnit.SECONDS);
            String s = b.toString("UTF-8");
            return s.isEmpty() ? "(no lines of this process left in the log buffer)\n" : s;
        } catch (IOException | InterruptedException e) { return "(logcat failed: " + e + ")\n"; }
        finally { if (p != null) p.destroy(); }
    }

    private static String readTrace(ApplicationExitInfo x) {
        if (Build.VERSION.SDK_INT < 30) return null;
        try (InputStream in = x.getTraceInputStream()) {
            if (in == null) return null;
            ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0 && b.size() < (256 << 10); ) b.write(buf, 0, k);
            return b.toString("UTF-8");
        } catch (IOException e) { return null; }
    }
    private static String exitName(int r) {
        switch (r) {
            case 1: return "EXIT_SELF"; case 2: return "SIGNALED"; case 3: return "LOW_MEMORY"; case 4: return "CRASH"; case 5: return "CRASH_NATIVE";
            case 6: return "ANR"; case 7: return "INITIALIZATION_FAILURE"; case 8: return "PERMISSION_CHANGE"; case 9: return "EXCESSIVE_RESOURCE_USAGE";
            case 10: return "USER_REQUESTED"; case 11: return "USER_STOPPED"; case 12: return "DEPENDENCY_DIED"; case 13: return "OTHER";
            case 14: return "FREEZER"; case 15: return "PACKAGE_STATE_CHANGE"; case 16: return "PACKAGE_UPDATED"; default: return "UNKNOWN";
        }
    }
    static String stamp(long ms) { return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date(ms)); }
    static String readSmall(File f) {
        if (!f.isFile()) return null;
        try { return DmActivity.readSmall(f); } catch (IOException e) { return ""; }
    }
    static String stackTrace(Throwable t) {
        java.io.StringWriter w = new java.io.StringWriter(); t.printStackTrace(new java.io.PrintWriter(w)); return w.toString();
    }
}
