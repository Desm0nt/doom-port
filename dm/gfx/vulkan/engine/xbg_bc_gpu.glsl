// [nonadreno] BC1-BC7 block decoders of the GPU BC decode (DM_GFX_BC_DECODE=gpu, issue #6): xbg_bc_decode.comp includes this file.
// The same text compiles as C++ through tests/glsl_shim.h (engine/tests/bc_gpu_decode_test.cpp), which checks every format byte for
// byte against the CPU decoder (xbg_decode_block): the GPU path uploads exactly the bytes the CPU path does (BC4/BC5 SNORM: the CPU
// rounds a float palette, so a GPU whose division is not correctly rounded may differ by 1 in the 16-bit value).
//
// BC6H, BC7 and the bit extraction are ported from Granite, https://github.com/Themaister/Granite at commit 69f450c62dc3,
// assets/shaders/decode/{bc6.comp,bc7.comp,bitextract.h}:
//   Copyright (c) 2020-2026 Hans-Kristian Arntzen. MIT License (full text in third_party/granite_decode/LICENSE).
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
//   files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
//   modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
//   is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be
//   included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
// Changes: one pixel per call (the caller loops over the block and packs rows into a storage buffer) instead of texelFetch /
// imageStore, one copy of the 2-subset tables for BC6H and BC7, swizzles spelled out, BC6H returns binary16 bits (or E5B9G9R9).
// BC1-BC5 are not Granite's float s3tc/rgtc decoders (their rounding differs from the CPU bytes and they drop the 16-bit BC4/BC5
// interpolants): they are the integer arithmetic of xbg_texture.c color()/channel() and bcdec's float SNORM palette.
//
// The includer defines XBG_FMT (NVN format 0x41..0x50) and XBG_OPT (bit 0: BC4/BC5 unsigned to 8-bit channels, the CPU's
// xbg_bc_8bit_channels; bit 1: unsigned BC6H to E5B9G9R9, xbg_bc6_e5).
#ifndef XBG_BC_GPU_GLSL
#define XBG_BC_GPU_GLSL

#ifdef XBG_HOST
#define XBG_ARRAY(T, N, C) static const T N[C] = {
#define XBG_ARRAY_END };
#define XBG_DONT_UNROLL
#else
#extension GL_EXT_control_flow_attributes : require
#define XBG_ARRAY(T, N, C) const T N[C] = T[](
#define XBG_ARRAY_END );
#define XBG_DONT_UNROLL [[dont_unroll]]
#endif

// ---- Bit extraction (Granite bitextract.h, rewritten 06.11) ----
// Granite's version branched into a same-word and a cross-word path; the cross-word path, if a GPU compiler evaluates it for a field
// that does not cross a word (or that ends in the last word), reads payload[4] and shifts by 32 or by a negative count, all undefined
// in GLSL / SPIR-V. This one has no branch and no operation outside its defined range for any offset 0..127 and bits 1..32: the 64-bit
// window lo:hi is shifted right by s with (hi << 1) << (31 - s) (0 for s = 0, never a shift by 32), the mask is ~0 >> (32 - n).
// bits <= 0 gives 0 as before (no call site passes it). Same results as Granite's for every valid field (bc_gpu_decode_test).
uint xbg_bits(uvec4 payload, int offset, int bits)
{
    uint o = uint(offset) & 127u, w = o >> 5, s = o & 31u;
    uint lo = payload[w], hi = payload[min(w + 1u, 3u)];
    uint v = (lo >> s) | ((hi << 1u) << (31u - s));
    uint n = uint(clamp(bits, 1, 32));
    return v & (0xffffffffu >> (32u - n));
}

int extract_bits(uvec4 payload, int offset, int bits)
{
    return bits > 0 ? int(xbg_bits(payload, offset, bits)) : 0;
}

int extract_bits_sign(uvec4 payload, int offset, int bits)
{
    uint k = 32u - uint(clamp(bits, 1, 32));
    return bits > 0 ? int(xbg_bits(payload, offset, bits) << k) >> k : 0;
}

int extract_bits_reverse(uvec4 payload, int offset, int bits)
{
    uint k = 32u - uint(clamp(bits, 1, 32));
    return bits > 0 ? int(bitfieldReverse(xbg_bits(payload, offset, bits)) >> k) : 0;
}

// ---- Granite bc7.comp / bc6.comp tables (BC6H uses the first 32 entries of the 2-subset tables) ----
XBG_ARRAY(int, weight_table2, 4) 0, 21, 43, 64 XBG_ARRAY_END
XBG_ARRAY(int, weight_table3, 8) 0, 9, 18, 27, 37, 46, 55, 64 XBG_ARRAY_END
XBG_ARRAY(int, weight_table4, 16) 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 XBG_ARRAY_END

#define P3(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    (((a) << 0) | ((b) << 2) | ((c) << 4) | ((d) << 6) | \
    ((e) << 8) | ((f) << 10) | ((g) << 12) | ((h) << 14) | \
    ((i) << 16) | ((j) << 18) | ((k) << 20) | ((l) << 22) | \
    ((m) << 24) | ((n) << 26) | ((o) << 28) | ((p) << 30))

XBG_ARRAY(int, partition_table3, 64)
    P3(0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2),
    P3(0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1),
    P3(0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1),
    P3(0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1),
    P3(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2),
    P3(0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2),
    P3(0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1),
    P3(0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1),

    P3(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2),
    P3(0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2),
    P3(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2),
    P3(0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2),
    P3(0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2),
    P3(0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2),
    P3(0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2),
    P3(0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0),

    P3(0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2),
    P3(0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0),
    P3(0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2),
    P3(0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1),
    P3(0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2),
    P3(0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1),
    P3(0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2),
    P3(0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0),

    P3(0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0),
    P3(0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2),
    P3(0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0),
    P3(0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1),
    P3(0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2),
    P3(0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2),
    P3(0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1),
    P3(0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1),

    P3(0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2),
    P3(0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1),
    P3(0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2),
    P3(0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0),
    P3(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0),
    P3(0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0),
    P3(0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0),
    P3(0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1),

    P3(0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1),
    P3(0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2),
    P3(0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1),
    P3(0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2),
    P3(0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1),
    P3(0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1),
    P3(0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1),
    P3(0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1),

    P3(0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2),
    P3(0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1),
    P3(0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2),
    P3(0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2),
    P3(0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2),
    P3(0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2),
    P3(0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2),
    P3(0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2),

    P3(0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2),
    P3(0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2),
    P3(0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2),
    P3(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2),
    P3(0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1),
    P3(0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2),
    P3(0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    P3(0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0)
XBG_ARRAY_END

#define P2(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    (((a) << 0) | ((b) << 1) | ((c) << 2) | ((d) << 3) | \
    ((e) << 4) | ((f) << 5) | ((g) << 6) | ((h) << 7) | \
    ((i) << 8) | ((j) << 9) | ((k) << 10) | ((l) << 11) | \
    ((m) << 12) | ((n) << 13) | ((o) << 14) | ((p) << 15))

