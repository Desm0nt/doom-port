package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// [DOOM] The player's game folder (GitHub issue #11, BUGS_AND_FIXES 2.28): "DOOM Mobile" with romfs/, dm/ (out, shaders, env, ...),
// optionally astc/, drivers/, and the logs/ and saves/ the app writes. It used to be Download/DOOM Mobile on the internal storage only.
//   auto search: <volume>/Download/DOOM Mobile (and <volume>/DOOM Mobile) on every storage volume: the internal storage, SD cards, USB
//     drives. API 30+: StorageManager.getStorageVolumes() + StorageVolume.getDirectory(); Android 10 (the minSdk 29 build): the roots of
//     getExternalFilesDirs(null), StorageVolume.getPathFile() by reflection (may be refused for targetSdk 36) and /storage/*. A volume that
//     cannot be listed (Android 10: removable volumes may be closed to apps) is skipped with a log line.
//   complete = romfs/ is a listable folder, the patched modules are in the runtime's out/ and a shader bundle (*.xgs) in its shaders/, where
//     the runtime is <files>/dm or <external files>/dm when it has out/main.patched.bin (a debug push), else <folder>/dm: the rule of
//     jni_android.c nativeStart (-2 / -3 / -4).
//   choice: the player's folder (pref "game_dir" in "doom_graphics", set in the gear dialog or on the start error screen) when complete,
//     else the internal storage's, else the first complete one. Every candidate and the choice go to the boot log (DmActivity.logFolder).
//   users: the source dir handed to nativeStart (romfs, dm runtime, env file, the ASTC overlay next to romfs), the driver import
//     (<folder>/drivers), CrashLog.dir (<folder>/logs) and the save exports (<folder>/saves).
final class GameFolder {
    static final String PREF = "game_dir", NAME = DmActivity.FRONT_DATA_DIR;
    static final int OK = 0, MISSING = 1, UNREADABLE = 2, NO_ROMFS = 3, NO_MODULES = 4, NO_SHADERS = 5;
    static final String[] MODULES = {"main.patched.bin", "main.fixups.tsv", "main.gates.tsv", "sdk.patched.bin", "sdk.fixups.tsv", "sdk.gates.tsv"};

    static final class Candidate {
        final File dir; final String kind; final int state;   // kind: "chosen", "internal", "SD card / USB (<label>)"
        Candidate(File d, String k, int s) { dir = d; kind = k; state = s; }
    }
    static final class Result {
        File dir;                    // the complete folder used, or null
        String how = "";             // "saved choice" / "internal" / "first complete"
        String saved = "";           // the player's folder ("" = automatic)
        int savedState = OK;         // its state when there is one
        final List<Candidate> all = new ArrayList<>();
        final List<String> notes = new ArrayList<>();   // volume lines for the log
        // The most complete candidate's state (for the start error: -2 / -3 / -4)
        int best() { int b = MISSING; for (Candidate c : all) if (c.state > b) b = c.state; return b; }
        String log() {
            StringBuilder sb = new StringBuilder();
            for (String n : notes) sb.append("game folder: ").append(n).append('\n');
            for (Candidate c : all) sb.append("game folder: candidate ").append(c.dir).append(" [").append(c.kind).append("]: ").append(stateName(c.state)).append('\n');
            sb.append("game folder: ").append(dir != null ? "using " + dir + " (" + how + ")" : "no complete game folder").append(saved.isEmpty() ? ", automatic search" : ", the player's choice " + saved);
            return sb.toString();
        }
    }

    private static volatile Result last;   // the last complete resolution of this process (kept: the running game's folder)

    private GameFolder() {}

