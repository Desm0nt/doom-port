package com.xiaoboluo.d2r;

import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;
import android.view.View;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

// [front] The Vulkan driver side of the gear, shared by every port (frontend-common; moved 1:1 from DOOM 2016's DmActivity, its
// BUGS_AND_FIXES 2.16 / 2.22 / 2.25 / 2.39). The native half is vk_driver.c (frontend-common/native/vk_driver.c: DM_VK_DRIVER, DM_DRIVERS_DIR,
// DM_NATIVE_LIB_DIR, the status string), loading adrenotools packages through libadrenotools. This class:
//   - at the start (prepare + choose, from GearPages.driver's env, before the library loads): the APK's bundled packages
//     (assets/drivers/<name>/, tools/drivers/fetch_drivers.py) copied to files/drivers when they fit this phone, the player's packages from
//     GearHost.driverImportDir() (folders or zips), the repository's drivers/manifest.json in the background, then the choice: the saved
//     pref "driver" (or the GPU's default, GearHost.defaultDriver), a hardware check (another GPU family, glibc build, missing library,
//     meta.json minApi -> the system driver, the choice reset, the player told once), and the crash guard;
//   - the crash guard: before a non-system driver is loaded its name goes to files/driver-trying ("<name>\n<failed launches>"); the file is
//     removed once the game has presented a frame (GearHost.frameStats) or the native side reports it fell back to the system driver
//     (watch). A file left over means the last launch died before its first frame with that driver: after GUARD_STRIKES such launches in a
//     row the system driver is chosen (saved, so the player sees it and can pick the package again) and a message says why. Two strikes,
//     not one: a launch killed by hand (or by a debug script) before its first frame is not a crash;
//   - the package helpers the gear's driver list and DriverManager use (labels, families, problems, the reason texts).
// Prefs (GearHost.gamePrefs, "doom_graphics"): "driver" ("" = system, else a folder under files/drivers), "driver_removed" (names the
// player deleted in the manager: the manifest updater does not fetch them again).
final class VkDrivers {
    static final String MANIFEST_URL = "https://raw.githubusercontent.com/Desm0nt/doom-port/main/drivers/manifest.json";
    static final String SOURCES_URL = "https://raw.githubusercontent.com/Desm0nt/doom-port/main/drivers/sources.json";
    static final String DEFAULT_DRIVER = "turnip27", DRIVER_A710 = "turnip27-a710", DRIVER_A8XX = "turnip27-a8xx";
    static final int GUARD_STRIKES = 2;
    static final String ORIGIN = ".dm_origin";

    final GearHost host;
    private String notice;   // shown once the game view is up (started)
    private boolean guardDone, statusDone, firstFrameDone;
    private long startedAt;
    private Runnable onFirstFrames;   // gameStarted's callback, run once right after the "first frames presented" line (DOOM: UpdateNotice.start, 2.41)

    VkDrivers(GearHost h) { host = h; }
    private android.app.Activity a() { return host.activity(); }
    private String t(String ru, String en) { return Ui.t(a(), ru, en); }
    File dir() { return new File(a().getFilesDir(), "drivers"); }
    private File guardFile() { return new File(a().getFilesDir(), "driver-trying"); }
    private SharedPreferences prefs() { return host.gamePrefs(); }