XBG_ARRAY(int, partition_table2, 64)
    P2(0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1),
    P2(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1),
    P2(0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1),
    P2(0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1),
    P2(0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1),
    P2(0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1),

    P2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1),
    P2(0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1),
    P2(0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    P2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1),

    P2(0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1),
    P2(0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0),
    P2(0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0),
    P2(0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0),
    P2(0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0),
    P2(0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0),
    P2(0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0),
    P2(0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1),

    P2(0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0),
    P2(0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0),
    P2(0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0),
    P2(0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0),
    P2(0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0),
    P2(0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0),
    P2(0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0),
    P2(0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0),

    P2(0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1),
    P2(0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1),
    P2(0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0),
    P2(0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0),
    P2(0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0),
    P2(0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0),
    P2(0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1),
    P2(0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1),

    P2(0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0),
    P2(0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0),
    P2(0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0),
    P2(0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0),
    P2(0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0),
    P2(0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1),
    P2(0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1),
    P2(0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0),

    P2(0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0),
    P2(0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0),
    P2(0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0),
    P2(0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0),
    P2(0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1),
    P2(0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1),
    P2(0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0),
    P2(0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0),

    P2(0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1),
    P2(0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1),
    P2(0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1),
    P2(0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1),
    P2(0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1),
    P2(0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0),
    P2(0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0),
    P2(0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1)
XBG_ARRAY_END

XBG_ARRAY(int, anchor_table2, 64)
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15,
    2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15,
    2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2,
    15, 15, 15, 15, 15, 2, 2, 15
XBG_ARRAY_END

XBG_ARRAY(ivec2, anchor_table3, 64)
    ivec2(3, 15), ivec2(3, 8), ivec2(15, 8), ivec2(15, 3), ivec2(8, 15), ivec2(3, 15), ivec2(15, 3), ivec2(15, 8),
    ivec2(8, 15), ivec2(8, 15), ivec2(6, 15), ivec2(6, 15), ivec2(6, 15), ivec2(5, 15), ivec2(3, 15), ivec2(3, 8),
    ivec2(3, 15), ivec2(3, 8), ivec2(8, 15), ivec2(15, 3), ivec2(3, 15), ivec2(3, 8), ivec2(6, 15), ivec2(10, 8),
    ivec2(5, 3), ivec2(8, 15), ivec2(8, 6), ivec2(6, 10), ivec2(8, 15), ivec2(5, 15), ivec2(15, 10), ivec2(15, 8),
    ivec2(8, 15), ivec2(15, 3), ivec2(3, 15), ivec2(5, 10), ivec2(6, 10), ivec2(10, 8), ivec2(8, 9), ivec2(15, 10),
    ivec2(15, 6), ivec2(3, 15), ivec2(15, 8), ivec2(5, 15), ivec2(15, 3), ivec2(15, 6), ivec2(15, 6), ivec2(15, 8),
    ivec2(3, 15), ivec2(15, 3), ivec2(5, 15), ivec2(5, 15), ivec2(5, 15), ivec2(8, 15), ivec2(5, 15), ivec2(10, 15),
    ivec2(5, 15), ivec2(10, 15), ivec2(8, 15), ivec2(13, 15), ivec2(15, 3), ivec2(12, 15), ivec2(3, 15), ivec2(3, 8)
XBG_ARRAY_END

// ---- Granite bc7.comp ----
struct Bc7Interp
{
    uvec4 ep0, ep1;
    uint color_weight;
    uint alpha_weight;
    uint rotation;
};

Bc7Interp decode_bc7_mode0(uvec4 payload, int linear_pixel)
{
    int part_index = extract_bits(payload, 1, 4);
    int part = (partition_table3[part_index] >> (2 * linear_pixel)) & 3;
    int bit_offset = part * 8;

    int r0 = extract_bits(payload, 5 + bit_offset, 4);
    int r1 = extract_bits(payload, 9 + bit_offset, 4);
    int g0 = extract_bits(payload, 29 + bit_offset, 4);
    int g1 = extract_bits(payload, 33 + bit_offset, 4);
    int b0 = extract_bits(payload, 53 + bit_offset, 4);
    int b1 = extract_bits(payload, 57 + bit_offset, 4);

    int sep0 = extract_bits(payload, 77 + part * 2, 1);
    int sep1 = extract_bits(payload, 78 + part * 2, 1);

    ivec2 anchor_pixels = anchor_table3[part_index];
    int index = extract_bits(
            payload,
            max(82 + linear_pixel * 3 - int(linear_pixel > anchor_pixels.x) - int(linear_pixel > anchor_pixels.y), 83),
            (linear_pixel == anchor_pixels.y || linear_pixel == anchor_pixels.x || linear_pixel == 0) ? 2 : 3);

    ivec3 rgb0 = ivec3(r0, g0, b0);
    ivec3 rgb1 = ivec3(r1, g1, b1);
    rgb0 = (rgb0 << 4) | (sep0 << 3) | (rgb0 >> 1);
    rgb1 = (rgb1 << 4) | (sep1 << 3) | (rgb1 >> 1);

    uint w = uint(weight_table3[index]);
    return Bc7Interp(uvec4(rgb0, 0xff), uvec4(rgb1, 0xff), w, w, 0u);
}

Bc7Interp decode_bc7_mode1(uvec4 payload, int linear_pixel)
{
    int part_index = extract_bits(payload, 2, 6);
    int part = (partition_table2[part_index] >> linear_pixel) & 1;
    int bit_offset = part * 12;

    int r0 = extract_bits(payload, 8 + bit_offset, 6);
    int r1 = extract_bits(payload, 14 + bit_offset, 6);
    int g0 = extract_bits(payload, 32 + bit_offset, 6);
    int g1 = extract_bits(payload, 38 + bit_offset, 6);
    int b0 = extract_bits(payload, 56 + bit_offset, 6);
    int b1 = extract_bits(payload, 62 + bit_offset, 6);
    int sep = extract_bits(payload, 80 + part, 1) << 1;

    int anchor_pixel = anchor_table2[part_index];

    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == anchor_pixel || linear_pixel == 0) ? 2 : 3);

    ivec3 rgb0 = ivec3(r0, g0, b0);
    ivec3 rgb1 = ivec3(r1, g1, b1);
    rgb0 = (rgb0 << 2) | sep | (rgb0 >> 5);
    rgb1 = (rgb1 << 2) | sep | (rgb1 >> 5);

    uint w = uint(weight_table3[index]);
    return Bc7Interp(uvec4(rgb0, 0xff), uvec4(rgb1, 0xff), w, w, 0u);
}

