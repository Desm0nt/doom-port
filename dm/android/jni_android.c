// App 形态的 JNI 入口（Java 侧 app/src/main/java/com/xiaoboluo/d2r/DmActivity.java）：对应 iOS 的 main.m 启动链 + front_ios.m 的生命周期 / 触屏转发。
//   nativeStart：备好目录与环境变量（同 iOS boot_prepare：DM_ROMFS / DM_SAVE / DM_TEMP、cwd），stderr 接到日志文件 + logcat，起宿主线程跑 dm_main。
//   nativeSurface：交来 / 收回 ANativeWindow（front_android.c）；nativeTouch：整组触点（front.h dm_touch_set）；nativeFocus：场景生命周期（同 iOS 1 / 2 / 3 + 后台挂起）。
// 目录约定（都不进 APK，见 dm/android/README）：
//   <外部私有目录>/dm/out/   改写产物（*.patched.bin 与各表，patch_nso.py DM_TARGET=android 出），开发时 adb push
//   资源目录（缺省 /sdcard/Download/S2X/D2R Mobile/data）  玩家的 18 个 .data 档案 + .build.config；App 在内部目录建 romfs/data/ 逐个链接过去
//   <内部目录>/save、temp            存档与临时文件
// JNI entry of the App form (Java side app/src/main/java/com/xiaoboluo/d2r/DmActivity.java): counterpart of iOS's main.m startup chain + front_ios.m's lifecycle / touch forwarding.
//   nativeStart: prepare directories and environment variables (as iOS boot_prepare: DM_ROMFS / DM_SAVE / DM_TEMP, cwd), connect stderr to a log file + logcat, start the host thread running dm_main.
//   nativeSurface: hand over / take back the ANativeWindow (front_android.c); nativeTouch: the whole set of touch points (front.h dm_touch_set); nativeFocus: scene lifecycle (as iOS 1 / 2 / 3 + background suspend).
// Directory conventions (none of these go into the APK, see dm/android/README):
//   <external private dir>/dm/out/   rewriter products (*.patched.bin and tables, from patch_nso.py DM_TARGET=android), adb push during development
//   resource dir (default /sdcard/Download/S2X/D2R Mobile/data)  the player's 18 .data archives + .build.config; the App creates romfs/data/ in the internal dir linking to each of them
//   <internal dir>/save, temp        saves and temporary files
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/hle.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dirent.h>
#include <errno.h>
#include <jni.h>
#include "vulkan_present.h"
#include "touch_android.h"
#include "lifecycle_dispatch.h"
#ifndef DM_GAME_DOOM   // [DOOM] D2R overlay contract is not built
#include "overlay_validate.h"
#include "dm_overlay_contract.h"
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include "../host/dm_cpu.h"   // [DOOM] getauxval + dm_cpu_lse() for the boot cpu line
#include "cpu_topo.h"          // [DOOM] thread placement by core capacity: the [cpu] topology / plan lines at boot

void front_android_set_window(void *win, int w, int h);
void front_android_set_refresh(float hz);
int dm_main(int argc, char **argv);
void front_android_boot_failed(int code);

static char g_files[1024], g_ext[1024], g_src[1024], g_runtime[1100];
static FILE *g_logf;

#ifdef DM_ANDROID_DEBUG_ENV
#include "debug_env.h"
/* 专用调试文件只供性能A/B，不读取游戏通用env，也不开放同步等危险键。
 * This dedicated performance A/B file is separate from the game's general env and never exposes synchronization keys. */
static void debug_environment(void) {
    char path[1200];snprintf(path,sizeof path,"%s/dm/android-debug.env",g_files);
    dm_android_debug_values values;
    int result=dm_android_debug_load(path,&values);
    if(result<0) { LOG("boot: debug environment rejected");return; }
    if(!result)return;
    for(unsigned i=0;i<3;++i)if(values.present&(1u<<i))setenv(dm_android_debug_keys[i],values.value[i],1);
    LOG("boot: debug environment allowlist applied");
}
#endif

// 挂起只能在原生pthread栈上执行；UI只登记最新状态，不能等正在等待Surface的呈现调用。
// Suspend only on a native pthread stack; UI posts latest state without waiting for a present that needs a Surface.
static dm_lifecycle_dispatch g_lifecycle;
static pthread_once_t g_lifecycle_once = PTHREAD_ONCE_INIT;
static int g_lifecycle_error;
static void lifecycle_graphics(int on) {
    __android_log_print(ANDROID_LOG_INFO, "dm-lifecycle", "graphics suspend=%d begin", on);
    void dm_hold_bg_graphics(int on); dm_hold_bg_graphics(on);   // [front] = dm_gfx_suspend unless the pause menu holds it (hle_hold.c)
    __android_log_print(ANDROID_LOG_INFO, "dm-lifecycle", "graphics suspend=%d complete", on);
}
static void lifecycle_init(void) {
    void dm_hold_bg_audio(int on);   // [front] the pause menu holds audio / graphics too (hle_hold.c): the effective state is either
    dm_lifecycle_ops ops = {dm_focus_set, dm_hold_bg_audio, lifecycle_graphics};
    g_lifecycle_error = dm_lifecycle_start(&g_lifecycle, ops);
    if (!g_lifecycle_error) pthread_setname_np(g_lifecycle.thread, "dm-lifecycle");
    if (g_lifecycle_error) __android_log_print(ANDROID_LOG_ERROR, "dm-lifecycle", "worker start failed rc=%d", g_lifecycle_error);
}
static int lifecycle_ready(void) { pthread_once(&g_lifecycle_once, lifecycle_init); return !g_lifecycle_error; }

