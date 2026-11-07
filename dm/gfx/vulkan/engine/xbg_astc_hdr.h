// [nonadreno] ASTC 4x4 HDR safety for the BC6H -> ASTC overlay (variant data; docs/BUGS_AND_FIXES.md 2.37, issues #7 #14 #25 #28).
// Pure functions (no device calls), so engine/tests/astc_hdr_test.cpp runs the production code against the independent decoder
// xbg_astc_ref_decode.h:
//
// 1. xbg_astc_block_error: whether 16 bytes are an ASTC "error block" (reserved block mode, impossible weight grid or bit count,
//    too many colour values, HDR endpoints in the LDR profile, bad void extent). The spec decodes an error block in the HDR
//    profile to NaN in every channel (astcenc too: 0xFFFF). An all-zero block is one: memory the game never wrote (empty probe
//    slots, unwritten mips) samples as black through BC6H but as NaN through ASTC HDR, and a NaN in a light probe spreads
//    through lighting and TAA history as black rectangles. xbg_astc_sanitize replaces such blocks in ASTC HDR uploads with a
//    void-extent block of black (what BC6H gives for zero memory); valid blocks are never touched. Engine: xbg_game.cpp
//    astc_sanitize_upload, DM_GFX_ASTC_SANITIZE.
// 2. The GPU self-test plan (xbg_astc_hdr_plan) and verdict (xbg_astc_hdr_judge): known ASTC 4x4 HDR blocks (xbg_astc_hdr_data.h,
//    real overlay blocks and blocks encoded from extreme BC6H values) uploaded into a cube array of the engine's format, sampled
//    with texelFetch (2D array view), through the cube array view (direction to the texel centre, the face selection the game's
//    shaders use) and with linear filtering; compared with the expected FP16 values. The Vulkan side is xbg_astc_hdr_vk.h,
//    called from android/vulkan_present.cpp before the overlay variant is mounted; a failure mounts the nohdr variant (BC6H
//    stays on the GPU BC decoder). DM_GFX_ASTC_HDR_SELFTEST.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#ifndef XBG_ASTC_HDR_NO_DATA   // tools/astc_hdr_scan.cpp (which helps generate it) builds without the vectors
#include "xbg_astc_hdr_data.h"
#endif