Bc7Interp decode_bc7_mode2(uvec4 payload, int linear_pixel)
{
    int part_index = extract_bits(payload, 3, 6);
    int part = (partition_table3[part_index] >> (2 * linear_pixel)) & 3;
    int bit_offset = part * 10;

    int r0 = extract_bits(payload, 9 + bit_offset, 5);
    int r1 = extract_bits(payload, 14 + bit_offset, 5);
    int g0 = extract_bits(payload, 39 + bit_offset, 5);
    int g1 = extract_bits(payload, 44 + bit_offset, 5);
    int b0 = extract_bits(payload, 69 + bit_offset, 5);
    int b1 = extract_bits(payload, 74 + bit_offset, 5);

    ivec2 anchor_pixels = anchor_table3[part_index];
    int index = extract_bits(
            payload,
            max(98 + linear_pixel * 2 - int(linear_pixel > anchor_pixels.x) - int(linear_pixel > anchor_pixels.y), 99),
            (linear_pixel == anchor_pixels.y || linear_pixel == anchor_pixels.x || linear_pixel == 0) ? 1 : 2);

    ivec3 rgb0 = ivec3(r0, g0, b0);
    ivec3 rgb1 = ivec3(r1, g1, b1);
    rgb0 = (rgb0 << 3) | (rgb0 >> 2);
    rgb1 = (rgb1 << 3) | (rgb1 >> 2);

    uint w = uint(weight_table2[index]);
    return Bc7Interp(uvec4(rgb0, 0xff), uvec4(rgb1, 0xff), w, w, 0u);
}

Bc7Interp decode_bc7_mode3(uvec4 payload, int linear_pixel)
{
    int part_index = extract_bits(payload, 4, 6);
    int part = (partition_table2[part_index] >> linear_pixel) & 1;
    int bit_offset = part * 14;

    int r0 = extract_bits(payload, 10 + bit_offset, 7);
    int r1 = extract_bits(payload, 17 + bit_offset, 7);
    int g0 = extract_bits(payload, 38 + bit_offset, 7);
    int g1 = extract_bits(payload, 45 + bit_offset, 7);
    int b0 = extract_bits(payload, 66 + bit_offset, 7);
    int b1 = extract_bits(payload, 73 + bit_offset, 7);

    int sep0 = extract_bits(payload, 94 + part * 2, 1);
    int sep1 = extract_bits(payload, 95 + part * 2, 1);

    int anchor_pixel = anchor_table2[part_index];

    int index = extract_bits(
        payload,
        max(97 + linear_pixel * 2 - int(linear_pixel > anchor_pixel), 98),
        (linear_pixel == anchor_pixel || linear_pixel == 0) ? 1 : 2);

    ivec3 rgb0 = ivec3(r0, g0, b0);
    ivec3 rgb1 = ivec3(r1, g1, b1);
    rgb0 = (rgb0 << 1) | sep0;
    rgb1 = (rgb1 << 1) | sep1;

    uint w = uint(weight_table2[index]);
    return Bc7Interp(uvec4(rgb0, 0xff), uvec4(rgb1, 0xff), w, w, 0u);
}

Bc7Interp decode_bc7_mode4(uvec4 payload, int linear_pixel)
{
    int rot = extract_bits(payload, 5, 2);
    bool isb = (payload.x & 0x80u) != 0u;
    int r0 = extract_bits(payload, 8, 5);
    int r1 = extract_bits(payload, 13, 5);
    int g0 = extract_bits(payload, 18, 5);
    int g1 = extract_bits(payload, 23, 5);
    int b0 = extract_bits(payload, 28, 5);
    int b1 = extract_bits(payload, 33, 5);
    int a0 = extract_bits(payload, 38, 6);
    int a1 = extract_bits(payload, 44, 6);

    int primary_index = extract_bits(
            payload,
            max(49 + linear_pixel * 2, 50),
            linear_pixel == 0 ? 1 : 2);
    int secondary_index = extract_bits(
            payload,
            max(80 + linear_pixel * 3, 81),
            linear_pixel == 0 ? 2 : 3);

    int color_weight = weight_table2[primary_index];
    int alpha_weight = weight_table3[secondary_index];

    if (isb)
    {
        int tmp = color_weight;
        color_weight = alpha_weight;
        alpha_weight = tmp;
    }

    ivec3 rgb0 = ivec3(r0, g0, b0);
    ivec3 rgb1 = ivec3(r1, g1, b1);
    rgb0 = (rgb0 << 3) | (rgb0 >> 2);
    rgb1 = (rgb1 << 3) | (rgb1 >> 2);
    a0 = (a0 << 2) | (a0 >> 4);
    a1 = (a1 << 2) | (a1 >> 4);
    return Bc7Interp(uvec4(rgb0, a0), uvec4(rgb1, a1), uint(color_weight), uint(alpha_weight), uint(rot));
}

Bc7Interp decode_bc7_mode5(uvec4 payload, int linear_pixel)
{
    int rot = extract_bits(payload, 6, 2);
    int r0 = extract_bits(payload, 8, 7);
    int r1 = extract_bits(payload, 15, 7);
    int g0 = extract_bits(payload, 22, 7);
    int g1 = extract_bits(payload, 29, 7);
    int b0 = extract_bits(payload, 36, 7);
    int b1 = extract_bits(payload, 43, 7);
    int a0 = extract_bits(payload, 50, 8);
    int a1 = extract_bits(payload, 58, 8);

    int primary_index = extract_bits(
            payload,
            max(65 + linear_pixel * 2, 66),
            linear_pixel == 0 ? 1 : 2);
    int secondary_index = extract_bits(
            payload,
            max(96 + linear_pixel * 2, 97),
            linear_pixel == 0 ? 1 : 2);

    int color_weight = weight_table2[primary_index];
    int alpha_weight = weight_table2[secondary_index];

    ivec3 rgb0 = ivec3(r0, g0, b0);
    ivec3 rgb1 = ivec3(r1, g1, b1);
    rgb0 = (rgb0 << 1) | (rgb0 >> 6);
    rgb1 = (rgb1 << 1) | (rgb1 >> 6);
    return Bc7Interp(uvec4(rgb0, a0), uvec4(rgb1, a1), uint(color_weight), uint(alpha_weight), uint(rot));
}

Bc7Interp decode_bc7_mode6(uvec4 payload, int linear_pixel)
{
    int sep0 = extract_bits(payload, 63, 1);
    int sep1 = extract_bits(payload, 64, 1);
    int r0 = extract_bits(payload, 7, 7);
    int r1 = extract_bits(payload, 14, 7);
    int g0 = extract_bits(payload, 21, 7);
    int g1 = extract_bits(payload, 28, 7);
    int b0 = extract_bits(payload, 35, 7);
    int b1 = extract_bits(payload, 42, 7);
    int a0 = extract_bits(payload, 49, 7);
    int a1 = extract_bits(payload, 56, 7);

    ivec4 ep0 = ivec4(r0, g0, b0, a0) * 2 + sep0;
    ivec4 ep1 = ivec4(r1, g1, b1, a1) * 2 + sep1;

    int index = extract_bits(
            payload,
            max(64 + linear_pixel * 4, 65),
            linear_pixel == 0 ? 3 : 4);

    uint w = uint(weight_table4[index]);
    return Bc7Interp(uvec4(ep0), uvec4(ep1), w, w, 0u);
}

