// [DOOM] perf round 4: RelaxedPrecision for translated fragment shaders (DM_GFX_RELAXED, default off).
// Maxwell shaders are fp32 throughout and the translator keeps every register in its own 32-bit function variable; Adreno
// runs mediump (fp16) ALU at twice the rate and with half the register footprint, and register pressure is what holds the
// heavy lighting shaders at the lowest occupancy (1 wave; D2R measurement, src/docs/android-notes/2026-10-18-gpu-profile.md).
// This pass decorates float arithmetic results (and, with bit 2, float register variables) RelaxedPrecision, except the
// backward slice of everything where 16 bits would visibly break: texture coordinates / LOD / depth reference of every image
// op, values converted or bitcast to integers (addresses, packing, indices), stores to anything but Function/Output storage
// (SSBO / image / VT feedback writes) and FragDepth. The slice follows values through function variables (all stores into a
// variable whose load is in the slice join it), so it is conservative: a register reused by coordinate math stays highp.
#include "xbg_spirv_relaxed.h"
#include <unordered_map>
#include <vector>

namespace {
enum : uint32_t {
    OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
    OpMemberDecorate = 72, OpConvertFToU = 109, OpConvertFToS = 110, OpBitcast = 124, OpFunction = 54,
    ScFunction = 7, ScOutput = 3, DecBuiltIn = 11, DecRelaxedPrecision = 0, BuiltInFragDepth = 22,
};
// Instructions with a result type and result id (so operands start at word 3). Enough for the slice: anything else is either
// declared before the functions or carries no value.
bool has_result(uint32_t op) {
    switch (op) {
    case OpStore: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 246: case 218: case 219:
    case 99: /* ImageWrite */ case 224: case 225: /* barriers */ case 56: /* FunctionEnd */ case 8: case 317: case OpDecorate:
    case OpMemberDecorate: case 16: case OpEntryPoint: case 17: case 14: case 3: case 4: case 5: case 6: case 7: case 10: case 11:
        return false;
    default: return true;
    }
}
bool image_op(uint32_t op) { return (op >= 87 && op <= 98) || op == 105; }
bool relaxable_ext(uint32_t g) {   // GLSL.std.450 float ops
    switch (g) {
    case 4: case 8: case 9: case 10: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 37: case 40: case 43: case 46:
    case 48: case 49: case 50: case 66: case 69: case 13: case 14: case 1: case 2: case 3:
        return true;
    default: return false;
    }
}
bool relaxable_op(uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136: case 141: case 142: case 148: case 169: /* Select */ case 81: case 80:
        return true;
    default: return false;
    }
}
}