// stderr → 管道 → 本线程：逐行写日志文件（adb pull 取）并转 logcat（标签 dm）
// stderr → pipe → this thread: write line by line to the log file (fetched with adb pull) and forward to logcat (tag dm)
// [DOOM] (BUGS_AND_FIXES 2.22) The log no longer grows forever: every launch moves boot.log to boot.prev.log (one previous run kept) and
// starts a new one; within a run, past DM_LOG_MAX_MB (default 32) the file is cut back to its first 4 MB (the boot: device, driver, GPU)
// and goes on from there, so the head and the latest lines both survive. The app's own __android_log_print lines (dm-vulkan: the
// engine's xbg_logf, swapchain, FATAL; dm-lifecycle; dm-input) are copied into the file too (__wrap___android_log_print below; the
// link uses -Wl,--wrap). A crash (crash.c report hook) marks <files>/dm-crash.txt and fsyncs the log; DmActivity / CrashLog.java then
// writes Download/DOOM Mobile/logs/crash.txt from it at the next start.
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -1;
static char g_log_path[1200], g_crash_flag[1200];
static _Atomic int g_pump_busy;
static char g_crash_note[384];   // the last FATAL line: copied into the crash marker (read in the signal handler, racy by design)
static long g_log_head = -1, g_log_max = 32l << 20;
#define LOG_HEAD_KEEP (4l << 20)
// [DOOM] DM_LOG_TIME=1 (the diagnostics APK sets it): every boot.log line starts with the local wall-clock time it was written,
// "HH:MM:SS.mmm ", so a report shows when each step happened and how long the run lasted (logcat has its own stamps anyway)
static int g_log_time = -1;
static void log_file_line(const char *tag, const char *s) {
    if (!g_logf) return;
    pthread_mutex_lock(&g_log_mu);
    if (g_log_time < 0) { const char *e = getenv("DM_LOG_TIME"); g_log_time = e && *e && strcmp(e, "0") ? 1 : 0; }
    if (g_log_time) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm tm; time_t sec = ts.tv_sec; localtime_r(&sec, &tm);
        fprintf(g_logf, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    }
    if (tag) fprintf(g_logf, "[%s] %s\n", tag, s); else { fputs(s, g_logf); fputc('\n', g_logf); }
    fflush(g_logf);
    long at = ftell(g_logf);
    if (g_log_head < 0 && at >= LOG_HEAD_KEEP) g_log_head = at;   // the boot head ends at this line
    if (g_log_head > 0 && at > g_log_max) {
        if (!ftruncate(g_log_fd, g_log_head)) { fseek(g_logf, 0, SEEK_END);
            fprintf(g_logf, "[dm] [log] boot.log passed %ld MB: cut back to its first %ld MB (DM_LOG_MAX_MB), later lines follow\n", g_log_max >> 20, g_log_head >> 20); fflush(g_logf); }
    }
    pthread_mutex_unlock(&g_log_mu);
}
static void note_fatal(const char *s) { if (strstr(s, "FATAL")) snprintf(g_crash_note, sizeof g_crash_note, "%s", s); }
static void *log_pump(void *arg) {
    int fd = (int)(intptr_t)arg; char buf[4096]; size_t have = 0; ssize_t k;
    while ((k = read(fd, buf + have, sizeof buf - 1 - have)) > 0) {
        atomic_store(&g_pump_busy, 1);
        have += (size_t)k; buf[have] = 0; char *s = buf, *nl;
        while ((nl = strchr(s, '\n'))) { *nl = 0; __android_log_write(ANDROID_LOG_INFO, "dm", s); note_fatal(s); log_file_line(NULL, s); s = nl + 1; }
        have = strlen(s); memmove(buf, s, have);
        if (have == sizeof buf - 1) { __android_log_write(ANDROID_LOG_INFO, "dm", buf); log_file_line(NULL, buf); have = 0; }
        atomic_store(&g_pump_busy, 0);
    }
    return NULL;
}
// Every __android_log_print of libdm.so (vulkan_present.cpp dm-vulkan incl. the engine's log and FATAL callback, dm-lifecycle,
// dm-input, dm-vvl) comes here (CMakeLists.txt: -Wl,--wrap=__android_log_print): to logcat as before, and into boot.log.
int __wrap___android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char text[1024]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    int r = __android_log_write(prio, tag, text);
    if (prio >= ANDROID_LOG_FATAL) snprintf(g_crash_note, sizeof g_crash_note, "[%s] FATAL: %s", tag ? tag : "?", text);
    log_file_line(tag ? tag : "?", text);
    return r;
}
// Async-signal-safe helpers for the crash hook (no stdio, no locks).
static size_t s_hex(char *o, uint64_t v) { char t[17]; int n = 0; do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); o[0] = '0'; o[1] = 'x'; for (int i = 0; i < n; i++) o[2 + i] = t[n - 1 - i]; return (size_t)n + 2; }
static size_t s_dec(char *o, int v) { char t[12]; int n = 0; unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v; do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u); size_t k = 0; if (v < 0) o[k++] = '-'; while (n) o[k++] = t[--n]; return k; }
static size_t s_cat(char *o, const char *s) { size_t n = 0; while (s[n]) { o[n] = s[n]; n++; } return n; }
static void crash_report_hook(int phase, int sig, uint64_t addr, uint64_t pc) {
    if (phase == 0) {   // the marker first: <files>/dm-crash.txt "signal=<n> addr=<hex> pc=<hex>\n<last FATAL line>\n"
        int fd = g_crash_flag[0] ? open(g_crash_flag, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
        if (fd < 0) return;
        char b[640]; size_t n = 0;
        n += s_cat(b + n, "signal="); n += s_dec(b + n, sig); n += s_cat(b + n, " addr="); n += s_hex(b + n, addr); n += s_cat(b + n, " pc="); n += s_hex(b + n, pc); b[n++] = '\n';
        for (size_t i = 0; g_crash_note[i] && n < sizeof b - 2; i++) b[n++] = g_crash_note[i];
        b[n++] = '\n';
        if (write(fd, b, n) < 0) {} fsync(fd); close(fd);
        return;
    }
    // phase 1: let the pump put the dump into boot.log (the pipe empty and the pump idle twice in a row), then fsync it; at most 1.5 s
    int quiet = 0;
    for (int i = 0; i < 300 && quiet < 2; i++) {
        int n = 0; quiet = !ioctl(2, FIONREAD, &n) && n == 0 && !atomic_load(&g_pump_busy) ? quiet + 1 : 0;
        struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL);
    }
    if (g_log_fd >= 0) fsync(g_log_fd);
}
void crash_set_report_hook(void (*fn)(int phase, int sig, uint64_t addr, uint64_t pc));   // crash.c
static void prop(const char *name, char *out) { out[0] = 0; __system_property_get(name, out); if (!out[0]) snprintf(out, PROP_VALUE_MAX, "-"); }
static void log_header(void) {
    char mf[PROP_VALUE_MAX], md[PROP_VALUE_MAX], dv[PROP_VALUE_MAX], sm[PROP_VALUE_MAX], so[PROP_VALUE_MAX], hw[PROP_VALUE_MAX], bp[PROP_VALUE_MAX], rel[PROP_VALUE_MAX], sdk[PROP_VALUE_MAX], fp[PROP_VALUE_MAX];
    prop("ro.product.manufacturer", mf); prop("ro.product.model", md); prop("ro.product.device", dv); prop("ro.soc.manufacturer", sm); prop("ro.soc.model", so);
    prop("ro.hardware", hw); prop("ro.board.platform", bp); prop("ro.build.version.release", rel); prop("ro.build.version.sdk", sdk); prop("ro.build.fingerprint", fp);
    time_t now = time(NULL); struct tm tm; char when[64]; localtime_r(&now, &tm); strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", &tm);
    LOG("boot: log %s (the previous run: boot.prev.log), pid %d, %s, app %s", g_log_path, (int)getpid(), when, getenv("DM_APP_VERSION") ? getenv("DM_APP_VERSION") : "?");
    LOG("boot: device %s %s (%s), SoC %s %s, hardware %s, platform %s, Android %s (SDK %s), build %s", mf, md, dv, sm, so, hw, bp, rel, sdk, fp);
    char model[96] = ""; FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r"); if (f) { if (fgets(model, sizeof model, f)) model[strcspn(model, "\r\n")] = 0; fclose(f); }
    long total = 0, avail = 0; f = fopen("/proc/meminfo", "r");
    if (f) { char l[128]; while (fgets(l, sizeof l, f)) { sscanf(l, "MemTotal: %ld", &total); sscanf(l, "MemAvailable: %ld", &avail); } fclose(f); }
    LOG("boot: gpu kgsl=%d gpu_model=%s mali=%d, memory %ld MB (available %ld MB), Vulkan driver asked: %s", !access("/dev/kgsl-3d0", F_OK) || !access("/sys/class/kgsl/kgsl-3d0", F_OK),
        model[0] ? model : "-", !access("/dev/mali0", F_OK), total >> 10, avail >> 10, getenv("DM_VK_DRIVER") && *getenv("DM_VK_DRIVER") ? getenv("DM_VK_DRIVER") : "system");
}
static void log_limit(void) { const char *e = getenv("DM_LOG_MAX_MB"); if (e && atol(e) >= 8) g_log_max = atol(e) << 20; }   // again after dm/env
// [DOOM] One boot line with the CPU features that matter for the machine code we ship (GitHub issue #10: SIGILL on the NVN thunks'
// stadd on a Snapdragon 680 = Cortex-A73/A53, ARMv8.0 without LSE): AT_HWCAP / AT_HWCAP2 bits, the cores' implementer:part and the
// variant our hand-written code runs (host/dm_cpu.h). Called after the env files, so a forced DM_CPU_NOLSE=1 shows here too.
static const char *cpu_part_name(unsigned impl, unsigned part) {
    static const struct { unsigned short impl, part; const char *name; } t[] = {
        {0x41, 0xd03, "A53"}, {0x41, 0xd04, "A35"}, {0x41, 0xd05, "A55"}, {0x41, 0xd07, "A57"}, {0x41, 0xd08, "A72"}, {0x41, 0xd09, "A73"},
        {0x41, 0xd0a, "A75"}, {0x41, 0xd0b, "A76"}, {0x41, 0xd0d, "A77"}, {0x41, 0xd41, "A78"}, {0x41, 0xd44, "X1"}, {0x41, 0xd46, "A510"},
        {0x41, 0xd47, "A710"}, {0x41, 0xd48, "X2"}, {0x41, 0xd4d, "A715"}, {0x41, 0xd4e, "X3"}, {0x41, 0xd80, "A520"}, {0x41, 0xd81, "A720"},
        {0x41, 0xd82, "X4"}, {0x41, 0xd85, "X925"}, {0x41, 0xd87, "A725"}, {0x51, 0x800, "Kryo-gold/A73"}, {0x51, 0x801, "Kryo-silver/A53"},
        {0x51, 0x802, "Kryo-gold/A75"}, {0x51, 0x803, "Kryo-silver/A55"}, {0x51, 0x804, "Kryo-gold/A76"}, {0x51, 0x805, "Kryo-silver/A55"},
        {0x51, 0x001, "Oryon"}};
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (t[i].impl == impl && t[i].part == part) return t[i].name;
    return "?";
}
static void log_cpu(void) {
    unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
    // cores: "N x impl:part (name)" per distinct pair, from /proc/cpuinfo (CPU implementer comes before CPU part in each block)
    struct { unsigned impl, part, n; } k[8]; unsigned nk = 0, impl = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char l[256]; unsigned v;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, "CPU implementer : %x", &v) == 1) impl = v;
            else if (sscanf(l, "CPU part : %x", &v) == 1) {
                unsigned j = 0; while (j < nk && !(k[j].impl == impl && k[j].part == v)) j++;
                if (j < nk) k[j].n++; else if (nk < 8) { k[nk].impl = impl; k[nk].part = v; k[nk].n = 1; nk++; }
            }
        }
        fclose(f);
    }
    char cores[320] = ""; size_t o = 0;
    for (unsigned j = 0; j < nk && o < sizeof cores - 48; j++)
        o += (size_t)snprintf(cores + o, sizeof cores - o, "%s%ux 0x%02x:0x%03x (%s)", j ? " + " : "", k[j].n, k[j].impl, k[j].part, cpu_part_name(k[j].impl, k[j].part));
