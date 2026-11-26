package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Paint;
import android.text.InputFilter;
import android.text.InputType;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.EditText;
import android.widget.Toast;
import java.util.function.Consumer;
import java.util.function.IntConsumer;

// [front] The layout editors' dialogs for players' own controls (frontend-common; DoomPadView's editor and the PadEditor of the D3 / TL2 / DOS2
// trees, owner request 15.11.2026): "Add button" (a list of the hidden buttons: the stand-alone L3 / R3, hidden by default, and any the player hid)
// and "Rename…" (a button's own label, at most MAX_LABEL characters, empty = the default glyph / letter). Every text comes from the caller (the
// shared pad: Ui.t RU / EN; a port's editor: its string resources), so the class has no language of its own. Callbacks run on the UI thread;
// after a dialog the game's window gets its immersive bars back (as DmActivity.immersiveBars).
final class PadEdit {
    static final int MAX_LABEL = 8;
    private PadEdit() {}

    // the hidden buttons the player may add back; pick(i) gets the index into labels
    static void addMenu(Context c, String title, String[] labels, String none, IntConsumer pick) {
        Activity a = activity(c); if (a == null) return;
        if (labels.length == 0) { Toast.makeText(a, none, Toast.LENGTH_SHORT).show(); return; }
        show(a, new AlertDialog.Builder(a).setTitle(title).setItems(labels, (d, w) -> pick.accept(w)).setNegativeButton(cancelText(c), null).create());
    }
    // the label editor of one button: done(null) = the default (Default, or an empty / default text); done(text) = the player's label
    static void rename(Context c, String title, String message, String current, String defLabel, String ok, String reset, String cancel, Consumer<String> done) {
        Activity a = activity(c); if (a == null) return;
        EditText in = new EditText(a);
        in.setSingleLine(true); in.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        in.setFilters(new InputFilter[] {new InputFilter.LengthFilter(MAX_LABEL)});
        in.setHint(defLabel); in.setText(current != null ? current : ""); in.setSelection(in.length());
        AlertDialog d = new AlertDialog.Builder(a).setTitle(title).setMessage(message).setView(in)
            .setPositiveButton(ok, (x, w) -> { String t = in.getText().toString().trim(); done.accept(t.isEmpty() || t.equals(defLabel) ? null : t); })
            .setNeutralButton(reset, (x, w) -> done.accept(null))
            .setNegativeButton(cancel, null).create();
        show(a, d);
    }
    // a label scaled down until it fits maxW (px), from the size the caller set on p; the size used is returned
    static float fit(Paint p, String s, float maxW) {
        float ts = p.getTextSize(), w = p.measureText(s);
        if (w > maxW && w > 0) { ts *= maxW / w; p.setTextSize(ts); }
        return ts;
    }
    // a player's label: 1..MAX_LABEL characters, no line breaks; anything else = none
    static String clean(String s) {
        if (s == null) return null;
        s = s.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ').trim();
        if (s.isEmpty()) return null;
        return s.length() > MAX_LABEL ? s.substring(0, MAX_LABEL) : s;
    }

    private static String cancelText(Context c) { return Ui.t(c, "Отмена", "Cancel"); }
    private static Activity activity(Context c) {
        while (c instanceof android.content.ContextWrapper && !(c instanceof Activity)) c = ((android.content.ContextWrapper) c).getBaseContext();
        Activity a = c instanceof Activity ? (Activity) c : null;
        return a == null || a.isFinishing() || a.isDestroyed() ? null : a;
    }
    private static void show(Activity a, AlertDialog d) { d.setOnDismissListener(x -> immersive(a)); d.show(); }
    static void immersive(Activity a) {   // = DmActivity.immersiveBars: the system bars hidden again after a dialog
        if (android.os.Build.VERSION.SDK_INT < 30) {
            a.getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            return;
        }
        WindowInsetsController ic = a.getWindow().getInsetsController();
        if (ic != null) { ic.hide(WindowInsets.Type.systemBars()); ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }
}
