// t_vk_geometry: attribute 0 (two floats) as the position, as it is (the geometry stage reads it in pixels)
#version 450
layout(location = 0) in vec2 p;
void main() { gl_Position = vec4(p, 0.0, 1.0); }