Bc7Interp decode_bc7_mode7(uvec4 payload, int linear_pixel)
{
    int part_index = extract_bits(payload, 8, 6);
    int part = (partition_table2[part_index] >> linear_pixel) & 1;
    int bit_offset = part * 10;

    int r0 = extract_bits(payload, 14 + bit_offset, 5);
    int r1 = extract_bits(payload, 19 + bit_offset, 5);
    int g0 = extract_bits(payload, 34 + bit_offset, 5);
    int g1 = extract_bits(payload, 39 + bit_offset, 5);
    int b0 = extract_bits(payload, 54 + bit_offset, 5);
    int b1 = extract_bits(payload, 59 + bit_offset, 5);
    int a0 = extract_bits(payload, 74 + bit_offset, 5);
    int a1 = extract_bits(payload, 79 + bit_offset, 5);

    int sep0 = extract_bits(payload, 94 + part * 2, 1);
    int sep1 = extract_bits(payload, 95 + part * 2, 1);

    int anchor_pixel = anchor_table2[part_index];

    int index = extract_bits(
        payload,
        max(97 + linear_pixel * 2 - int(linear_pixel > anchor_pixel), 98),
        (linear_pixel == anchor_pixel || linear_pixel == 0) ? 1 : 2);

    ivec4 rgba0 = ivec4(r0, g0, b0, a0);
    ivec4 rgba1 = ivec4(r1, g1, b1, a1);
    rgba0 = (rgba0 << 3) | (rgba0 >> 3) | (sep0 << 2);
    rgba1 = (rgba1 << 3) | (rgba1 >> 3) | (sep1 << 2);

    uint w = uint(weight_table2[index]);
    return Bc7Interp(uvec4(rgba0), uvec4(rgba1), w, w, 0u);
}

// RGBA8 of one BC7 pixel, packed as the CPU writes it (R in the low byte).
uint bc7_pixel(uvec4 payload, int linear_pixel)
{
    Bc7Interp interp;
    int mode = findLSB(payload.x & 0xffu);
    switch (mode)
    {
    case 0: interp = decode_bc7_mode0(payload, linear_pixel); break;
    case 1: interp = decode_bc7_mode1(payload, linear_pixel); break;
    case 2: interp = decode_bc7_mode2(payload, linear_pixel); break;
    case 3: interp = decode_bc7_mode3(payload, linear_pixel); break;
    case 4: interp = decode_bc7_mode4(payload, linear_pixel); break;
    case 5: interp = decode_bc7_mode5(payload, linear_pixel); break;
    case 6: interp = decode_bc7_mode6(payload, linear_pixel); break;
    case 7: interp = decode_bc7_mode7(payload, linear_pixel); break;
    default: interp = Bc7Interp(uvec4(0u), uvec4(0u), 0u, 0u, 0u); break;   // mode 8 (no bit set in byte 0): transparent black
    }
    uint cw = interp.color_weight, aw = interp.alpha_weight;
    uint r = ((64u - cw) * interp.ep0.x + cw * interp.ep1.x + 32u) >> 6;
    uint g = ((64u - cw) * interp.ep0.y + cw * interp.ep1.y + 32u) >> 6;
    uint b = ((64u - cw) * interp.ep0.z + cw * interp.ep1.z + 32u) >> 6;
    uint a = ((64u - aw) * interp.ep0.w + aw * interp.ep1.w + 32u) >> 6;
    uvec4 rgba = uvec4(r, g, b, a);
    if (interp.rotation == 1u) rgba = uvec4(rgba.w, rgba.y, rgba.z, rgba.x);        // agbr
    else if (interp.rotation == 2u) rgba = uvec4(rgba.x, rgba.w, rgba.z, rgba.y);   // rabg
    else if (interp.rotation == 3u) rgba = uvec4(rgba.x, rgba.y, rgba.w, rgba.z);   // rgab
    return rgba.x | (rgba.y << 8) | (rgba.z << 16) | (rgba.w << 24);
}

// ---- Granite bc6.comp ----
#define BC6_SIGNED (XBG_FMT == 0x4fu)

struct Bc6Interp
{
    ivec3 ep0, ep1;
    int weight;
};

ivec3 bc6_unquantize_endpoint(ivec3 ep, int bits)
{
    ivec3 unq;
    if (BC6_SIGNED)
    {
        ep = bitfieldExtract(ep, 0, bits);
        if (bits < 16)
        {
            ivec3 sgn = 1 - ((ep >> 30) & 2);
            ivec3 abs_ep = abs(ep);
            unq = ((abs_ep << 15) + 0x4000) >> (bits - 1);
            unq = mix(unq, ivec3(0), equal(ep, ivec3(0)));
            unq = mix(unq, ivec3(0x7fff), greaterThanEqual(abs_ep, ivec3((1 << (bits - 1)) - 1)));
            unq = unq * sgn;
        }
        else
            unq = ep;
    }
    else
    {
        ep = ivec3(bitfieldExtract(uvec3(ep), 0, bits));
        if (bits < 15)
        {
            unq = ((ep << 15) + 0x4000) >> (bits - 1);
            unq = mix(unq, ivec3(0), equal(ep, ivec3(0)));
            unq = mix(unq, ivec3(0xffff), equal(ep, ivec3((1 << bits) - 1)));
        }
        else
            unq = ep;
    }
    return unq;
}

