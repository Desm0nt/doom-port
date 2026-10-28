// [DOOM] DOOM's triangle-culling geometry shaders (defined in xbg_spirv_lone.cpp, see the comment there)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// True for a geometry shader with the Maxwell warp compaction: ballot / shuffle plus an atomic add and a compare-exchange.
bool xbg_spirv_gscull_shape(const uint32_t *words, size_t count);
// Turns individual culling tests off in place (mask bits: 2 Hi-Z occlusion, 4 small triangle, 8 back face, 16 frustum).
// Returns the number of instructions changed; *applied gets the bits that matched exactly one site.
size_t xbg_spirv_gscull_relax(std::vector<uint32_t> &words, uint32_t mask, uint32_t *applied);
// Diagnostic: the slot of a visible triangle becomes 3 * PrimitiveId (identity compaction with all tests off). 1 = applied.
size_t xbg_spirv_gscull_identity(std::vector<uint32_t> &words);
// The culling pairs' vertex index travels VS -> GS as float bits (denormals); rewrites it to travel as a float value (vertex
// shader: VertexIndex & 0xffff path to an output; geometry shader: the values handed to the index insert). Returns the
// number of bitcasts turned into conversions (0: not this idiom).
size_t xbg_spirv_index_carrier(std::vector<uint32_t> &words, uint32_t stage);
// The compare-exchange 16-bit store keeps the old index word in a float temporary (denormal / NaN bit patterns); retypes
// such temporaries to uint in CAS functions. Returns the number of variables retyped.
size_t xbg_spirv_cas_int_temps(std::vector<uint32_t> &words);
