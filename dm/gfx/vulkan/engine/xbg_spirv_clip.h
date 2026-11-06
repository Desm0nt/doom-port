// Unused gl_ClipDistance / gl_CullDistance arrays over the device limit (see xbg_spirv_clip.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The translator declares gl_PerVertex with ClipDistance[8] and CullDistance[1] in every pre-rasterization stage, 9 in all,
// while DOOM never reads or writes them. Where that sum is above max_combined (VkPhysicalDeviceLimits::
// maxCombinedClipAndCullDistances; 8 on Adreno), this shrinks ClipDistance to the CullDistance array type (one float) and
// stores 0 to both at the start of the entry point (and before every EmitVertex of a geometry shader), so the values the
// rasterizer clips and culls with are defined. Modules that access either array, load or copy a whole gl_PerVertex, or
// whose CullDistance is not float[1] are left unchanged.
// Returns the number of gl_PerVertex blocks rewritten (0: words unchanged). force: rewrite even when the sum fits.
size_t xbg_spirv_clip_fix(std::vector<uint32_t> &words, uint32_t max_combined, bool force);
