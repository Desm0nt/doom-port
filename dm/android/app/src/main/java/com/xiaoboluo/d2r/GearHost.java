package com.xiaoboluo.d2r;

import android.app.Activity;
import android.content.SharedPreferences;
import java.io.File;
import java.util.List;

// [front] What the shared gear (settings) dialog asks of a port (frontend-common, docs/FRONTEND_UNIFIED.md "Gear"). The port's DmActivity
// implements it next to FrontMenu.Host; the shared code (GearSettings, GearPages, GearDisplay, VkDrivers, DriverManager) never names a game.
//   - which sections the dialog shows and in what order: gearSections() returns them, the generic ones made by GearPages (frame rate limit,
//     sharpening, Vulkan driver, game language, vibration, look, dead zone, FPS counter, menu button, on-screen controls, Switch mode, picture
//     shape...) mixed with the port's own GearSettings.Section classes (DOOM 2016: dynamic resolution, light mode, aim assist, game folder);
//   - the same list sets the environment at the start (GearSettings.applyEnv, before the library loads): each section's env();
//   - native calls are the port's: a section that needs one calls the port (its own section) or a GearHost method with a default below.
// Pref files: gamePrefs() is the port's "doom_graphics" (every port so far), the pad's is FrontMenu.Pad.prefsName() ("doom_controls").
interface GearHost {
    Activity activity();
    String dataDirName();                       // Download/<this> (FRONT_DATA_DIR, e.g. "DOOM Mobile"): driver import fallback, the download User-Agent
    SharedPreferences gamePrefs();              // "doom_graphics": the next-launch settings
    FrontMenu.Pad pad();                        // the on-screen pad (null: none yet / no pad); reloadSettings() after a change
    FrontMenu front();                          // the pause menu (null: none); the layout editor starts through it (the game stays held)
    boolean libraryLoaded();                    // libdm.so (or the host library) is loaded: native calls may be made
    boolean started();                          // the game runs
    void crumb(String line);                    // a "[java] ..." line in the port's run log (DOOM: nativeLog into boot.log; else logcat)
    void immersive();                           // the system bars hidden again (after a dialog)
    VkDrivers drivers();                        // the port's one VkDrivers (made in onCreate: new VkDrivers(this)); null = no driver choice
    List<GearSettings.Section> gearSections(GearSettings g);   // the dialog's sections, top to bottom (also used for the start environment)

    // ---- optional, with the behaviour of DOOM 2016 as the default
    default String gearTitle(GearSettings g) { return g.s("gear_title"); }
    // true = the dialog's texts are exactly those of the port's own string resources, as its dialog had them before the move (DOOM 2016, its
    // res/values*/doom_settings.xml + update_notice.xml, the source of i18n/gear_text.tsv): a GearText key the port has as a string resource of
    // the same name comes from there (GearText.s: Android's own locale choice, e.g. zh-TW gets values-zh only as Android decides), and GearSettings.str
    // uses the activity's resources, not the pause menu's language (front ui_lang). The Ui.t texts follow ui_lang either way. false (default) =
    // GearText's table, in the menu language when one is picked (the generic behaviour of a newly migrated port).
    default boolean gearUsesPortResources() { return false; }
    // vk_driver.c status "<requested>\t<active>\t<reason>[\t<details>]" (JNI nativeDriverStatus), null when unknown / no such native
    default String driverStatus() { return null; }
    // {fps, mean ms, max ms, GPU ms} of the last 0.5 s (JNI nativeFrameStats), null when unknown: the driver crash guard clears on fps > 0
    default float[] frameStats() { return null; }
    // where the player drops driver packages (folders or zips): <data folder>/drivers (DOOM: GameFolder.dir(this)/drivers)
    default File driverImportDir() { return new File(android.os.Environment.getExternalStorageDirectory(), "Download/" + dataDirName() + "/drivers"); }
    // the default driver package when the player has not picked one ("" = the system driver): DOOM's rule per GPU (VkDrivers.gpuDefault)
    default String defaultDriver(VkDrivers d) { return d.gpuDefault(); }
    // a package forced over the saved choice (DOOM's diagnostics APK -PdmDiagDriver), null = none
    default String forcedDriver() { return null; }
    // the repository files the driver code reads (null = no download): drivers/manifest.json (auto updates) and drivers/sources.json (the manager)
    default String driverManifestUrl() { return VkDrivers.MANIFEST_URL; }
    default String driverSourcesUrl() { return VkDrivers.SOURCES_URL; }
    // the request code the driver manager's file picker uses; the port's onActivityResult hands it to DriverManager.picked
    default int driverZipRequest() { return DriverManager.REQ_ZIP; }
    // ---- [safe-mode] safe graphics mode for Mali / PowerVR (DOOM BUGS_AND_FIXES 2.57; SafeMode.java, DmGuard.java, GearPages.safeMode). A port
    // opts in: one SafeMode made in onCreate and returned here, the two natives answered, GearPages.safeMode listed in gearSections, DmGuard
    // declared in its manifest. The defaults (null) keep a port without it exactly as before.
    default SafeMode safeMode() { return null; }
    // the native decision "<eligible>	<step>	<how>	<family>	<count>	<last why>	<switches set>" ("" = not decided yet), null = no such native
    default String safeModeStatus() { return null; }
    // the next in-process fallback message "<kind>	<detail>" (kind zc / fence / pipe; "" = none), null = no such native
    default String safeModeEvent() { return null; }
    // the folder with the logs for a bug report (the safe-mode notice names it), null = not named
    default File logsDir() { return null; }
}
