#version 450
// [DOOM] RCAS (Robust Contrast-Adaptive Sharpening), the sharpening pass of AMD FidelityFX Super Resolution 1.0, MIT License,
// Copyright (c) 2021 Advanced Micro Devices, Inc. (docs/THIRD_PARTY.md; licence text in fsr_easu.frag). Second pass of
// DM_GFX_UPSCALE=fsr with DM_GFX_SHARPEN: sharpens the EASU output at the window size (vulkan_present.cpp namespace upscale),
// as FSR 1 is meant to run. Same math as rcas.comp. Offline compiled into fsr_rcas.frag.spv.inc:
//   glslc --target-env=vulkan1.1 -O -mfmt=num fsr_rcas.frag -o fsr_rcas.frag.spv.inc
layout(binding = 0) uniform sampler2D src;   // the EASU result, the size of the target
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform Push { vec2 scale; vec2 offset; vec2 texel; float rcas; float pad; } pc;   // rcas = exp2(-stops)
const float RCAS_LIMIT = 0.25 - 1.0 / 16.0;
ivec2 lim;
vec3 at(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), lim), 0).rgb; }
void main() {
    lim = textureSize(src, 0) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    //    b
    //  d e f
    //    h
    vec3 b = at(p + ivec2(0, -1)), d = at(p + ivec2(-1, 0)), e = at(p), f = at(p + ivec2(1, 0)), h = at(p + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-5));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.rcas;
    vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
