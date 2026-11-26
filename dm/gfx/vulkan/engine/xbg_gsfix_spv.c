// gs-fix: geometry-shader SPIR-V rewrites (xbg_gsfix_spv.h). Adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later:
// dm/gfx/vk_android/vka_spvpatch.c (claim_pass / vka_spv_gs_claim, smod_*, vka_spv_gs_link, vka_spv_gs_fill). This file is under
// GPL-3.0-or-later as well. Host test: port-common/modules/gs-fix/test (t_gsfix.c + spirv-val over D16 / NieR geometry shaders).
#include "xbg_gsfix_spv.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { OpUndef = 1, OpName = 5, OpMemberName = 6, OpLine = 8, OpExtInst = 12, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeBool = 20, OpTypeInt = 21,
       OpTypeFloat = 22, OpTypeVector = 23, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeForwardPointer = 39,
       OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46, OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56, OpVariable = 59,
       OpImageTexelPointer = 60, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65, OpInBoundsAccessChain = 66,
       OpPtrAccessChain = 67, OpInBoundsPtrAccessChain = 70, OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74,
       OpGroupMemberDecorate = 75, OpImageWrite = 99, OpConvertUToPtr = 120, OpBitcast = 124, OpIAdd = 128, OpLogicalOr = 166, OpSelect = 169,
       OpIEqual = 170, OpINotEqual = 171, OpEmitVertex = 218, OpEndPrimitive = 219, OpEmitStreamVertex = 220, OpEndStreamPrimitive = 221,
       OpAtomicStore = 228, OpAtomicExchange = 229, OpAtomicCompareExchange = 230, OpAtomicCompareExchangeWeak = 231, OpAtomicXor = 242, OpPhi = 245,
       OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253,
       OpReturnValue = 254, OpUnreachable = 255, OpNoLine = 317, OpDecorateId = 332, OpTerminateInvocation = 4416, OpDecorateString = 5632,
       OpMemberDecorateString = 5633, OpAtomicFMinEXT = 5614, OpAtomicFMaxEXT = 5615, OpAtomicFAddEXT = 6035, OpCopyObject = 83 };
enum { SCInput = 1, SCUniform = 2, SCOutput = 3, SCPrivate = 6, SCImage = 11, SCStorageBuffer = 12, SCPhysicalStorageBuffer = 5349 };
enum { DecBlock = 2, DecBufferBlock = 3, DecArrayStride = 6, DecBuiltIn = 11, DecComponent = 31, DecLocation = 30, DecBinding = 33, DecDescriptorSet = 34,
       DecOffset = 35, BIPosition = 0, BIPrimitiveId = 7 };

typedef struct { uint32_t *w; size_t n, cap; } vec_t;
static int put(vec_t *v, uint32_t x) {
    if (v->n == v->cap) { size_t c = v->cap ? 2 * v->cap : 4096; uint32_t *p = (uint32_t *)realloc(v->w, c * 4); if (!p) return 0; v->w = p; v->cap = c; }
    v->w[v->n++] = x; return 1;
}
static int put_n(vec_t *v, const uint32_t *x, size_t n) { for (size_t i = 0; i < n; i++) if (!put(v, x[i])) return 0; return 1; }
static int sc_mem(uint32_t sc) { return sc == SCUniform || sc == SCImage || sc == SCStorageBuffer || sc == SCPhysicalStorageBuffer; }
static int is_term(uint32_t op) { return op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpKill || op == OpReturn || op == OpReturnValue || op == OpUnreachable || op == OpTerminateInvocation; }
static int is_annot(uint32_t op) { return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate || op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString; }

