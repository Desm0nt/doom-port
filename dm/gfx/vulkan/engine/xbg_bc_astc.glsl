// [nonadreno] bc-astc-gpu: ASTC 4x4 LDR block encoder (one invocation per 4x4 block) of the GPU BC decode path. xbg_bc_astc_encode.comp
// includes this file; the same text compiles as C++ through tests/glsl_shim.h + tests/bc_astc_encode_test.cpp (XBG_HOST), which decodes
// every block it writes with xbg_astc_ref_decode.h and measures it.
//
// Ported from GameNative's Mesa Vulkan wrapper, https://github.com/GameNative/mesa branch wrapper-25 at commit 6bcfbaa701a8,
// src/vulkan/wrapper/wrapper_astc_encode.comp (+ wrapper_astc_tables.h): Mesa, MIT License:
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
//   (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge,
//   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
//   subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
// What was kept: the 4x4 path of that shader as it runs for RGBA texels (kind 0): constant block -> void extent; BC2 / BC3 block with
// constant alpha -> "direct" block (the BC colour endpoints as 8-bit RGBA endpoints, the BC indices as 2-bit weights, no search);
// otherwise one partition, full 4x4 weight grid, the single block mode of its class (RGB: mode 595, 5-bit weights; RGBA: mode 594,
// 20-level weights; RGBA with alpha on a second weight plane: mode 1090, 2-bit weights), endpoints from a power-iteration line fit,
// least-squares refit against the quantized weights, the class with the smaller error kept. Same arithmetic, same tables.
// What changed: no buffer_reference (texels / tables / BC blocks come from the includer's xbg_astc_texel / xbg_astc_table / the
// caller's uvec4), no 6x6 / 8x8 grid search (dead for 4x4: every 4x4 class has one full-grid candidate), no luminance kinds, and the
// direct path also takes BC1 blocks in four-colour mode (c0 > c1: opaque, the same 2-bit index meaning as BC2 / BC3).
//
// The includer defines, before including this file:
//   vec4 xbg_astc_texel(uint i)   texel i (0..15, row-major) of the block, RGBA 0..255 as floats
//   uint xbg_astc_table(uint i)   word i of xbg_bc_astc_tables (2569 words)
//   XBG_ASTC_CANDS                 candidates per class: 3 (engine default) or 1 (GameNative's), a specialization constant on the GPU
// and on the host XBG_HOST plus glsl_shim.h with the float types (tests/bc_astc_encode_test.cpp).
#ifndef XBG_BC_ASTC_GLSL
#define XBG_BC_ASTC_GLSL

#ifndef XBG_DONT_UNROLL
#ifdef XBG_HOST
#define XBG_DONT_UNROLL
#else
#extension GL_EXT_control_flow_attributes : require
#define XBG_DONT_UNROLL [[dont_unroll]]
#endif
#endif

// Tables (xbg_bc_astc_tables.h): byte tables packed four to a word.
#define XBG_AT_TRIT_ENC 0u
#define XBG_AT_QUINT_ENC 61u
#define XBG_AT_WUNQ 93u
#define XBG_AT_WQUANT 189u
#define XBG_AT_CUNQ 393u
#define XBG_AT_CQUANT 1481u
uint xbg_at_byte(uint off, uint i) { return (xbg_astc_table(off + (i >> 2u)) >> ((i & 3u) * 8u)) & 255u; }

