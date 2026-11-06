// [DOOM] CPU topology and thread placement by core capacity: see cpu_topo.h. The plan is a pure function of what sysfs reported
// (dm_cpu_plan_compute, host test android/tests/t_cpu_topo.c); dm_cpu_plan() reads sysfs once and logs the result.
#include "cpu_topo.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- core types (MIDR implementer:part) ----
// weight: rough per-MHz throughput class, used only when cpu_capacity is missing (freq x weight) or when only MIDR is known;
// 40 marks the in-order little cores.
static const struct { uint16_t impl, part; uint8_t w; const char *name; } k_parts[] = {
    {0x41, 0xd03, 40, "A53"}, {0x41, 0xd04, 40, "A35"}, {0x41, 0xd05, 40, "A55"}, {0x41, 0xd46, 40, "A510"}, {0x41, 0xd80, 40, "A520"},
    {0x41, 0xd07, 80, "A57"}, {0x41, 0xd08, 80, "A72"}, {0x41, 0xd09, 80, "A73"}, {0x41, 0xd0a, 100, "A75"}, {0x41, 0xd0b, 100, "A76"},
    {0x41, 0xd0d, 100, "A77"}, {0x41, 0xd41, 100, "A78"}, {0x41, 0xd47, 100, "A710"}, {0x41, 0xd4d, 100, "A715"}, {0x41, 0xd81, 100, "A720"},
    {0x41, 0xd87, 100, "A725"}, {0x41, 0xd44, 125, "X1"}, {0x41, 0xd48, 125, "X2"}, {0x41, 0xd4e, 125, "X3"}, {0x41, 0xd82, 125, "X4"},
    {0x41, 0xd85, 125, "X925"}, {0x51, 0x800, 80, "Kryo-gold/A73"}, {0x51, 0x801, 40, "Kryo-silver/A53"}, {0x51, 0x802, 100, "Kryo-gold/A75"},
    {0x51, 0x803, 40, "Kryo-silver/A55"}, {0x51, 0x804, 100, "Kryo-gold/A76"}, {0x51, 0x805, 40, "Kryo-silver/A55"}, {0x51, 0x001, 125, "Oryon"}};
static int part_index(uint32_t midr) {
    for (size_t i = 0; i < sizeof k_parts / sizeof *k_parts; i++) if ((uint32_t)k_parts[i].impl << 12 == (midr & ~0xfffu) && k_parts[i].part == (midr & 0xfff)) return (int)i;
    return -1;
}
const char *dm_cpu_part_name(uint32_t midr) { int i = part_index(midr); return i < 0 ? (midr ? "?" : "-") : k_parts[i].name; }
static unsigned part_weight(uint32_t midr) { int i = part_index(midr); return i < 0 ? 100 : k_parts[i].w; }
static int part_little(uint32_t midr) { return part_weight(midr) == 40 && part_index(midr) >= 0; }
static int popc(uint32_t m) { return __builtin_popcount(m); }

// The pre-11.06 rule (vulkan_present.cpp fastest_cores, xbg_game.cpp vkrec::apply_affinity, vulkan_present.cpp pt::loop):
// translate = the CPUs with the highest cpuinfo_max_freq (0 when they all share it); record and present = every CPU but those.
static void index_rule(const dm_cpu_topo_in_t *in, uint32_t all, uint32_t *t, uint32_t *r, uint32_t *p) {
    uint32_t best = 0, mask = 0, present = 0;
    for (int c = 0; c < in->ncpu; c++) { uint32_t v = in->max_khz[c]; if (!v) continue; present |= 1u << c; if (v > best) { best = v; mask = 0; } if (v == best) mask |= 1u << c; }
    *t = mask == present ? 0 : mask;
    *r = all & ~*t; if (!*r) *r = all;
    *p = *r;
}

