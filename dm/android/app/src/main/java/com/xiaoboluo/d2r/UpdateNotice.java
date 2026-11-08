package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.StyleSpan;
import android.text.style.TypefaceSpan;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONObject;

// [front] Two one-time notices (frontend-common, docs/FRONTEND_UNIFIED.md; DOOM 2016 BUGS_AND_FIXES 2.41). Self-contained: no R, no port class;
// the port's DmActivity gives a Host and calls start() once the game shows its first frames, checkNow() from its gear (⚙) and statusLine()
// for the gear's status text.
//   1. "A new version is out": GitHub's releases/latest of Host.repoSlug() (stable releases only: the API leaves out drafts and pre-releases),
//      once per process start on a background thread, cached CACHE_MS in prefs (upd_cache / upd_cache_at) so a start rarely asks GitHub; its
//      tag is compared numerically per component with the running version ("v0.4.21" vs "0.4.21", "-test1" style suffixes sort before the
//      plain version, "-diag" is ignored). Newer and not announced before (prefs upd_announced = the last tag shown) -> a dialog with the
//      release's changelog: the section of the app language (## English / ## Русский; Russian for ru / uk / be) without the "---" tail and
//      the SHA-256 lines, light markdown rendered (bold, bullets, inline code, links as text); the whole body when it has no sections.
//      Offline, rate limit, any error: silent, one log line "update check: ...". checkNow() (the gear) asks GitHub now and shows the result
//      either way (also a tag announced before, or "you have the latest version").
//   2. "Your game files are older than this app needs": setup on the PC stamps <game folder>/dm/setup-stamp.json ("game_files": N); the APK
//      needs Host.requiredGameFiles(). Older (or no stamp and Host.gameFilesBaseline() older) -> once per (have > need) pair (prefs
//      gf_announced = "have>need"); again only when an APK needs an even newer version. Informational: the game runs either way.
// Showing: never over the pause menu, the layout editor, the system keyboard or another dialog (Host.canShowNotice), at the earliest
// SHOW_AFTER_MS after start(), the game held while a notice is up (Host.holdGame); a notice that finds no moment within SHOW_DEADLINE_MS
// is not marked as shown and comes at the next start. Switches (environment, dm/env): DM_UPDATE_CHECK=0 no start check (1 = on whatever the
// gear says, "always" = no cache and the announced tag ignored), DM_UPDATE_AS=<version> pretend to run that version; DM_GAME_FILES_CHECK=0 /
// "always", DM_GAME_FILES_AS=<n> pretend the game files are version n. The gear's "Check for new versions at start" is prefs upd_check.
final class UpdateNotice {
    interface Host {
        Activity activity();
        String appName();                           // "DOOM Mobile" (the notice text, the User-Agent)
        String repoSlug();                          // "Desm0nt/doom-port": the GitHub repository whose releases are checked
        String currentVersion();                    // the running app version, e.g. BuildConfig.VERSION_NAME "0.4.21"
        int requiredGameFiles();                    // the game-files version this APK needs (0 = no game-files check)
        File gameFilesStamp();                      // <game folder>/dm/setup-stamp.json (null = no game folder known)
        int gameFilesBaseline();                    // the version of game files without a stamp (made by a setup from before the stamp)
        String gameFilesFeatures(int have, int need);   // what versions have+1 .. need bring, one "• ..." line each ("" = none to list)
        SharedPreferences prefs();                  // the front end's prefs ("front"): upd_* / gf_* keys
        String text(String key, Object... args);    // the port's text for a key (DOOM: res/values*/update_notice.xml; GearText ports: GearText.s)
        void crumb(String line);                    // a "[java] ..." line in the run log
        void immersive();                           // system bars hidden again after a dialog
        boolean canShowNotice();                    // resumed, the game runs, no menu / editor / keyboard / dialog is up
        default boolean holdGame(boolean on) { return false; }   // hold the game while a notice shows (true = held, release later)
        default String language() { return Locale.getDefault().getLanguage(); }   // the changelog section: ru / uk / be -> Русский
        default String setupGuideUrl() { return null; }           // the "Instructions" button of the game-files notice (null = none)
        default String userAgent() { return appName().replace(' ', '-') + "-update-check"; }
    }

