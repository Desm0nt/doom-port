// [DOOM] gl_ClipDistance[8] + gl_CullDistance[1] that no shader writes. The stock Qualcomm driver on Adreno 740 (Mi 13, driver
// 0676.69, 06.11; Samsung S23 players saw the same) drew every body and monster black: the validation layer's only error on
// those pipelines was VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710 (9 declared, limit 8), and a test module that
// wrote all 9 crashed the driver's compiler ("stack corruption detected"), so it does not handle the ninth slot. With the
// values undefined, a primitive whose garbage cull distance is negative is dropped: the depth pre-pass drew the bodies, the
// colour pass did not, and the cleared G-buffer showed through. Turnip ignores the excess.
// Fix (spec-valid on every driver): ClipDistance takes the CullDistance array type (float[1]) and both get 0 at the start of
// the entry point (and before each EmitVertex: a geometry shader's outputs are undefined after an emit).
#include "xbg_spirv_clip.h"
#include <map>
#include <set>

namespace {
enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43,
    OpFunction = 54, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65,
    OpInBoundsAccessChain = 66, OpPtrAccessChain = 67, OpInBoundsPtrAccessChain = 70, OpMemberDecorate = 72, OpEmitVertex = 218,
    OpEmitStreamVertex = 220, OpLabel = 248,
    DecBuiltIn = 11, BuiltInClipDistance = 3, BuiltInCullDistance = 4, StorageOutput = 3,
};
inline uint32_t inst(uint32_t op, uint32_t n) { return (n << 16) | op; }
struct Block { int clip = -1, cull = -1; };
}