// ISE range q (0..20 = 2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256 levels): levels | bits << 16 | trits << 24 | quints << 28
uint xbg_astc_qr(uint q)
{
    uint r = 65538u;
    if (q == 1u) r = 16777219u; else if (q == 2u) r = 131076u; else if (q == 3u) r = 268435461u; else if (q == 4u) r = 16842758u;
    else if (q == 5u) r = 196616u; else if (q == 6u) r = 268501002u; else if (q == 7u) r = 16908300u; else if (q == 8u) r = 262160u;
    else if (q == 9u) r = 268566548u; else if (q == 10u) r = 16973848u; else if (q == 11u) r = 327712u; else if (q == 12u) r = 268632104u;
    else if (q == 13u) r = 17039408u; else if (q == 14u) r = 393280u; else if (q == 15u) r = 268697680u; else if (q == 16u) r = 17104992u;
    else if (q == 17u) r = 458880u; else if (q == 18u) r = 268763296u; else if (q == 19u) r = 17170624u; else if (q == 20u) r = 524544u;
    return r;
}
uint xbg_qr_bits(uint q) { return (xbg_astc_qr(q) >> 16u) & 255u; }
bool xbg_qr_trits(uint q) { return ((xbg_astc_qr(q) >> 24u) & 15u) != 0u; }
bool xbg_qr_quints(uint q) { return (xbg_astc_qr(q) >> 28u) != 0u; }
uint xbg_ise_size(uint n, uint q)
{
    uint b = xbg_qr_bits(q);
    return n * b + (xbg_qr_trits(q) ? (n * 8u + 4u) / 5u : 0u) + (xbg_qr_quints(q) ? (n * 7u + 2u) / 3u : 0u);
}
uint xbg_wunq(uint q, uint v) { return xbg_at_byte(XBG_AT_WUNQ, q * 32u + v); }
uint xbg_wquant(uint q, uint t) { return xbg_at_byte(XBG_AT_WQUANT, q * 68u + t); }
uint xbg_cunq(uint q, uint v) { return xbg_at_byte(XBG_AT_CUNQ, (q - 4u) * 256u + v); }
uint xbg_cquant(uint q, uint x) { return xbg_at_byte(XBG_AT_CQUANT, (q - 4u) * 256u + x); }

// The block being written (128 bits, bit 0 = bit 0 of word 0).
uint xbg_ob[4];
void xbg_ob_clear() { xbg_ob[0] = 0u; xbg_ob[1] = 0u; xbg_ob[2] = 0u; xbg_ob[3] = 0u; }
void xbg_setbits(uint pos, uint n, uint v)
{
    if (n == 0u)
        return;
    uint word = pos >> 5u, off = pos & 31u;
    xbg_ob[word] |= v << off;
    if (off + n > 32u)
        xbg_ob[word + 1u] |= v >> (32u - off);
}
// n bits of v stored downwards from bit 127 - pos (weights)
void xbg_setrev(uint pos, uint n, uint v)
{
    XBG_DONT_UNROLL for (uint b = 0u; b < n; b++)
        if (((v >> b) & 1u) != 0u)
            xbg_setbits(127u - pos - b, 1u, 1u);
}

// Integer sequence encoding into a temporary bit buffer.
uint xbg_ib[4];
uint xbg_ipos;
uint xbg_ise_vals[64];
void xbg_ibits(uint n, uint v)
{
    if (n == 0u)
        return;
    uint word = xbg_ipos >> 5u, off = xbg_ipos & 31u;
    uint m = v & ((1u << n) - 1u);
    xbg_ib[word] |= m << off;
    if (off + n > 32u)
        xbg_ib[word + 1u] |= m >> (32u - off);
    xbg_ipos += n;
}
void xbg_ise_encode(uint count, uint q)
{
    xbg_ib[0] = 0u; xbg_ib[1] = 0u; xbg_ib[2] = 0u; xbg_ib[3] = 0u;
    xbg_ipos = 0u;
    uint b = xbg_qr_bits(q);
    if (xbg_qr_trits(q)) {
        // five values per group: their low bits interleaved with the 8 bits of the trit block (2, 2, 1, 2, 1)
        XBG_DONT_UNROLL for (uint g = 0u; g < count; g += 5u) {
            uint t[5];
            uint m[5];
            XBG_DONT_UNROLL for (uint j = 0u; j < 5u; j++) {
                t[j] = 0u;
                m[j] = 0u;
                if (g + j < count) {
                    t[j] = xbg_ise_vals[g + j] >> b;
                    m[j] = xbg_ise_vals[g + j] & ((1u << b) - 1u);
                }
            }
            uint T = xbg_at_byte(XBG_AT_TRIT_ENC, t[0] + 3u * t[1] + 9u * t[2] + 27u * t[3] + 81u * t[4]);
            uint ts = 0u;
            XBG_DONT_UNROLL for (uint j = 0u; j < 5u; j++) {
                if (g + j >= count)
                    break;
                uint tb = (j == 2u || j == 4u) ? 1u : 2u;
                xbg_ibits(b, m[j]);
                xbg_ibits(tb, T >> ts);
                ts += tb;
            }
        }
    } else if (xbg_qr_quints(q)) {
        // three values per group: low bits interleaved with the 7 bits of the quint block (3, 2, 2)
        XBG_DONT_UNROLL for (uint g = 0u; g < count; g += 3u) {
            uint qv[3];
            uint m[3];
            XBG_DONT_UNROLL for (uint j = 0u; j < 3u; j++) {
                qv[j] = 0u;
                m[j] = 0u;
                if (g + j < count) {
                    qv[j] = xbg_ise_vals[g + j] >> b;
                    m[j] = xbg_ise_vals[g + j] & ((1u << b) - 1u);
                }
            }
            uint Q = xbg_at_byte(XBG_AT_QUINT_ENC, qv[0] + 5u * qv[1] + 25u * qv[2]);
            uint qs = 0u;
            XBG_DONT_UNROLL for (uint j = 0u; j < 3u; j++) {
                if (g + j >= count)
                    break;
                uint qb = j == 0u ? 3u : 2u;
                xbg_ibits(b, m[j]);
                xbg_ibits(qb, Q >> qs);
                qs += qb;
            }
        }
    } else {
        XBG_DONT_UNROLL for (uint j = 0u; j < count; j++)
            xbg_ibits(b, xbg_ise_vals[j]);
    }
}

