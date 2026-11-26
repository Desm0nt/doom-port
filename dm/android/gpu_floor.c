// [gpu-floor] PNH-C01 (port-common/modules/gpu-floor): a GPU clock floor without root, only while the frames run late.
// The KGSL parts (the queue contexts' power constraint, the "pulse" of a kernel context of our own) adapted from peanuts-native 85eeb7a
// (igawa6), GPL-3.0-or-later: dm/peanuts/peanuts_gpufloor.c (peanuts_gpu_floor_beat) and dm/gfx/vk_android/vka_dev.c (kg_set, kg_scan,
// vka_kgsl_before / vka_kgsl_after, kg_apply, floor_set_'s queue hint). Their reasoning: docs/reference/frame-rate.md "A GPU clock floor
// that follows the present period holds a dynamic interval at its slowest" and "The KGSL clock floor reaches the clock only through flagged
// commands on host-DCVS Adreno". This file is therefore GPL-3.0-or-later.
//
// Why: the GPU's governor follows its busy time. When the CPU and the GPU take turns (the translator records, then the GPU draws), the GPU
// idles part of each frame, the governor lowers its clock, the same frame then takes longer on the GPU, and the game sits in a slow state
// it does not leave by itself (peanuts: Adreno 740 at 401-550 of 680 MHz with Turnip; Adreno 840 282-646 MHz -> 1,200 MHz with the floor,
// GPU 26-40 -> 10-12 ms a frame). One lever per GPU, picked at the first present and logged once:
//   hint      Qualcomm's own driver that lists VK_QCOM_queue_perf_hint with its feature: vkQueueSetPerfHintQCOM(highest | scaled | default).
//   kgsl beat an Adreno on KGSL whose kernel takes only the "highest" form of the constraint (host DCVS: Adreno 6xx / 7xx, 8 Gen 2 / 8 Gen 3,
//             Snapdragon 7): the kernel applies a stored constraint only when a command flagged KGSL_CONTEXT_PWR_CONSTRAINT is submitted, and
//             Turnip never flags its commands, so a kernel context of our own with that flag gets one CP_NOP at most every
//             DM_GPU_FLOOR_BEAT_MS (8) while the floor is on, at a present. Each such submit puts the clock at the highest level device-wide
//             and the kernel holds it until the command retired and its idle interval (QCOM_KGSL_IDLE_TIMEOUT, 80 ms) passed; no frames
//             (pause, background) = no pulses = the floor lapses by itself. The driver's queue contexts also get the constraint (peanuts
//             does both: a driver that flags its own commands then needs no pulse).
//   kgsl ctx  an Adreno whose GMU decides the clock (gen8: Adreno 830 / 840): the "level" form is taken (2-100 %), the kernel hands it to the
//             GMU as the rule of the driver's queue contexts (the contexts this process's /dev/kgsl-3d0 descriptors own after vkCreateDevice
//             and did not before). Sent again every 30 presents while on (the rule is lost when the GMU registers the context again).
//   none      any other GPU (Mali, PowerVR, Xclipse, Maleoon, ...): one line "no GPU clock lever on <vendor>", nothing else is done.
// The same kernel paths serve Qualcomm's driver when it lacks the hint (no driver package is required: plain Turnip, any Turnip build, or
// the system driver). No root, no sysfs writes, no system setting is changed and nothing outlives the process (the descriptor is ours).
//
// DM_GPU_FLOOR=auto (default): the floor only while frames run late against the pacing target (gpu_floor_auto.h: 3 late frames in a row or
// 6 of the last 60 turn it on; 10 s without a late frame, with GPU headroom when the GPU time is known, turn it off; a release that has to
// be taken back within 5 s doubles the next wait, up to 120 s). =1: on whenever frames come. =0: off, nothing is opened.
// Other switches: DM_GPU_FLOOR_PATH=auto|hint|kgsl (force a lever for A/B; kgsl also on Qualcomm's driver with the hint),
// DM_GPU_FLOOR_PCT=100 (hint / GMU: % of the highest clock; host-DCVS kernels take only the highest), DM_GPU_FLOOR_BEAT_MS=8,
// DM_GPU_FLOOR_TARGET_MS (default: the pacer's period, dm_present_period; 16.67 when it has none), DM_GPU_FLOOR_LATE_PCT=10,
// DM_GPU_FLOOR_ON_FRAMES=3, DM_GPU_FLOOR_OFF_S=10, DM_GPU_FLOOR_IDLE_PCT=50, DM_GPU_FLOOR_LOG_S=10 (the "[gpu-floor] 10s:" line; 0 off).
#if defined(__ANDROID__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../host/dm.h"
#include "gpu_floor.h"
#include "gpu_floor_auto.h"

