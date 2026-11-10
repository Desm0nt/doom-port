package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.view.View;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

// [safe-mode] Safe graphics mode for Mali / PowerVR, the app side (frontend-common; DOOM 2016 BUGS_AND_FIXES 2.57). The native side
// (DOOM: xbg_game.cpp namespace safemode, xbg_safe_mode.h) counts graphics failures (a driver crash in a dm-record packet, on a graphics
// thread or in the driver's code, a GPU-hang fence fatal, VK_ERROR_DEVICE_LOST) in files/gfx-safe-mode and turns on a step of workarounds at
// the next start: 1 = the experimental 0.5.x speed-ups off, 2 = + the tile-GPU policies, 3 = + compile lock 2 / colour mask / culling GS.
// This class, made once by the port (GearHost.safeMode):
//   - atStart (onCreate): an earlier run's failure the player was not told about (gfx-safe-mode "time" != gfx-safe-mode.seen), how this
//     process was started (EXTRA: by the crash guard or its notification), the guard's notes (files/guard.log) into the run log;
//   - gameStarted (nativeStart ok): waits for the native decision (GearHost.safeModeStatus), shows the notice once (which step is on, that the
//     0.5.x speed-ups are experimental and off, that the next crash tries the next step, where the logs are), starts the crash guard
//     (DmGuard, its own process) on a Mali / PowerVR family, then polls the in-process fallback messages (GearHost.safeModeEvent) every 2 s
//     and shows each as a toast (non-blocking);
//   - the gear row (GearPages.safeMode): off / auto / on = DM_SAFE_MODE 0 / unset / on (pref "safe_mode" in GearHost.gamePrefs); a change of
//     the choice starts the count again (gfx-safe-mode removed).
// Files (GearHost.activity().getFilesDir()): gfx-safe-mode (native, "key=value": count, step, pid, time ms, family, why), gfx-safe-mode.seen
// (the "time" of the failure the notice was shown for), guard-first (the pid of a run that reached its first frames), guard-relaunch and
// guard.log (DmGuard). Internal prefs: file "safe_mode" (the notification permission asked once, the zero-copy note once per app version).
final class SafeMode {
    static final String MARKER = "gfx-safe-mode", SEEN = "gfx-safe-mode.seen", FIRST = "guard-first", RELAUNCH = "guard-relaunch", GUARD_LOG = "guard.log";
    static final String EXTRA = "dmSafeRelaunch";   // the intent extra of a start by DmGuard: "guard" (automatic) or "notification" (its tap)
    static final String PREF = "safe_mode";         // GearHost.gamePrefs int: 0 off, 1 auto (default), 2 on
    static final int OFF = 0, AUTO = 1, ON = 2;
    private static final android.os.Binder TOKEN = new android.os.Binder();   // DmGuard links to its death: this process is gone

    // ---- the native marker file
    static final class Marker {
        int count, step = -1, pid; long time; String family = "", why = "";
        static Marker read(File f) {
            String s = VkDrivers.readText(f); if (s == null) return null;
            Marker m = new Marker();
            for (String l : s.split("\n")) {
                int eq = l.indexOf('='); if (eq <= 0) continue;
                String k = l.substring(0, eq), v = l.substring(eq + 1).trim();
                try {
                    switch (k) {
                        case "count": m.count = Integer.parseInt(v); break;
                        case "step": m.step = Integer.parseInt(v); break;
                        case "pid": m.pid = Integer.parseInt(v); break;
                        case "time": m.time = Long.parseLong(v); break;
                        case "family": m.family = v; break;
                        case "why": m.why = v; break;
                        default: break;
                    }
                } catch (NumberFormatException e) { /* keep the default */ }
            }
            return m;
        }
    }

    final GearHost host;
    private Marker pending;        // an earlier run's graphics failure the notice has not been shown for
    private String relaunchedBy;   // EXTRA of this start, null = a normal start
    private String[] status;       // the native decision once known (length 0: none / not decided)
    private boolean noticeDone, guardStarted, eventsOn;
    private long startedAt;

