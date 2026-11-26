// gs-fix (port-common/modules/gs-fix, KB PNH-B-03): geometry-shader SPIR-V rewrites for Adreno, plain C (no Vulkan), host-tested.
// Adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: dm/gfx/vk_android/vka_spvpatch.c vka_spv_gs_claim / vka_spv_gs_fill /
// vka_spv_gs_link. Deviations are listed at each function (vka names in brackets).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// gs_claim [vka_spv_gs_claim]: a geometry shader that writes memory, made to write it once per primitive. Adreno runs a geometry shader
// several times for each primitive (three times for a triangle in, three vertices out: once per declared output vertex), each run
// executing the whole body, so every store / atomic into buffer or image memory happens that many times. Maxwell runs it once.
// The module claims its primitive first, in a storage buffer at (set, binding): word 0 is the draw's tag (written by the host, never
// 0 for a claimed draw), word 1 + gl_PrimitiveIDIn the primitive's claim. At entry each run does
//   old = atomicExchange(claim[1 + PrimitiveId], tag); first = tag == 0 || old != tag
// and only a run with first true writes memory (every store / copy / image write / atomic into Uniform, StorageBuffer,
// PhysicalStorageBuffer or Image memory is put in a selection on it, its block split around it). The other runs see an atomic's
// result as the memory's current value (a compare-exchange: its comparator, so a compare-exchange loop ends). Emits are untouched.
// Deviation from peanuts: peanuts zeroes the claim words for each draw (memset of one word per vertex on the CPU); here the words
// are never cleared: a region the host hands out holds 0 or tags of earlier draws, which are never this draw's tag. tag == 0 is the
// host's "no claim" (an instanced or indirect draw: PrimitiveId restarts per instance / the count is the GPU's): then the exchange
// goes to word 0 (it stays 0) and every run writes, as without the rewrite. Peanuts binds 1 word and lets the exchange run out of range.
// Returns the count of memory writes put under the claim (0: nothing to do, *out NULL), -1 with err on failure (more than one
// invocation per primitive, a PrimitiveId input that is not a 32-bit int, a write in a loop header block, ...). With out and a count
// above 0: the rewritten module (malloc'd, *out_nw words).
int xbg_gsfix_claim(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn);

// gs_fill [vka_spv_gs_fill, unchanged]: a geometry shader that may end a run without emitting anything emits one degenerate primitive
// instead (every vertex at (0, 0, 0, -1), outside the clip volume: it covers nothing). Qualcomm's driver bins the primitives in one
// pass and draws them in another; a shader that decides by a float test whether to emit can decide differently in the two passes,
// and the driver then draws stray triangles and stops the GPU (peanuts: Donkey Kong Bananza, Adreno 740, Forgejo #24). Taken: one
// Geometry entry point, a strip or point output, every emit in the entry function, no streams, gl_Position a vec4 of 32-bit floats.
// NULL with err empty: nothing to do (not such a shader); with err: not rewritten.
uint32_t *xbg_gsfix_fill(const uint32_t *w, size_t nw, size_t *out_nw, char *err, size_t errn);

// The user Input (sc 1) or Output (sc 3) variables of a module by Location: sig[loc] = kind << 4 | components (kind 1 float, 2 int,
// 3 uint, 32-bit scalars and vectors only), 0 none, 0xff a location whose variable has another type or a Component decoration.
// Returns 0, -1 when the module is not read.
int xbg_gsfix_io(const uint32_t *w, size_t nw, uint32_t sc, uint8_t sig[32]);
// The pixel inputs a geometry shader does not write: bit l set when fin[l] is a plain 32-bit input and gout[l] is 0.
uint32_t xbg_gsfix_link_need(const uint8_t gout[32], const uint8_t fin[32]);

// gs_link [vka_spv_gs_link]: the pixel shader's inputs a geometry shader does not write. A passthrough geometry shader hands the
// rasterizer every attribute the vertex shader wrote; a translator that emits the vertices itself copies only what it knew the next
// stage reads. Qualcomm's driver refuses a pipeline whose pixel shader reads a location no earlier stage writes (VK_ERROR_UNKNOWN,
// -13; Turnip links it, the input undefined). Each such location gets an output written before every OpEmitVertex: the vertex
// shader's output at that location for the vertex emitted (the gl_in index of that vertex's position read just before; else vertex
// 0), or a null value when the vertex shader does not write it (undefined on the hardware too). *copied / *nulled: the locations.
// Deviations from peanuts: the vertex / pixel interfaces come as signatures (xbg_gsfix_io) instead of their modules; a location the
// geometry shader already has an Input at is refused (err; peanuts would declare a second input at it); the entry point gets the new
// variables in every SPIR-V version (as peanuts). NULL with err empty: nothing to add.
uint32_t *xbg_gsfix_link(const uint32_t *gw, size_t gn, const uint8_t vout[32], const uint8_t fin[32], uint32_t *copied, uint32_t *nulled,
                         size_t *out_nw, char *err, size_t errn);

// Primitives of a draw of `count` vertices / indices for a Vulkan primitive topology (VkPrimitiveTopology value), an upper bound for
// the claim words (0..9; anything else: count).
uint32_t xbg_gsfix_prims(uint32_t topology, uint32_t count);

#ifdef __cplusplus
}
#endif
