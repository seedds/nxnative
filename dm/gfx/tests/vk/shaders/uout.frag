// t_vk_integer_blend: an unsigned integer output beside a normalized one (both targets have blending asked for)
#version 450
layout(location = 0) out uvec4 a;
layout(location = 1) out vec4 b;
void main() { a = uvec4(0x12345678u, 0x9abcdef0u, 0x0fedcba9u, 0x87654321u); b = vec4(0.5); }
