package com.xiaoboluo.d2r;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// The on-screen gamepad's layout and skin, from the APK's assets (frontend-common/tools/sync_frontend.sh copies them from nx2android, dm/android/themes/README.md; MIT, Copyright (c) 2026 seedds):
//   overlay_default.tsv   the ergonomic layout per form factor (tablet / phone): every control's kind, anchor, centre and size, in window heights
//   themes/<pack>/        a skin: theme.tsv (its sprites: control, part, state, place in the atlas, offset from the control's centre in the design's CSS px)
//                         and one atlas per layout (<layout>.png)
//   themes/themes.tsv     title ID -> pack -> the game the pack is styled after (the names the settings list)
// The same model as nx2android's native drawing (overlay_layout.h, overlay_theme.c, gfx_vk_present.c ov_theme_draw), so a pack looks as it does there.
final class PadTheme {
    static final int STICK_L = 0, STICK_R = 1, BUTTON = 2, DPAD = 3, MENU = 4;
    static final int IDLE = 0, HELD = 1, KNOB = 2, DECOR = 3;
    // DM_BTN_* (dm/host/dm_input.h = nn::hid::NpadButton bits)
    static final int A = 1, B = 1 << 1, X = 1 << 2, Y = 1 << 3, LS = 1 << 4, RS = 1 << 5, L = 1 << 6, R = 1 << 7, ZL = 1 << 8, ZR = 1 << 9,
        PLUS = 1 << 10, MINUS = 1 << 11, LEFT = 1 << 12, UP = 1 << 13, RIGHT = 1 << 14, DOWN = 1 << 15;
    static final int[] DPAD_BITS = {RIGHT, LEFT, DOWN, UP};   // the theme's d-pad parts: right, left, down, up
    static final String[] DPAD_PARTS = {"right", "left", "down", "up"};

    static final class Ctl { String name; int kind; float ax, x, y, w, h; int btn; String label; }
    static final class Sprite { int ctl, dir, state; float u, v, w, h, ox, oy; }
    static final class Skin { float alpha = 1, dw, dh, scale = 1, travel; Bitmap atlas; final List<Sprite> sprites = new ArrayList<>(); }

    final Ctl[][] layouts = new Ctl[2][];   // [0] tablet, [1] phone: the same controls in the same order
    static final String[] LAYOUT_NAMES = {"tablet", "phone"};

    PadTheme(AssetManager am) throws IOException {
        List<List<Ctl>> l = new ArrayList<>(); l.add(new ArrayList<>()); l.add(new ArrayList<>());
        for (String line : lines(am, "overlay_default.tsv")) {
            String[] f = line.split("\t"); if (f.length < 10) continue;
            int li = f[0].equals("phone") ? 1 : f[0].equals("tablet") ? 0 : -1; if (li < 0) continue;
            Ctl c = new Ctl(); c.name = f[1];
            c.kind = f[2].equals("stick_l") ? STICK_L : f[2].equals("stick_r") ? STICK_R : f[2].equals("dpad") ? DPAD : f[2].equals("menu") ? MENU : BUTTON;
            c.ax = Float.parseFloat(f[3]); c.x = Float.parseFloat(f[4]); c.y = Float.parseFloat(f[5]); c.w = Float.parseFloat(f[6]); c.h = Float.parseFloat(f[7]);
            c.btn = button(f[8]); c.label = f[9].equals("~") ? "" : f[9];
            l.get(li).add(c);
        }
        layouts[0] = l.get(0).toArray(new Ctl[0]); layouts[1] = l.get(1).toArray(new Ctl[0]);
    }

    static int button(String s) {
        switch (s) {
            case "DM_BTN_A": return A; case "DM_BTN_B": return B; case "DM_BTN_X": return X; case "DM_BTN_Y": return Y;
            case "DM_BTN_LS": return LS; case "DM_BTN_RS": return RS; case "DM_BTN_L": return L; case "DM_BTN_R": return R;
            case "DM_BTN_ZL": return ZL; case "DM_BTN_ZR": return ZR; case "DM_BTN_PLUS": return PLUS; case "DM_BTN_MINUS": return MINUS;
            default: return 0;
        }
    }
    int index(int layout, String name) { Ctl[] c = layouts[layout]; for (int i = 0; i < c.length; i++) if (c[i].name.equals(name)) return i; return -1; }

    // A pack's skin for one layout; null when the APK has no such pack (the pad then draws outlines)
    Skin skin(AssetManager am, String pack, int layout) {
        String dir = "themes/" + pack + "/";
        List<String> rows; try { rows = lines(am, dir + "theme.tsv"); } catch (IOException e) { return null; }
        Skin s = new Skin(); String atlas = null;
        for (String line : rows) {
            String[] f = line.split("\t");
            if (f[0].equals("alpha") && f.length > 1) s.alpha = Float.parseFloat(f[1]);
            else if (f[0].equals("layout") && f.length >= 7 && f[1].equals(LAYOUT_NAMES[layout])) {
                s.dw = Float.parseFloat(f[2]); s.dh = Float.parseFloat(f[3]); atlas = f[4]; s.scale = Float.parseFloat(f[5]); s.travel = Float.parseFloat(f[6]);
            } else if (f[0].equals("sprite") && f.length >= 11 && f[1].equals(LAYOUT_NAMES[layout])) {
                Sprite p = new Sprite(); p.ctl = index(layout, f[2]); if (p.ctl < 0) continue;
                p.dir = -1; for (int k = 0; k < 4; k++) if (DPAD_PARTS[k].equals(f[3])) p.dir = k;
                p.state = f[4].equals("held") ? HELD : f[4].equals("knob") ? KNOB : f[4].equals("decor") ? DECOR : IDLE;
                p.u = Float.parseFloat(f[5]); p.v = Float.parseFloat(f[6]); p.w = Float.parseFloat(f[7]); p.h = Float.parseFloat(f[8]);
                p.ox = Float.parseFloat(f[9]); p.oy = Float.parseFloat(f[10]);
                s.sprites.add(p);
            }
        }
        if (atlas == null || s.dh <= 0) return null;
        BitmapFactory.Options o = new BitmapFactory.Options(); o.inScaled = false; o.inPreferredConfig = Bitmap.Config.ARGB_8888;
        try (InputStream in = am.open(dir + atlas)) { s.atlas = BitmapFactory.decodeStream(in, null, o); } catch (IOException e) { return null; }
        return s.atlas == null ? null : s;
    }

    // The packs in the APK: folder -> the game it is styled after (themes.tsv; a pack no row names keeps its folder name)
    static Map<String, String> packs(AssetManager am) {
        Map<String, String> m = new LinkedHashMap<>();
        try { String[] dirs = am.list("themes"); if (dirs != null) for (String d : dirs) if (!d.endsWith(".tsv")) m.put(d, d); } catch (IOException e) { /* none */ }
        try { for (String line : lines(am, "themes/themes.tsv")) { String[] f = line.split("\t"); if (f.length >= 3 && m.containsKey(f[1]) && m.get(f[1]).equals(f[1])) m.put(f[1], f[2]); } }
        catch (IOException e) { /* names stay the folders' */ }
        if (m.containsKey("ergo")) m.put("ergo", "Ergo");
        return m;
    }

    private static List<String> lines(AssetManager am, String name) throws IOException {
        List<String> out = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(am.open(name), StandardCharsets.UTF_8))) {
            for (String s; (s = r.readLine()) != null; ) if (!s.isEmpty() && !s.startsWith("#")) out.add(s);
        }
        return out;
    }
}