#define B(w, n) (int)(((w) >> (n)) & 1)
    LOG("boot: cpu hwcap=%#lx hwcap2=%#lx atomics(LSE)=%d lrcpc=%d ilrcpc=%d dotprod=%d crc32=%d aes=%d pmull=%d sha2=%d fphp=%d asimdhp=%d "
        "rdm=%d jscvt=%d fhm=%d sve=%d sve2=%d i8mm=%d bf16=%d bti=%d mte=%d; cores %s; our asm: %s",
        hw, hw2, B(hw, 8), B(hw, 15), B(hw, 26), B(hw, 20), B(hw, 7), B(hw, 3), B(hw, 4), B(hw, 6), B(hw, 9), B(hw, 10),
        B(hw, 12), B(hw, 13), B(hw, 23), B(hw, 22), B(hw2, 1), B(hw2, 13), B(hw2, 14), B(hw2, 17), B(hw2, 18), nk ? cores : "?",
        dm_cpu_lse() ? "LSE (ARMv8.1+)" : dm_cpu_hw_lse() ? "ARMv8.0 fallback, forced by DM_CPU_NOLSE" : "ARMv8.0 fallback (no LSE)");
#undef B
}
static void log_init(void) {
    static int initialized; if (initialized) return; initialized = 1;
    char prev[1220]; snprintf(g_log_path, sizeof g_log_path, "%s/boot.log", g_runtime); snprintf(prev, sizeof prev, "%s/boot.prev.log", g_runtime);
    if (rename(g_log_path, prev) && errno != ENOENT) unlink(g_log_path);   // [DOOM] one previous run kept; never append forever
    g_logf = fopen(g_log_path, "w"); g_log_fd = g_logf ? fileno(g_logf) : -1;
    log_limit();
    snprintf(g_crash_flag, sizeof g_crash_flag, "%s/dm-crash.txt", g_files);
    crash_set_report_hook(crash_report_hook);
    int fds[2]; if (pipe(fds)) return;
    dup2(fds[1], 2); close(fds[1]); setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t th; if (!pthread_create(&th, NULL, log_pump, (void *)(intptr_t)fds[0])) { pthread_setname_np(th, "dm-log"); pthread_detach(th); }
    log_header();
}

