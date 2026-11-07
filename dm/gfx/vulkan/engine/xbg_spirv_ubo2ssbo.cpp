// [DOOM] Bones from a storage buffer (DM_GFX_BONES_SSBO, xbg_game.cpp ssbo_rewrite). On the stock Qualcomm driver of the Adreno
// 740 (Mi 13, driver 0676.69, 06.11; Samsung S23 players saw the same) every skinned model (bodies, monsters) was black: the
// colour / G-buffer pass VS (97106c2f415fb619, 0a2486a44cd37021, ...) got bone data (c5 = vp_c5, binding 6, NVN uniform slot 2)
// and vertex data byte-identical to the depth pre-pass VS, and the same position math, yet its triangles dropped out and
// flickered frame to frame; with the skinning branch off (bind pose, no c5 reads) the draw was stable. The bone reads are
// dynamically indexed, c5[((idx*48+K)>>2)>>2][((idx*48+K)>>2)&3] with idx from a vertex attribute. Turnip draws them right.
// Theory: the driver's binning pass (its own position-only VS variant) or its UBO constant-RAM path mishandles dynamically
// indexed uniform-buffer reads. Workaround: read such blocks as read-only storage buffers (same guest memory, same layout).
// Only the descriptor type changes on the engine side (xbg_game.cpp: binding flag XBG_BIND_SSBO).
#include "xbg_spirv_ubo2ssbo.h"
#include <cstring>
#include <map>
#include <unordered_map>

namespace {
enum : uint32_t {
    OpExtension = 10, OpExtInstImport = 11, OpMemoryModel = 14, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43,
    OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
    OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75, OpDecorateId = 332,
    OpDecorateString = 5632, OpMemberDecorateString = 5633, OpLine = 8, OpNoLine = 317,
    DecBlock = 2, DecBufferBlock = 3, DecNonWritable = 24, DecBinding = 33, DecDescriptorSet = 34,
    ScUniform = 2, ScStorageBuffer = 12,
};
inline uint32_t inst(uint32_t op, uint32_t n) { return (n << 16) | op; }
const char kExt[] = "SPV_KHR_storage_buffer_storage_class";
bool is_annotation(uint32_t op) {
    return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate ||
           op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString;
}
}