Bc6Interp decode_bc6_mode0(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 10);
    int g0 = extract_bits(payload, 15, 10);
    int b0 = extract_bits(payload, 25, 10);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 5);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits_sign(payload, 2, 1) << 4);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits_sign(payload, 3, 1) << 4);
        int r3 = extract_bits_sign(payload, 71, 5);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits_sign(payload, 40, 1) << 4);
        int b3 = extract_bits(payload, 50, 1) | (extract_bits(payload, 60, 1) << 1) | (extract_bits(payload, 70, 1) << 2) |
                (extract_bits(payload, 76, 1) << 3) | (extract_bits_sign(payload, 4, 1) << 4);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 5);
        int g1 = extract_bits_sign(payload, 45, 5);
        int b1 = extract_bits_sign(payload, 55, 5);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 10);
    ep1 = bc6_unquantize_endpoint(ep1, 10);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode1(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 7);
    int g0 = extract_bits(payload, 15, 7);
    int b0 = extract_bits(payload, 25, 7);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 6);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits(payload, 24, 1) << 4) | (extract_bits_sign(payload, 2, 1) << 5);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits(payload, 14, 1) << 4) | (extract_bits_sign(payload, 22, 1) << 5);
        int r3 = extract_bits_sign(payload, 71, 6);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits_sign(payload, 3, 2) << 4);
        int b3 = extract_bits(payload, 12, 2) | (extract_bits(payload, 23, 1) << 2) | (extract_bits(payload, 32, 1) << 3) |
                (extract_bits(payload, 34, 1) << 4) | (extract_bits_sign(payload, 33, 1) << 5);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 6);
        int g1 = extract_bits_sign(payload, 45, 6);
        int b1 = extract_bits_sign(payload, 55, 6);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 7);
    ep1 = bc6_unquantize_endpoint(ep1, 7);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode2(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 10) | (extract_bits(payload, 40, 1) << 10);
    int g0 = extract_bits(payload, 15, 10) | (extract_bits(payload, 49, 1) << 10);
    int b0 = extract_bits(payload, 25, 10) | (extract_bits(payload, 59, 1) << 10);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 5);
        int g2 = extract_bits_sign(payload, 41, 4);
        int b2 = extract_bits_sign(payload, 61, 4);
        int r3 = extract_bits_sign(payload, 71, 5);
        int g3 = extract_bits_sign(payload, 51, 4);
        int b3 = extract_bits(payload, 50, 1) | (extract_bits(payload, 60, 1) << 1) |
                (extract_bits(payload, 70, 1) << 2) | (extract_bits_sign(payload, 76, 1) << 3);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 5);
        int g1 = extract_bits_sign(payload, 45, 4);
        int b1 = extract_bits_sign(payload, 55, 4);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 11);
    ep1 = bc6_unquantize_endpoint(ep1, 11);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode3(uvec4 payload, int linear_pixel)
{
    int r0 = extract_bits(payload, 5, 10);
    int g0 = extract_bits(payload, 15, 10);
    int b0 = extract_bits(payload, 25, 10);
    int r1 = extract_bits(payload, 35, 10);
    int g1 = extract_bits(payload, 45, 10);
    int b1 = extract_bits(payload, 55, 10);
    ivec3 ep0 = ivec3(r0, g0, b0);
    ivec3 ep1 = ivec3(r1, g1, b1);
    ep0 = bc6_unquantize_endpoint(ep0, 10);
    ep1 = bc6_unquantize_endpoint(ep1, 10);
    int index = extract_bits(
        payload,
        max(64 + linear_pixel * 4, 65),
        linear_pixel == 0 ? 3 : 4);
    int w = weight_table4[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode6(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 10) | (extract_bits(payload, 39, 1) << 10);
    int g0 = extract_bits(payload, 15, 10) | (extract_bits(payload, 50, 1) << 10);
    int b0 = extract_bits(payload, 25, 10) | (extract_bits(payload, 59, 1) << 10);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 4);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits_sign(payload, 75, 1) << 4);
        int b2 = extract_bits_sign(payload, 61, 4);
        int r3 = extract_bits_sign(payload, 71, 4);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits_sign(payload, 40, 1) << 4);
        int b3 = extract_bits(payload, 69, 1) | (extract_bits(payload, 60, 1) << 1) |
                (extract_bits(payload, 70, 1) << 2) | (extract_bits_sign(payload, 76, 1) << 3);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 4);
        int g1 = extract_bits_sign(payload, 45, 5);
        int b1 = extract_bits_sign(payload, 55, 4);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 11);
    ep1 = bc6_unquantize_endpoint(ep1, 11);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode7(uvec4 payload, int linear_pixel)
{
    int r0 = extract_bits(payload, 5, 10) | (extract_bits(payload, 44, 1) << 10);
    int g0 = extract_bits(payload, 15, 10) | (extract_bits(payload, 54, 1) << 10);
    int b0 = extract_bits(payload, 25, 10) | (extract_bits(payload, 64, 1) << 10);
    int r1 = extract_bits_sign(payload, 35, 9);
    int g1 = extract_bits_sign(payload, 45, 9);
    int b1 = extract_bits_sign(payload, 55, 9);
    r1 += r0;
    g1 += g0;
    b1 += b0;
    ivec3 ep0 = ivec3(r0, g0, b0);
    ivec3 ep1 = ivec3(r1, g1, b1);
    ep0 = bc6_unquantize_endpoint(ep0, 11);
    ep1 = bc6_unquantize_endpoint(ep1, 11);
    int index = extract_bits(
        payload,
        max(64 + linear_pixel * 4, 65),
        linear_pixel == 0 ? 3 : 4);
    int w = weight_table4[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode10(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 10) | (extract_bits(payload, 39, 1) << 10);
    int g0 = extract_bits(payload, 15, 10) | (extract_bits(payload, 49, 1) << 10);
    int b0 = extract_bits(payload, 25, 10) | (extract_bits(payload, 60, 1) << 10);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 4);
        int g2 = extract_bits_sign(payload, 41, 4);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits_sign(payload, 40, 1) << 4);
        int r3 = extract_bits_sign(payload, 71, 4);
        int g3 = extract_bits_sign(payload, 51, 4);
        int b3 = extract_bits(payload, 50, 1) | (extract_bits(payload, 69, 2) << 1) |
                (extract_bits(payload, 76, 1) << 3) | (extract_bits_sign(payload, 75, 1) << 4);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 4);
        int g1 = extract_bits_sign(payload, 45, 4);
        int b1 = extract_bits_sign(payload, 55, 5);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 11);
    ep1 = bc6_unquantize_endpoint(ep1, 11);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode11(uvec4 payload, int linear_pixel)
{
    int r0 = extract_bits(payload, 5, 10) | (extract_bits_reverse(payload, 43, 2) << 10);
    int g0 = extract_bits(payload, 15, 10) | (extract_bits_reverse(payload, 53, 2) << 10);
    int b0 = extract_bits(payload, 25, 10) | (extract_bits_reverse(payload, 63, 2) << 10);
    int r1 = extract_bits_sign(payload, 35, 8);
    int g1 = extract_bits_sign(payload, 45, 8);
    int b1 = extract_bits_sign(payload, 55, 8);
    r1 += r0;
    g1 += g0;
    b1 += b0;
    ivec3 ep0 = ivec3(r0, g0, b0);
    ivec3 ep1 = ivec3(r1, g1, b1);
    ep0 = bc6_unquantize_endpoint(ep0, 12);
    ep1 = bc6_unquantize_endpoint(ep1, 12);
    int index = extract_bits(
        payload,
        max(64 + linear_pixel * 4, 65),
        linear_pixel == 0 ? 3 : 4);
    int w = weight_table4[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode14(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 9);
    int g0 = extract_bits(payload, 15, 9);
    int b0 = extract_bits(payload, 25, 9);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 5);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits_sign(payload, 24, 1) << 4);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits_sign(payload, 14, 1) << 4);
        int r3 = extract_bits_sign(payload, 71, 5);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits_sign(payload, 40, 1) << 4);
        int b3 = extract_bits(payload, 50, 1) | (extract_bits(payload, 60, 1) << 1) |
                (extract_bits(payload, 70, 1) << 2) |
                (extract_bits(payload, 76, 1) << 3) | (extract_bits_sign(payload, 34, 1) << 4);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 5);
        int g1 = extract_bits_sign(payload, 45, 5);
        int b1 = extract_bits_sign(payload, 55, 5);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 9);
    ep1 = bc6_unquantize_endpoint(ep1, 9);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode15(uvec4 payload, int linear_pixel)
{
    int r0 = extract_bits(payload, 5, 10) | (extract_bits_reverse(payload, 39, 6) << 10);
    int g0 = extract_bits(payload, 15, 10) | (extract_bits_reverse(payload, 49, 6) << 10);
    int b0 = extract_bits(payload, 25, 10) | (extract_bits_reverse(payload, 59, 6) << 10);
    int r1 = extract_bits_sign(payload, 35, 4);
    int g1 = extract_bits_sign(payload, 45, 4);
    int b1 = extract_bits_sign(payload, 55, 4);
    r1 += r0;
    g1 += g0;
    b1 += b0;
    ivec3 ep0 = ivec3(r0, g0, b0);
    ivec3 ep1 = ivec3(r1, g1, b1);
    ep0 = bc6_unquantize_endpoint(ep0, 16);
    ep1 = bc6_unquantize_endpoint(ep1, 16);
    int index = extract_bits(
        payload,
        max(64 + linear_pixel * 4, 65),
        linear_pixel == 0 ? 3 : 4);
    int w = weight_table4[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode18(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 8);
    int g0 = extract_bits(payload, 15, 8);
    int b0 = extract_bits(payload, 25, 8);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 6);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits_sign(payload, 24, 1) << 4);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits_sign(payload, 14, 1) << 4);
        int r3 = extract_bits_sign(payload, 71, 6);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits_sign(payload, 13, 1) << 4);
        int b3 = extract_bits(payload, 50, 1) | (extract_bits(payload, 60, 1) << 1) |
                (extract_bits(payload, 23, 1) << 2) | (extract_bits_sign(payload, 33, 2) << 3);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 6);
        int g1 = extract_bits_sign(payload, 45, 5);
        int b1 = extract_bits_sign(payload, 55, 5);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 8);
    ep1 = bc6_unquantize_endpoint(ep1, 8);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode22(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 8);
    int g0 = extract_bits(payload, 15, 8);
    int b0 = extract_bits(payload, 25, 8);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 5);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits(payload, 24, 1) << 4) | (extract_bits_sign(payload, 23, 1) << 5);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits_sign(payload, 14, 1) << 4);
        int r3 = extract_bits_sign(payload, 71, 5);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits(payload, 40, 1) << 4) | (extract_bits_sign(payload, 33, 1) << 5);
        int b3 = extract_bits(payload, 13, 1) | (extract_bits(payload, 60, 1) << 1) |
                (extract_bits(payload, 70, 1) << 2) | (extract_bits(payload, 76, 1) << 3) |
                (extract_bits_sign(payload, 34, 1) << 4);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 5);
        int g1 = extract_bits_sign(payload, 45, 6);
        int b1 = extract_bits_sign(payload, 55, 5);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 8);
    ep1 = bc6_unquantize_endpoint(ep1, 8);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode26(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    int r0 = extract_bits(payload, 5, 8);
    int g0 = extract_bits(payload, 15, 8);
    int b0 = extract_bits(payload, 25, 8);
    ep0 = ivec3(r0, g0, b0);
    if (part != 0)
    {
        int r2 = extract_bits_sign(payload, 65, 5);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits_sign(payload, 24, 1) << 4);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits(payload, 14, 1) << 4) | (extract_bits_sign(payload, 23, 1) << 5);
        int r3 = extract_bits_sign(payload, 71, 5);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits_sign(payload, 40, 1) << 4);
        int b3 = extract_bits(payload, 50, 1) | (extract_bits(payload, 13, 1) << 1) |
                (extract_bits(payload, 70, 1) << 2) | (extract_bits(payload, 76, 1) << 3) |
                (extract_bits(payload, 34, 1) << 4) | (extract_bits_sign(payload, 33, 1) << 5);
        ep1 = ivec3(r3, g3, b3) + ep0;
        ep0 += ivec3(r2, g2, b2);
    }
    else
    {
        int r1 = extract_bits_sign(payload, 35, 5);
        int g1 = extract_bits_sign(payload, 45, 5);
        int b1 = extract_bits_sign(payload, 55, 6);
        ep1 = ivec3(r1, g1, b1) + ep0;
    }
    ep0 = bc6_unquantize_endpoint(ep0, 8);
    ep1 = bc6_unquantize_endpoint(ep1, 8);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