// ---- gs_claim ----------------------------------------------------------------------------------------------------------------
// One pass of the rewrite, run twice: the first records in rec where each split block of the input ends, the second gives that to
// the phis (last) that name such a block as their parent (the new ids are the same in both passes). [peanuts claim_pass]
static int claim_pass(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, const uint32_t *last, uint32_t *rec, vec_t *o, int *found, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t bound = w[3], nb = bound;
    const int v14 = w[1] >= 0x10400;   // SPIR-V 1.4+: the claim buffer is a StorageBuffer Block, every global it uses joins the entry point
    const uint32_t csc = v14 ? SCStorageBuffer : SCUniform;
    uint32_t *psc = (uint32_t *)calloc(bound, 4), *ty = (uint32_t *)calloc(bound, 4);   // psc[pointer type]: storage class + 1; ty[id]: the type of a pointer-valued id
    if (!psc || !ty) FAIL("out of memory");
    uint32_t t_bool = 0, t_u32 = 0, t_i32 = 0, p_in_i32 = 0, p_priv_bool = 0, p_c_u32 = 0, main_fn = 0, prim_var = 0;
    size_t first_fn = nw, first_annot = nw, ep_at = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) psc[w[i + 1]] = w[i + 2] + 1;
        if (op == OpTypePointer && len >= 4) {
            if (w[i + 2] == SCInput && w[i + 3] && w[i + 3] == t_i32) p_in_i32 = w[i + 1];
            if (w[i + 2] == SCPrivate && w[i + 3] && w[i + 3] == t_bool) p_priv_bool = w[i + 1];
            if (w[i + 2] == csc && w[i + 3] && w[i + 3] == t_u32) p_c_u32 = w[i + 1];
        }
        else if ((op == OpVariable || op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain || op == OpConvertUToPtr ||
                  op == OpBitcast || op == OpCopyObject || op == OpImageTexelPointer || op == OpFunctionParameter) && len >= 3 && w[i + 2] < bound) ty[w[i + 2]] = w[i + 1];
        if (op == OpTypeBool) t_bool = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpEntryPoint && len >= 3) { if (main_fn) FAIL("more than one entry point"); main_fn = w[i + 2]; ep_at = i; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == BIPrimitiveId) prim_var = w[i + 1];
        else if (op == OpExecutionMode && len >= 4 && w[i + 2] == 0 && w[i + 3] > 1) FAIL("%u invocations a primitive: they would share its claim", w[i + 3]);   // Invocations
        if (is_annot(op) && first_annot == nw) first_annot = i;
        if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!main_fn || first_fn == nw || !t_u32) FAIL("no entry point, function or 32-bit unsigned type");
    if (first_annot > first_fn) first_annot = first_fn;
    if (prim_var && (!p_in_i32 || ty[prim_var] != p_in_i32)) FAIL("a PrimitiveId input that is not a 32-bit int");
    // New ids: types missing, the claim buffer's types and variable, the constants, the guard variable
    uint32_t mk_bool = t_bool ? 0 : nb++, mk_i32 = t_i32 ? 0 : nb++; if (!t_bool) t_bool = mk_bool; if (!t_i32) t_i32 = mk_i32;
    uint32_t mk_pin = p_in_i32 && !mk_i32 ? 0 : nb++, mk_ppb = p_priv_bool && !mk_bool ? 0 : nb++, mk_pc = p_c_u32 ? 0 : nb++;
    if (mk_pin) p_in_i32 = mk_pin;
    if (mk_ppb) p_priv_bool = mk_ppb;
    if (mk_pc) p_c_u32 = mk_pc;
    uint32_t mk_prim = prim_var ? 0 : nb++; if (!prim_var) prim_var = mk_prim;
    uint32_t rta = nb++, st = nb++, pst = nb++, cvar = nb++, c0 = nb++, c1 = nb++, gvar = nb++;
    if (!put_n(o, w, 5)) FAIL("out of memory");
    uint32_t cur = 0, cur_orig = 0; int in_fn = 0, in_main_entry = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == ep_at && !mk_prim && !v14) { if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (i == ep_at) {   // the PrimitiveId input joins the interface (1.4+: the claim buffer and the guard too)
            uint32_t add = (mk_prim ? 1 : 0) + (v14 ? 2 : 0);
            if (!put(o, (len + add) << 16 | OpEntryPoint) || !put_n(o, in + 1, len - 1)) FAIL("out of memory");
            if (mk_prim && !put(o, prim_var)) FAIL("out of memory");
            if (v14 && (!put(o, cvar) || !put(o, gvar))) FAIL("out of memory");
            i += len; continue;
        }
        if (i == first_annot) {
            uint32_t x[] = {4u << 16 | OpDecorate, rta, DecArrayStride, 4, 3u << 16 | OpDecorate, st, v14 ? DecBlock : DecBufferBlock, 5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0,
                            4u << 16 | OpDecorate, cvar, DecDescriptorSet, set, 4u << 16 | OpDecorate, cvar, DecBinding, binding};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (mk_prim) { uint32_t y[] = {4u << 16 | OpDecorate, mk_prim, DecBuiltIn, BIPrimitiveId}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
        }
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(o, x, 2)) FAIL("out of memory"); }
            if (mk_i32) { uint32_t x[] = {4u << 16 | OpTypeInt, mk_i32, 32, 1}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pin) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pin, SCInput, t_i32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_ppb) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_ppb, SCPrivate, t_bool}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pc) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pc, csc, t_u32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_prim) { uint32_t x[] = {4u << 16 | OpVariable, p_in_i32, mk_prim, SCInput}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {3u << 16 | OpTypeRuntimeArray, rta, t_u32, 3u << 16 | OpTypeStruct, st, rta, 4u << 16 | OpTypePointer, pst, csc, st,
                            4u << 16 | OpVariable, pst, cvar, csc, 4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1,
                            4u << 16 | OpVariable, p_priv_bool, gvar, SCPrivate};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (op == OpFunction) { in_fn = 1; in_main_entry = w[i + 2] == main_fn ? 1 : 0; }
        else if (op == OpFunctionEnd) in_fn = 0;
        if (op == OpLabel) { cur = cur_orig = w[i + 1]; if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (in_main_entry == 1 && op != OpVariable && op != OpLine && op != OpNoLine && cur) {   // the claim, after the entry block's variables
            // tag = claim[0]; idx = tag == 0 ? 0 : 1 + PrimitiveId; old = atomicExchange(claim[idx], tag); first = tag == 0 || old != tag
            uint32_t tp = nb++, tag = nb++, byp = nb++, pid = nb++, i1 = nb++, idx = nb++, ptr = nb++, old = nb++, neq = nb++, first = nb++;
            uint32_t x[] = {6u << 16 | OpAccessChain, p_c_u32, tp, cvar, c0, c0, 4u << 16 | OpLoad, t_u32, tag, tp, 5u << 16 | OpIEqual, t_bool, byp, tag, c0,
                            4u << 16 | OpLoad, t_i32, pid, prim_var, 5u << 16 | OpIAdd, t_u32, i1, pid, c1, 6u << 16 | OpSelect, t_u32, idx, byp, c0, i1,
                            6u << 16 | OpAccessChain, p_c_u32, ptr, cvar, c0, idx, 7u << 16 | OpAtomicExchange, t_u32, old, ptr, c1, c0, tag,
                            5u << 16 | OpINotEqual, t_bool, neq, old, tag, 5u << 16 | OpLogicalOr, t_bool, first, byp, neq, 3u << 16 | OpStore, gvar, first};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            in_main_entry = 2;
        }
        if (op == OpPhi && last) {   // a parent block that was split ends in its last piece
            if (!put_n(o, in, 3)) FAIL("out of memory");
            for (uint32_t k = 3; k + 1 < len; k += 2) { uint32_t par = in[k + 1]; if (!put(o, in[k]) || !put(o, par < bound && last[par] ? last[par] : par)) FAIL("out of memory"); }
            i += len; continue;
        }
        #define SC(id) ((id) < bound && ty[id] < bound && psc[ty[id]] ? psc[ty[id]] - 1 : 0u)
        int drop = 0, atomic = 0;
        if (in_fn && (op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpAtomicStore) && len >= 2 && sc_mem(SC(in[1]))) drop = 1;
        else if (in_fn && op == OpImageWrite) drop = 1;
        else if (in_fn && ((op >= OpAtomicExchange && op <= OpAtomicXor) || op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT) && len >= 4 && sc_mem(SC(in[3]))) atomic = 1;
        if (drop || atomic) {
            (*found)++;
            for (size_t j = i; j < nw;) { uint32_t oj = w[j] & 0xffff; if (oj == OpLoopMerge) FAIL("a memory write in a loop header block"); if (is_term(oj)) break; j += w[j] >> 16; }
            uint32_t g = nb++, a = nb++, l = nb++, fb = 0;
            if (atomic) {
                uint32_t sc = SC(in[3]);
                if ((op == OpAtomicCompareExchange || op == OpAtomicCompareExchangeWeak) && len >= 9) fb = in[8];
                else { fb = nb++;
                    if (sc == SCImage) { uint32_t x[] = {3u << 16 | OpUndef, in[1], fb}; if (!put_n(o, x, 3)) FAIL("out of memory"); }
                    else if (sc == SCPhysicalStorageBuffer) { uint32_t x[] = {6u << 16 | OpLoad, in[1], fb, in[3], 2, 4}; if (!put_n(o, x, 6)) FAIL("out of memory"); }   // Aligned 4
                    else { uint32_t x[] = {4u << 16 | OpLoad, in[1], fb, in[3]}; if (!put_n(o, x, 4)) FAIL("out of memory"); } }
            }
            uint32_t x[] = {4u << 16 | OpLoad, t_bool, g, gvar, 3u << 16 | OpSelectionMerge, l, 0, 4u << 16 | OpBranchConditional, g, a, l, 2u << 16 | OpLabel, a};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (atomic) { uint32_t r = in[2]; if (!put(o, in[0]) || !put(o, in[1]) || !put(o, nb) || !put_n(o, in + 3, len - 3)) FAIL("out of memory");
                uint32_t ra = nb++, y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l, 7u << 16 | OpPhi, in[1], r, ra, a, fb, cur};
                if (!put_n(o, y, sizeof y / 4)) FAIL("out of memory"); }
            else { if (!put_n(o, in, len)) FAIL("out of memory"); uint32_t y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
            cur = l; if (rec && cur_orig < bound) rec[cur_orig] = l;
            i += len; continue;
        }
        #undef SC
        if (!put_n(o, in, len)) FAIL("out of memory");
        i += len;
    }
    o->w[3] = nb;
    free(psc); free(ty);
    return 0;
fail:
    free(psc); free(ty);
    return -1;
#undef FAIL
}
int xbg_gsfix_claim(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn) {
    if (out) *out = NULL;
    err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return -1; }
    uint32_t *rec = (uint32_t *)calloc(w[3], 4); vec_t o = {0}; int found = 0;
    if (!rec) { snprintf(err, errn, "out of memory"); return -1; }
    if (claim_pass(w, nw, set, binding, NULL, rec, &o, &found, err, errn)) { free(rec); free(o.w); return -1; }
    if (found && out) {
        free(o.w); o = (vec_t){0}; int f2 = 0;
        if (claim_pass(w, nw, set, binding, rec, NULL, &o, &f2, err, errn)) { free(rec); free(o.w); return -1; }
        *out = o.w; *out_nw = o.n; o.w = NULL;
    }
    free(rec); free(o.w);
    return found;
}

