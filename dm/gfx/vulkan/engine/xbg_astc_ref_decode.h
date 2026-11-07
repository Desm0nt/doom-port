// Independent reference ASTC decoder (2D blocks, LDR and HDR profiles), written from the Khronos Data Format Specification
// (chapter "ASTC Compressed Texture Image Formats"), not from astcenc (BUGS_AND_FIXES 2.37). Used by:
//   tools/astc_hdr_scan.cpp            decodes every BC6H -> ASTC 4x4 HDR block of the overlay and compares with astcenc
//   engine/tests/astc_hdr_test.cpp     checks the embedded self-test vectors and the engine's error-block predicate against it
//   xbg_game.cpp astc_sanitize_upload  DM_GFX_ASTC_DIAG: classifies the blocks uploaded into ASTC HDR substitutes (diagnostics)
// Output per texel: four 16-bit values. HDR profile: FP16 bits (LDR endpoints as UNORM16 -> FP16, HDR endpoints LNS -> FP16 with
// the spec's clamp of Inf / NaN results to 0x7BFF; the clamp is counted in AstcRefInfo::lns_clamped). LDR profile: UNORM16 of
// the interpolated value (the top 8 bits are the UNORM8 result). Error blocks: every texel 0xFFFF (HDR: NaN) / magenta (LDR).
#pragma once
#include <stdint.h>
#include <string.h>

struct AstcRefInfo {
    int error;              // 1 = error block (and why: a short reason)
    const char *why;
    int void_extent, void_hdr;
    int partitions, dual, ccs, wx, wy, weight_quant, color_quant, color_values;
    int cem[4];
    int lns_clamped;        // texels x channels whose LNS -> FP16 conversion hit 0x7C00 or above (clamped to 0x7BFF)
    int lns_max;            // the largest interpolated 16-bit LNS value of an HDR channel (-1: none)
};

namespace astcref {

static inline int bit(const uint8_t *b, int i) { return (b[i >> 3] >> (i & 7)) & 1; }
static inline unsigned bits(const uint8_t *b, int at, int n) { unsigned v = 0; for (int i = 0; i < n; i++) v |= unsigned(bit(b, at + i)) << i; return v; }

// ISE ranges by index: trits / quints / bits. Index 0..20 = ranges 2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256.
struct Range { int trits, quints, nbits, range; };
static const Range kRanges[21] = {
    {0,0,1,2},{1,0,0,3},{0,0,2,4},{0,1,0,5},{1,0,1,6},{0,0,3,8},{0,1,1,10},{1,0,2,12},{0,0,4,16},{0,1,2,20},{1,0,3,24},{0,0,5,32},
    {0,1,3,40},{1,0,4,48},{0,0,6,64},{0,1,4,80},{1,0,5,96},{0,0,7,128},{0,1,5,160},{1,0,6,192},{0,0,8,256}};
static inline int ise_bits(int count, int q) {
    const Range &r = kRanges[q];
    return count * r.nbits + (r.trits ? (8 * count + 4) / 5 : 0) + (r.quints ? (7 * count + 2) / 3 : 0);
}
// Decodes `count` values of range q from the bit stream `src` starting at `at` (bit order as stored).
// Bits past the end of the sequence (the missing tail of a last, partial trit / quint group) read as 0.
static inline void ise_decode(const uint8_t *src, int at, int count, int q, int *out) {
    const Range &r = kRanges[q];
    const int n = r.nbits, end = at + ise_bits(count, q);
    auto bits = [&](const uint8_t *b, int from, int k) { unsigned v = 0; for (int i = 0; i < k; i++) if (from + i < end) v |= unsigned(bit(b, from + i)) << i; return v; };
    if (r.trits) {
        for (int base = 0; base < count; base += 5) {
            int m[5] = {0, 0, 0, 0, 0}; unsigned T = 0; int p = at;
            // m0 T[1:0] m1 T[3:2] m2 T[4] m3 T[6:5] m4 T[7]
            const int tb[5] = {2, 2, 1, 2, 1}; int ts = 0;
            for (int k = 0; k < 5; k++) {
                m[k] = int(bits(src, p, n)); p += n;
                T |= bits(src, p, tb[k]) << ts; p += tb[k]; ts += tb[k];
            }
            at = p;
            int t[5]; unsigned C;
            if (((T >> 2) & 7) == 7) { C = (((T >> 5) & 7) << 2) | (T & 3); t[4] = t[3] = 2; }
            else {
                C = T & 0x1F;
                if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = (T >> 7) & 1; }
                else { t[4] = (T >> 7) & 1; t[3] = (T >> 5) & 3; }
            }
            if ((C & 3) == 3) { t[2] = 2; t[1] = (C >> 4) & 1; t[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1)); }
            else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
            else { t[2] = (C >> 4) & 1; t[1] = (C >> 2) & 3; t[0] = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1)); }
            for (int k = 0; k < 5 && base + k < count; k++) out[base + k] = (t[k] << n) | m[k];
        }
    } else if (r.quints) {
        for (int base = 0; base < count; base += 3) {
            int m[3]; unsigned Q = 0; int p = at;
            // m0 Q[2:0] m1 Q[4:3] m2 Q[6:5]
            const int qb[3] = {3, 2, 2}; int qs = 0;
            for (int k = 0; k < 3; k++) { m[k] = int(bits(src, p, n)); p += n; Q |= bits(src, p, qb[k]) << qs; p += qb[k]; qs += qb[k]; }
            at = p;
            int qv[3];
            if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
                qv[2] = int(((Q & 1) << 2) | ((((Q >> 4) & 1) & ~(Q & 1)) << 1) | (((Q >> 3) & 1) & ~(Q & 1)));
                qv[1] = qv[0] = 4;
            } else {
                unsigned C;
                if (((Q >> 1) & 3) == 3) { qv[2] = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
                else { qv[2] = (Q >> 5) & 3; C = Q & 0x1F; }
                if ((C & 7) == 5) { qv[1] = 4; qv[0] = (C >> 3) & 3; }
                else { qv[1] = (C >> 3) & 3; qv[0] = C & 7; }
            }
            for (int k = 0; k < 3 && base + k < count; k++) out[base + k] = (qv[k] << n) | m[k];
        }
    } else {
        for (int k = 0; k < count; k++) { out[k] = int(bits(src, at, n)); at += n; }
    }
}

// Colour unquantization to 8 bits (spec table "Color Unquantization Parameters").
static inline int unq_color(int v, int q) {
    const Range &r = kRanges[q];
    const int n = r.nbits;
    if (!r.trits && !r.quints) {   // bit replication
        int x = v, have = n, out = 0, need = 8;
        while (need > 0) { if (need >= have) { out = (out << have) | x; need -= have; } else { out = (out << need) | (x >> (have - need)); need = 0; } }
        return out;
    }
    const int D = v >> n, m = v & ((1 << n) - 1);
    const int a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1, d = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
    const int A = a ? 0x1FF : 0;
    int B = 0, C = 0;
    auto pat = [](const char *s, int bb, int cc, int dd, int ee, int ff) {   // 9-character MSB-first pattern of b..f and 0
        int x = 0;
        for (const char *p = s; *p; p++) { int v2 = 0; switch (*p) { case 'b': v2 = bb; break; case 'c': v2 = cc; break; case 'd': v2 = dd; break; case 'e': v2 = ee; break; case 'f': v2 = ff; break; default: v2 = 0; } x = (x << 1) | v2; }
        return x;
    };
    switch (r.range) {
    case 6:   B = 0; C = 204; break;
    case 12:  B = pat("b000b0bb0", b, c, d, e, f); C = 93; break;
    case 24:  B = pat("cb000cbcb", b, c, d, e, f); C = 44; break;
    case 48:  B = pat("dcb000dcb", b, c, d, e, f); C = 22; break;
    case 96:  B = pat("edcb000ed", b, c, d, e, f); C = 11; break;
    case 192: B = pat("fedcb000f", b, c, d, e, f); C = 5; break;
    case 10:  B = 0; C = 113; break;
    case 20:  B = pat("b0000bb00", b, c, d, e, f); C = 54; break;
    case 40:  B = pat("cb0000cbc", b, c, d, e, f); C = 26; break;
    case 80:  B = pat("dcb0000dc", b, c, d, e, f); C = 13; break;
    case 160: B = pat("edcb0000e", b, c, d, e, f); C = 6; break;
    default: return 0;
    }
    int T = D * C + B;
    T ^= A;
    T = (A & 0x80) | (T >> 2);
    return T & 0xFF;
}
// Weight unquantization to 0..64 (spec table "Weight Unquantization Parameters").
static inline int unq_weight(int v, int q) {
    const Range &r = kRanges[q];
    const int n = r.nbits;
    int T;
    if (!r.trits && !r.quints) {
        int x = v, have = n, out = 0, need = 6;
        while (need > 0) { if (need >= have) { out = (out << have) | x; need -= have; } else { out = (out << need) | (x >> (have - need)); need = 0; } }
        T = out;
    } else if (n == 0) {
        static const int t3[3] = {0, 32, 63}, q5[5] = {0, 16, 32, 47, 63};
        T = r.trits ? t3[v] : q5[v];
    } else {
        const int D = v >> n, m = v & ((1 << n) - 1);
        const int a = m & 1, b = (m >> 1) & 1, c = (m >> 2) & 1;
        const int A = a ? 0x7F : 0;
        int B = 0, C = 0;
        switch (r.range) {
        case 6:  B = 0; C = 50; break;
        case 10: B = 0; C = 28; break;
        case 12: B = (b << 6) | (b << 2) | (b << 0) /* b000b0b */; C = 23; break;
        case 20: B = (b << 6) | (b << 1) /* b0000b0 */; C = 13; break;
        case 24: B = (c << 6) | (b << 5) | (c << 1) | (b << 0) /* cb000cb */; C = 11; break;
        default: return 0;
        }
        T = D * C + B;
        T ^= A;
        T = (A & 0x20) | (T >> 2);
    }
    if (T > 32) T += 1;
    return T;
}

