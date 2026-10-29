// t_vk_queue_origin: green for a front face, red for a back face (the Metal test's f)
#version 450
layout(location = 0) out vec4 o;
void main() { o = gl_FrontFacing ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }
