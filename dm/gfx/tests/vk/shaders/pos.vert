// t_vk_* tests: the position from NVN vertex attribute 0 (four floats), as it is
#version 450
layout(location = 0) in vec4 p;
void main() { gl_Position = p; }
