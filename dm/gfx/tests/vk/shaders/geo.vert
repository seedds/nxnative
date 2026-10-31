// t_vk_geometry: the cell of a point (attribute 0, per vertex) plus its instance's offset (attribute 1, a per-instance stream), as raw cell coordinates for the geometry stage
#version 450
layout(location = 0) in vec2 cell;
layout(location = 1) in vec2 off;
void main() { gl_Position = vec4(cell + off, 0.0, 1.0); gl_PointSize = 1.0; }