#define GLOG(...) LOG("[gpu-floor] " __VA_ARGS__)

__attribute__((weak)) double dm_present_period(void);   // the pacer's period of this present, s (gfx/vulkan/nvn_vk.c)
__attribute__((weak)) double xbg_recent_gpu_ms(void);   // the engine's GPU time of a recent frame, ms (< 0 unknown)
// The presenter's queue lock where two threads share the VkQueue (present-thread, PERF-D07: vulkan_present.cpp); the queue hint is a
// queue call, so it takes it. Without the export (one thread uses the queue) the hint runs on the presenting thread as the present does.
__attribute__((weak)) void dm_present_queue_lock(void);
__attribute__((weak)) void dm_present_queue_unlock(void);

static int64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static double env_num(const char *k, double def) { const char *e = getenv(k); return e && *e ? atof(e) : def; }

// ---- settings (read once, at the first hook)
enum { M_OFF = 0, M_ON = 1, M_AUTO = 2 };
enum { F_AUTO = 0, F_HINT = 1, F_KGSL = 2 };
static int g_mode = -1, g_force, g_pct = 100, g_beat_ms = 8, g_log_ms = 10000;
static double g_target_ms;
static gf_auto_t g_auto;
static void cfg_read(void) {
    if (g_mode >= 0) return;
    const char *m = getenv("DM_GPU_FLOOR");
    g_mode = !m || !*m || !strcmp(m, "auto") ? M_AUTO : atoi(m) ? M_ON : M_OFF;
    const char *p = getenv("DM_GPU_FLOOR_PATH");
    g_force = p && !strcmp(p, "hint") ? F_HINT : p && !strcmp(p, "kgsl") ? F_KGSL : F_AUTO;
    g_pct = (int)env_num("DM_GPU_FLOOR_PCT", 100); if (g_pct < 2) g_pct = 2; if (g_pct > 100) g_pct = 100;
    g_beat_ms = (int)env_num("DM_GPU_FLOOR_BEAT_MS", 8); if (g_beat_ms < 1) g_beat_ms = 1;
    g_log_ms = (int)(env_num("DM_GPU_FLOOR_LOG_S", 10) * 1000);
    g_target_ms = env_num("DM_GPU_FLOOR_TARGET_MS", 0);
    gf_auto_init(&g_auto);
    g_auto.late_pct = (int)env_num("DM_GPU_FLOOR_LATE_PCT", g_auto.late_pct);
    g_auto.on_run = (int)env_num("DM_GPU_FLOOR_ON_FRAMES", g_auto.on_run); if (g_auto.on_run < 1) g_auto.on_run = 1;
    g_auto.off_ns = (int64_t)(env_num("DM_GPU_FLOOR_OFF_S", 10) * 1e9); g_auto.cur_off_ns = g_auto.off_ns;
    if (g_auto.off_max_ns < g_auto.off_ns) g_auto.off_max_ns = g_auto.off_ns;
    g_auto.idle_pct = (int)env_num("DM_GPU_FLOOR_IDLE_PCT", g_auto.idle_pct);
}
static const char *mode_name(void) { return g_mode == M_AUTO ? "auto" : g_mode == M_ON ? "1 (always)" : "0 (off)"; }