size_t xbg_spirv_clip_fix(std::vector<uint32_t> &w, uint32_t max_combined, bool force) {
    if (w.size() < 5 || w[0] != 0x07230203u) return 0;
    std::map<uint32_t, Block> blocks;                       // struct id -> ClipDistance / CullDistance member
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> arrays;   // array id -> (element type, length constant)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> consts;   // constant id -> (type, value)
    std::map<uint32_t, uint32_t> int_width;                 // 32-bit integer types
    std::set<uint32_t> floats;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptrs;     // pointer type -> (storage, pointee)
    std::map<uint32_t, size_t> struct_at;                   // struct id -> word index of its OpTypeStruct
    std::map<uint32_t, uint32_t> vars;                      // variable id -> pointer type
    size_t first_function = 0; uint32_t entry = 0, model = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535; if (!n || i + n > w.size()) return 0;
        if (op == OpMemberDecorate && n >= 5 && w[i + 3] == DecBuiltIn) {
            if (w[i + 4] == BuiltInClipDistance) blocks[w[i + 1]].clip = int(w[i + 2]);
            if (w[i + 4] == BuiltInCullDistance) blocks[w[i + 1]].cull = int(w[i + 2]);
        } else if (op == OpEntryPoint && n >= 3 && !entry) { model = w[i + 1]; entry = w[i + 2]; }
        else if (op == OpTypeInt && n == 4 && w[i + 2] == 32) int_width[w[i + 1]] = 32;
        else if (op == OpTypeFloat && n == 3 && w[i + 2] == 32) floats.insert(w[i + 1]);
        else if (op == OpTypeArray && n == 4) arrays[w[i + 1]] = {w[i + 2], w[i + 3]};
        else if (op == OpTypeStruct && n >= 2) struct_at[w[i + 1]] = i;
        else if (op == OpTypePointer && n == 4) ptrs[w[i + 1]] = {w[i + 2], w[i + 3]};
        else if (op == OpConstant && n == 4) consts[w[i + 2]] = {w[i + 1], w[i + 3]};
        else if (op == OpVariable && n >= 4) vars[w[i + 2]] = w[i + 1];
        else if (op == OpFunction && !first_function) first_function = i;
        i += n;
    }
    if (blocks.empty() || !entry || !first_function) return 0;
    auto len = [&](uint32_t arr) -> int64_t { auto a = arrays.find(arr); if (a == arrays.end()) return -1;
        auto c = consts.find(a->second.second); return c == consts.end() ? -1 : int64_t(c->second.second); };
    // The blocks to rewrite: ClipDistance and CullDistance both present, CullDistance float[1] of the same element type.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> fix;   // struct id -> (new clip type = cull type, element type)
    bool over = false;
    for (auto &b : blocks) {
        auto s = struct_at.find(b.first); if (s == struct_at.end() || b.second.clip < 0 || b.second.cull < 0) continue;
        const size_t at = s->second; const uint32_t members = (w[at] >> 16) - 2;
        if (uint32_t(b.second.clip) >= members || uint32_t(b.second.cull) >= members) continue;
        const uint32_t clip_t = w[at + 2 + b.second.clip], cull_t = w[at + 2 + b.second.cull];
        const int64_t lc = len(clip_t), lu = len(cull_t);
        if (lc < 0 || lu != 1 || arrays[clip_t].first != arrays[cull_t].first || !floats.count(arrays[cull_t].first)) continue;
        if (uint64_t(lc + lu) > max_combined) over = true;
        if (lc > 1) fix[b.first] = {cull_t, arrays[cull_t].first};
    }
    if (fix.empty() || (!over && !force)) return 0;
    // Variables holding such a block, directly or as an array (gl_in[] / gl_out[]).
    std::map<uint32_t, std::pair<uint32_t, bool>> bvars;   // var -> (struct, arrayed)
    for (auto &v : vars) {
        auto p = ptrs.find(v.second); if (p == ptrs.end()) continue;
        uint32_t t = p->second.second; bool arrayed = false;
        if (!fix.count(t)) { auto a = arrays.find(t); if (a == arrays.end() || !fix.count(a->second.first)) continue; t = a->second.first; arrayed = true; }
        bvars[v.first] = {t, arrayed};
    }
    // Any access to the arrays, or a load / store / copy of a whole block, keeps the module unchanged.
    std::set<uint32_t> whole;   // pointers to a whole block (the variables, gl_in[i])
    for (auto &v : bvars) whole.insert(v.first);
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain) && n >= 4) {
            auto v = bvars.find(w[i + 3]);
            if (v != bvars.end()) {
                const size_t first = i + 4 + ((op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain) ? 1 : 0) + (v->second.second ? 1 : 0);
                if (first >= i + n) whole.insert(w[i + 2]);   // gl_in[i] itself
                else { auto c = consts.find(w[first]); if (c == consts.end()) return 0;
                    const Block &b = blocks[v->second.first]; if (int(c->second.second) == b.clip || int(c->second.second) == b.cull) return 0; }
            }
        } else if ((op == OpLoad && n >= 4 && whole.count(w[i + 3])) || (op == OpStore && n >= 3 && whole.count(w[i + 1])) ||
                   ((op == OpCopyMemory || op == OpCopyMemorySized) && n >= 3 && (whole.count(w[i + 1]) || whole.count(w[i + 2])))) return 0;
        i += n;
    }
    // Zero stores need an Output float pointer, a 32-bit integer for the indices and a float 0: reuse or add before the functions.
    uint32_t bound = w[3];
    std::vector<uint32_t> add;
    auto find_const = [&](uint32_t type, uint32_t value) -> uint32_t {
        for (auto &c : consts) if (c.second.first == type && c.second.second == value) return c.first;
        const uint32_t id = bound++; add.insert(add.end(), {inst(OpConstant, 4), type, id, value}); consts[id] = {type, value}; return id; };
    std::vector<std::pair<uint32_t, uint32_t>> outs;   // (output variable, struct) to zero
    for (auto &v : bvars) if (!v.second.second && ptrs[vars[v.first]].first == StorageOutput) outs.push_back({v.first, v.second.first});
    std::vector<uint32_t> zero;
    if (!outs.empty()) {
        if (int_width.empty()) return 0;
        const uint32_t ity = int_width.begin()->first, elem = fix.begin()->second.second;
        uint32_t pf = 0; for (auto &p : ptrs) if (p.second.first == StorageOutput && p.second.second == elem) { pf = p.first; break; }
        if (!pf) { pf = bound++; add.insert(add.end(), {inst(OpTypePointer, 4), pf, StorageOutput, elem}); ptrs[pf] = {StorageOutput, elem}; }
        const uint32_t f0 = find_const(elem, 0), i0 = find_const(ity, 0);
        for (auto &o : outs) for (int m : {blocks[o.second].clip, blocks[o.second].cull}) {
            const uint32_t im = find_const(ity, uint32_t(m));   // the access chain's id is set per site (site() below)
            zero.insert(zero.end(), {inst(OpAccessChain, 6), pf, 0, o.first, im, i0, inst(OpStore, 3), 0, f0});
        }
    }
    // Rewrite: member types first (word positions before first_function do not move until the insertions below).
    for (auto &f : fix) w[struct_at[f.first] + 2 + blocks[f.first].clip] = f.second.first;
    // Zero stores: after the entry function's first label and its variables, and before each EmitVertex (geometry shaders).
    // Every site gets its own access-chain ids (SSA).
    auto site = [&]() { std::vector<uint32_t> z = zero; for (size_t k = 0; k < z.size(); k += 9) z[k + 2] = z[k + 7] = bound++; return z; };
    std::vector<uint32_t> out; out.reserve(w.size() + add.size() + zero.size() * 2);
    out.insert(out.end(), w.begin(), w.begin() + ptrdiff_t(first_function));
    out.insert(out.end(), add.begin(), add.end());
    bool in_entry = false, placed = false;
    for (size_t i = first_function; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op == OpFunction) in_entry = w[i + 2] == entry;
        if (!zero.empty() && model == 3 && (op == OpEmitVertex || op == OpEmitStreamVertex)) { auto z = site(); out.insert(out.end(), z.begin(), z.end()); }
        out.insert(out.end(), w.begin() + ptrdiff_t(i), w.begin() + ptrdiff_t(i + n)); i += n;
        if (in_entry && !placed && op == OpLabel && !zero.empty()) {
            while (i < w.size() && (w[i] & 65535) == OpVariable) { const uint32_t m = w[i] >> 16; out.insert(out.end(), w.begin() + ptrdiff_t(i), w.begin() + ptrdiff_t(i + m)); i += m; }
            auto z = site(); out.insert(out.end(), z.begin(), z.end()); placed = true;
        }
    }
    out[3] = bound; w.swap(out);
    return fix.size();
}
