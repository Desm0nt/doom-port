// 常量缓冲区按字寻址的分量下标折叠。翻译器把带计算地址的 Maxwell LDC 译成 (addr >> 2) >> 2 取 vec4、(addr >> 2) & 3 取
// 分量，驱动拿到的是动态分量下标。蒙皮着色器的骨骼地址是 骨骼号 × 48 加 0 / 8 / 16 / 24 / 32 / 40，低四位已知，分量其实都是
// 常量。这里沿乘、加、减、常量移位、位转换、整数转换、复制，以及翻译器只写一次的 Function 变量，追踪每个 32 位整数的低四位；
// 凡与常量掩码（< 16）相与、结果已完全确定的，就把这个结果的各处使用换成常量，与运算本身留作死代码。语义不变。之后再由
// xbg_spirv_temps.cpp 转发临时变量，常量才直接落进访问链；Redmi K50 Ultra 上两者合用，阴影图的蒙皮着色器快了 2.1 倍，单用其一
// 都测不出差别。
// Component-index folding for constant-buffer word addressing. The translator turns a Maxwell LDC with a computed address into
// (addr >> 2) >> 2 for the vec4 and (addr >> 2) & 3 for the component, so the driver sees a dynamic component index. A skinning
// shader's bone addresses are bone * 48 plus 0 / 8 / 16 / 24 / 32 / 40, so their low four bits are known and every component is
// in fact a constant. The low four bits of each 32-bit integer are tracked here through multiplies, adds, subtracts, constant
// shifts, bitcasts, integer conversions, copies and the translator's single-store Function variables. Where an & with a
// constant mask below 16 has a fully known result, its uses are replaced by that constant and the AND stays as dead code. The
// module computes exactly the same values. Only once xbg_spirv_temps.cpp then forwards the temporaries do the constants reach
// the access chains directly: on the Redmi K50 Ultra the two together made the shadow map's skinning shader 2.1x faster, and
// either alone changed nothing measurable.
#include "xbg_spirv_ldc.h"

namespace {
enum : uint32_t {
    OpTypeInt = 21, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyObject = 83,
    OpUConvert = 113, OpSConvert = 114, OpBitcast = 124, OpIAdd = 128, OpISub = 130, OpIMul = 132,
    OpShiftRightLogical = 194, OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseAnd = 199,
    StorageFunction = 7,
};
inline uint32_t inst(uint32_t op, uint32_t words) { return (words << 16) | op; }
// What is known of a 32-bit integer: its value modulo 2^bits (0-4 bits). An AND result marked 4 is known exactly (its mask is
// below 16).
struct Low { uint8_t bits = 0, value = 0; };
Low low(uint32_t v, uint32_t bits) { Low l; l.bits = uint8_t(bits > 4 ? 4 : bits); l.value = uint8_t(v & ((1u << l.bits) - 1)); return l; }
// Word w (1 = the first after the opcode) of a function-body instruction holds a literal, not an id.
bool literal_at(uint32_t op, uint32_t w) {
    switch (op) {
    case 8: return w >= 2;                       // OpLine: file id, then line / column
    case 12: return w == 4;                      // OpExtInst: the instruction number
    case OpVariable: return w == 3;              // storage class
    case OpLoad: return w >= 4;                  // memory operands
    case 62: case 63: return w >= 3;             // OpStore, OpCopyMemory: memory operands
    case 79: case 82: return w >= 5;             // OpVectorShuffle components, OpCompositeInsert indexes
    case 81: return w >= 4;                      // OpCompositeExtract indexes
    case 246: return w >= 3;                     // OpLoopMerge: loop control
    case 247: return w >= 2;                     // OpSelectionMerge: selection control
    case 250: return w >= 4;                     // OpBranchConditional: branch weights
    case 251: return w >= 3;                     // OpSwitch: case literals (and labels)
    // Image instructions: only the image-operands mask is a literal; the dref, gather component and operand values are ids.
    case 87: case 88: case 91: case 92: case 95: case 98: return w == 5;   // sample, fetch, read
    case 89: case 90: case 93: case 94: case 96: case 97: return w == 6;   // dref sample, gather, dref gather
    case 99: return w == 4;                      // OpImageWrite
    default: return false;
    }
}
}