// ---- VK_QCOM_queue_perf_hint (not in the NDK's headers yet: VkPhysicalDeviceQueuePerfHintFeaturesQCOM, VkPerfHintInfoQCOM)
typedef struct { VkStructureType sType; void *pNext; VkBool32 queuePerfHint; } gf_ph_features_t;
typedef struct { VkStructureType sType; const void *pNext; uint32_t type, scale; } gf_ph_info_t;
enum { GF_PH_FEATURES = 1000302001, GF_PH_INFO = 1000302000, GF_PH_DEFAULT = 0, GF_PH_MAX = 2, GF_PH_SCALED = 3 };
typedef VkResult (*gf_set_hint_fn)(VkQueue, const gf_ph_info_t *);
static gf_ph_features_t g_ph_feat;
static int g_ph_want;                    // the extension went into vkCreateDevice
static char g_ph_note[96];               // why not (for the first-present line)
static gf_set_hint_fn g_ph_set;

int dm_gpu_floor_want_hint(VkPhysicalDevice pd, const VkExtensionProperties *ext, uint32_t n) {
    cfg_read();
    g_ph_want = 0;
    if (g_mode == M_OFF) return 0;
    int has = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ext[i].extensionName, "VK_QCOM_queue_perf_hint")) has = 1;
    if (!has) { snprintf(g_ph_note, sizeof g_ph_note, "no VK_QCOM_queue_perf_hint"); return 0; }
    gf_ph_features_t probe = {(VkStructureType)GF_PH_FEATURES, NULL, VK_FALSE};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &probe, {0}};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!probe.queuePerfHint) { snprintf(g_ph_note, sizeof g_ph_note, "VK_QCOM_queue_perf_hint without its feature"); return 0; }
    if (g_force == F_KGSL) { snprintf(g_ph_note, sizeof g_ph_note, "VK_QCOM_queue_perf_hint not used (DM_GPU_FLOOR_PATH=kgsl)"); return 0; }
    g_ph_want = 1;
    return 1;
}
void *dm_gpu_floor_chain(void *next) {
    g_ph_feat.sType = (VkStructureType)GF_PH_FEATURES; g_ph_feat.pNext = next; g_ph_feat.queuePerfHint = VK_TRUE;
    return &g_ph_feat;
}

// ---- KGSL (msm_kgsl.h uapi, the parts used here; sizes checked below for LP64)
struct gf_kgsl_drawctxt_create { unsigned int flags, drawctxt_id; };
struct gf_kgsl_gpumem_alloc_id { unsigned int id, flags; size_t size, mmapsize; unsigned long gpuaddr, pad[2]; };
struct gf_kgsl_command_object { uint64_t offset, gpuaddr, size; unsigned int flags, id; };
struct gf_kgsl_gpu_command { uint64_t flags, cmdlist; unsigned int cmdsize, numcmds; uint64_t objlist; unsigned int objsize, numobjs; uint64_t synclist;
                             unsigned int syncsize, numsyncs, context_id, timestamp; };
struct gf_kgsl_device_constraint { unsigned int type, context_id; void *data; size_t size; };
struct gf_kgsl_setprop { unsigned int type; void *value; size_t sizebytes; };
#if defined(__LP64__)
_Static_assert(sizeof(struct gf_kgsl_gpumem_alloc_id) == 48 && sizeof(struct gf_kgsl_command_object) == 32 && sizeof(struct gf_kgsl_gpu_command) == 64 &&
               sizeof(struct gf_kgsl_device_constraint) == 24 && sizeof(struct gf_kgsl_setprop) == 24, "msm_kgsl.h layout");
#endif
#define GF_KGSL_DRAWCTXT_CREATE _IOWR(0x09, 0x13, struct gf_kgsl_drawctxt_create)
#define GF_KGSL_GPUMEM_ALLOC_ID _IOWR(0x09, 0x34, struct gf_kgsl_gpumem_alloc_id)
#define GF_KGSL_GPU_COMMAND     _IOWR(0x09, 0x4A, struct gf_kgsl_gpu_command)
#define GF_KGSL_SETPROPERTY     _IOW(0x09, 0x32, struct gf_kgsl_setprop)
enum { GF_CTX_NO_GMEM_ALLOC = 0x2, GF_CTX_PREAMBLE = 0x10, GF_PWR_CONSTRAINT = 0x800, GF_CMDLIST_IB = 0x1, GF_PROP_PWR_CONSTRAINT = 0x12,
       GF_CONSTRAINT_NONE = 0, GF_CONSTRAINT_PWRLEVEL = 1, GF_PWR_MAX = 1 };
