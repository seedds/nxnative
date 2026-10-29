// t_vk_primitive_restart: eight corners of two rectangles (left and right), chosen by the index modulo 8 (the Metal test's v)
#version 450
const vec2 p[8] = vec2[](vec2(-0.9, -0.8), vec2(-0.3, -0.8), vec2(-0.3, 0.8), vec2(-0.9, 0.8), vec2(0.3, -0.8), vec2(0.9, -0.8), vec2(0.9, 0.8), vec2(0.3, 0.8));
void main() { gl_Position = vec4(p[gl_VertexIndex % 8], 0.5, 1.0); }