// ---- module tables (gs_link, gs_fill) ------------------------------------------------------------------------------------------
typedef struct { const uint32_t *w; size_t n; uint32_t bound, *op, *a1, *a2, *a3; } spvmod_t;   // per id declared before the first function: its opcode and operands
static int smod_open(spvmod_t *m, const uint32_t *w, size_t n) {
    memset(m, 0, sizeof *m); m->w = w; m->n = n;
    if (n < 5 || w[0] != 0x07230203) return 0;
    m->bound = w[3];
    if (!(m->op = (uint32_t *)calloc(m->bound, 4)) || !(m->a1 = (uint32_t *)calloc(m->bound, 4)) || !(m->a2 = (uint32_t *)calloc(m->bound, 4)) || !(m->a3 = (uint32_t *)calloc(m->bound, 4))) return 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if (op == OpFunction) break;
        // a type: its operands after the result; a variable or constant: its type, then the operands after the result (storage class; value)
        uint32_t r = 0, a[3] = {0};
        if (op == OpTypeFloat || op == OpTypeInt || op == OpTypeVector || op == OpTypePointer || op == OpTypeArray || op == OpTypeStruct) {
            r = w[i + 1]; for (int k = 0; k < 3; k++) a[k] = 2u + k < len ? w[i + 2 + k] : 0; }
        else if ((op == OpVariable || op == OpConstant) && len >= 4) { r = w[i + 2]; a[0] = w[i + 1]; a[1] = w[i + 3]; a[2] = len > 4 ? w[i + 4] : 0; }
        if (r && r < m->bound) { m->op[r] = op; m->a1[r] = a[0]; m->a2[r] = a[1]; m->a3[r] = a[2]; }
        i += len;
    }
    return 1;
}
static void smod_close(spvmod_t *m) { free(m->op); free(m->a1); free(m->a2); free(m->a3); }
// A type's signature: kind (1 float, 2 int, 3 uint, all 32-bit) << 4 | components; 0 = not a 32-bit scalar or vector
static uint32_t smod_sig(const spvmod_t *m, uint32_t t) {
    if (t >= m->bound) return 0;
    uint32_t n = 1;
    if (m->op[t] == OpTypeVector) { n = m->a2[t]; t = m->a1[t]; if (t >= m->bound || n < 2 || n > 4) return 0; }
    if (m->op[t] == OpTypeFloat && m->a1[t] == 32) return 1u << 4 | n;
    if (m->op[t] == OpTypeInt && m->a1[t] == 32) return (m->a2[t] ? 2u : 3u) << 4 | n;
    return 0;
}
// The user Input or Output variables (sc) by Location (sig: see xbg_gsfix_io). A geometry shader's inputs are arrays per vertex:
// with arrayed, an array of such a type counts as that type.
static void smod_io(const spvmod_t *m, uint32_t sc, int arrayed, uint8_t sig[32]) {
    memset(sig, 0, 32);
    uint8_t *comp = (uint8_t *)calloc(m->bound, 1); uint32_t *loc = (uint32_t *)malloc(m->bound * 4); if (!comp || !loc) { free(comp); free(loc); return; }
    for (uint32_t i = 0; i < m->bound; i++) loc[i] = ~0u;
    for (size_t i = 5; i < m->n;) {
        uint32_t op = m->w[i] & 0xffff, len = m->w[i] >> 16;
        if (op == OpFunction || !len) break;
        if (op == OpDecorate && len >= 4 && m->w[i + 1] < m->bound) { if (m->w[i + 2] == DecLocation) loc[m->w[i + 1]] = m->w[i + 3]; else if (m->w[i + 2] == DecComponent) comp[m->w[i + 1]] = 1; }
        i += len;
    }
    for (uint32_t v = 0; v < m->bound; v++) {
        if (m->op[v] != OpVariable || m->a2[v] != sc || loc[v] >= 32) continue;
        uint32_t pt = m->a1[v], t = pt < m->bound && m->op[pt] == OpTypePointer ? m->a2[pt] : 0;
        if (arrayed && t < m->bound && m->op[t] == OpTypeArray) t = m->a1[t];
        uint32_t s = t ? smod_sig(m, t) : 0;
        sig[loc[v]] = comp[v] || !s ? 0xff : (uint8_t)s;
    }
    free(comp); free(loc);
}
int xbg_gsfix_io(const uint32_t *w, size_t nw, uint32_t sc, uint8_t sig[32]) {
    spvmod_t m; memset(sig, 0, 32);
    if (!smod_open(&m, w, nw)) { smod_close(&m); return -1; }
    smod_io(&m, sc, 0, sig); smod_close(&m); return 0;
}
uint32_t xbg_gsfix_link_need(const uint8_t gout[32], const uint8_t fin[32]) {
    uint32_t need = 0;
    for (int l = 0; l < 32; l++) if (fin[l] && fin[l] != 0xff && !gout[l]) need |= 1u << l;
    return need;
}

