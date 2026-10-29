// [DOOM] perf round 3: the depth pre-pass draws (~400 a frame in the open Mars levels) run DOOM's triangle-culling geometry
// shader. With r_geometryCulling off (our default, hle_args.c) its switch word c[0][0].y is not 1 and the shader only copies
// gl_in[0..2].gl_Position to the output and emits the triangle: the geometry stage does nothing but cost (Adreno runs a GS
// as an extra pass through memory), and its two written storage buffers make every such draw rebuild its descriptor set.
// This matcher proves the pass-through statically, so the engine may drop the stage for draws whose switch is off
// (xbg_game.cpp, DM_GFX_GS_PASSTHRU). Mirrored and checked on all 863 DOOM bundles by a Python copy of the same rules:
// exactly the six culling shaders match (set 0 binding 27, word 4, value 1).
#include "xbg_spirv_gspass.h"
#include <vector>

namespace {
enum : uint32_t {
    OpLine = 8, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpConstant = 43, OpTypeArray = 28,
    OpTypePointer = 32, OpFunction = 54, OpVariable = 59,
    OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
    OpBitcast = 124, OpLogicalNot = 168, OpINotEqual = 171, OpEmitVertex = 218, OpEndPrimitive = 219, OpSelectionMerge = 247,
    OpLabel = 248, OpBranchConditional = 250, OpReturn = 253, OpNoLine = 317,
    ScInput = 1, ScUniform = 2, ScOutput = 3, ScFunction = 7,
    ModeInvocations = 0, ModeTriangles = 22, ModeOutputVertices = 26, ModeOutputTriangleStrip = 29,
    DecBuiltIn = 11, DecLocation = 30, DecComponent = 31, DecBinding = 33, DecSet = 34, DecOffset = 35, BuiltInPosition = 0,
};
const uint32_t kNone = ~0u;
}