size_t xbg_spirv_ubo2ssbo(std::vector<uint32_t> &w, const std::set<uint32_t> *allowed, std::vector<uint32_t> *converted, bool dry_run) {
    if (converted) converted->clear();
    if (w.size() < 5 || w[0] != 0x07230203u) return 0;
    const uint32_t version = w[1];
    std::unordered_map<uint32_t, uint32_t> binding;              // variable -> Binding
    std::set<uint32_t> has_set, block, buffer_block, var_nonwritable;
    std::map<uint32_t, std::vector<size_t>> member_decos;      // struct -> OpMemberDecorate instructions
    std::map<uint32_t, std::vector<size_t>> struct_decos;      // struct -> OpDecorate instructions
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptrs;   // pointer type -> (storage class, pointee)
    std::unordered_map<uint32_t, size_t> ptr_at, struct_at;    // type id -> word index of its declaration
    std::set<uint32_t> consts;
    std::vector<std::pair<uint32_t, size_t>> uvars;            // Uniform variables (id, word index)
    size_t first_function = 0, last_annotation = 0, ext_at = 0;
    bool has_ext = false;
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > w.size()) return 0;
        if (op == OpFunction) { first_function = i; break; }
        if (op == OpExtension && n >= 2 && (n - 1) * 4 >= sizeof(kExt) &&
            !memcmp(reinterpret_cast<const char *>(&w[i + 1]), kExt, sizeof(kExt))) has_ext = true;
        if ((op == OpExtInstImport || op == OpMemoryModel) && !ext_at) ext_at = i;
        if (is_annotation(op)) last_annotation = i;
        if (op == OpDecorate && n >= 3) {
            const uint32_t id = w[i + 1], d = w[i + 2];
            if (d == DecBinding && n >= 4) binding[id] = w[i + 3];
            else if (d == DecDescriptorSet) has_set.insert(id);
            else if (d == DecBlock) block.insert(id);
            else if (d == DecBufferBlock) buffer_block.insert(id);
            else if (d == DecNonWritable) var_nonwritable.insert(id);
            struct_decos[id].push_back(i);
        } else if (op == OpMemberDecorate && n >= 4) member_decos[w[i + 1]].push_back(i);
        else if (op == OpTypePointer && n == 4) { ptrs[w[i + 1]] = {w[i + 2], w[i + 3]}; ptr_at[w[i + 1]] = i; }
        else if (op == OpTypeStruct && n >= 2) struct_at[w[i + 1]] = i;
        else if (op == OpConstant && n >= 4) consts.insert(w[i + 2]);
        else if (op == OpVariable && n >= 4 && w[i + 3] == ScUniform) uvars.push_back({w[i + 2], i});
        i += n;
    }
    if (!first_function || !last_annotation || uvars.empty()) return 0;
    // Candidates: a Block (not BufferBlock) struct directly, with set and binding.
    std::unordered_map<uint32_t, uint32_t> root;   // pointer id -> candidate variable
    std::unordered_map<uint32_t, uint32_t> var_struct;
    std::set<uint32_t> whole;                      // pointers to the whole block (the variable, chains without indices)
    for (auto &v : uvars) {
        auto p = ptrs.find(w[v.second + 1]); if (p == ptrs.end()) continue;
        const uint32_t s = p->second.second;
        if (!struct_at.count(s) || !block.count(s) || buffer_block.count(s)) continue;
        auto b = binding.find(v.first); if (b == binding.end() || !has_set.count(v.first)) continue;
        if (allowed && !allowed->count(b->second)) continue;
        root[v.first] = v.first; var_struct[v.first] = s; whole.insert(v.first);
    }
    if (root.empty()) return 0;
    std::set<uint32_t> dynamic, unsafe;
    std::vector<size_t> chains;   // access chains rooted at a candidate
    for (size_t i = first_function; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > w.size()) return 0;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4) {
            auto r = root.find(w[i + 3]);
            if (r != root.end()) {
                root[w[i + 2]] = r->second; chains.push_back(i);
                if (n == 4 && whole.count(w[i + 3])) whole.insert(w[i + 2]);
                for (uint32_t k = 4; k < n; k++) if (!consts.count(w[i + k])) dynamic.insert(r->second);
            }
            for (uint32_t k = 4; k < n; k++) { auto x = root.find(w[i + k]); if (x != root.end()) unsafe.insert(x->second); }
        } else if (op == OpLoad && n >= 4) {
            auto r = root.find(w[i + 3]); if (r != root.end() && whole.count(w[i + 3])) unsafe.insert(r->second);   // a whole-block value
        } else if (op != OpLine && op != OpNoLine) {
            for (uint32_t k = 1; k < n; k++) { auto x = root.find(w[i + k]); if (x != root.end()) unsafe.insert(x->second); }
        }
        i += n;
    }
    std::vector<uint32_t> vars;
    for (auto &r : var_struct) if (dynamic.count(r.first) && !unsafe.count(r.first)) vars.push_back(r.first);
    if (vars.empty()) return 0;
    std::set<uint32_t> conv(vars.begin(), vars.end());
    // Every chain result type must be a Uniform pointer.
    for (size_t at : chains) { auto p = ptrs.find(w[at + 1]); if (conv.count(root[w[at + 3]]) && (p == ptrs.end() || p->second.first != ScUniform)) return 0; }
    if (converted) { std::set<uint32_t> bs; for (uint32_t v : vars) bs.insert(binding[v]); converted->assign(bs.begin(), bs.end()); }
    if (dry_run) return vars.size();

    uint32_t bound = w[3];
    std::map<size_t, std::vector<uint32_t>> after;   // word index of an instruction -> words inserted after it
    std::vector<uint32_t> decos;                     // inserted after the last annotation
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> copy_of;   // struct -> (StorageBuffer copy, pointer to it)
    for (uint32_t v : vars) {
        const uint32_t s = var_struct[v];
        if (!copy_of.count(s)) {
            const uint32_t s2 = bound++, p2 = bound++;
            const size_t at = struct_at[s]; const uint32_t n = w[at] >> 16;
            auto &ins = after[at];
            ins.push_back(w[at]); ins.push_back(s2); ins.insert(ins.end(), w.begin() + ptrdiff_t(at + 2), w.begin() + ptrdiff_t(at + n));
            ins.insert(ins.end(), {inst(OpTypePointer, 4), p2, ScStorageBuffer, s2});
            decos.insert(decos.end(), {inst(OpDecorate, 3), s2, DecBlock});
            for (size_t d : struct_decos[s]) {
                const uint32_t dn = w[d] >> 16; if (w[d + 2] == DecBlock || w[d + 2] == DecBufferBlock) continue;
                decos.push_back(w[d]); decos.push_back(s2); decos.insert(decos.end(), w.begin() + ptrdiff_t(d + 2), w.begin() + ptrdiff_t(d + dn));
            }
            std::set<uint32_t> nonwritable;
            for (size_t d : member_decos[s]) {
                const uint32_t dn = w[d] >> 16; if (w[d + 3] == DecNonWritable) nonwritable.insert(w[d + 2]);
                decos.push_back(w[d]); decos.push_back(s2); decos.insert(decos.end(), w.begin() + ptrdiff_t(d + 2), w.begin() + ptrdiff_t(d + dn));
            }
            for (uint32_t m = 0; m + 2 < n; m++) if (!nonwritable.count(m)) decos.insert(decos.end(), {inst(OpMemberDecorate, 4), s2, m, DecNonWritable});
            copy_of[s] = {s2, p2};
        }
        if (!var_nonwritable.count(v)) decos.insert(decos.end(), {inst(OpDecorate, 3), v, DecNonWritable});
    }
    // StorageBuffer pointers for the chains' result types (reused where the module has one, else declared after the Uniform one).
    std::map<uint32_t, uint32_t> sb_ptr;   // pointee -> StorageBuffer pointer
    for (auto &p : ptrs) if (p.second.first == ScStorageBuffer) sb_ptr.emplace(p.second.second, p.first);
    for (size_t at : chains) {
        const uint32_t var = root[w[at + 3]]; if (!conv.count(var)) continue;
        if (whole.count(w[at + 2])) { w[at + 1] = copy_of[var_struct[var]].second; continue; }   // no indices: the block itself
        const uint32_t t = w[at + 1], pointee = ptrs[t].second;
        auto f = sb_ptr.find(pointee);
        if (f == sb_ptr.end()) {
            const uint32_t id = bound++; auto &ins = after[ptr_at[t]];
            ins.insert(ins.end(), {inst(OpTypePointer, 4), id, ScStorageBuffer, pointee});
            f = sb_ptr.emplace(pointee, id).first;
        }
        w[at + 1] = f->second;
    }
    for (auto &v : uvars) if (conv.count(v.first)) { w[v.second + 1] = copy_of[var_struct[v.first]].second; w[v.second + 3] = ScStorageBuffer; }
    std::vector<uint32_t> ext;
    if (version < 0x00010300u && !has_ext && ext_at) {
        const uint32_t sw = uint32_t((sizeof(kExt) + 3) / 4); ext.assign(1 + sw, 0); ext[0] = inst(OpExtension, 1 + sw);
        memcpy(&ext[1], kExt, sizeof(kExt));
    }
    std::vector<uint32_t> out; out.reserve(w.size() + decos.size() + ext.size() + 64);
    out.insert(out.end(), w.begin(), w.begin() + 5);
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16;
        if (i == ext_at && !ext.empty()) out.insert(out.end(), ext.begin(), ext.end());
        out.insert(out.end(), w.begin() + ptrdiff_t(i), w.begin() + ptrdiff_t(i + n));
        if (i == last_annotation) out.insert(out.end(), decos.begin(), decos.end());
        auto a = after.find(i); if (a != after.end()) out.insert(out.end(), a->second.begin(), a->second.end());
        i += n;
    }
    out[3] = bound; w.swap(out);
    return vars.size();
}
