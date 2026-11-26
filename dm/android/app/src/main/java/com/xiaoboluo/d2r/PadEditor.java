package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// [front] SHARED (frontend-common/java, copied into every port by tools/sync_frontend.sh; edit it only there). Owner rule 25.11.2026: every port
// has the full layout editor, always; no port ships the old "Drag a control to move it" + Reset layout / Done editor (migration/pad-editor-shared.md).
// DOOM's layout editor (frontend-common DoomPadView, same gestures and toolbar look) as a part a pad view owns: Tl2Touch (TL2's tl2 /
// tl2_side) and D3PadView's layouts (DOS2's / NieR's Switch pad) use it. Factored out of Tl2Touch (09.11.2026 "editor as in DOOM"); the prefs
// format is Tl2Touch's, under the prefixes the host names:
//   group  <offPrefix><group> = "fx,fy" or "fx,fy,scale": offset (fractions of the view) and scale about the group's default centre (the mean of
//          its items' default centres); scale 1 is written in the 2-field form
//   item   <itemPrefix><name> = "dx;dy;scale;hidden": its own place inside the group (fractions of the view, before the group's scale), size
//          (x the group's) and hide (0 / 1); a zone item (zone != null) writes only its hide, as "0;0;1;1"
//   <splitKey> = the Grouped / Separate toggle (written at once, kept by Reset)
// Toolbar − + Grouped/Separate Hide/Show Reset Cancel Save: drag a control with a finger, a second finger resizes it, − / + with nothing
// chosen (or a control without a size) resize every group. Grouped moves / resizes the chosen control's whole group, Separate the chosen
// item alone. Hide / Show: the chosen item (Separate) or its group (Grouped); fixed items (the gear) never hide; the host does not draw nor hit
// a hidden item in play, the editor shows it dimmed with a red slash. Nothing is written before Save; Cancel / Back roll back.
// Zone items (Tl2Touch's zoom slider and move zone): a rect the host lays out itself from its group's offset / scale; always a group of its own.
// [front] Players' own controls (owner 15.11.2026; dialogs in PadEdit.java, the same as frontend-common DoomPadView's editor): toolbar
// − + Grouped/Separate Hide/Show Add… Rename… Reset Cancel Save. An item with defHidden (the stand-alone L3 / R3) is hidden until "Add…" shows it:
// a missing key or a value without its 4th field keeps that default, so layouts saved before load unchanged. "Add…" lists every hidden item
// (defHidden ones first); a pick shows it (with its group while grouped) and chooses it. "Rename…" gives the chosen item with canRename its own
// label: <itemPrefix><name>_lbl = the text (at most PadEdit.MAX_LABEL characters; missing = defLabel); the host draws custom instead of its
// glyph. Saved on Save with the places, Cancel rolls back, Reset returns hides and labels to the defaults too.
// Texts (no port class is named here): every text has a string resource name (pad_ed_add, pad_ed_hide, ...; see text()). A port that defines
// that string (DOS2 / NieR d3_settings.xml, values + values-ru) gets its own text in the language of the Context given to configure(); a port
// without it gets the shared Ui.t() texts (RU / EN, Chinese through UiZh). The hints come from the host as resource ids (0 = the shared hint).
final class PadEditor {
    static final float SCALE_MIN = 0.5f, SCALE_MAX = 2.5f;   // DOOM's editor limits
    static final String PREFS = "doom_controls";

    static class Item {
        final String name, group;
        float x0, y0, r0;                        // the host's default centre and radius (px), before any player change
        float x, y, r;                           // placed (px): place() for plain items
        float dx, dy, ps = 1; boolean hidden;    // the player's place inside its group (fractions of the view), size, hide
        boolean square, fixed, noScale;          // drawn / hit as a square; can't be hidden (gear); has no size (a zone)
        boolean defHidden;                       // [front] hidden until the player adds it ("Add…": the stand-alone L3 / R3)
        boolean canRename; String defLabel, custom;   // [front] "Rename…": the default label (also the Add list's name), the player's (null = default)
        String title;                            // [front] the Add list's name when not the label (L3: "L3 (left stick click)")
        RectF zone;                              // non-null: a zone / slider the host places itself
        Item(String name, String group) { this.name = name; this.group = group; }
    }
    interface Host {
        Item editHit(float x, float y);   // the item under a finger in the editor (hidden ones too), or null
        void editChanged();               // places changed: lay out again (the view is invalidated after)
        void editClosed(boolean saved);   // after Save / Cancel: leave the edit mode
    }

