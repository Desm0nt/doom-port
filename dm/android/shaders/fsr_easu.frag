#version 450
// [DOOM] Spatial upscale of the game's final frame to the window size at present: DM_GFX_UPSCALE=fsr (vulkan_present.cpp,
// namespace upscale; docs/SWITCHES.md, docs/BUGS_AND_FIXES.md 2.73). Offline compiled into fsr_easu.frag.spv.inc:
//   glslc --target-env=vulkan1.1 -O -mfmt=num fsr_easu.frag -o fsr_easu.frag.spv.inc
//
// Upscaler: EASU (Edge Adaptive Spatial Upsampling), the scaling pass of AMD FidelityFX Super Resolution 1.0 (FP32 path of
// ffx_fsr1.h), MIT License, Copyright (c) 2021 Advanced Micro Devices, Inc. (docs/THIRD_PARTY.md):
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
//   files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
//   modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
//   is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be
//   included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
// Re-written for this presenter: the same 12 taps, direction / length analysis, lanczos-2 approximation and dering clamp; the
// taps are texelFetch (clamped) instead of gathers, and the input position comes from gl_FragCoord and push constants (crop,
// letterbox, vertical mirror) instead of FsrEasuCon. Sharpening (RCAS) is a second pass at the output size (fsr_rcas.frag).
layout(binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 outColor;
// source position p = gl_FragCoord.xy * scale + offset, in pixels of the stored image (pixel centres at +0.5)
layout(push_constant) uniform Push { vec2 scale; vec2 offset; vec2 texel; float rcas; float pad; } pc;

ivec2 lim;
vec3 at(ivec2 q) { return texelFetch(src, clamp(q, ivec2(0), lim), 0).rgb; }
float luma(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
float rcpSafe(float x) { return 1.0 / max(x, 1.0 / 65536.0); }

void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    // A
    // B C D
    //   E
    float dc = lD - lC, cb = lC - lB;
    float lenX = rcpSafe(max(abs(dc), abs(cb)));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = rcpSafe(max(abs(ec), abs(ca)));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main() {
    lim = textureSize(src, 0) - 1;
    vec2 pp = gl_FragCoord.xy * pc.scale + pc.offset - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 o = ivec2(fp);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = at(o + ivec2(0, -1)), c = at(o + ivec2(1, -1));
    vec3 e = at(o + ivec2(-1, 0)), f = at(o), g = at(o + ivec2(1, 0)), h = at(o + ivec2(2, 0));
    vec3 i = at(o + ivec2(-1, 1)), j = at(o + ivec2(0, 1)), k = at(o + ivec2(1, 1)), l = at(o + ivec2(2, 1));
    vec3 n = at(o + ivec2(0, 2)), oo = at(o + ivec2(1, 2));
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(oo);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcpSafe(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oo);
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    outColor = vec4(min(max4, max(min4, aC / aW)), 1.0);
}
