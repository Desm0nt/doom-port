// [DOOM] perf round 4: RelaxedPrecision for translated fragment shaders (DM_GFX_RELAXED). Defined in xbg_spirv_relaxed.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
// mode bit 1: float arithmetic results; bit 2: also float register variables and their loads. Fragment shaders only; the
// backward slice of texture coordinates, integer conversions, memory stores and FragDepth stays 32-bit. Returns the number
// of decorations added (0 = unchanged); *excluded = candidates kept highp by the slice.
size_t xbg_spirv_relax(std::vector<uint32_t> &words, unsigned mode, size_t *excluded = nullptr);
