package com.xiaoboluo.d2r;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Locale;
import java.util.function.BooleanSupplier;

// [front] The frame counter and the throttle dot, in a small view of their own beside the on-screen pad (frontend-common; docs/port-kb
// peanuts/C-framerate-system.md PNH-C04 + PNH-C10, KB FE-FPS2). Before this the pads drew the counter themselves: every 500 ms the whole
// full-screen pad was drawn again (DoomPadView.fpsTick -> redraw(): a CPU canvas of every control and skin on PadSurface's thread, or an
// invalidate() of the whole view = the UI thread recording the pad again and HWUI's RenderThread drawing the window on the GPU; D3PadView the same).
// Now the pad never redraws for the counter:
//   - the numbers are read on this class's own thread ("dm-fps", background priority), twice a second, as before: {fps, mean / max frame ms,
//     GPU ms} of the last 0.5 s (the port's nativeFrameStats, atomic loads in hle_nvn.c dm_frame_stats; same text as the old counter);
//   - a pad drawn on its own PadSurface (DOOM 2016, DOOM Eternal: the window must not draw while the game runs, KB APP-01 - an HWUI frame of the
//     window was the ANR this surface fixed, the counter tick was the pending message) gets a small SurfaceView of the counter's size, drawn by
//     the same thread with a CPU canvas (no HWUI, no UI-thread frame); a pad drawn on the window (D3PadView, TL2 / DOS2 / NieR without the
//     surface, DOOM 2016's DM_PAD_SURFACE=0) gets a small View, so only its own rectangle is recorded and drawn again, never the pad's controls.
//   The view sits right under the pad in its parent (the pad's controls are drawn over the counter as before) and takes no touches.
// Old behaviour: DM_FPS_VIEW=0 (env, read once the first frames are counted; for A/B runs only, never asked of players) = the pad draws the
// counter itself as before (Legacy: the pad gets the numbers twice a second and redraws). A pad whose parent is not a FrameLayout gets the same.
//
// The throttle dot (pad pref "fps_dot", on by default, shown only with the counter; adapted from peanuts-native 85eeb7a (igawa6),
// GPL-3.0-or-later, android/.../Overlay.java level()): green / yellow / red / grey left of the numbers, the worst of three readings:
//   red     PowerManager thermal status MODERATE or worse, or thermal headroom (getThermalHeadroom(10), API 30+) >= 1.0 (= SEVERE), or the main
//           CPU cluster's clock cap below 65 % of its highest cap this session;
//   yellow  thermal status LIGHT, or headroom >= 0.85, or that cap below 85 %;
//   green   otherwise; grey: nothing could be read.
// The cap measure differs from peanuts (owner's rule: never compare phone clocks with the spec maximum cpuinfo_max_freq, which a phone keeps
// for seconds only, but with the current cap): peanuts divides scaling_max_freq by cpuinfo_max_freq, so a phone that never lifts its cap to the
// spec value (Honor PNM-N49: 2.50 of 3.63 GHz when cool) reads yellow forever. Here the reference is the highest scaling_max_freq this process
// has seen on that cluster (static, kept across counter on / off): "the phone lowered its own ceiling since the game started". A game started
// on an already hot phone has a low reference, but then the thermal status / headroom readings (temperatures, not clocks) are what turn it.
// The main cluster: the cpufreq policy with the most cores (the game's threads run there), a tie goes to the lower clocks (peanuts' rule).
// The thermal status comes from a listener (no polling); headroom and the caps are read every 2 s (getThermalHeadroom answers NaN when asked
// more often than about once a second). A colour change is logged ("[java] throttle dot ..."), at most every 10 s, so a slow stretch in a run
// log can be matched with throttling. A long press on the counter (a still finger on a free spot of the pad, not on a control: the pad hands
// those touches over, pressDown / pressMove / pressUp) opens a short explanation with the readings (Ui.t: ru / en / zh).
final class FpsOverlay {
    interface Stats { void read(float[] out); }        // {fps, mean ms, max ms, GPU ms (< 0 unknown)} of the last 0.5 s; fps <= 0 = no frames yet
    interface Legacy { void stats(float[] s); }        // UI thread: the pad draws the counter itself (DM_FPS_VIEW=0 / no FrameLayout); null = stop
    static final String PREF_DOT = "fps_dot";
    static final int GREY = -1, GREEN = 0, YELLOW = 1, RED = 2;
    private static final int[] DOT = {0xFF4CAF50, 0xFFFFC107, 0xFFF44336};
    private static final int DOT_GREY = 0xFF9E9E9E;
    private static final String FMT = "%.0f fps  %.1f ms (max %.0f)%s";   // the old counter's text, unchanged
    private static final long TICK_MS = 500;