// Range of the block's texels (set by xbg_astc_encode before anything else).
vec4 xbg_lo;
vec4 xbg_hi;

// BC1 in four-colour mode (direct 1: colour block in words 0-1) or BC2 / BC3 (direct 2: colour block in words 2-3) with one alpha
// value for the whole block: RGBA direct (CEM 12) with 8-bit endpoints = the BC endpoints expanded from 5:6:5, the BC indices as
// 2-bit weights (0, 1/3, 2/3, 1 -> 0, 21, 43, 64 of 64). GameNative's direct_block for BC2 / BC3, extended to BC1.
bool xbg_astc_direct(uint direct, uvec4 w)
{
    xbg_ob_clear();
    if (xbg_lo.w != xbg_hi.w)
        return false;
    uint c0, c1, idx;
    if (direct == 1u) {
        c0 = w.x & 0xffffu;
        c1 = w.x >> 16u;
        idx = w.y;
        if (c0 <= c1)
            return false;   // three-colour mode (index 3 = black / transparent): searched below
    } else {
        c0 = w.z & 0xffffu;
        c1 = w.z >> 16u;
        idx = w.w;
    }
    uvec3 e0 = uvec3((c0 >> 11u) & 31u, (c0 >> 5u) & 63u, c0 & 31u);
    uvec3 e1 = uvec3((c1 >> 11u) & 31u, (c1 >> 5u) & 63u, c1 & 31u);
    e0 = uvec3((e0.x << 3u) | (e0.x >> 2u), (e0.y << 2u) | (e0.y >> 4u), (e0.z << 3u) | (e0.z >> 2u));
    e1 = uvec3((e1.x << 3u) | (e1.x >> 2u), (e1.y << 2u) | (e1.y >> 4u), (e1.z << 3u) | (e1.z >> 2u));
    // the decoder swaps (and blue-contracts) when the second endpoint sums lower: keep it higher, weights reversed
    bool sw = e1.x + e1.y + e1.z < e0.x + e0.y + e0.z;
    if (sw) {
        uvec3 t = e0;
        e0 = e1;
        e1 = t;
    }
    uint a = uint(xbg_lo.w);
    xbg_setbits(0u, 11u, 0x42u);   // block mode: 4x4 grid, 2-bit weights, one plane
    xbg_setbits(13u, 4u, 12u);     // one partition, CEM 12 (LDR RGBA direct)
    xbg_setbits(17u, 8u, e0.x); xbg_setbits(25u, 8u, e1.x);
    xbg_setbits(33u, 8u, e0.y); xbg_setbits(41u, 8u, e1.y);
    xbg_setbits(49u, 8u, e0.z); xbg_setbits(57u, 8u, e1.z);
    xbg_setbits(65u, 8u, a); xbg_setbits(73u, 8u, a);
    XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++) {
        uint k = (idx >> (2u * i)) & 3u;
        uint q = k == 0u ? 0u : k == 1u ? 3u : k - 1u;
        xbg_setrev(2u * i, 2u, sw ? 3u - q : q);
    }
    return true;
}

