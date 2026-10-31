// t_vk_guest: a position and a colour per vertex, the colour passed on flat (to col.frag)
#version 450
layout(location = 0) in vec2 p;
layout(location = 1) in vec4 c;
layout(location = 0) flat out vec4 col;
void main() { gl_Position = vec4(p, 0.0, 1.0); col = c; }
