#version 450
// [DOOM] DM_GFX_TU=sgsr2 (xbg_tu.h, BUGS_AND_FIXES 2.74): stands in for the game's TAA resolve (taa_vsps, ps 615e5352a9db7427)
// while the temporal upscaler is live. It is the game's own taadisabledtransfers_vsps (ps f1feb256691f7b81) written against
// the TAA program's interface: RT0 = the current colour as it is (jittered, not accumulated: the upscaler does the
// accumulation at the screen size), RT1 = the velocity target the game's motion blur reads (1 / blur length, linear depth),
// from the same computeVelocity. The game keeps r_antialiasing 6..9, so it keeps jittering the projection.
// Bindings are those of the TAA bundle's SPIR-V (the pipeline layout is built from the bundle's binding table, which this
// module shares): c3 = freqHigh_fragmentUniforms at 40, c4 = freqLow_fragmentUniforms at 52, NVN texture slots 0 / 1 / 2 at
// 10 / 22 / 16. The pre-transparency colour (28), the history (34), c1 (46) and the support block (4) are not read.
layout(set=0,binding=40) uniform C3{vec4 d[14];}c3;   // 0 texsize, 1..3 prevviewprojection w x y, 4 renderpositiontoviewtexture, 13 motionblurparms
layout(set=0,binding=52) uniform C4{vec4 d[7];}c4;    // 0 projectionmatrixz, 1 frustum tl, 2 tr, 3 resolutionscale, 4 bl, 5 br, 6 globalvieworigin
layout(set=0,binding=10) uniform sampler2D depthTex;
layout(set=0,binding=22) uniform sampler2D objVel;
layout(set=0,binding=16) uniform sampler2D current;
layout(location=0) out vec4 o0;
layout(location=1) out vec4 o1;
// The game gathers the four diagonal neighbours' depth (textureGatherOffsets at the pixel centre = these texels) and
// reprojects from the nearest one.
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
    vec3 M=texelFetch(current,ivec2(gl_FragCoord.xy),0).xyz;
    vec3 vel=computeVelocity(tc);
    o0=vec4(max(vec3(0.0),M),1.0);
    // OutputVelocityTarget(motionblurparms, v, z).zw, then x = 1 / x, as the game writes it
    vec4 mb=c3.d[13];
    vec2 v=vel.xy*mb.z;
    v*=clamp(mb.w*inversesqrt(dot(v,v)),0.0,1.0);
    v*=mb.xy;
    o1=vec4(1.0/length(v),vel.z,0.0,0.0);
}