    SafeMode(GearHost h) { host = h; }
    private Activity a() { return host.activity(); }
    private File file(String n) { return new File(a().getFilesDir(), n); }
    private String s(String key, Object... args) { return GearText.s(a(), key, args); }
    private SharedPreferences own() { return a().getSharedPreferences("safe_mode", Context.MODE_PRIVATE); }
    static void write(File f, String text) {
        try (OutputStream os = new FileOutputStream(f)) { os.write(text.getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { android.util.Log.w("dm", "safe mode: " + f + ": " + e); }
    }
    static String stamp(long ms) { return new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date(ms)); }

    // ---- onCreate, before the native start
    void atStart(Intent i) {
        relaunchedBy = i != null ? i.getStringExtra(EXTRA) : null;
        Marker m = Marker.read(file(MARKER));
        String seen = VkDrivers.readText(file(SEEN));
        if (m != null && m.time > 0 && !String.valueOf(m.time).equals(seen == null ? "" : seen.trim())) pending = m;
        String g = VkDrivers.readText(file(GUARD_LOG));
        if (g != null) { file(GUARD_LOG).delete(); for (String l : g.split("\n")) if (!l.trim().isEmpty()) host.crumb("safe mode guard: " + l.trim()); }
        if (relaunchedBy != null) host.crumb("safe mode: this start came from the crash guard (" + relaunchedBy + ")");
        if (m != null) host.crumb("safe mode: gfx-safe-mode count " + m.count + ", last: " + (m.why.isEmpty() ? "-" : m.why) + (m.time > 0 ? " at " + stamp(m.time) + " (pid " + m.pid + ")" : "")
            + (pending != null ? "; not shown yet: the notice comes after the native decision" : "; already shown"));
    }

    // ---- nativeStart succeeded: the decision, the notice, the guard, then the live messages
    void gameStarted(View v) {
        startedAt = SystemClock.uptimeMillis();
        if (v != null) v.postDelayed(() -> poll(v), 150);
    }
    void onResume() { if (guardStarted && eligible()) { guardStarted = false; startGuard(); } }   // the system stops a started service ~1 min after the app went to the background
    // The run reached its first frames: the crash guard then may relaunch it after a later crash (DmGuard reads guard-first)
    void firstFrames() { if (eligible()) write(file(FIRST), String.valueOf(android.os.Process.myPid())); }

    String[] status() {
        if (status != null && status.length > 0) return status;
        String st; try { st = host.safeModeStatus(); } catch (Throwable t) { st = null; }
        return st == null || st.isEmpty() ? null : st.split("\t", -1);
    }
    // Mali / PowerVR (simulated included): the native decision, else (before the library loads) the device nodes
    boolean eligible() {
        String[] st = status();
        if (st != null) return "1".equals(st[0]);
        return VkDrivers.mali() || VkDrivers.powervr();
    }
    private static int num(String[] st, int i, int dflt) { try { return st != null && st.length > i ? Integer.parseInt(st[i].trim()) : dflt; } catch (NumberFormatException e) { return dflt; } }
    // "This run: step N of 3 (graphics crashes counted: K)" for the gear, null when not known
    String nowLine() { String[] st = status(); return st == null || !"1".equals(st[0]) ? null : s("gear_safe_now", num(st, 1, 0), num(st, 4, 0)); }

    private void poll(View v) {
        if (a().isDestroyed() || !host.started()) return;
        if (status == null) {
            String st; try { st = host.safeModeStatus(); } catch (Throwable t) { st = null; }
            if (st != null && !st.isEmpty()) {
                status = st.split("\t", -1);
                host.crumb("safe mode: native decision: eligible " + status[0] + ", step " + num(status, 1, 0) + " (" + (status.length > 2 ? status[2] : "?") + "), family "
                    + (status.length > 3 ? status[3] : "?") + ", count " + num(status, 4, 0) + (status.length > 6 && !status[6].isEmpty() ? ", set " + status[6] : ""));
            } else if (st != null && SystemClock.uptimeMillis() - startedAt < 15000) { v.postDelayed(() -> poll(v), 150); return; }
            else status = new String[0];   // no such native, or Vulkan did not get that far: the notice from the marker alone
        }
        if (eligible()) startGuard();
        if (pending != null && !noticeDone && status.length > 0 && !"1".equals(status[0])) {   // e.g. a DM_GFX_SIM_GPU run's failure, now on the real Adreno
            noticeDone = true; write(file(SEEN), String.valueOf(pending.time)); host.crumb("safe mode: an earlier graphics failure is not shown: this run is not on Mali / PowerVR");
        }
        if (pending != null && !noticeDone && !showNotice()) { v.postDelayed(() -> poll(v), 500); return; }
        if (status.length > 0 && "1".equals(status[0]) && !eventsOn) { eventsOn = true; v.postDelayed(() -> events(v), 2000); }
    }

    // The notice after a graphics failure: false = the activity is not ready for a dialog yet (ask again later)
    private boolean showNotice() {
        Activity a = a();
        if (a.isFinishing() || a.isDestroyed()) { noticeDone = true; return true; }
        if (!a.hasWindowFocus()) { if (SystemClock.uptimeMillis() - startedAt < 60000) return false; host.crumb("safe mode: notice not shown (no window focus for a minute)"); noticeDone = true; return true; }
        noticeDone = true;
        Marker m = pending;
        String[] st = status != null && status.length > 0 ? status : null;
        int step = st != null ? num(st, 1, 0) : Math.min(m.count, 3);
        String how = st != null && st.length > 2 ? st[2] : "counted";
        StringBuilder sb = new StringBuilder(s("safe_crash"));
        if (relaunchedBy != null) sb.append(' ').append(s("safe_relaunched"));
        sb.append("\n\n").append(step <= 0 ? s("safe_step_0") : step == 1 ? s("safe_step_1") : step == 2 ? s("safe_step_2") : s("safe_step_3"));
        if (step > 0 && ("counted".equals(how) || "on".equals(how))) sb.append("\n\n").append(step >= 3 ? s("safe_last") : s("safe_next"));
        sb.append("\n\n").append(s("safe_gear"));
        File logs = host.logsDir(); if (logs != null) sb.append("\n\n").append(s("safe_logs", logs.getAbsolutePath()));
        if (!m.why.isEmpty()) sb.append("\n\n(").append(m.why).append(')');
        try {
            AlertDialog d = new AlertDialog.Builder(a).setTitle(s("safe_title")).setMessage(sb.toString()).setPositiveButton(s("safe_ok"), null).create();
            d.setOnDismissListener(x -> { host.immersive(); askNotifications(); });
            d.show();
        } catch (RuntimeException e) { host.crumb("safe mode: notice not shown: " + e); return true; }
        write(file(SEEN), String.valueOf(m.time));
        host.crumb("safe mode: notice shown: step " + step + " of 3 (" + how + ") after the graphics failure " + (m.why.isEmpty() ? "?" : "\"" + m.why + "\"") + " at " + stamp(m.time)
            + " (count " + m.count + ")" + (relaunchedBy != null ? "; this start: " + relaunchedBy : ""));
        return true;
    }
    // Android 13+: the crash guard's "tap to restart" notification needs POST_NOTIFICATIONS; asked once, right after the first notice (never at a
    // normal start). Declined = the guard simply posts nothing.
    private void askNotifications() {
        if (Build.VERSION.SDK_INT < 33 || own().getBoolean("notif_asked", false)) return;
        own().edit().putBoolean("notif_asked", true).apply();
        if (a().checkSelfPermission("android.permission.POST_NOTIFICATIONS") == android.content.pm.PackageManager.PERMISSION_GRANTED) return;
        host.crumb("safe mode: asking for the notification permission once (the crash guard's restart notification)");
        try { a().requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, 4712); } catch (RuntimeException e) { /* no dialog: skipped */ }
    }

