// t_vk_descbench: a point at the draw's position, from the per-draw bank (c0's role: a dynamic uniform buffer, or a pushed one)
#version 450
layout(set = 0, binding = 0) uniform C0 { vec4 pos; } c0;
void main() { gl_Position = vec4(c0.pos.xy, 0.0, 1.0); gl_PointSize = 1.0; }