    // ---- at the start, before the library loads
    // The environment vk_driver.c reads and the packages in files/drivers. The hooks adrenotools loads live in nativeLibraryDir.
    void prepare() throws ErrnoException {
        Os.setenv("DM_NATIVE_LIB_DIR", a().getApplicationInfo().nativeLibraryDir, true);
        File drivers = dir();
        Os.setenv("DM_DRIVERS_DIR", drivers.getAbsolutePath(), true);
        extractBundled(drivers);                        // the APK's packages (Turnip default + experimental ones, assets/drivers/<name>/)
        File imp = host.driverImportDir(); if (imp != null) importDrivers(imp, drivers);   // <data folder>/drivers (DOOM: GameFolder, issue #11)
        if (host.driverManifestUrl() != null) updateInBackground(drivers);   // other packages from the repository's drivers/manifest.json when online
    }
    // The driver this launch uses (DM_VK_DRIVER set / unset); "" = the system driver.
    String choose() throws ErrnoException {
        SharedPreferences p = prefs();
        // [DOOM] diagnostics APK with -PdmDiagDriver=<pkg>: always that driver, the guard file dropped so its strikes cannot switch it back
        String forced = host.forcedDriver();
        String drv = forced != null ? forced : p.getString("driver", defaultDriver());
        if (forced != null) guardFile().delete();
        // hardware check at every start (DOOM 2.22, 2.25): Turnip needs a Qualcomm Adreno 6xx+ (KGSL), PanVK a Mali (/dev/mali0), RADV-Xclipse a
        // Samsung Xclipse, and a Linux glibc build (the "-EMULATOR" zips) runs nowhere in an Android app (problem). Such a saved choice (settings
        // restored from another phone, or picked before this check existed) gets the system driver; the choice is reset so the dialog and the
        // next starts agree, and the player is told once why. Runs before the crash guard, so a reset choice never counts as a strike.
        boolean turnipOk = turnipPossible();
        String why = drv.isEmpty() ? null : problem(drv);
        if (why != null) {
            host.crumb("driver: saved choice " + drv + " cannot run here (" + why + "), this phone has " + hardwareSummary() + ": the system driver is used and the choice reset");
            p.edit().putString("driver", "").apply();
            notice = t("Драйвер Vulkan " + label(drv) + " не подходит: " + reasonText(why) + ". Используется системный драйвер.",
                "Vulkan driver " + label(drv) + " cannot run here: " + reasonText(why) + ". The system driver is used.");
            drv = "";
        }
        host.crumb("driver: " + (drv.isEmpty() ? "system" : drv) + " (saved " + (p.contains("driver") ? "\"" + p.getString("driver", "") + "\"" : "none, default " + defaultDriver())
            + "), turnip possible " + turnipOk + ", " + hardwareSummary());
        File guard = guardFile(); String left = readText(guard);
        int strikes = 0;
        if (left != null) {
            String[] g = left.split("\n"); String name = g[0].trim();
            try { strikes = g.length > 1 ? Integer.parseInt(g[1].trim()) : 1; } catch (NumberFormatException e) { strikes = 1; }
            if (!name.equals(drv)) strikes = 0;   // about another driver: the player changed it since
            android.util.Log.w("dm", "driver guard: last launch with " + name + " ended before its first frame (" + strikes + " in a row)");
            if (strikes >= GUARD_STRIKES) {
                p.edit().putString("driver", "").apply(); drv = ""; strikes = 0;
                notice = GearText.s(a(), "toast_driver_crashed", label(name));
            }
            guard.delete();
        }
        if (!drv.isEmpty()) {
            try (OutputStream os = new FileOutputStream(guard)) { os.write((drv + "\n" + (strikes + 1) + "\n").getBytes(StandardCharsets.UTF_8)); }
            catch (IOException e) { android.util.Log.e("dm", "driver guard: " + e); }
        }
        if (!drv.isEmpty()) Os.setenv("DM_VK_DRIVER", drv, true); else Os.unsetenv("DM_VK_DRIVER");
        return drv;
    }
    String defaultDriver() { return host.defaultDriver(this); }

    // ---- after the game started (the port calls this once nativeStart succeeded): the notice from choose(), then the watch; firstFrames (null =
    // none) runs once when the game has presented its first frames (DOOM 2016 starts its update / game-files checks there: BUGS_AND_FIXES 2.41)
    void gameStarted(View v) { gameStarted(v, null); }
    void gameStarted(View v, Runnable firstFrames) {
        if (notice != null) { Toast.makeText(a(), notice, Toast.LENGTH_LONG).show(); notice = null; }
        onFirstFrames = firstFrames;
        startedAt = android.os.SystemClock.uptimeMillis();
        if (v != null) v.postDelayed(() -> watch(v), 1000);
    }
    // Removes the crash guard once a frame was presented, and tells the player once when the chosen driver did not start and the native side
    // fell back to the system driver (vk_driver.c status). Polls every second until both are settled.
    private final float[] stats0 = new float[4];
    private void watch(View v) {
        if (!host.started() || a().isDestroyed()) return;
        String[] st = status();
        if (!statusDone && st != null) {
            statusDone = true;
            host.crumb("Vulkan driver status: requested \"" + st[0] + "\" active " + st[1] + " reason " + st[2] + (st.length > 3 ? " " + st[3] : ""));
            if (!st[0].isEmpty() && "system".equals(st[1])) {
                android.util.Log.w("dm", "driver " + st[0] + " did not start (" + st[2] + "): system driver");
                Toast.makeText(a(), GearText.s(a(), "toast_driver_fallback", label(st[0]), reasonText(st[2])), Toast.LENGTH_LONG).show();
                guardFile().delete(); guardDone = true;   // not a crash: the native side already chose the system driver
            }
        }
        if (!guardDone || !firstFrameDone) {   // the frame stats are read once per tick, only while they are needed (as DOOM's driverWatch did)
            float[] fs = host.frameStats(); boolean none = fs == null; if (none) { fs = stats0; fs[0] = 0; }
            if (fs[0] > 0 && !guardDone) { guardFile().delete(); guardDone = true; android.util.Log.i("dm", "driver guard: first frames presented, guard cleared"); }
            if (fs[0] > 0 && !firstFrameDone) { firstFrameDone = true;
                host.crumb(String.format(Locale.ROOT, "first frames presented %.1f s after the game started (%.0f fps, frame %.1f ms)", (android.os.SystemClock.uptimeMillis() - startedAt) / 1000.0, fs[0], fs[1]));
                firstFrames(); }
            // [front] a port without frame stats (GearHost.frameStats null: no nativeFrameStats) cannot see its first frame: the guard is cleared
            // once the game has run 20 s, else every launch would count as a strike; without a native status there is nothing more to wait for
            if (none && android.os.SystemClock.uptimeMillis() - startedAt > 20_000) {
                if (!guardDone) { guardFile().delete(); guardDone = true; android.util.Log.i("dm", "driver guard: no frame stats in this port, cleared after 20 s"); }
                if (!firstFrameDone) { firstFrameDone = true; firstFrames(); } if (st == null) statusDone = true;
            }
        }
        if (!(guardDone && statusDone && firstFrameDone)) v.postDelayed(() -> watch(v), 1000);
    }
    // "<requested>\t<active>\t<reason>[\t<details>]" from vk_driver.c, or null before the driver is loaded. details (2.25):
    // "verified=0|1 gpu=... driver=... id=..." (what the GPU reports for the custom driver).
    private void firstFrames() { Runnable r = onFirstFrames; onFirstFrames = null; if (r != null) r.run(); }
    String[] status() {
        if (!host.libraryLoaded()) return null;
        String s; try { s = host.driverStatus(); } catch (Throwable t) { return null; }
        String[] f = s == null ? new String[0] : s.split("\t", -1);
        return f.length >= 3 ? f : null;
    }

    // ---- the default per GPU (docs/research/non_adreno_drivers.md 4.1 of DOOM 2016); the player's choice (pref "driver") always wins.
    // This is the hook for vendor rules: a package name under files/drivers/ or "" = the system driver.
    //   Adreno 6xx / 7xx            -> turnip27 (bundled)
    //   Adreno 710 / 720 / 722      -> turnip27-a710 when present (bundled since 2.39, or downloaded), else system
    //   Adreno 8xx                  -> turnip27-a8xx when present (bundled since 2.39, or downloaded), else system
    //   Adreno 5xx and older        -> system (Turnip does not support a5xx)
    //   Mali / PowerVR / Xclipse / Maleoon (no kgsl gpu_model) -> system; their optional packages (PanVK, RADV-Xclipse) stay selectable
    // A manifest entry with an "auto" regex that matches this Adreno (drivers/manifest.json) still comes first for Adreno >= 600.
    // When gpu_model is unreadable on a Qualcomm SoC, turnip27 is tried (vk_driver.c falls back to the system driver without a GPU).
    String gpuDefault() {
        if (!turnipPossible()) return "";   // 2.25: non-Adreno: the bundled Turnip is never the default
        File dir = dir();
        int m = adrenoModel(gpuModel());
        if (m == 0) return qualcommSoc() ? pkgIfPresent(dir, DEFAULT_DRIVER) : "";
        if (m < 600) return "";
        String auto = autoDriverForGpu(dir); if (auto != null) return auto;
        if (m == 710 || m == 720 || m == 722) return pkgIfPresent(dir, DRIVER_A710);
        if (m >= 800 && m < 900) return pkgIfPresent(dir, DRIVER_A8XX);
        return pkgIfPresent(dir, DEFAULT_DRIVER);
    }
    private static String pkgIfPresent(File dir, String name) { return hasDriverLib(new File(dir, name)) ? name : ""; }
    // A downloaded package whose manifest entry has an "auto" regex matching this GPU (cached as <dir>/manifest.json); null when none applies.
    private static String autoDriverForGpu(File dir) {
        String gpu = gpuModel(); if (gpu.isEmpty()) return null;
        try {
            org.json.JSONArray pk = new org.json.JSONObject(readSmall(new File(dir, "manifest.json"))).getJSONArray("packages");
            for (int i = 0; i < pk.length(); i++) {
                org.json.JSONObject o = pk.getJSONObject(i); String re = o.optString("auto", ""), name = o.getString("name");
                if (!re.isEmpty() && java.util.regex.Pattern.compile(re).matcher(gpu).find() && hasDriverLib(new File(dir, name))) return name;
            }
        } catch (Exception e) { /* no manifest yet */ }
        return null;
    }

