#version 450
//============================================================================================================
// Snapdragon(TM) Game Super Resolution 2 (SGSR2), pass 2 of the 2-pass fragment variant ("upscale"): the temporal
// resolve at the output size. Re-implemented from the published algorithm (sgsr/v2/include/glsl_2_pass_fs/
// sgsr2_upscale.fs) for DOOM 2016's buffers; the kernel, the neighbourhood box and the blend weights keep its constants.
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
//  - the colour input is the game's post-processed LDR scene (sRGB-encoded, before the UI): the lower-left part
//    renderSize of a larger target, so texel reads are clamped to it and uv is scaled by the target's size;
//  - two outputs: location 0 the new history (scene only), location 1 the frame that is presented = the same colour with
//    the game's UI texture blended over it the way viewcolorupsample_vsps does (linear blend of premultiplied UI, gamma);
//  - the pass works in the game's row order (the NVN lower-left window: row 0 is the bottom of the picture) and can write
//    its outputs upside down (par.z), so the presented frame is upright and needs no flip blit; only the history read
//    and the output position are mirrored;
//  - debug views (par.w, DM_GFX_TU_DEBUG).
layout(push_constant) uniform PC{
    vec4 rsz;   // render size: w, h, 1/w, 1/h (the 3D view inside the game's targets)
    vec4 osz;   // output size: w, h, 1/w, 1/h
    vec4 jit;   // xy: jitter offset in render pixels, game rows (the projection moved the picture by +jit); z: biasmax; w: scalefactor
    vec4 par;   // x: minLerpContribution; y: reset; z: 1 = output rows are the game's rows mirrored; w: debug view
    vec4 tsz;   // xy: 1 / size of the colour target; zw: 1 / size of the motion image
    vec4 ui;    // x: gamma (1 = none); y: UI colour scale; z: 1 = blend the UI; w: 1 = location 1 is an sRGB attachment
}pc;
layout(set=0,binding=0) uniform sampler2D PrevOutput;                   // history, output size, linear filter
layout(set=0,binding=1) uniform sampler2D MotionDepthClipAlphaBuffer;   // pass 1, linear filter
layout(set=0,binding=2) uniform sampler2D InputColor;                   // texel fetches
layout(set=0,binding=3) uniform sampler2D UiColor;                      // the game's UI layer, premultiplied, linear filter
layout(location=0) out vec4 History;
layout(location=1) out vec4 Final;
float FastLanczos(float base){float y=base-1.0,y2=y*y,y_temp=0.75*y+y2;return y_temp*y2;}
vec3 SRGBlinear(vec3 c){return mix(pow(c/1.055+0.0521327,vec3(2.4)),c/12.92,lessThanEqual(c,vec3(0.04045)));}
vec3 linearSRGB(vec3 c){return mix(c*12.92,pow(c,vec3(1.0/2.4))*1.055-0.055,greaterThan(c,vec3(0.0031308)));}
void main(){
    const bool mirror=pc.par.z>0.5;
    vec2 ouv=gl_FragCoord.xy*pc.osz.zw;
    vec2 Hruv=mirror?vec2(ouv.x,1.0-ouv.y):ouv;                 // this output pixel in the game's row order
    vec2 Jitteruv=clamp(Hruv+pc.jit.xy*pc.rsz.zw,0.0,1.0);
    ivec2 hi=ivec2(pc.rsz.xy)-1;
    ivec2 InputPos=min(ivec2(Jitteruv*pc.rsz.xy),hi);

    vec3 mda=textureLod(MotionDepthClipAlphaBuffer,min(Jitteruv*pc.rsz.xy,pc.rsz.xy-0.5)*pc.tsz.zw,0.0).xyz;
    vec2 Motion=mda.xy;
    float depthfactor=mda.z;

    vec2 PrevUV=clamp(Hruv-0.5*Motion,0.0,1.0);
    vec3 HistoryColor=textureLod(PrevOutput,mirror?vec2(PrevUV.x,1.0-PrevUV.y):PrevUV,0.0).xyz;

    float biasmax=pc.jit.z,scalefactor=pc.jit.w;
    float biasmin=max(1.0,0.3+0.3*biasmax);
    float kernelbias=mix(biasmax,biasmin,0.25*depthfactor);
    float motion_viewport_len=length(Motion*pc.osz.xy);
    float curvebias=mix(-2.0,-3.0,clamp(motion_viewport_len*0.02,0.0,1.0));
    kernelbias*=0.5;
    float kernelbias2=kernelbias*kernelbias;
    // the centre of the fetched render pixel in the un-jittered picture, relative to this output pixel (render pixels)
    vec2 srcpos_srcOutputPos=vec2(InputPos)+vec2(0.5)-pc.jit.xy-Hruv*pc.rsz.xy;

    // five taps: top, right, left, centre, bottom (the published pass keeps its four corner taps switched off)
    const ivec2 tap[5]=ivec2[5](ivec2(0,1),ivec2(1,0),ivec2(-1,0),ivec2(0,0),ivec2(0,-1));
    vec4 Upsampledcw=vec4(0.0);
    vec3 rectboxmin=vec3(0.0),rectboxmax=vec3(0.0),rectboxcenter=vec3(0.0),rectboxvar=vec3(0.0);
    float rectboxweight=0.0;
    for(int i=0;i<5;i++){
        vec3 samplecolor=texelFetch(InputColor,clamp(InputPos+tap[i],ivec2(0),hi),0).xyz;
        vec2 baseoffset=srcpos_srcOutputPos+vec2(tap[i]);
        float baseoffset_dot=dot(baseoffset,baseoffset);
        float weight=FastLanczos(clamp(baseoffset_dot*kernelbias2,0.0,1.0));
        Upsampledcw+=vec4(samplecolor*weight,weight);
        float boxweight=exp(baseoffset_dot*curvebias);
        if(i==0){rectboxmin=samplecolor;rectboxmax=samplecolor;}
        else{rectboxmin=min(rectboxmin,samplecolor);rectboxmax=max(rectboxmax,samplecolor);}
        vec3 wsample=samplecolor*boxweight;
        rectboxcenter+=wsample;
        rectboxvar+=samplecolor*wsample;
        rectboxweight+=boxweight;
    }
    rectboxweight=1.0/rectboxweight;
    rectboxcenter*=rectboxweight;
    rectboxvar*=rectboxweight;
    rectboxvar=sqrt(abs(rectboxvar-rectboxcenter*rectboxcenter));

    const float EPSILON=1.192e-07;
    Upsampledcw.xyz=clamp(Upsampledcw.xyz/max(Upsampledcw.w,EPSILON),rectboxmin-vec3(0.075),rectboxmax+vec3(0.075));
    Upsampledcw.w=Upsampledcw.w*(1.0/3.0);

    float baseupdate=1.0-depthfactor;
    baseupdate=min(baseupdate,mix(baseupdate,Upsampledcw.w*10.0,clamp(10.0*motion_viewport_len,0.0,1.0)));
    baseupdate=min(baseupdate,mix(baseupdate,Upsampledcw.w,clamp(motion_viewport_len*0.05,0.0,1.0)));
    float basealpha=baseupdate;

    float boxscale=max(depthfactor,clamp(motion_viewport_len*0.05,0.0,1.0));
    float boxsize=mix(scalefactor,1.0,boxscale);
    vec3 sboxvar=rectboxvar*boxsize;
    rectboxmax=min(rectboxmax,rectboxcenter+sboxvar);
    rectboxmin=max(rectboxmin,rectboxcenter-sboxvar);

    vec3 clampedcolor=clamp(HistoryColor,rectboxmin,rectboxmax);
    float startLerpValue=pc.par.x;
    if(abs(mda.x)+abs(mda.y)>0.000001)startLerpValue=0.0;
    float lerpcontribution=(any(greaterThan(rectboxmin,HistoryColor)) || any(greaterThan(HistoryColor,rectboxmax)))?startLerpValue:1.0;
    HistoryColor=mix(clampedcolor,HistoryColor,clamp(lerpcontribution,0.0,1.0));
    float basemin=min(basealpha,0.1);
    basealpha=mix(basemin,basealpha,clamp(lerpcontribution,0.0,1.0));

    float alphasum=max(EPSILON,basealpha+Upsampledcw.w);
    float alpha=clamp(Upsampledcw.w/alphasum+pc.par.y,0.0,1.0);
    vec3 color=mix(HistoryColor,Upsampledcw.xyz,alpha);
    History=vec4(color,1.0);

    // [DOOM] the presented frame
    vec3 shown=color;
    int dbg=int(pc.par.w+0.5);
    if(dbg==1)shown=vec3(abs(Motion)*pc.osz.xy*0.02,0.25);        // motion: 25 output pixels a frame = full red / green
    else if(dbg==2)shown=vec3(depthfactor);                         // depth-clip factor (edges / disocclusion)
    else if(dbg==3)shown=Upsampledcw.xyz;                           // this frame's samples only (no history)
    else if(dbg==4)shown=texelFetch(InputColor,InputPos,0).xyz;     // the nearest input pixel (the jittered input as it is)
    if(pc.ui.z>0.5){
        vec4 gui=textureLod(UiColor,Hruv,0.0);
        if(gui.a>0.0 || gui.r+gui.g+gui.b>0.0)                      // premultiplied: nothing of the UI on this pixel otherwise
            shown=linearSRGB(SRGBlinear(shown)*(1.0-gui.a)+SRGBlinear(gui.xyz)*pc.ui.y);
    }
    if(pc.ui.x!=1.0)shown=pow(max(shown,vec3(0.0)),vec3(pc.ui.x));
    if(pc.ui.w>0.5)shown=SRGBlinear(clamp(shown,0.0,1.0));
    Final=vec4(shown,1.0);
}
