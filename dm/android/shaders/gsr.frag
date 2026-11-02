#version 450
// [DOOM] Spatial upscale of the game's final frame to the window size at present: DM_GFX_UPSCALE=gsr (vulkan_present.cpp,
// namespace upscale; docs/SWITCHES.md, docs/BUGS_AND_FIXES.md 2.19). Offline compiled into gsr.frag.spv.inc:
//   glslc --target-env=vulkan1.1 -O -mfmt=num gsr.frag -o gsr.frag.spv.inc
//
// Upscaler: Snapdragon Game Super Resolution 1 (SGSR1), "mobile" variant (RGBA mode: green as luma, no edge direction).
//   Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//   SPDX-License-Identifier: BSD-3-Clause
//   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
//   following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list of
//   conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, this
//   list of conditions and the following disclaimer in the documentation and/or other materials provided with the
//   distribution. 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or
//   promote products derived from this software without specific prior written permission.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
//   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
//   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// Re-written for this presenter from the published algorithm (no file was fetched): the same 12 taps (the 4x4 around the
// bilinear quad minus its corners, as four 2x2 gathers), weights, constants and clamps; the input position comes from
// gl_FragCoord and push constants (crop, letterbox, vertical mirror) instead of a vertex texcoord.
//
// With DM_GFX_SHARPEN (pc.rcas > 0) the same pass also sharpens: the 12 taps are exactly the RCAS crosses of the four quad
// texels, so each quad texel is RCAS-sharpened (AMD FidelityFX Super Resolution 1 RCAS, MIT, Copyright (c) 2021 Advanced Micro
// Devices, Inc.; the same math as rcas.comp) and the pixel's base colour is the bilinear mix of the sharpened quad -- equal to
// RCAS at the game's resolution followed by a bilinear upscale -- and the GSR edge delta, measured on the unsharpened green,
// is added on top. One pass, no intermediate image.
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform sampler2D src;   // the game's frame; linear filter, clamp to edge
layout(push_constant) uniform Push {
    vec2 scale;    // source pixels per output pixel (y < 0: the source is stored bottom-up, DM_GFX_PRESENT_FLIP_FOLD)
    vec2 offset;   // source position (pixels) of framebuffer coordinate 0: p = gl_FragCoord.xy * scale + offset
    vec2 texel;    // 1 / source image size
    float rcas;    // RCAS sharpness exp2(-stops) (1 = strongest); 0 = no RCAS
    float edge;    // GSR EdgeSharpness (2.0)
} pc;

const float EdgeThreshold = 8.0 / 255.0;
const float RCAS_LIMIT = 0.25 - 1.0 / 16.0;

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, float stdev) {
    float x = (dx * dx + dy * dy) * 0.55 + clamp(abs(c) * stdev, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
// Gathers of the green channel, base texel (0,0) = the quad's top-left, Vulkan gather order (x,y,z,w) = (i0,j1) (i1,j1) (i1,j0) (i0,j0):
//   L = block (-1,0): x (-1,1)  y (0,1)  z (0,0)  w (-1,0)      R = block (1,0): x (1,1)  y (2,1)  z (2,0)  w (1,0)
//   U = block (0,-1): x (0,0)   y (1,0)  z (1,-1) w (0,-1)      D = block (0,1): x (0,2)  y (1,2)  z (1,1)  w (0,1)
// Quad: (0,0) L.z, (1,0) R.w, (0,1) L.y, (1,1) R.x. g = the pixel's bilinear green, pl = its position in the quad.
float gsrDelta(vec4 L, vec4 R, vec4 U, vec4 D, float g, vec2 pl) {
    float mean = (L.y + L.z + R.x + R.w) * 0.25;
    L -= vec4(mean); R -= vec4(mean); U -= vec4(mean); D -= vec4(mean); g -= mean;
    float sum = dot(abs(L), vec4(1.0)) + dot(abs(R), vec4(1.0)) + abs(U.z) + abs(U.w) + abs(D.x) + abs(D.y);
    float sumMean = 10.14185 / max(sum, 1e-6);
    float stdev = sumMean * sumMean;
    vec2 aWY = weightY(pl.x, pl.y + 1.0, U.w, stdev);        // (0,-1)
    aWY += weightY(pl.x - 1.0, pl.y + 1.0, U.z, stdev);       // (1,-1)
    aWY += weightY(pl.x - 1.0, pl.y - 2.0, D.y, stdev);       // (1,2)
    aWY += weightY(pl.x, pl.y - 2.0, D.x, stdev);             // (0,2)
    aWY += weightY(pl.x + 1.0, pl.y - 1.0, L.x, stdev);       // (-1,1)
    aWY += weightY(pl.x, pl.y - 1.0, L.y, stdev);             // (0,1)
    aWY += weightY(pl.x, pl.y, L.z, stdev);                   // (0,0)
    aWY += weightY(pl.x + 1.0, pl.y, L.w, stdev);             // (-1,0)
    aWY += weightY(pl.x - 1.0, pl.y - 1.0, R.x, stdev);       // (1,1)
    aWY += weightY(pl.x - 2.0, pl.y - 1.0, R.y, stdev);       // (2,1)
    aWY += weightY(pl.x - 2.0, pl.y, R.z, stdev);             // (2,0)
    aWY += weightY(pl.x - 1.0, pl.y, R.w, stdev);             // (1,0)
    float finalY = aWY.x != 0.0 ? aWY.y / aWY.x : 0.0;
    float max4 = max(max(L.y, L.z), max(R.x, R.w));
    float min4 = min(min(L.y, L.z), min(R.x, R.w));
    finalY = clamp(pc.edge * finalY, min4, max4);
    return clamp(finalY - g, -23.0 / 255.0, 23.0 / 255.0);   // smooth high-contrast input
}
//    b
//  d e f      RCAS (rcas.comp): the largest negative lobe that keeps e inside its cross's range (no ringing)
//    h
vec3 rcasTexel(vec3 b, vec3 d, vec3 e, vec3 f, vec3 h) {
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-5));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.rcas;
    return clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0);
}
vec3 tap(vec4 r, vec4 g, vec4 b, int i) { return vec3(r[i], g[i], b[i]); }