Bc6Interp decode_bc6_mode30(uvec4 payload, int linear_pixel, int part, int anchor_pixel)
{
    ivec3 ep0, ep1;
    if (part != 0)
    {
        int r2 = extract_bits(payload, 65, 6);
        int g2 = extract_bits(payload, 41, 4) | (extract_bits(payload, 24, 1) << 4) | (extract_bits(payload, 21, 1) << 5);
        int b2 = extract_bits(payload, 61, 4) | (extract_bits(payload, 14, 1) << 4) | (extract_bits(payload, 22, 1) << 5);
        int r3 = extract_bits(payload, 71, 6);
        int g3 = extract_bits(payload, 51, 4) | (extract_bits(payload, 11, 1) << 4) | (extract_bits(payload, 31, 1) << 5);
        int b3 = extract_bits(payload, 12, 2) | (extract_bits(payload, 23, 1) << 2) |
            (extract_bits(payload, 32, 1) << 3) | (extract_bits(payload, 34, 1) << 4) | (extract_bits(payload, 33, 1) << 5);
        ep0 = ivec3(r2, g2, b2);
        ep1 = ivec3(r3, g3, b3);
    }
    else
    {
        int r0 = extract_bits(payload, 5, 6);
        int g0 = extract_bits(payload, 15, 6);
        int b0 = extract_bits(payload, 25, 6);
        int r1 = extract_bits(payload, 35, 6);
        int g1 = extract_bits(payload, 45, 6);
        int b1 = extract_bits(payload, 55, 6);
        ep0 = ivec3(r0, g0, b0);
        ep1 = ivec3(r1, g1, b1);
    }
    ep0 = bc6_unquantize_endpoint(ep0, 6);
    ep1 = bc6_unquantize_endpoint(ep1, 6);
    int index = extract_bits(
        payload,
        max(81 + linear_pixel * 3 - int(linear_pixel > anchor_pixel), 82),
        (linear_pixel == 0 || linear_pixel == anchor_pixel) ? 2 : 3);
    int w = weight_table3[index];
    return Bc6Interp(ep0, ep1, w);
}

// ---- E5B9G9R9 (same integer arithmetic as xbg_e5b9g9r9_from_half in xbg_texture.c) ----
uint xbg_e5_round(uint h, int e)
{
    uint he = (h >> 10) & 31u;
    uint m = he != 0u ? (h & 1023u) | 1024u : h & 1023u;
    int sh = e - (he != 0u ? int(he) - 1 : 0);
    // Both results with shift counts clamped to 0..31 (a compiler may evaluate both sides of a branch), then selected.
    uint up = m << uint(clamp(-sh, 0, 31));
    uint k = uint(clamp(sh, 1, 31));
    uint down = (m + (1u << (k - 1u))) >> k;
    return sh <= 0 ? up : (sh >= 32 ? 0u : down);
}

