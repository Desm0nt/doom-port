// Round-3 SPIR-V peepholes of the translator's output (docs/research/pipeline_audit.md, round 3 steps 3e / 3f; see xbg_spirv_peep.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Each function returns the number of rewrites; 0: out is empty and the module is to be used unchanged.
//
// HSET2 boolean idiom: FOrdEqual(CompositeExtract(UnpackHalf2x16(v), 0), +-0.0) is true exactly when the low half of v is +-0.
// Where v provably carries only +-0 or a normal / infinite half in its low 16 bits (shifts by 16, masks, PackHalf2x16 of a
// constant, and the translator's if-then flag variables), the comparison is replaced by the boolean those values come from.
size_t xbg_spirv_halfflag_fold(const uint32_t *words, size_t count, std::vector<uint32_t> &out);
// Trunc before ConvertFToU / ConvertFToS (through FMin / FMax / FClamp / NMin / NMax / NClamp with integral constant bounds): the
// conversion rounds toward zero itself, so the conversion path reads the untruncated value.
size_t xbg_spirv_f2i_notrunc(const uint32_t *words, size_t count, std::vector<uint32_t> &out);
// Vertex modules only: OpStore of the constant 1.0 to the gl_PointSize builtin is removed (valid only for pipelines that do not
// rasterize points; the caller picks the module per pipeline). Returns 0 when PointSize is read or written otherwise.
size_t xbg_spirv_strip_pointsize(const uint32_t *words, size_t count, std::vector<uint32_t> &out);
// Not exact for NaN coordinates (opt-in): Select(IsNan(c), 0, c) feeding the coordinate of an OpImageSample* / Gather is
// bypassed, so the sample reads c itself.
size_t xbg_spirv_texcoord_unguard(const uint32_t *words, size_t count, std::vector<uint32_t> &out);