static void mkdirs(const char *p) {
    char b[1100]; snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0700); *s = '/'; }
    mkdir(b, 0700);
}
// 内部目录 romfs/data/ 下逐个链接到资源目录的文件（游戏按 rom:/data/… 读；链接让资源留在玩家放的位置，不复制 5 GB）
// Under the internal dir romfs/data/, link each file of the resource dir (the game reads rom:/data/...; links keep the resources where the player put them instead of copying 5 GB)
// 剔零档案的「.holes」区段表也要链过去：hle_fs.c 在档案旁边（即 romfs/data/ 下）找它；缺了就按物理偏移读剔过零的档案，第一个零段之后读到的全错位（解压失败、随后崩溃）
// The ".holes" extent tables of zero-stripped archives are linked too: hle_fs.c looks for them next to the archive (in romfs/data/); without them a zero-stripped archive is read at physical offsets, and everything after its first stripped range comes from the wrong place (decompression failures, then a crash)
#ifndef DM_GAME_DOOM   // [DOOM] D2R CASC layout (.data archives + .build.config); Doom uses the extracted RomFS directory as is
static int link_romfs(const char *romfs) {
    char data[1100]; snprintf(data, sizeof data, "%s/data", romfs); mkdirs(data);
    DIR *d = opendir(g_src); if (!d) { LOG("【红】资源目录打不开 %s：errno %d（没给「所有文件访问」权限？）", g_src, errno); return -1; }
    struct dirent *e; int n = 0;
    while ((e = readdir(d))) {
        const char *nm = e->d_name; size_t l = strlen(nm);
        if (!(l > 5 && !strcmp(nm + l - 5, ".data")) && !(l > 11 && !strcmp(nm + l - 11, ".data.holes")) && strcmp(nm, ".build.config") && strcmp(nm, "build.config")) continue;
        char from[1100], to[1100]; snprintf(from, sizeof from, "%s/%s", g_src, nm);
        snprintf(to, sizeof to, "%s/%s", data, strcmp(nm, "build.config") ? nm : ".build.config");   // 电脑上常被存成无点名 | often saved without the leading dot on a computer
        unlink(to); if (!symlink(from, to)) n++;
    }
    closedir(d);
    return n;
}
#endif

static void *host_thread(void *arg) {
    (void)arg;
    char out[1200]; snprintf(out, sizeof out, "%s/out", g_runtime);
    // 图形初始化在宿主线程进行；着色器由玩家在包外提供，失败时停止启动。
    // Initialize graphics on the host thread; shaders stay external, and failure stops startup.
    char shaders[1200], error[512] = {0};
    snprintf(shaders, sizeof shaders, "%s/shaders", g_runtime);
    uint64_t t0 = now_ns(); LOG("boot: Vulkan initialization (shaders %s)", shaders);
    int graphics_rc = dm_android_vk_initialize(shaders, error, sizeof error);
    if (graphics_rc) { LOG("boot: Vulkan initialization failed %d: %s", graphics_rc, error); front_android_boot_failed(-6); return NULL; }
    LOG("boot: Vulkan initialized in %.0f ms", (now_ns() - t0) / 1e6);   // [DOOM] breadcrumb
    char *argv[] = {"dmhost", out, "main", "sdk", NULL};
    LOG("boot: dm_main %s", out);
    int rc = dm_main(4, argv);
    LOG("boot: dm_main 返回 %d", rc);
    front_android_boot_failed(-7);
    return NULL;
}

// 测试用：files/dm/env 每行 KEY=VALUE（# 开头为注释），在下面的缺省值之前设进环境，让 run-as 能改 DM_* 开关（iOS / Mac 用启动环境变量做同样的事）；没有这个文件就什么都不做
// For testing: each KEY=VALUE line of files/dm/env (# starts a comment) is set before the defaults below, so run-as can change DM_* switches (iOS / Mac do the same with launch environment variables); without the file nothing happens
static void load_env_file(const char *dir) {
    char path[1300]; snprintf(path, sizeof path, "%s/env", dir);
    FILE *fp = fopen(path, "r"); if (!fp) return;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!line[0] || line[0] == '#' || !eq || eq == line) continue;
        *eq = 0; setenv(line, eq + 1, 1); LOG("boot: 环境 %s=%s（来自 %s）", line, eq + 1, path);
    }
    fclose(fp);
}

JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeStart(JNIEnv *env, jclass cls, jstring files, jstring ext, jstring src, jstring lang) {
    (void)cls;
    static int started; if (started) return 0;
    const char *f = (*env)->GetStringUTFChars(env, files, NULL), *x = (*env)->GetStringUTFChars(env, ext, NULL), *s = (*env)->GetStringUTFChars(env, src, NULL), *lg = (*env)->GetStringUTFChars(env, lang, NULL);
    snprintf(g_files, sizeof g_files, "%s", f); snprintf(g_ext, sizeof g_ext, "%s", x); snprintf(g_src, sizeof g_src, "%s", s);
    // 启动时间线零点：Process.getStartUptimeMillis() 是 SystemClock.uptimeMillis（CLOCK_MONOTONIC），与 now_ns 同一时钟（见 main.c proc_start_mono_ns）
    // Boot timeline zero: Process.getStartUptimeMillis() is SystemClock.uptimeMillis (CLOCK_MONOTONIC), the same clock as now_ns (see main.c proc_start_mono_ns)
    jclass proc = (*env)->FindClass(env, "android/os/Process");
    g_proc_start_ns = (uint64_t)(*env)->CallStaticLongMethod(env, proc, (*env)->GetStaticMethodID(env, proc, "getStartUptimeMillis", "()J")) * 1000000ull;
    // 调试包可用 run-as 导入内部目录；有完整主程序时优先选内部目录。
    // Debug builds can import via run-as; prefer internal storage when the main module exists.
    char p[1200]; snprintf(g_runtime, sizeof g_runtime, "%s/dm", g_files);
    snprintf(p, sizeof p, "%s/out/main.patched.bin", g_runtime);
    if (access(p, R_OK)) snprintf(g_runtime, sizeof g_runtime, "%s/dm", g_ext);
    mkdirs(g_runtime);
    log_init();
    load_env_file(g_runtime);
    LOG("boot: 内部目录 %s，外部目录 %s，资源 %s，系统语言 %s", g_files, g_ext, g_src, lg);
    char romfs[1100], save[1100], temp[1100];
    snprintf(romfs, sizeof romfs, "%s/romfs", g_files); snprintf(save, sizeof save, "%s/save", g_files); snprintf(temp, sizeof temp, "%s/temp", g_files);
    mkdirs(save); mkdirs(temp);
#ifdef DM_GAME_DOOM
    // [DOOM] DM_ROMFS = the player's extracted RomFS directory itself (no CASC archives to link)
    snprintf(romfs, sizeof romfs, "%s/romfs", g_src);
    snprintf(p, sizeof p, "%s/out/main.patched.bin", g_runtime);
    if (access(p, R_OK)) { snprintf(g_runtime, sizeof g_runtime, "%s/dm", g_src); LOG("boot: runtime %s", g_runtime); load_env_file(g_runtime); }
    log_limit();
    // [DOOM] DM_STDOUT_FILE=1: send this process's stdout (Android drops it) to <runtime>/stdout.txt, e.g. for the shader
    // disassembly of Turnip/ir3 (IR3_SHADER_DEBUG=disasm) or other driver debug output that is not written to logcat.
    { const char *e = getenv("DM_STDOUT_FILE");
      if (e && *e == '1') { char sp[1200]; snprintf(sp, sizeof sp, "%s/stdout.txt", g_runtime);
          if (freopen(sp, "w", stdout)) { setvbuf(stdout, NULL, _IOLBF, 0); LOG("boot: stdout -> %s", sp); } else LOG("boot: stdout -> %s failed", sp); } }
    int n = 1;
    if (access(romfs, R_OK | X_OK)) { LOG("【红】RomFS directory not accessible %s: errno %d", romfs, errno); n = -1; }
#else
    int n = link_romfs(romfs);
    LOG("boot: 资源链接 %d 个文件", n);
#endif
    log_cpu();                                // [DOOM] after the env files (DM_CPU_NOLSE), before any NVN thunk is handed out
    (void)dm_cpu_plan();                      // [DOOM] after the env files (DM_CPU_TOPO): reads the topology once, logs "[cpu] topology" / "[cpu] plan"
    setenv("DM_RUNTIME_DIR", g_runtime, 1);   // [DOOM] hle_nvn.c polls <it>/dump for an on-demand thread dump
    setenv("DM_FILES_DIR", g_files, 0);       // [r5] internal storage (not FUSE): DM_GFX_SHADER_RAWCACHE=2|3 mirrors the shader bundles there
    setenv("DM_ROMFS", romfs, 0); setenv("DM_SAVE", save, 0); setenv("DM_TEMP", temp, 0);
#ifdef DM_ANDROID_DEBUG_ENV
    debug_environment();
#endif
    // 被更新的 GPU 结果覆盖、又只有 GPU 读的存储写回不做（引擎的 gfx-wb-skip；标记文件仍优先）：营地每帧省 1.37 MB、约 1 ms 的回读。
    // Storage writebacks that a newer GPU result supersedes and only the GPU reads are dropped (the engine's gfx-wb-skip; a
    // marker file still wins): 1.37 MB and about 1 ms of readback per camp frame.
    setenv("DM_GFX_WB_SKIP", "1", 0);
    // 绘制状态与上一次绘制相同时直接沿用上一次的管线，不再序列化建管线信息去查（引擎的 gfx-pipe-memo；标记文件仍优先）。
    // A draw whose pipeline state equals the previous draw's reuses its pipeline without serializing the create info for a
    // lookup (the engine's gfx-pipe-memo; a marker file still wins).
    setenv("DM_GFX_PIPE_MEMO", "1", 0);
    // 3D 渲染比例上限 0.85（其他手机平台缺省 0.75，见 hle_oe.c；env 文件仍优先）：Redmi K50 Ultra 营地和 9 分钟战斗都稳 60 帧，比 0.75
    // 多 28% 像素，打 9 分钟多热约 2 °C；重场景或机身过热掉帧时，动态比例每次退 0.05。
    // 3D render scale ceiling 0.85 (other phone platforms default to 0.75, see hle_oe.c; the env file still wins): on the Redmi K50
    // Ultra the camp and a 9-minute fight hold 60 fps with 28% more pixels than 0.75 and about 2 C more heat; when heavy scenes or
    // a hot phone drop frames, the dynamic scale steps down 0.05 at a time.
    setenv("DM_3D_SCALE", "0.85", 0);
    // Vulkan 适配器先沿用同步提交；上游新增翻译线程需独立验证对象生命周期。
    // Keep synchronous submission until the new upstream translation thread's object lifetimes are verified with Vulkan.
    // [DOOM] 29.10 A/B (round 2): translation on its own thread beats translating on the game's render thread (heavy scenes
    // 25 -> 33 fps); DM_NVN_SYNC=1 restores the synchronous path.
    setenv("DM_NVN_SYNC", "0", 0);
    // BC 贴图按原格式采样（引擎的 gfx-native-bc；设备支持时；标记文件仍优先）：Redmi K50 Ultra 营地 GPU 少 0.5 ms，图像内存 827 → 320 MB。
    // BC textures sampled in their own formats where the device can (the engine's gfx-native-bc; a marker file still wins):
    // on the Redmi K50 Ultra camp GPU -0.5 ms and live image memory 827 -> 320 MB.
    setenv("DM_GFX_NATIVE_BC", "1", 0);
    // 片元着色器的四元组邻居交换改用精细导数（引擎的 gfx-quad-derivatives；标记文件仍优先）：Adreno 上辅助调用在子组洗牌里取到垃圾，
    // 罪恶洞穴打斗时单位周围出现黑方块。Adreno 740/750 上引擎缺省关掉它（那两个驱动会丢掉设备，见 xbg_adreno_policy.h）。
    // Fragment-shader quad-neighbour shuffles go through fine derivatives (the engine's gfx-quad-derivatives; a marker file
    // still wins): on Adreno helper invocations read garbage in subgroup shuffles, and black squares appeared around units in
    // the Den of Evil fight. On Adreno 740/750 the engine defaults it off (those drivers drop the device; see xbg_adreno_policy.h).
    setenv("DM_GFX_QUAD_DERIVATIVES", "1", 0);
    // 按 32 道线程束翻译的计算着色器，在 64 道的子组里每 32 道各当一个线程束（引擎的 gfx-warp32；标记文件仍优先）：Adreno 上
    // 杂物、草地、流体与 PPR 着色器的线程束聚合追加丢了 32–63 号车道、条目互相覆盖。Adreno 740/750 上引擎缺省关掉它。
    // Compute shaders translated for 32-lane warps run every 32 lanes of a 64-lane subgroup as their own warp (the engine's
    // gfx-warp32; a marker file still wins): on Adreno the clutter, grass, fluid and PPR shaders' warp-aggregated appends lost
    // lanes 32-63 and overwrote entries. On Adreno 740/750 the engine defaults it off.
    setenv("DM_GFX_WARP32", "1", 0);
    // 翻译器输出的两项等价整理（引擎的 gfx-ldc-fold 与 gfx-forward-temps；标记文件仍优先）：常量缓冲区按字寻址时可推出的分量下标
    // 换成常量，只写一次的临时变量直接转发。Redmi K50 Ultra 营地（比例 0.75）GPU 每帧 15.70 → 14.34 ms，结果不变。
    // Two exact clean-ups of the translator's output (the engine's gfx-ldc-fold and gfx-forward-temps; a marker file still
    // wins): constant-buffer component indexes that follow from known low bits become constants, and single-store temporaries
    // are forwarded. Redmi K50 Ultra camp at scale 0.75: GPU 15.70 -> 14.34 ms a frame, with the same results.
    setenv("DM_GFX_LDC_FOLD", "1", 0);
    setenv("DM_GFX_FORWARD_TEMPS", "1", 0);
    // 浮点 min / max / clamp 照 Maxwell 的规矩遇 NaN 取另一个操作数（引擎的 gfx-nan-minmax；标记文件仍优先）。不开时一加 13
    // （Adreno 830）上恰莎打铁的火花靠近画面上沿时，周围一块矩形会黑一帧。
    // Float min / max / clamp take the other operand when one is NaN, as on Maxwell (the engine's gfx-nan-minmax; a marker file
    // still wins). Without it, on the OnePlus 13 (Adreno 830) a rectangle around Charsi goes black for a frame when her hammer
    // sparks come near the top edge of the screen.
    setenv("DM_GFX_NAN_MINMAX", "1", 0);
    // 帧重叠缺省开：命令缓冲里的 SignalEvent 不提交、留到所在槽退役时再公布（引擎的 gfx-defer-events = 2；标记文件仍优先）。
    // 与呈现端的 gfx-present-overlap 一起，Redmi K50 Ultra 营地从 7.7 fps 到 19.5 fps。
    // Frame overlap on by default: command-buffer SignalEvents do not submit and are published when their slot retires (the
    // engine's gfx-defer-events = 2; a marker file still wins). With the presenter's gfx-present-overlap the Redmi K50 Ultra
    // camp went from 7.7 to 19.5 fps.
    setenv("DM_GFX_DEFER_EVENTS", "2", 0);
    // 不在每个绘制、计算和拷贝前先完成全部待写回与报告（引擎的 gfx-read-preflight；标记文件仍优先）。延后事件下几乎总有报告待写，
    // 开着就等于每个绘制前同步一次 GPU；营地与 Den of Evil 战斗里 CPU 读从未碰到待写结果（NVN PERF why=a0）。
    // CPU reads are not all resolved before every draw, dispatch and copy (the engine's gfx-read-preflight; a marker file still
    // wins). With deferred events a report is nearly always pending, so it would sync the GPU before every draw; in the camp
    // and the Den of Evil fight no CPU read ever met a pending result (NVN PERF why=a0).
    setenv("DM_GFX_READ_PREFLIGHT", "0", 0);
    // 另一个槽还在读的回读缓冲改由 GPU 在它之后写入（引擎的 gfx-xview-guard；标记文件仍优先）：营地、Den of Evil、奶牛关每帧都有一次
    // 这种重用；Redmi K50 Ultra 上每帧约 6 个、1.37 MB，进程内对比测不出代价。
    // Readback buffers the other slot still reads are written by the GPU after it (the engine's gfx-xview-guard; a marker
    // file still wins): such a reuse happens once a frame in the camp, the Den of Evil and the Cow Level; on the Redmi K50
    // Ultra about 6 buffers, 1.37 MB, a frame, at no cost measurable in-process.
    setenv("DM_GFX_XVIEW_GUARD", "1", 0);
    // 本次提交还没用过的图像，整份上传排到提交之前（引擎的 gfx-upload-prologue；标记文件仍优先），不再在帧中间打断通道：Redmi K50 Ultra
    // 进营地时打断通道的上传由每次 382-558 个变为 0，第一帧 GPU 85-107 ms（8 次，均值约 96）对关时 106-115 ms（3 次，均值约 109），
    // 这一帧的上传 CPU 由约 20 ms 降到 12 ms；平常的帧没有上传，不受影响。
    // Whole uploads of images the current submission has not used yet go ahead of it (the engine's gfx-upload-prologue; a
    // marker file still wins) instead of breaking the open pass mid-frame: entering the camp on the Redmi K50 Ultra, uploads that
    // broke a pass went from 382-558 a run to 0, the first frame took 85-107 ms of GPU time (8 runs, mean about 96) against
    // 106-115 ms off (3 runs, mean about 109), and its upload CPU dropped from about 20 to 12 ms; ordinary frames upload nothing.
    setenv("DM_GFX_UPLOAD_PROLOGUE", "1", 0);
    // 着色器载入直接用预热线程改写好的 SPIR-V（引擎的 gfx-shader-reuse；标记文件仍优先）：与现场改写逐字相同，每次进营地少约 100 ms 的
    // 改写 CPU（菜单、进营地、新怪物或新法术出现的帧）；代价是预热留着的、这一局没用到的着色器，约 6-13 MB。
    // Shader loads take the SPIR-V the warm-up threads already rewrote (the engine's gfx-shader-reuse; a marker file still wins):
    // word for word what rewriting on the spot gives, it saves about 100 ms of rewrite CPU per camp entry (menus, entering the
    // camp, frames where a new monster or spell appears); the cost is the kept shaders a session does not load, about 6-13 MB.
    setenv("DM_GFX_SHADER_REUSE", "1", 0);
    // 顶点与索引的跨帧副本缺省开（引擎的 gfx-vcache；靠 nvn_dirty.c 的写保护脏页跟踪，标记文件仍优先）：营地 CPU 少约 0.7 ms。
    // Cross-frame vertex and index copies on by default (the engine's gfx-vcache, through nvn_dirty.c's write-protect
    // tracking; a marker file still wins): about 0.7 ms less CPU per camp frame on the Redmi K50 Ultra.
    setenv("DM_GFX_VCACHE", "1", 0);
    // 贴图内容检查走同一个写保护脏页跟踪（引擎的 gfx-tex-dirty；标记文件仍优先），同 iOS nvn_mtl.m：上传后设只读，游戏写过
    // 才重新散列；原先每张贴图每 16 次提交整张重算一次（Redmi K50 Ultra 洞穴打斗渲染线程每帧约 0.27 ms）。
    // Texture content checks through the same write-protect tracking (the engine's gfx-tex-dirty; a marker file still wins),
    // as nvn_mtl.m does on iOS: write-protected after upload and re-hashed only after a guest write; before, every texture was
    // re-hashed whole on every 16th submission (~0.27 ms of the render thread per Den of Evil fight frame).
    setenv("DM_GFX_TEX_DIRTY", "1", 0);
    // 命令池在另一线程上重置（引擎的 gfx-async-pool-reset；标记文件仍优先）：Redmi K50 Ultra 上 vkResetCommandPool 每帧约占渲染线程 0.4 ms。
    // Command pools reset on another thread (the engine's gfx-async-pool-reset; a marker file still wins): vkResetCommandPool
    // took about 0.4 ms of every frame on the Redmi K50 Ultra's render thread.
    setenv("DM_GFX_ASYNC_POOL_RESET", "1", 0);
    // 不对 NVN PERF 分项逐帧计时（引擎的 gfx-perf-sample；标记文件仍优先）：被计时的那帧渲染线程要多花约 4 ms，在营地里每 32 帧
    // 必误一次垂直同步。测量时用标记打开。
    // No NVN PERF parts timing (the engine's gfx-perf-sample; a marker file still wins): a timed frame took ~4 ms more of the
    // render thread and missed its vsync in the camp, one hitch every 32 frames. Measurement runs turn it on with the marker.
    setenv("DM_GFX_PERF_SAMPLE", "0", 0);
    // 客体主线程的短等待先自旋至多 30 µs 再睡（HLE 的 DM_HLE_SPIN_US）：D2R 主线程每帧约 45 次短暂地等工作线程持有的锁。
    // The guest main thread spins up to 30 us before a wait sleeps (the HLE's DM_HLE_SPIN_US): D2R's main thread waits ~45
    // times a frame, briefly, on locks its job workers hold.
    setenv("DM_HLE_SPIN_US", "30", 0);
    if (!getenv("DM_LANG") && *lg) setenv("DM_LANG", lg, 0);   // 首次安装的语言按系统首选（同 iOS CFLocaleCopyPreferredLanguages，见 dm_i18n.c） | first-install language follows the system preference (as iOS CFLocaleCopyPreferredLanguages, see dm_i18n.c)
    if (chdir(g_files)) LOG("【红】chdir %s 失败 errno %d", g_files, errno);
    (*env)->ReleaseStringUTFChars(env, files, f); (*env)->ReleaseStringUTFChars(env, ext, x); (*env)->ReleaseStringUTFChars(env, src, s); (*env)->ReleaseStringUTFChars(env, lang, lg);
    if (n <= 0) return -2;
    char required[1300];
    const char *modules[] = {"main.patched.bin", "main.fixups.tsv", "main.gates.tsv", "sdk.patched.bin", "sdk.fixups.tsv", "sdk.gates.tsv"};
    for (unsigned i = 0; i < sizeof modules / sizeof modules[0]; i++) {
        snprintf(required, sizeof required, "%s/out/%s", g_runtime, modules[i]);
        if (access(required, R_OK)) { LOG("boot: missing %s", required); return -3; }
    }
    LOG("boot: patched modules present in %s/out (main / sdk: patched.bin, fixups, gates)", g_runtime);   // [DOOM] breadcrumb
#ifndef DM_GAME_DOOM   // [DOOM] .build.config is D2R CASC
    snprintf(required, sizeof required, "%s/data/.build.config", romfs);
    if (access(required, R_OK)) { LOG("boot: missing build configuration"); return -5; }
#endif
    snprintf(required, sizeof required, "%s/shaders", g_runtime);
    DIR *shaders = opendir(required); int have_shader = 0;
    if (shaders) { struct dirent *entry; while ((entry = readdir(shaders))) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && !strcmp(entry->d_name + len - 4, ".xgs")) { have_shader = 1; break; }
    } closedir(shaders); }
    if (!have_shader) { LOG("boot: no shader bundles in %s", required); return -4; }
    LOG("boot: shader bundles found in %s", required);
