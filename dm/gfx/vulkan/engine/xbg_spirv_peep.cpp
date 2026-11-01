// Round-3 peepholes of the translator's SPIR-V (docs/research/pipeline_audit.md section 5 and round 3, steps 3e / 3f). All
// but the texture-coordinate one compute exactly the values the module computed before; each is off unless its switch is
// set (xbg_game.cpp tidy_rewrite / load_shader), so without the switches no module changes.
//
// - halfflag: Maxwell keeps per-lane booleans of HSET2 as fp16 halves. The translator turns `if (c) flag = 1.0h` into a
//   Function variable stored before the if (low half known to be zero: (x >> 16) << 16) and, in the then-block, stored
//   PackHalf2x16(vec2(1.0, unpack(..).y)); after the merge it tests FOrdEqual(unpack(load).x, 0.0). The test is true exactly
//   when the low half is +-0. Each value that can reach the load is followed: zero-low values (shift left by >= 16, AND with
//   a mask whose low 15 bits are 0, constant 0) give true, PackHalf2x16 of a constant whose half is normal or infinite gives
//   false, Select / OR / bitcasts / single-store variables combine them, and the if-shape variable (exactly two stores: one in
//   a block B0 that dominates the branch block A and is passed again before A is re-entered from the then-block, one in the
//   then-block T whose only predecessor is A and which branches straight to the merge) gives c ? Z(T) : Z(B0). No value in
//   the chain can hold a denormal or NaN half, so a driver that flushes fp16 denormals still agrees. The comparison becomes
//   that boolean (LogicalNot / And / Or of the branch conditions); the unpack, and with it the pack chain, goes dead.
// - f2i: F2I.TRUNC is emitted as Trunc -> FMax(.., 0.0) -> ConvertFToU. ConvertFToU / ConvertFToS round toward zero, and a
//   clamp to integral bounds commutes with truncation as far as the converted integer goes (x inside [lo, hi] keeps
//   trunc(x) inside it; outside, both clamp to the same bound; NaN reaches the clamp either way), so the chain reads x.
// - pointsize: Ryujinx stores gl_PointSize = 1.0 in every vertex shader; for lines and triangles the value is never used.
// - texcoord: Ryujinx guards every float texture coordinate with Select(IsNan(c), 0, c). Dropping it changes only what a NaN
//   coordinate samples (Vulkan leaves that to the implementation), so it is an opt-in A/B (DM_GFX_TEXCOORD_NAN=0).
#include "xbg_spirv_peep.h"
#include <cstring>

