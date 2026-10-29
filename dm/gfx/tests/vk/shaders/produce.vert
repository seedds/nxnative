// t_vk_rast_discard: a point per vertex, each writing index + 0x1200 into storage buffer 0 (the Metal test's pd)
#version 450
layout(set = 1, binding = 0, std430) buffer B { uint p[]; };
void main() { uint n = uint(gl_VertexIndex); p[n] = n + 0x1200u; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }
