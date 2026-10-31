// t_vk_* tests: the colour the stage before gives (flat)
#version 450
layout(location = 0) flat in vec4 col;
layout(location = 0) out vec4 o;
void main() { o = col; }
