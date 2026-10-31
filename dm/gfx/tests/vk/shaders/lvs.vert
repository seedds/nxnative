// t_vk_geometry: a triangle covering the target per instance, its instance index passed to the geometry stage (which picks the layer from it: gs_slice's form)
#version 450
layout(location = 0) flat out int li;
void main() { gl_Position = vec4(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0, 0.0, 1.0); li = gl_InstanceIndex; }