void dm_cpu_plan_compute(const dm_cpu_topo_in_t *in, int legacy, dm_cpu_plan_t *o) {
    memset(o, 0, sizeof *o);
    int n = in->ncpu < 1 ? 1 : in->ncpu > DM_CPU_MAX ? DM_CPU_MAX : in->ncpu;
    o->all = n == 32 ? 0xffffffffu : (1u << n) - 1;
    o->legacy = legacy;
    index_rule(in, o->all, &o->idx_translate, &o->idx_record, &o->idx_present);
    // ---- score per CPU: cpu_capacity, else max freq x core weight, else core weight; a source counts only if every CPU has it
    // and not all values are equal (a kernel without capacities reports 1024 everywhere)
    uint32_t score[DM_CPU_MAX] = {0};
    int have_cap = 1, have_f = 1, have_m = 1, cap_differs = 0, f_differs = 0, m_differs = 0;
    for (int c = 0; c < n; c++) {
        have_cap &= in->capacity[c] > 0; have_f &= in->max_khz[c] > 0; have_m &= part_index(in->midr[c]) >= 0;
        if (c) { cap_differs |= in->capacity[c] != in->capacity[0]; f_differs |= in->max_khz[c] != in->max_khz[0];
                 m_differs |= part_weight(in->midr[c]) != part_weight(in->midr[0]); }
    }
    o->source = "none";
    if (have_cap && cap_differs) { o->source = "capacity"; for (int c = 0; c < n; c++) score[c] = in->capacity[c]; }
    else if (have_f && f_differs) { o->source = "freq"; for (int c = 0; c < n; c++) score[c] = in->max_khz[c] / 1000 * part_weight(in->midr[c]) / 10; }
    else if (have_m && m_differs) { o->source = "midr"; for (int c = 0; c < n; c++) score[c] = part_weight(in->midr[c]); }
    // ---- clusters: equal scores, fastest first
    uint32_t left = strcmp(o->source, "none") ? o->all : 0;
    while (left && o->nclusters < DM_CPU_MAX) {
        uint32_t best = 0, m = 0;
        for (int c = 0; c < n; c++) if (left >> c & 1) { if (score[c] > best) { best = score[c]; m = 0; } if (score[c] == best) m |= 1u << c; }
        o->cluster[o->nclusters] = m; o->cluster_score[o->nclusters] = best; o->nclusters++; left &= ~m;
    }
    if (!o->nclusters) { o->nclusters = 1; o->cluster[0] = o->all; }
    // ---- the little cluster: the slowest one, at most 60% of the fastest, or in-order cores (A53 / A55 / A510 / A520) when the
    // fastest cluster is not (frequency alone hides it: SD680 A53 1.9 GHz vs A73 2.4 GHz is 79%)
    if (o->nclusters >= 2) {
        int lo = o->nclusters - 1; uint32_t lm = o->cluster[lo], tm = o->cluster[0]; int lo_inorder = 1, top_inorder = 0;
        for (int c = 0; c < n; c++) { if (lm >> c & 1) lo_inorder &= part_little(in->midr[c]); if (tm >> c & 1) top_inorder |= part_little(in->midr[c]); }
        if ((uint64_t)o->cluster_score[lo] * 10 <= (uint64_t)o->cluster_score[0] * 6 || (lo_inorder && !top_inorder)) o->little = lm;
    }
    if (legacy) { o->translate = o->idx_translate; o->record = o->idx_record; o->present = o->idx_present; o->guest = 0; return; }
    // ---- roles
    if (o->nclusters < 2) { o->translate = 0; o->record = o->present = o->all; o->guest = 0; return; }   // one core class: no pin (as before)
    const uint32_t strong = o->all & ~o->little, top = o->cluster[0], rest = strong & ~top;
    if (popc(rest) >= 2 || popc(strong) <= 1) {
        // another strong cluster outside the fastest one (flagships, 1+3+4) or a single strong core: the old shape
        o->translate = top; o->record = o->present = o->all & ~top; o->guest = 0;
        if (!o->record) o->record = o->present = o->all;
        return;
    }
    // one strong cluster (4+4, 2+6, 6+2) or prime + one big (1+1+6): the fastest core for the translator alone, the next strong core
    // for the record thread (shared with the guest threads when there are only two), the guests everywhere but the translator's
    // core, the present thread on the little cluster
    int ord[DM_CPU_MAX], no = 0;
    for (int c = n - 1; c >= 0; c--) if (strong >> c & 1) ord[no++] = c;   // higher index first among equals (the prime core is the last one)
    for (int i = 1; i < no; i++) for (int j = i; j > 0 && score[ord[j]] > score[ord[j - 1]]; j--) { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
    o->translate = 1u << ord[0];
    o->record = 1u << ord[1];
    o->guest = o->all & ~o->translate;
    o->present = o->little ? o->little : o->all & ~o->translate & ~o->record;
    if (!o->present) o->present = o->all & ~o->translate;
}

static size_t cpus_text(uint32_t m, char *b, size_t n) {   // "0-3,7"
    size_t o = 0; if (n) b[0] = 0;
    for (int c = 0; c < 32 && o + 8 < n; c++) {
        if (!(m >> c & 1)) continue;
        int e = c; while (e + 1 < 32 && (m >> (e + 1) & 1)) e++;
        o += (size_t)snprintf(b + o, n - o, e > c ? "%s%d-%d" : "%s%d", o ? "," : "", c, e); c = e;
    }
    return o;
}
size_t dm_cpu_topo_describe(const dm_cpu_topo_in_t *in, const dm_cpu_plan_t *p, char *buf, size_t n) {
    size_t o = (size_t)snprintf(buf, n, "%d cpus, ranked by %s:", in->ncpu, p->source);
    for (int k = 0; k < p->nclusters && o + 64 < n; k++) {
        char cs[48]; cpus_text(p->cluster[k], cs, sizeof cs); int c0 = __builtin_ctz(p->cluster[k] ? p->cluster[k] : 1);
        o += (size_t)snprintf(buf + o, n - o, "%s cpu%s %ux %s cap=%u mhz=%u score=%u%s", k ? " |" : "", cs, (unsigned)popc(p->cluster[k]), dm_cpu_part_name(in->midr[c0]),
                              in->capacity[c0], in->max_khz[c0] / 1000, p->cluster_score[k], p->cluster[k] == p->little ? " (little)" : "");
    }
    return o;
}
size_t dm_cpu_plan_describe(const dm_cpu_plan_t *p, char *buf, size_t n) {
    char g[16]; if (p->guest) snprintf(g, sizeof g, "%x", p->guest); else snprintf(g, sizeof g, "free");
    return (size_t)snprintf(buf, n, "rule=%s translate=%x record=%x present=%x guest=%s (index rule: translate=%x record=%x present=%x guest=free)",
                            p->legacy ? "index" : "capacity", p->translate, p->record, p->present, g, p->idx_translate, p->idx_record, p->idx_present);
}

#if defined(__linux__)
static uint32_t read_u32(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    unsigned long v = 0; if (fscanf(f, "%lu", &v) != 1) v = 0; fclose(f); return (uint32_t)v;
}
static void read_topo(dm_cpu_topo_in_t *in) {
    memset(in, 0, sizeof *in);
    long n = sysconf(_SC_NPROCESSORS_CONF); if (n < 1) n = 8; if (n > DM_CPU_MAX) n = DM_CPU_MAX;
    in->ncpu = (int)n;
    char p[128];
    for (int c = 0; c < in->ncpu; c++) {
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c); in->capacity[c] = read_u32(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); in->max_khz[c] = read_u32(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", c);
        FILE *f = fopen(p, "r");
        if (f) { unsigned long long v = 0; if (fscanf(f, "%llx", &v) == 1 && v) in->midr[c] = (uint32_t)((v >> 24 & 0xff) << 12 | (v >> 4 & 0xfff)); fclose(f); }
    }
    // MIDR fallback: /proc/cpuinfo blocks ("processor : N", then "CPU implementer" before "CPU part")
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char l[256]; int cpu = -1; unsigned impl = 0, v;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, "processor : %u", &v) == 1) { cpu = (int)v; impl = 0; }
            else if (sscanf(l, "CPU implementer : %x", &v) == 1) impl = v;
            else if (sscanf(l, "CPU part : %x", &v) == 1 && cpu >= 0 && cpu < in->ncpu && !in->midr[cpu]) in->midr[cpu] = (impl & 0xff) << 12 | (v & 0xfff);
        }
        fclose(f);
    }
}
static dm_cpu_plan_t g_plan; static dm_cpu_topo_in_t g_in;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void plan_init(void) {
    read_topo(&g_in);
    const char *e = getenv("DM_CPU_TOPO"); const int legacy = e && (!strcmp(e, "index") || !strcmp(e, "legacy") || !strcmp(e, "old"));
    dm_cpu_plan_compute(&g_in, legacy, &g_plan);
    // [DOOM] DM_GFX_PRESENT_CPUS / DM_GUEST_CPUS=<hex>: own masks for dm-present / the guest threads (DM_GUEST_CPUS=0: no guest pin)
    if ((e = getenv("DM_GFX_PRESENT_CPUS")) && *e) { uint32_t v = (uint32_t)strtoul(e, NULL, 16) & g_plan.all; if (v) g_plan.present = v; }
    if ((e = getenv("DM_GUEST_CPUS")) && *e) { uint32_t v = (uint32_t)strtoul(e, NULL, 16) & g_plan.all; g_plan.guest = v == g_plan.all ? 0 : v; }
    char b[768];
    dm_cpu_topo_describe(&g_in, &g_plan, b, sizeof b); fprintf(stderr, "[dm] [cpu] topology: %s\n", b);
    dm_cpu_plan_describe(&g_plan, b, sizeof b);
    fprintf(stderr, "[dm] [cpu] plan: %s; switches DM_CPU_TOPO=index, DM_GFX_PIN_CPU, DM_GFX_RECORD_CPUS, DM_GFX_PRESENT_CPUS, DM_GUEST_CPUS\n", b);
}
const dm_cpu_plan_t *dm_cpu_plan(void) { pthread_once(&g_once, plan_init); return &g_plan; }
#endif
