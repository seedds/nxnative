// t_vk_geometry: the form dmsh_build.py calls gs_slice (Unreal's FWriteToSliceGS, DRAGON QUEST XI S): triangles in, the positions copied, the layer taken from an input; the
// colour of the layer (layer 0 red, 1 green, 2 blue) passed on
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in int li[];
layout(location = 0) flat out vec4 col;
void main() {
    for (int k = 0; k < 3; k++) {
        gl_Position = gl_in[k].gl_Position; gl_Layer = li[k];
        col = vec4(li[k] == 0 ? 1.0 : 0.0, li[k] == 1 ? 1.0 : 0.0, li[k] == 2 ? 1.0 : 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