    private final View view; private final Host host; private final float dp; private final String tag;
    private String offPrefix = "", itemPrefix = "", splitKey = "split";
    private int hintGrouped, hintSeparate;
    private Context ui;
    private List<? extends Item> items = new ArrayList<>();
    final Map<String, float[]> off = new HashMap<>();
    private final Map<String, float[]> centre = new HashMap<>();   // group -> placed centre (default centre + offset)
    boolean split, open;

    PadEditor(View v, Host host, String tag) { view = v; this.host = host; this.tag = tag; dp = v.getResources().getDisplayMetrics().density; ui = v.getContext(); }
    // prefs keys and the hint texts (the port's string resource ids: Grouped / Separate; 0 = the shared hint) of the layout now shown;
    // ui = resources in the menu language
    void configure(Context ui, String offPrefix, String itemPrefix, String splitKey, int hintGrouped, int hintSeparate) {
        this.ui = ui != null ? ui : view.getContext(); this.offPrefix = offPrefix; this.itemPrefix = itemPrefix; this.splitKey = splitKey;
        this.hintGrouped = hintGrouped; this.hintSeparate = hintSeparate; texts.clear();
    }
    void configure(Context ui, String offPrefix, String itemPrefix, String splitKey) { configure(ui, offPrefix, itemPrefix, splitKey, 0, 0); }

