package com.xiaoboluo.d2r;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

// [DOOM] The Vulkan driver manager (BUGS_AND_FIXES 2.39), "Manage drivers…" under the gear dialog's driver list, after Winlator Ludashi's
// driver ("contents") manager:
//   - the installed packages (files/drivers/<name>/: bundled with the APK, downloaded, installed from a file or imported from
//     <game folder>/drivers) with meta.json name / version / description, GPU family, size, origin (<name>/.dm_origin, DmActivity.writeOrigin)
//     and why one cannot run here (DmActivity.driverProblem). All but the bundled ones can be deleted (the next start copies those again);
//   - "Install from file": the system file picker (ACTION_OPEN_DOCUMENT) -> a zip -> DmActivity.unzipDriver -> the same checks; a package
//     that cannot run on this phone (another GPU family, a glibc build, a missing library, meta.json minApi) is removed again with the reason;
//   - "Download": drivers/sources.json of the repository (cached 6 h, the APK carries a copy: assets/driver-sources.json) lists GitHub
//     repositories; for the ones of this phone's GPU family (all with "Show all GPUs") the unauthenticated releases API gives the zips
//     (cached 6 h, a rate limit falls back to the cache); a tap downloads one (progress, cancel), checks the SHA-256 GitHub gives in the
//     asset's "digest" when it gives one, unpacks and checks it like a file install.
// The gear dialog rebuilds its driver list when this closes (the changed callback). Network and unpacking run on background threads,
// the dialogs on the UI thread; offline the installed list and file installs still work. The crash guard (DmActivity.DRIVER_GUARD_STRIKES)
// treats these packages like any other: two launches that die before their first frame switch back to the system driver.
final class DriverManager {
    static final String SOURCES_URL = "https://raw.githubusercontent.com/Desm0nt/doom-port/main/drivers/sources.json";
    static final int REQ_ZIP = 4712;                    // DmActivity.onActivityResult -> picked()
    static final long CACHE_MS = 6L * 3600 * 1000;      // sources.json and the releases lists
    static final int MAX_ZIP = 256 << 20;
    private static WeakReference<DriverManager> waiting = new WeakReference<>(null);   // the manager a file pick returns to

    private final DmActivity a;
    private final Runnable changed;
    private LinearLayout list;
    private boolean dirty;
    private Runnable dlRefresh;   // re-renders the Download dialog (the "installed" marks) while it is open

    DriverManager(DmActivity a, Runnable changed) { this.a = a; this.changed = changed; }
    private File dir() { return new File(a.getFilesDir(), "drivers"); }
    private File cacheDir() { File c = new File(dir(), ".cache"); c.mkdirs(); return c; }
    private String t(String ru, String en) { return Ui.t(a, ru, en); }
    private LinearLayout vbox() { LinearLayout b = new LinearLayout(a); b.setOrientation(LinearLayout.VERTICAL); return b; }
    private TextView text(LinearLayout box, String s) { TextView v = new TextView(a); v.setText(s); box.addView(v); return v; }
    private TextView heading(LinearLayout box, String s) { TextView v = text(box, s); v.setPadding(0, 24, 0, 4); v.setTypeface(null, android.graphics.Typeface.BOLD); return v; }
    private Button button(LinearLayout box, String s) { Button b = new Button(a); b.setText(s); b.setAllCaps(false); box.addView(b); return b; }
    private static String mb(long n) { return String.format(Locale.ROOT, "%.1f MB", n / 1048576.0); }
    private boolean gone() { return a.isFinishing() || a.isDestroyed(); }