#define GF_CP_NOP 0x70108000u   // type-7 packet, CP_NOP (0x10), no payload, with its parity bits

// ioctl with EINTR retried; EAGAIN (a full queue) retried 3 times, then reported (peanuts retries it forever: a hung GPU would spin here)
static int kg_ioc(int fd, unsigned long req, void *arg) {
    int r, again = 0;
    do r = ioctl(fd, req, arg); while (r < 0 && (errno == EINTR || (errno == EAGAIN && ++again <= 3)));
    return r < 0 ? -errno : 0;
}
static int kg_set(int fd, unsigned id, int type, unsigned level) {
    unsigned lv = level; struct gf_kgsl_device_constraint c = {(unsigned)type, id, &lv, sizeof lv}; struct gf_kgsl_setprop g = {GF_PROP_PWR_CONSTRAINT, &c, sizeof c};
    return kg_ioc(fd, GF_KGSL_SETPROPERTY, &g);
}

// The driver's queue contexts: the ids this process's /dev/kgsl-3d* descriptors own after vkCreateDevice and did not own before (the kernel
// lets a descriptor set the constraint only on contexts it owns, kgsl_context_get_owner; the test is the same call with CONSTRAINT_NONE,
// which changes nothing on a context without a constraint). Turnip: one per VkQueue (tu_knl_kgsl.cc kgsl_submitqueue_new).
enum { KG_FDS = 8, KG_IDS = 1024, KG_CTX = 8 };
typedef struct { int n, fd[KG_FDS]; uint8_t own[KG_FDS][KG_IDS / 8]; } kg_scan_t;
static kg_scan_t g_kg_was, g_kg_now;
static int g_kg_before;                  // the "before" scan ran
static struct { int fd; unsigned id; } g_kg[KG_CTX];
static int g_kg_n;
static void kg_scan(kg_scan_t *s) {
    s->n = 0;
    DIR *d = opendir("/proc/self/fd"); if (!d) return;
    struct dirent *de; char path[64], to[64];
    while ((de = readdir(d)) && s->n < KG_FDS) {
        int fd = atoi(de->d_name); if (fd <= 2) continue;
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd); ssize_t n = readlink(path, to, sizeof to - 1); if (n <= 0) continue; to[n] = 0;
        if (!strncmp(to, "/dev/kgsl-3d", 12)) s->fd[s->n++] = fd;
    }
    closedir(d);
    for (int i = 0; i < s->n; i++) {
        memset(s->own[i], 0, sizeof s->own[i]);
        for (unsigned id = 1; id < KG_IDS; id++) if (!kg_set(s->fd[i], id, GF_CONSTRAINT_NONE, 0)) s->own[i][id / 8] |= (uint8_t)(1u << (id % 8));
    }
}
void dm_gpu_floor_before_device(void) {
    cfg_read();
    g_kg_before = 0;
    if (g_mode == M_OFF || g_ph_want) return;   // (with the queue hint the driver's contexts are left alone)
    kg_scan(&g_kg_was);
    g_kg_before = 1;
}
static void kg_find_queue_contexts(void) {
    g_kg_n = 0;
    if (!g_kg_before) return;
    kg_scan(&g_kg_now);
    for (int i = 0; i < g_kg_now.n; i++) {
        int w = -1; for (int j = 0; j < g_kg_was.n; j++) if (g_kg_was.fd[j] == g_kg_now.fd[i]) w = j;
        for (unsigned id = 1; id < KG_IDS && g_kg_n < KG_CTX; id++)
            if ((g_kg_now.own[i][id / 8] >> (id % 8) & 1) && !(w >= 0 && (g_kg_was.own[w][id / 8] >> (id % 8) & 1))) { g_kg[g_kg_n].fd = g_kg_now.fd[i]; g_kg[g_kg_n].id = id; g_kg_n++; }
    }
}