namespace {
// Passes 1 and 2 of the fold (shared with the canonical-index rewrite below): 32-bit integer types and constants, the
// Function variables stored once, and the known low bits of every 32-bit integer.
struct Facts {
    uint32_t bound = 0; size_t first_func = 0;
    std::vector<uint8_t> int32, is_const, fvar, stores;
    std::vector<uint32_t> const_val, const_type, stored;
    std::vector<Low> known;
};
bool analyse(const uint32_t *in, size_t n, Facts &f) {
    if (!in || n < 5 || in[0] != 0x07230203u) return false;
    uint32_t bound = in[3]; f.bound = bound;
    f.int32.assign(bound, 0); f.is_const.assign(bound, 0); f.fvar.assign(bound, 0); f.stores.assign(bound, 0);
    f.const_val.assign(bound, 0); f.const_type.assign(bound, 0); f.stored.assign(bound, 0); f.known.assign(bound, Low{});
    auto &int32 = f.int32; auto &is_const = f.is_const; auto &fvar = f.fvar; auto &stores = f.stores;
    auto &const_val = f.const_val; auto &const_type = f.const_type; auto &stored = f.stored; auto &known = f.known;
    size_t first_func = 0;
    // Pass 1: 32-bit integer types and constants, Function variables and how often each is stored.
    for (size_t i = 5; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        if (op == OpFunction && !first_func) first_func = i;
        if (op == OpTypeInt && wc == 4 && in[i + 1] < bound && in[i + 2] == 32) int32[in[i + 1]] = 1;
        else if (op == OpConstant && wc == 4 && in[i + 1] < bound && in[i + 2] < bound && int32[in[i + 1]]) {
            uint32_t id = in[i + 2]; is_const[id] = 1; const_val[id] = in[i + 3]; const_type[id] = in[i + 1]; known[id] = low(in[i + 3], 4);
        }
        else if (op == OpVariable && wc >= 4 && in[i + 2] < bound && in[i + 3] == StorageFunction) {
            fvar[in[i + 2]] = 1;
            if (wc >= 5) stores[in[i + 2]] = 2;   // an initializer is a value before any store
        }
        i += wc;
    }
    if (!first_func) return false;
    // A Function variable is followed through only if every use in the bodies is a whole OpStore to it or an OpLoad of it, and it
    // is stored once: no access chain, copy or call can change it behind the tracked store.
    for (size_t i = first_func; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        for (uint32_t w = 1; w < wc; w++) {
            uint32_t id = in[i + w];
            if (id >= bound || !fvar[id] || literal_at(op, w)) continue;
            if (op == OpStore && w == 1) { if (stores[id] < 2) stores[id]++; stored[id] = in[i + 2]; }
            else if (!(op == OpLoad && w == 3) && !(op == OpVariable && w == 2)) stores[id] = 2;
        }
        i += wc;
    }
    // Pass 2: propagate known low bits until nothing changes (a load may come before its variable's store in binary order).
    auto known_of = [&](uint32_t id) { return id < bound ? known[id] : Low{}; };
    for (int round = 0; round < 8; round++) {
        bool changed = false;
        for (size_t i = first_func; i < n;) {
            uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
            if (wc >= 4 && in[i + 1] < bound && in[i + 2] < bound && int32[in[i + 1]] && known[in[i + 2]].bits < 4) {
                uint32_t r = in[i + 2];
                Low a = known_of(in[i + 3]), b = wc >= 5 ? known_of(in[i + 4]) : Low{}, res = known[r];
                auto shift = [&](uint32_t id, uint32_t &s) { if (wc != 5 || id >= bound || !is_const[id] || const_val[id] >= 32) return false; s = const_val[id]; return true; };
                uint32_t s = 0;
                switch (op) {
                case OpBitcast: case OpUConvert: case OpSConvert: case OpCopyObject: if (wc == 4) res = a; break;
                case OpLoad: { uint32_t v = in[i + 3]; if (v < bound && fvar[v] && stores[v] == 1) res = known_of(stored[v]); break; }
                case OpIAdd: case OpISub: if (wc == 5) res = low(op == OpIAdd ? uint32_t(a.value) + b.value : uint32_t(a.value) - b.value, a.bits < b.bits ? a.bits : b.bits); break;
                case OpIMul:
                    if (wc == 5) {
                        Low p = low(uint32_t(a.value) * b.value, a.bits < b.bits ? a.bits : b.bits);
                        // A constant factor with z trailing zero bits makes the product's low z bits zero, whatever the other.
                        for (uint32_t c : {in[i + 3], in[i + 4]}) if (c < bound && is_const[c]) {
                            uint32_t z = 0; while (z < 4 && !(const_val[c] & (1u << z))) z++;
                            if (z > p.bits) p = low(0, z);
                        }
                        res = p;
                    }
                    break;
                case OpShiftRightLogical: case OpShiftRightArithmetic: if (shift(in[i + 4], s) && a.bits > s) res = low(uint32_t(a.value) >> s, a.bits - s); break;
                case OpShiftLeftLogical: if (shift(in[i + 4], s)) res = low(uint32_t(a.value) << s, a.bits + s); break;
                case OpBitwiseAnd:
                    if (wc == 5) for (int side = 0; side < 2; side++) {
                        uint32_t c = in[i + 4 - side], x = in[i + 3 + side];
                        if (c < bound && is_const[c] && const_val[c] < 16) {
                            uint32_t need = 0; while (need < 4 && (const_val[c] >> need)) need++;
                            if (known_of(x).bits >= need) res = low(known_of(x).value & const_val[c], 4);
                            break;
                        }
                    }
                    break;
                default: break;
                }
                if (res.bits > known[r].bits) { known[r] = res; changed = true; }
            }
            i += wc;
        }
        if (!changed) break;
    }
    f.first_func = first_func;
    return true;
}
}

