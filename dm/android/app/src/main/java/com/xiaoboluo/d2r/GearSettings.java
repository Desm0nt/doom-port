package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// [front] The gear (settings) dialog of every port (frontend-common, docs/FRONTEND_UNIFIED.md "Gear"; the pause menu's "Game settings…" and
// the pad's gear when a port has no pause menu). It is DOOM 2016's gear dialog made generic: one scrolling list of sections, Save / Cancel,
// each option labelled with when it applies ("applies immediately" / "applies on next launch", gear_label_fmt). Two kinds of options:
//   next launch: kept in GearHost.gamePrefs ("doom_graphics") and handed to the native host as environment variables before the library loads
//     (applyEnv: every section's env(), in the dialog's order; dm/env still wins where the native side reads it after);
//   immediately: the pad's prefs (FrontMenu.Pad.prefsName, "doom_controls", read by the pad's reloadSettings) and live native switches
//     (the port's sections call their JNI in after()).
// A Section builds its views (build), writes its prefs on Save (save: into the two shared editors, applied together, then the pad reloads),
// does its live work after that (after: JNI, toasts) and sets its environment at the start (env). The generic sections are GearPages; a port
// adds its own (DOOM 2016: dynamic resolution, light mode, aim assist, the game folder) with the helpers here, and may group sections into
// sub-pages (page(): a button that opens them as a dialog of their own with its own Save).
// Texts: GearText (13 languages, s()) for the generic ones, Ui.t (Russian / English) where DOOM 2016 had only those, and the port's string
// resources through str() - in the same language as the rest: the resources of ui(), a context in the menu language when one is picked.
// A port with GearHost.gearUsesPortResources (DOOM 2016) gets its own resources for both, in the system locale, exactly as before the move.
final class GearSettings {
    interface Section {
        void build(GearSettings g, LinearLayout box);                                                    // add the views
        default void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) {}  // Save: write prefs (not applied yet)
        default void after(GearSettings g) {}                                                            // after the prefs are applied: live calls, toasts
        default void env(GearSettings g) throws ErrnoException {}                                       // at the start, before the library loads
    }

    final GearHost host; final Activity a; final Context ui;
    private int nextId = 10000;
    private AlertDialog dlg;

    GearSettings(GearHost h) {
        host = h; a = h.activity();
        Context c = a;
        String l = h.gearUsesPortResources() ? "" : a.getSharedPreferences(Ui.PREFS, Context.MODE_PRIVATE).getString("ui_lang", "");
        // the menu language picked in the pause menu: the port's resources in it too (Diablo III's d3Ui); not for a port whose gear keeps its
        // resources' own language (GearHost.gearUsesPortResources: DOOM 2016's dialog always followed the system locale)
        if (!l.isEmpty()) {
            android.content.res.Configuration cf = new android.content.res.Configuration(a.getResources().getConfiguration());
            cf.setLocale(Locale.forLanguageTag(l)); c = a.createConfigurationContext(cf);
        }
        ui = c;
    }

    // ---- texts
    String s(String key, Object... args) { return GearText.s(a, key, args); }
    String str(int res, Object... args) { return args.length == 0 ? ui.getString(res) : ui.getString(res, args); }
    String t(String ru, String en) { return Ui.t(a, ru, en); }
    // "<label> (applies immediately)" / "<label> (applies on next launch)", in the locale's own punctuation (gear_label_fmt)
    String when(String label, boolean now) { return s("gear_label_fmt", label, s(now ? "gear_when_now" : "gear_when_next")); }
    String whenKey(String key, boolean now) { return when(s(key), now); }

    // ---- prefs
    SharedPreferences gamePrefs() { return host.gamePrefs(); }
    SharedPreferences padPrefs() { FrontMenu.Pad p = host.pad(); return a.getSharedPreferences(p != null ? p.prefsName() : DoomPadView.PREFS, Context.MODE_PRIVATE); }
    void reloadPad() { FrontMenu.Pad p = host.pad(); if (p != null) p.reloadSettings(); }

    // ---- view helpers (DOOM 2016's look: plain TextView headings with top padding, radio groups, check boxes; notes at 70 % for D3 / XC3)
    TextView heading(LinearLayout box, String text) { TextView t = new TextView(a); t.setText(text); t.setPadding(0, 24, 0, 0); box.addView(t); return t; }
    TextView text(LinearLayout box, String text) { TextView t = new TextView(a); t.setText(text); box.addView(t); return t; }
    TextView note(LinearLayout box, String text) { TextView t = text(box, text); t.setAlpha(0.7f); return t; }
    CheckBox check(LinearLayout box, String label, boolean on) { CheckBox c = new CheckBox(a); c.setText(label); c.setChecked(on); box.addView(c); return c; }
    Button button(LinearLayout box, String label) { Button b = new Button(a); b.setText(label); box.addView(b); return b; }
    LinearLayout vbox(LinearLayout parent) { LinearLayout b = new LinearLayout(a); b.setOrientation(LinearLayout.VERTICAL); if (parent != null) parent.addView(b); return b; }
    // A radio group (ids base..base+n-1, the base kept as its tag for picked); checked is clamped to the labels
    RadioGroup radios(LinearLayout box, String[] labels, int checked, boolean horizontal) {
        int base = nextId; nextId += labels.length + 1;
        RadioGroup g = new RadioGroup(a); g.setOrientation(horizontal ? RadioGroup.HORIZONTAL : RadioGroup.VERTICAL); g.setTag(base);
        for (int i = 0; i < labels.length; i++) { RadioButton r = new RadioButton(a); r.setText(labels[i]); r.setId(base + i); g.addView(r); }
        if (labels.length > 0) g.check(base + Math.min(Math.max(0, checked), labels.length - 1));
        box.addView(g); return g;
    }
    // The picked index of a radios() group, 0..n-1 (n = its labels)
    static int picked(RadioGroup g) {
        int base = g.getTag() instanceof Integer ? (Integer) g.getTag() : 0, n = g.getChildCount();
        return Math.min(Math.max(0, g.getCheckedRadioButtonId() - base), Math.max(0, n - 1));
    }
    String[] keys(String... k) { String[] s = new String[k.length]; for (int i = 0; i < k.length; i++) s[i] = s(k[i]); return s; }
    static int nearest(float[] v, float x) { int b = 0; for (int i = 1; i < v.length; i++) if (Math.abs(v[i] - x) < Math.abs(v[b] - x)) b = i; return b; }
    static int indexOf(String[] v, String x) { for (int i = 0; i < v.length; i++) if (v[i].equals(x)) return i; return 0; }
    static int indexOf(int[] v, int x, int dflt) { for (int i = 0; i < v.length; i++) if (v[i] == x) return i; return dflt; }
    // A live slider for one int pref (min..max), saved at once (the pad shows it behind the dialog); fmt takes the value (%d)
    SeekBar slider(LinearLayout box, SharedPreferences p, String fmt, String key, int min, int max, int def, Runnable changed) {
        final int was = Math.max(min, Math.min(max, p.getInt(key, def)));
        final TextView label = text(box, String.format(Locale.ROOT, fmt, was));
        SeekBar s = new SeekBar(a); s.setMax(max - min); s.setProgress(was - min); box.addView(s);
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar sb, int v, boolean user) {
                label.setText(String.format(Locale.ROOT, fmt, v + min)); p.edit().putInt(key, v + min).apply();
                if (changed != null) changed.run();
            }
            public void onStartTrackingTouch(SeekBar sb) {}
            public void onStopTrackingTouch(SeekBar sb) {}
        });
        return s;
    }
    void toast(String s, boolean longer) { android.widget.Toast.makeText(a, s, longer ? android.widget.Toast.LENGTH_LONG : android.widget.Toast.LENGTH_SHORT).show(); }

    // ---- the dialog
    // The main dialog (the port's gameSettings / showDoomSettings): the sections GearHost.gearSections lists, on the UI thread
    void show() { a.runOnUiThread(() -> showPage(host.gearTitle(this), host.gearSections(this))); }
    // Close the dialog (a section's button that leaves it, e.g. "Edit the layout…")
    void dismiss() { if (dlg != null) try { dlg.dismiss(); } catch (RuntimeException e) { /* gone */ } }
    // Reopen it (a section that changes the dialog's own language): saved state is kept, the views are made anew
    void reopen() { dismiss(); new GearSettings(host).show(); }

    private void showPage(String title, List<Section> sections) {   // one dialog per GearSettings (a sub-page has its own instance)
        LinearLayout box = new LinearLayout(a); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
        for (Section s : sections) s.build(this, box);
        ScrollView scroll = new ScrollView(a); scroll.addView(box);
        AlertDialog d = new AlertDialog.Builder(a).setTitle(title).setView(scroll)
            .setPositiveButton(s("gear_save"), (x, w) -> save(sections))
            .setNegativeButton(s("gear_cancel"), null).create();
        dlg = d;
        d.show();
    }
    private void save(List<Section> sections) {
        SharedPreferences.Editor ge = gamePrefs().edit(), pe = padPrefs().edit();
        for (Section s : sections) s.save(this, ge, pe);
        ge.apply(); pe.apply();
        reloadPad();
        for (Section s : sections) s.after(this);
    }
    // A sub-page: a button "<title>…" in the dialog that opens these sections as a dialog of their own (its own Save / Cancel). Their env()
    // runs at the start like the top-level ones.
    static Section page(String title, Section... sections) {
        final List<Section> list = java.util.Arrays.asList(sections);
        return new Section() {
            public void build(GearSettings g, LinearLayout box) {
                Button b = g.button(box, title + "…");
                b.setOnClickListener(v -> new GearSettings(g.host).showPage(title, list));
            }
            public void env(GearSettings g) throws ErrnoException { for (Section s : list) s.env(g); }
        };
    }

    // ---- the start: every section's environment, before the library loads (the port's applyDoomSettings / applySettings)
    static void applyEnv(GearHost h) {
        GearSettings g = new GearSettings(h);
        List<Section> all = new ArrayList<>(h.gearSections(g));
        for (Section s : all) {
            try { s.env(g); } catch (ErrnoException | RuntimeException e) { android.util.Log.e("dm", "gear settings env (" + s.getClass().getSimpleName() + "): " + e); }
        }
    }
}