    // ---- this phone's GPU
    // "Adreno750v2" -> 750; 0 = no readable model; -1 = a model that is not an Adreno.
    static int adrenoModel(String gpu) {
        if (gpu == null || gpu.isEmpty()) return 0;
        String g = gpu.toLowerCase(Locale.ROOT); int at = g.indexOf("adreno"); if (at < 0) return -1;
        int i = at + 6, v = 0, n = 0;
        while (i < g.length() && !Character.isDigit(g.charAt(i)) && n == 0 && i < at + 12) i++;
        while (i < g.length() && Character.isDigit(g.charAt(i)) && n < 4) { v = v * 10 + (g.charAt(i++) - '0'); n++; }
        return n > 0 ? v : -1;
    }
    // Can Turnip (Mesa freedreno, Adreno only) work on this phone at all? It drives the GPU through Qualcomm's kernel driver KGSL, so the device
    // node (or its sysfs class) must exist, and the GPU must be an Adreno 6xx or newer (a readable gpu_model), or, when the model is unreadable,
    // the SoC must be a Qualcomm one. vk_driver.c repeats the KGSL check.
    static boolean kgsl() { return new File("/dev/kgsl-3d0").exists() || new File("/sys/class/kgsl/kgsl-3d0").exists(); }
    static boolean mali() { return new File("/dev/mali0").exists(); }
    static boolean powervr() { return new File("/dev/pvrsrvkm").exists(); }
    static boolean turnipPossible() {
        if (!kgsl()) return false;
        int m = adrenoModel(gpuModel());
        return m == 0 ? qualcommSoc() : m >= 600;
    }
    static boolean qualcommSoc() {
        String s = ((android.os.Build.VERSION.SDK_INT >= 31 ? android.os.Build.SOC_MANUFACTURER + " " : "") + android.os.Build.HARDWARE).toLowerCase(Locale.ROOT);
        return s.contains("qti") || s.contains("qualcomm") || s.contains("qcom");
    }
    // The GPU model as the kernel reports it, e.g. "Adreno750v2" (empty when unreadable or not an Adreno).
    static String gpuModel() {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/sys/class/kgsl/kgsl-3d0/gpu_model"))) {
            String l = r.readLine(); return l == null ? "" : l.trim();
        } catch (IOException e) { return ""; }
    }
    // "gpu_model=Adreno750v2 kgsl=true mali=false SoC=QTI SM8650" (logs, crash reports, the driver notice)
    static String hardwareSummary() {
        String g = gpuModel();
        return "gpu_model=" + (g.isEmpty() ? "-" : g) + " kgsl=" + kgsl() + " mali=" + mali() + " SoC="
            + (android.os.Build.VERSION.SDK_INT >= 31 ? android.os.Build.SOC_MANUFACTURER + " " + android.os.Build.SOC_MODEL : android.os.Build.HARDWARE);
    }
    // This phone's family for the driver lists (the manifest, the driver manager): any KGSL GPU = adreno, then mali / powervr; none of these
    // nodes = xclipse (Samsung Xclipse, or a GPU nobody ships packages for)
    static String phoneFamily() { return kgsl() ? "adreno" : mali() ? "mali" : powervr() ? "powervr" : "xclipse"; }
    // API level -> the Android version players know (34 -> "14")
    static String androidName(int api) { return api >= 33 ? String.valueOf(api - 20) : api == 32 ? "12L" : api >= 28 ? String.valueOf(api - 19) : "API " + api; }

    // ---- packages
    // (2.25) The GPU family a driver package is for: "mali" (PanVK / panfrost), "xclipse" (RADV), "powervr", "adreno" (Turnip / freedreno) or ""
    // (unknown: offered everywhere; vk_driver.c verifies what really runs). The folder name and meta.json "name" decide first, description /
    // libraryName only when they say nothing (the GameHub PanVK zip ships libvulkan_freedreno.so and says so in its description).
    // vk_driver.c family_of uses the same words.
    static String familyWord(String s) {
        s = s.toLowerCase(Locale.ROOT);
        if (s.contains("panvk") || s.contains("panfrost") || s.contains("mali")) return "mali";
        if (s.contains("radv") || s.contains("radeon") || s.contains("xclipse")) return "xclipse";
        if (s.contains("powervr") || s.contains("imagination") || s.contains("mesa-pvr")) return "powervr";
        if (s.contains("turnip") || s.contains("freedreno") || s.contains("adreno")) return "adreno";
        return "";
    }
    static String familyOf(String name, org.json.JSONObject o) {
        String f = familyWord(name); if (!f.isEmpty() || o == null) return f;
        f = familyWord(o.optString("name", "")); if (!f.isEmpty()) return f;
        f = familyWord(o.optString("description", "")); if (!f.isEmpty()) return f;
        return familyWord(o.optString("libraryName", ""));
    }
    // (2.39) can a driver of this family run on this phone (the device nodes problem / vk_driver.c check); "" = unknown = anywhere
    static boolean familyFits(String fam) {
        switch (fam) {
            case "adreno": return turnipPossible();
            case "mali": return mali();
            case "xclipse": return !(kgsl() || mali() || powervr());
            case "powervr": return powervr();
            default: return true;
        }
    }
    // (2.39) the checks that need only the folder name and meta.json (also for the APK's assets before they are copied): another GPU family,
    // a glibc build, or a meta.json minApi above this Android (RADV-Xclipse says 34); null = may run here
    static String packageProblem(String name, org.json.JSONObject meta) {
        String fam = familyOf(name, meta);
        if (!familyFits(fam)) return "no-" + fam;
        if (meta != null && meta.optString("abi", "").toLowerCase(Locale.ROOT).contains("glibc")) return "glibc-build";
        int min = meta != null ? meta.optInt("minApi", 0) : 0;
        if (min > android.os.Build.VERSION.SDK_INT) return "min-api " + min;
        return null;
    }
    org.json.JSONObject meta(String name) {
        try { return new org.json.JSONObject(readSmall(new File(new File(dir(), name), "meta.json"))); }
        catch (Exception e) { return null; }
    }
    String family(String name) { return familyOf(name, meta(name)); }
    // Why a driver package cannot run on this phone, or null: a package for another GPU family (no-adreno: Turnip needs an Adreno 6xx+ on KGSL /
    // no-mali / no-xclipse / no-powervr), a Linux glibc build (glibc-build: the "-EMULATOR" PanVK zips are for Winlator-style glibc containers;
    // DOOM issue #3 picked one and it needed libdrm.so.2), a versioned library it needs and does not ship (missing-lib), a meta.json minApi
    // above this Android (min-api N). vk_driver.c repeats these checks natively (DM_VK_DRIVER_FORCE=1 skips them there).
    String problem(String name) {
        File d = new File(dir(), name);
        org.json.JSONObject meta = meta(name);
        String why = packageProblem(name, meta); if (why != null) return why;
        File lib = driverLib(d, meta);
        if (lib != null) for (String n : elfNeeded(lib)) {
            if (n.equals("libc.so.6") || n.equals("libm.so.6") || n.equals("libpthread.so.0") || n.equals("libdl.so.2") || n.contains("ld-linux")) return "glibc-build";
            if (n.matches(".*\\.so\\.[0-9].*") && !new File(d, n).exists()) return "missing-lib " + n;
        }
        return null;
    }
    // The player's words for a problem / vk_driver.c status reason.
    String reasonText(String r) {
        if (r == null || r.isEmpty()) return "";
        switch (r) {
            case "no-adreno": return t("он только для Adreno 6xx+ (Snapdragon)", "it is for Adreno 6xx+ (Snapdragon) GPUs only");
            case "no-mali": return t("он только для Mali", "it is for Mali GPUs only");
            case "no-xclipse": return t("он только для Samsung Xclipse", "it is for Samsung Xclipse GPUs only");
            case "no-powervr": return t("он только для PowerVR", "it is for PowerVR GPUs only");
            case "glibc-build": return t("это сборка для Linux/glibc (эмуляторы вроде Winlator glibc), нужен Android-пакет этого драйвера (например …-adrenotools.zip)",
                "this is a Linux glibc build (for Winlator-style glibc containers); use the Android package of this driver (e.g. …-adrenotools.zip)");
            case "not-taken-over": return t("драйвер не загрузился, вместо него работал системный (подробности в логе)", "the driver did not load and the system driver ran instead (details in the log)");
            case "no-gpu": return t("драйвер не видит GPU этого телефона", "the driver sees no GPU on this phone");
            case "load-failed": return t("драйвер не открылся", "the driver could not be opened");
            case "no-library": return t("в пакете нет библиотеки драйвера", "the package has no driver library");
            default:
                if (r.startsWith("missing-lib")) return t("в пакете не хватает библиотеки", "the package lacks a library") + r.substring(11);
                if (r.startsWith("min-api")) { int api = 0; try { api = Integer.parseInt(r.substring(7).trim()); } catch (NumberFormatException e) { /* shown as is */ }
                    String need = androidName(api), have = androidName(android.os.Build.VERSION.SDK_INT);
                    return t("нужен Android " + need + " или новее, а здесь Android " + have, "it needs Android " + need + " or newer, this phone has Android " + have); }
                return r;
        }
    }
    // Driver packages in private storage, sorted by folder name; (2.39) not the driver manager's own folders (".cache", ".stage-*") nor a
    // half-unpacked "<name>.tmp".
    String[] packages() {
        File[] dirs = dir().listFiles(); java.util.ArrayList<String> out = new java.util.ArrayList<>();
        if (dirs != null) for (File d : dirs) { File[] fs = d.isDirectory() && !d.getName().startsWith(".") && !d.getName().endsWith(".tmp") ? d.listFiles() : null; if (fs == null) continue;
            for (File f : fs) if (f.getName().endsWith(".so")) { out.add(d.getName()); break; } }
        java.util.Collections.sort(out); return out.toArray(new String[0]);
    }
    // "<meta.json name> (<folder>)", or just the folder name when there is no readable meta.json.
    String label(String name) {
        File m = new File(new File(dir(), name), "meta.json");
        try (java.io.FileInputStream in = new java.io.FileInputStream(m)) {
            byte[] b = new byte[(int) Math.min(m.length(), 65536)]; int n = in.read(b);
            String t = new org.json.JSONObject(new String(b, 0, Math.max(n, 0), "UTF-8")).optString("name", "");
            if (!t.isEmpty()) return t + " (" + name + ")";
        } catch (Exception e) { /* no or unreadable meta.json */ }
        return name;
    }

    // ---- files
    // The package's driver library: meta.json libraryName, else its first .so (a *vulkan* one preferred), as vk_driver.c picks it.
    static File driverLib(File dir, org.json.JSONObject meta) {
        String ln = meta != null ? meta.optString("libraryName", "") : "";
        if (!ln.isEmpty() && !ln.contains("/")) { File f = new File(dir, ln); if (f.isFile()) return f; }
        File[] fs = dir.listFiles(); File pick = null;
        if (fs != null) for (File f : fs) if (f.getName().endsWith(".so") && (pick == null || (f.getName().contains("vulkan") && !pick.getName().contains("vulkan")))) pick = f;
        return pick;
    }
    // DT_NEEDED of an ELF64 library (empty on any parse problem); vk_driver.c elf_check reads it the same way.
    static java.util.List<String> elfNeeded(File f) {
        java.util.ArrayList<String> out = new java.util.ArrayList<>();
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            byte[] eh = new byte[64]; r.readFully(eh);
            if (eh[0] != 0x7f || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F' || eh[4] != 2) return out;
            java.nio.ByteBuffer e = java.nio.ByteBuffer.wrap(eh).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            long phoff = e.getLong(32); int phent = e.getShort(54) & 0xffff, phnum = e.getShort(56) & 0xffff;
            if (phent != 56 || phnum == 0 || phnum > 64) return out;
            byte[] phb = new byte[phent * phnum]; r.seek(phoff); r.readFully(phb);
            java.nio.ByteBuffer ph = java.nio.ByteBuffer.wrap(phb).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            long dynOff = -1, dynSz = 0;
            for (int i = 0; i < phnum; i++) if (ph.getInt(i * 56) == 2) { dynOff = ph.getLong(i * 56 + 8); dynSz = ph.getLong(i * 56 + 32); }   // PT_DYNAMIC
            if (dynOff < 0) return out;
            int n = (int) Math.min(dynSz, 512 * 16); byte[] db = new byte[n]; r.seek(dynOff); r.readFully(db);
            java.nio.ByteBuffer d = java.nio.ByteBuffer.wrap(db).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            long strtab = 0, strsz = 0; java.util.ArrayList<Long> need = new java.util.ArrayList<>();
            for (int i = 0; i + 16 <= n; i += 16) { long tag = d.getLong(i), val = d.getLong(i + 8); if (tag == 0) break; if (tag == 5) strtab = val; else if (tag == 10) strsz = val; else if (tag == 1) need.add(val); }
            long stroff = -1;   // DT_STRTAB is a virtual address: the PT_LOAD that holds it gives its file offset
            for (int i = 0; i < phnum; i++) { int b = i * 56; if (ph.getInt(b) == 1) { long off = ph.getLong(b + 8), va = ph.getLong(b + 16), fsz = ph.getLong(b + 32); if (strtab >= va && strtab < va + fsz) stroff = strtab - va + off; } }
            if (stroff < 0) return out;
            for (long v : need) { if (v >= strsz) continue; r.seek(stroff + v); StringBuilder sb = new StringBuilder(); int c; while ((c = r.read()) > 0 && sb.length() < 128) sb.append((char) c); out.add(sb.toString()); }
        } catch (Exception ex) { /* not an ELF we can read: no verdict */ }
        return out;
    }
    static boolean hasDriverLib(File d) { File[] fs = d.listFiles(); if (fs != null) for (File f : fs) if (f.getName().endsWith(".so")) return true; return false; }
    static String readText(File f) {
        try { return new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return null; }
    }
    static String readSmall(File f) throws IOException { try (java.io.InputStream in = new java.io.FileInputStream(f)) { return readStream(in); } }
    static String readStream(java.io.InputStream in) throws IOException {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[8192]; int n; while ((n = in.read(buf)) > 0) b.write(buf, 0, n); return b.toString("UTF-8");
    }
    static long treeSize(File f) {
        if (!f.isDirectory()) return f.length();
        long n = 0; File[] list = f.listFiles();
        if (list != null) for (File c : list) n += treeSize(c);
        return n;
    }
    // (2.39) where a package under files/drivers came from: <package>/.dm_origin = "<kind>\t<detail>", kind bundled (detail: the APK install that
    // copied it, bundleStamp), download (manifest / "<repo> <tag>"), file (the picked zip's name) or import (<data folder>/drivers/...). The driver
    // manager shows it and lets the player delete all but the bundled ones. Packages from before 2.39 have none (DriverManager.origin guesses).
    static void writeOrigin(File dir, String kind, String detail) {
        try (OutputStream os = new FileOutputStream(new File(dir, ORIGIN))) { os.write((kind + "\t" + detail.replace('\n', ' ') + "\n").getBytes(StandardCharsets.UTF_8)); }
        catch (IOException e) { android.util.Log.e("dm", "driver origin " + dir.getName() + ": " + e); }
    }
    static String[] readOrigin(File dir) {
        String s = readText(new File(dir, ORIGIN)); if (s == null || s.trim().isEmpty()) return null;
        String[] f = s.trim().split("\t", 2); return f.length == 2 ? f : new String[] {f[0], ""};
    }
    static byte[] httpGet(String url, int max) throws IOException {
        for (int hop = 0; hop < 5; hop++) {   // GitHub release assets redirect to another host
            java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            c.setConnectTimeout(10000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(false);
            int code = c.getResponseCode();
            if (code / 100 == 3) { url = c.getHeaderField("Location"); c.disconnect(); if (url == null) break; continue; }
            if (code != 200) { c.disconnect(); throw new IOException("HTTP " + code + " " + url); }
            try (java.io.InputStream in = c.getInputStream()) {
                java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[1 << 16]; int n;
                while ((n = in.read(buf)) > 0) { b.write(buf, 0, n); if (b.size() > max) throw new IOException("too large: " + url); }
                return b.toByteArray();
            } finally { c.disconnect(); }
        }
        throw new IOException("too many redirects");
    }
    static String sha256(byte[] d) {
        try { byte[] h = java.security.MessageDigest.getInstance("SHA-256").digest(d); StringBuilder sb = new StringBuilder(); for (byte x : h) sb.append(String.format("%02x", x)); return sb.toString(); }
        catch (java.security.NoSuchAlgorithmException e) { return ""; }
    }
    // Unpack an adrenotools package zip (meta.json + the driver library and anything it ships, at the zip root or in one folder) into out/.
    static void unzipDriver(java.io.InputStream src, File out) throws IOException {
        File tmp = new File(out.getPath() + ".tmp"); deleteDriverTree(tmp); tmp.mkdirs();
        try (java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(src)) {
            java.util.zip.ZipEntry e; byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                // (2.25) also the helper libraries a driver may ship next to it (versioned lib*.so.N) and its json / text files
                String n = new File(e.getName()).getName();
                if (!(n.endsWith(".so") || n.matches(".+\\.so\\.[0-9][0-9.]*") || n.endsWith(".json") || n.endsWith(".txt"))) continue;
                try (OutputStream os = new FileOutputStream(new File(tmp, n))) { int k; while ((k = z.read(buf)) > 0) os.write(buf, 0, k); }
            }
        }
        if (!hasDriverLib(tmp)) { deleteDriverTree(tmp); throw new IOException("no driver library in the package"); }
        deleteDriverTree(out); if (!tmp.renameTo(out)) throw new IOException("rename failed: " + out);
    }
    static void deleteDriverTree(File f) { File[] fs = f.listFiles(); if (fs != null) for (File c : fs) deleteDriverTree(c); f.delete(); }

    // Copy assets/drivers/<name>/* to <dst>/<name>/.
    // (2.39) the APK carries the Turnip builds and, as experimental options, RADV-Xclipse and two PanVK builds (tools/drivers/fetch_drivers.py);
    // only the packages that can run on this phone are copied (packageProblem over the asset's folder name + meta.json: GPU family, glibc,
    // minApi), so an Adreno phone does not get ~70 MB of Mali / Xclipse libraries in its private storage. A package is copied again when this
    // is another APK install than the one that copied it (<name>/.dm_origin "bundled\t<bundleStamp>"): the assets are stored compressed, so
    // their size is unknown and a size check never replaced a same-name package an update brought. A bundled copy that does not fit this phone
    // (turnip27 an older APK put on a Mali phone) is removed; a package of the player's own (origin file / import) is left alone.
    // ([ETERNAL drv] had the same idea with <name>/.apk-stamp; this replaces it.)
    void extractBundled(File dst) {
        String stamp = bundleStamp();
        try {
            String[] names = a().getAssets().list("drivers"); if (names == null) return;
            for (String n : names) {
                String[] files = a().getAssets().list("drivers/" + n); if (files == null || files.length == 0) continue;
                File out = new File(dst, n); String[] org = readOrigin(out);
                org.json.JSONObject meta = null;
                try (java.io.InputStream in = a().getAssets().open("drivers/" + n + "/meta.json")) { meta = new org.json.JSONObject(readStream(in)); } catch (Exception e) { /* no meta.json: the folder name decides */ }
                String why = packageProblem(n, meta);
                if (why != null) {
                    if (out.isDirectory() && (org == null || "bundled".equals(org[0]))) { deleteDriverTree(out); android.util.Log.i("dm", "bundled driver " + n + ": removed, not for this phone (" + why + ")"); }
                    continue;
                }
                if (org != null && "bundled".equals(org[0]) && stamp.equals(org[1]) && hasDriverLib(out)) continue;
                out.mkdirs(); new File(out, ORIGIN).delete();   // a copy cut short is copied again at the next start
                for (String f : files) {
                    try (java.io.InputStream in = a().getAssets().open("drivers/" + n + "/" + f); OutputStream os = new FileOutputStream(new File(out, f))) {
                        byte[] buf = new byte[1 << 16]; int k; while ((k = in.read(buf)) > 0) os.write(buf, 0, k);
                    }
                }
                writeOrigin(out, "bundled", stamp);
                android.util.Log.i("dm", "bundled driver " + n + ": extracted (" + files.length + " files)");
            }
        } catch (IOException e) { android.util.Log.e("dm", "bundled drivers: " + e); }
    }
    // This APK install: versionName + install / update time (a reinstall of the same version copies the bundled drivers again too).
    @SuppressWarnings("deprecation")
    String bundleStamp() {
        try { android.content.pm.PackageInfo pi = a().getPackageManager().getPackageInfo(a().getPackageName(), 0); return pi.versionName + "/" + pi.lastUpdateTime; }
        catch (Exception e) { return "?"; }
    }
    // Copy every driver package <src>/<name>/{meta.json, library} into <dst>/<name>/ when missing or of another size; a package zip dropped
    // as is is unpacked to <dst>/<zip name>/ once.
    static void importDrivers(File src, File dst) {
        File[] dirs = src.listFiles(); if (dirs == null) return;
        for (File d : dirs) {
            if (d.isFile() && d.getName().endsWith(".zip")) {
                String name = d.getName().substring(0, d.getName().length() - 4); File out = new File(dst, name);
                if (!hasDriverLib(out)) try (java.io.InputStream in = new java.io.FileInputStream(d)) { unzipDriver(in, out); writeOrigin(out, "import", d.getPath()); android.util.Log.i("dm", "driver package " + name + ": unpacked from zip"); }
                catch (IOException e) { android.util.Log.e("dm", "driver package " + d.getName() + ": " + e); }
                continue;
            }
            File[] files = d.isDirectory() ? d.listFiles() : null; if (files == null) continue;
            File out = new File(dst, d.getName()); out.mkdirs();
            boolean copied = false;
            for (File f : files) {
                File o = new File(out, f.getName());
                if (o.length() == f.length() || f.getName().equals(ORIGIN)) continue;
                try (java.io.InputStream in = new java.io.FileInputStream(f); OutputStream os = new FileOutputStream(o)) {
                    byte[] buf = new byte[1 << 16]; int n; while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                    android.util.Log.i("dm", "driver package " + d.getName() + ": " + f.getName() + " copied"); copied = true;
                } catch (IOException e) { android.util.Log.e("dm", "driver package " + d.getName() + ": " + e); }
            }
            if (copied || readOrigin(out) == null) writeOrigin(out, "import", d.getPath());   // (2.39) the driver manager shows where it came from
        }
    }
    // Driver packages from the internet (when there is a connection): the repository's drivers/manifest.json lists adrenotools packages (name,
    // url, sha256, optional "auto" GPU regex, "family"). Missing ones are downloaded in the background, checked against their SHA-256 and
    // unpacked to <dir>/<name>/; they show up in the settings and, when "auto" matches this GPU, are used by default from the next launch.
    // Offline nothing happens. (2.39) family-aware: an entry's "family" (else the family words of its name + title) must fit this phone, so a Mali
    // phone does not download the Turnip builds; a package the player deleted in the driver manager (pref driver_removed) is not downloaded again.
    private void updateInBackground(final File dir) {
        final String url = host.driverManifestUrl();
        final java.util.Set<String> removed = new java.util.HashSet<>(prefs().getStringSet("driver_removed", java.util.Collections.emptySet()));
        new Thread(() -> {
            try {
                byte[] man = httpGet(url, 1 << 20);
                org.json.JSONArray pk = new org.json.JSONObject(new String(man, "UTF-8")).getJSONArray("packages");
                dir.mkdirs(); try (FileOutputStream os = new FileOutputStream(new File(dir, "manifest.json"))) { os.write(man); }
                for (int i = 0; i < pk.length(); i++) {
                    org.json.JSONObject o = pk.getJSONObject(i); String name = o.getString("name");
                    if (!name.matches("[A-Za-z0-9._-]+") || name.startsWith(".") || hasDriverLib(new File(dir, name)) || removed.contains(name)) continue;
                    String fam = o.optString("family", ""); if (fam.isEmpty()) fam = familyWord(name + " " + o.optString("title", ""));
                    if (!familyFits(fam)) { android.util.Log.i("dm", "driver " + name + ": for " + fam + ", not this GPU (" + phoneFamily() + "), not downloaded"); continue; }
                    byte[] zip = httpGet(o.getString("url"), 64 << 20);
                    String got = sha256(zip);
                    if (!got.equalsIgnoreCase(o.getString("sha256"))) { android.util.Log.e("dm", "driver " + name + ": SHA-256 mismatch " + got + ", skipped"); continue; }
                    unzipDriver(new java.io.ByteArrayInputStream(zip), new File(dir, name));
                    writeOrigin(new File(dir, name), "download", "manifest");
                    android.util.Log.i("dm", "driver " + name + ": downloaded (" + zip.length + " bytes)");
                }
            } catch (Exception e) { android.util.Log.i("dm", "driver update skipped: " + e); }
        }, "dm-driver-update").start();
    }
}