    // In-process fallbacks the engine made (zc: zero-copy off at boot after its self-test; fence: a recovered fence error, the safer fence mode
    // for the rest of the run; pipe: a failed pipeline build, pipelines one at a time): a toast each; zc once per app version (phones whose
    // gralloc refuses it fail the self-test at every start).
    private void events(View v) {
        if (a().isDestroyed() || !host.started()) return;
        for (int n = 0; n < 4; n++) {
            String e; try { e = host.safeModeEvent(); } catch (Throwable t) { return; }
            if (e == null || e.isEmpty()) break;
            String[] f = e.split("\t", 2); String kind = f[0], detail = f.length > 1 ? f[1] : "";
            String key = "zc".equals(kind) ? "toast_safe_zc" : "fence".equals(kind) ? "toast_safe_fence" : "pipe".equals(kind) ? "toast_safe_pipe" : null;
            boolean show = key != null;
            if (show && "zc".equals(kind)) { String ver = version(), was = own().getString("note_zc", ""); show = !ver.equals(was); if (show) own().edit().putString("note_zc", ver).apply(); }
            host.crumb("safe mode (live): " + kind + ": " + detail + (show ? " -> message shown" : key == null ? " (unknown kind)" : " (message already shown for this app version)"));
            if (show) Toast.makeText(a(), s(key), Toast.LENGTH_LONG).show();
        }
        v.postDelayed(() -> events(v), 2000);
    }
    private String version() { try { return a().getPackageManager().getPackageInfo(a().getPackageName(), 0).versionName; } catch (Exception e) { return "?"; } }

