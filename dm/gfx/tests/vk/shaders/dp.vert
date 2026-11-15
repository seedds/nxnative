// t_vk_draw_params: a triangle covering the whole target, as fullscreen.vert, at a depth from the draw's base instance (gl_BaseInstance: the DrawParameters capability, which
// the game shaders' translations declare when they read the base vertex, the base instance or the draw index)
#version 460
void main() { gl_Position = vec4(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0, gl_BaseInstance == 0 ? 0.0 : 0.5, 1.0); }
