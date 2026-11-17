#version 450
// [DOOM] DM_GFX_TU (xbg_tu.h, BUGS_AND_FIXES 2.74): one triangle over the whole viewport; the passes work from gl_FragCoord.
void main(){
    vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
