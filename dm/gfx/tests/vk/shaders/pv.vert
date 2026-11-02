// t_vk_provoking: the position from NVN vertex attribute 0, and a flat varying that differs per vertex: the vertex's index (gl_VertexIndex: the index the draw fetched, the base
// vertex or first added) plus 1
#version 450
layout(location = 0) in vec4 p;
layout(location = 0) flat out uint v;
void main() {
    gl_Position = p;
    gl_PointSize = 1.0;
    v = uint(gl_VertexIndex) + 1u;
}
