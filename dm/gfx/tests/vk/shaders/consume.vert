// t_vk_rast_discard: a point per vertex, each copying what the producer wrote 4096 words further (the Metal test's cd)
#version 450
layout(set = 1, binding = 0, std430) buffer B { uint p[]; };
void main() { uint n = uint(gl_VertexIndex); p[n + 4096u] = p[n]; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }
