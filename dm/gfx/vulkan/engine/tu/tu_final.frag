#version 450
// [DOOM] DM_GFX_TU=sgsr2, route A-replay (xbg_tu.h, BUGS_AND_FIXES 2.75): the frame that is presented. The scene is the
// game's own postprocess draw replayed at the output size (sRGB-encoded LDR, the game's rows: row 0 is the bottom of the
// picture, in the lower-left part of a larger target); it is read 1:1 and the game's UI texture is blended over it the way
// viewcolorupsample_vsps does (linear blend of the premultiplied UI, gamma). The output is upright, so the presenter needs no
// flip. Same UI code as the route B1 pass (sgsr2_upscale.frag).
layout(push_constant) uniform PC{
    vec4 osz;   // output size: w, h, 1/w, 1/h
    vec4 ui;    // x: gamma (1 = none); y: UI colour scale; z: 1 = blend the UI; w: 1 = the attachment is sRGB
}pc;
layout(set=0,binding=0) uniform sampler2D Scene;     // texel fetches
layout(set=0,binding=1) uniform sampler2D UiColor;   // the game's UI layer, premultiplied, linear filter
layout(location=0) out vec4 Final;
vec3 SRGBlinear(vec3 c){return mix(pow(c/1.055+0.0521327,vec3(2.4)),c/12.92,lessThanEqual(c,vec3(0.04045)));}
vec3 linearSRGB(vec3 c){return mix(c*12.92,pow(c,vec3(1.0/2.4))*1.055-0.055,greaterThan(c,vec3(0.0031308)));}
void main(){
    ivec2 p=ivec2(gl_FragCoord.xy);p.y=int(pc.osz.y)-1-p.y;         // this output pixel in the game's row order
    vec3 shown=texelFetch(Scene,p,0).xyz;
    if(pc.ui.z>0.5){
        vec4 gui=textureLod(UiColor,(vec2(p)+0.5)*pc.osz.zw,0.0);
        if(gui.a>0.0 || gui.r+gui.g+gui.b>0.0)                      // premultiplied: nothing of the UI on this pixel otherwise
            shown=linearSRGB(SRGBlinear(shown)*(1.0-gui.a)+SRGBlinear(gui.xyz)*pc.ui.y);
    }
    if(pc.ui.x!=1.0)shown=pow(max(shown,vec3(0.0)),vec3(pc.ui.x));
    if(pc.ui.w>0.5)shown=SRGBlinear(clamp(shown,0.0,1.0));
    Final=vec4(shown,1.0);
}
