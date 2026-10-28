// [DOOM] Subgroup operations in a stage the device does not run them in. The shader library translates Maxwell warp
// instructions (VOTE / SHFL / S2R SR_LANEID) into GroupNonUniform operations in every stage, DOOM's geometry shaders
// included (a ballot to test "no lane in the warp has X" and a range-checked shuffle). Vulkan only allows them in the stages
// of VkPhysicalDeviceSubgroupProperties::supportedStages: the Qualcomm driver on Adreno 750 leaves geometry out, Mali and
// older Adreno leave out more. There every invocation is treated as a warp of one lane (lane 0, size 1): a ballot holds
// only the invocation's own bit, a shuffle / broadcast / reduction returns the invocation's own value, all / any return the
// predicate. For the warp idioms the translator emits (vote-then-skip, shuffle with an in-range check that falls back to the
// lane's own value) the single-lane answer is a valid execution of the same program, just without sharing across lanes.
// Subgroup masks become lane 0's bit (Eq/Ge/Le) or 0 (Gt/Lt). Anything else subgroup-related (bit counts, inverse ballot,
// quad operations, the SPV_KHR_shader_ballot forms) leaves the module unchanged.
#include "xbg_spirv_lone.h"
#include "xbg_spirv_gscull.h"

namespace {
enum : uint32_t {
    OpName = 5, OpMemberName = 6, OpExtension = 10, OpEntryPoint = 15, OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21,
    OpTypeVector = 23, OpConstantTrue = 41, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61,
    OpDecorate = 71, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpCompositeConstruct = 80, OpCopyObject = 83, OpSelect = 169,
    DecBuiltIn = 11, BuiltInSubgroupSize = 36, BuiltInSubgroupLocalInvocationId = 41,
    CapGroupNonUniformFirst = 61, CapGroupNonUniformLast = 68,
};
inline uint32_t inst(uint32_t op, uint32_t words) { return (words << 16) | op; }
// Word w (1 = the first after the opcode) of a function-body instruction holds a literal, not an id: a number that equals a
// built-in's id there is not a use (same table as xbg_spirv_warp.cpp).
bool literal_at(uint32_t op, uint32_t w) {
    switch (op) {
    case 8: return w >= 2;                       // OpLine
    case 12: return w == 4;                      // OpExtInst instruction number
    case 59: return w == 3;                      // OpVariable storage class
    case 61: return w >= 4;                      // OpLoad memory operands
    case 62: case 63: return w >= 3;             // OpStore / OpCopyMemory memory operands
    case 79: case 82: return w >= 5;             // OpVectorShuffle components, OpCompositeInsert indexes
    case 81: return w >= 4;                      // OpCompositeExtract indexes
    case 246: return w >= 3;                     // OpLoopMerge
    case 247: return w >= 2;                     // OpSelectionMerge
    case 250: return w >= 4;                     // OpBranchConditional weights
    case 251: return w >= 3;                     // OpSwitch literals / labels
    case 99: return w >= 4;                      // OpImageWrite operands
    case 43: case 41: case 42: return true;      // constants
    default: return op >= 87 && op <= 98 && w >= 5;   // image sample / fetch / gather / read operands
    }
}
}