// The pulse's own kernel context (peanuts_gpufloor.c setup): created with KGSL_CONTEXT_PWR_CONSTRAINT, "highest" stored on it, one 4-dword
// CP_NOP buffer. host = 1 when the kernel refuses the "level" form (-EINVAL: host DCVS, the pulse is needed), 0 when it takes it (GMU DCVS).
static int g_bfd = -1; static unsigned g_bctx, g_bbuf_id; static uint64_t g_bbuf_va; static int g_host = -1;
static int beat_setup(char *why, size_t n) {
    g_bfd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (g_bfd < 0) { snprintf(why, n, "/dev/kgsl-3d0 not opened (%s)", strerror(errno)); return 0; }
    struct gf_kgsl_drawctxt_create c = {GF_CTX_NO_GMEM_ALLOC | GF_CTX_PREAMBLE | GF_PWR_CONSTRAINT, 0};
    int r = kg_ioc(g_bfd, GF_KGSL_DRAWCTXT_CREATE, &c);
    if (r) { snprintf(why, n, "no kernel context of our own (%d)", r); goto fail; }
    g_bctx = c.drawctxt_id;
    r = kg_set(g_bfd, g_bctx, GF_CONSTRAINT_PWRLEVEL, 50);   // the level form the kernel takes: 2-100 % with GMU DCVS, else -EINVAL and only 1
    g_host = r == -EINVAL;
    if (!g_host) { kg_set(g_bfd, g_bctx, GF_CONSTRAINT_NONE, 0); return 1; }   // (GMU: the rule is per context, our own context's is no use)
    struct gf_kgsl_gpumem_alloc_id a; memset(&a, 0, sizeof a); a.size = 4096;
    r = kg_ioc(g_bfd, GF_KGSL_GPUMEM_ALLOC_ID, &a);
    if (r || !a.gpuaddr) { snprintf(why, n, "no command buffer (%d)", r); goto fail; }
    g_bbuf_id = a.id; g_bbuf_va = a.gpuaddr;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, g_bfd, (off_t)a.id << 12);
    if (m == MAP_FAILED) { snprintf(why, n, "command buffer not mapped (%s)", strerror(errno)); goto fail; }
    for (int i = 0; i < 4; i++) ((volatile uint32_t *)m)[i] = GF_CP_NOP;
    munmap(m, 4096);
    r = kg_set(g_bfd, g_bctx, GF_CONSTRAINT_PWRLEVEL, GF_PWR_MAX);
    if (r) { snprintf(why, n, "the constraint was refused on our context (%d)", r); goto fail; }
    return 1;
fail:
    close(g_bfd); g_bfd = -1;   // (closing the descriptor destroys its context and frees its memory)
    return 0;
}
static void beat_close(void) { if (g_bfd >= 0) close(g_bfd); g_bfd = -1; }

// ---- the GPU clock, for the status line only (Adreno nodes; closed to apps on many phones: then "not readable", once)
static const char *const k_clk[] = {"/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/class/kgsl/kgsl-3d0/clock_mhz",
                                    "/sys/kernel/gpu/gpu_clock"};
static const char *g_clk;
static int clk_mhz(const char *path, int *err) {
    char b[64]; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) { if (err) *err = errno; return -1; }
    ssize_t r = read(fd, b, sizeof b - 1); if (err) *err = r < 0 ? errno : 0; close(fd);
    if (r <= 0) return -1;
    b[r] = 0; long long v = strtoll(b, NULL, 10);
    return v <= 0 ? -1 : v >= 10000000LL ? (int)(v / 1000000) : v >= 10000 ? (int)(v / 1000) : (int)v;   // Hz / kHz / MHz by size
}
static void clk_probe(char *out, size_t n) {
    size_t o = 0; out[0] = 0;
    for (unsigned i = 0; i < sizeof k_clk / sizeof *k_clk; i++) {
        int e = 0, mhz = clk_mhz(k_clk[i], &e);
        if (mhz > 0) { g_clk = k_clk[i]; snprintf(out, n, "%s (now %d MHz)", g_clk, mhz); return; }
        if (o < n) o += (size_t)snprintf(out + o, n - o, "%s%s: %s", o ? "; " : "not readable without root (", k_clk[i], e ? strerror(e) : "no number");
    }
    if (o < n) snprintf(out + o, n - o, ")");
}

// ---- the lever
enum { P_NONE, P_HINT, P_BEAT, P_CTX };
static int g_path = -1;                  // -1: not decided yet (the first present decides)
static VkQueue g_queue;
static char g_why[320]; static int g_why_applied, g_why_pending;
static uint64_t g_sets, g_set_bad, g_beats, g_beat_bad; static int g_set_last_r, g_beat_r;
static int64_t g_beat_last;

static const char *vendor_name(uint32_t v) {
    switch (v) {
    case 0x13B5: return "Mali (ARM)"; case 0x1010: return "PowerVR (Imagination)"; case 0x144D: return "Xclipse (Samsung)";
    case 0x19E5: return "Maleoon (Huawei)"; case 0x1002: return "AMD"; case 0x10DE: return "NVIDIA"; case 0x8086: return "Intel";
    case 0x10005: return "Mesa software"; case 0x1AE0: return "SwiftShader (Google)"; default: return NULL;
    }
}
static const char *path_name(void) {
    return g_path == P_HINT ? "VK_QCOM_queue_perf_hint" : g_path == P_BEAT ? "KGSL pulse (host DCVS)" : g_path == P_CTX ? "KGSL queue contexts (GMU DCVS)" : "none";
}
static void lever_set(int on) {
    int pct = g_pct >= 100 ? 100 : g_pct;
    if (g_path == P_HINT) {
        gf_ph_info_t h = {(VkStructureType)GF_PH_INFO, NULL, on ? (pct >= 100 ? (uint32_t)GF_PH_MAX : (uint32_t)GF_PH_SCALED) : (uint32_t)GF_PH_DEFAULT,
                          on && pct < 100 ? (uint32_t)pct : 0u};
        const int lk = dm_present_queue_lock && dm_present_queue_unlock;
        if (lk) dm_present_queue_lock();
        VkResult r = g_ph_set(g_queue, &h);
        if (lk) dm_present_queue_unlock();
        g_sets++; g_set_last_r = (int)r;
        if (r != VK_SUCCESS && g_set_bad++ < 3) GLOG("vkQueueSetPerfHintQCOM(%s) -> %d", on ? "high" : "default", (int)r);
        return;
    }
    for (int k = 0; k < g_kg_n; k++) {   // P_BEAT and P_CTX: the driver's queue contexts (kg_apply)
        int r = on ? kg_set(g_kg[k].fd, g_kg[k].id, GF_CONSTRAINT_PWRLEVEL, pct >= 100 || g_host ? GF_PWR_MAX : (unsigned)pct)
                   : kg_set(g_kg[k].fd, g_kg[k].id, GF_CONSTRAINT_NONE, 0);
        g_sets++; g_set_last_r = r;
        if (r && g_set_bad++ < 3) GLOG("KGSL power constraint %s on context %u: %d", on ? "on" : "off", g_kg[k].id, r);
    }
}
static void beat(int64_t now) {   // peanuts_gpu_floor_beat: one flagged CP_NOP at most every DM_GPU_FLOOR_BEAT_MS
    if (g_bfd < 0 || (g_beat_last && now - g_beat_last < (int64_t)g_beat_ms * 1000000LL)) return;
    struct gf_kgsl_command_object o = {0, g_bbuf_va, 4, GF_CMDLIST_IB, g_bbuf_id};
    struct gf_kgsl_gpu_command gc; memset(&gc, 0, sizeof gc);
    gc.flags = GF_PWR_CONSTRAINT; gc.cmdlist = (uint64_t)(uintptr_t)&o; gc.cmdsize = sizeof o; gc.numcmds = 1; gc.context_id = g_bctx;
    int r = kg_ioc(g_bfd, GF_KGSL_GPU_COMMAND, &gc);
    g_beat_last = now; g_beat_r = r;
    if (r) {
        if (++g_beat_bad <= 4) GLOG("pulse submit refused (%d)%s", r, g_beat_bad >= 4 ? ": pulse off for this run" : "");
        if (g_beat_bad >= 4) beat_close();
    } else if (++g_beats == 1) GLOG("first pulse submitted (kernel context %u, timestamp %u)", g_bctx, gc.timestamp);
}

