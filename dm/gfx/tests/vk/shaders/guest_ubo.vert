// t_vk_guest: the colour is element 2 of an array of four in a uniform block (the test moves the array's ArrayStride onto the block's member, as Super Mario Galaxy's
// generated SPIR-V has it, and the guest layer moves it back)
#version 450
layout(set = 0, binding = 0, std140) uniform U { vec4 c[4]; } u;
layout(location = 0) in vec2 p;
layout(location = 0) flat out vec4 col;
void main() { gl_Position = vec4(p, 0.0, 1.0); col = u.c[2]; }
