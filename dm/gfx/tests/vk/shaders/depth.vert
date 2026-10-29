// t_vk_depth16_clear: the position from attribute 0 and a depth from attribute 1, passed flat to the fragment shader
#version 450
layout(location = 0) in vec4 p;
layout(location = 1) in float d;
layout(location = 0) flat out float od;
void main() { gl_Position = p; od = d; }