static void decide(VkPhysicalDevice pd, VkDevice dev) {
    VkPhysicalDeviceDriverProperties dp; memset(&dp, 0, sizeof dp); dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2); p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &dp;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const uint32_t vendor = p2.properties.vendorID;
    char drv[96]; snprintf(drv, sizeof drv, "driverID %d %s", (int)dp.driverID, dp.driverName[0] ? dp.driverName : "?");
    char clk[400] = "";
    g_path = P_NONE; g_why_applied = 0;
    if (g_mode == M_OFF) {
        snprintf(g_why, sizeof g_why, "off (DM_GPU_FLOOR=0)");
    } else if (vendor != 0x5143) {
        const char *vn = vendor_name(vendor); char vb[24]; if (!vn) { snprintf(vb, sizeof vb, "vendor %#x", vendor); vn = vb; }
        snprintf(g_why, sizeof g_why, "no GPU clock lever on %s (%s, %s): nothing done", vn, p2.properties.deviceName, drv);
    } else if (g_ph_want && g_force != F_KGSL && (g_ph_set = (gf_set_hint_fn)vkGetDeviceProcAddr(dev, "vkQueueSetPerfHintQCOM"))) {
        g_path = P_HINT;
    } else {
        kg_find_queue_contexts();   // (before our own descriptor exists)
        char bw[160] = ""; int ok = beat_setup(bw, sizeof bw);
        if (ok && g_host) g_path = P_BEAT;
        else if (ok && g_kg_n) g_path = P_CTX;
        else if (!ok && g_kg_n) {   // no context of our own: the form from the driver's context, the constraint alone (no pulse)
            int r = kg_set(g_kg[0].fd, g_kg[0].id, GF_CONSTRAINT_PWRLEVEL, 50); g_host = r == -EINVAL; kg_set(g_kg[0].fd, g_kg[0].id, GF_CONSTRAINT_NONE, 0);
            g_path = P_CTX;
            snprintf(g_why, sizeof g_why, "%s", bw);
        } else {
            if (ok) beat_close();
            snprintf(g_why, sizeof g_why, "no GPU clock lever on this Adreno (%s, %s): %s%s%s", p2.properties.deviceName, drv,
                     ok ? "GMU DCVS kernel and no queue context of the driver found" : bw, g_ph_note[0] ? "; " : "", g_ph_note);
        }
        if (g_path == P_CTX) beat_close();
    }
    if (g_path != P_NONE) {
        clk_probe(clk, sizeof clk);
        char ctx[96] = ""; size_t o = 0;
        for (int k = 0; k < g_kg_n && o < sizeof ctx; k++) o += (size_t)snprintf(ctx + o, sizeof ctx - o, "%s%u", k ? "," : "", g_kg[k].id);
        char lever[200];
        if (g_path == P_HINT) snprintf(lever, sizeof lever, "%s, %d %%", path_name(), g_pct);
        else if (g_path == P_BEAT) snprintf(lever, sizeof lever, "%s: own context %u, a CP_NOP at most every %d ms while on; queue contexts %s", path_name(), g_bctx, g_beat_ms, g_kg_n ? ctx : "none found");
        else snprintf(lever, sizeof lever, "%s %s, %d %%%s%s", path_name(), ctx, g_host ? 100 : g_pct, g_why[0] ? "; no pulse: " : "", g_why);
        snprintf(g_why, sizeof g_why, "%s, mode %s (%s, %s)", lever, mode_name(), p2.properties.deviceName, drv);
        g_why_applied = 1;
        GLOG("lever: %s; GPU clock %s; DM_GPU_FLOOR=auto|1|0, DM_GPU_FLOOR_PATH=auto|hint|kgsl", g_why, clk);
    } else GLOG("%s", g_why);
    g_why_pending = 1;
}
const char *dm_gpu_floor_report(int *applied) {
    if (!g_why_pending) return NULL;
    g_why_pending = 0; if (applied) *applied = g_why_applied;
    return g_why;
}