void main() {
    vec2 p = gl_FragCoord.xy * pc.scale + pc.offset;   // source position in pixels
    vec2 q = p - 0.5;
    vec2 base = floor(q);
    vec2 pl = q - base;
    // a 2x2 block (a,b) relative to the base texel is gathered at its centre corner (base + (a+1, b+1)) in texel units
    vec2 cL = (base + vec2(0.0, 1.0)) * pc.texel;
    vec2 cR = (base + vec2(2.0, 1.0)) * pc.texel;
    vec2 cU = (base + vec2(1.0, 0.0)) * pc.texel;
    vec2 cD = (base + vec2(1.0, 2.0)) * pc.texel;
    vec4 Lg = textureGather(src, cL, 1);
    vec4 Rg = vec4(0.0), Ug = vec4(0.0), Dg = vec4(0.0);
    vec3 color;
    float g;
    bool all12 = false;
    if (pc.rcas > 0.0) {
        Rg = textureGather(src, cR, 1); Ug = textureGather(src, cU, 1); Dg = textureGather(src, cD, 1);
        vec4 Lr = textureGather(src, cL, 0), Lb = textureGather(src, cL, 2);
        vec4 Rr = textureGather(src, cR, 0), Rb = textureGather(src, cR, 2);
        vec4 Ur = textureGather(src, cU, 0), Ub = textureGather(src, cU, 2);
        vec4 Dr = textureGather(src, cD, 0), Db = textureGather(src, cD, 2);
        vec3 t00 = tap(Lr, Lg, Lb, 2), t10 = tap(Rr, Rg, Rb, 3), t01 = tap(Lr, Lg, Lb, 1), t11 = tap(Rr, Rg, Rb, 0);
        vec3 s00 = rcasTexel(tap(Ur, Ug, Ub, 3), tap(Lr, Lg, Lb, 3), t00, t10, t01);   // b (0,-1)  d (-1,0) f (1,0) h (0,1)
        vec3 s10 = rcasTexel(tap(Ur, Ug, Ub, 2), t00, t10, tap(Rr, Rg, Rb, 2), t11);   // b (1,-1)  d (0,0)  f (2,0) h (1,1)
        vec3 s01 = rcasTexel(t00, tap(Lr, Lg, Lb, 0), t01, t11, tap(Dr, Dg, Db, 0));   // b (0,0)   d (-1,1) f (1,1) h (0,2)
        vec3 s11 = rcasTexel(t10, t01, t11, tap(Rr, Rg, Rb, 1), tap(Dr, Dg, Db, 1));   // b (1,0)   d (0,1)  f (2,1) h (1,2)
        color = mix(mix(s00, s10, pl.x), mix(s01, s11, pl.x), pl.y);
        g = mix(mix(t00.g, t10.g, pl.x), mix(t01.g, t11.g, pl.x), pl.y);
        all12 = true;
    } else {
        color = textureLod(src, p * pc.texel, 0.0).rgb;
        g = color.g;
    }
    float edgeVote = abs(Lg.z - Lg.y) + abs(g - Lg.y) + abs(g - Lg.z);
    if (edgeVote > EdgeThreshold) {
        if (!all12) { Rg = textureGather(src, cR, 1); Ug = textureGather(src, cU, 1); Dg = textureGather(src, cD, 1); }
        color = clamp(color + gsrDelta(Lg, Rg, Ug, Dg, g, pl), 0.0, 1.0);
    }
    outColor = vec4(color, 1.0);   // alpha is not used
}