    static final long CACHE_MS = 6L * 3600 * 1000;          // like the driver manager's GitHub lists
    static final long SHOW_AFTER_MS = 4000, SHOW_DEADLINE_MS = 15L * 60 * 1000, RETRY_MS = 2000;
    static final int MAX_BODY = 1 << 20, MAX_NOTES = 12000;
    static final String P_CHECK = "upd_check", P_ANNOUNCED = "upd_announced", P_CACHE = "upd_cache", P_CACHE_AT = "upd_cache_at", P_GF_ANNOUNCED = "gf_announced";
    private static boolean startedThisProcess;               // "once per start": a recreated activity does not check again

    private final Host h;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ArrayList<Runnable> queue = new ArrayList<>();   // notices waiting for a good moment (UI thread only)
    private AlertDialog showing;
    private boolean retrying;
    private long notBefore, deadline;

    UpdateNotice(Host h) { this.h = h; }

    static String env(String k) { try { String v = android.system.Os.getenv(k); return v == null ? "" : v.trim(); } catch (Throwable t) { return ""; } }
    private String cur() { String as = env("DM_UPDATE_AS"); return as.isEmpty() ? h.currentVersion() : as; }
    private static String vtag(String v) { return v.startsWith("v") || v.startsWith("V") ? v : "v" + v; }
    boolean startCheckOn() { return h.prefs().getBoolean(P_CHECK, true); }
    void setStartCheck(boolean on) { if (on != startCheckOn()) { h.prefs().edit().putBoolean(P_CHECK, on).apply(); h.crumb("update check: at start " + (on ? "on" : "off") + " (gear)"); } }

    // ---- start: both checks in the background, the notices queued (call on the UI thread when the game shows its first frames)
    void start() {
        if (startedThisProcess) return;
        startedThisProcess = true;
        long now = SystemClock.uptimeMillis(); notBefore = now + SHOW_AFTER_MS; deadline = now + SHOW_DEADLINE_MS;
        String gf = env("DM_GAME_FILES_CHECK");
        if ("0".equals(gf)) h.crumb("game files: check off (DM_GAME_FILES_CHECK=0)");
        else { final boolean always = "always".equals(gf);
            new Thread(() -> { Runnable r = gameFilesAtStart(always); if (r != null) ui.post(() -> { queue.add(0, r); pump(); }); }, "dm-gamefiles-check").start(); }
        String up = env("DM_UPDATE_CHECK");
        if ("0".equals(up) || (up.isEmpty() && !startCheckOn())) h.crumb("update check: off (" + ("0".equals(up) ? "DM_UPDATE_CHECK=0" : "gear: not at start") + ")");
        else { final boolean always = "always".equals(up);
            new Thread(() -> { Runnable r = updateAtStart(always); if (r != null) ui.post(() -> { queue.add(r); pump(); }); }, "dm-update-check").start(); }
    }
    void dismiss() { queue.clear(); if (showing != null) showing.dismiss(); }

    // the next queued notice, when the moment is right; else again in RETRY_MS (until SHOW_DEADLINE_MS: then it waits for the next start)
    private void pump() {
        if (showing != null || queue.isEmpty()) return;
        Activity a = h.activity();
        if (a.isFinishing() || a.isDestroyed()) { queue.clear(); return; }
        long now = SystemClock.uptimeMillis();
        if (now > deadline) { h.crumb("notices: no moment to show " + queue.size() + " notice(s) in " + SHOW_DEADLINE_MS / 60000 + " min, they come at the next start"); queue.clear(); return; }
        if (now < notBefore || !h.canShowNotice()) {
            if (!retrying) { retrying = true; ui.postDelayed(() -> { retrying = false; pump(); }, Math.max(RETRY_MS, notBefore - now)); }
            return;
        }
        queue.remove(0).run();
    }
    private void show(AlertDialog.Builder b, boolean hold) {
        final boolean held = hold && h.holdGame(true);
        final AlertDialog d = b.create();
        d.setOnDismissListener(x -> { if (showing == d) showing = null; if (held) h.holdGame(false); h.immersive(); ui.postDelayed(this::pump, 1000); });
        showing = d;
        d.show();
    }

    // ---- 1. the release check
    static final class Release { String tag = "", url = "", body = ""; long at; boolean cached; }
    static final class Fetch { Release r; IOException error; }
    static final class HttpError extends IOException {
        final int code; final boolean limited; final long reset;
        HttpError(int code, boolean limited, long reset) { super("HTTP " + code); this.code = code; this.limited = limited; this.reset = reset; }
    }