uint xbg_e5b9g9r9(uint r, uint g, uint b)
{
    r = min(r, 0x7bfcu);
    g = min(g, 0x7bfcu);
    b = min(b, 0x7bfcu);
    uint m = max(r, max(g, b));
    uint me = (m >> 10) & 31u;
    int e = me != 0u ? int(me) + 1 : max(findMSB(m & 1023u) - 8, 0);
    if (xbg_e5_round(m, e) == 512u) e += 1;
    return xbg_e5_round(r, e) | (xbg_e5_round(g, e) << 9) | (xbg_e5_round(b, e) << 18) | (uint(e) << 27);
}

// [nonadreno] 06.11: a BC6H block is decoded once (bc6_block: mode, partition, the unquantized endpoints of both subsets), then each
// pixel only takes its subset, index and weight (bc6_pixel_b). Before, the whole mode decode ran per pixel inside the shader's two
// rolled loops; on the Adreno 750 (Mi 14, DM_GFX_SIM_GPU=mali, DM_GFX_BC_VERIFY=1) every BC6H mode-1 block came out right in its first
// pixel row and zero in rows 1-3, while the same code compiled for the host is exact (also under UBSan shift / bounds checks): a code
// generation fault for that shape (BUGS_AND_FIXES §2.24). The endpoints come from Granite's mode functions unchanged (pixel 0, subset
// 0 and 1); the per-pixel index is the one every two-subset (3-bit, anchor-aware) or one-subset (4-bit) mode of Granite reads.
struct Bc6Block
{
    ivec3 a0, a1, b0, b1;   // subset 0 / subset 1 endpoints (unquantized)
    int part_index;
    int anchor;
    int kind;               // 0 reserved mode (zero), 1 one subset, 2 two subsets
};

Bc6Block bc6_block(uvec4 payload)
{
    int mode = extract_bits(payload, 0, 5);
    int part_index = extract_bits(payload, 77, 5);
    int anchor = anchor_table2[part_index];
    Bc6Interp s0 = Bc6Interp(ivec3(0), ivec3(0), 0);
    Bc6Interp s1 = s0;
    int kind = 2;
    if ((mode & 2) == 0)
    {
        if ((mode & 1) != 0)
        {
            s0 = decode_bc6_mode1(payload, 0, 0, anchor);
            s1 = decode_bc6_mode1(payload, 0, 1, anchor);
        }
        else
        {
            s0 = decode_bc6_mode0(payload, 0, 0, anchor);
            s1 = decode_bc6_mode0(payload, 0, 1, anchor);
        }
    }
    else
    {
        switch (mode)
        {
        case 2: s0 = decode_bc6_mode2(payload, 0, 0, anchor); s1 = decode_bc6_mode2(payload, 0, 1, anchor); break;
        case 6: s0 = decode_bc6_mode6(payload, 0, 0, anchor); s1 = decode_bc6_mode6(payload, 0, 1, anchor); break;
        case 10: s0 = decode_bc6_mode10(payload, 0, 0, anchor); s1 = decode_bc6_mode10(payload, 0, 1, anchor); break;
        case 14: s0 = decode_bc6_mode14(payload, 0, 0, anchor); s1 = decode_bc6_mode14(payload, 0, 1, anchor); break;
        case 18: s0 = decode_bc6_mode18(payload, 0, 0, anchor); s1 = decode_bc6_mode18(payload, 0, 1, anchor); break;
        case 22: s0 = decode_bc6_mode22(payload, 0, 0, anchor); s1 = decode_bc6_mode22(payload, 0, 1, anchor); break;
        case 26: s0 = decode_bc6_mode26(payload, 0, 0, anchor); s1 = decode_bc6_mode26(payload, 0, 1, anchor); break;
        case 30: s0 = decode_bc6_mode30(payload, 0, 0, anchor); s1 = decode_bc6_mode30(payload, 0, 1, anchor); break;
        case 3: s0 = decode_bc6_mode3(payload, 0); kind = 1; break;
        case 7: s0 = decode_bc6_mode7(payload, 0); kind = 1; break;
        case 11: s0 = decode_bc6_mode11(payload, 0); kind = 1; break;
        case 15: s0 = decode_bc6_mode15(payload, 0); kind = 1; break;
        default: kind = 0; break;   // reserved modes: zero RGB
        }
    }
    return Bc6Block(s0.ep0, s0.ep1, s1.ep0, s1.ep1, part_index, anchor, kind);
}

// BC6H pixel: RGBA16F (alpha 1.0) as two words, or E5B9G9R9 in .x (XBG_OPT bit 1, unsigned only).
uvec2 bc6_pixel_b(Bc6Block b, uvec4 payload, int linear_pixel)
{
    ivec3 e0 = b.a0, e1 = b.a1;
    int weight = 0;
    if (b.kind == 2)
    {
        if (((partition_table2[b.part_index] >> linear_pixel) & 1) != 0)
        {
            e0 = b.b0;
            e1 = b.b1;
        }
        int index = extract_bits(
            payload,
            max(81 + linear_pixel * 3 - int(linear_pixel > b.anchor), 82),
            (linear_pixel == 0 || linear_pixel == b.anchor) ? 2 : 3);
        weight = weight_table3[index];
    }
    else if (b.kind == 1)
    {
        int index = extract_bits(payload, max(64 + linear_pixel * 4, 65), linear_pixel == 0 ? 3 : 4);
        weight = weight_table4[index];
    }
    ivec3 rgb = ((64 - weight) * e0 + weight * e1 + 32) >> 6;
    // Squeeze range.
    if (BC6_SIGNED)
    {
        ivec3 neg_result = 0x8000 | (((-rgb) * 31) >> 5);
        ivec3 pos_result = (rgb * 31) >> 5;
        rgb = mix(pos_result, neg_result, lessThan(rgb, ivec3(0)));
        // Fixup for -0.0 (bcdec emits +0 there too).
        rgb = mix(rgb, ivec3(0), equal(rgb, ivec3(0x8000)));
    }
    else
    {
        rgb = (rgb * 31) >> 6;
    }
    if (!BC6_SIGNED && (XBG_OPT & 2u) != 0u)
        return uvec2(xbg_e5b9g9r9(uint(rgb.x), uint(rgb.y), uint(rgb.z)), 0u);
    return uvec2(uint(rgb.x) | (uint(rgb.y) << 16), uint(rgb.z) | (0x3c00u << 16));
}

uvec2 bc6_pixel(uvec4 payload, int linear_pixel)
{
    return bc6_pixel_b(bc6_block(payload), payload, linear_pixel);
}