namespace {
enum : uint32_t {
    OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeStruct = 30,
    OpTypePointer = 32, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpDecorate = 71, OpMemberDecorate = 72, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCopyObject = 83,
    OpConvertFToU = 109, OpConvertFToS = 110, OpBitcast = 124, OpIsNan = 156, OpLogicalOr = 166, OpLogicalAnd = 167,
    OpLogicalNot = 168, OpSelect = 169, OpFOrdEqual = 180, OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseAnd = 199,
    OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251,
    StorageOutput = 3, StorageFunction = 7, DecorationBuiltIn = 11, BuiltInPointSize = 1,
    GlslTrunc = 3, GlslFMin = 37, GlslFMax = 40, GlslFClamp = 43, GlslPackHalf2x16 = 58, GlslUnpackHalf2x16 = 62,
    GlslNMin = 79, GlslNMax = 80, GlslNClamp = 81,
};
inline uint32_t inst(uint32_t op, uint32_t words) { return (words << 16) | op; }
// Where an instruction's result id sits: 0 none, 1 word 1 (types, labels, imports), 2 word 2 (after the result type).
int result_pos(uint32_t op) {
    if (op == 7 || op == OpExtInstImport || (op >= 19 && op <= 38) || op == 73 || op == OpLabel) return 1;
    switch (op) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 8: case 10: case 14: case 15: case 16: case 17: case 39: case 56:
    case 62: case 63: case 64: case 71: case 72: case 74: case 75: case 99: case 218: case 219: case 220: case 221: case 224:
    case 225: case 228: case 246: case 247: case 249: case 250: case 251: case 252: case 253: case 254: case 255: case 256:
    case 257: case 317: case 330: case 331: case 332: case 4416: case 5380: case 5632: case 5633:
        return 0;
    default: return 2;
    }
}
// Word w (1 = the first after the opcode) of a function-body instruction holds a literal, not an id (as xbg_spirv_ldc.cpp;
// anything counted as an id use too many only makes a rewrite less likely).
bool literal_at(uint32_t op, uint32_t w) {
    switch (op) {
    case 8: return w >= 2;
    case 12: return w == 4;
    case OpVariable: return w == 3;
    case OpLoad: return w >= 4;
    case 62: case 63: return w >= 3;
    case 79: case 82: return w >= 5;
    case 81: return w >= 4;
    case 246: return w >= 3;
    case 247: return w >= 2;
    case 250: return w >= 4;
    case 251: return w >= 3;
    case 87: case 88: case 91: case 92: case 95: case 98: return w == 5;
    case 89: case 90: case 93: case 94: case 96: case 97: return w == 6;
    case 99: return w == 4;
    default: return false;
    }
}
struct Spv {
    const uint32_t *w = nullptr; size_t n = 0; uint32_t bound = 0, glsl = 0; size_t first_func = 0;
    std::vector<size_t> at;            // instruction offsets
    std::vector<uint32_t> def;         // id -> instruction index + 1
    std::vector<uint32_t> block;       // instruction index -> label of its block (0 outside)
    std::vector<uint32_t> uses;        // id -> uses as an operand inside function bodies
    bool parse(const uint32_t *words, size_t count) {
        w = words; n = count;
        if (!w || n < 5 || w[0] != 0x07230203u) return false;
        bound = w[3]; def.assign(bound, 0); uses.assign(bound, 0);
        uint32_t cur = 0;
        for (size_t i = 5; i < n;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
            if (!wc || i + wc > n) return false;
            const size_t k = at.size(); at.push_back(i);
            if (op == OpFunction && !first_func) first_func = i;
            if (op == OpLabel && wc >= 2) cur = w[i + 1];
            if (op == OpFunctionEnd) cur = 0;
            block.push_back(cur);
            const int rp = result_pos(op);
            if (rp && uint32_t(rp) < wc && w[i + rp] < bound) def[w[i + rp]] = uint32_t(k + 1);
            if (op == OpExtInstImport && wc >= 3 && !memcmp(&w[i + 2], "GLSL.std.450", 13)) glsl = w[i + 1];
            if (first_func)
                for (uint32_t x = 1; x < wc; x++) {
                    if (literal_at(op, x) || (rp && (x == uint32_t(rp) || (rp == 2 && x == 1)))) continue;
                    if (w[i + x] < bound) uses[w[i + x]]++;
                }
            i += wc;
        }
        return first_func != 0;
    }
    size_t count() const { return at.size(); }
    const uint32_t *ins(size_t k) const { return w + at[k]; }
    uint32_t op(size_t k) const { return w[at[k]] & 0xffff; }
    uint32_t wc(size_t k) const { return w[at[k]] >> 16; }
    // Defining instruction of id (index), or SIZE_MAX.
    size_t def_index(uint32_t id) const { return id < bound && def[id] ? def[id] - 1 : SIZE_MAX; }
    const uint32_t *def_of(uint32_t id) const { size_t k = def_index(id); return k == SIZE_MAX ? nullptr : ins(k); }
    uint32_t def_op(uint32_t id) const { const uint32_t *d = def_of(id); return d ? d[0] & 0xffff : 0; }
    bool is_glsl(const uint32_t *d, uint32_t e) const { return d && (d[0] & 0xffff) == OpExtInst && (d[0] >> 16) >= 5 && glsl && d[3] == glsl && d[4] == e; }
    // A 32-bit scalar OpConstant: its bits.
    bool const32(uint32_t id, uint32_t &bits) const { const uint32_t *d = def_of(id); if (!d || (d[0] & 0xffff) != OpConstant || (d[0] >> 16) != 4) return false; bits = d[3]; return true; }
    // A scalar float constant equal to an integer (or an infinity).
    bool integral_float(uint32_t id) const {
        uint32_t b; if (!const32(id, b)) return false;
        const uint32_t *t = def_of(def_of(id)[1]); if (!t || (t[0] & 0xffff) != OpTypeFloat || t[2] != 32) return false;
        float f; memcpy(&f, &b, 4); if (f != f) return false;
        if (f > 1e30f || f < -1e30f) return true;
        return float(int64_t(f)) == f;
    }
};
// Writes the module with per-instruction replacements / insertions; new declarations go before the first function.
struct Edit {
    const Spv &s; std::vector<std::vector<uint32_t>> before, repl; std::vector<uint8_t> replaced; std::vector<uint32_t> decl; uint32_t bound;
    explicit Edit(const Spv &m) : s(m), before(m.count()), repl(m.count()), replaced(m.count(), 0), bound(m.bound) {}
    void replace(size_t k, std::vector<uint32_t> words) { repl[k] = std::move(words); replaced[k] = 1; }
    void drop(size_t k) { repl[k].clear(); replaced[k] = 1; }
    uint32_t fresh() { return bound++; }
    void write(std::vector<uint32_t> &out) const {
        out.clear(); out.reserve(s.n + decl.size() + 64);
        out.insert(out.end(), s.w, s.w + s.first_func); out[3] = bound;
        out.insert(out.end(), decl.begin(), decl.end());
        for (size_t k = 0; k < s.count(); k++) {
            if (s.at[k] < s.first_func) continue;
            out.insert(out.end(), before[k].begin(), before[k].end());
            if (replaced[k]) out.insert(out.end(), repl[k].begin(), repl[k].end());
            else out.insert(out.end(), s.ins(k), s.ins(k) + s.wc(k));
        }
    }
};
// ---- halfflag ----
struct Cfg {
    // per label: successors, predecessors, idom (label), function start label
    std::vector<std::vector<uint32_t>> succ, pred; std::vector<uint32_t> idom, term, entry;
    void build(const Spv &s) {
        succ.assign(s.bound, {}); pred.assign(s.bound, {}); idom.assign(s.bound, 0); term.assign(s.bound, UINT32_MAX); entry.assign(s.bound, 0);
        std::vector<uint32_t> labels; uint32_t fentry = 0;
        auto finish = [&]() {   // dominators of one function (Cooper, Harvey, Kennedy), over its reverse post-order
            if (labels.empty()) return;
            std::vector<uint32_t> order, rpo_index(0); std::vector<uint8_t> seen(s.bound, 0);
            std::vector<std::pair<uint32_t, size_t>> stack{{fentry, 0}}; seen[fentry] = 1;
            while (!stack.empty()) {
                auto &top = stack.back();
                if (top.second < succ[top.first].size()) { uint32_t nx = succ[top.first][top.second++]; if (nx < s.bound && !seen[nx]) { seen[nx] = 1; stack.push_back({nx, 0}); } }
                else { order.push_back(top.first); stack.pop_back(); }
            }
            std::vector<uint32_t> po(s.bound, UINT32_MAX); for (size_t i = 0; i < order.size(); i++) po[order[i]] = uint32_t(i);
            idom[fentry] = fentry;
            for (bool changed = true; changed;) {
                changed = false;
                for (size_t i = order.size(); i-- > 0;) {
                    uint32_t b = order[i]; if (b == fentry) continue;
                    uint32_t nd = 0;
                    for (uint32_t p : pred[b]) {
                        if (p >= s.bound || po[p] == UINT32_MAX || !idom[p]) continue;
                        if (!nd) { nd = p; continue; }
                        uint32_t a = p, c = nd;
                        while (a != c) { while (po[a] < po[c]) a = idom[a]; while (po[c] < po[a]) c = idom[c]; }
                        nd = a;
                    }
                    if (nd && idom[b] != nd) { idom[b] = nd; changed = true; }
                }
            }
            for (uint32_t l : labels) entry[l] = fentry;
            labels.clear();
        };
        uint32_t cur = 0;
        for (size_t k = 0; k < s.count(); k++) {
            const uint32_t op = s.op(k), *d = s.ins(k), wc = s.wc(k);
            if (op == OpFunction) { labels.clear(); fentry = 0; }
            if (op == OpLabel) { cur = d[1]; labels.push_back(cur); if (!fentry) fentry = cur; }
            auto edge = [&](uint32_t to) { if (cur < s.bound && to < s.bound) { succ[cur].push_back(to); pred[to].push_back(cur); } };
            if (op == OpBranch && wc >= 2) { edge(d[1]); term[cur] = uint32_t(k); }
            else if (op == OpBranchConditional && wc >= 4) { edge(d[2]); edge(d[3]); term[cur] = uint32_t(k); }
            else if (op == OpSwitch && wc >= 3) { edge(d[2]); for (uint32_t x = 4; x < wc; x += 2) edge(d[x]); term[cur] = uint32_t(k); }
            else if (op >= 252 && op <= 255) term[cur] = uint32_t(k);
            if (op == OpFunctionEnd) finish();
        }
    }
    bool dominates(uint32_t a, uint32_t b) const {   // a dominates b (same function)
        if (a >= idom.size() || b >= idom.size() || !idom[b]) return false;
        for (int guard = 0; guard < 100000; guard++) { if (b == a) return true; uint32_t up = idom[b]; if (!up || up == b) return false; b = up; }
        return false;
    }
    // Can `to` be reached from `from` (inclusive) without entering `avoid`?
    bool reach_avoiding(uint32_t from, uint32_t to, uint32_t avoid, size_t bound) const {
        std::vector<uint8_t> seen(bound, 0); std::vector<uint32_t> stack;
        if (from == avoid) return false;
        stack.push_back(from); seen[from] = 1;
        while (!stack.empty()) {
            uint32_t b = stack.back(); stack.pop_back(); if (b == to) return true;
            for (uint32_t nx : succ[b]) if (nx < bound && !seen[nx] && nx != avoid) { seen[nx] = 1; stack.push_back(nx); }
        }
        return false;
    }
};
struct Half {
    const Spv &s; const Cfg &g; Edit &e;
    std::vector<uint8_t> fvar, other_use; std::vector<std::vector<size_t>> stores;   // Function variables: their whole-variable stores
    uint32_t tbool = 0, ctrue = 0, cfalse = 0;
    std::vector<uint32_t> emit;   // instructions for the current comparison, inserted before it
    int budget = 0;               // values followed for the current comparison (OR / Select branch both ways)
    enum Kind { Fail, False, True, Ssa };
    struct Z { Kind k; uint32_t id; };
    Half(const Spv &m, const Cfg &c, Edit &ed) : s(m), g(c), e(ed), fvar(m.bound, 0), other_use(m.bound, 0), stores(m.bound) {
        for (size_t k = 0; k < s.count(); k++) {
            const uint32_t op = s.op(k), *d = s.ins(k), wc = s.wc(k);
            if (op == OpVariable && wc >= 4 && d[3] == StorageFunction && d[2] < s.bound) { fvar[d[2]] = 1; if (wc >= 5) other_use[d[2]] = 1; }
        }
        for (size_t k = 0; k < s.count(); k++) {
            const uint32_t op = s.op(k), *d = s.ins(k), wc = s.wc(k);
            if (s.at[k] < s.first_func) continue;
            for (uint32_t x = 1; x < wc; x++) {
                uint32_t id = d[x]; if (id >= s.bound || !fvar[id] || literal_at(op, x)) continue;
                if (op == OpStore && x == 1) stores[id].push_back(k);
                else if (!(op == OpLoad && x == 3) && !(op == OpVariable && x == 2)) other_use[id] = 1;
            }
        }
    }
    uint32_t cbool(bool v) {
        uint32_t &c = v ? ctrue : cfalse; if (c) return c;
        for (size_t k = 0; k < s.count() && s.at[k] < s.first_func; k++)
            if (s.op(k) == (v ? OpConstantTrue : OpConstantFalse) && s.wc(k) == 3 && s.ins(k)[1] == tbool) return c = s.ins(k)[2];
        c = e.fresh(); e.decl.insert(e.decl.end(), {inst(v ? OpConstantTrue : OpConstantFalse, 3), tbool, c}); return c;
    }
    uint32_t id_of(Z z) { return z.k == Ssa ? z.id : cbool(z.k == True); }
    Z mk(uint32_t op, std::initializer_list<uint32_t> args) {
        uint32_t id = e.fresh(); emit.push_back(inst(op, uint32_t(3 + args.size()))); emit.push_back(tbool); emit.push_back(id);
        emit.insert(emit.end(), args); return {Ssa, id};
    }
    Z lnot(Z a) { if (a.k == True) return {False, 0}; if (a.k == False) return {True, 0}; return mk(OpLogicalNot, {a.id}); }
    Z land(Z a, Z b) { if (a.k == False || b.k == False) return {False, 0}; if (a.k == True) return b; if (b.k == True) return a; return mk(OpLogicalAnd, {a.id, b.id}); }
    Z lor(Z a, Z b) { if (a.k == True || b.k == True) return {True, 0}; if (a.k == False) return b; if (b.k == False) return a; return mk(OpLogicalOr, {a.id, b.id}); }
    Z sel(uint32_t c, Z a, Z b) {   // c ? a : b
        Z cz{Ssa, c};
        if (a.k != Ssa && a.k == b.k) return a;
        if (a.k == True && b.k == False) return cz;
        if (a.k == False && b.k == True) return lnot(cz);
        if (a.k == False) return land(lnot(cz), b);
        if (a.k == True) return lor(cz, b);
        if (b.k == False) return land(cz, a);
        if (b.k == True) return lor(lnot(cz), a);
        return mk(OpSelect, {c, a.id, b.id});
    }
    // The low half of a 16-bit pattern: +-0 -> True, normal or infinite -> False, denormal / NaN -> Fail.
    static Z half_class(uint32_t h) {
        h &= 0x7fff; if (!h) return {True, 0};
        uint32_t ex = h >> 10; if (ex == 0) return {Fail, 0}; if (ex == 31 && (h & 0x3ff)) return {Fail, 0};
        return {False, 0};
    }
    // Low half of value v: is it +-0 (FOrdEqual(unpack(v).x, 0.0))? Valid at any point v dominates.
    Z low_zero(uint32_t v, int depth) {
        if (depth > 32 || --budget < 0) return {Fail, 0};
        const uint32_t *d = s.def_of(v); if (!d) return {Fail, 0};
        const uint32_t op = d[0] & 0xffff, wc = d[0] >> 16;
        uint32_t c;
        switch (op) {
        case OpConstant: return wc == 4 ? half_class(d[3] & 0xffff) : Z{Fail, 0};
        case OpConstantNull: return {True, 0};
        case OpBitcast: case OpCopyObject: return wc == 4 ? low_zero(d[3], depth + 1) : Z{Fail, 0};
        case OpShiftLeftLogical: return wc == 5 && s.const32(d[4], c) && c >= 16 && c < 32 ? Z{True, 0} : Z{Fail, 0};
        case OpBitwiseAnd:
            if (wc == 5 && ((s.const32(d[3], c) && !(c & 0x7fff)) || (s.const32(d[4], c) && !(c & 0x7fff)))) return {True, 0};
            return {Fail, 0};
        case OpBitwiseOr: {
            if (wc != 5) return {Fail, 0};
            Z a = low_zero(d[3], depth + 1); if (a.k == Fail) return a;
            Z b = low_zero(d[4], depth + 1); if (b.k == Fail) return b;
            return land(a, b);
        }
        case OpSelect: {
            if (wc != 6) return {Fail, 0};
            Z a = low_zero(d[4], depth + 1); if (a.k == Fail) return a;
            Z b = low_zero(d[5], depth + 1); if (b.k == Fail) return b;
            return sel(d[3], a, b);
        }
        case OpExtInst: {
            if (!s.is_glsl(d, GlslPackHalf2x16) || wc != 6) return {Fail, 0};
            const uint32_t *v2 = s.def_of(d[5]); if (!v2) return {Fail, 0};
            uint32_t xop = v2[0] & 0xffff, x;
            if ((xop == OpCompositeConstruct || xop == OpConstantComposite) && (v2[0] >> 16) == 5) x = v2[3]; else return {Fail, 0};
            uint32_t b; if (!s.const32(x, b)) return {Fail, 0};
            if (!(b & 0x7fffffffu)) return {True, 0};
            const uint32_t ex = (b >> 23) & 0xff;
            if (ex == 0xff && (b & 0x7fffff)) return {Fail, 0};   // NaN
            if (ex >= 127 - 14) return {False, 0};                 // |K| >= 2^-14: a normal half (or infinity / max after rounding)
            return {Fail, 0};
        }
        case OpLoad: return load_zero(s.def_index(v), d, depth);
        default: return {Fail, 0};
        }
    }
    Z load_zero(size_t lk, const uint32_t *d, int depth) {
        if ((d[0] >> 16) != 4) return {Fail, 0};
        const uint32_t var = d[3]; if (var >= s.bound || !fvar[var] || other_use[var]) return {Fail, 0};
        const uint32_t lb = s.block[lk]; const auto &st = stores[var];
        if (st.size() == 1) {
            const size_t sk = st[0]; const uint32_t sb = s.block[sk];
            if (!((sb == lb && sk < lk) || (sb != lb && g.dominates(sb, lb)))) return {Fail, 0};
            return low_zero(s.ins(sk)[2], depth + 1);
        }
        if (st.size() != 2 || lb >= s.bound) return {Fail, 0};
        // If-shape: the load's block M has exactly the predecessors A (branch) and T (then-block, only predecessor A, branches to M).
        const auto &pm = g.pred[lb]; if (pm.size() != 2) return {Fail, 0};
        for (int side = 0; side < 2; side++) {
            const uint32_t a = pm[side], t = pm[1 - side];
            if (a == t || a == lb || t == lb || g.pred[t].size() != 1 || g.pred[t][0] != a) continue;
            if (g.term[t] == UINT32_MAX || s.op(g.term[t]) != OpBranch || s.ins(g.term[t])[1] != lb) continue;
            if (g.term[a] == UINT32_MAX || s.op(g.term[a]) != OpBranchConditional) continue;
            const uint32_t *br = s.ins(g.term[a]); const uint32_t cond = br[1];
            bool taken_if_true;
            if (br[2] == t && br[3] == lb) taken_if_true = true; else if (br[2] == lb && br[3] == t) taken_if_true = false; else continue;
            size_t st_t = SIZE_MAX, st_0 = SIZE_MAX;
            for (size_t k : st) { if (s.block[k] == t) st_t = k; else st_0 = k; }
            if (st_t == SIZE_MAX || st_0 == SIZE_MAX) continue;
            const uint32_t b0 = s.block[st_0];
            if (b0 == lb || b0 == t || !g.dominates(b0, a) || g.reach_avoiding(t, a, b0, s.bound)) continue;
            const size_t mark = emit.size(); const uint32_t bmark = e.bound;
            Z zt = low_zero(s.ins(st_t)[2], depth + 1);
            if (zt.k != True && zt.k != False) { emit.resize(mark); e.bound = bmark; return {Fail, 0}; }   // the then-value must be a constant
            Z z0 = low_zero(s.ins(st_0)[2], depth + 1); if (z0.k == Fail) return z0;
            return taken_if_true ? sel(cond, zt, z0) : sel(cond, z0, zt);
        }
        return {Fail, 0};
    }
};
}

size_t xbg_spirv_halfflag_fold(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear(); Spv s; if (!s.parse(in, n) || !s.glsl) return 0;
    Cfg g; g.build(s); Edit e(s); Half h(s, g, e); size_t folded = 0;
    for (size_t k = 0; k < s.count(); k++) {
        if (s.op(k) != OpFOrdEqual || s.wc(k) != 5 || s.at[k] < s.first_func) continue;
        const uint32_t *d = s.ins(k);
        const uint32_t *t = s.def_of(d[1]); if (!t || (t[0] & 0xffff) != OpTypeBool) continue;
        uint32_t x = 0;
        for (int side = 0; side < 2 && !x; side++) {
            uint32_t z; if (!s.const32(d[4 - side], z) || (z & 0x7fffffffu)) continue;
            const uint32_t *ex = s.def_of(d[3 + side]);
            if (!ex || (ex[0] & 0xffff) != OpCompositeExtract || (ex[0] >> 16) != 5 || ex[4] != 0) continue;
            const uint32_t *u = s.def_of(ex[3]); if (!s.is_glsl(u, GlslUnpackHalf2x16) || (u[0] >> 16) != 6) continue;
            x = u[5];
        }
        if (!x) continue;
        h.tbool = d[1]; h.emit.clear(); h.budget = 512; const uint32_t bmark = e.bound; const size_t dmark = e.decl.size();
        Half::Z z = h.low_zero(x, 0);
        // (Only id_of below declares constants, so a failure leaves e.decl as it was.)
        if (z.k == Half::Fail) { e.bound = bmark; e.decl.resize(dmark); continue; }
        // The last emitted instruction may take the comparison's id; otherwise a copy of the boolean does.
        std::vector<uint32_t> self;
        if (z.k == Half::Ssa && !h.emit.empty()) {
            size_t p = 0, last = SIZE_MAX; while (p < h.emit.size()) { last = p; p += h.emit[p] >> 16; }
            if (last != SIZE_MAX && h.emit[last + 2] == z.id) {
                self.assign(h.emit.begin() + ptrdiff_t(last), h.emit.end()); self[2] = d[2]; h.emit.resize(last);
            }
        }
        if (self.empty()) self = {inst(OpCopyObject, 4), d[1], d[2], h.id_of(z)};
        e.before[k] = h.emit; e.replace(k, self); folded++;
    }
    if (!folded) return 0;
    e.write(out); return folded;
}

size_t xbg_spirv_f2i_notrunc(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear(); Spv s; if (!s.parse(in, n) || !s.glsl) return 0;
    Edit e(s); size_t done = 0;
    for (size_t k = 0; k < s.count(); k++) {
        const uint32_t op = s.op(k); if ((op != OpConvertFToU && op != OpConvertFToS) || s.wc(k) != 4 || s.at[k] < s.first_func) continue;
        size_t site = k; uint32_t slot = 3, v = s.ins(k)[3];   // the operand word to repoint
        bool ok = true;
        for (int guard = 0; guard < 8; guard++) {
            const uint32_t *d = s.def_of(v); if (!d) { ok = false; break; }
            if (s.is_glsl(d, GlslTrunc)) break;
            const bool clamp = s.is_glsl(d, GlslFClamp) || s.is_glsl(d, GlslNClamp);
            const bool mm = s.is_glsl(d, GlslFMin) || s.is_glsl(d, GlslFMax) || s.is_glsl(d, GlslNMin) || s.is_glsl(d, GlslNMax);
            if ((!clamp && !mm) || s.uses[v] != 1) { ok = false; break; }
            const uint32_t wc = d[0] >> 16; const size_t dk = s.def_index(v);
            if (clamp) { if (wc != 8 || !s.integral_float(d[6]) || !s.integral_float(d[7])) { ok = false; break; } site = dk; slot = 5; v = d[5]; }
            else if (wc != 7) { ok = false; break; }
            else if (s.integral_float(d[6])) { site = dk; slot = 5; v = d[5]; }
            else if (s.integral_float(d[5])) { site = dk; slot = 6; v = d[6]; }
            else { ok = false; break; }
        }
        const uint32_t *tr = s.def_of(v);
        if (!ok || !s.is_glsl(tr, GlslTrunc) || (tr[0] >> 16) != 6) continue;
        if (e.replaced[site]) continue;
        std::vector<uint32_t> w(s.ins(site), s.ins(site) + s.wc(site)); w[slot] = tr[5];
        e.replace(site, w); done++;
    }
    if (!done) return 0;
    e.write(out); return done;
}

size_t xbg_spirv_strip_pointsize(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear(); Spv s; if (!s.parse(in, n)) return 0;
    std::vector<uint8_t> ps_var(s.bound, 0), ps_ptr(s.bound, 0); std::vector<int32_t> member(s.bound, -1);
    bool vertex = false;
    for (size_t k = 0; k < s.count() && s.at[k] < s.first_func; k++) {
        const uint32_t *d = s.ins(k), op = s.op(k), wc = s.wc(k);
        if (op == OpEntryPoint && wc >= 3) { if (d[1] != 0) return 0; vertex = true; }
        if (op == OpMemberDecorate && wc >= 5 && d[3] == DecorationBuiltIn && d[4] == BuiltInPointSize && d[1] < s.bound) member[d[1]] = int32_t(d[2]);
        if (op == OpDecorate && wc >= 4 && d[2] == DecorationBuiltIn && d[3] == BuiltInPointSize && d[1] < s.bound) ps_var[d[1]] = 2;   // the variable itself
    }
    if (!vertex) return 0;
    std::vector<int32_t> var_member(s.bound, -1);
    for (size_t k = 0; k < s.count() && s.at[k] < s.first_func; k++) {
        const uint32_t *d = s.ins(k);
        if (s.op(k) != OpVariable || s.wc(k) < 4 || d[3] != StorageOutput || d[2] >= s.bound) continue;
        const uint32_t *pt = s.def_of(d[1]); if (!pt || (pt[0] & 0xffff) != OpTypePointer || pt[3] >= s.bound) continue;
        if (member[pt[3]] >= 0) var_member[d[2]] = member[pt[3]];
    }
    Edit e(s); size_t stripped = 0; std::vector<size_t> drops;
    for (size_t k = 0; k < s.count(); k++) {
        if (s.at[k] < s.first_func) continue;
        const uint32_t *d = s.ins(k), op = s.op(k), wc = s.wc(k);
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 5 && d[3] < s.bound && var_member[d[3]] >= 0) {
            uint32_t idx; if (s.const32(d[4], idx) && idx == uint32_t(var_member[d[3]])) { if (wc != 5) return 0; ps_ptr[d[2]] = 1; }
        }
    }
    for (size_t k = 0; k < s.count(); k++) {
        if (s.at[k] < s.first_func) continue;
        const uint32_t *d = s.ins(k), op = s.op(k), wc = s.wc(k);
        for (uint32_t x = 1; x < wc; x++) {
            const uint32_t id = d[x]; if (id >= s.bound || literal_at(op, x)) continue;
            if (!ps_ptr[id] && ps_var[id] != 2) continue;
            if (op == OpAccessChain && x == 2 && ps_ptr[id]) continue;   // its own definition
            if (op == OpStore && x == 1) {
                uint32_t b; if (!s.const32(d[2], b) || b != 0x3f800000u) return 0;   // only the constant 1.0
                drops.push_back(k); continue;
            }
            return 0;   // read, copied or passed on: keep the module
        }
    }
    for (size_t k : drops) { e.drop(k); stripped++; }
    if (!stripped) return 0;
    e.write(out); return stripped;
}

