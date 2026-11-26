// [gpu-floor] PNH-C01: a GPU clock floor without root while frames run late (port-common/modules/gpu-floor; gpu_floor.c).
// Three hooks in the Vulkan presenter (vulkan_present.cpp), all on the threads that already make those calls:
//   1. before vkCreateDevice: dm_gpu_floor_want_hint() -> enable VK_QCOM_queue_perf_hint and chain dm_gpu_floor_chain() when it says 1;
//      dm_gpu_floor_before_device() (which kernel contexts this process owns before the device exists);
//   2. once per present, on the presenting thread before vkQueuePresentKHR, with no queue lock held: dm_gpu_floor_present(physical,
//      device, queue) (the first call picks the lever and logs it; then the DM_GPU_FLOOR=auto decision and the lever; the queue hint takes
//      the presenter's queue lock itself through dm_present_queue_lock where the port exports it);
//   3. [pc] report: dm_gpu_floor_report(&applied) returns the one-line verdict once (NULL before and after).
// Switches (env file): DM_GPU_FLOOR=auto (default) | 1 (always while frames come) | 0 (off, nothing is opened or called); the rest in gpu_floor.c.
// Builds without Android (HarmonyOS, host tests) get no-op inlines, so a presenter shared with them needs no #if around the calls.
#pragma once
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
void pc_note(const char *id, int applied, const char *fmt, ...);   // [pc] port-common core/pc.h (only the report hunk calls it)
#if defined(__ANDROID__)
int dm_gpu_floor_want_hint(VkPhysicalDevice physical, const VkExtensionProperties *ext, uint32_t n_ext);
void *dm_gpu_floor_chain(void *next);
void dm_gpu_floor_before_device(void);
void dm_gpu_floor_present(VkPhysicalDevice physical, VkDevice device, VkQueue queue);
const char *dm_gpu_floor_report(int *applied);
#else
static inline int dm_gpu_floor_want_hint(VkPhysicalDevice p, const VkExtensionProperties *e, uint32_t n) { (void)p; (void)e; (void)n; return 0; }
static inline void *dm_gpu_floor_chain(void *next) { return next; }
static inline void dm_gpu_floor_before_device(void) {}
static inline void dm_gpu_floor_present(VkPhysicalDevice p, VkDevice d, VkQueue q) { (void)p; (void)d; (void)q; }
static inline const char *dm_gpu_floor_report(int *applied) { if (applied) *applied = 0; return 0; }
#endif
#ifdef __cplusplus
}
#endif