    // ---- the crash guard (DmGuard, process ":guard"): told this process's pid, a token whose death it watches, and a PendingIntent made here
    // while the game is in front (Android 15+: with the creator's background-start opt-in; the guard adds the sender's on Android 14+)
    void startGuard() {
        if (guardStarted) return;
        guardStarted = true;
        Activity a = a();
        try {
            Intent launch = new Intent(a, a.getClass()).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK).putExtra(EXTRA, "guard");
            Bundle opts = null;
            if (Build.VERSION.SDK_INT >= 35) {
                ActivityOptions o = ActivityOptions.makeBasic();
                o.setPendingIntentCreatorBackgroundActivityStartMode(Build.VERSION.SDK_INT >= 36 ? ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS : ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED);
                opts = o.toBundle();
            }
            PendingIntent pi = PendingIntent.getActivity(a, 4713, launch, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT, opts);
            Bundle token = new Bundle(); token.putBinder("token", TOKEN);
            Intent g = new Intent(a, DmGuard.class).putExtra("pid", android.os.Process.myPid()).putExtra("relaunch", pi).putExtra("token", token)
                .putExtra("activity", a.getClass().getName()).putExtra("startedBy", relaunchedBy == null ? "" : relaunchedBy);
            a.startService(g);
            host.crumb("safe mode: crash guard started (process :guard watches pid " + android.os.Process.myPid() + "; relaunch only after a graphics failure, at most once per crash)");
        } catch (RuntimeException e) { host.crumb("safe mode: crash guard not started: " + e); }
    }

    // ---- the gear row (GearPages.safeMode)
    static int pref(SharedPreferences p) { return Math.min(Math.max(p.getInt(PREF, AUTO), OFF), ON); }
    static void env(SharedPreferences p) throws ErrnoException {
        int v = pref(p);
        if (v == OFF) Os.setenv("DM_SAFE_MODE", "0", true); else if (v == ON) Os.setenv("DM_SAFE_MODE", "on", true); else Os.unsetenv("DM_SAFE_MODE");
    }
    // A new choice starts the count again (the native side reads gfx-safe-mode only at the start; it writes it only after a failure)
    void choiceChanged(int was, int now) {
        boolean had = file(MARKER).delete();
        host.crumb("safe mode: gear choice " + was + " -> " + now + " (0 off, 1 auto, 2 on)" + (had ? "; the graphics-failure count starts again" : ""));
    }
}
