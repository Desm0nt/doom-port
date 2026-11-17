#version 450
//============================================================================================================
// Snapdragon(TM) Game Super Resolution 2 (SGSR2), pass 1 of the 2-pass fragment variant ("convert"): motion vector and
// depth-clip factor per render pixel. Re-implemented from the published algorithm (sgsr/v2/include/glsl_2_pass_fs/
// sgsr2_convert.fs) for DOOM 2016's buffers.
//
//                  Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list of
// conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation and/or other materials provided with the
// distribution. 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or
// promote products derived from this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. (Full text: sgsr2_license.txt.)
//============================================================================================================
// [DOOM] DM_GFX_TU=sgsr2 (xbg_tu.h, BUGS_AND_FIXES 2.74). What differs from the published pass:
//  - the motion vector is the game's own (taa_vsps computeVelocity): the RG16F object velocity where the depth pre-pass
//    wrote one (x < 1), else the camera reprojection of the pixel from its depth. The reprojection is the game's
//    world = origin + frustum ray * linear depth, prev = prevviewprojection * world, folded on the CPU into a bilinear
//    form of the view position (ax / ay / aw, k), so no matrix has to be passed and the origin's large coordinates cancel
//    in double precision there;
//  - the depth test works on the game's linear depth: the published Depthsep / |depth difference| on device depth equals
//    K * z_neighbour / |z_neighbour - z_centre| for a standard projection, which does not depend on the depth convention;
//  - the 4x4 neighbourhood is read with the four gathers of the published pass; the inner 2x2 and the 3x3 around the
//    pixel are picked by texel position.
layout(push_constant) uniform PC{
    vec4 ax,ay,aw;   // previous clip x / y / w of the frustum ray per unit of linear depth: c0 + c1*s + c2*t + c3*s*t
    vec4 k;          // xyz: previous clip x / y / w of the view origin; w: depth separation constant (Ksep * Kfov * diagonal)
    vec4 texsize;    // the game's target: w, h, 1/w, 1/h
    vec4 r2t;        // target uv = gl_FragCoord * zw + xy (renderpositiontoviewtexture)
    vec4 rs;         // resolutionscale: xy = the part of the target the view covers, zw = 1 / xy
    vec4 pz;         // x, y: projectionmatrixz.z / .w (linear z = y / (depth + x)); z: +1 depth grows with distance, -1 reversed;
                     // w: 1 = the camera did not move since the last frame (its part of the motion is exactly 0)
}pc;
layout(set=0,binding=0) uniform sampler2D depthTex;
layout(set=0,binding=1) uniform sampler2D objVel;
layout(location=0) out vec4 o;   // xy motion in NDC units (2 = the whole view), z depth-clip factor
float nearest4(vec4 v){return min(min(v.x,v.y),min(v.z,v.w));}
float linearZ(float e){return pc.pz.y/(e*pc.pz.z+pc.pz.x);}   // e = depth * direction (pz.z is +-1, so e * pz.z is the depth again)
void main(){
    vec2 tc=gl_FragCoord.xy*pc.r2t.zw+pc.r2t.xy,px=pc.texsize.zw;
    vec2 lim=pc.rs.xy-px;                      // keep the gathers inside the rendered part of the target
    vec2 g=tc-0.5*px;
    // e = depth * direction, so min() is the nearest surface with either depth convention
    vec4 bl=textureGather(depthTex,min(g,lim),0)*pc.pz.z;                           // texels (x-1..x, y-1..y)
    vec4 br=textureGather(depthTex,min(g+vec2(2.0*px.x,0.0),lim),0)*pc.pz.z;        // (x+1..x+2, y-1..y)
    vec4 tl=textureGather(depthTex,min(g+vec2(0.0,2.0*px.y),lim),0)*pc.pz.z;        // (x-1..x, y+1..y+2)
    vec4 tr=textureGather(depthTex,min(g+2.0*px,lim),0)*pc.pz.z;                    // (x+1..x+2, y+1..y+2)
    // gather order: x = (i0,j1), y = (i1,j1), z = (i1,j0), w = (i0,j0)
    float eC=min(min(bl.y,br.x),min(tl.z,tr.w));           // nearest of the inner 2x2: (x,y) (x+1,y) (x,y+1) (x+1,y+1)
    float bl4=nearest4(bl);
    float e9=min(min(bl4,eC),min(br.w,tl.w));              // nearest of the 3x3 around the pixel: + (x+1,y-1), (x-1,y+1)
    float zC=linearZ(eC),clipf=0.0;
    if(zC>0.0 && zC<abs(pc.pz.y)*1.0e5){                   // not the far plane / sky (the published depth < 1 - 1e-5)
        const float EPSILON=1.19e-07;
        vec4 zq=vec4(linearZ(bl4),linearZ(nearest4(br)),linearZ(nearest4(tl)),linearZ(nearest4(tr)));
        vec4 w=clamp(pc.k.w*zq/(abs(zq-vec4(zC))+EPSILON*zq),0.0,1.0);
        clipf=clamp(1.0-0.25*(w.x+w.y+w.z+w.w),0.0,1.0);
    }
    vec2 st=tc*pc.rs.zw;                                   // 0..1 across the view
    vec2 ov=textureLod(objVel,tc,0.0).xy;                  // uv_now - uv_prev in target uv; x >= 1: no object velocity here
    vec2 mv;
    if(ov.x<1.0)mv=ov*pc.rs.zw;
    else if(pc.pz.w>0.5)mv=vec2(0.0);
    else{
        float z=linearZ(e9);z*=z<1.0?10.0:1.0;             // the game's first-person arms rescale
        z=min(abs(z),1.0e7);                               // an infinite far plane stays finite
        vec4 bil=vec4(1.0,st.x,st.y,st.x*st.y);
        vec3 prev=pc.k.xyz+z*vec3(dot(pc.ax,bil),dot(pc.ay,bil),dot(pc.aw,bil));
        mv=st-prev.xy/prev.z;
    }
    o=vec4(mv*2.0,clipf,0.0);
}