#ifndef DM_GAME_DOOM   // [DOOM] no D2R touch overlay / overlay contract
    snprintf(required, sizeof required, "%s/overlay", g_runtime);
    // 启动前核必需覆盖层；旧资源不能自动补指纹，也不能带着错误资源进入游戏。
    // Validate the required overlay before starting; never stamp old resources or enter with mismatched content.
    char overlay_reason[256];
    dm_overlay_result overlay = dm_overlay_validate(required, DM_OVERLAY_SOURCE_SHA256, overlay_reason, sizeof overlay_reason);
    if (overlay == DM_OVERLAY_OK) overlay = dm_overlay_require_touch_art(required, overlay_reason, sizeof overlay_reason);
    if (overlay != DM_OVERLAY_OK) {
        LOG("boot: overlay rejected (%d): %s", overlay, overlay_reason);
        return overlay == DM_OVERLAY_MISSING ? -9 : -10;
    }
    LOG("boot: overlay verified source=%s", DM_OVERLAY_SOURCE_SHA256);
    setenv("DM_ROMFS_OVERLAY", required, 1);
#endif
    if (!lifecycle_ready()) return -8;
    dm_aperf_start();             // thermal status listener (perf_android.c)
    dm_haptics_android_start();   // rumble thread, after the env file (DM_RUMBLE...) and before the game asks for vibration devices
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    pthread_t th; int rc = pthread_create(&th, &a, host_thread, NULL); pthread_attr_destroy(&a);
    if (!rc) { started = 1; pthread_detach(th); }
    LOG("boot: host thread %s", rc ? "NOT started" : "started");
    return rc ? -1 : n;
}

