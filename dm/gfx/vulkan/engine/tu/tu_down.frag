#version 450
// [DOOM] DM_GFX_TU=sgsr2, route A-replay (xbg_tu.h, BUGS_AND_FIXES 2.75): stands in for the game's TAA resolve (taa_vsps, ps
// 615e5352a9db7427) while the temporal upscaler works at the TAA point. The upscaler's pass has just written the accumulated
// scene at the output size (linear HDR); the engine binds that image in place of the game's TAA history (NVN texture slot 4)
// for this draw. RT0 = that image brought down to the render size (the box of output pixels under each render pixel), so
// everything the game computes from its TAA output at the render size (the bloom chain, the luminance chain of the
// auto-exposure, its own motion blur and postprocess, whose frame stays the fallback) sees an accumulated picture that does
// not move with the jitter, as with the game's own TAA. RT1 = the velocity target the game's motion blur reads, as the
// game's module writes it (tu_notaa.frag has the same code).
// Bindings are those of the TAA bundle's SPIR-V: c3 = freqHigh_fragmentUniforms at 40, c4 = freqLow_fragmentUniforms at 52,
// NVN texture slots 0 / 1 / 4 at 10 / 22 / 34. The current colour (16) and the pre-transparency colour (28) are not read.
layout(set=0,binding=40) uniform C3{vec4 d[14];}c3;   // 0 texsize, 1..3 prevviewprojection w x y, 4 renderpositiontoviewtexture, 13 motionblurparms
layout(set=0,binding=52) uniform C4{vec4 d[7];}c4;    // 0 projectionmatrixz, 1 frustum tl, 2 tr, 3 resolutionscale, 4 bl, 5 br, 6 globalvieworigin
layout(set=0,binding=10) uniform sampler2D depthTex;
layout(set=0,binding=22) uniform sampler2D objVel;
layout(set=0,binding=34) uniform sampler2D upscaled;   // the upscaler's output: the game's target size times the upscale ratio, linear filter
layout(location=0) out vec4 o0;
layout(location=1) out vec4 o1;
vec3 computeVelocity(vec2 winPos){
    ivec2 p=ivec2(gl_FragCoord.xy),hi=textureSize(depthTex,0)-1;
    vec3 m=vec3(-1.0,-1.0,texelFetch(depthTex,clamp(p+ivec2(-1,-1),ivec2(0),hi),0).r);
    float d=texelFetch(depthTex,clamp(p+ivec2(1,-1),ivec2(0),hi),0).r;if(d<m.z)m=vec3(1.0,-1.0,d);
    d=texelFetch(depthTex,clamp(p+ivec2(-1,1),ivec2(0),hi),0).r;if(d<m.z)m=vec3(-1.0,1.0,d);
    d=texelFetch(depthTex,clamp(p+ivec2(1,1),ivec2(0),hi),0).r;if(d<m.z)m=vec3(1.0,1.0,d);
    winPos+=m.xy*c3.d[0].zw;
    float lz=c4.d[0].w/(m.z+c4.d[0].z);
    lz*=lz<1.0?10.0:1.0;   // first-person arms rescale
    vec3 fx0=mix(c4.d[1].xyz,c4.d[2].xyz,winPos.x*c4.d[3].z);
    vec3 fx1=mix(c4.d[4].xyz,c4.d[5].xyz,winPos.x*c4.d[3].z);
    vec3 wp=c4.d[6].xyz+mix(fx1,fx0,winPos.y*c4.d[3].w)*lz;
    float rcp=1.0/(wp.x*c3.d[1].x+(wp.y*c3.d[1].y+(wp.z*c3.d[1].z+c3.d[1].w)));
    vec2 prev=vec2(wp.x*c3.d[2].x+(wp.y*c3.d[2].y+(wp.z*c3.d[2].z+c3.d[2].w)),wp.x*c3.d[3].x+(wp.y*c3.d[3].y+(wp.z*c3.d[3].z+c3.d[3].w)))*rcp;
    vec2 objV=textureLod(objVel,winPos,0.0).xy;
    return vec3(objV.x<1.0?objV:winPos-prev*c4.d[3].xy,lz);
}
void main(){
    vec2 tc=gl_FragCoord.xy*c3.d[4].zw+c3.d[4].xy;
    // The image covers the same uv as the game's target, so tc is the centre of this render pixel's box in it. Four bilinear
    // taps a quarter of a render pixel off the centre average the box (exactly at a ratio of 2, closely at 1.5).
    vec2 q=0.25*c3.d[0].zw,lim=c4.d[3].xy-0.5/vec2(textureSize(upscaled,0));
    vec3 c=textureLod(upscaled,min(tc+vec2(-q.x,-q.y),lim),0.0).xyz+textureLod(upscaled,min(tc+vec2(q.x,-q.y),lim),0.0).xyz
          +textureLod(upscaled,min(tc+vec2(-q.x,q.y),lim),0.0).xyz+textureLod(upscaled,min(tc+q,lim),0.0).xyz;
    vec3 vel=computeVelocity(tc);
    o0=vec4(max(vec3(0.0),0.25*c),1.0);
    // OutputVelocityTarget(motionblurparms, v, z).zw, then x = 1 / x, as the game writes it
    vec4 mb=c3.d[13];
    vec2 v=vel.xy*mb.z;
    v*=clamp(mb.w*inversesqrt(dot(v,v)),0.0,1.0);
    v*=mb.xy;
    o1=vec4(1.0/length(v),vel.z,0.0,0.0);
}
