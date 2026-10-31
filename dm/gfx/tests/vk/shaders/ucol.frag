// t_vk_raster: the colour of uniform slot 0 of the fragment stage (c3: the translator's binding 1 + 18 x stage 4 + 3)
#version 450
layout(set = 0, binding = 76, std140) uniform C { vec4 c; } u;
layout(location = 0) out vec4 o;
void main() { o = u.c; }