static ANativeWindow *g_win;
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeRefresh(JNIEnv *env, jclass cls, jfloat hz) {
    (void)env; (void)cls;
    front_android_set_refresh(hz);
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeSurface(JNIEnv *env, jclass cls, jobject surface, jint w, jint h) {
    (void)cls;
    ANativeWindow *old = g_win;
    g_win = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    dm_android_vk_set_window(g_win);
    front_android_set_window(g_win, w, h);
    if (old) ANativeWindow_release(old);
    LOG("前端：画面 %s %dx%d", g_win ? "就绪" : "收回", w, h);
}

// 触点：Java 按手指槽号（最小空槽，抬起前不变）交整组归一化坐标，n = 0 = 全抬起
// Touch points: Java hands over the whole set of normalized coordinates by finger slot number (lowest free slot, unchanged until lifted), n = 0 = all lifted
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeTouch(JNIEnv *env, jclass cls, jint n, jintArray ids, jfloatArray xs, jfloatArray ys) {
    (void)cls;
    dm_touch t[16]; if (n > 16) n = 16; if (n < 0) n = 0;
    jint id[16]; jfloat x[16], y[16];
    if (n) { (*env)->GetIntArrayRegion(env, ids, 0, n, id); (*env)->GetFloatArrayRegion(env, xs, 0, n, x); (*env)->GetFloatArrayRegion(env, ys, 0, n, y); }
    for (int i = 0; i < n; i++) t[i] = (dm_touch){(uint32_t)id[i], x[i], y[i]};
    dm_touch_set(t, n);
}

// UI只入队：0失焦，1后台，2前台，3激活；激活也声明前台，合并2/3时不会漏恢复。
// UI posts only: 0 unfocused, 1 background, 2 foreground, 3 active; active also declares foreground so merging 2/3 cannot lose resume.
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeFocus(JNIEnv *env, jclass cls, jint ev) {
    (void)env; (void)cls;
    { static const char *const names[] = {"unfocused (onPause)", "background (onStop)", "foreground (onStart)", "active (onResume)"};
      LOG("[life] %s", ev >= 0 && ev <= 3 ? names[ev] : "?"); }   // [DOOM] breadcrumb
    dm_android_touch_focus(ev == 2 || ev == 3);
    front_android_sensors(ev == 2 || ev == 3);          // motion sensors only while in front
    dm_haptics_android_foreground(ev == 2 || ev == 3);  // no rumble left running in the background
    if (lifecycle_ready()) {
        dm_lifecycle_post(&g_lifecycle, ev);
        __android_log_print(ANDROID_LOG_INFO, "dm-lifecycle", "UI posted event=%d", ev);
    }
}

// Application Context for the rumble thread (haptics_android.c), before nativeStart: the JavaVM and a global reference kept for the process.
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeContext(JNIEnv *env, jclass cls, jobject ctx) {
    (void)cls;
    static jobject g_ctx; JavaVM *vm = NULL;
    if (g_ctx || !ctx || (*env)->GetJavaVM(env, &vm) != JNI_OK) return;
    g_ctx = (*env)->NewGlobalRef(env, ctx);
    dm_haptics_android_context(vm, g_ctx);
}
// Display.getRotation() (0..3): the motion sensors are turned to the screen's frame with it (front_android.c).
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeRotation(JNIEnv *env, jclass cls, jint rotation) {
    (void)env; (void)cls;
    front_android_set_rotation(rotation);
}
// Game controllers (DmActivity forwards gamepad / joystick / d-pad sources only). Key: true when the game took it (else the system handles it).
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativePadKey(JNIEnv *env, jclass cls, jint device, jint keycode, jboolean down) {
    (void)env; (void)cls;
    return front_android_pad_key(device, keycode, down) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativePadAxes(JNIEnv *env, jclass cls, jint device, jfloat x, jfloat y, jfloat z, jfloat rz,
                                                                       jfloat lt, jfloat rt, jfloat hx, jfloat hy) {
    (void)env; (void)cls;
    const float axes[8] = {x, y, z, rz, lt, rt, hx, hy};
    front_android_pad_axes(device, axes);
}
// [DOOM] frame counter for the on-screen overlay: {fps, mean frame ms, max frame ms, mean GPU ms (-1 unknown)} over the last 0.5 s
void dm_frame_stats(float out[4]);
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeFrameStats(JNIEnv *env, jclass cls, jfloatArray out) {
    (void)cls; float v[4]; dm_frame_stats(v); (*env)->SetFloatArrayRegion(env, out, 0, 4, v);
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativePadRemoved(JNIEnv *env, jclass cls, jint device) {
    (void)env; (void)cls;
    front_android_pad_removed(device);
}
#ifdef DM_GAME_DOOM
// [DOOM] On-screen look (DoomPadView): a finger delta in dp and the stick tilt-seconds per dp (look sensitivity / 500). The host turns it
// through the game's gyro aim channel (hle_hid.c dm_touch_look_add, DM_TOUCH_LOOK); false = not taken, DoomPadView plays it out on the stick.
int dm_touch_look_add(float dx_dp, float dy_dp, float k);   // hle_hid.c
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeLookDelta(JNIEnv *env, jclass cls, jfloat dx, jfloat dy, jfloat k) {
    (void)env; (void)cls;
    return dm_touch_look_add(dx, dy, k) ? JNI_TRUE : JNI_FALSE;
}
#endif
// [DOOM] Gear dialog, "wide" checkbox changed (DmActivity save): live widescreen switch (DM_DOOM_WIDE_LIVE=1, BUGS_AND_FIXES 2.12).
// The game always renders wide under that switch; "off" is the presenter's centre 16:9 crop. Returns false when the switch is not
// available (the change then applies at the next launch through DM_DOOM_WIDE).
int dm_doom_wide_live(void);           // hle_args.c
void dm_present_crop169(int on);       // vulkan_present.cpp
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeWideLive(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls;
    if (!dm_doom_wide_live()) return JNI_FALSE;
    dm_present_crop169(on ? 0 : 1);
    return JNI_TRUE;
}
// [DOOM] Gear checkbox <-> the game's rs_enable (hle_doom_drs.c): -1 = not known yet, 0 / 1 = off / on; set returns false when the
// value cannot be written live (another main build / no Frame gate).
int dm_doom_drs_get(void);
int dm_doom_drs_set(int on);
JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeGetDrs(JNIEnv *env, jclass cls) { (void)env; (void)cls; return dm_doom_drs_get(); }
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeSetDrs(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls; return dm_doom_drs_set(on ? 1 : 0) ? JNI_TRUE : JNI_FALSE;
}
// [DOOM] Gear <-> the game's controller settings (hle_doom_settings.c, BUGS_AND_FIXES 2.40): id 0 Motion Aiming (0 / 1), 1 its
// sensitivity slider (0..100), 2 Aim Assist (0 / 1). get: -1 = not known yet (profile not loaded); set: false = cannot be written here.
int dm_doom_game_setting_get(int id);
int dm_doom_game_setting_set(int id, int v);
JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeGetGameSetting(JNIEnv *env, jclass cls, jint id) { (void)env; (void)cls; return dm_doom_game_setting_get(id); }
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeSetGameSetting(JNIEnv *env, jclass cls, jint id, jint v) {
    (void)env; (void)cls; return dm_doom_game_setting_set(id, v) ? JNI_TRUE : JNI_FALSE;
}
// [DOOM] r6 (BUGS_AND_FIXES 2.18, hle_doom_drs.c): light mode (the game's handheld quality preset) live; false = the loaded main lacks
// the light-mode patch. DRS target fps (30 / 45 / 60) for rs_drop/raiseMilliseconds while dynamic resolution is on.
int dm_doom_light_set(int on);
void dm_doom_drs_fps_set(int fps);
JNIEXPORT jboolean JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeSetLight(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls; return dm_doom_light_set(on ? 1 : 0) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeSetDrsFps(JNIEnv *env, jclass cls, jint fps) {
    (void)env; (void)cls; dm_doom_drs_fps_set(fps);
}
// [DOOM] Vulkan driver status (vk_driver.c): "<requested>\t<active>\t<reason>", or "" before the driver is loaded.
const char *dm_vk_driver_status(void);
JNIEXPORT jstring JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeDriverStatus(JNIEnv *env, jclass cls) {
    (void)cls; return (*env)->NewStringUTF(env, dm_vk_driver_status());
}

// [front] The unified front end (frontend-common, docs/FRONTEND_UNIFIED.md); the same names and signatures as nx2android's front_android.c and DOOM
// Eternal's branch. nativeHold: the pause menu is open: the game is held as the Switch holds it behind HOME (hle_hold.c: graphics, audio, guest clock);
// the Java side stops sending input. nativeQuit: the graceful exit (hle_lifecycle.c dm_host_shutdown: audio and graphics stopped, then _exit(0)).
// nativeOpmode / nativeSetOpmode: the Switch operation mode, 0 handheld / 1 docked, switched live (hle_oe.c dm_set_opmode: notifications 30 / 31).
void dm_hold_menu(int on); void dm_host_shutdown(const char *why); void dm_haptics_android_foreground(int on);
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeHold(JNIEnv *env, jclass cls, jboolean on) {
    (void)env; (void)cls;
    dm_haptics_android_foreground(on ? 0 : 1);   // no rumble left running under the menu
    dm_hold_menu(on ? 1 : 0);
}
// [DOOM] Java breadcrumbs into boot.log (DmActivity.crumb: trim memory, driver decision, first frame, crash report written) and the log
// file's path for CrashLog.java (the same rule as here; "" before nativeStart).
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeLog(JNIEnv *env, jclass cls, jstring text) {
    (void)cls; if (!text) return;
    const char *t = (*env)->GetStringUTFChars(env, text, NULL); if (!t) return;
    LOG("[java] %s", t); (*env)->ReleaseStringUTFChars(env, text, t);
}
JNIEXPORT jstring JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeLogPath(JNIEnv *env, jclass cls) { (void)cls; return (*env)->NewStringUTF(env, g_log_path); }
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeQuit(JNIEnv *env, jclass cls) { (void)env; (void)cls; LOG("[java] the app ends (pause menu)"); dm_host_shutdown("pause menu: quit"); }
JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeOpmode(JNIEnv *env, jclass cls) { (void)env; (void)cls; return dm_opmode(); }
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativeSetOpmode(JNIEnv *env, jclass cls, jint m) { (void)env; (void)cls; dm_set_opmode(m ? 1 : 0); }
