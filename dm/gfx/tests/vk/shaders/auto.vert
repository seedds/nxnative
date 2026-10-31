// t_vk_transform_feedback: the draw DrawTransformFeedback makes: every vertex n writes 0x800 + n into word n of storage buffer 0 (the Metal test's consumer vd)
#version 450
layout(set = 1, binding = 0, std430) buffer B { uint p[]; };
void main() { uint n = uint(gl_VertexIndex); p[n] = 0x800u + n; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }
