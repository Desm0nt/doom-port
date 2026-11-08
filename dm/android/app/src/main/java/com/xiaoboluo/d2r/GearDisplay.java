package com.xiaoboluo.d2r;

import android.app.Activity;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

// [front] The display side of the gear (frontend-common; from DOOM 2016's DmActivity and the XC2 / Diablo III picture shape):
//   - the frame rate limit (pref "fps_limit"): DM_PRESENT_MAX_HZ caps the present pacing (hle_nvn.c: the largest integer fraction of the
//     refresh rate not above it). Above 60 the window asks for a display mode at a multiple of the limit (applyDisplayMode, from the port's
//     onCreate) and the surface votes for it (vulkan_present.cpp); a limit above 60 is offered only when the display has such a mode.
//     DOOM 2016 also mirrors it to DM_DOOM_TICK_MAX (the game's adaptive tick, hle_args.c): GearPages.fpsLimit(..., "DM_DOOM_TICK_MAX");
//   - the picture shape on a screen wider (or narrower) than 16:9 (fitSurface): 16:9 with black bars = the SurfaceView itself sized 16:9 and
//     centred (the compositor still scales the game's buffer), stretched / the game's own widescreen = the whole screen (XC2, Diablo III,
//     Torchlight II: the port says which of its modes means what).
final class GearDisplay {
    static final int[] FPS_LIMITS = {30, 60, 90, 120};
    static final int FPS_LIMIT_DEFAULT = 60;
    private GearDisplay() {}

    static Display display(Activity a) {
        Display d = null;
        if (android.os.Build.VERSION.SDK_INT >= 30) try { d = a.getDisplay(); } catch (RuntimeException e) { /* not attached yet */ }
        DisplayManager dm = a.getSystemService(DisplayManager.class);
        if (d == null && dm != null) d = dm.getDisplay(Display.DEFAULT_DISPLAY);
        return d;
    }
    // A display mode whose refresh rate is an integer multiple of hz (within 1 Hz), preferring the current resolution, then the lowest such
    // rate; null = the display has none.
    static Display.Mode modeFor(Activity a, int hz) {
        Display d = display(a); if (d == null) return null;
        Display.Mode cur = d.getMode(), best = null; boolean bestSame = false;
        for (Display.Mode m : d.getSupportedModes()) {
            float r = m.getRefreshRate(); int k = Math.round(r / hz);
            if (k < 1 || Math.abs(r - k * hz) > 1.0f) continue;
            boolean same = cur != null && m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight();
            if (best == null || (same && !bestSame) || (same == bestSame && r < best.getRefreshRate())) { best = m; bestSame = same; }
        }
        return best;
    }
    static boolean fpsLimitAvailable(Activity a, int hz) { return hz <= 60 || modeFor(a, hz) != null; }
    // The saved limit, lowered to the highest one this display supports (another screen / the system's refresh setting changed). No display yet
    // (or no mode list): keep the saved limit; the native pacer clamps it to an integer fraction of the real refresh rate anyway (hle_nvn.c
    // nvn_pace_cap_hz). The choice is logged ("fps limit ...") because a silent drop to 60 went unnoticed (DOOM 2016).
    static int fpsLimit(Activity a, SharedPreferences p, int[] limits, int def) {
        int want = p.getInt("fps_limit", def);
        if (want <= 60) return want == 30 ? 30 : 60;
        Display d = display(a);
        if (d == null) { android.util.Log.i("dm", "fps limit " + want + ": no display yet, kept"); return want; }
        int got = 60; for (int l : limits) if (l > got && l <= want && fpsLimitAvailable(a, l)) got = l;
        StringBuilder modes = new StringBuilder();
        for (Display.Mode m : d.getSupportedModes()) modes.append(' ').append(m.getModeId()).append('@').append(m.getRefreshRate());
        android.util.Log.i("dm", "fps limit " + want + " -> " + got + " (modes" + modes + ", current " + d.getMode().getModeId() + ")");
        return got;
    }
    // Above 60 the window asks the system for a display mode at a multiple of the limit (e.g. 90 Hz for 90, 120 Hz for 120); at 30 / 60 nothing is
    // requested (the surface's own 120 Hz vote and the system decide). Call from onCreate, before setContentView.
    static void applyDisplayMode(Activity a, SharedPreferences p, int[] limits, int def) {
        int lim = fpsLimit(a, p, limits, def); if (lim <= 60) return;
        Display.Mode m = modeFor(a, lim); if (m == null) return;
        WindowManager.LayoutParams lp = a.getWindow().getAttributes(); lp.preferredDisplayModeId = m.getModeId(); a.getWindow().setAttributes(lp);
        android.util.Log.i("dm", "frame rate limit " + lim + ": display mode " + m.getModeId() + " " + m.getPhysicalWidth() + "x" + m.getPhysicalHeight() + " @ " + m.getRefreshRate() + " Hz");
    }
    static void applyDisplayMode(Activity a, SharedPreferences p) { applyDisplayMode(a, p, FPS_LIMITS, FPS_LIMIT_DEFAULT); }

    // [XC2 / D3] The game's SurfaceView sized for the picture shape: bars = 16:9 centred in the frame (black around it: give the frame a black
    // background), else the whole frame. Call again from the frame's layout listener (rotation, window size) and after the gear's Save.
    static void fitSurface(View view, FrameLayout frame, boolean bars) {
        if (view == null || frame == null) return;
        int fw = frame.getWidth(), fh = frame.getHeight();
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER);
        if (bars && fw > 0 && fh > 0 && fw * 9 > fh * 16) lp.width = fh * 16 / 9;
        else if (bars && fw > 0 && fh > 0 && fw * 9 < fh * 16) lp.height = fw * 9 / 16;
        FrameLayout.LayoutParams cur = (FrameLayout.LayoutParams) view.getLayoutParams();
        if (cur == null || cur.width != lp.width || cur.height != lp.height || cur.gravity != lp.gravity) view.setLayoutParams(lp);
    }
    // landscape: the long side first (the size the game's surface has)
    @SuppressWarnings("deprecation")
    static int[] screenSize(Activity a) {
        int w, h;
        if (android.os.Build.VERSION.SDK_INT >= 30) { android.graphics.Rect b = a.getWindowManager().getCurrentWindowMetrics().getBounds(); w = b.width(); h = b.height(); }
        else { android.graphics.Point pt = new android.graphics.Point(); a.getWindowManager().getDefaultDisplay().getRealSize(pt); w = pt.x; h = pt.y; }   // Android 10 build
        return new int[] {Math.max(w, h), Math.min(w, h)};
    }
}
