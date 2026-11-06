// [DOOM] CPU topology and thread placement by core capacity (GitHub issue #10 follow-up: Redmi 10C / Snapdragon 680, Helio G99).
// Before 11.06 the translation thread was pinned to the cores with the highest cpuinfo_max_freq and every other thread we place
// (dm-record, dm-present) got "all cores minus that pin". On a flagship (1 prime + 4-5 mid + 2-3 little) that leaves the mid cores
// to the record thread; on a 4+4 or 2+6 phone the pin takes every big core and the record thread lands on the little ones
// (SD680: translation pin 0xf0, dm-record affinity 0xf = four A53).
// The plan below ranks the cores by capacity (sysfs cpu_capacity, else cpuinfo_max_freq weighted by the core type, else the
// core type from MIDR), groups equal ones into clusters and gives each role its cores:
//   - a second strong cluster with >= 2 cores outside the fastest one (8 Gen 3, 8 Gen 2, Dimensity 8300 / 9300, 8 Elite):
//     exactly the old masks (translate = fastest cluster, record = present = the rest, guests left to the scheduler);
//   - otherwise (one strong cluster: 4+4, 2+6, 6+2; or prime + one big, 1+1+6): translate = the fastest core alone, record = the
//     next strong core, guest threads = every core but the translator's, present = the little cluster.
// DM_CPU_TOPO=index restores the old rule everywhere (A/B). The pure part (dm_cpu_plan_compute) has a host test:
// android/tests/t_cpu_topo.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DM_CPU_MAX 32
typedef struct {
    int ncpu;                        // CPUs 0..ncpu-1 (_SC_NPROCESSORS_CONF, at most 32)
    uint32_t capacity[DM_CPU_MAX];   // /sys/devices/system/cpu/cpuN/cpu_capacity (0 = unreadable)
    uint32_t max_khz[DM_CPU_MAX];    // cpuN/cpufreq/cpuinfo_max_freq (0 = unreadable)
    uint32_t midr[DM_CPU_MAX];       // implementer << 12 | part number (0 = unknown)
} dm_cpu_topo_in_t;

typedef struct {
    int legacy;                      // DM_CPU_TOPO=index: the masks below are the old rule's
    const char *source;              // what ranked the cores: "capacity", "freq", "midr", "none"
    uint32_t all;                    // (1 << ncpu) - 1
    int nclusters;                   // fastest first
    uint32_t cluster[DM_CPU_MAX], cluster_score[DM_CPU_MAX];
    uint32_t little;                 // the little cluster (0 = none)
    uint32_t translate;              // nvn-translate (caller of present) pin; 0 = no pin
    uint32_t record;                 // dm-record affinity
    uint32_t present;                // dm-present affinity
    uint32_t guest;                  // guest threads' affinity; 0 = left to the scheduler
    uint32_t idx_translate, idx_record, idx_present;   // the old rule's masks, for the log and the test
} dm_cpu_plan_t;

void dm_cpu_plan_compute(const dm_cpu_topo_in_t *in, int legacy, dm_cpu_plan_t *out);
// "[cpu] topology" / "[cpu] plan" text (no prefix); returns the length written
size_t dm_cpu_topo_describe(const dm_cpu_topo_in_t *in, const dm_cpu_plan_t *p, char *buf, size_t n);
size_t dm_cpu_plan_describe(const dm_cpu_plan_t *p, char *buf, size_t n);
const char *dm_cpu_part_name(uint32_t midr);
// Linux / Android: reads the topology once (DM_CPU_TOPO, DM_GFX_PRESENT_CPUS, DM_GUEST_CPUS from the env), logs the two
// [cpu] lines, returns the same plan to every caller. Thread-safe.
const dm_cpu_plan_t *dm_cpu_plan(void);

#ifdef __cplusplus
}
#endif