    private final View pad; private final Context ctx; private final Stats stats; private final Legacy legacy; private final BooleanSupplier onSurface;
    private final float dp;
    private final Handler ui = new Handler(Looper.getMainLooper());
    // UI thread state
    private volatile boolean counterOn, dotOn; private boolean shown = true, legacyOn;
    private volatile View host;        // Small or Surf in the pad's parent, null = none (set on the UI thread, read by the tick thread)
    private HandlerThread thread; private volatile Handler h;
    // the picture (written by the tick thread under `this`, painted by it (Surf) or by the UI thread (Small))
    private String text; private int level = GREY; private boolean dot; private float pillRight;
    private volatile float left;       // x where the line starts: clear of the rounded screen corner and the cutout (UI thread, from the insets)
    private final Paint tp = new Paint(Paint.ANTI_ALIAS_FLAG), bg = new Paint(Paint.ANTI_ALIAS_FLAG), dp_ = new Paint(Paint.ANTI_ALIAS_FLAG);
    private Paint tpTick, tpUi;   // copies of tp for measuring: one per thread (a Paint is not meant to be shared between threads)
    private final float padT, baseline, height, dotR, dotCy, gap, padR;
    private final float[] f = new float[4];

    FpsOverlay(View pad, Stats stats, Legacy legacy, BooleanSupplier onSurface) {
        this.pad = pad; ctx = pad.getContext(); this.stats = stats; this.legacy = legacy; this.onSurface = onSurface;
        dp = ctx.getResources().getDisplayMetrics().density;
        tp.setTextSize(14 * dp); tp.setColor(0xEBFFFFFF); tp.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        tp.setFontFeatureSettings("tnum");   // tabular digits: the line does not wobble as the numbers change
        bg.setColor(0x99000000);
        Paint.FontMetrics fm = tp.getFontMetrics();
        padT = 3 * dp; baseline = padT - fm.ascent; height = (float) Math.ceil(baseline + fm.descent + 4 * dp);
        Rect r = new Rect(); tp.getTextBounds("8", 0, 1, r);
        dotR = 4 * dp; dotCy = baseline + r.top / 2f; gap = 7 * dp; padR = 9 * dp;
        left = 18 * dp; tpTick = new Paint(tp); tpUi = new Paint(tp);
        power = (PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
    }

    // ---- what the pad tells (UI thread)
    void set(boolean counter, boolean dotPref) { counterOn = counter; dotOn = dotPref; sync(); }
    void shown(boolean on) { if (on == shown) return; shown = on; View v = host; if (v != null) v.setVisibility(on ? View.VISIBLE : View.INVISIBLE); }
    // attach / detach / a change of the pad's drawing (PadSurface on / off): put the right view in the pad's parent, or take it out
    void sync() { ui.removeCallbacks(syncTask); ui.post(syncTask); }
    private final Runnable syncTask = this::syncNow;
    private void syncNow() {
        ViewGroup parent = pad.getParent() instanceof ViewGroup ? (ViewGroup) pad.getParent() : null;
        boolean run = counterOn && pad.isAttachedToWindow() && parent != null;
        if (!run) {
            stopThread();
            if (host != null && (!counterOn || pad.getParent() == null)) removeHost();   // the pad left its parent / the counter is off: take the view away
            if (legacyOn && !counterOn) { legacyOn = false; legacy.stats(null); }
            return;
        }
        startThread();
        if (dotOn) thermalStart(); else thermalStop();
        if (!(parent instanceof FrameLayout) || envOld == 1) { removeHost(); return; }   // the pad's own counter (tick -> legacy)
        boolean surf = onSurface.getAsBoolean();
        if (host != null && (host.getParent() != parent || (host instanceof Surf) != surf)) removeHost();
        if (host == null) {
            host = surf ? new Surf(ctx) : new Small(ctx);
            host.setVisibility(shown ? View.VISIBLE : View.INVISIBLE);
            parent.addView(host, Math.max(0, parent.indexOfChild(pad)), new FrameLayout.LayoutParams(width(), (int) height, Gravity.TOP | Gravity.LEFT));
            android.util.Log.i("dm", "[java] fps counter: own " + (surf ? "surface (drawn on dm-fps, beside the pad's PadSurface)" : "view (window)") + ", throttle dot " + (dotOn ? "on" : "off"));
        }
        if (legacyOn) { legacyOn = false; legacy.stats(null); }
    }
    private void removeHost() {
        View v = host; host = null;
        if (v != null && v.getParent() instanceof ViewGroup) ((ViewGroup) v.getParent()).removeView(v);
        synchronized (this) { text = null; }
    }
    private int width() {   // the widest line: four-digit max, three-digit mean, GPU (tabular digits: every digit is as wide as an 8)
        float w = tpUi.measureText(String.format(Locale.ROOT, FMT, 888f, 888.8f, 8888f, "  GPU 888.8"));
        return (int) Math.ceil(left + 2 * dotR + gap + w + padR);
    }
    // the insets of the window: the top-left rounded corner (Android 12+) and the cutout, as the pads' old drawFps (FE-FPS, diablo3-port #1)
    private void geometry(View v) {
        WindowInsets in = v.getRootWindowInsets();
        float cr = 0, cx = 0, cy = 0, safe = 0;
        if (in != null && Build.VERSION.SDK_INT >= 31) {
            android.view.RoundedCorner rc = in.getRoundedCorner(android.view.RoundedCorner.POSITION_TOP_LEFT);
            if (rc != null) { cr = rc.getRadius(); cx = rc.getCenter().x; cy = rc.getCenter().y; }
            safe = in.getInsets(WindowInsets.Type.displayCutout()).left;
        }
        float corner = 0;   // x where the corner arc ends at the top of the glyphs
        if (cr > 0 && padT < cy) { float d = cy - padT; corner = d >= cr ? cx : cx - (float) Math.sqrt(cr * cr - d * d); }
        float l = Math.max(18 * dp, Math.max(safe, corner) + 6 * dp);
        if (l != left) {
            left = l;
            ViewGroup.LayoutParams lp = v.getLayoutParams();
            if (lp != null && lp.width != width()) { lp.width = width(); v.setLayoutParams(lp); }
            force = true; kick();
        }
    }

    // ---- the tick thread
    private static int envOld = -1;   // DM_FPS_VIEW=0 -> 1 (the pad draws the counter itself), read once frames are counted (dm/env is loaded by then)
    private volatile boolean running, force;   // force: draw at the next tick even when nothing changed (a new surface, a new place)
    private int ticks; private PowerManager power; private PowerManager.OnThermalStatusChangedListener thermalL;
    private volatile int thermal = -1; private volatile float headroom = Float.NaN, cap = Float.NaN; private volatile long capKHz, peakKHz;
    private int loggedLevel = -2; private long loggedAt;
    private final Runnable tick = this::tickNow;
    private void startThread() {
        if (thread != null) return;
        thread = new HandlerThread("dm-fps", Process.THREAD_PRIORITY_BACKGROUND); thread.start();
        h = new Handler(thread.getLooper()); running = true; ticks = 0; force = true;
        h.post(tick);
    }
    private void stopThread() {
        if (thread == null) return;
        running = false; Handler hh = h; h = null;
        if (hh != null) hh.removeCallbacks(tick);
        thermalStop();
        thread.quitSafely(); thread = null;
    }
    private void kick() { Handler hh = h; if (hh != null) { hh.removeCallbacks(tick); hh.post(tick); } }
    private void tickNow() {
        Handler hh = h; if (!running || hh == null) return;
        hh.removeCallbacks(tick);
        try { stats.read(f); } catch (Throwable e) { f[0] = 0; }   // the library not loaded yet / no such native
        if (envOld < 0 && f[0] > 0) {
            String v = null; try { v = android.system.Os.getenv("DM_FPS_VIEW"); } catch (Throwable e) { /* none */ }
            envOld = "0".equals(v) ? 1 : 0;
            if (envOld == 1) { android.util.Log.i("dm", "[java] fps counter: DM_FPS_VIEW=0, drawn by the pad as before"); sync(); }
        }
        boolean old = envOld == 1 || hostKind() == 0;
        if (old) {   // the pad's own counter, as before: the numbers to the pad, which redraws
            final float[] s = f.clone();
            ui.post(() -> { if (running && counterOn && host == null) { legacyOn = true; legacy.stats(s); } });
        } else {
            boolean wantDot = dotOn;
            if (wantDot && ticks % 4 == 0) readSlow();
            int lv = wantDot ? levelNow() : GREY;
            String t = f[0] > 0 ? String.format(Locale.ROOT, FMT, f[0], f[1], f[2], f[3] >= 0 ? String.format(Locale.ROOT, "  GPU %.1f", f[3]) : "") : null;
            boolean changed;
            synchronized (this) {
                changed = !java.util.Objects.equals(t, text) || lv != level || wantDot != dot;
                text = t; level = lv; dot = wantDot;
                pillRight = t == null ? 0 : left + (wantDot ? 2 * dotR + gap : 0) + tpTick.measureText(t) + padR;
            }
            if (wantDot) note(lv);
            if (changed || force) { force = false; draw(); }
        }
        ticks++;
        hh.postDelayed(tick, TICK_MS);
    }
    private int hostKind() { View v = host; return v instanceof Surf ? 2 : v instanceof Small ? 1 : 0; }   // a racy read is fine: the next tick corrects it
    private void draw() {
        View v = host;
        if (v instanceof Small) v.postInvalidate();
        else if (v instanceof Surf) ((Surf) v).drawNow();
    }
    // the picture: a dark pill flush with the top-left screen corner (only its bottom-right corner rounded), the dot, the numbers
    private void paint(Canvas c) {
        String t; int lv; boolean d; float right;
        synchronized (this) { t = text; lv = level; d = dot; right = pillRight; }
        if (t == null) return;
        float r = 8 * dp, l = left;
        c.drawRoundRect(-r, -r, right, height, r, r, bg);
        float x = l;
        if (d) { dp_.setColor(lv < 0 ? DOT_GREY : DOT[lv]); c.drawCircle(x + dotR, dotCy, dotR, dp_); x += 2 * dotR + gap; }
        c.drawText(t, x, baseline, tp);
    }

    // ---- the throttle dot's readings (the listener: UI thread with the thread's start / stop; the rest: tick thread)
    private void thermalStart() {
        if (Build.VERSION.SDK_INT < 29 || power == null || thermalL != null) return;
        try { thermal = power.getCurrentThermalStatus(); thermalL = s -> { thermal = s; kick(); }; power.addThermalStatusListener(Runnable::run, thermalL); }
        catch (RuntimeException e) { thermalL = null; }
    }
    private void thermalStop() {
        if (power != null && thermalL != null && Build.VERSION.SDK_INT >= 29) try { power.removeThermalStatusListener(thermalL); } catch (RuntimeException e) { /* gone */ }
        thermalL = null;
    }
    private void readSlow() {
        if (power != null && Build.VERSION.SDK_INT >= 30) try { headroom = power.getThermalHeadroom(10); } catch (RuntimeException e) { headroom = Float.NaN; }
        String dir = mainCluster();
        long k = dir == null ? -1 : readLong(new File(dir, "scaling_max_freq"));
        if (k > 0) {
            synchronized (PEAK) { Long p = PEAK.get(dir); if (p == null || k > p) PEAK.put(dir, k); peakKHz = PEAK.get(dir); }
            capKHz = k; cap = (float) k / peakKHz;
        } else { capKHz = 0; cap = Float.NaN; }
    }
    private int levelNow() {
        int th = thermal; float hr = headroom, cp = cap;
        if (th < 0 && Float.isNaN(hr) && Float.isNaN(cp)) return GREY;
        if (th >= PowerManager.THERMAL_STATUS_MODERATE || hr >= 1.0f || cp < 0.65f) return RED;
        if (th >= PowerManager.THERMAL_STATUS_LIGHT || hr >= 0.85f || cp < 0.85f) return YELLOW;
        return GREEN;
    }
    private void note(int lv) {
        long now = SystemClock.uptimeMillis();
        if (lv == loggedLevel || (loggedLevel != -2 && now - loggedAt < 10000)) return;
        loggedLevel = lv; loggedAt = now;
        android.util.Log.i("dm", String.format(Locale.ROOT, "[java] throttle dot %s: thermal status %d, headroom %.2f, main CPU cluster cap %s",
            lv < 0 ? "grey (nothing read)" : new String[] {"green", "yellow", "red"}[lv], thermal, headroom,
            capKHz > 0 ? String.format(Locale.ROOT, "%.2f GHz = %.0f %% of %.2f GHz (its highest this session)", capKHz / 1e6, cap * 100, peakKHz / 1e6) : "unreadable"));
    }
    // highest scaling_max_freq seen per cpufreq policy in this process (the cap's reference, class comment)
    private static final HashMap<String, Long> PEAK = new HashMap<>();
    private static String mainDir; private static boolean mainLooked;
    private static synchronized String mainCluster() {
        if (mainLooked) return mainDir;
        mainLooked = true;
        File[] ps = new File("/sys/devices/system/cpu/cpufreq").listFiles((d, n) -> n.startsWith("policy"));
        if (ps == null) return null;
        int bestCores = 0; long bestMax = 0;
        for (File p : ps) {
            long max = readLong(new File(p, "cpuinfo_max_freq")); if (max <= 0) continue;   // only to break a tie (the lower-clocked cluster)
            int cores = 1;
            try { cores = new String(Files.readAllBytes(new File(p, "related_cpus").toPath()), StandardCharsets.US_ASCII).trim().split("\\s+").length; } catch (Exception e) { /* 1 */ }
            if (cores > bestCores || (cores == bestCores && max < bestMax)) { bestCores = cores; bestMax = max; mainDir = p.getPath(); }
        }
        return mainDir;
    }
    private static long readLong(File f) {
        try { return Long.parseLong(new String(Files.readAllBytes(f.toPath()), StandardCharsets.US_ASCII).trim()); } catch (Exception e) { return -1; }
    }

    // ---- long press on the counter: the explanation (UI thread; the pad hands over its free-spot touches in its own coordinates)
    private int pressId = -1; private float pressX, pressY; private PopupWindow info;
    private final Runnable longPress = () -> { pressId = -1; showInfo(); };
    void pressDown(int id, float x, float y) {
        if (pressId >= 0 || !inPill(x, y)) return;
        pressId = id; pressX = x; pressY = y; ui.postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
    }
    void pressMove(MotionEvent e) {
        if (pressId < 0) return;
        int k = e.findPointerIndex(pressId);
        if (k < 0 || Math.hypot(e.getX(k) - pressX, e.getY(k) - pressY) > ViewConfiguration.get(ctx).getScaledTouchSlop()) pressCancel();
    }
    void pressUp(int id, boolean all) { if (pressId >= 0 && (all || id == pressId)) pressCancel(); }
    private void pressCancel() { pressId = -1; ui.removeCallbacks(longPress); }
    private boolean inPill(float x, float y) {
        View v = host; if (v == null || !shown) return false;
        float right; boolean d; synchronized (this) { right = text == null ? 0 : pillRight; d = dot; }
        if (right <= 0 || !d) return false;   // the explanation is the dot's: only with the dot
        float hx = x + pad.getLeft() - v.getLeft(), hy = y + pad.getTop() - v.getTop(), m = 12 * dp;
        return hx >= 0 && hx <= right + m && hy >= 0 && hy <= height + m;
    }
    private static final String[][] HEAT = {   // PowerManager.THERMAL_STATUS_NONE .. SHUTDOWN: ru, en
        {"нет", "none"}, {"лёгкий", "light"}, {"умеренный", "moderate"}, {"сильный", "severe"}, {"критический", "critical"}, {"аварийный", "emergency"}, {"отключение", "shutdown"}};
    private void showInfo() {
        View v = host; if (v == null || !v.isAttachedToWindow()) return;
        if (info != null) info.dismiss();
        int lv; synchronized (this) { lv = level; }
        String title = lv == RED ? Ui.t(ctx, "Телефон горячий и сильно снизил частоты: кадров может стать меньше", "The phone is hot and has cut its clocks: the frame rate may drop")
                     : lv == YELLOW ? Ui.t(ctx, "Телефон нагрелся и немного снизил частоты", "The phone is warm and has lowered its clocks a little")
                     : lv == GREEN ? Ui.t(ctx, "Нагрева нет: телефон работает на полной скорости", "No throttling: the phone runs at full speed")
                     : Ui.t(ctx, "Телефон не сообщает о нагреве", "The phone does not report its heat");
        SpannableStringBuilder s = new SpannableStringBuilder();
        s.append("●  ");
        s.setSpan(new ForegroundColorSpan(lv < 0 ? DOT_GREY : DOT[lv]), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        int t0 = s.length(); s.append(title); s.setSpan(new StyleSpan(Typeface.BOLD), t0, s.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        int th = thermal; float hr = headroom; long ck = capKHz, pk = peakKHz;
        if (th >= 0 && th < HEAT.length) s.append("\n").append(String.format(Locale.ROOT, Ui.t(ctx, "Нагрев по данным системы: %s", "Heat level the system reports: %s"),
            Ui.t(ctx, HEAT[th][0], HEAT[th][1])));
        if (!Float.isNaN(hr)) s.append("\n").append(String.format(Locale.ROOT, Ui.t(ctx, "Тепловой запас: %.2f (с 1.00 телефон сильно режет частоты)", "Thermal headroom: %.2f (from 1.00 the phone throttles hard)"), hr));
        if (ck > 0 && pk > 0) s.append("\n").append(String.format(Locale.ROOT, Ui.t(ctx, "Предел частоты процессора: %.1f из %.1f ГГц (самого высокого за эту игру)", "CPU clock limit: %.1f of %.1f GHz (the highest this session)"), ck / 1e6, pk / 1e6));
        s.append("\n\n").append(Ui.t(ctx, "Зелёная — полная скорость, жёлтая — телефон греется, красная — горячий. Остыть помогают: снять чехол, играть без зарядки, снизить настройки в ⚙.",
            "Green: full speed. Yellow: the phone is warming up. Red: it is hot. To cool it down: take off the case, play unplugged, lower the settings in ⚙."));
        s.append("\n").append(Ui.t(ctx, "Точку можно убрать в ⚙.", "The dot can be turned off in ⚙."));
        TextView tv = new TextView(ctx);
        tv.setText(s); tv.setTextColor(0xFFFFFFFF); tv.setTextSize(13); tv.setLineSpacing(3 * dp, 1f);
        int px = Math.round(14 * dp), py = Math.round(11 * dp); tv.setPadding(px, py, px, py);
        GradientDrawable g = new GradientDrawable(); g.setColor(0xF01B1F24); g.setCornerRadius(12 * dp); g.setStroke(Math.max(1, Math.round(dp)), 0x40FFFFFF);
        tv.setBackground(g);
        int sw = pad.getRootView().getWidth();
        tv.setMaxWidth(Math.round(Math.min(380 * dp, sw - 32 * dp)));
        final PopupWindow p = new PopupWindow(tv, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, false);   // not focusable: the game keeps input
        p.setOutsideTouchable(true); p.setTouchable(true); tv.setOnClickListener(x -> p.dismiss());
        int[] at = new int[2]; v.getLocationInWindow(at);
        try { p.showAtLocation(v, Gravity.TOP | Gravity.LEFT, at[0] + Math.round(left), at[1] + Math.round(height + 6 * dp)); }
        catch (RuntimeException e) { return; }   // the window is going away
        info = p;
        ui.postDelayed(() -> { if (p.isShowing()) try { p.dismiss(); } catch (RuntimeException e) { /* gone */ } }, 10000);
    }

    // ---- the two hosts: no touches (the pad below gets them), the picture from paint()
    private final class Small extends View {
        Small(Context c) { super(c); setWillNotDraw(false); setClickable(false); setFocusable(false); }
        @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); geometry(this); }
        @Override public WindowInsets onApplyWindowInsets(WindowInsets in) { post(() -> geometry(this)); return super.onApplyWindowInsets(in); }
        @Override protected void onDraw(Canvas c) { paint(c); }
        @Override public boolean onTouchEvent(MotionEvent e) { return false; }
    }
    // a CPU-drawn surface like PadSurface (that class has why): above the game, below the window; drawn only on the dm-fps thread, onto a second
    // Java Surface over the same native window when the library has PadSurface.nativeWrap
    private static boolean wrapMissing;
    private final class Surf extends SurfaceView implements SurfaceHolder.Callback {
        private Surface out; private boolean owned;   // dm-fps thread only
        // the surface the UI thread was handed, taken over by the dm-fps thread at its next draw (the thread may be (re)started after it came)
        private Surface pend; private boolean pendOwn, pendSet;
        Surf(Context c) { super(c); setZOrderMediaOverlay(true); getHolder().setFormat(PixelFormat.TRANSLUCENT); getHolder().addCallback(this); setClickable(false); setFocusable(false); }
        @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); geometry(this); }
        @Override public WindowInsets onApplyWindowInsets(WindowInsets in) { post(() -> geometry(this)); return super.onApplyWindowInsets(in); }
        @Override public boolean onTouchEvent(MotionEvent e) { return false; }
        @Override public void surfaceCreated(SurfaceHolder holder) {}
        @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int ht) {
            Surface s = holder.getSurface(), wrapped = null;
            if (!wrapMissing) try { wrapped = PadSurface.nativeWrap(s); } catch (UnsatisfiedLinkError e) { wrapMissing = true; }
            hand(wrapped != null ? wrapped : s, wrapped != null);
        }
        @Override public void surfaceDestroyed(SurfaceHolder holder) { hand(null, false); }
        @Override protected void onDetachedFromWindow() { super.onDetachedFromWindow(); hand(null, false); }
        private void hand(Surface s, boolean own) {
            synchronized (this) { pend = s; pendOwn = own; pendSet = true; }
            force = true; Handler hh = h; if (hh != null) hh.post(this::drawNow);
        }
        void drawNow() {   // dm-fps thread
            synchronized (this) {
                if (pendSet) {
                    pendSet = false;
                    if (out != null && out != pend && owned) try { out.release(); } catch (RuntimeException e) { /* gone already */ }
                    out = pend; owned = pendOwn;
                }
            }
            Surface s = out; if (s == null || !s.isValid()) return;
            Canvas c;
            try { c = s.lockCanvas(null); } catch (RuntimeException e) { return; }
            try { c.drawColor(0, PorterDuff.Mode.CLEAR); paint(c); }
            finally { try { s.unlockCanvasAndPost(c); } catch (RuntimeException e) { /* the surface went away */ } }
        }
    }
}