    String familyName(String f) {
        switch (f) {
            case "adreno": return "Adreno (Qualcomm)";
            case "mali": return "Mali (Arm)";
            case "xclipse": return "Xclipse (Samsung)";
            case "powervr": return "PowerVR (Imagination)";
            default: return t("любой GPU", "any GPU");
        }
    }
    private Set<String> bundledNames() {
        Set<String> s = new HashSet<>();
        try { String[] n = a.getAssets().list("drivers"); if (n != null) Collections.addAll(s, n); } catch (IOException e) { /* none */ }
        return s;
    }
    // {kind, detail} of a package; before 2.39 there was no origin file: a bundled name, a manifest name or else an import.
    private String[] origin(String name, Set<String> bundled) {
        String[] o = DmActivity.readOrigin(new File(dir(), name)); if (o != null) return o;
        if (bundled.contains(name)) return new String[] {"bundled", ""};
        try {
            JSONArray pk = new JSONObject(DmActivity.readSmall(new File(dir(), "manifest.json"))).getJSONArray("packages");
            for (int i = 0; i < pk.length(); i++) if (name.equals(pk.getJSONObject(i).optString("name"))) return new String[] {"download", "manifest"};
        } catch (Exception e) { /* no manifest */ }
        return new String[] {"import", ""};
    }
    private String originText(String[] o) {
        switch (o[0]) {
            case "bundled": return t("в составе APK", "bundled with the APK");
            case "download": return "manifest".equals(o[1]) ? t("скачан автоматически", "downloaded automatically") : t("скачан", "downloaded") + (o[1].isEmpty() ? "" : ": " + o[1]);
            case "file": return t("из файла", "from a file") + (o[1].isEmpty() ? "" : " " + o[1]);
            default: return t("импорт", "imported") + (o[1].isEmpty() ? "" : t(" из ", " from ") + o[1]);
        }
    }

    // ---- the manager dialog
    void show() {
        LinearLayout box = vbox(); box.setPadding(48, 24, 48, 0);
        text(box, t("Этот телефон: ", "This phone: ") + familyName(DmActivity.phoneFamily()) + ", Android " + android.os.Build.VERSION.RELEASE
            + " (API " + android.os.Build.VERSION.SDK_INT + ")\n" + DmActivity.hardwareSummary() + "\n"
            + t("Выбор драйвера — в списке «Драйвер Vulkan» в настройках, действует со следующего запуска. Драйверы не для Adreno — экспериментальные; если игра с драйвером дважды не дойдёт до первого кадра, включится системный.",
                "Pick the driver in the settings' \"Vulkan driver\" list; it applies on the next launch. Drivers for GPUs other than Adreno are experimental; if the game fails to reach its first frame twice with one, the system driver is used again."));
        Button file = button(box, t("Установить из файла (zip)…", "Install from file (zip)…"));
        Button dl = button(box, t("Скачать…", "Download…"));
        heading(box, t("Установленные пакеты", "Installed packages"));
        list = vbox(); box.addView(list); fill();
        ScrollView sc = new ScrollView(a); sc.addView(box);
        AlertDialog d = new AlertDialog.Builder(a).setTitle(t("Драйверы Vulkan", "Vulkan drivers")).setView(sc).setNegativeButton(t("Закрыть", "Close"), null).create();
        d.setOnDismissListener(x -> { a.immersive(); if (dirty && changed != null) changed.run(); });
        file.setOnClickListener(v -> pickFile());
        dl.setOnClickListener(v -> showDownloads());
        d.show();
    }
    private void fill() {
        if (list == null) return;
        list.removeAllViews();
        Set<String> bundled = bundledNames(); String cur = a.doomPrefs().getString("driver", null);
        String[] names = a.driverPackages();
        if (names.length == 0) text(list, t("Пакетов нет: работает системный драйвер.", "No packages: the system driver is used."));
        for (String n : names) {
            File d = new File(dir(), n); JSONObject m = a.driverMeta(n); String[] org = origin(n, bundled); String why = a.driverProblem(n);
            StringBuilder sb = new StringBuilder(a.driverLabel(n));
            if (n.equals(cur)) sb.append(t(" — выбран", " — selected"));
            ArrayList<String> parts = new ArrayList<>();
            String ver = m != null ? m.optString("driverVersion", "") : "", pv = m != null ? m.optString("packageVersion", "") : "";
            if (!ver.isEmpty()) parts.add(ver);
            if (!pv.isEmpty()) parts.add(t("пакет ", "package ") + pv);
            parts.add(familyName(a.driverFamily(n))); parts.add(mb(DmActivity.treeSize(d))); parts.add(originText(org));
            sb.append('\n').append(android.text.TextUtils.join(" · ", parts));
            String desc = m != null ? m.optString("description", "").trim() : "";
            if (!desc.isEmpty()) sb.append('\n').append(desc.length() > 220 ? desc.substring(0, 217) + "..." : desc);
            if (why != null) sb.append('\n').append(t("Не запустится здесь: ", "Cannot run here: ")).append(a.driverReasonText(why));
            TextView v = text(list, sb.toString()); v.setPadding(0, 16, 0, 0);
            if (!"bundled".equals(org[0])) { Button del = button(list, t("Удалить ", "Delete ") + n); del.setOnClickListener(x -> confirmDelete(n, org)); }
        }
    }
    private void confirmDelete(String n, String[] org) {
        String msg = t("Удалить пакет ", "Delete the package ") + a.driverLabel(n) + "?";
        if ("import".equals(org[0])) msg += "\n\n" + t("Он снова появится при следующем запуске, пока лежит в ", "It comes back at the next start while it is in ")
            + (org[1].isEmpty() ? GameFolder.dir(a).getAbsolutePath() + "/drivers" : org[1]) + ".";
        new AlertDialog.Builder(a).setMessage(msg).setPositiveButton(t("Удалить", "Delete"), (x, w) -> delete(n)).setNegativeButton(t("Отмена", "Cancel"), null).show();
    }
    private void delete(String n) {
        DmActivity.deleteDriverTree(new File(dir(), n));
        SharedPreferences p = a.doomPrefs(); SharedPreferences.Editor e = p.edit();
        if (n.equals(p.getString("driver", null))) e.remove("driver");   // back to this GPU's default
        Set<String> rm = new HashSet<>(p.getStringSet("driver_removed", Collections.emptySet())); rm.add(n);
        e.putStringSet("driver_removed", rm).apply();   // the manifest updater does not fetch it again
        a.crumb("driver manager: " + n + " deleted");
        dirty = true; fill(); if (dlRefresh != null) dlRefresh.run();
    }

