package com.xiaoboluo.d2r;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;

// [front] Save export / import for the d2r-mobile based ports (frontend-common, the same file in every port), the way nx2android's saves_android.c does it:
//   the game's saves: one folder (DM_SAVE = <files>/save in jni_android.c nativeStart: the host's save data mounts live under it);
//   export: the whole folder is copied to <exports>/<yyyyMMdd-HHmmss>/ (first to <name>.part, renamed when complete), while the game is paused;
//   import: never while the game runs (it keeps its save in memory and would write it back). The pause menu asks for it (requestImport: files/
//   save-import.txt names the export) and the app closes; at the next start, before the host is loaded (applyPending), the current folder is first
//   exported as <yyyyMMdd-HHmmss>-before-import, then the chosen export is copied next to it (save.import.part) and swapped in. Any failure leaves the
//   current saves as they were (the request is dropped and the reason shown).
// <exports> = Download/<the game's data folder>/saves when the app can write there (all files access), else <external files>/saves.
final class SaveTransfer implements FrontMenu.Saves {
    private final Context ctx; private final File save, exports;
    static final String REQUEST = "save-import.txt";

    SaveTransfer(Context c, File saveDir, File exportsDir) { ctx = c; save = saveDir; exports = exportsDir; }
    // the usual places: <files>/save, and Download/<dataFolder>/saves (or the external files folder's saves/ when that is not writable)
    static SaveTransfer forGame(Context c, String dataFolder) {
        File pub = new File(android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS), dataFolder + "/saves");
        File ext = c.getExternalFilesDir(null);
        boolean canPub = android.os.Environment.isExternalStorageManager() && (pub.isDirectory() || pub.mkdirs()) && pub.canWrite();
        File dir = canPub || ext == null ? pub : new File(ext, "saves");
        return new SaveTransfer(c, new File(c.getFilesDir(), "save"), dir);
    }
    @Override public String where() { return exports.getAbsolutePath(); }

    private static String stamp() { return new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()); }
    private static long[] count(File f) {   // {files, bytes}
        if (!f.isDirectory()) return new long[] {f.isFile() ? 1 : 0, f.length()};
        long[] n = {0, 0}; File[] l = f.listFiles(); if (l != null) for (File c : l) { long[] k = count(c); n[0] += k[0]; n[1] += k[1]; }
        return n;
    }
    private static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to);
            File[] l = from.listFiles(); if (l == null) throw new IOException("list " + from);
            for (File c : l) copyTree(c, new File(to, c.getName()));
            return;
        }
        byte[] buf = new byte[1 << 16];
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) { for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n); }
        if (to.length() != from.length()) throw new IOException("short copy " + to);
    }
    static void deleteTree(File f) { File[] l = f.isDirectory() ? f.listFiles() : null; if (l != null) for (File c : l) deleteTree(c); f.delete(); }
    private static String exportTo(File save, File exports, String name) throws IOException {
        if (count(save)[0] == 0) throw new IOException("no save yet");
        if (!exports.isDirectory() && !exports.mkdirs()) throw new IOException("cannot create " + exports);
        File part = new File(exports, name + ".part"), out = new File(exports, name);
        deleteTree(part); copyTree(save, part);
        if (out.exists() || !part.renameTo(out)) { deleteTree(part); throw new IOException("rename " + out); }
        long[] n = count(out);
        android.util.Log.i("dm", "[saves] exported " + save + " -> " + out + " (" + n[0] + " files, " + n[1] + " bytes)");
        return name;
    }

    // An export now (the game is paused): "" + its name, or "!" + why
    @Override public String export() {
        try { return exportTo(save, exports, stamp()); }
        catch (IOException e) { android.util.Log.e("dm", "[saves] export failed: " + e.getMessage()); return "!" + e.getMessage(); }
    }
    // The exports, newest first: "name\tfiles\tbytes"
    @Override public String[] list() {
        File[] d = exports.listFiles(); List<String> out = new ArrayList<>();
        if (d != null) { Arrays.sort(d, (a, b) -> b.getName().compareTo(a.getName()));
            for (File f : d) if (f.isDirectory() && !f.getName().endsWith(".part")) { long[] n = count(f); if (n[0] > 0) out.add(f.getName() + "\t" + n[0] + "\t" + n[1]); } }
        return out.toArray(new String[0]);
    }
    // Ask for an import at the next start: null when asked, else why not
    @Override public String requestImport(String name) {
        File src = new File(exports, name);
        if (!src.isDirectory() || count(src)[0] == 0) return "not found: " + src;
        try (OutputStream o = new FileOutputStream(new File(ctx.getFilesDir(), REQUEST))) { o.write(src.getAbsolutePath().getBytes(StandardCharsets.UTF_8)); }
        catch (IOException e) { return e.getMessage(); }
        android.util.Log.i("dm", "[saves] import of " + src + " requested for the next start");
        return null;
    }
    // At the start, before the host loads: a requested import. null = none; else the message for the player (it starts with "!" on failure)
    String applyPending() {
        File req = new File(ctx.getFilesDir(), REQUEST); if (!req.isFile()) return null;
        String path; try { path = new String(Files.readAllBytes(req.toPath()), StandardCharsets.UTF_8).trim(); } catch (IOException e) { path = ""; }
        req.delete();
        File src = new File(path), part = new File(save.getParentFile(), "save.import.part"), old = new File(save.getParentFile(), "save.before-import");
        try {
            if (!src.isDirectory() || count(src)[0] == 0) throw new IOException("the export is gone: " + path);
            String backup = null;
            if (count(save)[0] > 0) backup = exportTo(save, exports, stamp() + "-before-import");   // throws: nothing is touched
            deleteTree(part); copyTree(src, part);
            deleteTree(old);
            if (save.exists() && !save.renameTo(old)) throw new IOException("rename " + save);
            if (!part.renameTo(save)) { old.renameTo(save); throw new IOException("rename " + part); }
            deleteTree(old);
            android.util.Log.i("dm", "[saves] imported " + src + " -> " + save + (backup == null ? "" : " (the previous saves: " + backup + ")"));
            return Ui.t(ctx, "Сохранение импортировано: " + src.getName() + (backup == null ? "" : "\nПрежнее: saves/" + backup),
                "Save imported: " + src.getName() + (backup == null ? "" : "\nThe previous one: saves/" + backup));
        } catch (IOException e) {
            deleteTree(part);
            android.util.Log.e("dm", "[saves] import of " + path + " failed, the current saves are kept: " + e.getMessage());
            return "!" + Ui.t(ctx, "Импорт не удался, текущее сохранение не тронуто: ", "The import failed, the current save is kept: ") + e.getMessage();
        }
    }
}