    // ---------------- texts ----------------
    // The port's string resource <res> (looked up by name in ui, so the shared file needs no R class), else the shared ru / en pair (Ui.t).
    // Cached per configure(): draw() asks for the toolbar names every frame.
    private final Map<String, String> texts = new HashMap<>();
    String text(String res, String ru, String en) {
        String s = texts.get(res);
        if (s == null) {
            int id = 0;
            try { id = ui.getResources().getIdentifier(res, "string", ui.getPackageName()); } catch (RuntimeException e) { id = 0; }
            if (id != 0) { try { s = ui.getString(id); } catch (RuntimeException e) { s = null; } }
            if (s == null) s = Ui.t(view.getContext(), ru, en);
            texts.put(res, s);
        }
        return s;
    }
    private String renameMsg(String def) {   // pad_ed_rename_msg: "%1$d" = PadEdit.MAX_LABEL, "%2$s" = the default label
        String f = text("pad_ed_rename_msg", "До %1$d символов; пусто — как было (%2$s)", "Up to %1$d characters; empty = the default (%2$s)");
        try { return String.format(Locale.ROOT, f, PadEdit.MAX_LABEL, def); } catch (RuntimeException e) { return f; }
    }
    private String hint() {
        int hr = split ? hintSeparate : hintGrouped;
        if (hr != 0) { try { return ui.getString(hr); } catch (RuntimeException e) { /* the shared one */ } }
        // the shared hint (D3's editor texts, so UiZh has both halves); not looked up by resource name: a port's pad_ed_hint_* are whole hints
        Context c = view.getContext();
        return (split ? Ui.t(c, "Тяните кнопку пальцем (каждая отдельно)", "Drag a button (every button on its own)")
                      : Ui.t(c, "Тяните кнопку пальцем (группа — целиком)", "Drag a button (its group moves as one)"))
            + Ui.t(c, ", второй палец — размер; −/+ без выбора меняют все; «Скрыть» убирает выбранное из игры; «Добавить…» — L3 / R3 и скрытые; «Имя…» — своя подпись",
                   "; a second finger resizes it; − / + with none chosen resize all; Hide takes the chosen out of the game; Add… brings L3 / R3 and hidden ones; Name… sets a label");
    }
    void setItems(List<? extends Item> l) { items = l; sel = null; }
    private SharedPreferences prefs() { return view.getContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE); }
    String offKey(String group) { return offPrefix + group; }
    String itemKey(String name) { return itemPrefix + name; }
    String labelKey(String name) { return itemPrefix + name + "_lbl"; }

    // ---------------- places ----------------
    List<String> groups() { List<String> g = new ArrayList<>(); for (Item b : items) if (!g.contains(b.group)) g.add(b.group); return g; }
    static float clampS(float s) { return Float.isNaN(s) ? 1 : Math.max(SCALE_MIN, Math.min(SCALE_MAX, s)); }
    float[] off(String group) { return off.get(group); }
    float gscale(String group) { float[] o = off.get(group); return o == null ? 1 : o[2]; }
    float[] centre(String group) { return centre.get(group); }
    // group offsets "fx,fy[,scale]" (2 fields = scale 1), every item's "dx;dy;scale;hidden", Separate
    void load(SharedPreferences p) {
        off.clear();
        for (String g : groups()) {
            String v = p.getString(offKey(g), null); if (v == null) continue;
            String[] f = v.split(",");
            try { if (f.length == 2 || f.length == 3) off.put(g, new float[] {Float.parseFloat(f[0]), Float.parseFloat(f[1]), f.length == 3 ? clampS(Float.parseFloat(f[2])) : 1}); }
            catch (NumberFormatException e) { /* ignore */ }
        }
        for (Item b : items) {
            b.dx = b.dy = 0; b.ps = 1; b.hidden = b.defHidden && !b.fixed; b.custom = b.canRename ? PadEdit.clean(p.getString(labelKey(b.name), null)) : null;
            String v = p.getString(itemKey(b.name), null); if (v == null) continue;
            String[] f = v.split(";");
            try { if (f.length >= 3) { b.dx = Float.parseFloat(f[0]); b.dy = Float.parseFloat(f[1]); b.ps = clampS(Float.parseFloat(f[2])); if (f.length >= 4) b.hidden = "1".equals(f[3]) && !b.fixed; } }
            catch (NumberFormatException e) { b.dx = b.dy = 0; b.ps = 1; b.hidden = b.defHidden && !b.fixed; }
            if (b.zone != null) { b.dx = b.dy = 0; b.ps = 1; }
        }
        split = p.getBoolean(splitKey, false);
    }
    void save() {
        SharedPreferences.Editor ed = prefs().edit();
        for (String g : groups()) {
            float[] o = off.get(g);
            if (o == null || (o[0] == 0 && o[1] == 0 && o[2] == 1)) ed.remove(offKey(g));
            else ed.putString(offKey(g), o[2] == 1 ? String.format(Locale.ROOT, "%.4f,%.4f", o[0], o[1]) : String.format(Locale.ROOT, "%.4f,%.4f,%.4f", o[0], o[1], o[2]));
        }
        for (Item b : items) {
            if (b.zone != null) { if (b.hidden) ed.putString(itemKey(b.name), "0;0;1;1"); else ed.remove(itemKey(b.name)); continue; }
            if (b.dx == 0 && b.dy == 0 && b.ps == 1 && b.hidden == (b.defHidden && !b.fixed)) ed.remove(itemKey(b.name));
            else ed.putString(itemKey(b.name), String.format(Locale.ROOT, "%.5f;%.5f;%.4f;%d", b.dx, b.dy, b.ps, b.hidden ? 1 : 0));
            if (b.custom == null) ed.remove(labelKey(b.name)); else ed.putString(labelKey(b.name), b.custom);
        }
        ed.apply();
    }
    void clear() { off.clear(); for (Item b : items) { b.dx = b.dy = 0; b.ps = 1; b.hidden = b.defHidden && !b.fixed; b.custom = null; } }
    // Places every plain item: its own place inside the group, then the group's scale about its default centre, then the group's offset.
    // The host sets x0 / y0 / r0 first; zone items are the host's.
    void place(int w, int h) {
        Map<String, float[]> piv = new HashMap<>();   // a group's default centre (sum x, sum y, n)
        for (Item b : items) {
            if (b.zone != null) continue;
            float[] s = piv.get(b.group); if (s == null) piv.put(b.group, s = new float[3]);
            s[0] += b.x0; s[1] += b.y0; s[2]++;
        }
        centre.clear();
        for (Map.Entry<String, float[]> en : piv.entrySet()) {
            float[] s = en.getValue(), o = off.get(en.getKey());
            centre.put(en.getKey(), new float[] {s[0] / s[2] + (o == null ? 0 : o[0] * w), s[1] / s[2] + (o == null ? 0 : o[1] * h)});
        }
        for (Item b : items) {
            if (b.zone != null) continue;
            float[] s = piv.get(b.group), o = off.get(b.group); float px = s[0] / s[2], py = s[1] / s[2], gs = o == null ? 1 : o[2];
            float x = px + (b.x0 + b.dx * w - px) * gs, y = py + (b.y0 + b.dy * h - py) * gs;
            if (o != null) { x += o[0] * w; y += o[1] * h; }
            b.x = x; b.y = y; b.r = b.r0 * b.ps * gs;
        }
    }

    // ---------------- edit mode ----------------
    private static final int ED_BUTTONS = 9, ED_HIDE = 3, ED_ADD = 4, ED_RENAME = 5;
    private int edPtr = -1, edPtr2 = -1, edBtnDown = -1; private Item sel;
    private float edX0, edY0, edD0, edDx0, edDy0, edPs0; private float[] edO0 = {0, 0, 1};
    private final Map<String, float[]> savedOff = new HashMap<>(); private float[][] savedItem = new float[0][]; private String[] savedLabel = new String[0];
    private final RectF[] edRects = new RectF[ED_BUTTONS]; private final RectF selRect = new RectF();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    { line.setStyle(Paint.Style.STROKE); text.setTextAlign(Paint.Align.CENTER); }

    void start() {
        open = true; sel = null; edPtr = edPtr2 = edBtnDown = -1;
        savedOff.clear(); for (Map.Entry<String, float[]> en : off.entrySet()) savedOff.put(en.getKey(), en.getValue().clone());
        savedItem = new float[items.size()][]; savedLabel = new String[items.size()];
        for (int i = 0; i < items.size(); i++) { Item b = items.get(i); savedItem[i] = new float[] {b.dx, b.dy, b.ps, b.hidden ? 1 : 0}; savedLabel[i] = b.custom; }
        view.invalidate();
    }
    // save: write every place / size / hide (Save); else back to what it was when the editor opened (Cancel, Back)
    void close(boolean save) {
        if (!open) return;
        if (save) save();
        else {
            off.clear(); for (Map.Entry<String, float[]> en : savedOff.entrySet()) off.put(en.getKey(), en.getValue().clone());
            for (int i = 0; i < items.size() && i < savedItem.length; i++) { Item b = items.get(i); float[] s = savedItem[i]; b.dx = s[0]; b.dy = s[1]; b.ps = s[2]; b.hidden = s[3] != 0; b.custom = i < savedLabel.length ? savedLabel[i] : null; }
        }
        open = false; edPtr = edPtr2 = edBtnDown = -1; sel = null;
        host.editChanged(); host.editClosed(save); view.invalidate();
    }
    boolean inSel(Item b) { return open && sel != null && (b == sel || (!split && b.group.equals(sel.group))); }
    boolean anySel() { return open && sel != null; }
    private boolean selOne() { return sel != null && split && sel.zone == null; }   // Separate: the chosen item alone
    private void grab(float x, float y) {   // the chosen control's place / size when the finger went down (or the pinch changed)
        edX0 = x; edY0 = y;
        float[] o = sel == null ? null : off.get(sel.group); edO0 = o == null ? new float[] {0, 0, 1} : o.clone();
        if (sel != null) { edDx0 = sel.dx; edDy0 = sel.dy; edPs0 = sel.ps; }
    }
    private void moveTo(float x, float y) {
        int w = view.getWidth(), h = view.getHeight(); if (w <= 0 || h <= 0 || sel == null) return;
        float ddx = x - edX0, ddy = y - edY0;
        if (selOne()) { float gs = gscale(sel.group); sel.dx = edDx0 + ddx / (gs * w); sel.dy = edDy0 + ddy / (gs * h); }
        else off.put(sel.group, new float[] {edO0[0] + ddx / w, edO0[1] + ddy / h, edO0[2]});
        host.editChanged();
    }
    private void scale(float f) {   // f relative to the grab; a control without a size keeps it
        if (selOne()) sel.ps = clampS(edPs0 * f);
        else if (sel != null && !sel.noScale) off.put(sel.group, new float[] {edO0[0], edO0[1], clampS(edO0[2] * f)});
        host.editChanged();
    }
    private void scaleAll(float f) {
        for (String g : groups()) {
            boolean fixedSize = false; for (Item b : items) if (b.group.equals(g) && b.noScale) fixedSize = true;
            if (fixedSize) continue;
            float[] o = off.get(g); off.put(g, o == null ? new float[] {0, 0, clampS(f)} : new float[] {o[0], o[1], clampS(o[2] * f)});
        }
        host.editChanged();
    }
    private boolean canHide() { if (sel != null) for (Item b : items) if (inSel(b) && !b.fixed) return true; return false; }
    private boolean selHidden() {   // the toolbar says Show when everything chosen is hidden
        boolean any = false;
        if (sel != null) for (Item b : items) if (inSel(b) && !b.fixed) { if (!b.hidden) return false; any = true; }
        return any;
    }
    private void toggleHide() {
        if (!canHide()) return;
        boolean hide = !selHidden();
        for (Item b : items) if (inSel(b) && !b.fixed) b.hidden = hide;
        host.editChanged();
    }
    // [front] Add…: the hidden items (defHidden ones, the stand-alone L3 / R3, first; a group is one row while grouped)
    private String title(Item b) { String d = b.title != null ? b.title : b.defLabel != null ? b.defLabel : b.name; return b.custom != null ? b.custom + " (" + d + ")" : d; }
    private void add() {
        final List<Item> pick = new ArrayList<>(); List<String> labels = new ArrayList<>(), seen = new ArrayList<>();
        for (int pass = 0; pass < 2; pass++) for (Item b : items) {
            if (!b.hidden || b.fixed || b.defHidden != (pass == 0)) continue;
            boolean asGroup = !split && b.zone == null && groupSize(b.group) > 1;
            if (asGroup) { if (seen.contains(b.group)) continue; seen.add(b.group); }
            pick.add(b); labels.add(asGroup ? groupTitle(b.group) : title(b));
        }
        PadEdit.addMenu(view.getContext(), text("pad_ed_add_title", "Добавить кнопку", "Add a button"), labels.toArray(new String[0]), text("pad_ed_add_none", "Все кнопки уже на экране", "Every button is on the screen already"), i -> {
            if (!open || i < 0 || i >= pick.size()) return;
            sel = pick.get(i);
            for (Item b : items) if (inSel(b) && !b.fixed) b.hidden = false;
            android.util.Log.i("dm", "[java] " + tag + " layout editor: added " + sel.name);
            host.editChanged(); view.invalidate();
        });
    }
    private int groupSize(String g) { int n = 0; for (Item b : items) if (b.group.equals(g)) n++; return n; }
    private String groupTitle(String g) {
        StringBuilder s = new StringBuilder();
        for (Item b : items) if (b.group.equals(g)) { if (s.length() > 0) s.append(' '); s.append(b.custom != null ? b.custom : b.defLabel != null ? b.defLabel : b.name); }
        return s.toString();
    }
    // [front] Rename…: the chosen item's own label
    private void rename() {
        final Item b = sel; Context c = view.getContext();
        if (b == null || !b.canRename) { android.widget.Toast.makeText(c, text("pad_ed_pick_first", "Сначала выберите кнопку", "Choose a button first"), android.widget.Toast.LENGTH_SHORT).show(); return; }
        String d = b.defLabel != null ? b.defLabel : b.name;
        PadEdit.rename(c, text("pad_ed_rename_title", "Имя кнопки", "Button name"), renameMsg(d), b.custom, d,
            text("pad_ed_ok", "Готово", "OK"), text("pad_ed_default", "По умолчанию", "Default"), text("pad_ed_cancel", "Отмена", "Cancel"), t -> {
                if (!open) return;
                b.custom = PadEdit.clean(t);
                android.util.Log.i("dm", "[java] " + tag + " layout editor: " + b.name + " label " + (b.custom == null ? "default" : "set"));
                host.editChanged(); view.invalidate();
            });
    }
    private RectF button(int i) {   // the toolbar (DOOM's look), centred a little above the middle where the layouts keep no buttons
        float W = view.getWidth(), H = view.getHeight(), gap = H * 0.02f, bw = Math.min(H * 0.17f, (W - 32 * dp - (ED_BUTTONS - 1) * gap) / ED_BUTTONS), bh = H * 0.085f;
        float total = ED_BUTTONS * bw + (ED_BUTTONS - 1) * gap, x0 = (W - total) / 2, y0 = H * 0.30f;
        if (edRects[i] == null) edRects[i] = new RectF();
        edRects[i].set(x0 + i * (bw + gap), y0, x0 + i * (bw + gap) + bw, y0 + bh); return edRects[i];
    }
    private int buttonAt(float x, float y) { for (int i = 0; i < ED_BUTTONS; i++) if (button(i).contains(x, y)) return i; return -1; }
    private boolean action(int bt) {   // true = the editor closed
        switch (bt) {
        case 0: case 1: { float f = bt == 1 ? 1.1f : 1 / 1.1f; if (sel == null || sel.noScale) scaleAll(f); else { grab(edX0, edY0); scale(f); } break; }
        case 2: split = !split; prefs().edit().putBoolean(splitKey, split).apply();
            android.util.Log.i("dm", "[java] " + tag + " layout editor: " + (split ? "every button on its own" : "groups move as one")); break;
        case ED_HIDE: toggleHide(); break;
        case ED_ADD: view.post(this::add); break;
        case ED_RENAME: view.post(this::rename); break;
        case 6: clear(); sel = null; host.editChanged(); break;
        case 7: close(false); return true;
        default: close(true); return true;
        }
        return false;
    }
    void touch(MotionEvent e) {
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) {
            int bt = buttonAt(x, y);
            if (bt >= 0 && edPtr < 0) { edBtnDown = bt; edPtr = id; }
            else if (edPtr >= 0 && edBtnDown < 0 && edPtr2 < 0 && sel != null && !sel.noScale) {   // a second finger pinches the chosen control
                int k1 = e.findPointerIndex(edPtr);
                if (k1 >= 0) { edD0 = (float) Math.hypot(x - e.getX(k1), y - e.getY(k1)); edPtr2 = id; grab(e.getX(k1), e.getY(k1)); }
            } else if (edPtr < 0) {   // an item (hidden ones too); elsewhere = none chosen
                sel = host.editHit(x, y); edBtnDown = -1;
                edPtr = sel != null ? id : -1;
                if (sel != null) grab(x, y);
            }
        } else if (a == MotionEvent.ACTION_MOVE) {
            if (edPtr >= 0 && edBtnDown < 0 && sel != null) {
                int k1 = e.findPointerIndex(edPtr), k2 = edPtr2 >= 0 ? e.findPointerIndex(edPtr2) : -1;
                if (k1 >= 0 && k2 >= 0 && edD0 > 1) scale((float) Math.hypot(e.getX(k1) - e.getX(k2), e.getY(k1) - e.getY(k2)) / edD0);
                else if (k1 >= 0) moveTo(e.getX(k1), e.getY(k1));
            }
        } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_POINTER_UP || a == MotionEvent.ACTION_CANCEL) {
            if (a == MotionEvent.ACTION_POINTER_UP && id == edPtr2) {   // the pinch ends: the drag goes on from where the first finger is now
                edPtr2 = -1; int k1 = e.findPointerIndex(edPtr); if (k1 >= 0) grab(e.getX(k1), e.getY(k1));
            } else if (id == edPtr || a != MotionEvent.ACTION_POINTER_UP) {
                int bt = edBtnDown; edPtr = edPtr2 = edBtnDown = -1;
                if (bt >= 0 && a != MotionEvent.ACTION_CANCEL && bt == buttonAt(x, y) && action(bt)) return;
            }
        }
        view.invalidate();
    }

    // ---------------- drawing ----------------
    // the editor's look of an item: 1 chosen, else at least half the opacity; hidden ones at 40 % of that
    float alpha(Item b, float op) { return (inSel(b) ? 1 : Math.max(op, 0.5f)) * (b.hidden ? 0.4f : 1); }
    void strike(Canvas c, float cx, float cy, float r) {   // the mark on a hidden control: a red slash across it
        line.setStrokeWidth(2 * dp); line.setColor(0xffff5050); line.setAlpha(0xe0); float d = r * 0.75f; c.drawLine(cx - d, cy + d, cx + d, cy - d, line);
    }
    private void bounds(Item b, RectF out) { if (b.zone != null) out.set(b.zone); else out.set(b.x - b.r, b.y - b.r, b.x + b.r, b.y + b.r); }
    private final RectF tmp = new RectF();
    void draw(Canvas c) {   // DOOM's toolbar look (DoomPadView.drawEditor), the chosen control framed
        boolean sh = selHidden(), ch = canHide();
        boolean cr = sel != null && sel.canRename;
        String[] names = {"−", "+", split ? text("pad_ed_separate", "Раздельно", "Separate") : text("pad_ed_grouped", "Группами", "Grouped"), sh ? text("pad_ed_show", "Показать", "Show") : text("pad_ed_hide", "Скрыть", "Hide"),
            text("pad_ed_add", "Добавить…", "Add…"), text("pad_ed_rename", "Имя…", "Rename…"), text("pad_ed_reset", "Сброс", "Reset"), text("pad_ed_cancel", "Отмена", "Cancel"), text("pad_ed_save", "Сохранить", "Save")};
        float H = view.getHeight(), W = view.getWidth();
        line.setStrokeWidth(1.5f * dp);
        for (int i = 0; i < ED_BUTTONS; i++) {
            RectF r = button(i);
            fill.setColor(i == edBtnDown ? 0xff1fb6c9 : i == ED_BUTTONS - 1 ? 0xe0157c8a : 0xd0202833); line.setColor(0xff7fe3f0);
            float rad = r.height() / 2; c.drawRoundRect(r, rad, rad, fill); c.drawRoundRect(r, rad, rad, line);
            float ts = H * 0.04f; text.setTextSize(ts); float tw = text.measureText(names[i]); if (tw > r.width() * 0.9f) { ts *= r.width() * 0.9f / tw; text.setTextSize(ts); }
            text.setColor(0xffffffff); text.setAlpha((i == ED_HIDE && !ch) || (i == ED_RENAME && !cr) ? 0x60 : 0xff);   // Hide / Rename are grey while nothing they act on is chosen
            c.drawText(names[i], r.centerX(), r.centerY() + ts * 0.35f, text);
        }
        line.setStrokeWidth(2 * dp);
        selRect.setEmpty();
        if (sel != null) for (Item b : items) if (inSel(b)) { bounds(b, tmp); selRect.union(tmp); }
        if (!selRect.isEmpty()) {   // one frame around the chosen control, or around its whole group
            line.setColor(0xff7fe3f0); selRect.inset(-4 * dp, -4 * dp); c.drawRoundRect(selRect, 12 * dp, 12 * dp, line);
        }
        String hint = hint();
        float ts = H * 0.032f; text.setTextSize(ts); float tw = text.measureText(hint); if (tw > W * 0.96f) { ts *= W * 0.96f / tw; text.setTextSize(ts); }
        text.setColor(0xffffffff); text.setAlpha(220);
        c.drawText(hint, W / 2f, button(0).bottom + H * 0.06f, text);
    }
}