size_t xbg_spirv_lone_lane(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear();
    if (!in || n < 5 || in[0] != 0x07230203u) return 0;
    uint32_t bound = in[3];
    std::vector<uint8_t> builtin(bound, 0);   // 1 lane id, 2 subgroup size, 3 mask holding lane 0 (Eq/Ge/Le), 4 empty mask (Gt/Lt)
    std::vector<uint8_t> is_const(bound, 0); std::vector<uint32_t> const_val(bound, 0);
    std::vector<uint32_t> chain_word(bound, ~0u);   // access chain into a mask: the word it reads (kind in chain_kind)
    std::vector<uint8_t> chain_kind(bound, 0);
    std::vector<uint32_t> int_width(bound, 0), int_signed(bound, 0), vec_comp(bound, 0);
    uint32_t t_bool = 0, t_uint = 0, c_true = 0, c_u0 = 0, c_u1 = 0;
    size_t first_func = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        if (op == OpFunction) { first_func = i; break; }
        if (op == OpExtension) return 0;   // SPV_KHR_shader_ballot / subgroup_vote and the like: not handled
        if (op == OpDecorate && wc >= 4 && in[i + 2] == DecBuiltIn && in[i + 1] < bound) {
            uint32_t b = in[i + 3];
            if (b == BuiltInSubgroupLocalInvocationId) builtin[in[i + 1]] = 1;
            else if (b == BuiltInSubgroupSize) builtin[in[i + 1]] = 2;
            else if (b >= 36 && b <= 41) return 0;            // subgroup id / count
            else if (b == 4416 || b == 4417 || b == 4419) builtin[in[i + 1]] = 3;   // Eq / Ge / Le: lane 0's own bit
            else if (b == 4418 || b == 4420) builtin[in[i + 1]] = 4;                // Gt / Lt: no other lanes
        }
        if (op == OpTypeBool && wc == 2) t_bool = in[i + 1];
        else if (op == OpTypeInt && wc == 4 && in[i + 1] < bound) { int_width[in[i + 1]] = in[i + 2]; int_signed[in[i + 1]] = in[i + 3]; if (in[i + 2] == 32 && !in[i + 3] && !t_uint) t_uint = in[i + 1]; }
        else if (op == OpTypeVector && wc == 4 && in[i + 1] < bound) vec_comp[in[i + 1]] = in[i + 2];
        else if (op == OpConstantTrue && wc == 3 && in[i + 1] == t_bool && t_bool && !c_true) c_true = in[i + 2];
        if (op == OpConstant && wc == 4 && in[i + 2] < bound) { is_const[in[i + 2]] = 1; const_val[in[i + 2]] = in[i + 3]; }
        if (op == OpConstant && wc == 4 && t_uint && in[i + 1] == t_uint) { if (in[i + 3] == 0 && !c_u0) c_u0 = in[i + 2]; if (in[i + 3] == 1 && !c_u1) c_u1 = in[i + 2]; }
        i += wc;
    }
    if (!first_func) return 0;
    // Pass 2: every subgroup operation must be one this knows; built-ins may only be loaded.
    size_t sites = 0;
    bool need_true = false, need_uint = false;
    for (size_t i = first_func; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        if (op >= 4421 && op <= 4432) return 0;   // KHR ballot / vote forms
        if (op >= 333 && op <= 366) {
            switch (op) {
            case 333: case 336: need_true = true; break;                       // Elect, AllEqual
            case 334: case 335: case 337: case 338: case 345: case 346: case 347: case 348: break;   // All, Any, Broadcast(First), Shuffle(Xor/Up/Down)
            case 339: if (wc != 5 || vec_comp[in[i + 1]] == 0) return 0; need_uint = true; break;   // Ballot
            default:
                if (op >= 349 && op <= 364) { if (wc < 6) return 0; break; }   // reductions / scans: value at word 5
                return 0;                                                      // inverse ballot, bit counts, quad ops
            }
            ++sites;
        } else if (op == OpLoad && wc >= 4 && in[i + 3] < bound && builtin[in[i + 3]]) {
            need_uint = true; ++sites;
        } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 4 && in[i + 3] < bound && builtin[in[i + 3]] >= 3) {
            if (wc != 5 || in[i + 4] >= bound || !is_const[in[i + 4]] || in[i + 2] >= bound) return 0;
            chain_word[in[i + 2]] = const_val[in[i + 4]]; chain_kind[in[i + 2]] = builtin[in[i + 3]]; ++sites;
        } else if (op == OpLoad && wc >= 4 && in[i + 3] < bound && chain_kind[in[i + 3]]) {
            need_uint = true; ++sites;
        } else {
            for (uint32_t w = 1; w < wc; ++w) if (in[i + w] < bound && builtin[in[i + w]] && !literal_at(op, w)) return 0;   // other use of a built-in
        }
        i += wc;
    }
    if (!sites) return 0;
    if (need_true && !t_bool) return 0;
    if (need_uint && !t_uint) return 0;
    // New constants go right before the first function.
    std::vector<uint32_t> consts;
    if (need_true && !c_true) { c_true = bound++; consts.insert(consts.end(), {inst(OpConstantTrue, 3), t_bool, c_true}); }
    if (need_uint && !c_u0) { c_u0 = bound++; consts.insert(consts.end(), {inst(OpConstant, 4), t_uint, c_u0, 0}); }
    if (need_uint && !c_u1) { c_u1 = bound++; consts.insert(consts.end(), {inst(OpConstant, 4), t_uint, c_u1, 1}); }
    out.reserve(n + consts.size() + sites * 8);
    out.insert(out.end(), in, in + 5);
    for (size_t i = 5; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        const uint32_t *p = in + i;
        if (i == first_func) out.insert(out.end(), consts.begin(), consts.end());
        if (i < first_func) {
            if (op == OpCapability && p[1] >= CapGroupNonUniformFirst && p[1] <= CapGroupNonUniformLast) { i += wc; continue; }
            if ((op == OpName || op == OpDecorate || op == OpMemberName) && p[1] < bound && builtin[p[1]]) { i += wc; continue; }
            if (op == OpVariable && wc >= 4 && p[2] < bound && builtin[p[2]]) { i += wc; continue; }
            if (op == OpEntryPoint) {
                // execution model, id, literal name (nul-terminated, padded), interface ids
                size_t at = 3; while (at < wc && (p[at] >> 24) != 0) ++at; ++at;
                size_t start = out.size(); out.insert(out.end(), p, p + at);
                for (size_t w = at; w < wc; ++w) if (!(p[w] < bound && builtin[p[w]])) out.push_back(p[w]);
                out[start] = inst(OpEntryPoint, uint32_t(out.size() - start));
                i += wc; continue;
            }
            out.insert(out.end(), p, p + wc); i += wc; continue;
        }
        if (op == OpLoad && wc >= 4 && p[3] < bound && builtin[p[3]] >= 3) {   // whole mask: uvec4(bit, 0, 0, 0)
            out.insert(out.end(), {inst(OpCompositeConstruct, 7), p[1], p[2], builtin[p[3]] == 3 ? c_u1 : c_u0, c_u0, c_u0, c_u0});
        } else if (op == OpLoad && wc >= 4 && p[3] < bound && builtin[p[3]]) {
            out.insert(out.end(), {inst(OpCopyObject, 4), p[1], p[2], builtin[p[3]] == 1 ? c_u0 : c_u1});
        } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 4 && p[3] < bound && builtin[p[3]] >= 3) {
            // dropped: its only uses are the loads below
        } else if (op == OpLoad && wc >= 4 && p[3] < bound && chain_kind[p[3]]) {
            out.insert(out.end(), {inst(OpCopyObject, 4), p[1], p[2], chain_kind[p[3]] == 3 && chain_word[p[3]] == 0 ? c_u1 : c_u0});
        } else if (op >= 333 && op <= 366) {
            uint32_t type = p[1], res = p[2], value = 0;
            switch (op) {
            case 333: case 336: value = c_true; break;
            case 334: case 335: case 337: case 338: case 345: case 346: case 347: case 348: value = p[4]; break;
            case 339: {
                uint32_t bit = bound++;
                out.insert(out.end(), {inst(OpSelect, 6), t_uint, bit, p[4], c_u1, c_u0});
                out.insert(out.end(), {inst(OpCompositeConstruct, 7), type, res, bit, c_u0, c_u0, c_u0});
                i += wc; continue;
            }
            default: value = p[5]; break;   // reduction / scan of one lane: the lane's own value (inclusive) -- exclusive scans are rare in translated code
            }
            out.insert(out.end(), {inst(OpCopyObject, 4), type, res, value});
        } else {
            out.insert(out.end(), p, p + wc);
        }
        i += wc;
    }
    out[3] = bound;
    return sites;
}