// ---- per present
static int g_on;                         // the lever's state now
static int64_t g_on_since, g_on_ns, g_line_at, g_line_on_ns;
static uint64_t g_since_send, g_l_frames, g_l_late, g_l_beats, g_l_eng, g_l_rel, g_l_relapse;
static void status_line(int64_t now, int64_t target) {
    if (g_log_ms <= 0) return;
    if (!g_line_at) { g_line_at = now; g_line_on_ns = g_on_ns; g_l_frames = g_auto.frames; g_l_late = g_auto.late; g_l_beats = g_beats; return; }
    if (now - g_line_at < (int64_t)g_log_ms * 1000000LL) return;
    int64_t span = now - g_line_at, on_ns = g_on_ns + (g_on ? now - g_on_since : 0);
    double share = span > 0 ? 100.0 * (double)(on_ns - g_line_on_ns) / (double)span : 0;
    char clk[48] = "not readable"; if (g_clk) { int mhz = clk_mhz(g_clk, NULL); if (mhz > 0) snprintf(clk, sizeof clk, "%d MHz", mhz); }
    double gms = xbg_recent_gpu_ms ? xbg_recent_gpu_ms() : -1;
    char pulses[96];
    if (g_path == P_BEAT) snprintf(pulses, sizeof pulses, "pulses %llu (refused %llu, last %d)", (unsigned long long)(g_beats - g_l_beats), (unsigned long long)g_beat_bad, g_beat_r);
    else snprintf(pulses, sizeof pulses, "lever calls %llu (refused %llu, last %d)", (unsigned long long)g_sets, (unsigned long long)g_set_bad, g_set_last_r);
    GLOG("%.0fs: mode %s, %s, floor %s, on %.0f %% of the time (on %llu, off %llu, taken back %llu, next calm %.0f s), frames %llu late %llu (target %.2f ms), "
         "%s, GPU %.1f ms, GPU clock %s",
         span / 1e9, mode_name(), path_name(), g_on ? "on" : "off", share, (unsigned long long)(g_auto.engages - g_l_eng), (unsigned long long)(g_auto.releases - g_l_rel),
         (unsigned long long)(g_auto.relapses - g_l_relapse), g_auto.cur_off_ns / 1e9, (unsigned long long)(g_auto.frames - g_l_frames),
         (unsigned long long)(g_auto.late - g_l_late), target / 1e6, pulses, gms, clk);
    g_line_at = now; g_line_on_ns = on_ns; g_l_frames = g_auto.frames; g_l_late = g_auto.late; g_l_beats = g_beats;
    g_l_eng = g_auto.engages; g_l_rel = g_auto.releases; g_l_relapse = g_auto.relapses;
}
void dm_gpu_floor_present(VkPhysicalDevice pd, VkDevice dev, VkQueue q) {
    cfg_read();
    if (g_path < 0) { g_queue = q; decide(pd, dev); }
    if (g_path == P_NONE) return;
    g_queue = q;
    const int64_t now = mono_ns();
    double per = g_target_ms > 0 ? g_target_ms / 1e3 : dm_present_period ? dm_present_period() : 0;
    const int64_t target = per > 0.002 ? (int64_t)(per * 1e9) : 16666667LL;   // no pacing period (uncapped): judged against 60 fps
    const double gms = xbg_recent_gpu_ms ? xbg_recent_gpu_ms() : -1;
    const int a = gf_auto_frame(&g_auto, now, target, gms);   // (=1 runs it too, for the late-frame counts in the line)
    const int want = g_mode == M_ON ? 1 : a;
    if (want != g_on) {
        lever_set(want); g_since_send = 0;
        if (want) g_on_since = now; else g_on_ns += now - g_on_since;
        g_on = want;
    } else if (want && g_path != P_HINT && ++g_since_send >= 30) { lever_set(1); g_since_send = 0; }   // the GMU loses a context's rule when it registers the context again
    if (g_on && g_path == P_BEAT) beat(now);
    status_line(now, target);
}
#endif