// Classes: 3 RGB, 4 RGBA, 5 RGBA with alpha on a second weight plane. Candidate = block mode | grid 4 << 11 | 4 << 15 | weight range << 19.
// k 0 is GameNative's single 4x4 candidate of the class (astc_cand): RGB mode 595 (5-bit weights, 32-level endpoints), RGBA mode 594
// (20-level weights, 32-level endpoints), dual plane mode 1090 (2-bit weights, 48-level endpoints). XBG_ASTC_CANDS = 3 (the includer's
// constant; default of the engine) adds for RGB / RGBA (deviation, bc_astc_encode_test: BC7 content +5..13 dB) mode 578 (4-bit weights:
// RGB 192-level / RGBA 48-level endpoints) and mode 83 (3-bit weights: RGB 256 / RGBA 192-level endpoints); 1 = GameNative's set only.
uint xbg_cand(uint mode, uint wq) { return mode | (4u << 11u) | (4u << 15u) | (wq << 19u); }
uint xbg_cls_cand(uint cls) { return cls == 3u ? 5907027u : cls == 4u ? 4858450u : 17966146u; }
uint xbg_cls_ncand(uint cls) { return XBG_ASTC_CANDS >= 3u && cls != 5u ? 3u : 1u; }
uint xbg_cls_cand_k(uint cls, uint k) { return k == 0u ? xbg_cls_cand(cls) : k == 1u ? xbg_cand(578u, 8u) : xbg_cand(83u, 5u); }
uint xbg_mode_bits_cem(uint cls) { return cls == 3u ? 8u : 12u; }   // CEM 8 RGB direct, 12 RGBA direct
uint xbg_nvals(uint cls) { return cls == 3u ? 6u : 8u; }
vec4 xbg_cls_mask(uint cls) { return cls == 4u ? vec4(1.0, 1.0, 1.0, 1.0) : vec4(1.0, 1.0, 1.0, 0.0); }
uint xbg_full_cq(uint cls, uint cand)
{
    bool dual = cls == 5u;
    uint wbits = xbg_ise_size(16u * (dual ? 2u : 1u), (cand >> 19u) & 31u);
    uint cbits = 128u - 17u - wbits - (dual ? 2u : 0u);
    uint cq = 20u;
    while (cq > 4u && xbg_ise_size(xbg_nvals(cls), cq) > cbits)
        cq--;
    return cq;
}

// quantized endpoints (ISE values) and their unquantized 8-bit values
uvec4 xbg_qe0;
uvec4 xbg_qe1;
uvec4 xbg_ue0;
uvec4 xbg_ue1;
void xbg_quantize_endpoints(uint cq, vec4 e0, vec4 e1)
{
    XBG_DONT_UNROLL for (uint c = 0u; c < 4u; c++) {
        xbg_qe0[c] = xbg_cquant(cq, uint(clamp(e0[c], 0.0, 255.0) + 0.5));
        xbg_qe1[c] = xbg_cquant(cq, uint(clamp(e1[c], 0.0, 255.0) + 0.5));
        xbg_ue0[c] = xbg_cunq(cq, xbg_qe0[c]);
        xbg_ue1[c] = xbg_cunq(cq, xbg_qe1[c]);
    }
}