// [DOOM] Test switches for DOOM's triangle-culling geometry shaders (703f1fcc3cda7204 and five siblings that differ only in
// varying locations). Each GS invocation tests its triangle (frustum out-codes, back face, "no pixel centre covered", and for
// triangles of at most 8x8 pixels a 4x4 Hi-Z occlusion probe), always emits the triangle for the depth pre-pass, and appends
// the visible ones as 16-bit indices to a compacted index buffer (atomic add on the counter, compare-exchange insert of each
// half word) that the later passes draw. A triangle culled wrongly is therefore missing from the G-buffer only: a hole whose
// silhouette is still in the pre-pass depth. Each test is gated by its own constant-buffer word (c[0].x back face, .z
// frustum, .w small triangle; .y == 1 selects the compaction at all); the occlusion probe is the module's only
// OpFOrdGreaterThan (nearest triangle depth > farthest Hi-Z sample). Turning a test off replaces its gate with false in
// place (OpLogicalAnd false false: same word count, so nothing else moves); the triangle then counts as visible for that test.
namespace {
enum : uint32_t { OpConstantFalse = 42, OpBitcast = 124, OpLogicalAnd = 167, OpINotEqual = 171, OpFOrdGreaterThan = 186,
                  OpAtomicCompareExchange = 230, OpAtomicIAdd = 234, StorageUniform = 2 };
}
bool xbg_spirv_gscull_shape(const uint32_t *w, size_t n) {
    if (!w || n < 5 || w[0] != 0x07230203u) return false;
    bool geometry = false, warp = false, add = false, cas = false;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        if (op == OpEntryPoint && wc >= 3 && w[i + 1] == 3) geometry = true;
        else if (op == 339 || op == 345) warp = true;   // ballot / shuffle: the Maxwell warp compaction
        else if (op == OpAtomicIAdd) add = true;
        else if (op == OpAtomicCompareExchange) cas = true;
        i += wc;
    }
    return geometry && warp && add && cas;
}
size_t xbg_spirv_gscull_relax(std::vector<uint32_t> &w, uint32_t mask, uint32_t *applied) {
    if (applied) *applied = 0;
    size_t n = w.size();
    if (n < 5 || w[0] != 0x07230203u || !(mask & 30)) return 0;
    uint32_t bound = w[3], t_bool = 0, c_false = 0;
    std::vector<uint8_t> is_zero(bound, 0), is_uniform(bound, 0), comp(bound, 0);   // comp: 1 + c[0].<k> along chain -> load -> bitcast
    std::vector<uint32_t> const_val(bound, ~0u);
    std::vector<size_t> gate[4], greater;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        const uint32_t *p = w.data() + i;
        if (op == OpTypeBool && wc == 2) t_bool = p[1];
        else if (op == OpConstantFalse && wc == 3 && p[1] == t_bool && t_bool && !c_false) c_false = p[2];
        else if (op == OpConstant && wc == 4 && p[2] < bound) { const_val[p[2]] = p[3]; is_zero[p[2]] = p[3] == 0; }
        else if (op == OpVariable && wc >= 4 && p[2] < bound && p[3] == StorageUniform) is_uniform[p[2]] = 1;
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc == 7 && p[2] < bound && p[3] < bound && is_uniform[p[3]]
                 && p[4] < bound && is_zero[p[4]] && p[5] < bound && is_zero[p[5]] && p[6] < bound && const_val[p[6]] < 4)
            comp[p[2]] = uint8_t(1 + const_val[p[6]]);   // c[0][0].<k>: member 0, vec4 0, component k
        else if ((op == OpLoad || op == OpBitcast) && wc >= 4 && p[2] < bound && p[3] < bound && comp[p[3]]) comp[p[2]] = comp[p[3]];
        else if (op == OpINotEqual && wc == 5 && p[1] == t_bool && p[3] < bound && p[4] < bound) {
            uint32_t a = p[3], b = p[4];
            if (is_zero[a] && comp[b]) gate[comp[b] - 1].push_back(i);
            else if (is_zero[b] && comp[a]) gate[comp[a] - 1].push_back(i);
        } else if (op == OpFOrdGreaterThan && wc == 5 && p[1] == t_bool) greater.push_back(i);
        i += wc;
    }
    if (!c_false) return 0;
    size_t k = 0; uint32_t done = 0;
    auto off = [&](const std::vector<size_t> &at, uint32_t bit) {
        if (!(mask & bit) || at.size() != 1) return;   // not exactly one site: the shape is not the known one, leave it
        uint32_t *p = w.data() + at[0];
        p[0] = (5u << 16) | OpLogicalAnd; p[3] = c_false; p[4] = c_false;   // result type and id stay
        ++k; done |= bit;
    };
    off(greater, 2);    // Hi-Z occlusion probe
    off(gate[3], 4);    // small triangle (c[0].w)
    off(gate[0], 8);    // back face (c[0].x)
    off(gate[2], 16);   // frustum out-codes (c[0].z)
    if (applied) *applied = done;
    return k;
}
// [DOOM] Identity compaction (diagnostic): the slot a visible triangle's three indices go to becomes 3 * PrimitiveId instead
// of the reserved counter value, so with every test off the compacted list is the input list in input order. The counter is
// still advanced (the draw count stays the game's). Only the atomic add's result is replaced: in the lone-lane form the
// prefix sum adds nothing to it, so it is the slot. Adds a PrimitiveId input (BuiltIn 7) to the entry point.
size_t xbg_spirv_gscull_identity(std::vector<uint32_t> &w) {
    const size_t n = w.size();
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3], t_uint = 0, p_in_uint = 0, c3 = 0;
    size_t entry = 0, first_func = 0, last_decor = 0, atomic = 0, atomics = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        const uint32_t *p = w.data() + i;
        if (op == OpEntryPoint && !entry) entry = i;
        else if (op == OpDecorate || op == 72) last_decor = i + wc;
        else if (op == OpTypeInt && wc == 4 && p[2] == 32 && !p[3] && !t_uint) t_uint = p[1];
        else if (op == 32 && wc == 4 && p[2] == 1 && t_uint && p[3] == t_uint && !p_in_uint) p_in_uint = p[1];   // OpTypePointer Input uint
        else if (op == OpConstant && wc == 4 && t_uint && p[1] == t_uint && p[3] == 3 && !c3) c3 = p[2];
        else if (op == OpFunction && !first_func) first_func = i;
        else if (op == OpAtomicIAdd && wc == 7 && p[1] == t_uint) { atomic = i; ++atomics; }
        i += wc;
    }
    if (!entry || !first_func || !last_decor || !t_uint || atomics != 1) return 0;
    const uint32_t var = bound++, loaded = bound++, dummy = bound++, res = w[atomic + 2];
    std::vector<uint32_t> types;   // before the first function: pointer type / constant if missing, then the variable
    if (!p_in_uint) { p_in_uint = bound++; types.insert(types.end(), {inst(32, 4), p_in_uint, 1, t_uint}); }
    if (!c3) { c3 = bound++; types.insert(types.end(), {inst(OpConstant, 4), t_uint, c3, 3}); }
    types.insert(types.end(), {inst(OpVariable, 4), p_in_uint, var, 1});
    std::vector<uint32_t> out; out.reserve(n + 32);
    for (size_t i = 0; i < n;) {
        if (i < 5) { out.insert(out.end(), w.begin(), w.begin() + 5); i = 5; continue; }
        uint32_t wc = w[i] >> 16;
        if (i == last_decor) out.insert(out.end(), {inst(OpDecorate, 4), var, DecBuiltIn, 7});
        if (i == first_func) out.insert(out.end(), types.begin(), types.end());
        if (i == entry) {
            size_t start = out.size(); out.insert(out.end(), w.begin() + i, w.begin() + i + wc); out.push_back(var);
            out[start] = inst(OpEntryPoint, uint32_t(out.size() - start)); i += wc; continue;
        }
        if (i == atomic) {
            size_t start = out.size(); out.insert(out.end(), w.begin() + i, w.begin() + i + wc); out[start + 2] = dummy;
            out.insert(out.end(), {inst(OpLoad, 4), t_uint, loaded, var, inst(132, 5), t_uint, res, loaded, c3});   // OpIMul
            i += wc; continue;
        }
        out.insert(out.end(), w.begin() + i, w.begin() + i + wc); i += wc;
    }
    out[3] = bound; w.swap(out);
    return 1;
}
// [DOOM] Vertex index carried as float bits (DM_GFX_GSCULL_FIX 0x800). The culling pairs (VS 53014b303bb87b09, fc05bdb8034c2633,
// 58c049020fb92076, 965e2c6a6007db74, e2ce6d304e5c6428, badf12d97b0ed8ef -> the six culling geometry shaders) pass
// gl_VertexIndex & 0xffff to the geometry shader as the bits of a float varying (intBitsToFloat), through float temporaries
// on both sides, and the geometry shader writes those bits as the compacted 16-bit index. Every such value is an fp32
// denormal; a float move that flushes denormals (depending on how the compiler copies a value) turns the index into 0: the
// triangle then uses vertex 0 of the mesh (giant / misplaced triangle, hole where the real one was). Only the compacted list
// sees these values, so it shows only with culling on. The rewrite carries the index as a float VALUE instead (exact below
// 2^24): in the vertex shader every bitcast on the path from VertexIndex to the output becomes a conversion (int -> float:
// ConvertSToF / UToF, float -> int: ConvertFToS / FToU); in the geometry shader the float -> int bitcasts whose result is
// stored to a variable handed to the index-insert function (the one with the compare-exchange) become conversions. Same
// word counts, rewritten in place. The vertex side applies only to a vertex shader whose path from VertexIndex to an output
// goes through "& 0xffff" (this idiom); both sides must be rewritten together.
namespace {
enum : uint32_t { OpTypeFloat = 22, OpStore = 62, OpFunctionCall = 57, OpFunctionEnd = 56, OpBitwiseAnd = 199,
                  OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112, DecLocation = 30,
                  BuiltInVertexIndex = 42, StorageInput = 1, StorageOutput = 3, StorageFunction = 7 };
}
size_t xbg_spirv_index_carrier(std::vector<uint32_t> &w, uint32_t stage) {
    const size_t n = w.size();
    if (n < 5 || w[0] != 0x07230203u || (stage != 0 && stage != 3)) return 0;
    const uint32_t bound = w[3];
    std::vector<uint8_t> is_float(bound, 0), int_kind(bound, 0), sc(bound, 0xff), vindex(bound, 0), has_loc(bound, 0), taint(bound, 0), tvar(bound, 0);
    std::vector<uint32_t> base(bound, 0), type_of(bound, 0), cval(bound, ~0u);
    std::vector<uint8_t> cas_func(bound, 0);
    for (uint32_t i = 0; i < bound; ++i) base[i] = i;
    uint32_t cur_func = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        const uint32_t *p = w.data() + i;
        auto ok = [&](uint32_t id) { return id < bound; };
        if (op == OpTypeFloat && wc >= 3 && ok(p[1])) is_float[p[1]] = 1;
        else if (op == OpTypeInt && wc == 4 && ok(p[1])) int_kind[p[1]] = uint8_t(1 + (p[3] ? 1 : 0));   // 1 unsigned, 2 signed
        else if (op == OpConstant && wc == 4 && ok(p[2])) cval[p[2]] = p[3];
        else if (op == OpVariable && wc >= 4 && ok(p[2])) sc[p[2]] = uint8_t(p[3] < 0xfe ? p[3] : 0xfe);
        else if (op == OpDecorate && wc >= 4 && ok(p[1]) && p[2] == DecBuiltIn && p[3] == BuiltInVertexIndex) vindex[p[1]] = 1;
        else if (op == OpDecorate && wc >= 4 && ok(p[1]) && p[2] == DecLocation) has_loc[p[1]] = 1;
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 4 && ok(p[2]) && ok(p[3])) base[p[2]] = base[p[3]];
        else if (op == OpFunction && wc >= 3 && ok(p[2])) cur_func = p[2];
        else if (op == OpFunctionEnd) cur_func = 0;
        else if (op == OpAtomicCompareExchange && cur_func) cas_func[cur_func] = 1;
        if ((op == OpLoad || op == OpBitcast || op == OpCopyObject || op == OpBitwiseAnd) && wc >= 4 && ok(p[1]) && ok(p[2])) type_of[p[2]] = p[1];
        i += wc;
    }
    // Taint: values carrying the index (vertex: from VertexIndex; geometry: from a located input), through bitcasts, copies,
    // "& 0xffff" (vertex) and Function variables (stored, then loaded).
    bool masked = false;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 5; i < n;) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
            if (op == OpLoad && wc >= 4 && p[2] < bound && p[3] < bound && !taint[p[2]]) {
                uint32_t b = base[p[3]];
                if ((stage == 0 && vindex[b]) || (stage == 3 && sc[b] == StorageInput && has_loc[b]) || tvar[p[3]]) { taint[p[2]] = 1; changed = true; }
            } else if (op == OpStore && wc >= 3 && p[1] < bound && p[2] < bound && taint[p[2]] && sc[p[1]] == StorageFunction && !tvar[p[1]]) { tvar[p[1]] = 1; changed = true; }
            else if ((op == OpBitcast || op == OpCopyObject) && wc == 4 && p[2] < bound && p[3] < bound && taint[p[3]] && !taint[p[2]]) { taint[p[2]] = 1; changed = true; }
            else if (stage == 0 && op == OpBitwiseAnd && wc == 5 && p[2] < bound && p[3] < bound && p[4] < bound && !taint[p[2]]
                     && ((taint[p[3]] && cval[p[4]] == 0xffffu) || (taint[p[4]] && cval[p[3]] == 0xffffu))) { taint[p[2]] = 1; masked = true; changed = true; }
            i += wc;
        }
    }
    std::vector<size_t> sites;
    if (stage == 0) {
        bool to_output = false;
        for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
            if (op == OpStore && wc >= 3 && p[1] < bound && p[2] < bound && taint[p[2]] && sc[base[p[1]]] == StorageOutput) to_output = true;
            if (op == OpBitcast && wc == 4 && p[3] < bound && taint[p[3]]) sites.push_back(i);
            i += wc; }
        if (!masked || !to_output) return 0;
    } else {
        std::vector<uint8_t> arg(bound, 0), feeds(bound, 0);   // variables handed to the index-insert function, values stored to them
        for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
            if (op == OpFunctionCall && wc >= 4 && p[3] < bound && cas_func[p[3]]) for (uint32_t k = 4; k < wc; ++k) if (p[k] < bound) arg[p[k]] = 1;
            i += wc; }
        for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
            if (op == OpStore && wc >= 3 && p[1] < bound && p[2] < bound && arg[p[1]]) feeds[p[2]] = 1;
            i += wc; }
        for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
            if (op == OpBitcast && wc == 4 && p[2] < bound && p[3] < bound && taint[p[3]] && feeds[p[2]]) sites.push_back(i);
            i += wc; }
    }
    size_t k = 0;
    for (size_t at : sites) {
        uint32_t *p = w.data() + at; const uint32_t to = p[1], from = p[3] < bound ? type_of[p[3]] : 0;
        if (to < bound && is_float[to] && from < bound && int_kind[from]) { p[0] = (4u << 16) | (int_kind[from] == 2 ? OpConvertSToF : OpConvertUToF); ++k; }
        else if (to < bound && int_kind[to] && from < bound && is_float[from]) { p[0] = (4u << 16) | (int_kind[to] == 2 ? OpConvertFToS : OpConvertFToU); ++k; }
    }
    return k;
}
// [DOOM] DM_GFX_GSCULL_FIX 0x1000. The 16-bit store emulation of the culling GS (the compare-exchange loop, %61 in
// 703f1fcc) keeps the OLD 32-bit word of the compacted index buffer in a float temporary (load uint -> bitcast float ->
// store; load float -> bitcast uint for the bit-field insert and the CAS comparator). That word is two 16-bit indices; with
// the upper index < 128 it is an fp32 denormal, with an upper index 0x7f80+ (or 0xff80+) a NaN. A driver move that flushes
// denormals or quiets NaNs would zero / alter the neighbouring index (or spin the loop when the comparator differs). The
// rewrite keeps such temporaries integer: in every function with OpAtomicCompareExchange, a Function float variable whose
// stores all come from "OpBitcast float <int>" and whose loads are only bitcast back to an integer becomes a uint variable
// (stores: copy / bitcast to uint, loads: uint, uses: copy / bitcast from uint). Same word counts, in place. Returns the
// number of variables retyped.
size_t xbg_spirv_cas_int_temps(std::vector<uint32_t> &w) {
    const size_t n = w.size();
    if (n < 5 || w[0] != 0x07230203u) return 0;
    const uint32_t bound = w[3];
    std::vector<uint8_t> is_float(bound, 0), int_kind(bound, 0), fptr(bound, 0);
    std::vector<uint32_t> type_of(bound, 0), func_of(bound, 0);
    std::vector<uint8_t> cas_func(bound, 0);
    uint32_t t_uint = 0, p_func_uint = 0, cur = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
        if (!wc || i + wc > n) return 0;
        if (op == OpTypeFloat && wc == 3 && p[1] < bound && p[2] == 32) is_float[p[1]] = 1;
        else if (op == OpTypeInt && wc == 4 && p[1] < bound && p[2] == 32) { int_kind[p[1]] = uint8_t(1 + (p[3] ? 1 : 0)); if (!p[3] && !t_uint) t_uint = p[1]; }
        else if (op == 32 && wc == 4 && p[1] < bound && p[2] == StorageFunction) { if (p[3] < bound && is_float[p[3]]) fptr[p[1]] = 1; if (p[3] == t_uint && t_uint && !p_func_uint) p_func_uint = p[1]; }
        else if (op == OpFunction && wc >= 3 && p[2] < bound) cur = p[2];
        else if (op == OpFunctionEnd) cur = 0;
        else if (op == OpAtomicCompareExchange && cur) cas_func[cur] = 1;
        if (cur && wc >= 3 && p[2] < bound && (op == OpVariable || op == OpLoad || op == OpBitcast || op == OpCopyObject)) { type_of[p[2]] = p[1]; func_of[p[2]] = cur; }
        i += wc;
    }
    if (!t_uint || !p_func_uint) return 0;
    // candidate variables: Function float variables of a CAS function
    std::vector<uint8_t> cand(bound, 0), bad(bound, 0), loaded(bound, 0);
    for (uint32_t id = 0; id < bound; ++id) if (func_of[id] && cas_func[func_of[id]] && type_of[id] < bound && fptr[type_of[id]]) cand[id] = 1;
    std::vector<uint32_t> load_var(bound, 0);
    std::vector<uint8_t> producer_ok(bound, 0);   // value = OpBitcast float <32-bit int>
    for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
        if (op == OpBitcast && wc == 4 && p[1] < bound && is_float[p[1]] && p[3] < bound && type_of[p[3]] < bound && int_kind[type_of[p[3]]]) producer_ok[p[2]] = 1;
        if (op == OpLoad && wc >= 4 && p[3] < bound && cand[p[3]]) { load_var[p[2]] = p[3]; loaded[p[2]] = 1; }
        i += wc; }
    // operand references per id (the stored bitcast may feed nothing but its store)
    std::vector<uint32_t> refs(bound, 0);
    for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
        const uint32_t res = (op == OpBitcast || op == OpLoad || op == OpCopyObject) && wc >= 3 ? 2 : 0;
        for (uint32_t k = 1; k < wc; ++k) if (k != res && p[k] < bound) ++refs[p[k]];
        i += wc; }
    // every reference of a candidate / of its loads must be one we can retype
    for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *p = w.data() + i;
        for (uint32_t k = 1; k < wc; ++k) { uint32_t id = p[k]; if (id >= bound) continue;
            if (cand[id]) {
                const bool ok = (op == OpVariable && k == 2) || (op == OpLoad && k == 3) || (op == OpStore && k == 1 && p[2] < bound && producer_ok[p[2]] && refs[p[2]] == 1) || (op == OpName && k == 1);
                if (!ok) bad[id] = 1;
            }
            if (loaded[id]) {
                const bool ok = (op == OpLoad && k == 2) || (op == OpBitcast && wc == 4 && k == 3 && p[1] < bound && int_kind[p[1]]);
                if (!ok) bad[load_var[id]] = 1;
            }
        }
        i += wc; }
    size_t k = 0; std::vector<uint8_t> done(bound, 0);
    for (uint32_t id = 0; id < bound; ++id) if (cand[id] && !bad[id]) done[id] = 1, ++k;
    if (!k) return 0;
    for (size_t i = 5; i < n;) { uint32_t wc = w[i] >> 16, op = w[i] & 0xffff; uint32_t *p = w.data() + i;
        if (op == OpVariable && wc >= 4 && p[2] < bound && done[p[2]]) p[1] = p_func_uint;
        else if (op == OpLoad && wc >= 4 && p[3] < bound && done[p[3]]) p[1] = t_uint;
        else if (op == OpStore && wc >= 3 && p[1] < bound && done[p[1]]) {
            // retype the producing bitcast (search backwards in the same block region; value ids are unique)
            const uint32_t v = p[2];
            for (size_t j = 5; j < n;) { uint32_t wj = w[j] >> 16; uint32_t *q = w.data() + j;
                if ((q[0] & 0xffff) == OpBitcast && wj == 4 && q[2] == v) { const bool src_uint = type_of[q[3]] == t_uint;
                    q[0] = (4u << 16) | (src_uint ? OpCopyObject : OpBitcast); q[1] = t_uint; break; }
                j += wj; }
        }
        else if (op == OpBitcast && wc == 4 && p[3] < bound && loaded[p[3]] && done[load_var[p[3]]] && p[1] == t_uint) p[0] = (4u << 16) | OpCopyObject;
        i += wc; }
    return k;
}
