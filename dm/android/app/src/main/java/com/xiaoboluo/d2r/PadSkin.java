package com.xiaoboluo.d2r;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

// [front] Draws on-screen controls in a skin (frontend-common, the same file in every port): nx2android's 26 theme packs (PadTheme, assets/themes, MIT,
// Copyright (c) 2026 seedds), or "classic" (no sprites: the pad draws its own outlines). A pack's sprites are designed for nx2android's ergonomic layout
// (assets/overlay_default.tsv); here each control is drawn at whatever place and size the pad gives it: the sprite group of the control with that name is
// scaled from its designed size to the box the pad passes (the smaller of the two ratios, so a round A stays round in a round box). Used by DoomPadView
// (DOOM 2016, Eternal, Xenoblade 2 and 3) and D3PadView (Diablo III).
final class PadSkin {
    static final String CLASSIC = "classic";
    final PadTheme theme;   // null when the APK has no layout file (then every pack draws classic)
    private PadTheme.Skin skin; private String pack = ""; private int layout = -1;
    private final Context ctx;
    private final Paint bmp = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Rect src = new Rect(); private final RectF dst = new RectF();

    PadSkin(Context c) {
        ctx = c; PadTheme t = null;
        try { t = new PadTheme(c.getAssets()); } catch (Exception e) { android.util.Log.i("dm", "[front] no gamepad themes in the APK: " + e); }
        theme = t;
    }
    // the layout of a window: phone at 1.9:1 and wider (as nx2android), else tablet
    static int layoutFor(float w, float h) { return w >= 1.9f * h ? 1 : 0; }
    // pick the pack (CLASSIC or a folder) for a layout; cheap when nothing changed
    void select(String p, int l) {
        if (p == null) p = CLASSIC;
        if (p.equals(pack) && l == layout) return;
        pack = p; layout = l; skin = null;
        if (theme != null && !CLASSIC.equals(p)) skin = theme.skin(ctx.getAssets(), p, l);
        if (theme != null && !CLASSIC.equals(p)) android.util.Log.i("dm", "[front] pad skin " + p + " (" + PadTheme.LAYOUT_NAMES[l] + "): " + (skin == null ? "missing, classic" : skin.sprites.size() + " sprites"));
    }
    boolean on() { return skin != null; }
    String pack() { return pack; }
    // the pack's own opacity (theme.tsv "alpha"), 1 without a skin
    float packAlpha() { return skin == null ? 1 : skin.alpha; }
    boolean has(String name) { return skin != null && theme.index(layout, name) >= 0; }

    // Draw control `name` centred at (cx, cy) inside a box of half extents (hw, hh) px. held: a button / stick under a finger; dpadBits: PadTheme.RIGHT /
    // LEFT / DOWN / UP held on a d-pad; knob: a stick's knob offset, -1..1 of its travel; alpha 0..1. False when the skin has nothing for it (draw classic).
    boolean draw(Canvas cv, String name, float cx, float cy, float hw, float hh, boolean held, int dpadBits, float knobX, float knobY, float alpha, float viewH) {
        if (skin == null) return false;
        int ci = theme.index(layout, name); if (ci < 0) return false;
        PadTheme.Ctl c = theme.layouts[layout][ci];
        float k0 = viewH / skin.dh;                             // design px -> window px at the designed size
        float dw = c.w * viewH, dh = c.h * viewH;               // the designed size in window px
        float k = k0 * Math.min(2 * hw / dw, 2 * hh / dh);
        boolean stick = c.kind == PadTheme.STICK_L || c.kind == PadTheme.STICK_R, any = false;
        float a = Math.max(0, Math.min(1, alpha));
        for (PadTheme.Sprite s : skin.sprites) if (s.ctl == ci && s.state == PadTheme.DECOR) { sprite(cv, s, cx, cy, k, a); any = true; }
        for (PadTheme.Sprite s : skin.sprites) {
            if (s.ctl != ci || s.state == PadTheme.DECOR) continue;
            if (s.state == PadTheme.KNOB) { sprite(cv, s, cx + knobX * skin.travel * k, cy + knobY * skin.travel * k, k, a); any = true; continue; }
            boolean on = c.kind == PadTheme.DPAD ? s.dir >= 0 && (dpadBits & PadTheme.DPAD_BITS[s.dir]) != 0 : held && !stick;
            if (s.state == (on ? PadTheme.HELD : PadTheme.IDLE)) { sprite(cv, s, cx, cy, k, a); any = true; }
        }
        return any;
    }
    private void sprite(Canvas cv, PadTheme.Sprite s, float cx, float cy, float k, float alpha) {
        src.set((int) s.u, (int) s.v, (int) (s.u + s.w), (int) (s.v + s.h));
        float x0 = cx + s.ox * k, y0 = cy + s.oy * k;
        dst.set(x0, y0, x0 + s.w / skin.scale * k, y0 + s.h / skin.scale * k);
        bmp.setAlpha(Math.round(255 * alpha)); cv.drawBitmap(skin.atlas, src, dst, bmp);
    }

    // The packs for the settings: CLASSIC first, then the APK's packs by the game they are styled after; labels[i] for keys[i]
    static List<String> packKeys(Context c, Map<String, String> packs) {
        List<String> keys = new ArrayList<>(packs.keySet());
        Collections.sort(keys, (a, b) -> packs.get(a).compareToIgnoreCase(packs.get(b)));
        keys.add(0, CLASSIC); return keys;
    }
    static String label(Context c, Map<String, String> packs, String key) {
        if (CLASSIC.equals(key)) return Ui.t(c, "Классический (без скина)", "Classic (no skin)");
        String n = packs.get(key); return n == null ? key : n;
    }
}