// ---------------------------------------------------------------- error blocks
// Bit count of `count` integer-sequence values of ISE range index q (0..20 = 2,3,4,5,6,8,10,12,16,20,24,32,...,256).
static inline int xbg_astc_ise_bits(int count, int q) {
    static const uint8_t kind[21] = {0,1,0,2,1,0,2,1,0,2,1,0,2,1,0,2,1,0,2,1,0};   // 0 bits, 1 trits, 2 quints
    static const uint8_t nb[21] = {1,0,2,0,1,3,1,2,4,2,3,5,3,4,6,4,5,7,5,6,8};
    return count * nb[q] + (kind[q] == 1 ? (8 * count + 4) / 5 : kind[q] == 2 ? (7 * count + 2) / 3 : 0);
}
static inline uint32_t xbg_astc_bits(const uint8_t *b, int at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= uint32_t((b[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
    return v;
}
// 1 = the block is an ASTC error block for a 4x4 footprint in the given profile (hdr: the *_SFLOAT formats), else 0.
static inline int xbg_astc_block_error(const uint8_t *b, int hdr) {
    const uint32_t mode = xbg_astc_bits(b, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {                                   // void extent
        if (xbg_astc_bits(b, 10, 2) != 3) return 1;
        const uint32_t s0 = xbg_astc_bits(b, 12, 13), s1 = xbg_astc_bits(b, 25, 13), t0 = xbg_astc_bits(b, 38, 13), t1 = xbg_astc_bits(b, 51, 13);
        if (!(s0 == 0x1FFF && s1 == 0x1FFF && t0 == 0x1FFF && t1 == 0x1FFF) && (s0 >= s1 || t0 >= t1)) return 1;
        return ((mode >> 9) & 1) && !hdr;
    }
    int wx, wy, R, H = (mode >> 9) & 1, D = (mode >> 10) & 1;
    const int A = (mode >> 5) & 3;
    if (mode & 3) {
        R = int(((mode >> 4) & 1) | ((mode & 3) << 1));
        int B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
        case 0: wx = B + 4; wy = A + 2; break;
        case 1: wx = B + 8; wy = A + 2; break;
        case 2: wx = A + 2; wy = B + 8; break;
        default: B &= 1; if (mode & 0x100) { wx = B + 2; wy = A + 2; } else { wx = A + 2; wy = B + 6; } break;
        }
    } else {
        if (((mode >> 2) & 3) == 0) return 1;
        R = int(((mode >> 4) & 1) | (((mode >> 2) & 3) << 1));
        switch ((mode >> 7) & 3) {
        case 0: wx = 12; wy = A + 2; break;
        case 1: wx = A + 2; wy = 12; break;
        case 2: wx = A + 6; wy = int((mode >> 9) & 3) + 6; D = 0; H = 0; break;
        default: if (A == 0) { wx = 6; wy = 10; } else if (A == 1) { wx = 10; wy = 6; } else return 1; break;
        }
    }
    if (wx > 4 || wy > 4) return 1;                                   // a 4x4 block holds at most a 4x4 weight grid
    const int nweights = wx * wy * (D + 1), wbits = xbg_astc_ise_bits(nweights, (R - 2) + 6 * H);   // weight ranges 2..32 = ISE ranges 0..11
    if (nweights > 64 || wbits < 24 || wbits > 96) return 1;
    const int parts = int(xbg_astc_bits(b, 11, 2)) + 1;
    if (parts == 4 && D) return 1;
    int below = 128 - wbits, config_end, cem[4];
    if (parts == 1) { cem[0] = int(xbg_astc_bits(b, 13, 4)); config_end = 17; }
    else {
        const uint32_t low = xbg_astc_bits(b, 23, 6);
        config_end = 29;
        if ((low & 3) == 0) for (int i = 0; i < parts; i++) cem[i] = int(low >> 2);
        else {
            const int extra = 3 * parts - 4;
            below -= extra;
            const uint32_t enc = low | (xbg_astc_bits(b, below, extra) << 6);
            const int base = int(enc & 3) - 1;
            for (int i = 0; i < parts; i++) cem[i] = (int((enc >> (2 + i)) & 1) + base) << 2 | int((enc >> (2 + parts + 2 * i)) & 3);
        }
    }
    if (D) below -= 2;
    int ncolor = 0;
    for (int i = 0; i < parts; i++) {
        ncolor += ((cem[i] >> 2) + 1) * 2;
        if (!hdr && (cem[i] == 2 || cem[i] == 3 || cem[i] == 7 || cem[i] == 11 || cem[i] >= 14)) return 1;
    }
    if (ncolor > 18) return 1;
    return below - config_end < (13 * ncolor + 4) / 5;
}
// The replacement for an error block: void extent (constant colour, no extent) of black. HDR: FP16 (0, 0, 0, 1), what BC6H
// gives for all-zero memory (alpha 1: BC6H has none); LDR: UNORM16 (0, 0, 0, 0), what BC7 / BC3 give for zero memory.
static inline void xbg_astc_black_block(uint8_t *b, int hdr) {
    static const uint8_t h[16] = {0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x00, 0x3C};
    static const uint8_t l[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    memcpy(b, hdr ? h : l, 16);
}
// An HDR void extent whose constant colour is negative, Inf or NaN (no BC6H texel can be: the overlay never writes one).
static inline int xbg_astc_hdr_void_nonfinite(const uint8_t *b) {
    if ((xbg_astc_bits(b, 0, 10) & 0x3FF) != 0x3FC) return 0;
    for (int k = 0; k < 4; k++) { const uint32_t v = xbg_astc_bits(b, 64 + 16 * k, 16); if ((v & 0x8000) || (v & 0x7C00) == 0x7C00) return 1; }
    return 0;
}
// Replaces every error block (HDR also: non-finite void extents) of `bytes` (a multiple of 16) with xbg_astc_black_block.
// Returns the number replaced; *zero counts the all-zero ones among them (never-written memory).
static inline size_t xbg_astc_sanitize(uint8_t *p, size_t bytes, int hdr, size_t *zero = nullptr) {
    size_t n = 0, z = 0;
    for (size_t at = 0; at + 16 <= bytes; at += 16) {
        uint8_t *b = p + at;
        uint64_t lo, hi; memcpy(&lo, b, 8); memcpy(&hi, b + 8, 8);
        // fast path: the overlay's blocks are valid; only blocks that fail the predicate are rewritten
        if (!(lo | hi)) { xbg_astc_black_block(b, hdr); ++n; ++z; continue; }
        if (xbg_astc_block_error(b, hdr) || (hdr && xbg_astc_hdr_void_nonfinite(b))) { xbg_astc_black_block(b, hdr); ++n; }
    }
    if (zero) *zero += z;
    return n;
}

// ---------------------------------------------------------------- EAC R11 (2.37 DM_GFX_ASTC_CPU, tools/astc_ldr_scan.cpp)
// One unsigned EAC R11 block (Khronos Data Format spec, ETC2 / EAC; 8 bytes, big-endian, pixel indices column-major) -> 16 values of
// 11 bits (0..2047), row-major. RG11 = two of them (R then G).
static inline void xbg_eac_r11_block(const uint8_t *b, uint16_t out[16]) {
    static const int8_t mod[16][8] = {{-3,-6,-9,-15,2,5,8,14},{-3,-7,-10,-13,2,6,9,12},{-2,-5,-8,-13,1,4,7,12},{-2,-4,-6,-13,1,3,5,12},
        {-3,-6,-8,-12,2,5,7,11},{-3,-7,-9,-11,2,6,8,10},{-4,-7,-8,-11,3,6,7,10},{-3,-5,-8,-11,2,4,7,10},{-2,-6,-8,-10,1,5,7,9},
        {-2,-5,-8,-10,1,4,7,9},{-2,-4,-8,-10,1,3,7,9},{-2,-5,-7,-10,1,4,6,9},{-3,-4,-7,-10,2,3,6,9},{-1,-2,-3,-10,0,1,2,9},{-4,-6,-8,-9,3,5,7,8},{-3,-5,-7,-9,2,4,6,8}};
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | b[i];
    const int base = int(v >> 56), mult = int(v >> 52 & 15), table = int(v >> 48 & 15);
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) {
        const int m = mod[table][v >> (45 - 3 * (x * 4 + y)) & 7];
        const int c = base * 8 + 4 + (mult ? m * mult * 8 : m);
        out[y * 4 + x] = uint16_t(c < 0 ? 0 : c > 2047 ? 2047 : c);
    }
}

// One unsigned BC4 block -> 16 values scaled to 0..2047 (the EAC R11 scale), row-major.
static inline void xbg_bc4_block_11(const uint8_t *b, uint16_t out[16]) {
    const int r0 = b[0], r1 = b[1];
    int pal[8];
    pal[0] = r0 * 2047 / 255; pal[1] = r1 * 2047 / 255;
    if (r0 > r1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * r0 + i * r1) * 2047 / (7 * 255);
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * r0 + i * r1) * 2047 / (5 * 255); pal[6] = 0; pal[7] = 2047; }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = uint16_t(pal[(idx >> (3 * i)) & 7]);
}
// EAC census (2.37): is this 8-byte block more likely EAC R11 (the overlay's blocks) or BC4 (real BC written at run time)? Both
// formats accept any bits, so validity cannot tell; image content can: under the right decoder the 4x4 texels of a real texture
// vary smoothly, under the wrong one they scatter. Per reading: the second differences along rows and columns (0 for any gradient)
// relative to the block's range (a nearly flat wrong reading is not "smooth" by accident). +1 = EAC clearly smoother, -1 = BC4
// clearly smoother, 0 = undecided (both readings nearly flat, or close). tools/astc_ldr_scan.cpp measures it on the real overlay
// (the EAC substitutes) and on their BC4 / BC5 sources.
static inline int xbg_eac_vs_bc4(const uint8_t *b) {
    uint16_t e[16], c[16];
    xbg_eac_r11_block(b, e); xbg_bc4_block_11(b, c);
    auto rough = [](const uint16_t *p, int &range) {
        int lo = 4095, hi = 0, r = 0;
        for (int i = 0; i < 16; i++) { lo = p[i] < lo ? p[i] : lo; hi = p[i] > hi ? p[i] : hi; }
        for (int y = 0; y < 4; y++) for (int x = 1; x < 3; x++) r += abs(int(p[y * 4 + x - 1]) - 2 * int(p[y * 4 + x]) + int(p[y * 4 + x + 1]));
        for (int x = 0; x < 4; x++) for (int y = 1; y < 3; y++) r += abs(int(p[(y - 1) * 4 + x]) - 2 * int(p[y * 4 + x]) + int(p[(y + 1) * 4 + x]));
        range = hi - lo;
        return double(r) / double(range + 16);
    };
    int re, rc;
    const double ue = rough(e, re), uc = rough(c, rc);
    if (re < 24 && rc < 24) return 0;                 // both nearly flat
    if (ue * 2 < uc) return 1;
    if (uc * 2 < ue) return -1;
    return 0;
}

// ---------------------------------------------------------------- DM_GFX_ASTC_HDR_SELFTEST
// auto (unset, "", "auto", "1"): run when the overlay would mount variant data (ASTC HDR); a failure mounts nohdr.
// off ("0", "off"): never run (variant as chosen). force: also run without an overlay whenever the device has ASTC HDR (log only;
// with an overlay the same as auto). fail: like force, and the verdict is a failure whatever the GPU returned (tests the nohdr
// fallback on a device that passes, e.g. Adreno with DM_GFX_ASTC_OVERLAY=1).
enum { XBG_AHS_OFF = 0, XBG_AHS_AUTO = 1, XBG_AHS_FORCE = 2, XBG_AHS_FAIL = 3 };
static inline int xbg_astc_hdr_selftest_mode(const char *e) {
    if (!e || !*e || !strcmp(e, "auto") || !strcmp(e, "1")) return XBG_AHS_AUTO;
    if (!strcmp(e, "0") || !strcmp(e, "off")) return XBG_AHS_OFF;
    if (!strcmp(e, "force") || !strcmp(e, "2")) return XBG_AHS_FORCE;
    if (!strcmp(e, "fail")) return XBG_AHS_FAIL;
    return XBG_AHS_AUTO;
}
static inline const char *xbg_astc_hdr_selftest_mode_name(int m) {
    return m == XBG_AHS_OFF ? "off" : m == XBG_AHS_FORCE ? "force" : m == XBG_AHS_FAIL ? "fail" : "auto";
}

// ---------------------------------------------------------------- self-test plan
// The test image: 2D, VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK, CUBE_COMPATIBLE, XBG_AHS_SIZE^2, XBG_AHS_LEVELS levels (8, 4, 2: the last
// is a partial block like the probe array's 2x2 mip), XBG_AHS_CUBES * 6 layers. Every 4x4 block of every level and layer holds one
// test block (the vectors in turn); the last block of level 0 of the last layer holds an all-zero block (the error block of
// never-written memory: reported, not judged).
enum { XBG_AHS_SIZE = 8, XBG_AHS_LEVELS = 3, XBG_AHS_CUBES = 2, XBG_AHS_LAYERS = XBG_AHS_CUBES * 6 };
enum { XBG_AHS_FETCH = 0, XBG_AHS_CUBE = 1, XBG_AHS_LINEAR = 2 };
struct XbgAhsQuery { uint32_t x, y, layer, level_kind; };     // level_kind = level | kind << 8; GPU layout (std430 uvec4)
struct XbgAhsRegion { uint32_t level, layer, width, height; size_t offset; };   // one buffer-to-image copy (tight rows of blocks)
struct XbgAhsPlan {
    std::vector<uint8_t> blocks;            // upload bytes, regions in order
    std::vector<XbgAhsRegion> regions;
    std::vector<XbgAhsQuery> queries;
    std::vector<float> expected;            // 4 per query
    std::vector<int> vector_of;             // test vector of each query, -1 = the zero block (informational)
    std::vector<uint8_t> edge;              // 1 = an edge vector's query (reported, not judged): see XBG_AHS_EDGE
};
static inline float xbg_ahs_half(uint16_t h) {
    const int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? ldexpf(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf(float(m | 1024), e - 25);
    return s ? -v : v;
}
// Edge vectors: a channel above this. The whole real overlay stays at or below 1024 (tools/astc_hdr_scan.cpp: the largest
// interpolated LNS is 0xC800, exponent 25, far from the clamp at exponent 31); the hand-built LNS-31 blocks and the 65504 BC6H
// blocks test the spec's clamp to 0x7BFF and the filter's FP16 range, which no real probe reaches. Their failures are logged
// (edge_bad) but do not move the device to nohdr (that costs 4x the BC6H memory for a case the game never shows).
static const float XBG_AHS_EDGE = 16384.0f;
static inline float xbg_ahs_block_max(const uint16_t (*e)[4]) {
    float m = 0;
    for (int t = 0; t < 16; t++) for (int c = 0; c < 3; c++) m = fmaxf(m, xbg_ahs_half(e[t][c]));
    return m;
}
// nvec / vec: the test vectors (production: xbg_astc_hdr_tests; tests may pass their own).
template <class V>
static inline void xbg_astc_hdr_plan(XbgAhsPlan &p, const V *vec, int nvec, bool cube_queries) {
    p = XbgAhsPlan();
    int next = 0;
    for (uint32_t layer = 0; layer < XBG_AHS_LAYERS; layer++) for (uint32_t level = 0; level < XBG_AHS_LEVELS; level++) {
        const uint32_t dim = XBG_AHS_SIZE >> level, nb = (dim + 3) / 4;
        XbgAhsRegion r{level, layer, dim, dim, p.blocks.size()};
        for (uint32_t by = 0; by < nb; by++) for (uint32_t bx = 0; bx < nb; bx++) {
            const bool zero = layer == XBG_AHS_LAYERS - 1 && level == 0 && bx == nb - 1 && by == nb - 1;
            const int v = zero ? -1 : next++ % nvec;
            uint8_t blk[16] = {0};
            if (v >= 0) memcpy(blk, vec[v].block, 16);
            const float vmax = v >= 0 ? xbg_ahs_block_max(vec[v].expected) : 0;
            const uint8_t edge = vmax > XBG_AHS_EDGE;
            p.blocks.insert(p.blocks.end(), blk, blk + 16);
            for (uint32_t t = 0; t < 16; t++) {
                const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x >= dim || y >= dim) continue;
                for (int kind = 0; kind < (cube_queries ? 2 : 1); kind++) {
                    p.queries.push_back({x, y, layer, level | uint32_t(kind) << 8});
                    for (int c = 0; c < 4; c++) p.expected.push_back(v >= 0 ? xbg_ahs_half(vec[v].expected[t][c]) : NAN);
                    p.vector_of.push_back(v); p.edge.push_back(edge);
                }
            }
            // linear filtering at the corners inside the block (level 0 only: whole blocks): the average of the four texels. Not for
            // edge vectors: filtering there tests the filter's FP16 range, which the BC6H path's RGBA16F / E5B9G9R9 images share
            if (v >= 0 && level == 0 && !edge) for (uint32_t cy = 1; cy < 4; cy++) for (uint32_t cx = 1; cx < 4; cx++) {
                p.queries.push_back({bx * 4 + cx, by * 4 + cy, layer, level | uint32_t(XBG_AHS_LINEAR) << 8});
                for (int c = 0; c < 4; c++) {
                    float s = 0;
                    for (int k = 0; k < 4; k++) s += xbg_ahs_half(vec[v].expected[(cy - 1 + (k >> 1)) * 4 + (cx - 1 + (k & 1))][c]);
                    p.expected.push_back(s * 0.25f);
                }
                p.vector_of.push_back(v); p.edge.push_back(0);
            }
        }
        p.regions.push_back(r);
    }
}
// The verdict. got: 4 floats per query as the GPU returned them. Exact decodes are expected for fetch / cube (tolerance 1/256
// relative); linear filtering 2 %. Any Inf / NaN where a finite value is expected fails. Edge vectors count in edge_bad only.
struct XbgAhsVerdict {
    int pass;
    unsigned fetch_bad, cube_bad, linear_bad, nonfinite, checked;
    unsigned edge_bad, edge_nonfinite;   // queries of edge vectors that differ / are Inf or NaN (not judged)
    float max_rel;
    int first_bad;                // test vector of the first failure, -1 none
    unsigned first_kind; float first_got, first_want;
    float zero_block[4];          // what the device returned for the all-zero (error) block
    int zero_block_nan;
};
static inline XbgAhsVerdict xbg_astc_hdr_judge(const XbgAhsPlan &p, const float *got) {
    XbgAhsVerdict v; memset(&v, 0, sizeof v); v.first_bad = -1;
    for (int c = 0; c < 4; c++) v.zero_block[c] = 0;
    bool zero_seen = false;
    for (size_t i = 0; i < p.queries.size(); i++) {
        const unsigned kind = p.queries[i].level_kind >> 8;
        const float *g = got + 4 * i, *w = &p.expected[4 * i];
        if (p.vector_of[i] < 0) {
            if (!zero_seen && kind == XBG_AHS_FETCH) { zero_seen = true; for (int c = 0; c < 4; c++) { v.zero_block[c] = g[c]; if (isnan(g[c])) v.zero_block_nan = 1; } }
            continue;
        }
        bool bad = false, nf = false;
        if (p.edge[i]) {
            for (int c = 0; c < 4; c++) { if (!isfinite(g[c])) nf = true; else if (fabsf(g[c] - w[c]) > fabsf(w[c]) / 256 + 1e-6f) bad = true; }
            v.edge_nonfinite += nf; v.edge_bad += bad || nf;
            continue;
        }
        for (int c = 0; c < 4; c++) {
            ++v.checked;
            if (!isfinite(g[c])) { nf = bad = true; if (v.first_bad < 0) { v.first_bad = p.vector_of[i]; v.first_kind = kind; v.first_got = g[c]; v.first_want = w[c]; } continue; }
            const float err = fabsf(g[c] - w[c]), rel = err / fmaxf(fabsf(w[c]), 1e-4f);
            const float tol = kind == XBG_AHS_LINEAR ? 0.02f : 1.0f / 256;
            if (rel > v.max_rel && err > 1e-6f) v.max_rel = rel;
            if (rel > tol && err > 1e-6f) { bad = true; if (v.first_bad < 0) { v.first_bad = p.vector_of[i]; v.first_kind = kind; v.first_got = g[c]; v.first_want = w[c]; } }
        }
        if (nf) ++v.nonfinite;
        if (bad) { if (kind == XBG_AHS_FETCH) ++v.fetch_bad; else if (kind == XBG_AHS_CUBE) ++v.cube_bad; else ++v.linear_bad; }
    }
    v.pass = !v.fetch_bad && !v.cube_bad && !v.linear_bad && !v.nonfinite && v.checked > 0;
    return v;
}
