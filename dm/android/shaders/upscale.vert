#version 450
// [DOOM] Full-viewport triangle for the present upscale pass (gsr.frag, DM_GFX_UPSCALE). Offline compiled into upscale.vert.spv.inc:
//   glslc --target-env=vulkan1.1 -O -mfmt=num upscale.vert -o upscale.vert.spv.inc
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
