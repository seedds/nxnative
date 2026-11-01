#version 320 es
// t_vk_guest_gl: a textured quad in Digimon's dialect (GLSL ES 3.20): a rectangle in NDC from uRect (offset xy, scale zw) at clip z uDepth, which reaches a depth
// buffer only through gl_shaders.py --vk's clip-space pass (GL's z from -w to w; Vulkan clips below 0)
precision highp float;
in vec2 aPos;
in vec2 aUV;
uniform vec4 uRect;
uniform float uDepth;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos * uRect.zw + uRect.xy, uDepth, 1.0);
}
