// [DOOM] Host test for android/cpu_topo.c dm_cpu_plan_compute: synthetic topologies of the SoCs we care about, the masks each role
// gets. The flagships (8 Gen 3 = Mi 14, 8 Gen 2 = Mi 13 Pro) must give exactly the pre-11.06 masks (translate 80, record 7f,
// present 7f, guests free), whatever source ranks the cores (cpu_capacity, max freq, MIDR).
// Capacities: mainline sm8650 / sm8550 capacity-dmips-mhz (A520 / A510 1024, A720 / A715 / A710 1792, X4 / X3 1894) scaled by
// cpuinfo_max_freq and normalised to 1024 as the kernel does; the "flat" variants use one dmips value for every core (capacity
// then follows the frequency only), as some vendor trees do.
// Build and run (Linux / WSL): cc -O1 -Wall -o t_cpu_topo t_cpu_topo.c ../cpu_topo.c -lpthread && ./t_cpu_topo
#include "../cpu_topo.h"
#include <stdio.h>
#include <string.h>

enum { A53Q = 0x51801, A73 = 0x41d09, A55 = 0x41d05, A76 = 0x41d0b, A78 = 0x41d41, A510 = 0x41d46, A710 = 0x41d47, A715 = 0x41d4d,
       X3 = 0x41d4e, A520 = 0x41d80, A720 = 0x41d81, X4 = 0x41d82, ORYON = 0x51001 };
typedef struct { int n; uint32_t cap[8], khz[8], midr[8]; } topo_t;
static int fails, checks;

static dm_cpu_topo_in_t mk(const topo_t *t, int use_cap, int use_freq, int use_midr) {
    dm_cpu_topo_in_t in; memset(&in, 0, sizeof in); in.ncpu = t->n;
    for (int c = 0; c < t->n; c++) { in.capacity[c] = use_cap ? t->cap[c] : 0; in.max_khz[c] = use_freq ? t->khz[c] : 0; in.midr[c] = use_midr ? t->midr[c] : 0; }
    return in;
}
static void expect(const char *name, const dm_cpu_topo_in_t *in, int legacy, uint32_t T, uint32_t R, uint32_t P, uint32_t G) {
    dm_cpu_plan_t p; dm_cpu_plan_compute(in, legacy, &p); checks++;
    char a[768], b[400]; dm_cpu_topo_describe(in, &p, a, sizeof a); dm_cpu_plan_describe(&p, b, sizeof b);
    int ok = p.translate == T && p.record == R && p.present == P && p.guest == G;
    printf("%s %-34s %s\n      %s\n", ok ? "ok  " : "FAIL", name, b, a);
    if (!ok) { fails++; printf("      want translate=%x record=%x present=%x guest=%x\n", T, R, P, G); }
}
// The old rule must be what DM_CPU_TOPO=index gives, and (for the flagships) what the default gives.
static void expect_idx(const char *name, const dm_cpu_topo_in_t *in, uint32_t T, uint32_t R, uint32_t P) {
    dm_cpu_plan_t p; dm_cpu_plan_compute(in, 0, &p); checks++;
    int ok = p.idx_translate == T && p.idx_record == R && p.idx_present == P;
    if (!ok) { fails++; printf("FAIL %-34s index rule %x/%x/%x, want %x/%x/%x\n", name, p.idx_translate, p.idx_record, p.idx_present, T, R, P); }
    expect(name, in, 1, T, R, P, 0);
}
static void all_sources(const char *name, const topo_t *t, uint32_t T, uint32_t R, uint32_t P, uint32_t G) {
    char nm[64]; dm_cpu_topo_in_t in;
    in = mk(t, 1, 1, 1); snprintf(nm, sizeof nm, "%s [capacity]", name); expect(nm, &in, 0, T, R, P, G);
    in = mk(t, 0, 1, 1); snprintf(nm, sizeof nm, "%s [freq+midr]", name); expect(nm, &in, 0, T, R, P, G);
}