bool xbg_spirv_gs_passthru_gate(const uint32_t *w, size_t n, XbgGsPassGate *gate, bool varyings) {
    if (!w || n < 5 || w[0] != 0x07230203u) return false;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return false;
    std::vector<uint32_t> cval(bound, kNone), ptr_pointee(bound, kNone), binding(bound, kNone), set(bound, 0), off0(bound, kNone), arr_elem(bound, kNone);
    std::vector<uint8_t> is_const(bound, 0), sc(bound, 0xff), pos_member0(bound, 0);
    // [perf4] varyings: Location / Component / BuiltIn of variables, component count of vector and scalar types.
    std::vector<uint32_t> location(bound, kNone), component(bound, 0);std::vector<uint8_t> builtin_var(bound, 0), ncomp(bound, 0);
    std::vector<uint32_t> out_vars;
    uint32_t entry = kNone, invocations = 1, out_vertices = 0, outputs = 0, out_var = kNone;
    bool tri_in = false, strip_out = false;
    size_t fn = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        const uint32_t *p = w + i + 1;   // operands, as in the SPIR-V spec tables
        auto id = [&](uint32_t x) { return x < bound; };
        if (op == OpEntryPoint && wc >= 3) { if (p[0] != 3) return false; entry = p[1]; }   // Geometry only, one entry point
        else if (op == OpExecutionMode && wc >= 3) {
            if (p[1] == ModeInvocations && wc >= 4) invocations = p[2];
            else if (p[1] == ModeOutputVertices && wc >= 4) out_vertices = p[2];
            else if (p[1] == ModeTriangles) tri_in = true;
            else if (p[1] == ModeOutputTriangleStrip) strip_out = true;
        } else if (op == OpConstant && wc == 4 && id(p[1])) { cval[p[1]] = p[2]; is_const[p[1]] = 1; }
        else if (op == OpTypePointer && wc == 4 && id(p[0])) ptr_pointee[p[0]] = p[2];
        else if (op == OpTypeArray && wc == 4 && id(p[0])) arr_elem[p[0]] = p[1];
        else if (op == OpTypeVector && wc == 4 && id(p[0])) ncomp[p[0]] = uint8_t(p[2] <= 4 ? p[2] : 0);
        else if ((op == OpTypeInt || op == OpTypeFloat) && wc >= 3 && id(p[0])) ncomp[p[0]] = 1;
        else if (op == OpMemberDecorate && wc >= 5 && id(p[0]) && p[1] == 0) {
            if (p[2] == DecBuiltIn && p[3] == BuiltInPosition) pos_member0[p[0]] = 1;
            else if (p[2] == DecOffset) off0[p[0]] = p[3];
        } else if (op == OpDecorate && wc >= 4 && id(p[0])) {
            if (p[1] == DecBinding) binding[p[0]] = p[2];
            else if (p[1] == DecSet) set[p[0]] = p[2];
            else if (p[1] == DecLocation) location[p[0]] = p[2];
            else if (p[1] == DecComponent) component[p[0]] = p[2];
            else if (p[1] == DecBuiltIn) builtin_var[p[0]] = 1;
        } else if (op == OpVariable && wc >= 4 && id(p[1])) {
            sc[p[1]] = uint8_t(p[2] < 0xff ? p[2] : 0xfe);
            if (p[2] == ScOutput) { ++outputs; out_vars.push_back(p[1]); }
            if (id(p[0])) ptr_pointee[p[1]] = ptr_pointee[p[0]];   // variable -> its pointee type
        } else if (op == OpFunction && wc >= 5 && p[1] == entry && !fn) fn = i + wc;
        i += wc;
    }
    if (entry == kNone || !fn || invocations != 1 || out_vertices != 3 || !tri_in || !strip_out) return false;
    // Outputs: the gl_PerVertex block (member 0 Position) and, with `varyings`, plain Location variables (a vector or scalar,
    // no BuiltIn). Each varying must be the copy of the input of the same Location/Component (checked below), so the
    // fragment shader reads, without the stage, exactly what the vertex shader wrote there. Without `varyings` gl_Position
    // is the only output (the original rule).
    std::vector<uint32_t> out_key(bound, kNone), out_n(bound, 0);   // per output variable: 0 = Position, else 1 + 4 * Location + Component
    for (uint32_t o : out_vars) {
        const uint32_t t = ptr_pointee[o];
        if (t < bound && pos_member0[t] && out_var == kNone) { out_var = o; out_key[o] = 0; out_n[o] = 4; continue; }
        if (!varyings || builtin_var[o] || location[o] >= 64 || component[o] > 3 || t >= bound || !ncomp[t]) return false;
        out_key[o] = 1 + 4 * location[o] + component[o]; out_n[o] = ncomp[t];
    }
    if (out_var == kNone || outputs > 17 || (!varyings && outputs != 1)) return false;
    auto c = [&](uint32_t x) { return x < bound && is_const[x] ? cval[x] : kNone; };
    // gl_in: an input array of blocks whose member 0 is BuiltIn Position.
    auto gl_in = [&](uint32_t v) { uint32_t a = ptr_pointee[v], e = a < bound ? arr_elem[a] : kNone; return e < bound && pos_member0[e]; };

    // 1. From the function start to the first conditional branch: side-effect free, the gate is the first
    //    `c[0][0].k != K` of a uniform block, carried through function variables and logical nots.
    std::vector<uint32_t> comp(bound, kNone), comp_var(bound, kNone);   // value -> 1 + component / its uniform variable
    std::vector<int8_t> polarity(bound, -1), var_polarity(bound, -1);   // value / function variable -> gate (1) or !gate (0)
    uint32_t g_var = kNone, g_comp = 0, g_value = 0, block = kNone;
    bool have_gate = false;
    size_t i = fn;
    for (; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        const uint32_t *p = w + i + 1;
        switch (op) {
        case OpLabel: case OpVariable: case OpSelectionMerge: case OpLine: case OpNoLine: break;
        case OpAccessChain: case OpInBoundsAccessChain:
            if (wc == 7 && p[1] < bound && p[2] < bound && sc[p[2]] == ScUniform && c(p[3]) == 0 && c(p[4]) == 0 && c(p[5]) < 4) {
                comp[p[1]] = c(p[5]) + 1; comp_var[p[1]] = p[2]; }
            break;
        case OpLoad:
            if (wc < 4 || p[1] >= bound || p[2] >= bound) return false;
            if (comp[p[2]] != kNone) { comp[p[1]] = comp[p[2]]; comp_var[p[1]] = comp_var[p[2]]; }
            if (var_polarity[p[2]] >= 0) polarity[p[1]] = var_polarity[p[2]];
            break;
        case OpBitcast:
            if (wc != 4 || p[1] >= bound || p[2] >= bound) return false;
            if (comp[p[2]] != kNone) { comp[p[1]] = comp[p[2]]; comp_var[p[1]] = comp_var[p[2]]; }
            break;
        case OpINotEqual:
            if (wc != 5 || p[1] >= bound || p[2] >= bound || p[3] >= bound) return false;
            if (!have_gate) {
                uint32_t a = p[2], b = p[3];
                if (comp[b] != kNone && c(a) != kNone) { uint32_t t = a; a = b; b = t; }
                if (comp[a] != kNone && c(b) != kNone) { have_gate = true; g_var = comp_var[a]; g_comp = comp[a] - 1; g_value = c(b); polarity[p[1]] = 1; }
            }
            break;
        case OpLogicalNot:
            if (wc != 4 || p[1] >= bound || p[2] >= bound) return false;
            if (polarity[p[2]] >= 0) polarity[p[1]] = int8_t(!polarity[p[2]]);
            break;
        case OpStore:   // only into the function's own variables
            if (wc < 3 || p[0] >= bound || p[1] >= bound || sc[p[0]] != ScFunction) return false;
            var_polarity[p[0]] = polarity[p[1]];
            break;
        case OpBranchConditional:
            if (wc < 4 || !have_gate || p[0] >= bound || polarity[p[0]] < 0) return false;
            block = polarity[p[0]] ? p[1] : p[2];   // the branch taken while c[0][0].k != K
            break;
        default: return false;
        }
        i += wc;
        if (block != kNone) break;
    }
    if (block == kNone || g_var >= bound || binding[g_var] == kNone) return false;
    const uint32_t blk_type = ptr_pointee[g_var];
    if (blk_type >= bound || off0[blk_type] == kNone) return false;
    // 2. That block: per vertex v = 0..2, gl_Position.k = gl_in[v].gl_Position.k for k = 0..3 (through function variables
    //    only), EmitVertex; then EndPrimitive and return. Nothing else.
    for (; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        if (op == OpLabel && wc == 2 && w[i + 1] == block) { i += wc; break; }
        i += wc;
    }
    if (i >= n) return false;
    // Sources (key << 4) | (v << 2) | k, key as out_key; destinations (index into out_vars << 2) | k.
    std::vector<uint32_t> src(bound, kNone), out_chain(bound, kNone), in_chain(bound, kNone), fvar(bound, kNone);
    std::vector<uint32_t> out_index(bound, kNone);for (size_t q = 0; q < out_vars.size(); ++q) out_index[out_vars[q]] = uint32_t(q);
    std::vector<uint8_t> done(out_vars.size(), 0);
    auto in_varying = [&](uint32_t v) { uint32_t a = ptr_pointee[v], e = a < bound ? arr_elem[a] : kNone;
        return varyings && sc[v] == ScInput && !builtin_var[v] && location[v] < 64 && component[v] <= 3 && e < bound && ncomp[e] ? ncomp[e] : 0u; };
    uint32_t emits = 0;
    for (; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        const uint32_t *p = w + i + 1;
        switch (op) {
        case OpLine: case OpNoLine: break;
        case OpAccessChain: case OpInBoundsAccessChain: {
            if (wc < 5 || p[1] >= bound || p[2] >= bound) return false;
            const uint32_t b = p[2];
            if (b == out_var && wc == 6 && c(p[3]) == 0 && c(p[4]) < 4) out_chain[p[1]] = out_index[b] << 2 | c(p[4]);
            else if (b != out_var && out_index[b] != kNone && wc == 5 && c(p[3]) < out_n[b]) out_chain[p[1]] = out_index[b] << 2 | c(p[3]);
            else if (sc[b] == ScInput && wc == 7 && gl_in(b) && c(p[3]) < 3 && c(p[4]) == 0 && c(p[5]) < 4) in_chain[p[1]] = c(p[3]) << 2 | c(p[5]);
            else if (wc == 6 && in_varying(b) && c(p[3]) < 3 && c(p[4]) < in_varying(b))
                in_chain[p[1]] = (1 + 4 * location[b] + component[b]) << 4 | c(p[3]) << 2 | c(p[4]);
            else return false;
            break; }
        case OpLoad:
            if (wc < 4 || p[1] >= bound || p[2] >= bound) return false;
            if (in_chain[p[2]] != kNone) src[p[1]] = in_chain[p[2]];
            else if (fvar[p[2]] != kNone) src[p[1]] = fvar[p[2]];
            else return false;
            break;
        case OpStore:
            if (wc < 3 || p[0] >= bound || p[1] >= bound || src[p[1]] == kNone) return false;
            if (out_chain[p[0]] != kNone) {
                const uint32_t o = out_vars[out_chain[p[0]] >> 2], k = out_chain[p[0]] & 3, s = src[p[1]];
                if ((s >> 4) != out_key[o] || ((s >> 2) & 3) != emits || (s & 3) != k) return false;
                done[out_chain[p[0]] >> 2] |= uint8_t(1u << k);
            } else if (sc[p[0]] == ScFunction) fvar[p[0]] = src[p[1]];
            else return false;
            break;
        case OpEmitVertex:
            for (size_t q = 0; q < out_vars.size(); ++q) { if (done[q] != (1u << out_n[out_vars[q]]) - 1) return false; done[q] = 0; }
            ++emits; break;
        case OpEndPrimitive: if (emits != 3) return false; break;
        case OpReturn:
            if (emits != 3) return false;
            if (gate) { gate->set = set[g_var]; gate->binding = binding[g_var]; gate->offset = off0[blk_type] + 4 * g_comp; gate->value = g_value; gate->outputs = outputs; }
            return true;
        default: return false;
        }
        i += wc;
    }
    return false;
}
