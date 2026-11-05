// [DOOM] CPU features for the machine code we write ourselves (generated NVN thunks, nvn_entry.S, hle_count.c stubs).
// The compiler baseline is plain armv8-a and its atomics are outline atomics (run-time choice in the NDK's __aarch64_* helpers),
// so only hand-written ARMv8.1+ instructions need a choice. ARMv8.0 cores (Cortex-A53 / A57 / A72 / A73, e.g. Snapdragon 680 /
// 625 / 660, Helio G85) have no LSE: stadd / ldadd / cas / swp raise SIGILL there (GitHub issue #10, Redmi 10C).
// dm_cpu_lse() = 1 keeps the LSE code exactly as before; 0 selects the ARMv8.0 variant at the one place each address is chosen
// (nvn_lookup, the DM_NVN_PROF wrapper, the DM_HLECOUNT stubs), so LSE phones run the same instructions as before.
// DM_CPU_NOLSE=1 (files/dm/env) forces the ARMv8.0 variants on an LSE phone, to test them on a fast device.
// Apple builds: every A12 / M1 and later core has LSE.
#pragma once
#if defined(__linux__) && defined(__aarch64__)
#include <stdlib.h>
#include <sys/auxv.h>
#define DM_HWCAP_ATOMICS (1ul << 8)   // asm/hwcap.h HWCAP_ATOMICS (LSE, ARMv8.1)
static inline int dm_cpu_hw_lse(void) {
    static int v = -1;   // benign race: every thread computes the same value
    if (v < 0) v = (getauxval(AT_HWCAP) & DM_HWCAP_ATOMICS) != 0;
    return v;
}
// Read at every call (rare: once per NVN name lookup / stub), so DM_CPU_NOLSE from the env file applies whenever it is set.
static inline int dm_cpu_lse(void) {
    const char *e = getenv("DM_CPU_NOLSE");
    return dm_cpu_hw_lse() && !(e && *e && *e != '0');
}
#else
static inline int dm_cpu_hw_lse(void) { return 1; }
static inline int dm_cpu_lse(void) { return 1; }
#endif