static inline uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;
    p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3;
    p ^= p << 6; p ^= p >> 17;
    return p;
}
static inline int select_partition(int seed, int x, int y, int z, int count, int small) {
    if (small) { x <<= 1; y <<= 1; z <<= 1; }
    seed += (count - 1) * 1024;
    const uint32_t rnum = hash52(uint32_t(seed));
    int s[13];
    s[1] = rnum & 0xF; s[2] = (rnum >> 4) & 0xF; s[3] = (rnum >> 8) & 0xF; s[4] = (rnum >> 12) & 0xF;
    s[5] = (rnum >> 16) & 0xF; s[6] = (rnum >> 20) & 0xF; s[7] = (rnum >> 24) & 0xF; s[8] = (rnum >> 28) & 0xF;
    s[9] = (rnum >> 18) & 0xF; s[10] = (rnum >> 22) & 0xF; s[11] = (rnum >> 26) & 0xF; s[12] = ((rnum >> 30) | (rnum << 2)) & 0xF;
    for (int i = 1; i <= 12; i++) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = (count == 3) ? 6 : 5; }
    else { sh1 = (count == 3) ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    const int sh3 = (seed & 0x10) ? sh1 : sh2;
    s[1] >>= sh1; s[2] >>= sh2; s[3] >>= sh1; s[4] >>= sh2; s[5] >>= sh1; s[6] >>= sh2; s[7] >>= sh1; s[8] >>= sh2;
    s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3; s[12] >>= sh3;
    int a = s[1] * x + s[2] * y + s[11] * z + int(rnum >> 14);
    int b = s[3] * x + s[4] * y + s[12] * z + int(rnum >> 10);
    int c = s[5] * x + s[6] * y + s[9] * z + int(rnum >> 6);
    int d = s[7] * x + s[8] * y + s[10] * z + int(rnum >> 2);
    a &= 0x3F; b &= 0x3F; c &= 0x3F; d &= 0x3F;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

static inline int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline void bit_transfer_signed(int &a, int &b) { b >>= 1; b |= a & 0x80; a >>= 1; a &= 0x3F; if (a & 0x20) a -= 0x40; }
static inline void blue_contract(int &r, int &g, int &b) { r = (r + b) >> 1; g = (g + b) >> 1; }

// One endpoint pair: e[0], e[1] = RGBA; hdr[c] = channel c is HDR (12-bit LNS), else LDR 8-bit.
struct Endpoints { int e[2][4]; int hdr[4]; };

static inline void decode_mode7(const int *v, Endpoints &o) {
    int modeval = ((v[0] & 0xC0) >> 6) | ((v[1] & 0x80) >> 5) | ((v[2] & 0x80) >> 4);
    int majcomp, mode;
    if ((modeval & 0xC) != 0xC) { majcomp = modeval >> 2; mode = modeval & 3; }
    else if (modeval != 0xF) { majcomp = modeval & 3; mode = 4; }
    else { majcomp = 0; mode = 5; }
    int red = v[0] & 0x3F, green = v[1] & 0x1F, blue = v[2] & 0x1F, scale = v[3] & 0x1F;
    const int x0 = (v[1] >> 6) & 1, x1 = (v[1] >> 5) & 1, x2 = (v[2] >> 6) & 1, x3 = (v[2] >> 5) & 1, x4 = (v[3] >> 7) & 1, x5 = (v[3] >> 6) & 1, x6 = (v[3] >> 5) & 1;
    const int ohm = 1 << mode;
    if (ohm & 0x30) green |= x0 << 6;
    if (ohm & 0x3A) green |= x1 << 5;
    if (ohm & 0x30) blue |= x2 << 6;
    if (ohm & 0x3A) blue |= x3 << 5;
    if (ohm & 0x3D) scale |= x6 << 5;
    if (ohm & 0x2D) scale |= x5 << 6;
    if (ohm & 0x04) scale |= x4 << 7;
    if (ohm & 0x3B) red |= x4 << 6;
    if (ohm & 0x04) red |= x3 << 6;
    if (ohm & 0x10) red |= x5 << 7;
    if (ohm & 0x0F) red |= x2 << 7;
    if (ohm & 0x05) red |= x1 << 8;
    if (ohm & 0x0A) red |= x0 << 8;
    if (ohm & 0x05) red |= x0 << 9;
    if (ohm & 0x02) red |= x6 << 9;
    if (ohm & 0x01) red |= x3 << 10;
    if (ohm & 0x02) red |= x5 << 10;
    static const int shamts[6] = {1, 1, 2, 3, 4, 5};
    const int sh = shamts[mode];
    red <<= sh; green <<= sh; blue <<= sh; scale <<= sh;
    if (mode != 5) { green = red - green; blue = red - blue; }
    if (majcomp == 1) { int t = red; red = green; green = t; }
    if (majcomp == 2) { int t = red; red = blue; blue = t; }
    o.e[1][0] = clamp_i(red, 0, 0xFFF); o.e[1][1] = clamp_i(green, 0, 0xFFF); o.e[1][2] = clamp_i(blue, 0, 0xFFF);
    o.e[0][0] = clamp_i(red - scale, 0, 0xFFF); o.e[0][1] = clamp_i(green - scale, 0, 0xFFF); o.e[0][2] = clamp_i(blue - scale, 0, 0xFFF);
    o.e[0][3] = o.e[1][3] = 0x780; o.hdr[0] = o.hdr[1] = o.hdr[2] = o.hdr[3] = 1;
}
static inline void decode_mode11_rgb(const int *v, Endpoints &o) {
    const int majcomp = ((v[4] & 0x80) >> 7) | ((v[5] & 0x80) >> 6);
    if (majcomp == 3) {
        o.e[0][0] = v[0] << 4; o.e[0][1] = v[2] << 4; o.e[0][2] = (v[4] & 0x7F) << 5;
        o.e[1][0] = v[1] << 4; o.e[1][1] = v[3] << 4; o.e[1][2] = (v[5] & 0x7F) << 5;
        o.hdr[0] = o.hdr[1] = o.hdr[2] = 1;
        return;
    }
    const int mode = ((v[1] & 0x80) >> 7) | ((v[2] & 0x80) >> 6) | ((v[3] & 0x80) >> 5);
    int va = v[0] | ((v[1] & 0x40) << 2);
    int vb0 = v[2] & 0x3F, vb1 = v[3] & 0x3F, vc = v[1] & 0x3F;
    int vd0 = v[4] & 0x7F, vd1 = v[5] & 0x7F;
    static const int dbits[8] = {7, 6, 7, 6, 5, 6, 5, 6};
    const int db = dbits[mode];
    vd0 &= (1 << db) - 1; vd1 &= (1 << db) - 1;
    if (vd0 & (1 << (db - 1))) vd0 -= 1 << db;
    if (vd1 & (1 << (db - 1))) vd1 -= 1 << db;
    const int x0 = (v[2] >> 6) & 1, x1 = (v[3] >> 6) & 1, x2 = (v[4] >> 6) & 1, x3 = (v[5] >> 6) & 1, x4 = (v[4] >> 5) & 1, x5 = (v[5] >> 5) & 1;
    const int ohm = 1 << mode;
    if (ohm & 0xA4) va |= x0 << 9;
    if (ohm & 0x08) va |= x2 << 9;
    if (ohm & 0x50) va |= x4 << 9;
    if (ohm & 0x50) va |= x5 << 10;
    if (ohm & 0xA0) va |= x1 << 10;
    if (ohm & 0xC0) va |= x2 << 11;
    if (ohm & 0x04) vc |= x1 << 6;
    if (ohm & 0xE8) vc |= x3 << 6;
    if (ohm & 0x20) vc |= x2 << 7;
    if (ohm & 0x5B) vb0 |= x0 << 6;
    if (ohm & 0x5B) vb1 |= x1 << 6;
    if (ohm & 0x12) vb0 |= x2 << 7;
    if (ohm & 0x12) vb1 |= x3 << 7;
    const int sh = (mode >> 1) ^ 3;
    va <<= sh; vb0 <<= sh; vb1 <<= sh; vc <<= sh;
    vd0 = vd0 * (1 << sh); vd1 = vd1 * (1 << sh);
    int r1 = clamp_i(va, 0, 0xFFF), g1 = clamp_i(va - vb0, 0, 0xFFF), b1 = clamp_i(va - vb1, 0, 0xFFF);
    int r0 = clamp_i(va - vc, 0, 0xFFF), g0 = clamp_i(va - vb0 - vc - vd0, 0, 0xFFF), b0 = clamp_i(va - vb1 - vc - vd1, 0, 0xFFF);
    if (majcomp == 1) { int t = r0; r0 = g0; g0 = t; t = r1; r1 = g1; g1 = t; }
    if (majcomp == 2) { int t = r0; r0 = b0; b0 = t; t = r1; r1 = b1; b1 = t; }
    o.e[0][0] = r0; o.e[0][1] = g0; o.e[0][2] = b0; o.e[1][0] = r1; o.e[1][1] = g1; o.e[1][2] = b1;
    o.hdr[0] = o.hdr[1] = o.hdr[2] = 1;
}
// Returns 0, or 1 when the endpoint mode is HDR and the profile is LDR (error).
static inline int decode_endpoints(int cem, const int *v, Endpoints &o, int hdr_profile) {
    memset(&o, 0, sizeof o);
    int a, b, c, d, e, f;
    switch (cem) {
    case 0: for (int i = 0; i < 2; i++) { o.e[i][0] = o.e[i][1] = o.e[i][2] = v[i]; o.e[i][3] = 255; } break;
    case 1: { int L0 = (v[0] >> 2) | (v[1] & 0xC0); int L1 = L0 + (v[1] & 0x3F); if (L1 > 255) L1 = 255;
        o.e[0][0] = o.e[0][1] = o.e[0][2] = L0; o.e[1][0] = o.e[1][1] = o.e[1][2] = L1; o.e[0][3] = o.e[1][3] = 255; } break;
    case 2: { if (!hdr_profile) return 1; int y0, y1;
        if (v[1] >= v[0]) { y0 = v[0] << 4; y1 = v[1] << 4; } else { y0 = (v[1] << 4) + 8; y1 = (v[0] << 4) - 8; }
        o.e[0][0] = o.e[0][1] = o.e[0][2] = y0; o.e[1][0] = o.e[1][1] = o.e[1][2] = y1; o.e[0][3] = o.e[1][3] = 0x780;
        o.hdr[0] = o.hdr[1] = o.hdr[2] = o.hdr[3] = 1; } break;
    case 3: { if (!hdr_profile) return 1; int y0, dd;
        if (v[0] & 0x80) { y0 = ((v[1] & 0xE0) << 4) | ((v[0] & 0x7F) << 2); dd = (v[1] & 0x1F) << 2; }
        else { y0 = ((v[1] & 0xF0) << 4) | ((v[0] & 0x7F) << 1); dd = (v[1] & 0x0F) << 1; }
        int y1 = y0 + dd; if (y1 > 0xFFF) y1 = 0xFFF;
        o.e[0][0] = o.e[0][1] = o.e[0][2] = y0; o.e[1][0] = o.e[1][1] = o.e[1][2] = y1; o.e[0][3] = o.e[1][3] = 0x780;
        o.hdr[0] = o.hdr[1] = o.hdr[2] = o.hdr[3] = 1; } break;
    case 4: for (int i = 0; i < 2; i++) { o.e[i][0] = o.e[i][1] = o.e[i][2] = v[i]; o.e[i][3] = v[2 + i]; } break;
    case 5: { a = v[1]; b = v[0]; bit_transfer_signed(a, b); c = v[3]; d = v[2]; bit_transfer_signed(c, d);
        o.e[0][0] = o.e[0][1] = o.e[0][2] = b; o.e[0][3] = d;
        o.e[1][0] = o.e[1][1] = o.e[1][2] = clamp_i(b + a, 0, 255); o.e[1][3] = clamp_i(d + c, 0, 255); } break;
    case 6: o.e[0][0] = (v[0] * v[3]) >> 8; o.e[0][1] = (v[1] * v[3]) >> 8; o.e[0][2] = (v[2] * v[3]) >> 8; o.e[0][3] = 255;
        o.e[1][0] = v[0]; o.e[1][1] = v[1]; o.e[1][2] = v[2]; o.e[1][3] = 255; break;
    case 7: if (!hdr_profile) return 1; decode_mode7(v, o); break;
    case 8: case 12: {
        const int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        const int a0 = cem == 12 ? v[6] : 255, a1 = cem == 12 ? v[7] : 255;
        if (s1 >= s0) { o.e[0][0] = v[0]; o.e[0][1] = v[2]; o.e[0][2] = v[4]; o.e[0][3] = a0; o.e[1][0] = v[1]; o.e[1][1] = v[3]; o.e[1][2] = v[5]; o.e[1][3] = a1; }
        else { int r = v[1], g = v[3], bb = v[5]; blue_contract(r, g, bb); o.e[0][0] = r; o.e[0][1] = g; o.e[0][2] = bb; o.e[0][3] = a1;
               r = v[0]; g = v[2]; bb = v[4]; blue_contract(r, g, bb); o.e[1][0] = r; o.e[1][1] = g; o.e[1][2] = bb; o.e[1][3] = a0; }
    } break;
    case 9: case 13: {
        a = v[1]; b = v[0]; bit_transfer_signed(a, b); c = v[3]; d = v[2]; bit_transfer_signed(c, d); e = v[5]; f = v[4]; bit_transfer_signed(e, f);
        int ao = 0, ab = 255;
        if (cem == 13) { ao = v[7]; ab = v[6]; bit_transfer_signed(ao, ab); }
        if (a + c + e >= 0) {
            o.e[0][0] = b; o.e[0][1] = d; o.e[0][2] = f; o.e[0][3] = ab;
            o.e[1][0] = clamp_i(b + a, 0, 255); o.e[1][1] = clamp_i(d + c, 0, 255); o.e[1][2] = clamp_i(f + e, 0, 255); o.e[1][3] = cem == 13 ? clamp_i(ab + ao, 0, 255) : 255;
        } else {
            int r = b + a, g = d + c, bb = f + e; blue_contract(r, g, bb);
            o.e[0][0] = clamp_i(r, 0, 255); o.e[0][1] = clamp_i(g, 0, 255); o.e[0][2] = clamp_i(bb, 0, 255); o.e[0][3] = cem == 13 ? clamp_i(ab + ao, 0, 255) : 255;
            r = b; g = d; bb = f; blue_contract(r, g, bb);
            o.e[1][0] = clamp_i(r, 0, 255); o.e[1][1] = clamp_i(g, 0, 255); o.e[1][2] = clamp_i(bb, 0, 255); o.e[1][3] = ab;
        }
    } break;
    case 10: o.e[0][0] = (v[0] * v[3]) >> 8; o.e[0][1] = (v[1] * v[3]) >> 8; o.e[0][2] = (v[2] * v[3]) >> 8; o.e[0][3] = v[4];
        o.e[1][0] = v[0]; o.e[1][1] = v[1]; o.e[1][2] = v[2]; o.e[1][3] = v[5]; break;
    case 11: if (!hdr_profile) return 1; decode_mode11_rgb(v, o); o.e[0][3] = o.e[1][3] = 0x780; o.hdr[3] = 1; break;
    case 14: if (!hdr_profile) return 1; decode_mode11_rgb(v, o); o.e[0][3] = v[6]; o.e[1][3] = v[7]; o.hdr[3] = 0; break;
    case 15: { if (!hdr_profile) return 1; decode_mode11_rgb(v, o);
        int v6 = v[6], v7 = v[7];
        const int mode = ((v6 >> 7) & 1) | ((v7 >> 6) & 2);
        v6 &= 0x7F; v7 &= 0x7F;
        if (mode == 3) { o.e[0][3] = v6 << 5; o.e[1][3] = v7 << 5; }
        else {
            v6 |= (v7 << (mode + 1)) & 0x780;
            v7 &= 0x3F >> mode;
            v7 ^= 0x20 >> mode;
            v7 -= 0x20 >> mode;
            v6 <<= 4 - mode;
            v7 *= 1 << (4 - mode);
            v7 += v6;
            o.e[0][3] = v6; o.e[1][3] = clamp_i(v7, 0, 0xFFF);
        }
        o.hdr[3] = 1; } break;
    }
    return 0;
}

static inline uint16_t lns_to_fp16(int c, int &clamped) {
    const int e = (c >> 11) & 0x1F, m = c & 0x7FF;
    int mt;
    if (m < 512) mt = 3 * m;
    else if (m >= 1536) mt = 5 * m - 2048;
    else mt = 4 * m - 512;
    int r = (e << 10) + (mt >> 3);
    if (r >= 0x7C00) { r = 0x7BFF; ++clamped; }
    return uint16_t(r);
}
static inline uint16_t f32_to_f16_rne(float f) {   // round to nearest even, finite inputs in [0, 1]
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000; int exp = int((x >> 23) & 0xFF) - 127 + 15; uint32_t man = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7C00);
    if (exp <= 0) {   // subnormal
        if (exp < -10) return uint16_t(sign);
        man |= 0x800000; const int shift = 14 - exp; uint32_t h = man >> shift; const uint32_t rem = man & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (man >> 13); const uint32_t rem = man & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
    return uint16_t(sign | h);
}
// UNORM16 -> FP16 as the value C / 65535 rounded to nearest (0xFFFF -> exactly 1.0).
static inline uint16_t unorm16_to_fp16(int c) { return f32_to_f16_rne(float(double(c) / 65535.0)); }

// Decodes one 4x4 (or bw x bh, both <= 12) block. out: bw*bh texels x 4 channels, row-major. Returns info.error.
static inline int decode_block(const uint8_t blk[16], int bw, int bh, int hdr_profile, uint16_t *out, AstcRefInfo *info_out = nullptr) {
    AstcRefInfo info; memset(&info, 0, sizeof info); info.lns_max = -1;
    const int texels = bw * bh;
    auto fail = [&](const char *why) {
        info.error = 1; info.why = why;
        for (int i = 0; i < texels; i++) {
            if (hdr_profile) { out[4 * i + 0] = out[4 * i + 1] = out[4 * i + 2] = out[4 * i + 3] = 0xFFFF; }
            else { out[4 * i + 0] = 0xFFFF; out[4 * i + 1] = 0; out[4 * i + 2] = 0xFFFF; out[4 * i + 3] = 0xFFFF; }
        }
        if (info_out) *info_out = info;
        return 1;
    };
    const unsigned mode = bits(blk, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {   // void extent
        info.void_extent = 1; info.void_hdr = (mode >> 9) & 1;
        if (bits(blk, 10, 2) != 3) return fail("void-extent reserved bits");
        const unsigned s0 = bits(blk, 12, 13), s1 = bits(blk, 25, 13), t0 = bits(blk, 38, 13), t1 = bits(blk, 51, 13);
        const bool all1 = s0 == 0x1FFF && s1 == 0x1FFF && t0 == 0x1FFF && t1 == 0x1FFF;
        if (!all1 && (s0 >= s1 || t0 >= t1)) return fail("void-extent coordinates");
        if (info.void_hdr && !hdr_profile) return fail("HDR void extent in LDR profile");
        uint16_t c[4]; for (int k = 0; k < 4; k++) c[k] = uint16_t(bits(blk, 64 + 16 * k, 16));
        for (int i = 0; i < texels; i++) for (int k = 0; k < 4; k++)
            out[4 * i + k] = hdr_profile ? (info.void_hdr ? c[k] : unorm16_to_fp16(c[k])) : c[k];
        if (info_out) *info_out = info;
        return 0;
    }
    // block mode (2D)
    int wx = 0, wy = 0, R, H = (mode >> 9) & 1, D = (mode >> 10) & 1;
    const int A = (mode >> 5) & 3;
    if (mode & 3) {
        R = ((mode >> 4) & 1) | ((mode & 3) << 1);
        int B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
        case 0: wx = B + 4; wy = A + 2; break;
        case 1: wx = B + 8; wy = A + 2; break;
        case 2: wx = A + 2; wy = B + 8; break;
        case 3: B &= 1; if (mode & 0x100) { wx = B + 2; wy = A + 2; } else { wx = A + 2; wy = B + 6; } break;
        }
    } else {
        R = ((mode >> 4) & 1) | (((mode >> 2) & 3) << 1);
        if (((mode >> 2) & 3) == 0) return fail("reserved block mode");
        const int B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
        case 0: wx = 12; wy = A + 2; break;
        case 1: wx = A + 2; wy = 12; break;
        case 2: wx = A + 6; wy = B + 6; D = 0; H = 0; break;
        case 3: if (((mode >> 5) & 3) == 0) { wx = 6; wy = 10; } else if (((mode >> 5) & 3) == 1) { wx = 10; wy = 6; } else return fail("reserved block mode"); break;
        }
    }
    const int wq = (R - 2) + 6 * H;   // weight range index 0..11 (ranges 2..32)
    info.wx = wx; info.wy = wy; info.dual = D; info.weight_quant = wq;
    if (wx > bw || wy > bh) return fail("weight grid larger than the block");
    const int nweights = wx * wy * (D + 1);
    if (nweights > 64) return fail("more than 64 weights");
    const int wbits = ise_bits(nweights, wq);
    if (wbits < 24 || wbits > 96) return fail("weight bits outside 24..96");
    const int parts = int(bits(blk, 11, 2)) + 1;
    info.partitions = parts;
    if (parts == 4 && D) return fail("dual plane with 4 partitions");
    int below = 128 - wbits;
    int cem[4] = {0, 0, 0, 0}, config_end, seed = 0;
    if (parts == 1) { cem[0] = int(bits(blk, 13, 4)); config_end = 17; }
    else {
        seed = int(bits(blk, 13, 10));
        const unsigned low = bits(blk, 23, 6);
        config_end = 29;
        if ((low & 3) == 0) { for (int i = 0; i < parts; i++) cem[i] = int(low >> 2); }
        else {
            const int extra = 3 * parts - 4;
            below -= extra;
            const unsigned enc = low | (bits(blk, below, extra) << 6);
            const int base = int(enc & 3) - 1;
            int p = 2;
            for (int i = 0; i < parts; i++) { cem[i] = ((int((enc >> p) & 1) + base) << 2); p++; }
            for (int i = 0; i < parts; i++) { cem[i] |= int((enc >> p) & 3); p += 2; }
        }
    }
    if (D) { below -= 2; info.ccs = int(bits(blk, below, 2)); }
    int ncolor = 0;
    for (int i = 0; i < parts; i++) { info.cem[i] = cem[i]; ncolor += ((cem[i] >> 2) + 1) * 2; }
    info.color_values = ncolor;
    if (ncolor > 18) return fail("more than 18 colour values");
    const int cbits = below - config_end;
    if (cbits < (13 * ncolor + 4) / 5) return fail("too few colour bits");
    int cq = -1;
    for (int q = 20; q >= 0; q--) if (ise_bits(ncolor, q) <= cbits) { cq = q; break; }
    if (cq < 4) return fail("colour range below 6");
    info.color_quant = cq;
    int cv[18];
    ise_decode(blk, config_end, ncolor, cq, cv);
    for (int i = 0; i < ncolor; i++) cv[i] = unq_color(cv[i], cq);
    Endpoints ep[4];
    for (int i = 0, at = 0; i < parts; i++) {
        if (decode_endpoints(cem[i], cv + at, ep[i], hdr_profile)) return fail("HDR endpoint mode in LDR profile");
        at += ((cem[i] >> 2) + 1) * 2;
    }
    // weights: the stream is stored bit-reversed from bit 127 down
    uint8_t rev[16];
    for (int i = 0; i < 16; i++) { uint8_t x = blk[15 - i], r = 0; for (int k = 0; k < 8; k++) r |= uint8_t(((x >> k) & 1) << (7 - k)); rev[i] = r; }
    int wraw[64], wv[64];
    ise_decode(rev, 0, nweights, wq, wraw);
    for (int i = 0; i < nweights; i++) wv[i] = unq_weight(wraw[i], wq);
    // infill (per plane)
    const int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    const int small = texels < 31;
    for (int t = 0; t < bh; t++) for (int s = 0; s < bw; s++) {
        int w[2];
        for (int pl = 0; pl <= D; pl++) {
            const int cs = Ds * s, ct = Dt * t;
            const int gs = (cs * (wx - 1) + 32) >> 6, gt = (ct * (wy - 1) + 32) >> 6;
            const int js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
            const int v0 = js + jt * wx;
            auto W = [&](int idx, int dx, int dy) -> int {
                const int x = js + dx, y = jt + dy;
                if (x >= wx || y >= wy) return 0;   // multiplied by a zero factor
                (void)idx; return wv[(y * wx + x) * (D + 1) + pl];
            };
            (void)v0;
            const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            w[pl] = (W(0, 0, 0) * w00 + W(0, 1, 0) * w01 + W(0, 0, 1) * w10 + W(0, 1, 1) * w11 + 8) >> 4;
        }
        const int part = parts == 1 ? 0 : select_partition(seed, s, t, 0, parts, small);
        const Endpoints &E = ep[part];
        for (int ch = 0; ch < 4; ch++) {
            const int wt = (D && ch == info.ccs) ? w[1] : w[0];
            int c0, c1;
            if (E.hdr[ch]) { c0 = E.e[0][ch] << 4; c1 = E.e[1][ch] << 4; }
            else { c0 = (E.e[0][ch] << 8) | E.e[0][ch]; c1 = (E.e[1][ch] << 8) | E.e[1][ch]; }
            const int C = (c0 * (64 - wt) + c1 * wt + 32) >> 6;
            uint16_t r;
            if (!hdr_profile) r = uint16_t(C);
            else if (E.hdr[ch]) { r = lns_to_fp16(C, info.lns_clamped); if (C > info.lns_max) info.lns_max = C; }
            else r = C == 0xFFFF ? 0x3C00 : unorm16_to_fp16(C);
            out[4 * (t * bw + s) + ch] = r;
        }
    }
    if (info_out) *info_out = info;
    return 0;
}

}   // namespace astcref
