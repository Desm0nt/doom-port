package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

// [DOOM] On-screen controller.
//   left:  ZL / L in the top corner, "-" toward the middle, the move stick above the d-pad;
//   right: R / ZR in the top corner, "+" toward the middle, A/B/X/Y, and for the right thumb a large FIRE (ZR) button with MELEE
//          (R3, the right stick click: melee and DOOM's "press R" prompts) next to it.
// Looking around works like a mouse: drag on any free spot (not a button, not around the move stick), or drag a held FIRE. Each bit of
// finger travel is added to a pending turn, which is played out through the right stick as tilt x time (hle_args.c makes DOOM's stick
// response linear: no dead zone, curve, acceleration or smoothing). So a given finger offset always gives the same view angle, whether
// the swipe is fast or slow; a resting finger stops the view.
// The move stick is linear too (DOOM's dead zone is off); the gear dialog can add an offset if the game keeps one anyway.
// It feeds the same native controller channel as a physical gamepad (DmActivity.nativePadKey / nativePadAxes, device PAD_DEVICE)
// with the keycodes front_android.c maps to Switch buttons (Enter=A, Esc=B, R=X, F=Y, Q=L, E=R, Shift=ZL, Ctrl=ZR, Tab=+, M=-,
// BUTTON_THUMBL / THUMBR = L3 / R3).
public class DoomPadView extends View {
    static final int PAD_DEVICE = 0x7d00;
    static final int SETTINGS = -1;   // not a key: opens the graphics options

    // Anchor (fraction of the view) plus an offset in dp, so clusters keep their shape on any aspect ratio.
    private static final class Btn {
        final String label; final int key; final float fx, fy, ox, oy, r; final boolean look; int pointer = -1;
        Btn(String l, int k, float fx, float fy, float ox, float oy, float r) { this(l, k, fx, fy, ox, oy, r, false); }
        Btn(String l, int k, float fx, float fy, float ox, float oy, float r, boolean look) { label = l; key = k; this.fx = fx; this.fy = fy; this.ox = ox; this.oy = oy; this.r = r; this.look = look; }
    }
    private static final class Stick {
        final float fx, fy; int pointer = -1; float x, y, travel; long down; boolean onKnob; final int clickKey;
        Stick(float fx, float fy, int clickKey) { this.fx = fx; this.fy = fy; this.clickKey = clickKey; }
    }
    // One finger turning the view: its last position.
    private static final class Look { int pointer = -1; float x, y; }

    // Finger travel (dp) worth one second of full right-stick tilt at sensitivity 1.0, and the time constant over which a pending turn
    // is played out (short, but longer than a game frame so DOOM's per-frame stick sample integrates it evenly).
    private static final float DP_PER_TILT_SECOND = 500, PLAYOUT_S = 0.05f;
    private static final float ABXY_FX = 0.885f, ABXY_FY = 0.36f, DPAD_FX = 0.21f, DPAD_FY = 0.74f, GAP = 50;
    private final Btn[] btns;
    private final Stick left = new Stick(0.12f, 0.42f, KeyEvent.KEYCODE_BUTTON_THUMBL);
    private final Look look = new Look(), fireLook = new Look();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float dp, stickR;
    private float deadzone = 0f, lookGain = 1.0f, lookX, lookY, pendX, pendY;
    private long lookNanos;

