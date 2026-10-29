// t_vk_* tests: a triangle covering the whole target, from the vertex index alone (no vertex buffers)
#version 450
void main() { gl_Position = vec4(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0, 0.0, 1.0); }