size_t xbg_spirv_relax(std::vector<uint32_t> &w, unsigned mode, size_t *excluded_out) {
    if (w.size() < 5 || w[0] != 0x07230203u || !(mode & 3)) return 0;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return 0;
    std::vector<uint8_t> is_float(bound, 0), sc(bound, 0xff), excluded(bound, 0), frag_depth(bound, 0);
    std::vector<uint32_t> pointee(bound, ~0u), root(bound, ~0u), def(bound, ~0u);   // def: word index of the defining instruction
    std::unordered_map<uint32_t, std::vector<uint32_t>> stores;   // root variable -> stored value ids
    uint32_t glsl = ~0u;
    size_t insert = 0, fn_start = 0;
    bool fragment = false;
    std::vector<uint32_t> seeds;
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > w.size()) return 0;
        const uint32_t *p = &w[i + 1];
        auto id = [&](uint32_t x) { return x < bound; };
        if (op == OpEntryPoint && n >= 3) fragment = p[0] == 4;
        else if (op == OpExtInstImport && n >= 3) glsl = p[0];   // only GLSL.std.450 is imported by the translator
        else if (op == OpTypeFloat && n >= 3 && id(p[0])) is_float[p[0]] = p[1] == 32;
        else if (op == OpTypeVector && n >= 4 && id(p[0]) && id(p[1])) is_float[p[0]] = is_float[p[1]];
        else if (op == OpTypePointer && n >= 4 && id(p[0])) pointee[p[0]] = p[2];
        else if (op == OpDecorate || op == OpMemberDecorate) {
            insert = i + n;
            if (op == OpDecorate && n >= 4 && p[1] == DecBuiltIn && p[2] == BuiltInFragDepth && id(p[0])) frag_depth[p[0]] = 1;
        } else if (op == OpFunction && !fn_start) fn_start = i;
        if (op == OpVariable && n >= 4 && id(p[1])) { sc[p[1]] = uint8_t(p[2] < 0xff ? p[2] : 0xfe); root[p[1]] = p[1]; if (id(p[0])) pointee[p[1]] = pointee[p[0]]; }
        if (fn_start && has_result(op) && n >= 3 && id(p[1])) def[p[1]] = uint32_t(i);
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 && id(p[1]) && id(p[2])) root[p[1]] = root[p[2]];
        if (op == OpStore && n >= 3 && id(p[0]) && id(p[1])) {
            const uint32_t r = root[p[0]];
            if (r < bound) {
                stores[r].push_back(p[1]);
                if ((sc[r] != ScFunction && sc[r] != ScOutput) || frag_depth[r]) seeds.push_back(p[1]);
            } else seeds.push_back(p[1]);
        }
        if (image_op(op) && n >= 5) for (uint32_t k = 3; k < n - 1; ++k) seeds.push_back(p[k]);
        if (op == 99 && n >= 3) seeds.push_back(p[1]);   // ImageWrite coordinate (the texel goes to memory: whole slice too)
        if (op == 99 && n >= 4) seeds.push_back(p[2]);
        if ((op == OpConvertFToU || op == OpConvertFToS || op == OpBitcast) && n >= 4) seeds.push_back(p[2]);
        i += n;
    }
    if (!fragment || !insert || glsl == ~0u) return 0;
    // Backward slice of the seeds (through function variables).
    std::vector<uint8_t> var_done(bound, 0);
    while (!seeds.empty()) {
        const uint32_t v = seeds.back(); seeds.pop_back();
        if (v >= bound || excluded[v]) continue;
        excluded[v] = 1;
        if (root[v] == v) {   // a variable: every value stored into it
            if (!var_done[v]) { var_done[v] = 1; auto it = stores.find(v); if (it != stores.end()) for (uint32_t s : it->second) seeds.push_back(s); }
            continue;
        }
        if (def[v] == ~0u) continue;
        const size_t i = def[v]; const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OpLoad) { const uint32_t r = root[w[i + 3]]; if (r < bound) seeds.push_back(r); continue; }
        for (uint32_t k = 3; k < n; ++k) seeds.push_back(w[i + k]);
    }
    // Decorate.
    std::vector<uint32_t> add; size_t excl = 0;
    for (size_t i = fn_start; i && i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > w.size()) return 0;
        const uint32_t *p = &w[i + 1];
        bool cand = false;
        if (n >= 3 && p[0] < bound && p[1] < bound && is_float[p[0]]) {
            if (relaxable_op(op)) cand = true;
            else if (op == OpExtInst && n >= 5 && p[2] == glsl && relaxable_ext(p[3])) cand = true;
            else if (op == OpLoad && (mode & 2)) cand = true;
        }
        if (op == OpVariable && (mode & 2) && n >= 4 && p[2] == ScFunction && p[1] < bound && pointee[p[1]] < bound && is_float[pointee[p[1]]]) {
            if (excluded[p[1]]) ++excl; else { add.push_back((3u << 16) | OpDecorate); add.push_back(p[1]); add.push_back(DecRelaxedPrecision); }
        } else if (cand) {
            if (excluded[p[1]]) ++excl; else { add.push_back((3u << 16) | OpDecorate); add.push_back(p[1]); add.push_back(DecRelaxedPrecision); }
        }
        i += n;
    }
    if (excluded_out) *excluded_out = excl;
    if (add.empty()) return 0;
    w.insert(w.begin() + std::ptrdiff_t(insert), add.begin(), add.end());
    return add.size() / 3;
}