    public DoomPadView(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
        stickR = 64 * dp;
        reloadSettings();
        btns = new Btn[] {
            // right cluster: diamond X top, A right, B bottom, Y left
            new Btn("X", KeyEvent.KEYCODE_R, ABXY_FX, ABXY_FY, 0, -GAP, 26), new Btn("A", KeyEvent.KEYCODE_ENTER, ABXY_FX, ABXY_FY, GAP, 0, 28),
            new Btn("B", KeyEvent.KEYCODE_ESCAPE, ABXY_FX, ABXY_FY, 0, GAP, 26), new Btn("Y", KeyEvent.KEYCODE_F, ABXY_FX, ABXY_FY, -GAP, 0, 26),
            // right thumb: FIRE (ZR, also turns the view while held) and MELEE (R3) next to it
            new Btn("FIRE", KeyEvent.KEYCODE_CTRL_LEFT, 0.86f, 0.79f, 0, 0, 44, true),
            new Btn("R3", KeyEvent.KEYCODE_BUTTON_THUMBR, 0.86f, 0.79f, -88, 14, 28),
            new Btn("R", KeyEvent.KEYCODE_E, 0.84f, 0.09f, 0, 0, 26), new Btn("ZR", KeyEvent.KEYCODE_CTRL_LEFT, 0.95f, 0.09f, 0, 0, 28),
            new Btn("+", KeyEvent.KEYCODE_TAB, 0.62f, 0.09f, 0, 0, 22),
            // left: d-pad as four buttons
            new Btn("↑", KeyEvent.KEYCODE_DPAD_UP, DPAD_FX, DPAD_FY, 0, -GAP, 24), new Btn("→", KeyEvent.KEYCODE_DPAD_RIGHT, DPAD_FX, DPAD_FY, GAP, 0, 24),
            new Btn("↓", KeyEvent.KEYCODE_DPAD_DOWN, DPAD_FX, DPAD_FY, 0, GAP, 24), new Btn("←", KeyEvent.KEYCODE_DPAD_LEFT, DPAD_FX, DPAD_FY, -GAP, 0, 24),
            new Btn("L", KeyEvent.KEYCODE_Q, 0.16f, 0.09f, 0, 0, 26), new Btn("ZL", KeyEvent.KEYCODE_SHIFT_LEFT, 0.05f, 0.09f, 0, 0, 28),
            new Btn("-", KeyEvent.KEYCODE_M, 0.38f, 0.09f, 0, 0, 22),
            new Btn("⚙", SETTINGS, 0.50f, 0.09f, 0, 0, 20),   // options (DmActivity.showDoomSettings)
        };
        fill.setColor(0x30ffffff);
        line.setColor(0x90ffffff); line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(2 * dp);
        text.setColor(0xd0ffffff); text.setTextAlign(Paint.Align.CENTER); text.setTextSize(15 * dp);
    }

    // Control options from the gear dialog (DmActivity): move dead-zone compensation and look sensitivity.
    // A real controller is connected (DmActivity.updatePadVisibility): only the settings button stays; everything held is released.
    private boolean controllerMode;
    // Frame counter (gear dialog): fps, mean / max frame time and GPU time of the last 0.5 s, redrawn twice a second.
    private boolean showFps; private final float[] fstats = new float[4];
    private final Runnable fpsTick = new Runnable() { public void run() { if (!showFps) return; DmActivity.nativeFrameStats(fstats); invalidate(); postDelayed(this, 500); } };
    private void drawFps(Canvas c) {
        if (!showFps || fstats[0] <= 0) return;
        String t = String.format(java.util.Locale.ROOT, "%.0f fps  %.1f ms (max %.0f)%s", fstats[0], fstats[1], fstats[2],
            fstats[3] >= 0 ? String.format(java.util.Locale.ROOT, "  GPU %.1f", fstats[3]) : "");
        // top left corner, clear of the rounded screen corner (Xiaomi 14 Ultra: ~147 px radius)
        float w = text.measureText(t), x = 18 * dp + w / 2, y = text.getTextSize();
        fill.setColor(0x80000000); c.drawRect(0, 0, x + w / 2 + 6 * dp, y + 5 * dp, fill);
        c.drawText(t, x, y, text);
    }
    void setControllerMode(boolean on) {
        if (on == controllerMode) return;
        controllerMode = on;
        if (on) {
            for (Btn b : btns) if (b.pointer >= 0) { DmActivity.nativePadKey(PAD_DEVICE, b.key, false); b.pointer = -1; }
            left.pointer = -1; left.x = left.y = 0; look.pointer = fireLook.pointer = -1; pendX = pendY = lookX = lookY = 0;
        }
        invalidate();
    }