int main(void) {
    // Snapdragon 8 Gen 3 (Mi 14): A520 x2 cpu0-1 2.27 GHz, A720 x3 cpu2-4 3.15, A720 x2 cpu5-6 2.96, X4 cpu7 3.30
    const topo_t g3 = {8, {380, 380, 924, 924, 924, 867, 867, 1024}, {2265600, 2265600, 3148800, 3148800, 3148800, 2956800, 2956800, 3302400},
                       {A520, A520, A720, A720, A720, A720, A720, X4}};
    const topo_t g3flat = {8, {702, 702, 976, 976, 976, 917, 917, 1024}, {2265600, 2265600, 3148800, 3148800, 3148800, 2956800, 2956800, 3302400},
                           {A520, A520, A720, A720, A720, A720, A720, X4}};
    // Snapdragon 8 Gen 2 (Mi 13 Pro): A510 x3 cpu0-2 2.02 GHz, A715 x2 cpu3-4 + A710 x2 cpu5-6 2.80 (one cpufreq domain), X3 cpu7 3.19
    const topo_t g2 = {8, {350, 350, 350, 852, 852, 852, 852, 1024}, {2016000, 2016000, 2016000, 2803200, 2803200, 2803200, 2803200, 3187200},
                       {A510, A510, A510, A715, A715, A710, A710, X3}};
    // Snapdragon 680 (Redmi 10C): Kryo-silver/A53 x4 cpu0-3 1.90 GHz, A73 x4 cpu4-7 2.40 (flat capacities: 811 vs 1024)
    const topo_t sd680 = {8, {811, 811, 811, 811, 1024, 1024, 1024, 1024}, {1900800, 1900800, 1900800, 1900800, 2400000, 2400000, 2400000, 2400000},
                          {A53Q, A53Q, A53Q, A53Q, A73, A73, A73, A73}};
    // Helio G99: A55 x6 cpu0-5 2.0 GHz, A76 x2 cpu6-7 2.2
    const topo_t g99 = {8, {466, 466, 466, 466, 466, 466, 1024, 1024}, {2000000, 2000000, 2000000, 2000000, 2000000, 2000000, 2200000, 2200000},
                        {A55, A55, A55, A55, A55, A55, A76, A76}};
    // Dimensity 8300: A510 x4 cpu0-3 2.2 GHz, A715 x3 cpu4-6 3.2, A715 cpu7 3.35
    const topo_t d8300 = {8, {384, 384, 384, 384, 978, 978, 978, 1024}, {2200000, 2200000, 2200000, 2200000, 3200000, 3200000, 3200000, 3350000},
                          {A510, A510, A510, A510, A715, A715, A715, A715}};
    // 6+2: A55 x2 cpu0-1 1.8 GHz, A78 x6 cpu2-7 2.4 (one big cluster)
    const topo_t six2 = {8, {420, 420, 1024, 1024, 1024, 1024, 1024, 1024}, {1800000, 1800000, 2400000, 2400000, 2400000, 2400000, 2400000, 2400000},
                         {A55, A55, A78, A78, A78, A78, A78, A78}};
    // Snapdragon 8 Elite: Oryon x6 cpu0-5 3.53 GHz, Oryon x2 cpu6-7 4.32 (no little cores)
    const topo_t elite = {8, {837, 837, 837, 837, 837, 837, 1024, 1024}, {3532800, 3532800, 3532800, 3532800, 3532800, 3532800, 4320000, 4320000},
                          {ORYON, ORYON, ORYON, ORYON, ORYON, ORYON, ORYON, ORYON}};
    // Snapdragon 765G (1+1+6): A55 x6 cpu0-5 1.8 GHz, A76 cpu6 2.2, A76 cpu7 2.4
    const topo_t sd765 = {8, {436, 436, 436, 436, 436, 436, 939, 1024}, {1804800, 1804800, 1804800, 1804800, 1804800, 1804800, 2208000, 2400000},
                          {A55, A55, A55, A55, A55, A55, A76, A76}};
    // Snapdragon 439: A53 x4 cpu0-3 1.95 GHz, A53 x4 cpu4-7 1.45 (both in-order, 74%: no little cluster)
    const topo_t sd439 = {8, {1024, 1024, 1024, 1024, 761, 761, 761, 761}, {1950000, 1950000, 1950000, 1950000, 1450000, 1450000, 1450000, 1450000},
                          {A53Q, A53Q, A53Q, A53Q, A53Q, A53Q, A53Q, A53Q}};
    // Snapdragon 450: A53 x8 2.0 GHz, one class
    const topo_t sd450 = {8, {1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024}, {1800000, 1800000, 1800000, 1800000, 1800000, 1800000, 1800000, 1800000},
                          {A53Q, A53Q, A53Q, A53Q, A53Q, A53Q, A53Q, A53Q}};
    dm_cpu_topo_in_t in;

    // flagships: unchanged (and equal to the index rule)
    all_sources("8 Gen 3 (Mi 14)", &g3, 0x80, 0x7f, 0x7f, 0);
    in = mk(&g3flat, 1, 1, 1); expect("8 Gen 3 flat capacities", &in, 0, 0x80, 0x7f, 0x7f, 0);
    in = mk(&g3, 0, 0, 1); expect("8 Gen 3 [midr only]", &in, 0, 0x80, 0x7f, 0x7f, 0);
    in = mk(&g3, 1, 1, 1); expect_idx("8 Gen 3 index rule", &in, 0x80, 0x7f, 0x7f);
    all_sources("8 Gen 2 (Mi 13 Pro)", &g2, 0x80, 0x7f, 0x7f, 0);
    in = mk(&g2, 0, 0, 1); expect("8 Gen 2 [midr only]", &in, 0, 0x80, 0x7f, 0x7f, 0);
    in = mk(&g2, 1, 1, 1); expect_idx("8 Gen 2 index rule", &in, 0x80, 0x7f, 0x7f);
    all_sources("Dimensity 8300 (1+3+4)", &d8300, 0x80, 0x7f, 0x7f, 0);
    in = mk(&d8300, 1, 1, 1); expect_idx("Dimensity 8300 index rule", &in, 0x80, 0x7f, 0x7f);
    all_sources("8 Elite (2+6, no little)", &elite, 0xc0, 0x3f, 0x3f, 0);
    // weak SoCs: translator alone on the best core, record on the next big one, guests off the translator, present on the littles
    all_sources("SD680 (4+4)", &sd680, 0x80, 0x40, 0x0f, 0x7f);
    // flat capacities (79%) and no MIDR: nothing says the A53s are little, so the old shape stays (documents the limit)
    in = mk(&sd680, 1, 1, 0); expect("SD680 [flat capacity, no midr]", &in, 0, 0xf0, 0x0f, 0x0f, 0);
    in = mk(&sd680, 0, 0, 1); expect("SD680 [midr only]", &in, 0, 0x80, 0x40, 0x0f, 0x7f);
    in = mk(&sd680, 1, 1, 1); expect_idx("SD680 index rule", &in, 0xf0, 0x0f, 0x0f);
    all_sources("Helio G99 (2+6)", &g99, 0x80, 0x40, 0x3f, 0x7f);
    in = mk(&g99, 1, 1, 1); expect_idx("Helio G99 index rule", &in, 0xc0, 0x3f, 0x3f);
    all_sources("6+2 (6 big + 2 little)", &six2, 0x80, 0x40, 0x03, 0x7f);
    in = mk(&six2, 1, 1, 1); expect_idx("6+2 index rule", &in, 0xfc, 0x03, 0x03);
    all_sources("SD765G (1+1+6)", &sd765, 0x80, 0x40, 0x3f, 0x7f);
    // all-in-order phones: the old shape
    all_sources("SD439 (4+4 A53)", &sd439, 0x0f, 0xf0, 0xf0, 0);
    all_sources("SD450 (8x A53)", &sd450, 0, 0xff, 0xff, 0);
    in = mk(&sd450, 1, 1, 1); expect_idx("SD450 index rule", &in, 0, 0xff, 0xff);
    // cpu_capacity all 1024 (kernel without capacities): falls back to the frequency
    { topo_t t = sd680; for (int c = 0; c < 8; c++) t.cap[c] = 1024; in = mk(&t, 1, 1, 1); expect("SD680 capacity all 1024", &in, 0, 0x80, 0x40, 0x0f, 0x7f); }
    // nothing readable: no pin, as before
    { topo_t t = sd680; in = mk(&t, 0, 0, 0); expect("nothing readable", &in, 0, 0, 0xff, 0xff, 0); }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