    // the latest release: the prefs cache while younger than CACHE_MS (unless force), else GitHub; on an error the stale cache with the error
    private Fetch release(boolean force) {
        Fetch f = new Fetch(); Release c = cached();
        long now = System.currentTimeMillis();
        if (!force && c != null && c.at <= now && now - c.at < CACHE_MS) { f.r = c; return f; }
        try { f.r = fetch(); store(f.r); }
        catch (IOException e) { f.error = e; f.r = c; }
        return f;
    }
    private Release cached() {
        String s = h.prefs().getString(P_CACHE, "");
        if (s.isEmpty()) return null;
        try {
            JSONObject o = new JSONObject(s);
            if (!h.repoSlug().equals(o.optString("repo"))) return null;
            Release r = new Release(); r.tag = o.optString("tag"); r.url = o.optString("url"); r.body = o.optString("body"); r.cached = true;
            r.at = h.prefs().getLong(P_CACHE_AT, 0);
            return r.tag.isEmpty() ? null : r;
        } catch (Exception e) { return null; }
    }
    private void store(Release r) {
        try {
            JSONObject o = new JSONObject().put("repo", h.repoSlug()).put("tag", r.tag).put("url", r.url).put("body", r.body);
            h.prefs().edit().putString(P_CACHE, o.toString()).putLong(P_CACHE_AT, r.at).apply();
        } catch (Exception e) { /* not cached: asks again next start */ }
    }
    private Release fetch() throws IOException {
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL("https://api.github.com/repos/" + h.repoSlug() + "/releases/latest").openConnection();
        try {
            c.setConnectTimeout(10000); c.setReadTimeout(20000);
            c.setRequestProperty("User-Agent", h.userAgent());
            c.setRequestProperty("Accept", "application/vnd.github+json"); c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
            int code = c.getResponseCode();
            if (code != 200) throw new HttpError(code, code == 429 || (code == 403 && "0".equals(c.getHeaderField("X-RateLimit-Remaining"))), c.getHeaderFieldLong("X-RateLimit-Reset", 0));
            byte[] b;
            try (InputStream in = c.getInputStream()) { b = readAll(in, MAX_BODY); }
            JSONObject o = new JSONObject(new String(b, StandardCharsets.UTF_8));
            if (o.optBoolean("draft") || o.optBoolean("prerelease")) throw new IOException("latest is a draft / pre-release");
            Release r = new Release(); r.tag = o.optString("tag_name", ""); r.url = o.optString("html_url", ""); r.body = o.optString("body", "");
            if (r.body.equals("null")) r.body = "";
            r.at = System.currentTimeMillis();
            if (versionParts(r.tag).length == 0) throw new IOException("no version in the tag \"" + r.tag + "\"");
            return r;
        } catch (org.json.JSONException e) { throw new IOException("bad JSON: " + e.getMessage()); }
        finally { c.disconnect(); }
    }
    static byte[] readAll(InputStream in, int max) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] buf = new byte[16384]; int n;
        while ((n = in.read(buf)) > 0) { b.write(buf, 0, n); if (b.size() > max) throw new IOException("larger than " + max + " bytes"); }
        return b.toByteArray();
    }
    // the log's words for a failure (English, with the exception), and the player's
    static String logWhy(IOException e) {
        if (e instanceof HttpError) { HttpError x = (HttpError) e;
            return x.limited ? "GitHub rate limit" + (x.reset > 0 ? ", resets " + new java.text.SimpleDateFormat("HH:mm", Locale.ROOT).format(new java.util.Date(x.reset * 1000)) : "") : e.getMessage(); }
        if (offline(e)) return "no connection (" + e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "") + ")";
        return e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
    }
    private String uiWhy(IOException e) {
        if (e instanceof HttpError && ((HttpError) e).limited) return h.text("upd_err_limit");
        if (offline(e)) return h.text("upd_err_offline");
        return e.getMessage() != null ? e.getMessage() : e.toString();
    }
    private static boolean offline(IOException e) {
        return e instanceof java.net.UnknownHostException || e instanceof java.net.ConnectException || e instanceof java.net.SocketTimeoutException
            || e instanceof java.net.NoRouteToHostException || e instanceof javax.net.ssl.SSLException;
    }
    private String source(Release r) {
        if (!r.cached) return "GitHub";
        long min = Math.max(0, (System.currentTimeMillis() - r.at) / 60000);
        return "the cache (" + (min < 120 ? min + " min" : min / 60 + " h") + " old)";
    }

    // background thread: the start check; the notice to queue, or null
    private Runnable updateAtStart(boolean always) {
        final String cur = cur();
        Fetch f = release(always);
        if (f.r == null) { h.crumb("update check: failed (" + logWhy(f.error) + "), nothing cached"); return null; }
        final Release r = f.r;
        String head = "update check: latest " + r.tag + " from " + source(r) + (f.error != null ? " after a failed fetch (" + logWhy(f.error) + ")" : "") + ", running " + cur;
        if (compareVersions(r.tag, cur) <= 0) { h.crumb(head + ": up to date"); return null; }
        String ann = h.prefs().getString(P_ANNOUNCED, "");
        if (!always && !ann.isEmpty() && compareVersions(r.tag, ann) <= 0) { h.crumb(head + ": newer, already announced (" + ann + ")"); return null; }
        h.crumb(head + ": newer, notice queued");
        return () -> showRelease(r, cur, true, "at start");
    }
    // the gear's "Check for updates": GitHub now, the result shown either way
    void checkNow() {
        final Activity a = h.activity();
        LinearLayout box = new LinearLayout(a); box.setOrientation(LinearLayout.HORIZONTAL); box.setPadding(48, 32, 48, 16);
        box.addView(new ProgressBar(a));
        TextView t = new TextView(a); t.setText(h.text("upd_checking")); t.setPadding(32, 0, 0, 0); t.setGravity(android.view.Gravity.CENTER_VERTICAL);
        box.addView(t, new LinearLayout.LayoutParams(-2, -1));
        final AlertDialog wait = new AlertDialog.Builder(a).setView(box).setNegativeButton(h.text("upd_close"), null).create();
        wait.setOnDismissListener(x -> h.immersive());
        wait.show();
        final String cur = cur();
        new Thread(() -> {
            final Fetch f = release(true);
            ui.post(() -> {
                boolean waited = wait.isShowing(); wait.dismiss();
                if (a.isFinishing() || a.isDestroyed()) return;
                if (f.error != null || f.r == null) {
                    h.crumb("update check: gear: failed (" + logWhy(f.error) + ")");
                    if (waited) message(h.text("upd_heading"), h.text("upd_failed", uiWhy(f.error)));
                    return;
                }
                boolean newer = compareVersions(f.r.tag, cur) > 0;
                h.crumb("update check: gear: latest " + f.r.tag + " from GitHub, running " + cur + (newer ? ": newer" : ": up to date") + (waited ? "" : " (closed while checking: not shown)"));
                if (!waited) return;
                if (newer) showRelease(f.r, cur, false, "from the gear");
                else message(h.text("upd_heading"), h.text("upd_latest", vtag(cur)));
            });
        }, "dm-update-check").start();
    }
    private void message(String title, String text) {
        AlertDialog d = new AlertDialog.Builder(h.activity()).setTitle(title).setMessage(text).setPositiveButton(h.text("upd_close"), null).create();
        d.setOnDismissListener(x -> h.immersive());
        d.show();
    }
    private void showRelease(Release r, String cur, boolean hold, String how) {
        h.prefs().edit().putString(P_ANNOUNCED, r.tag).apply();
        h.crumb("update check: notice for " + r.tag + " shown " + how);
        String notes = changelog(r.body, h.language());
        if (notes.length() > MAX_NOTES) notes = notes.substring(0, MAX_NOTES) + "\n…";
        SpannableStringBuilder msg = new SpannableStringBuilder(h.text("upd_message", h.appName(), r.tag, vtag(cur)));
        if (!notes.trim().isEmpty()) {
            msg.append("\n\n"); int st = msg.length(); msg.append(h.text("upd_whats_new"));
            msg.setSpan(new StyleSpan(Typeface.BOLD), st, msg.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            msg.append("\n").append(spanned(render(notes)));
        }
        final String page = releasePage(r.url);
        show(new AlertDialog.Builder(h.activity()).setTitle(h.text("upd_title", r.tag)).setMessage(msg)
            .setPositiveButton(h.text("upd_open"), (d, w) -> open(page)).setNegativeButton(h.text("upd_close"), null), hold);
    }
    // the release's page: GitHub's html_url when it is a page of this repository, else its latest release page
    private String releasePage(String url) {
        String base = "https://github.com/" + h.repoSlug() + "/";
        return url != null && url.startsWith(base) && !url.contains("..") ? url : base + "releases/latest";
    }
    private void open(String url) {
        h.crumb("update check: opening " + url);
        try { h.activity().startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); }
        catch (RuntimeException e) { h.crumb("update check: no app opens " + url + " (" + e + ")"); }
    }

    // ---- 2. the game files
    static final class Files { int have = -1; boolean stamped; String log = ""; }
    Files gameFiles() {
        Files g = new Files();
        File f = h.gameFilesStamp();
        if (f == null) g.log = "no game folder";
        else if (f.isFile()) {
            try (InputStream in = new FileInputStream(f)) {
                JSONObject o = new JSONObject(new String(readAll(in, 65536), StandardCharsets.UTF_8));
                int v = o.optInt("game_files", -1);
                if (v < 0) throw new IOException("no \"game_files\"");
                g.have = v; g.stamped = true;
                JSONObject cm = o.optJSONObject("components");
                g.log = "stamp " + f + ": version " + v + ", setup " + o.optString("setup", "?") + (cm != null ? ", components " + cm : "");
            } catch (Exception e) { g.log = "stamp " + f + " unreadable (" + e + ")"; }
        } else g.log = "no stamp at " + f + " (an older setup)";
        if (f != null && !g.stamped) { g.have = h.gameFilesBaseline(); g.log += ": baseline version " + g.have; }
        String as = env("DM_GAME_FILES_AS");
        if (!as.isEmpty()) try { g.have = Integer.parseInt(as); g.log += "; DM_GAME_FILES_AS=" + as; } catch (NumberFormatException e) { /* ignored */ }
        return g;
    }
    // background thread: the start check; the notice to queue, or null
    private Runnable gameFilesAtStart(boolean always) {
        final int need = h.requiredGameFiles();
        if (need <= 0) return null;
        final Files g = gameFiles();
        String head = "game files: " + g.log + "; this APK needs " + need;
        if (g.have < 0) { h.crumb(head + ": not checked"); return null; }
        if (g.have >= need) { h.crumb(head + ": ok"); return null; }
        final String pair = g.have + ">" + need;
        String ann = h.prefs().getString(P_GF_ANNOUNCED, "");
        int annNeed = -1; try { annNeed = Integer.parseInt(ann.substring(ann.indexOf('>') + 1)); } catch (RuntimeException e) { /* none yet */ }
        if (!always && annNeed >= need) { h.crumb(head + ": older, already announced (" + ann + ")"); return null; }
        final String feats = h.gameFilesFeatures(g.have, need);
        h.crumb(head + ": older, notice queued (" + pair + ")");
        return () -> showGameFiles(pair, feats);
    }
    private void showGameFiles(String pair, String feats) {
        h.prefs().edit().putString(P_GF_ANNOUNCED, pair).apply();
        h.crumb("game files: notice shown (" + pair + ")");
        String msg = h.text("gf_message") + (feats.isEmpty() ? "" : "\n" + feats) + "\n\n" + h.text("gf_howto") + "\n\n" + h.text("gf_note");
        AlertDialog.Builder b = new AlertDialog.Builder(h.activity()).setTitle(h.text("gf_title")).setMessage(msg).setPositiveButton(h.text("upd_close"), null);
        final String guide = h.setupGuideUrl();
        if (guide != null) b.setNeutralButton(h.text("gf_guide"), (d, w) -> open(guide));
        show(b, true);
    }
    // the gear's status: the app version, then the game files against what this APK needs
    String statusLine() {
        StringBuilder sb = new StringBuilder(h.text("upd_version_line", vtag(cur())));
        int need = h.requiredGameFiles();
        if (need > 0) {
            Files g = gameFiles();
            sb.append('\n').append(g.have < 0 ? h.text("gf_line_none") : g.have < need ? h.text("gf_line_old", g.have, need)
                : g.stamped ? h.text("gf_line_ok", g.have, need) : h.text("gf_line_nostamp", g.have, need));
        }
        return sb.toString();
    }

    // ---- pure helpers (no Android: tools/test_update_notice.py runs them on the host)
    // "v0.4.21" -> {0, 4, 21}; the numbers before the first '-' / '+' / space; {} = no version
    static int[] versionParts(String v) {
        if (v == null) return new int[0];
        String s = v.trim(); if (s.startsWith("v") || s.startsWith("V")) s = s.substring(1);
        int cut = s.length(); for (int i = 0; i < s.length(); i++) { char c = s.charAt(i); if (c == '-' || c == '+' || c == ' ') { cut = i; break; } }
        String[] p = s.substring(0, cut).split("\\.");
        ArrayList<Integer> out = new ArrayList<>();
        for (String x : p) { if (x.isEmpty() || !x.chars().allMatch(Character::isDigit) || x.length() > 9) break; out.add(Integer.parseInt(x)); }
        int[] r = new int[out.size()]; for (int i = 0; i < r.length; i++) r[i] = out.get(i); return r;
    }
    // a pre-release suffix ("-test1", "-rc2", "-beta"...): sorts before the plain version; other suffixes ("-diag", "+build") do not count
    static boolean preRelease(String v) {
        if (v == null) return false;
        int d = v.indexOf('-'); if (d < 0) return false;
        return Pattern.compile("^(test|rc|beta|alpha|pre|dev|preview)", Pattern.CASE_INSENSITIVE).matcher(v.substring(d + 1)).find();
    }
    // < 0: a older than b, 0: the same release, > 0: a newer (numeric per component, missing components are 0)
    static int compareVersions(String a, String b) {
        int[] x = versionParts(a), y = versionParts(b);
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? x[i] : 0, q = i < y.length ? y[i] : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        boolean pa = preRelease(a), pb = preRelease(b);
        return pa == pb ? 0 : pa ? -1 : 1;
    }

    private static final Pattern HEAD = Pattern.compile("^#{1,6}\\s*(.*?)\\s*#*\\s*$");
    private static final Pattern BULLET = Pattern.compile("^[-*+]\\s+(.*)$");
    private static final Pattern NOTE_MARK = Pattern.compile("^\\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\\]$", Pattern.CASE_INSENSITIVE);
    private static final Pattern SHA_LINE = Pattern.compile("^(SHA-?256|SHA256SUMS?)\\b.*", Pattern.CASE_INSENSITIVE);
    static boolean russianUi(String lang) { String l = lang == null ? "" : lang.toLowerCase(Locale.ROOT); return l.startsWith("ru") || l.startsWith("uk") || l.startsWith("be"); }
    // 1 = English, 2 = Russian, 0 = another heading
    private static int languageOf(String heading) {
        String s = heading.replace("*", "").trim().toLowerCase(Locale.ROOT);
        if (s.equals("english") || s.equals("en") || s.startsWith("english ")) return 1;
        if (s.equals("русский") || s.equals("russian") || s.equals("ru") || s.startsWith("русский ") || s.startsWith("на русском")) return 2;
        return 0;
    }
    // The release body's text for this app language: the text before the first language heading (a bilingual notice) plus the "## English"
    // or "## Русский" section (the other one when this one is missing), without the "---" tail and SHA-256 lines; no language headings =
    // the whole body without those. Trimmed; "" when nothing is left.
    static String changelog(String body, String lang) {
        if (body == null) return "";
        String[] lines = body.replace("\r\n", "\n").replace('\r', '\n').split("\n", -1);
        List<String> pre = new ArrayList<>(), en = new ArrayList<>(), ru = new ArrayList<>();
        int where = 0;   // 0 before any language heading, 1 English, 2 Russian, -1 after a "---"
        boolean sections = false;
        for (String l : lines) {
            String t = l.trim();
            if (t.equals("---") || t.equals("***") || t.equals("___")) { where = -1; continue; }
            Matcher m = HEAD.matcher(t);
            if (m.matches() && t.startsWith("#")) { int k = languageOf(m.group(1)); if (k != 0) { where = k; sections = true; continue; } }
            if (SHA_LINE.matcher(t).matches()) continue;
            (where == 0 ? pre : where == 1 ? en : where == 2 ? ru : new ArrayList<String>()).add(l);
        }
        List<String> out = new ArrayList<>();
        if (!sections) out.addAll(pre);
        else {
            List<String> pick = russianUi(lang) ? (blank(ru) ? en : ru) : (blank(en) ? ru : en);
            if (!blank(pre)) { out.addAll(pre); out.add(""); }
            out.addAll(pick);
        }
        StringBuilder sb = new StringBuilder();
        for (String l : out) sb.append(rstrip(l)).append('\n');
        return sb.toString().replaceAll("^\\s*\\n", "").replaceAll("\\s+$", "");
    }
    private static boolean blank(List<String> l) { for (String s : l) if (!s.trim().isEmpty()) return false; return true; }
    private static String rstrip(String s) { int e = s.length(); while (e > 0 && Character.isWhitespace(s.charAt(e - 1))) e--; return s.substring(0, e); }

    // Light markdown -> plain text + spans: headings and **bold** bold, `code` monospace, "- " / "* " bullets "• " (nested "◦ "), "> " quotes
    // unquoted (GitHub's [!NOTE] markers dropped), [text](url) -> text, backslash escapes; runs of empty lines -> one.
    static final int BOLD = 1, CODE = 2;
    static final class Rich { final StringBuilder text = new StringBuilder(); final ArrayList<int[]> spans = new ArrayList<>(); }
    static Rich render(String md) {
        Rich r = new Rich();
        boolean blank = true;
        for (String raw : (md == null ? "" : md).split("\n", -1)) {
            String l = rstrip(raw);
            int lead = 0; while (lead < l.length() && (l.charAt(lead) == ' ' || l.charAt(lead) == '\t')) lead++;
            String t = l.trim();
            while (t.startsWith(">")) t = t.substring(1).trim();
            if (NOTE_MARK.matcher(t).matches()) continue;
            if (t.isEmpty()) { if (!blank) { r.text.append('\n'); blank = true; } continue; }
            boolean head = false;
            Matcher hm = HEAD.matcher(t);
            if (t.startsWith("#") && hm.matches()) { t = hm.group(1); head = true; }
            String prefix = "";
            Matcher bm = BULLET.matcher(t);
            if (!head && bm.matches()) { int level = Math.min(lead / 2, 3); StringBuilder p = new StringBuilder(); for (int i = 0; i < level; i++) p.append("    "); prefix = p + (level == 0 ? "• " : "◦ "); t = bm.group(1); }
            r.text.append(prefix);
            int st = r.text.length();
            inline(r, t);
            if (head) r.spans.add(new int[] {st, r.text.length(), BOLD});
            r.text.append('\n');
            blank = false;
        }
        while (r.text.length() > 0 && r.text.charAt(r.text.length() - 1) == '\n') r.text.setLength(r.text.length() - 1);
        for (int[] s : r.spans) { s[0] = Math.min(s[0], r.text.length()); s[1] = Math.min(s[1], r.text.length()); }
        return r;
    }
    private static void inline(Rich r, String s) {
        int bold = -1;
        for (int i = 0; i < s.length(); ) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length() && "\\`*_[]()#+-.!>|~".indexOf(s.charAt(i + 1)) >= 0) { r.text.append(s.charAt(i + 1)); i += 2; continue; }
            if (c == '*' && s.startsWith("**", i)) {
                if (bold >= 0) { r.spans.add(new int[] {bold, r.text.length(), BOLD}); bold = -1; i += 2; continue; }
                if (s.indexOf("**", i + 2) > i + 2) { bold = r.text.length(); i += 2; continue; }
            }
            if (c == '`') { int e = s.indexOf('`', i + 1); if (e > i + 1) { int st = r.text.length(); r.text.append(s, i + 1, e); r.spans.add(new int[] {st, r.text.length(), CODE}); i = e + 1; continue; } }
            if (c == '[' || (c == '!' && i + 1 < s.length() && s.charAt(i + 1) == '[')) {
                int o = c == '!' ? i + 1 : i, mid = s.indexOf("](", o), end = mid > o ? s.indexOf(')', mid + 2) : -1;
                if (mid > o && end > mid) { inline(r, s.substring(o + 1, mid)); i = end + 1; continue; }
            }
            r.text.append(c); i++;
        }
        if (bold >= 0) r.spans.add(new int[] {bold, r.text.length(), BOLD});
    }
    static CharSequence spanned(Rich r) {
        SpannableStringBuilder b = new SpannableStringBuilder(r.text);
        for (int[] s : r.spans) {
            if (s[1] <= s[0] || s[1] > b.length()) continue;
            b.setSpan(s[2] == CODE ? new TypefaceSpan("monospace") : new StyleSpan(Typeface.BOLD), s[0], s[1], Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        return b;
    }
}