    void reloadSettings() {
        SharedPreferences p = getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE);
        deadzone = p.getFloat("move_dz", 0f); showFps = p.getBoolean("fps", true); if (showFps) postDelayed(fpsTick, 500); lookGain = p.getFloat("look", 1.0f);
    }

    private float bx(Btn b) { return b.fx * getWidth() + b.ox * dp; }
    private float by(Btn b) { return b.fy * getHeight() + b.oy * dp; }
    private float sx(Stick s) { return s.fx * getWidth(); }
    private float sy(Stick s) { return s.fy * getHeight(); }

    private Btn hit(float x, float y) {
        Btn best = null; float bestD = Float.MAX_VALUE;
        for (Btn b : btns) {   // nearest button whose (enlarged) circle holds the touch
            float dx = x - bx(b), dy = y - by(b), r = b.r * dp * 1.25f, d = dx * dx + dy * dy;
            if (d <= r * r && d < bestD) { best = b; bestD = d; }
        }
        return best;
    }
    private boolean near(Stick s, float x, float y, float k) { float dx = x - sx(s), dy = y - sy(s); return dx * dx + dy * dy <= stickR * stickR * k * k; }
    private void moveStick(Stick s, float x, float y) {
        float dx = x - sx(s), dy = y - sy(s), d = (float) Math.sqrt(dx * dx + dy * dy);
        if (d > stickR) { dx *= stickR / d; dy *= stickR / d; }
        s.x = dx; s.y = dy; s.travel = Math.max(s.travel, d);
    }
    private void grab(Stick s, int id, float x, float y) { s.onKnob = near(s, x, y, 1.0f); s.pointer = id; s.down = SystemClock.uptimeMillis(); moveStick(s, x, y); s.travel = 0; }
    private void release(Stick s) {   // a short tap on the move stick's knob still presses L3
        if (s.onKnob && s.travel < 0.5f * stickR && SystemClock.uptimeMillis() - s.down < 300) {
            final int k = s.clickKey; DmActivity.nativePadKey(PAD_DEVICE, k, true); postDelayed(() -> DmActivity.nativePadKey(PAD_DEVICE, k, false), 120);
        }
        s.pointer = -1; s.x = s.y = 0;
    }

    // Radial dead-zone compensation: a small finger deflection maps to just past the game's dead zone, then grows linearly.
    private float[] shape(float x, float y) {
        float m = (float) Math.sqrt(x * x + y * y);
        if (m < 0.04f) return new float[] {0, 0};
        float o = Math.min(1f, deadzone + (1f - deadzone) * Math.min(1f, m));
        return new float[] {x / m * o, y / m * o};
    }

    private void lookStart(Look l, int id, float x, float y) { l.pointer = id; l.x = x; l.y = y; }
    private void lookMove(Look l, float x, float y) {   // finger travel -> pending turn, in tilt-seconds
        float k = lookGain / (DP_PER_TILT_SECOND * dp);
        pendX += (x - l.x) * k; pendY += (y - l.y) * k; l.x = x; l.y = y;
    }
    // Play the pending turn out: tilt = pending / PLAYOUT_S (at most full tilt); what was played since the last tick leaves the pending
    // turn, so the total rotation equals the finger travel. A very fast flick just takes a little longer than its swipe to finish.
    private void updateLook() {
        long now = System.nanoTime();
        float dt = lookNanos == 0 ? 0 : Math.min(0.1f, (now - lookNanos) * 1e-9f); lookNanos = now;
        float px = pendX, py = pendY;
        pendX -= lookX * dt; pendY -= lookY * dt;
        if (pendX * px <= 0) pendX = 0;   // played past zero: done on that axis
        if (pendY * py <= 0) pendY = 0;
        float x = pendX / PLAYOUT_S, y = pendY / PLAYOUT_S, m = (float) Math.sqrt(x * x + y * y);
        if (m > 1) { x /= m; y /= m; }
        if (Math.abs(pendX) < 1e-5f && Math.abs(pendY) < 1e-5f) { pendX = pendY = x = y = 0; lookNanos = 0; }
        lookX = x; lookY = y;
    }

    private void axes() {
        updateLook();
        float[] mv = shape(left.x / stickR, left.y / stickR);
        DmActivity.nativePadAxes(PAD_DEVICE, mv[0], mv[1], lookX, lookY, 0, 0, 0, 0);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i); long t = e.getEventTime();
        if (controllerMode) {   // a real controller drives the game: only the settings button, and no axes from this pad (they would
            // make the touch pad the active controller in front_android.c)
            if ((a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN)) { Btn b = hit(x, y);
                if (b != null && b.key == SETTINGS && getContext() instanceof DmActivity) ((DmActivity) getContext()).showDoomSettings(); }
            return true;
        }
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
            Btn b = hit(x, y);
            if (b != null && b.key == SETTINGS) { if (getContext() instanceof DmActivity) ((DmActivity) getContext()).showDoomSettings(); }
            else if (b != null) {
                b.pointer = id; DmActivity.nativePadKey(PAD_DEVICE, b.key, true);
                if (b.look && fireLook.pointer < 0) lookStart(fireLook, id, x, y);
            }
            else if (left.pointer < 0 && near(left, x, y, 1.6f)) grab(left, id, x, y);   // the move stick takes touches around its base only
            else if (look.pointer < 0) lookStart(look, id, x, y);                           // any other free spot turns the view
            break;
        }
        case MotionEvent.ACTION_MOVE:
            for (int k = 0; k < e.getPointerCount(); k++) {
                int pid = e.getPointerId(k);
                float px = e.getX(k), py = e.getY(k);
                if (pid == left.pointer) moveStick(left, px, py);
                if (pid == look.pointer) lookMove(look, px, py);
                if (pid == fireLook.pointer) lookMove(fireLook, px, py);
            }
            break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL: {
            boolean all = a == MotionEvent.ACTION_CANCEL || a == MotionEvent.ACTION_UP;
            for (Btn b : btns) if (b.pointer >= 0 && (b.pointer == id || all)) { DmActivity.nativePadKey(PAD_DEVICE, b.key, false); b.pointer = -1; }
            if (left.pointer >= 0 && (id == left.pointer || all)) release(left);
            if (id == look.pointer || all) look.pointer = -1;
            if (id == fireLook.pointer || all) fireLook.pointer = -1;
            break;
        }
        }
        axes(); invalidate();
        return true;
    }

    private void drawStick(Canvas c, Stick s) {
        float cx = sx(s), cy = sy(s);
        fill.setColor(0x20ffffff); c.drawCircle(cx, cy, stickR, fill); c.drawCircle(cx, cy, stickR, line);
        fill.setColor(s.pointer >= 0 ? 0x80ffffff : 0x40ffffff); c.drawCircle(cx + s.x, cy + s.y, stickR * 0.42f, fill);
    }

    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);
        // tick every frame while a pending turn is being played out
        if (pendX != 0 || pendY != 0 || lookX != 0 || lookY != 0) { axes(); postInvalidateOnAnimation(); }
        drawFps(c);
        if (controllerMode) { for (Btn b : btns) if (b.key == SETTINGS) drawBtn(c, b); return; }
        drawStick(c, left);
        for (Btn b : btns) drawBtn(c, b);
    }
    private void drawBtn(Canvas c, Btn b) {
        float x = bx(b), y = by(b), r = b.r * dp;
        fill.setColor(b.pointer >= 0 ? 0x80ffffff : (b.look ? 0x40ff6040 : 0x30ffffff));
        c.drawCircle(x, y, r, fill); c.drawCircle(x, y, r, line);
        c.drawText(b.label, x, y + text.getTextSize() * 0.35f, text);
    }
}
