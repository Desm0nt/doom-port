package com.xiaoboluo.d2r;

import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

// [safe-mode] The crash guard (frontend-common; DOOM 2016 BUGS_AND_FIXES 2.57): a tiny Service in its own process (android:process=":guard",
// declared by a port that opts in, see SafeMode.java), started by the game (SafeMode.startGuard) while it is in front on a Mali / PowerVR
// family. It links to a Binder token of the game's process (linkToDeath; /proc/<pid> as a fallback) and, when that process dies:
//   - relaunches the game only when the native marker files/gfx-safe-mode names that very pid with a graphics failure written within the last
//     2 minutes: a normal exit, a kill from the recents screen or by the system, a hang or a non-graphics crash leave no such marker, a force
//     stop kills this process too;
//   - at most once per crash (the marker's pid, handled once), and not when the run that died was itself an automatic relaunch that crashed
//     within 20 s of it or before its first frames (guard-first), nor after 3 relaunches within 10 minutes: then only the message waits for
//     the next manual start (gfx-safe-mode, SafeMode.atStart);
//   - through the PendingIntent the game made while in front (Android 14+: sent with the sender's background-activity-start opt-in);
//     Android may still block a start from the background: when no new game process shows up within 5 s, a notification "… crashed in the
//     graphics driver — tap to restart in safe mode" (Android 13+: only with POST_NOTIFICATIONS granted, else nothing).
// Each decision is a line in files/guard.log (the next start copies it into boot.log) and logcat "dm-guard". START_NOT_STICKY: not restarted.
public class DmGuard extends Service {
    static final long FRESH_MS = 120_000, QUICK_MS = 20_000, WINDOW_MS = 600_000;
    static final int MAX_RELAUNCHES = 3;
    private final Handler main = new Handler(Looper.getMainLooper());
    private int pid; private PendingIntent relaunch; private String activity = "", startedBy = "";
    private IBinder token; private IBinder.DeathRecipient death; private Thread poller; private volatile boolean handled; private int lastStart;

    @Override public IBinder onBind(Intent i) { return null; }
    @Override public int onStartCommand(Intent i, int flags, int startId) {
        lastStart = startId;
        if (i == null) { stopSelf(); return START_NOT_STICKY; }
        int p = i.getIntExtra("pid", 0);
        if (p <= 0) { stopSelf(); return START_NOT_STICKY; }
        PendingIntent pi = i.getParcelableExtra("relaunch");
        if (pi != null) relaunch = pi;
        String act = i.getStringExtra("activity"); if (act != null) activity = act;
        if (p == pid && !handled) return START_NOT_STICKY;   // the same game process again (onResume): already watched
        unwatch();
        pid = p; handled = false; String sb = i.getStringExtra("startedBy"); startedBy = sb == null ? "" : sb;
        Bundle b = i.getBundleExtra("token"); token = b != null ? b.getBinder("token") : null;
        boolean linked = false;
        if (token != null) {
            death = () -> died("binder death");
            try { token.linkToDeath(death, 0); linked = true; } catch (android.os.RemoteException e) { died("already gone at start"); return START_NOT_STICKY; }
        }
        if (!linked) {   // no token: look at /proc/<pid> twice a second (same uid: visible)
            final int watch = pid;
            poller = new Thread(() -> { while (!handled && watch == pid) { if (!new File("/proc/" + watch).exists()) { died("/proc gone"); return; } SystemClock.sleep(500); } }, "dm-guard-poll");
            poller.start();
        }
        note("watching pid " + pid + (linked ? " (binder death)" : " (/proc poll)") + (startedBy.isEmpty() ? "" : ", that run was started by " + startedBy));
        return START_NOT_STICKY;
    }
    private void unwatch() { if (token != null && death != null) try { token.unlinkToDeath(death, 0); } catch (RuntimeException e) { /* gone */ } token = null; death = null; }
    @Override public void onDestroy() { unwatch(); super.onDestroy(); }

    private synchronized void died(String how) {
        if (handled) return;
        handled = true;
        final int dead = pid, sid = lastStart; final String by = startedBy;
        // stopSelfResult(sid): not when a newer start (the relaunched game's own guard request) came in meanwhile
        new Thread(() -> { try { decide(dead, by, how); } catch (RuntimeException e) { note("pid " + dead + ": " + e); } main.post(() -> { if (pid == dead) stopSelfResult(sid); }); }, "dm-guard").start();
    }