size_t xbg_spirv_texcoord_unguard(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear(); Spv s; if (!s.parse(in, n)) return 0;
    Edit e(s); size_t done = 0;
    for (size_t k = 0; k < s.count(); k++) {
        const uint32_t op = s.op(k); if (op < 87 || op > 97 || op == 95 || s.wc(k) < 5 || s.at[k] < s.first_func) continue;
        const uint32_t *d = s.ins(k);
        const uint32_t *sel = s.def_of(d[4]); if (!sel || (sel[0] & 0xffff) != OpSelect || (sel[0] >> 16) != 6) continue;
        const uint32_t *nan = s.def_of(sel[3]); if (!nan || (nan[0] & 0xffff) != OpIsNan || (nan[0] >> 16) != 4 || nan[3] != sel[5]) continue;
        // the replacement value must be a zero constant (scalar 0.0, null, or a composite of 0.0)
        const uint32_t *z = s.def_of(sel[4]); if (!z) continue; const uint32_t zop = z[0] & 0xffff; bool zero = zop == OpConstantNull;
        if (zop == OpConstant && (z[0] >> 16) == 4) zero = !(z[3] & 0x7fffffffu);
        if (zop == OpConstantComposite) { zero = true; for (uint32_t x = 3; x < (z[0] >> 16); x++) { uint32_t b; if (!s.const32(z[x], b) || (b & 0x7fffffffu)) zero = false; } }
        if (!zero) continue;
        std::vector<uint32_t> w(d, d + s.wc(k)); w[4] = sel[5]; e.replace(k, w); done++;
    }
    if (!done) return 0;
    e.write(out); return done;
}