    // ---- install from a file
    private void pickFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        waiting = new WeakReference<>(this);
        try { a.startActivityForResult(i, REQ_ZIP); }
        catch (android.content.ActivityNotFoundException e) { a.crumb("driver manager: no file picker"); android.widget.Toast.makeText(a, R.string.folder_no_picker, android.widget.Toast.LENGTH_LONG).show(); }
    }
    // DmActivity.onActivityResult: the zip the player picked (null = cancelled)
    static void picked(DmActivity a, Uri u) {
        DriverManager m = waiting.get(); waiting = new WeakReference<>(null);
        if (u == null) { a.crumb("driver manager: file pick cancelled"); return; }
        if (m == null || m.a != a) m = new DriverManager(a, null);   // the activity was recreated meanwhile: install without the dialogs behind
        m.installUri(u);
    }
    private String displayName(Uri u) {
        try (android.database.Cursor c = a.getContentResolver().query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (RuntimeException e) { /* below */ }
        String s = u.getLastPathSegment(); return s == null ? "driver.zip" : s.substring(s.lastIndexOf('/') + 1);
    }
    private void installUri(Uri u) {
        final String name = displayName(u);
        a.crumb("driver manager: installing " + name + " from " + u);
        final Progress pg = progress(t("Установка ", "Installing ") + name, false);
        new Thread(() -> {
            Result r; File tmp = new File(dir(), ".install.zip");
            try {
                dir().mkdirs();
                try (InputStream in = a.getContentResolver().openInputStream(u); OutputStream os = new FileOutputStream(tmp)) {
                    if (in == null) throw new IOException("cannot open " + u);
                    byte[] buf = new byte[1 << 16]; long n = 0; int k;
                    while ((k = in.read(buf)) > 0) { os.write(buf, 0, k); if ((n += k) > MAX_ZIP) throw new IOException("larger than " + mb(MAX_ZIP)); }
                }
                r = install(tmp, name, "file", name);
            } catch (IOException e) { r = Result.fail(e.getMessage() == null ? e.toString() : e.getMessage()); }
            finally { tmp.delete(); }
            final Result res = r;
            a.runOnUiThread(() -> { pg.dismiss(); done(res, name); });
        }, "dm-driver-install").start();
    }

    // ---- unpack + check (background thread)
    private static final class Result {
        String name, error, sha; boolean verified, cancelled;
        static Result fail(String e) { Result r = new Result(); r.error = e; return r; }
    }
    // A folder name from a zip name: "Turnip-v27.0.0-20261108.zip" -> "Turnip-v27.0.0-20261108" (letters, digits, . _ -; no leading dot).
    static String folderName(String zip) {
        String s = zip.replaceAll("(?i)\\.zip$", "").replaceAll("[^A-Za-z0-9._-]", "_").replaceAll("^[._]+", "");
        if (s.length() > 64) s = s.substring(0, 64);
        return s.isEmpty() ? "driver" : s;
    }
    private Result install(File zip, String zipName, String kind, String detail) {
        String name = folderName(zipName);
        if (bundledNames().contains(name)) name += "-user";   // never over a bundled package (the next start would copy it back)
        File stage = new File(dir(), ".stage-" + name), out = new File(dir(), name);
        try (InputStream in = new FileInputStream(zip)) { DmActivity.unzipDriver(in, stage); }
        catch (IOException e) { DmActivity.deleteDriverTree(stage); return Result.fail(t("это не пакет драйвера adrenotools (", "this is not an adrenotools driver package (") + e.getMessage() + ")"); }
        // checked while staged (".stage-" adds no family word, so the folder name counts as at every start); an installed package of the
        // same name is replaced only by one that can run here
        String why = a.driverProblem(stage.getName());
        if (why != null) { DmActivity.deleteDriverTree(stage); return Result.fail(t("не подходит этому телефону: ", "it cannot run on this phone: ") + a.driverReasonText(why)); }
        DmActivity.writeOrigin(stage, kind, detail);
        DmActivity.deleteDriverTree(out);
        if (!stage.renameTo(out)) { DmActivity.deleteDriverTree(stage); return Result.fail("rename failed: " + out); }
        SharedPreferences p = a.doomPrefs(); Set<String> rm = new HashSet<>(p.getStringSet("driver_removed", Collections.emptySet()));
        if (rm.remove(name)) p.edit().putStringSet("driver_removed", rm).apply();
        Result r = new Result(); r.name = name; return r;
    }
    private void done(Result r, String what) {
        if (r.cancelled) { a.crumb("driver manager: " + what + " cancelled"); return; }
        a.crumb("driver manager: " + what + (r.name != null ? " installed as " + r.name : " not installed: " + r.error) + (r.sha != null ? " sha256 " + r.sha + (r.verified ? " (matches GitHub)" : " (no digest)") : ""));
        if (r.name != null) { dirty = true; fill(); if (dlRefresh != null) dlRefresh.run(); }
        if (gone()) return;
        String msg = r.name != null
            ? t("Установлен: ", "Installed: ") + a.driverLabel(r.name) + "\n\n" + t("Выберите его в списке «Драйвер Vulkan» в настройках (действует со следующего запуска).",
                "Pick it in the settings' \"Vulkan driver\" list (applies on the next launch).")
            : t("Не установлено: ", "Not installed: ") + r.error;
        if (r.sha != null) msg += "\n\nSHA-256 " + r.sha + "\n" + (r.verified ? t("совпадает с GitHub", "matches GitHub") : t("GitHub не дал контрольной суммы для сверки", "GitHub gave no checksum to compare with"));
        new AlertDialog.Builder(a).setTitle(what).setMessage(msg).setPositiveButton(android.R.string.ok, null).show();
    }

    // ---- progress dialog
    private final class Progress {
        AlertDialog d; ProgressBar bar; TextView text; volatile boolean cancelled;
        void dismiss() { try { if (d != null && d.isShowing()) d.dismiss(); } catch (RuntimeException e) { /* the window is gone */ } }
        void set(long done, long total) {
            a.runOnUiThread(() -> {
                if (total > 0) { bar.setIndeterminate(false); bar.setProgress((int) Math.min(1000, done * 1000 / total)); text.setText(mb(done) + " / " + mb(total)); }
                else text.setText(mb(done));
            });
        }
    }
    private Progress progress(String title, boolean cancellable) {
        Progress p = new Progress();
        LinearLayout box = vbox(); box.setPadding(48, 24, 48, 0);
        p.bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal); p.bar.setMax(1000); p.bar.setIndeterminate(true); box.addView(p.bar);
        p.text = text(box, "");
        AlertDialog.Builder b = new AlertDialog.Builder(a).setTitle(title).setView(box).setCancelable(false);
        if (cancellable) b.setNegativeButton(t("Отмена", "Cancel"), (x, w) -> p.cancelled = true);
        p.d = b.show();
        return p;
    }

    // ---- download from the sources' GitHub releases
    private static final class Asset { String name, url, digest; long size; }
    private static final class Release { String tag, date; boolean pre; final List<Asset> assets = new ArrayList<>(); }
    private static final class Src {
        String repo, title, family, note, gpu, err; Pattern inc, exc; JSONArray tags; List<Release> rel;
        String tagsFor(String asset) {
            StringBuilder sb = new StringBuilder();
            if (tags != null) for (int i = 0; i < tags.length(); i++) {
                JSONObject o = tags.optJSONObject(i); if (o == null) continue;
                try { if (Pattern.compile(o.optString("match", "$^")).matcher(asset).find()) sb.append(" · ").append(o.optString("text", "")); } catch (RuntimeException e) { /* a bad regex */ }
            }
            return sb.toString();
        }
    }
    private static final class HttpError extends IOException {
        final int code; final long reset; final boolean limited;
        HttpError(int code, boolean limited, long reset, String url) { super("HTTP " + code + " " + url); this.code = code; this.limited = limited; this.reset = reset; }
    }
    private String describe(Exception e) {
        if (e instanceof HttpError) {
            HttpError h = (HttpError) e;
            if (h.limited) return t("GitHub временно ограничил запросы без входа", "GitHub's limit for anonymous requests is used up")
                + (h.reset > 0 ? t(" (до ", " (until ") + new java.text.SimpleDateFormat("HH:mm", Locale.ROOT).format(new java.util.Date(h.reset * 1000)) + ")" : "");
            if (h.code == 404) return t("не найдено (404)", "not found (404)");
            return "HTTP " + h.code;
        }
        if (e instanceof java.net.UnknownHostException || e instanceof java.net.ConnectException || e instanceof java.net.SocketTimeoutException) return t("нет связи", "no connection");
        return e.getMessage() == null ? e.toString() : e.getMessage();
    }
    // An https connection that answered 200, redirects followed by hand (release assets redirect to another host).
    static java.net.HttpURLConnection connect(String url, boolean api) throws IOException {
        for (int hop = 0; hop < 6; hop++) {
            if (!url.startsWith("https://")) throw new IOException("not https: " + url);
            java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            c.setConnectTimeout(10000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(false);
            c.setRequestProperty("User-Agent", "DOOM-Mobile-driver-manager");
            if (api) { c.setRequestProperty("Accept", "application/vnd.github+json"); c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28"); }
            int code = c.getResponseCode();
            if (code / 100 == 3) { String loc = c.getHeaderField("Location"); c.disconnect(); if (loc == null) break; url = new java.net.URL(new java.net.URL(url), loc).toString(); continue; }
            if (code == 200) return c;
            boolean limited = code == 429 || (code == 403 && "0".equals(c.getHeaderField("X-RateLimit-Remaining")));
            long reset = c.getHeaderFieldLong("X-RateLimit-Reset", 0);
            c.disconnect();
            throw new HttpError(code, limited, reset, url);
        }
        throw new IOException("too many redirects");
    }
    static byte[] get(String url, int max, boolean api) throws IOException {
        java.net.HttpURLConnection c = connect(url, api);
        try (InputStream in = c.getInputStream()) {
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) { b.write(buf, 0, n); if (b.size() > max) throw new IOException("too large: " + url); }
            return b.toByteArray();
        } finally { c.disconnect(); }
    }
    // The text at url, cached as <drivers>/.cache/<file> for CACHE_MS; on a network error (offline, rate limit) the stale cache, with the
    // error in err; null when there is neither.
    private String cached(String url, String file, boolean force, boolean api, StringBuilder err) {
        File c = new File(cacheDir(), file);
        if (!force && c.isFile() && System.currentTimeMillis() - c.lastModified() < CACHE_MS) try { return DmActivity.readSmall(c); } catch (IOException e) { /* fetch again */ }
        try {
            byte[] b = get(url, 8 << 20, api);
            File tmp = new File(c.getPath() + ".part");
            try (OutputStream os = new FileOutputStream(tmp)) { os.write(b); }
            if (!tmp.renameTo(c)) tmp.delete();
            return new String(b, StandardCharsets.UTF_8);
        } catch (IOException e) {
            err.append(describe(e));
            try { return c.isFile() ? DmActivity.readSmall(c) : null; } catch (IOException x) { return null; }
        }
    }
    private static Pattern regex(String s, String def) { try { return Pattern.compile(s == null || s.isEmpty() ? def : s); } catch (RuntimeException e) { return Pattern.compile(def); } }
    // Background: sources.json (network / cache / the APK's copy), then the releases of every source shown (this phone's family, or all).
    private List<Src> load(boolean all, boolean force, StringBuilder note) {
        StringBuilder err = new StringBuilder();
        String js = cached(SOURCES_URL, "sources.json", force, false, err);
        if (js == null) try (InputStream in = a.getAssets().open("driver-sources.json")) { js = DmActivity.readStream(in); } catch (IOException e) { /* none */ }
        if (err.length() > 0) note.append(t("Список источников: ", "Source list: ")).append(err).append(js != null ? t(" (показан сохранённый)", " (the saved one is shown)") : "").append('\n');
        List<Src> out = new ArrayList<>();
        if (js == null) return out;
        String fam = DmActivity.phoneFamily(), gpu = DmActivity.gpuModel();
        try {
            JSONArray sa = new JSONObject(js).getJSONArray("sources");
            for (int i = 0; i < sa.length(); i++) {
                JSONObject o = sa.getJSONObject(i); Src s = new Src();
                s.repo = o.optString("repo", ""); if (!s.repo.matches("[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")) continue;
                s.title = o.optString("title", s.repo); s.family = o.optString("family", "any"); s.note = o.optString("note", ""); s.gpu = o.optString("gpu", "");
                s.inc = regex(o.optString("include", ""), "(?i)\\.zip$"); s.exc = o.optString("exclude", "").isEmpty() ? null : regex(o.optString("exclude"), "$^");
                s.tags = o.optJSONArray("tags");
                boolean fits = s.family.equals("any") || s.family.equals(fam);
                if (fits && !s.gpu.isEmpty()) try { fits = Pattern.compile(s.gpu).matcher(gpu).find(); } catch (RuntimeException e) { /* a bad regex: listed */ }
                if (!fits && !all) continue;
                StringBuilder e = new StringBuilder();
                String rj = cached("https://api.github.com/repos/" + s.repo + "/releases?per_page=10", "gh-" + s.repo.replace('/', '_') + ".json", force, true, e);
                if (e.length() > 0) s.err = e + (rj != null ? t(" (показан сохранённый список)", " (the saved list is shown)") : "");
                if (rj != null) s.rel = releases(s, rj);
                out.add(s);
            }
        } catch (Exception e) { note.append("sources.json: ").append(e.getMessage()).append('\n'); }
        return out;
    }
    private static List<Release> releases(Src s, String json) {
        List<Release> out = new ArrayList<>();
        try {
            JSONArray ra = new JSONArray(json);
            String prefix = "https://github.com/" + s.repo + "/releases/download/";
            for (int i = 0; i < ra.length(); i++) {
                JSONObject o = ra.getJSONObject(i); if (o.optBoolean("draft")) continue;
                Release r = new Release(); r.tag = o.optString("tag_name", "?"); r.pre = o.optBoolean("prerelease");
                String d = o.optString("published_at", ""); r.date = d.length() >= 10 ? d.substring(0, 10) : d;
                JSONArray as = o.optJSONArray("assets");
                if (as != null) for (int j = 0; j < as.length(); j++) {
                    JSONObject x = as.getJSONObject(j); Asset t = new Asset();
                    t.name = x.optString("name", ""); t.url = x.optString("browser_download_url", ""); t.size = x.optLong("size", 0); t.digest = x.optString("digest", "");
                    if (t.name.isEmpty() || !t.url.startsWith(prefix) || !s.inc.matcher(t.name).find() || (s.exc != null && s.exc.matcher(t.name).find())) continue;
                    r.assets.add(t);
                }
                if (!r.assets.isEmpty()) out.add(r);
            }
        } catch (Exception e) { /* not a releases list: nothing */ }
        return out;
    }
    @SuppressWarnings({"unchecked", "rawtypes"})
    private void showDownloads() {
        LinearLayout box = vbox(); box.setPadding(48, 24, 48, 0);
        CheckBox all = new CheckBox(a); all.setText(t("Показать для всех GPU", "Show all GPUs")); box.addView(all);
        TextView status = text(box, t("Загрузка списка…", "Loading the list…"));
        LinearLayout rel = vbox(); box.addView(rel);
        Button refresh = button(box, t("Обновить список", "Refresh the list"));
        ScrollView sc = new ScrollView(a); sc.addView(box);
        AlertDialog d = new AlertDialog.Builder(a).setTitle(t("Скачать драйвер", "Download a driver")).setView(sc).setNegativeButton(t("Закрыть", "Close"), null).create();
        final List<Src>[] shown = new List[] {null};
        final int[] gen = {0};
        final java.util.function.BiConsumer<Boolean, Boolean> loader = (showAll, force) -> {
            final int my = ++gen[0];
            status.setText(t("Загрузка списка…", "Loading the list…")); rel.removeAllViews(); refresh.setEnabled(false);
            new Thread(() -> {
                StringBuilder note = new StringBuilder(); List<Src> srcs = load(showAll, force, note);
                a.runOnUiThread(() -> {
                    if (my != gen[0] || gone()) return;
                    refresh.setEnabled(true); shown[0] = srcs;
                    status.setText(note.toString().trim() + (srcs.isEmpty() ? (note.length() > 0 ? "\n" : "") + t("Для GPU этого телефона источников нет; отметьте «Показать для всех GPU».",
                        "No sources for this phone's GPU; tick \"Show all GPUs\".") : ""));
                    render(rel, srcs);
                });
            }, "dm-driver-list").start();
        };
        all.setOnCheckedChangeListener((b, c) -> loader.accept(c, false));
        refresh.setOnClickListener(v -> loader.accept(all.isChecked(), true));
        dlRefresh = () -> { if (shown[0] != null) render(rel, shown[0]); };
        d.setOnDismissListener(x -> { dlRefresh = null; gen[0]++; a.immersive(); });
        d.show();
        loader.accept(false, false);
    }
    private void render(LinearLayout rel, List<Src> srcs) {
        rel.removeAllViews();
        Set<String> inst = new HashSet<>(); Collections.addAll(inst, a.driverPackages());
        for (Src s : srcs) {
            heading(rel, s.title + " — " + familyName(s.family));
            if (!s.note.isEmpty()) text(rel, s.note);
            if (s.err != null) text(rel, s.err);
            if (s.rel == null) continue;
            if (s.rel.isEmpty()) { text(rel, t("Подходящих zip-пакетов в релизах нет.", "No matching zip packages in the releases.")); continue; }
            for (int i = 0; i < s.rel.size(); i++) {
                Release r = s.rel.get(i);
                LinearLayout as = vbox();
                TextView h = text(rel, ""); h.setPadding(0, 12, 0, 0);
                String head = r.tag + " · " + r.date + (r.pre ? t(" · пре-релиз", " · pre-release") : "") + " (" + r.assets.size() + ")";
                Runnable mark = () -> h.setText((as.getVisibility() == android.view.View.VISIBLE ? "− " : "+ ") + head);
                as.setVisibility(i == 0 ? android.view.View.VISIBLE : android.view.View.GONE); mark.run();
                h.setOnClickListener(v -> { as.setVisibility(as.getVisibility() == android.view.View.VISIBLE ? android.view.View.GONE : android.view.View.VISIBLE); mark.run(); });
                rel.addView(as);
                for (Asset x : r.assets) {
                    boolean have = inst.contains(folderName(x.name));
                    Button b = button(as, x.name + " · " + mb(x.size) + s.tagsFor(x.name) + (have ? t(" · установлен", " · installed") : ""));
                    b.setOnClickListener(v -> confirmDownload(s, r, x, have));
                }
            }
        }
    }
    private void confirmDownload(Src s, Release r, Asset x, boolean have) {
        String msg = s.repo + " " + r.tag + "\n" + x.name + " (" + mb(x.size) + ")\n\n"
            + (x.digest.startsWith("sha256:") ? t("SHA-256 будет сверена с GitHub.", "The SHA-256 is checked against GitHub's.") : t("GitHub не даёт контрольной суммы для этого файла.", "GitHub gives no checksum for this file."))
            + (have ? "\n" + t("Установленный пакет с этим именем будет заменён.", "The installed package of this name is replaced.") : "");
        new AlertDialog.Builder(a).setTitle(t("Скачать?", "Download?")).setMessage(msg)
            .setPositiveButton(t("Скачать", "Download"), (d, w) -> download(s, r, x)).setNegativeButton(t("Отмена", "Cancel"), null).show();
    }
    private void download(Src s, Release r, Asset x) {
        a.crumb("driver manager: downloading " + x.url);
        final Progress pg = progress(x.name, true);
        new Thread(() -> {
            Result res; File tmp = new File(dir(), ".download.zip");
            try {
                dir().mkdirs();
                String sha = fetch(x.url, tmp, x.size, pg);
                if (sha == null) { res = new Result(); res.cancelled = true; }
                else {
                    boolean has = x.digest.startsWith("sha256:");
                    if (has && !x.digest.substring(7).equalsIgnoreCase(sha)) res = Result.fail(t("контрольная сумма не совпала с GitHub", "the checksum does not match GitHub's"));
                    else res = install(tmp, x.name, "download", s.repo + " " + r.tag);
                    res.sha = sha; res.verified = has;
                }
            } catch (IOException e) { res = Result.fail(describe(e)); }
            finally { tmp.delete(); }
            final Result fr = res;
            a.runOnUiThread(() -> { pg.dismiss(); done(fr, x.name); });
        }, "dm-driver-download").start();
    }
    // Streams url to out with its SHA-256 (hex); null when cancelled.
    private String fetch(String url, File out, long size, Progress pg) throws IOException {
        java.security.MessageDigest md;
        try { md = java.security.MessageDigest.getInstance("SHA-256"); } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
        java.net.HttpURLConnection c = connect(url, false);
        long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : size, done = 0, shownAt = 0;
        try (InputStream in = c.getInputStream(); OutputStream os = new FileOutputStream(out)) {
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) {
                if (pg.cancelled) return null;
                os.write(buf, 0, n); md.update(buf, 0, n); done += n;
                if (done > MAX_ZIP) throw new IOException("larger than " + mb(MAX_ZIP));
                long now = android.os.SystemClock.uptimeMillis(); if (now - shownAt > 150) { shownAt = now; pg.set(done, total); }
            }
        } finally { c.disconnect(); }
        if (pg.cancelled) return null;
        StringBuilder sb = new StringBuilder(); for (byte b : md.digest()) sb.append(String.format("%02x", b));
        return sb.toString();
    }
}