// One class with its full-grid block mode, reading the texels again in each pass (no per-texel arrays). Returns the block error
// (sum of squared channel errors of the channels the class codes); with pack set, writes the block into xbg_ob instead.
float xbg_encode_full(uint cls, uint cand, bool pack)
{
    uint mode = cand & 2047u, wq = (cand >> 19u) & 31u;
    uint cq = xbg_full_cq(cls, cand);
    bool dual = cls == 5u;
    uint planes = dual ? 2u : 1u;
    vec4 m = xbg_cls_mask(cls);
    uint cs = dual ? 3u : 4u;
    vec4 m2 = vec4(0.0);
    if (dual)
        m2[cs] = 1.0;

    // principal axis of the channels on plane 1 (power iteration on the covariance), endpoints at the extreme projections
    vec4 mean = vec4(0.0);
    XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++)
        mean += xbg_astc_texel(i);
    mean = mean / 16.0;
    mat4 cov = mat4(0.0);
    XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++) {
        vec4 d = (xbg_astc_texel(i) - mean) * m;
        cov += outerProduct(d, d);
    }
    vec4 a = (xbg_hi - xbg_lo) * m;
    XBG_DONT_UNROLL for (int it = 0; it < 8; it++) {
        vec4 n = cov * a;
        float mx = max(max(abs(n.x), abs(n.y)), max(abs(n.z), abs(n.w)));
        if (mx <= 0.0)
            break;
        a = n / mx;
    }
    float l = dot(a, a);
    float tmin = 0.0, tmax = 0.0;
    if (l > 0.0) {
        tmin = 1e9;
        tmax = -1e9;
        XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++) {
            float t = dot((xbg_astc_texel(i) - mean) * m, a) / l;
            tmin = min(tmin, t);
            tmax = max(tmax, t);
        }
    }
    vec4 e0 = mean + a * tmin, e1 = mean + a * tmax;
    if (dual) {
        e0[cs] = xbg_lo[cs];
        e1[cs] = xbg_hi[cs];
    }

    // weights against those endpoints, quantized; least-squares endpoints for them
    vec4 d0 = (e1 - e0) * m, d1 = (e1 - e0) * m2;
    float l0 = dot(d0, d0), l1 = dot(d1, d1);
    vec2 aa = vec2(0.0), ab = vec2(0.0), bb = vec2(0.0);
    vec4 ax0 = vec4(0.0), bx0 = vec4(0.0), ax1 = vec4(0.0), bx1 = vec4(0.0);
    XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++) {
        vec4 t = xbg_astc_texel(i);
        float id = l0 > 0.0 ? clamp(dot((t - e0) * m, d0) / l0, 0.0, 1.0) : 0.0;
        float w = float(xbg_wunq(wq, xbg_wquant(wq, uint(id * 64.0 + 0.5)))) / 64.0;
        aa.x += (1.0 - w) * (1.0 - w);
        ab.x += (1.0 - w) * w;
        bb.x += w * w;
        ax0 += (1.0 - w) * t;
        bx0 += w * t;
        if (dual) {
            id = l1 > 0.0 ? clamp(dot((t - e0) * m2, d1) / l1, 0.0, 1.0) : 0.0;
            w = float(xbg_wunq(wq, xbg_wquant(wq, uint(id * 64.0 + 0.5)))) / 64.0;
            aa.y += (1.0 - w) * (1.0 - w);
            ab.y += (1.0 - w) * w;
            bb.y += w * w;
            ax1 += (1.0 - w) * t;
            bx1 += w * t;
        }
    }
    XBG_DONT_UNROLL for (uint c = 0u; c < 4u; c++) {
        bool in1 = m[c] > 0.0, in2 = dual && c == cs;
        if (!in1 && !in2)
            continue;
        uint p = in2 ? 1u : 0u;
        float det = aa[p] * bb[p] - ab[p] * ab[p];
        if (abs(det) < 1e-6)
            continue;
        float ax = in2 ? ax1[c] : ax0[c], bx = in2 ? bx1[c] : bx0[c];
        e0[c] = clamp((ax * bb[p] - bx * ab[p]) / det, 0.0, 255.0);
        e1[c] = clamp((bx * aa[p] - ax * ab[p]) / det, 0.0, 255.0);
    }
    xbg_quantize_endpoints(cq, e0, e1);
    // blue contraction is taken when the second endpoint sums lower: swap so it never is
    if (xbg_ue1.x + xbg_ue1.y + xbg_ue1.z < xbg_ue0.x + xbg_ue0.y + xbg_ue0.z) {
        uvec4 t = xbg_qe0; xbg_qe0 = xbg_qe1; xbg_qe1 = t;
        t = xbg_ue0; xbg_ue0 = xbg_ue1; xbg_ue1 = t;
    }

    // final weights against the quantized endpoints
    vec4 de0 = vec4(xbg_ue0), de1 = vec4(xbg_ue1);
    d0 = (de1 - de0) * m;
    d1 = (de1 - de0) * m2;
    l0 = dot(d0, d0);
    l1 = dot(d1, d1);
    bool plain = !xbg_qr_trits(wq) && !xbg_qr_quints(wq);
    uint wb = xbg_qr_bits(wq);
    uint nw = 16u * planes;
    if (pack) {
        xbg_ob_clear();
        xbg_setbits(0u, 11u, mode);
        xbg_setbits(13u, 4u, xbg_mode_bits_cem(cls));
        uint n = 0u;
        XBG_DONT_UNROLL for (uint c = 0u; c < (cls == 3u ? 3u : 4u); c++) {
            xbg_ise_vals[n] = xbg_qe0[c];
            xbg_ise_vals[n + 1u] = xbg_qe1[c];
            n += 2u;
        }
        xbg_ise_encode(n, cq);
        XBG_DONT_UNROLL for (uint b = 0u; b < xbg_ipos; b++)
            if (((xbg_ib[b >> 5u] >> (b & 31u)) & 1u) != 0u)
                xbg_setbits(17u + b, 1u, 1u);
        if (dual)
            xbg_setbits(128u - xbg_ise_size(nw, wq) - 2u, 2u, cs);
    }
    float err = 0.0;
    XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++) {
        vec4 t = xbg_astc_texel(i);
        float id = l0 > 0.0 ? clamp(dot((t - de0) * m, d0) / l0, 0.0, 1.0) : 0.0;
        uint v0 = xbg_wquant(wq, uint(id * 64.0 + 0.5)), v1 = 0u;
        float w0 = float(xbg_wunq(wq, v0)) / 64.0, w1 = 0.0;
        if (dual) {
            id = l1 > 0.0 ? clamp(dot((t - de0) * m2, d1) / l1, 0.0, 1.0) : 0.0;
            v1 = xbg_wquant(wq, uint(id * 64.0 + 0.5));
            w1 = float(xbg_wunq(wq, v1)) / 64.0;
        }
        if (pack) {
            XBG_DONT_UNROLL for (uint p = 0u; p < planes; p++) {
                uint v = p == 0u ? v0 : v1, k = i * planes + p;
                if (plain) {
                    XBG_DONT_UNROLL for (uint b = 0u; b < wb; b++)
                        if (((v >> b) & 1u) != 0u)
                            xbg_setbits(127u - (k * wb + b), 1u, 1u);
                } else {
                    xbg_ise_vals[k] = v;
                }
            }
        } else {
            XBG_DONT_UNROLL for (uint c = 0u; c < 4u; c++) {
                bool in2 = dual && c == cs;
                if (!(m[c] > 0.0) && !in2)
                    continue;
                float v = mix(float(xbg_ue0[c]), float(xbg_ue1[c]), in2 ? w1 : w0);
                float e = v - t[c];
                err += e * e;
            }
        }
    }
    if (pack && !plain) {
        xbg_ise_encode(nw, wq);
        XBG_DONT_UNROLL for (uint b = 0u; b < xbg_ipos; b++)
            if (((xbg_ib[b >> 5u] >> (b & 31u)) & 1u) != 0u)
                xbg_setbits(127u - b, 1u, 1u);
    }
    return err;
}

