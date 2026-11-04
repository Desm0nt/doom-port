package com.xiaoboluo.d2r;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

// [front] The pause menu (frontend-common, the same file in every port): Xenoblade Chronicles 3's front end design, shown over the last frame dimmed while
// the game is held (FrontMenu: nativeHold). A panel in the middle; a finger taps a row (done when it lifts on it), a tap outside the panel resumes. A
// controller: up / down choose, A does, B or + resumes (Nintendo layout: A is the right face button = KEYCODE_BUTTON_B). The rows come from FrontMenu.
final class PauseMenu extends View {
    interface Listener { void onPick(int row); void onResume(); }
    private final Listener cb;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG), sub = new Paint(Paint.ANTI_ALIAS_FLAG);
    private String title = "", game = "", note = "";
    private String[] rows = new String[0];
    private int danger = -1, hi = 0, down = -1;
    static final int ACCENT = 0xff35c4d6, ACCENT_DARK = 0xff1b8fa1;

    PauseMenu(Context c, Listener l) {
        super(c); cb = l;
        line.setStyle(Paint.Style.STROKE); text.setColor(0xffffffff); sub.setColor(0xffa9d8e0);
        setFocusable(true);
    }
    // title: "Пауза"; game: the game's name at the panel's top right; rows: the labels; danger: the row drawn in red (quit), -1 none
    void setRows(String title, String game, String[] rows, int danger) { this.title = title; this.game = game; this.rows = rows; this.danger = danger; if (hi >= rows.length) hi = 0; invalidate(); }
    void open() { hi = 0; down = -1; note = ""; setVisibility(VISIBLE); invalidate(); }
    void setNote(String s) { note = s == null ? "" : s; invalidate(); }

    private float rowH() { return getHeight() * Math.min(0.095f, 0.68f / Math.max(1, rows.length)); }
    private RectF panel() {
        float W = getWidth(), H = getHeight(), pw = Math.min(W * 0.9f, H * 1.05f), ph = rowH() * rows.length + H * 0.2f;
        return new RectF((W - pw) / 2, (H - ph) / 2, (W + pw) / 2, (H + ph) / 2);
    }
    private RectF row(int i) { RectF p = panel(); float H = getHeight(), rh = rowH(), top = p.top + H * 0.13f;
        return new RectF(p.left + H * 0.04f, top + i * rh, p.right - H * 0.04f, top + (i + 1) * rh - H * 0.012f); }
    private int rowAt(float x, float y) { for (int i = 0; i < rows.length; i++) if (row(i).contains(x, y)) return i; return panel().contains(x, y) ? -1 : -2; }

    @Override protected void onDraw(Canvas c) {
        float H = getHeight(), d = getResources().getDisplayMetrics().density;
        c.drawColor(0x99000000);
        RectF p = panel(); float rad = H * 0.035f;
        fill.setShader(new LinearGradient(0, p.top, 0, p.bottom, 0xf20d1a2b, 0xf2081120, Shader.TileMode.CLAMP)); c.drawRoundRect(p, rad, rad, fill); fill.setShader(null);
        line.setColor(ACCENT); line.setStrokeWidth(1.5f * d); c.drawRoundRect(p, rad, rad, line);
        // title with the accent bar under it, the game's name on the right
        text.setTextSize(H * 0.05f); text.setFakeBoldText(true); text.setTextAlign(Paint.Align.LEFT);
        c.drawText(title, p.left + H * 0.05f, p.top + H * 0.085f, text);
        text.setFakeBoldText(false);
        fill.setColor(ACCENT); c.drawRect(p.left + H * 0.05f, p.top + H * 0.1f, p.left + H * 0.05f + H * 0.12f, p.top + H * 0.1f + 2.5f * d, fill);
        sub.setTextSize(H * 0.03f); sub.setTextAlign(Paint.Align.RIGHT);
        c.drawText(game, p.right - H * 0.05f, p.top + H * 0.085f, sub);
        text.setTextSize(Math.min(H * 0.042f, rowH() * 0.48f));
        for (int i = 0; i < rows.length; i++) {
            RectF r = row(i); boolean on = i == hi || i == down;
            if (on) { fill.setShader(new LinearGradient(r.left, 0, r.right, 0, ACCENT_DARK, 0x331b8fa1, Shader.TileMode.CLAMP)); c.drawRoundRect(r, r.height() / 2, r.height() / 2, fill); fill.setShader(null); }
            text.setColor(i == danger ? 0xffffb4a8 : 0xffffffff);
            c.drawText(rows[i], r.left + H * 0.03f, r.centerY() + text.getTextSize() * 0.35f, text);
        }
        text.setColor(0xffffffff);
        if (!note.isEmpty()) { sub.setTextAlign(Paint.Align.CENTER); sub.setTextSize(H * 0.03f); c.drawText(note, p.centerX(), p.bottom - H * 0.03f, sub); }
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(); float x = e.getX(), y = e.getY();
        if (a == MotionEvent.ACTION_DOWN) { down = rowAt(x, y); if (down >= 0) hi = down; invalidate(); }
        else if (a == MotionEvent.ACTION_MOVE && down >= 0) { int r = rowAt(x, y); hi = r == down ? down : -1; invalidate(); }
        else if (a == MotionEvent.ACTION_UP) { int r = rowAt(x, y), d0 = down; down = -1;
            if (d0 >= 0 && r == d0) cb.onPick(r); else if (d0 == -2 && r == -2) cb.onResume(); else invalidate(); }
        else if (a == MotionEvent.ACTION_CANCEL) { down = -1; invalidate(); }
        return true;
    }
    // a controller's key while the menu shows; true when taken
    boolean key(int code) {
        int n = Math.max(1, rows.length);
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: hi = (Math.max(hi, 0) + n - 1) % n; invalidate(); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: hi = (hi + 1) % n; invalidate(); return true;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_DPAD_CENTER: if (hi < 0) cb.onResume(); else cb.onPick(hi); return true;
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_ESCAPE: cb.onResume(); return true;
            default: return false;
        }
    }
}