size_t xbg_spirv_ldc_fold(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear();
    Facts f;
    if (!analyse(in, n, f)) return 0;
    uint32_t bound = f.bound; const size_t first_func = f.first_func;
    const auto &int32 = f.int32; const auto &is_const = f.is_const; const auto &const_val = f.const_val; const auto &const_type = f.const_type;
    const auto &known = f.known;
    // Pass 3: the ANDs to fold, and a constant of each one's type for its value (an existing one, else a new one).
    std::vector<uint32_t> fold(bound, 0), decl;
    size_t folded = 0;
    for (size_t i = first_func; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (op == OpBitwiseAnd && wc == 5 && in[i + 1] < bound && in[i + 2] < bound && int32[in[i + 1]] && known[in[i + 2]].bits == 4) {
            uint32_t t = in[i + 1], v = known[in[i + 2]].value, c = 0;
            for (uint32_t id = 0; id < bound && !c; id++) if (is_const[id] && const_type[id] == t && const_val[id] == v) c = id;
            for (size_t k = 0; !c && k + 3 < decl.size(); k += 4) if (decl[k + 1] == t && decl[k + 3] == v) c = decl[k + 2];
            if (!c) { c = bound++; decl.insert(decl.end(), {inst(OpConstant, 4), t, c, v}); }
            fold[in[i + 2]] = c; folded++;
        }
        i += wc;
    }
    if (!folded) return 0;
    // Write the module: the new constants go before the first function; uses of a folded AND read the constant.
    out.reserve(n + decl.size());
    out.insert(out.end(), in, in + first_func);
    out[3] = bound;
    out.insert(out.end(), decl.begin(), decl.end());
    for (size_t i = first_func; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        size_t at = out.size();
        out.insert(out.end(), in + i, in + i + wc);
        for (uint32_t w = 1; w < wc; w++) {
            uint32_t id = in[i + w];
            if (id >= bound || id >= fold.size() || !fold[id] || literal_at(op, w) || (op == OpBitwiseAnd && w == 2)) continue;
            out[at + w] = fold[id];
        }
        i += wc;
    }
    return folded;
}

// [r3] DM_GFX_LDC_CANON (pipeline_audit.md 3.8): canonical vec4 index of constant-buffer word addressing. After the fold above
// the component is constant, but the vec4 index of each LDC is its own SSA value: ((x + K) >> 2) >> 2 for the low word and
// (((x + K) >> 2) + 1) >> 2 for the high word of an LDC.64, with K = 0 / 8 / 16 / ... per matrix row. NIR merges loads of
// one vec4 only when their index is the same value, so a skinning shader issued two to four ldc per bone row. Two rewrites,
// both exact for every input (unsigned 32-bit arithmetic):
//  A. (B + k) >> 2 with the low two bits r of B known and r + k <= 3 is B >> 2 (adding k cannot carry out of the low bits).
//  B. (x + K) >> 4 (or ((x + K) >> 2) >> 2), x reached from the index through IAdd / ISub by constants and known to be a
//     multiple of 16, is (x + (K & ~15)) >> 4, and just x >> 4 when K < 16: x + (K & ~15) is a multiple of 16 modulo 2^32
//     too, so adding the remaining K & 15 < 16 carries into no bit at or above bit 4 and cannot wrap.
// The index of every LDC of one matrix row then reads one value (x >> 4, or (x + 16c) >> 4), which NIR CSEs, so the loads
// of that vec4 merge.
// Only shifts that feed an access-chain index (through bitcasts) are rewritten.
namespace {
bool body_result(uint32_t op) {   // function-body instructions with a result type and id at words 1 / 2
    switch (op) {
    case 8: case 56: case 62: case 63: case 64: case 99: case 218: case 219: case 220: case 221: case 224: case 225: case 228:
    case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 255: case 256: case 257:
    case 317: case 4416: case 5380: return false;
    default: return true;
    }
}
}
size_t xbg_spirv_ldc_canon(const uint32_t *in, size_t n, std::vector<uint32_t> &out, size_t *merges) {
    out.clear();
    if (merges) *merges = 0;
    Facts f;
    if (!analyse(in, n, f)) return 0;
    const uint32_t bound0 = f.bound; uint32_t bound = bound0;
    std::vector<size_t> def(bound0, 0); std::vector<uint32_t> type(bound0, 0); std::vector<uint8_t> index_use(bound0, 0);
    for (size_t i = 5; i < f.first_func;) { uint32_t wc = in[i] >> 16, op = in[i] & 0xffff; if (op == OpConstant && wc == 4 && in[i + 2] < bound0) type[in[i + 2]] = in[i + 1]; i += wc; }
    std::vector<size_t> copies;   // offsets of Bitcast / CopyObject in the bodies
    for (size_t i = f.first_func; i < n;) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (wc >= 3 && body_result(op) && in[i + 2] < bound0) { def[in[i + 2]] = i + 1; type[in[i + 2]] = in[i + 1]; }
        if ((op == 65 || op == 66) && wc >= 5) for (uint32_t w = 4; w < wc; w++) if (in[i + w] < bound0) index_use[in[i + w]] = 1;   // OpAccessChain indexes
        if ((op == OpBitcast || op == OpCopyObject) && wc == 4) copies.push_back(i);
        i += wc;
    }
    for (int round = 0; round < 6; round++) {
        bool changed = false;
        for (size_t k = copies.size(); k-- > 0;) { size_t i = copies[k]; if (in[i + 2] < bound0 && index_use[in[i + 2]] && in[i + 3] < bound0 && !index_use[in[i + 3]]) { index_use[in[i + 3]] = 1; changed = true; } }
        if (!changed) break;
    }
    auto is_int = [&](uint32_t id) { return id < bound0 && type[id] < bound0 && f.int32[type[id]]; };
    auto at = [&](uint32_t id, uint32_t op, uint32_t wc) -> const uint32_t * { if (id >= bound0 || !def[id]) return nullptr; const uint32_t *d = in + def[id] - 1; return (d[0] & 0xffff) == op && (d[0] >> 16) == wc ? d : nullptr; };
    auto strip = [&](uint32_t id) { for (int g = 0; g < 16; g++) { const uint32_t *d = at(id, OpBitcast, 4); if (!d) d = at(id, OpCopyObject, 4); if (!d || !is_int(d[3])) break; id = d[3]; } return id; };
    auto cst = [&](uint32_t id, uint32_t &v) { if (id >= bound0 || !f.is_const[id]) return false; v = f.const_val[id]; return true; };
    auto aligned16 = [&](uint32_t id) { return id < bound0 && f.known[id].bits == 4 && f.known[id].value == 0; };
    std::vector<uint32_t> decl;
    auto constant = [&](uint32_t t, uint32_t v) {
        for (uint32_t id = 0; id < bound0; id++) if (f.is_const[id] && f.const_type[id] == t && f.const_val[id] == v) return id;
        for (size_t k = 0; k + 3 < decl.size(); k += 4) if (decl[k + 1] == t && decl[k + 3] == v) return decl[k + 2];
        uint32_t c = bound++; decl.insert(decl.end(), {inst(OpConstant, 4), t, c, v}); return c;
    };
    std::vector<std::pair<size_t, std::vector<uint32_t>>> edits;   // offset -> words replacing that instruction (in order)
    size_t rule_a = 0;
    for (size_t i = f.first_func; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        uint32_t s = 0;
        if (op == OpShiftRightLogical && wc == 5 && in[i + 2] < bound0 && index_use[in[i + 2]] && is_int(in[i + 2]) && cst(in[i + 4], s) && (s == 2 || s == 4)) {
            const uint32_t t = in[i + 1], v = in[i + 2];
            uint32_t operand = in[i + 3], z = 0; bool a_done = false;
            if (s == 2) {
                const uint32_t *add = at(strip(operand), OpIAdd, 5);
                if (add) for (int side = 0; side < 2; side++) {
                    uint32_t k, b = add[4 - side];
                    if (!cst(add[3 + side], k) || k > 3 || !is_int(b) || f.known[b].bits < 2 || (f.known[b].value & 3u) + k > 3) continue;
                    operand = b; a_done = true; break;
                }
                const uint32_t *inner = at(strip(operand), OpShiftRightLogical, 5); uint32_t s2;
                if (inner && cst(inner[4], s2) && s2 == 2 && is_int(inner[3])) z = inner[3];
            } else z = operand;
            // Rule B: walk to the deepest multiple of 16 the index adds constants to.
            uint32_t base = 0, K = 0;
            if (z) {
                uint32_t cur = strip(z), acc = 0;
                if (aligned16(cur)) base = cur;
                for (int g = 0; g < 16; g++) {
                    const uint32_t *d = at(cur, OpIAdd, 5); uint32_t c = 0, o = 0;
                    if (d) { if (cst(d[4], c)) o = d[3]; else if (cst(d[3], c)) o = d[4]; }
                    else if ((d = at(cur, OpISub, 5)) && cst(d[4], c)) { o = d[3]; c = 0u - c; }
                    if (!o || !is_int(o)) break;
                    cur = strip(o); acc += c;
                    if (aligned16(cur)) { base = cur; K = acc; }
                }
            }
            std::vector<uint32_t> w;
            if (base && !(s == 4 && K == 0 && strip(operand) == base)) {
                const uint32_t row = K & ~15u, c4 = constant(t, 4);
                if (!row) w = {inst(OpShiftRightLogical, 5), t, v, base, c4};
                else { const uint32_t u = bound++; w = {inst(OpIAdd, 5), t, u, base, constant(t, row), inst(OpShiftRightLogical, 5), t, v, u, c4}; }
                if (a_done) rule_a++;
            } else if (a_done) { w = {inst(OpShiftRightLogical, 5), t, v, operand, in[i + 4]}; rule_a++; }
            if (!w.empty()) edits.push_back({i, std::move(w)});
        }
        i += wc;
    }
    if (edits.empty()) return 0;
    if (merges) *merges = rule_a;
    out.reserve(n + decl.size() + edits.size() * 12);
    out.insert(out.end(), in, in + f.first_func);
    out[3] = bound;
    out.insert(out.end(), decl.begin(), decl.end());
    size_t e = 0;
    for (size_t i = f.first_func; i < n;) {
        const uint32_t wc = in[i] >> 16;
        if (e < edits.size() && edits[e].first == i) { out.insert(out.end(), edits[e].second.begin(), edits[e].second.end()); e++; }
        else out.insert(out.end(), in + i, in + i + wc);
        i += wc;
    }
    return edits.size();
}
