// t_vk_geometry: layered rendering without a geometry stage (shaderOutputLayer, design 2.2's gs_slice fold): a triangle covering the target per instance, written into layer
// instance index, its colour that layer's (layer 0 green, 1 blue, 2 red)
#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(location = 0) flat out vec4 col;
void main() {
    int l = gl_InstanceIndex;
    gl_Position = vec4(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0, 0.0, 1.0); gl_Layer = l;
    col = vec4(l == 2 ? 1.0 : 0.0, l == 0 ? 1.0 : 0.0, l == 1 ? 1.0 : 0.0, 1.0);
}