    static SharedPreferences prefs(Context c) { return c.getSharedPreferences("doom_graphics", Context.MODE_PRIVATE); }
    static String saved(Context c) { return prefs(c).getString(PREF, ""); }
    // Download/DOOM Mobile on the internal storage: the old fixed place, and the fallback for logs / saves
    static File defaultDir() { return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), NAME); }

    static String stateName(int s) {
        switch (s) {
            case OK: return "complete"; case MISSING: return "not found"; case UNREADABLE: return "cannot be listed, skipped";
            case NO_ROMFS: return "no romfs"; case NO_MODULES: return "patched modules missing"; default: return "shaders missing";
        }
    }

    // The runtime folder nativeStart will use with this source folder (jni_android.c: internal, external private, then <src>/dm)
    static File runtime(Context c, File dir) {
        File internal = new File(c.getFilesDir(), "dm");
        if (new File(internal, "out/main.patched.bin").canRead()) return internal;
        File ext = c.getExternalFilesDir(null);
        if (ext != null && new File(ext, "dm/out/main.patched.bin").canRead()) return new File(ext, "dm");
        return new File(dir, "dm");
    }

    static int check(Context c, File dir) {
        if (dir == null || !dir.exists()) return MISSING;
        if (dir.list() == null) return UNREADABLE;
        File romfs = new File(dir, "romfs");
        if (!romfs.isDirectory()) return NO_ROMFS;
        if (romfs.list() == null) return UNREADABLE;
        File rt = runtime(c, dir);
        for (String m : MODULES) if (!new File(rt, "out/" + m).canRead()) return NO_MODULES;
        String[] sh = new File(rt, "shaders").list();
        if (sh != null) for (String s : sh) if (s.endsWith(".xgs")) return OK;
        return NO_SHADERS;
    }

    // The "DOOM Mobile" folder itself, or a folder that holds it
    static File normalize(File dir) {
        if (dir == null) return null;
        if (new File(dir, "romfs").isDirectory() || new File(dir, "dm").isDirectory()) return dir;
        File inner = new File(dir, NAME);
        return inner.isDirectory() ? inner : dir;
    }

    // ---- storage volumes: root -> label ("internal", "removable <description>")
    static Map<String, String> volumes(Context c, List<String> notes) {
        LinkedHashMap<String, String> out = new LinkedHashMap<>();
        File primary = Environment.getExternalStorageDirectory();
        out.put(primary.getAbsolutePath(), "internal");
        StorageManager sm = c.getSystemService(StorageManager.class);
        List<StorageVolume> vols = null;
        try { vols = sm == null ? null : sm.getStorageVolumes(); } catch (RuntimeException e) { notes.add("storage volumes: " + e); }
        if (vols != null) for (StorageVolume v : vols) {
            String desc; try { desc = v.getDescription(c); } catch (RuntimeException e) { desc = "?"; }
            File d = null;
            if (Build.VERSION.SDK_INT >= 30) d = v.getDirectory();
            else try { d = (File) StorageVolume.class.getMethod("getPathFile").invoke(v); }   // Android 10: hidden, may be refused
            catch (ReflectiveOperationException | RuntimeException e) { /* the other sources below */ }
            notes.add("volume \"" + desc + "\" uuid " + v.getUuid() + (v.isPrimary() ? " primary" : "") + (v.isRemovable() ? " removable" : "") + " state " + v.getState()
                + " -> " + (d != null ? d.getAbsolutePath() : "no directory"));
            if (d != null && !out.containsKey(d.getAbsolutePath())) out.put(d.getAbsolutePath(), v.isPrimary() ? "internal" : "removable " + desc);
        }
        // the app's own folder on each mounted volume: <root>/Android/data/<package>/files
        File[] own = null; try { own = c.getExternalFilesDirs(null); } catch (RuntimeException e) { notes.add("external files dirs: " + e); }
        if (own != null) for (File f : own) {
            if (f == null) continue;
            String p = f.getAbsolutePath(); int at = p.indexOf("/Android/data/");
            if (at > 0 && !out.containsKey(p.substring(0, at))) { out.put(p.substring(0, at), "removable"); notes.add("volume from the app's external files dir: " + p.substring(0, at)); }
        }
        String[] st = new File("/storage").list();   // SD cards and USB drives are /storage/<uuid>
        if (st != null) for (String n : st) {
            if (n.equals("emulated") || n.equals("self")) continue;
            String p = "/storage/" + n;
            if (!out.containsKey(p) && new File(p).isDirectory()) { out.put(p, "removable"); notes.add("volume from /storage: " + p); }
        }
        return out;
    }

    static Result resolve(Context c) {
        Result r = new Result();
        r.saved = saved(c);
        java.util.HashSet<String> seen = new java.util.HashSet<>();
        if (!r.saved.isEmpty()) {
            File d = new File(r.saved); r.savedState = check(c, d); seen.add(d.getAbsolutePath());
            r.all.add(new Candidate(d, "chosen", r.savedState));
            if (r.savedState == OK) { r.dir = d; r.how = "saved choice"; }
        }
        for (Map.Entry<String, String> v : volumes(c, r.notes).entrySet()) {
            File root = new File(v.getKey());
            File[] tries = {new File(new File(root, Environment.DIRECTORY_DOWNLOADS), NAME), new File(root, NAME)};
            for (int i = 0; i < tries.length; i++) {
                File d = tries[i];
                if (!seen.add(d.getAbsolutePath())) continue;
                int s = check(c, d);
                if (i > 0 && s == MISSING) continue;   // <volume>/DOOM Mobile: listed only when it exists
                r.all.add(new Candidate(d, v.getValue(), s));
                if (r.dir == null && s == OK) { r.dir = d; r.how = v.getValue().equals("internal") ? "internal" : "first complete"; }
            }
        }
        if (r.dir != null) last = r;
        return r;
    }

    // The folder of this run: the last complete resolution, else a new one; when nothing is complete, the player's folder or the default
    static File dir(Context c) {
        Result r = last;
        if (r == null || r.dir == null) r = resolve(c);
        if (r.dir != null) return r.dir;
        return r.saved.isEmpty() ? defaultDir() : new File(r.saved);
    }
    static Result last() { return last; }
    static void forget() { last = null; }

    // <game folder>/<sub> when it can be written (all files access), else Download/DOOM Mobile/<sub>, else null (the caller's private fallback)
    static File writable(Context c, String sub) {
        try { if (!SaveTransfer.allFilesAccess(c)) return null; } catch (RuntimeException e) { return null; }
        File[] tries = {new File(dir(c), sub), new File(defaultDir(), sub)};
        for (File d : tries) try { if ((d.isDirectory() || d.mkdirs()) && d.canWrite()) return d; } catch (RuntimeException e) { /* storage not ready */ }
        return null;
    }

    // ---- the system folder picker's tree URI as a path (all files access reads plain paths):
    // externalstorage "primary:<rel>" -> /storage/emulated/0/<rel>, "<uuid>:<rel>" -> the volume's directory (/storage/<uuid>)/<rel>,
    // "home:<rel>" -> Documents/<rel>; downloads "raw:<path>" -> <path>. null = not a place on the storage (cloud, another app).
    static File fromTreeUri(Context c, Uri uri) {
        if (uri == null) return null;
        String auth = uri.getAuthority(), id;
        try { id = DocumentsContract.getTreeDocumentId(uri); } catch (RuntimeException e) { return null; }
        if (id == null) return null;
        if ("com.android.externalstorage.documents".equals(auth)) {
            int colon = id.indexOf(':');
            String vol = colon < 0 ? id : id.substring(0, colon), rel = colon < 0 ? "" : id.substring(colon + 1);
            File root;
            if (vol.equalsIgnoreCase("primary")) root = Environment.getExternalStorageDirectory();
            else if (vol.equalsIgnoreCase("home")) root = new File(Environment.getExternalStorageDirectory(), Environment.DIRECTORY_DOCUMENTS);
            else root = volumeByUuid(c, vol);
            return rel.isEmpty() ? root : new File(root, rel);
        }
        if ("com.android.providers.downloads.documents".equals(auth)) {
            if (id.startsWith("raw:")) return new File(id.substring(4));
            if (id.equals("downloads")) return Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        }
        return null;
    }
    static File volumeByUuid(Context c, String uuid) {
        StorageManager sm = c.getSystemService(StorageManager.class);
        if (sm != null && Build.VERSION.SDK_INT >= 30) try {
            for (StorageVolume v : sm.getStorageVolumes()) if (uuid.equalsIgnoreCase(v.getUuid()) && v.getDirectory() != null) return v.getDirectory();
        } catch (RuntimeException e) { /* below */ }
        for (String root : volumes(c, new ArrayList<>()).keySet()) if (new File(root).getName().equalsIgnoreCase(uuid)) return new File(root);
        return new File("/storage/" + uuid.toUpperCase(Locale.ROOT));
    }
    // A removable volume (SD card / USB drive): slower than the internal storage
    static boolean removable(File dir) {
        String p = dir.getAbsolutePath(), prim = Environment.getExternalStorageDirectory().getAbsolutePath();
        return p.startsWith("/storage/") && !p.startsWith(prim) && !p.startsWith("/storage/emulated/") && !p.startsWith("/storage/self/");
    }
}