// ---- BC1-BC5: integer arithmetic of xbg_texture.c color() / channel(), bcdec's float SNORM palette ----
uvec3 bc_565(uint c)
{
    uint r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return uvec3((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

uint bc_rgba(uvec3 v, uint a)
{
    return v.x | (v.y << 8) | (v.z << 16) | (a << 24);
}

// color: endpoints word, sel: selector word; four: BC2/BC3 (always 4 colours); opaque: BC1 RGB (selector 3 is opaque black).
uint bc_color(uint color, uint sel, int i, bool four, bool opaque)
{
    uint c0 = color & 0xffffu, c1 = color >> 16;
    uvec3 v0 = bc_565(c0), v1 = bc_565(c1);
    bool f = four || c0 > c1;
    uint s = (sel >> uint(2 * i)) & 3u;
    if (s == 0u) return bc_rgba(v0, 255u);
    if (s == 1u) return bc_rgba(v1, 255u);
    if (s == 2u) return bc_rgba(f ? (2u * v0 + v1) / 3u : (v0 + v1) / 2u, 255u);
    return bc_rgba(f ? (v0 + 2u * v1) / 3u : uvec3(0u), (f || opaque) ? 255u : 0u);
}

// BC3 alpha / BC4 / BC5 channel as 16 bits (keeps the 1/5 and 1/7 interpolants).
uint bc_channel16(uint lo, uint hi, int i)
{
    uint b0 = lo & 0xffu, b1 = (lo >> 8) & 0xffu, v0 = b0 * 257u, v1 = b1 * 257u;
    uint k = uint(extract_bits(uvec4(lo, hi, 0u, 0u), 16 + 3 * i, 3));
    if (k == 0u) return v0;
    if (k == 1u) return v1;
    if (b0 > b1) return ((8u - k) * v0 + (k - 1u) * v1 + 3u) / 7u;
    if (k < 6u) return ((6u - k) * v0 + (k - 1u) * v1 + 2u) / 5u;
    return k == 6u ? 0u : 65535u;
}

// Signed BC4/BC5 channel as SNORM16 bits (bcdec_bc4_float, then the CPU's round half away from zero).
uint bc_snorm16(uint lo, uint hi, int i)
{
    float a0 = max(float(bitfieldExtract(int(lo), 0, 8)) / 127.0f, -1.0f);
    float a1 = max(float(bitfieldExtract(int(lo), 8, 8)) / 127.0f, -1.0f);
    uint k = uint(extract_bits(uvec4(lo, hi, 0u, 0u), 16 + 3 * i, 3));
    float v;
    if (k == 0u) v = a0;
    else if (k == 1u) v = a1;
    else if (a0 > a1) v = (float(8u - k) * a0 + float(k - 1u) * a1) / 7.0f;
    else if (k < 6u) v = (float(6u - k) * a0 + float(k - 1u) * a1) / 5.0f;
    else v = k == 6u ? -1.0f : 1.0f;
    float s = v * 32767.0f;
    return uint(int(s + (s < 0.0f ? -0.5f : 0.5f))) & 0xffffu;
}

// One decoded pixel (i = 4 * y + x) of the block p (16 bytes; 8-byte formats use p.x, p.y), little-endian as the CPU writes it.
uvec2 xbg_bc_pixel_b(Bc6Block b6, uvec4 p, int i)
{
    uint f = (XBG_FMT >= 0x45u && XBG_FMT <= 0x48u) ? XBG_FMT - 4u : XBG_FMT;   // sRGB BC1-BC3: the same bytes
    if (f == 0x41u || f == 0x42u) return uvec2(bc_color(p.x, p.y, i, false, f == 0x41u), 0u);
    if (f == 0x43u)
        return uvec2((bc_color(p.z, p.w, i, true, false) & 0xffffffu) | ((((p[(4 * i) >> 5] >> uint((4 * i) & 31)) & 15u) * 17u) << 24), 0u);
    if (f == 0x44u) return uvec2((bc_color(p.z, p.w, i, true, false) & 0xffffffu) | (((bc_channel16(p.x, p.y, i) + 128u) / 257u) << 24), 0u);
    if (f == 0x49u)
    {
        uint r = bc_channel16(p.x, p.y, i);
        return uvec2((XBG_OPT & 1u) != 0u ? r >> 8 : r, 0u);
    }
    if (f == 0x4au) return uvec2(bc_snorm16(p.x, p.y, i), 0u);
    if (f == 0x4bu)
    {
        uint r = bc_channel16(p.x, p.y, i), g = bc_channel16(p.z, p.w, i);
        return uvec2((XBG_OPT & 1u) != 0u ? (r >> 8) | ((g >> 8) << 8) : r | (g << 16), 0u);
    }
    if (f == 0x4cu) return uvec2(bc_snorm16(p.x, p.y, i) | (bc_snorm16(p.z, p.w, i) << 16), 0u);
    if (f == 0x4du || f == 0x4eu) return uvec2(bc7_pixel(p, i), 0u);
    return bc6_pixel_b(b6, p, i);
}

// The per-block part of a pixel decode (BC6H only; for the other formats an unused zero block).
Bc6Block xbg_block(uvec4 p)
{
    if (XBG_FMT == 0x4fu || XBG_FMT == 0x50u)
        return bc6_block(p);
    return Bc6Block(ivec3(0), ivec3(0), ivec3(0), ivec3(0), 0, 0, 0);
}

uvec2 xbg_bc_pixel(uvec4 p, int i)
{
    return xbg_bc_pixel_b(xbg_block(p), p, i);
}

// One pixel row r (4 pixels = bpp words) of the block: words 0-3 in lo, 4-7 (bpp 8 only) in hi. The loop is kept rolled so the
// decoders are inlined once (an unrolled row inlined all formats 16 times: 686 KB of SPIR-V instead of ~1/10 of that).
struct XbgRow
{
    uvec4 lo;
    uvec4 hi;
};

// b6: xbg_block(p), computed once per block by the caller (xbg_bc_decode.comp main).
XbgRow xbg_bc_row_b(Bc6Block b6, uvec4 p, int r, uint bpp)
{
    XbgRow w = XbgRow(uvec4(0u), uvec4(0u));
    XBG_DONT_UNROLL for (int c = 0; c < 4; c++)
    {
        uvec2 v = xbg_bc_pixel_b(b6, p, r * 4 + c);
        if (bpp == 8u)
        {
            if (c < 2) { w.lo[2 * c] = v.x; w.lo[2 * c + 1] = v.y; }
            else { w.hi[2 * c - 4] = v.x; w.hi[2 * c - 3] = v.y; }
        }
        else if (bpp == 4u) w.lo[c] = v.x;
        else if (bpp == 2u) w.lo[c >> 1] |= (v.x & 0xffffu) << uint(16 * (c & 1));
        else w.lo[0] |= (v.x & 0xffu) << uint(8 * c);
    }
    return w;
}

XbgRow xbg_bc_row(uvec4 p, int r, uint bpp)
{
    return xbg_bc_row_b(xbg_block(p), p, r, bpp);
}

#endif