    private File file(String n) { return new File(getFilesDir(), n); }
    private void decide(int dead, String by, String how) {
        long now = System.currentTimeMillis();
        SystemClock.sleep(300);   // the crash handler wrote the marker before _exit; a short grace for a slow flash write
        SafeMode.Marker m = SafeMode.Marker.read(file(SafeMode.MARKER));
        if (m == null || m.pid != dead || m.time <= 0 || now - m.time > FRESH_MS || now - m.time < -FRESH_MS) {
            note("pid " + dead + " ended (" + how + ") without a fresh graphics-failure marker: not a graphics crash, nothing to do"); return; }
        String first = VkDrivers.readText(file(SafeMode.FIRST));
        boolean reachedFirst = first != null && first.trim().equals(String.valueOf(dead));
        long lastT = 0; int lastN = 0;
        String rl = VkDrivers.readText(file(SafeMode.RELAUNCH));
        if (rl != null) for (String l : rl.split("\n")) { try { if (l.startsWith("time=")) lastT = Long.parseLong(l.substring(5).trim()); else if (l.startsWith("n=")) lastN = Integer.parseInt(l.substring(2).trim()); } catch (NumberFormatException e) { /* 0 */ } }
        boolean auto = "guard".equals(by);
        String why = "pid " + dead + " died with a graphics failure (" + m.why + ", count " + m.count + ")";
        if (auto && (!reachedFirst || now - lastT < QUICK_MS)) {
            note(why + "; it was an automatic relaunch that crashed again " + (!reachedFirst ? "before its first frames" : "within " + (now - lastT) / 1000 + " s") + ": no relaunch, the message waits for the next start");
            return; }
        int n = auto && now - lastT < WINDOW_MS ? lastN + 1 : 1;
        if (n > MAX_RELAUNCHES) { note(why + "; " + lastN + " relaunches within 10 minutes already: no relaunch"); return; }
        SafeMode.write(file(SafeMode.RELAUNCH), "time=" + now + "\nn=" + n + "\nfrom=" + dead + "\n");
        boolean sent = false;
        if (relaunch != null) {
            try {
                Bundle opts = null;
                if (Build.VERSION.SDK_INT >= 34) {
                    ActivityOptions o = ActivityOptions.makeBasic();
                    o.setPendingIntentBackgroundActivityStartMode(Build.VERSION.SDK_INT >= 36 ? ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS : ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED);
                    opts = o.toBundle();
                }
                relaunch.send(this, 0, null, null, null, null, opts);
                sent = true;
            } catch (PendingIntent.CanceledException | RuntimeException e) { note(why + "; relaunch failed: " + e); }
        }
        boolean up = false;
        for (int i = 0; sent && i < 25 && !up; i++) { SystemClock.sleep(200); up = gameRunning(dead); }
        if (up) { note(why + "; relaunched the game (" + n + " in a row)"); return; }
        note(why + "; " + (sent ? "Android did not start the game from the background" : "no relaunch intent") + ": " + notifyRestart());
    }
    // A process of this package other than the dead one and this one: the relaunched game
    private boolean gameRunning(int dead) {
        ActivityManager am = getSystemService(ActivityManager.class); if (am == null) return false;
        List<ActivityManager.RunningAppProcessInfo> l = am.getRunningAppProcesses(); if (l == null) return false;
        for (ActivityManager.RunningAppProcessInfo p : l) if (p.pid != dead && p.pid != android.os.Process.myPid() && getPackageName().equals(p.processName)) return true;
        return false;
    }
    private String notifyRestart() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm == null) return "no notification manager";
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != android.content.pm.PackageManager.PERMISSION_GRANTED) return "notification skipped (POST_NOTIFICATIONS not granted)";
        if (!nm.areNotificationsEnabled()) return "notification skipped (notifications off for the app)";
        try {
            NotificationChannel ch = new NotificationChannel("safe", GearText.s(this, "notif_safe_channel"), NotificationManager.IMPORTANCE_HIGH);
            nm.createNotificationChannel(ch);
            Intent launch = new Intent().setClassName(this, activity.isEmpty() ? getPackageName() + ".DmActivity" : activity)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK).putExtra(SafeMode.EXTRA, "notification");
            PendingIntent tap = PendingIntent.getActivity(this, 4714, launch, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            CharSequence app = getApplicationInfo().loadLabel(getPackageManager());
            Notification nt = new Notification.Builder(this, "safe").setSmallIcon(android.R.drawable.stat_notify_error)
                .setContentTitle(GearText.s(this, "notif_safe_title", app)).setContentText(GearText.s(this, "notif_safe_text"))
                .setStyle(new Notification.BigTextStyle().bigText(GearText.s(this, "notif_safe_text")))
                .setContentIntent(tap).setAutoCancel(true).setCategory(Notification.CATEGORY_ERROR).build();
            nm.notify(4714, nt);
            return "notification posted";
        } catch (RuntimeException e) { return "notification failed: " + e; }
    }
    private void note(String s) {
        android.util.Log.i("dm-guard", s);
        try (OutputStream os = new FileOutputStream(file(SafeMode.GUARD_LOG), true)) { os.write((SafeMode.stamp(System.currentTimeMillis()) + " " + s + "\n").getBytes(StandardCharsets.UTF_8)); }
        catch (IOException e) { /* logcat only */ }
    }
}