// The block of the 16 texels xbg_astc_texel gives. direct: 0 none, 1 BC1 colour block in bc.x / bc.y, 2 BC2 / BC3 colour block in
// bc.z / bc.w (the source BC block of these texels).
uvec4 xbg_astc_encode(uint direct, uvec4 bc)
{
    xbg_lo = vec4(255.0);
    xbg_hi = vec4(0.0);
    XBG_DONT_UNROLL for (uint i = 0u; i < 16u; i++) {
        vec4 t = xbg_astc_texel(i);
        xbg_lo = min(xbg_lo, t);
        xbg_hi = max(xbg_hi, t);
    }
    if (xbg_lo.x == xbg_hi.x && xbg_lo.y == xbg_hi.y && xbg_lo.z == xbg_hi.z && xbg_lo.w == xbg_hi.w) {
        // constant colour: LDR void extent, colour as UNORM16 (v * 257: the top byte is v, so sRGB images read v as well)
        uvec4 u = uvec4(xbg_lo) * 257u;
        return uvec4(0xfffffdfcu, 0xffffffffu, u.x | (u.y << 16u), u.z | (u.w << 16u));
    }
    if (direct != 0u && xbg_astc_direct(direct, bc))
        return uvec4(xbg_ob[0], xbg_ob[1], xbg_ob[2], xbg_ob[3]);
    uint fc0 = xbg_lo.w == 255.0 ? 3u : 4u;
    uint fc1 = xbg_lo.w == 255.0 ? 3u : 5u;
    uint pick = fc0, pick_cand = xbg_cls_cand(fc0);
    if (fc1 > fc0 || xbg_cls_ncand(fc0) > 1u) {
        float best = 1e30;
        XBG_DONT_UNROLL for (uint cls = fc0; cls <= fc1; cls++) {
            XBG_DONT_UNROLL for (uint k = 0u; k < xbg_cls_ncand(cls); k++) {
                uint cand = xbg_cls_cand_k(cls, k);
                float err = xbg_encode_full(cls, cand, false);
                if (!(err >= best)) {
                    best = err;
                    pick = cls;
                    pick_cand = cand;
                }
            }
        }
    }
    xbg_encode_full(pick, pick_cand, true);
    return uvec4(xbg_ob[0], xbg_ob[1], xbg_ob[2], xbg_ob[3]);
}

#endif
