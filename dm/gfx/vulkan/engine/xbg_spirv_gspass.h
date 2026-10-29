// [DOOM] perf round 3 (docs/research/perf_round3.md): DOOM's culling geometry shaders are a plain position pass-through
// while their culling switch is off. Defined in xbg_spirv_gspass.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

struct XbgGsPassGate {
    uint32_t set = 0, binding = 0;   // uniform block holding the switch (DOOM: set 0 binding 27, "gp_c16")
    uint32_t offset = 0;             // byte offset of the switch word in that block (c[0][0].y -> 4)
    uint32_t value = 0;              // the shader is a pass-through whenever the word != value (DOOM: 1)
    uint32_t outputs = 0;            // output variables (1 = gl_Position only; more = pass-through varyings)
};
// True when the geometry shader's entry function starts with `if (c[0][0].k != K) { copy gl_in[v].gl_Position to
// gl_Position, EmitVertex, for v = 0..2; EndPrimitive; return; }` with no side effect before that branch, triangles in,
// a triangle strip of 3 out, one invocation and gl_Position as its only output. Then, with the word != K, the geometry
// stage can be dropped: the rasterizer gets the vertex shader's own positions, bit for bit, in the same order.
// varyings (perf4, DM_GFX_GS_PASSTHRU_VARY): also accept Location outputs, each an exact per-vertex copy of the input
// varying with the same Location/Component (the shipped DOOM bundles: 5 of the 6 culling GS forward 1-6 varyings).
bool xbg_spirv_gs_passthru_gate(const uint32_t *words, size_t count, XbgGsPassGate *gate, bool varyings = false);