// ---- gs_link -----------------------------------------------------------------------------------------------------------------
uint32_t *xbg_gsfix_link(const uint32_t *gw, size_t gn, const uint8_t vout[32], const uint8_t fin[32], uint32_t *copied, uint32_t *nulled,
                         size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    spvmod_t G; vec_t o = {0}, decl = {0}, annot = {0}; uint8_t *isconst = NULL;   // isconst: 2 a Position block, then 1 an integer constant
    *copied = *nulled = 0; err[0] = 0;
    if (!smod_open(&G, gw, gn)) FAIL("not SPIR-V (geometry)");
    uint8_t gout[32], gin[32]; smod_io(&G, SCOutput, 0, gout); smod_io(&G, SCInput, 1, gin);
    uint32_t need = xbg_gsfix_link_need(gout, fin);
    if (!need) goto none;
    for (int l = 0; l < 32; l++) if ((need >> l & 1) && gin[l]) FAIL("location %d: the geometry shader has an input there", l);
    // gl_in: the Input array of a block whose member 0 is BuiltIn Position; its length; constant ids (indexes taken from its access chains)
    uint32_t glin = 0, nvert = 0, ep_at = 0, first_fn = 0, first_annot = 0;
    if (!(isconst = (uint8_t *)calloc(G.bound, 1))) FAIL("out of memory");
    for (size_t i = 5; i < gn;) {
        uint32_t op = gw[i] & 0xffff, len = gw[i] >> 16;
        if (op == OpFunction) { first_fn = (uint32_t)i; break; }
        if (op == OpEntryPoint) ep_at = (uint32_t)i;
        if (is_annot(op) && !first_annot) first_annot = (uint32_t)i;
        if (op == OpMemberDecorate && len >= 5 && gw[i + 2] == 0 && gw[i + 3] == DecBuiltIn && gw[i + 4] == BIPosition && gw[i + 1] < G.bound) isconst[gw[i + 1]] = 2;
        i += len;
    }
    if (!first_fn || !ep_at || !first_annot) FAIL("no function, entry point or decoration");
    for (uint32_t v = 0; v < G.bound; v++) {
        if (G.op[v] != OpVariable || G.a2[v] != SCInput) continue;
        uint32_t pt = G.a1[v], at = pt < G.bound ? G.a2[pt] : 0;
        if (at < G.bound && G.op[at] == OpTypeArray && G.a1[at] < G.bound && isconst[G.a1[at]] == 2 && G.a2[at] < G.bound && G.op[G.a2[at]] == OpConstant) { glin = v; nvert = G.a2[G.a2[at]]; }
    }
    if (!glin || !nvert || nvert > 6) FAIL("no gl_in array");
    for (uint32_t v = 0; v < G.bound; v++) isconst[v] = G.op[v] == OpConstant && G.a1[v] < G.bound && G.op[G.a1[v]] == OpTypeInt;
    // Types: reused when the module has them (a second declaration of a scalar or vector type is invalid), else declared before the first function
    uint32_t nb = G.bound;
    #define FIND(cond) ({ uint32_t _r = 0; for (uint32_t _t = 1; _t < G.bound && !_r; _t++) if (cond) _r = _t; _r; })
    uint32_t sc_t[4] = {0}, vec_t_[4][5] = {{0}};
    uint32_t mkp_sc[64], mkp_pt[64], mkp_id[64]; int nmkp = 0;   // pointer types made here
    uint32_t outv[32] = {0}, inv[32] = {0}, pin[32] = {0}, nul[32] = {0}, ty[32] = {0};
    for (int l = 0; l < 32; l++) if (need >> l & 1) {
        uint32_t s = fin[l], k = s >> 4, n = s & 15;
        if (!sc_t[k]) {
            sc_t[k] = k == 1 ? FIND(G.op[_t] == OpTypeFloat && G.a1[_t] == 32) : FIND(G.op[_t] == OpTypeInt && G.a1[_t] == 32 && G.a2[_t] == (k == 2));
            if (!sc_t[k]) { sc_t[k] = nb++; uint32_t x[4] = {(k == 1 ? 3u : 4u) << 16 | (k == 1 ? OpTypeFloat : OpTypeInt), sc_t[k], 32, k == 2}; if (!put_n(&decl, x, k == 1 ? 3 : 4)) FAIL("out of memory"); }
        }
        uint32_t t = sc_t[k];
        if (n > 1) {
            if (!vec_t_[k][n]) { vec_t_[k][n] = FIND(G.op[_t] == OpTypeVector && G.a1[_t] == sc_t[k] && G.a2[_t] == n);
                                 if (!vec_t_[k][n]) { vec_t_[k][n] = nb++; uint32_t x[4] = {4u << 16 | OpTypeVector, vec_t_[k][n], sc_t[k], n}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); } }
            t = vec_t_[k][n];
        }
        ty[l] = t;
        for (int pass = 0; pass < 2; pass++) {   // the Output pointer, then (a copy) the Input pointer to the same type
            uint32_t sc = pass ? SCInput : SCOutput;
            if (pass && vout[l] != s) break;
            uint32_t p = FIND(G.op[_t] == OpTypePointer && G.a1[_t] == sc && G.a2[_t] == t);
            for (int q = 0; q < nmkp && !p; q++) if (mkp_sc[q] == sc && mkp_pt[q] == t) p = mkp_id[q];
            if (!p) { if (nmkp == 64) FAIL("too many types"); p = nb++; mkp_sc[nmkp] = sc; mkp_pt[nmkp] = t; mkp_id[nmkp++] = p; uint32_t x[4] = {4u << 16 | OpTypePointer, p, sc, t}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
            if (!pass) { outv[l] = nb++; uint32_t x[4] = {4u << 16 | OpVariable, p, outv[l], SCOutput}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
            else pin[l] = p;
        }
        uint32_t d[4] = {4u << 16 | OpDecorate, outv[l], DecLocation, (uint32_t)l}; if (!put_n(&annot, d, 4)) FAIL("out of memory");
        if (pin[l]) {   // uint length, the array type, its Input pointer, the variable
            uint32_t u = FIND(G.op[_t] == OpTypeInt && G.a1[_t] == 32 && G.a2[_t] == 0);
            if (!u) { if (!sc_t[3]) { sc_t[3] = nb++; uint32_t x[4] = {4u << 16 | OpTypeInt, sc_t[3], 32, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); } u = sc_t[3]; }
            uint32_t len_c = nb++, arr = nb++, parr = nb++; inv[l] = nb++;
            uint32_t x[] = {4u << 16 | OpConstant, u, len_c, nvert, 4u << 16 | OpTypeArray, arr, t, len_c, 4u << 16 | OpTypePointer, parr, SCInput, arr, 4u << 16 | OpVariable, parr, inv[l], SCInput};
            if (!put_n(&decl, x, sizeof x / 4)) FAIL("out of memory");
            uint32_t y[4] = {4u << 16 | OpDecorate, inv[l], DecLocation, (uint32_t)l}; if (!put_n(&annot, y, 4)) FAIL("out of memory");
            *copied |= 1u << l;
        } else { nul[l] = nb++; uint32_t x[3] = {3u << 16 | OpConstantNull, t, nul[l]}; if (!put_n(&decl, x, 3)) FAIL("out of memory"); *nulled |= 1u << l; }
    }
    uint32_t zero = 0;   // vertex 0 when no gl_in read names the vertex: a uint constant
    { uint32_t u = sc_t[3]; if (!u) u = FIND(G.op[_t] == OpTypeInt && G.a1[_t] == 32 && G.a2[_t] == 0);
      if (!u) { u = sc_t[3] = nb++; uint32_t x[4] = {4u << 16 | OpTypeInt, u, 32, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
      zero = nb++; uint32_t x[4] = {4u << 16 | OpConstant, u, zero, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    #undef FIND
    // Copy: annotations before the first one, declarations before the first function, the entry point's interface grown, the copies before each vertex emitted
    if (!put_n(&o, gw, 5)) FAIL("out of memory");
    uint32_t vtx = zero;
    for (size_t i = 5; i < gn;) {
        uint32_t op = gw[i] & 0xffff, len = gw[i] >> 16; const uint32_t *in = gw + i;
        if (i == first_annot && !put_n(&o, annot.w, annot.n)) FAIL("out of memory");
        if (i == first_fn && !put_n(&o, decl.w, decl.n)) FAIL("out of memory");
        if (i == ep_at) {
            size_t at = o.n; if (!put_n(&o, in, len)) FAIL("out of memory");
            for (int l = 0; l < 32; l++) { if (outv[l] && !put(&o, outv[l])) FAIL("out of memory"); if (inv[l] && !put(&o, inv[l])) FAIL("out of memory"); }
            o.w[at] = (uint32_t)(o.n - at) << 16 | OpEntryPoint; i += len; continue;
        }
        if (op == OpFunction) vtx = zero;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 5 && gw[i + 3] == glin && gw[i + 4] < G.bound && isconst[gw[i + 4]]) vtx = gw[i + 4];
        if (op == OpEmitVertex || op == OpEmitStreamVertex)
            for (int l = 0; l < 32; l++) {
                if (!outv[l]) continue;
                uint32_t val = nul[l];
                if (inv[l]) { uint32_t p = nb++; val = nb++; uint32_t x[] = {5u << 16 | OpAccessChain, pin[l], p, inv[l], vtx, 4u << 16 | OpLoad, ty[l], val, p}; if (!put_n(&o, x, 9)) FAIL("out of memory"); }
                uint32_t s[3] = {3u << 16 | OpStore, outv[l], val}; if (!put_n(&o, s, 3)) FAIL("out of memory");
            }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    smod_close(&G); free(decl.w); free(annot.w); free(isconst);
    *out_nw = o.n; return o.w;
none:
fail:
    smod_close(&G); free(decl.w); free(annot.w); free(isconst); free(o.w);
    *copied = *nulled = 0;
    return NULL;
#undef FAIL
}

// ---- gs_fill (peanuts vka_spv_gs_fill, unchanged but for the names) --------------------------------------------------------------
uint32_t *xbg_gsfix_fill(const uint32_t *w, size_t nw, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    enum { ModeOutputVertices = 26, ModeOutputPoints = 27, ModeOutputLineStrip = 28, ModeOutputTriangleStrip = 29, ExecGeometry = 3 };
    spvmod_t G; vec_t o = {0}, decl = {0}; uint8_t *posblk = NULL;
    err[0] = 0;
    if (!smod_open(&G, w, nw)) FAIL("not SPIR-V");
    uint32_t ep = 0, ep_at = 0, neps = 0, nv = 0, maxv = 0, first_fn = 0, t_bool = 0, posvar_direct = 0;
    if (!(posblk = (uint8_t *)calloc(G.bound, 1))) FAIL("out of memory");   // 1: a struct whose member 0 is BuiltIn Position; 2: a variable decorated BuiltIn Position
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpFunction) { first_fn = (uint32_t)i; break; }
        if (op == OpEntryPoint && len >= 3) { neps++; if (w[i + 1] == ExecGeometry) { ep = w[i + 2]; ep_at = (uint32_t)i; } }
        if (op == OpExecutionMode && len >= 3 && w[i + 1] == ep) {
            uint32_t m = w[i + 2];
            if (m == ModeOutputVertices && len >= 4) maxv = w[i + 3];
            else if (m == ModeOutputPoints) nv = 1; else if (m == ModeOutputLineStrip) nv = 2; else if (m == ModeOutputTriangleStrip) nv = 3;
        }
        if (op == OpMemberDecorate && len >= 5 && w[i + 2] == 0 && w[i + 3] == DecBuiltIn && w[i + 4] == BIPosition && w[i + 1] < G.bound) posblk[w[i + 1]] = 1;
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == BIPosition && w[i + 1] < G.bound) posblk[w[i + 1]] = 2;
        if (op == OpTypeBool && len >= 2) t_bool = w[i + 1];
        i += len;
    }
    if (!ep || neps != 1 || !nv || maxv < nv || !first_fn) goto none;
    // The emits: all in the entry function, none to a stream
    uint32_t fn = 0, nemit = 0;
    for (size_t i = first_fn; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction length");
        if (op == OpFunction && len >= 3) fn = w[i + 2];
        if (op == OpEmitStreamVertex || op == OpEndStreamPrimitive) goto none;
        if (op == OpEmitVertex || op == OpEndPrimitive) { if (fn != ep) goto none; nemit += op == OpEmitVertex; }
        i += len;
    }
    if (!nemit) goto none;
    // gl_Position: an Output variable of a block with member 0 Position (a chain to member 0), or one decorated Position itself
    uint32_t posvar = 0, v4 = 0;
    for (uint32_t v = 1; v < G.bound && !posvar; v++) {
        if (G.op[v] != OpVariable || G.a2[v] != SCOutput || G.a1[v] >= G.bound) continue;
        uint32_t t = G.a2[G.a1[v]];
        if (t < G.bound && G.op[t] == OpTypeStruct && posblk[t] == 1) { posvar = v; v4 = G.a1[t]; }
        else if (posblk[v] == 2) { posvar = v; posvar_direct = 1; v4 = t; }
    }
    if (!posvar || v4 >= G.bound || G.op[v4] != OpTypeVector || G.a2[v4] != 4) goto none;
    uint32_t t_f = G.a1[v4];
    if (t_f >= G.bound || G.op[t_f] != OpTypeFloat || G.a1[t_f] != 32) goto none;
    // Declarations (types reused when the module has them), before the first function
    uint32_t nb = G.bound;
    #define FIND(cond) ({ uint32_t _r = 0; for (uint32_t _t = 1; _t < G.bound && !_r; _t++) if (cond) _r = _t; _r; })
    uint32_t t_u = FIND(G.op[_t] == OpTypeInt && G.a1[_t] == 32 && G.a2[_t] == 0);
    if (!t_u) { t_u = nb++; uint32_t x[4] = {4u << 16 | OpTypeInt, t_u, 32, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    if (!t_bool) { t_bool = nb++; uint32_t x[2] = {2u << 16 | OpTypeBool, t_bool}; if (!put_n(&decl, x, 2)) FAIL("out of memory"); }
    uint32_t p_priv = FIND(G.op[_t] == OpTypePointer && G.a1[_t] == SCPrivate && G.a2[_t] == t_u);
    if (!p_priv) { p_priv = nb++; uint32_t x[4] = {4u << 16 | OpTypePointer, p_priv, SCPrivate, t_u}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    uint32_t p_out = 0;
    if (!posvar_direct) { p_out = FIND(G.op[_t] == OpTypePointer && G.a1[_t] == SCOutput && G.a2[_t] == v4);
                          if (!p_out) { p_out = nb++; uint32_t x[4] = {4u << 16 | OpTypePointer, p_out, SCOutput, v4}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); } }
    #undef FIND
    uint32_t u0 = nb++, u1 = nb++, f0 = nb++, fm1 = nb++, dead = nb++, flag = nb++;
    { uint32_t x[] = {4u << 16 | OpConstant, t_u, u0, 0, 4u << 16 | OpConstant, t_u, u1, 1, 4u << 16 | OpConstant, t_f, f0, 0, 4u << 16 | OpConstant, t_f, fm1, 0xbf800000u,
                      7u << 16 | OpConstantComposite, v4, dead, f0, f0, f0, fm1, 4u << 16 | OpVariable, p_priv, flag, SCPrivate};
      if (!put_n(&decl, x, sizeof x / 4)) FAIL("out of memory"); }
    // Copy: the flag in the entry point's interface from SPIR-V 1.4 (every global there), the declarations before the first function, the entry function rewritten
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    int in_ep = 0, cleared = 0, nret = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn && !put_n(&o, decl.w, decl.n)) FAIL("out of memory");
        if (i == ep_at && w[1] >= 0x10400) {
            size_t at = o.n; if (!put_n(&o, in, len) || !put(&o, flag)) FAIL("out of memory");
            o.w[at] = (uint32_t)(o.n - at) << 16 | OpEntryPoint; i += len; continue;
        }
        if (op == OpFunction && len >= 3) { in_ep = w[i + 2] == ep; cleared = 0; }
        if (in_ep && !cleared && op != OpFunction && op != OpFunctionParameter && op != OpLabel && op != OpVariable && op != OpLine && op != OpNoLine) {
            uint32_t x[3] = {3u << 16 | OpStore, flag, u0}; if (!put_n(&o, x, 3)) FAIL("out of memory"); cleared = 1;
        }
        if (in_ep && op == OpReturn) {
            uint32_t ld = nb++, z = nb++, fill = nb++, merge = nb++;
            uint32_t x[] = {4u << 16 | OpLoad, t_u, ld, flag, 5u << 16 | OpIEqual, t_bool, z, ld, u0, 3u << 16 | OpSelectionMerge, merge, 0,
                            4u << 16 | OpBranchConditional, z, fill, merge, 2u << 16 | OpLabel, fill};
            if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
            for (uint32_t k = 0; k < nv; k++) {
                uint32_t dst = posvar;
                if (!posvar_direct) { dst = nb++; uint32_t c[5] = {5u << 16 | OpAccessChain, p_out, dst, posvar, u0}; if (!put_n(&o, c, 5)) FAIL("out of memory"); }
                uint32_t st[4] = {3u << 16 | OpStore, dst, dead, 1u << 16 | OpEmitVertex}; if (!put_n(&o, st, 4)) FAIL("out of memory");
            }
            uint32_t y[] = {1u << 16 | OpEndPrimitive, 2u << 16 | OpBranch, merge, 2u << 16 | OpLabel, merge};
            if (!put_n(&o, y, sizeof y / 4)) FAIL("out of memory");
            nret++;
        }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        if (in_ep && op == OpEmitVertex) { uint32_t x[3] = {3u << 16 | OpStore, flag, u1}; if (!put_n(&o, x, 3)) FAIL("out of memory"); }
        i += len;
    }
    if (!nret) FAIL("no OpReturn in the entry function");
    o.w[3] = nb;
    smod_close(&G); free(decl.w); free(posblk);
    *out_nw = o.n; return o.w;
none:
fail:
    smod_close(&G); free(decl.w); free(posblk); free(o.w);
    return NULL;
#undef FAIL
}

uint32_t xbg_gsfix_prims(uint32_t topology, uint32_t count) {
    switch (topology) {
    case 0: return count;                          // point list
    case 1: return count / 2;                      // line list
    case 2: return count ? count - 1 : 0;          // line strip
    case 3: return count / 3;                      // triangle list
    case 4: case 5: return count > 2 ? count - 2 : 0;   // triangle strip, fan
    case 6: return count / 4;                      // line list with adjacency
    case 7: return count > 3 ? count - 3 : 0;      // line strip with adjacency
    case 8: return count / 6;                      // triangle list with adjacency
    case 9: return count > 4 ? count / 2 - 2 : 0;  // triangle strip with adjacency
    default: return count;
    }
}
