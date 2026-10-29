// t_vk_integer_blend: two unsigned integer outputs (R32UI and RGBA8UI targets, blending asked for on both), the literal values of the Metal test
#version 450
layout(location = 0) out uvec4 a;
layout(location = 1) out uvec4 b;
void main() { a = uvec4(0x12345678u); b = uvec4(17u, 34u, 51u, 68u); }
