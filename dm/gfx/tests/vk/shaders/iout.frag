// t_vk_integer_blend: a signed integer output beside a normalized one (both targets have blending asked for)
#version 450
layout(location = 0) out ivec4 a;
layout(location = 1) out vec4 b;
void main() { a = ivec4(-2, 3, -4, 5); b = vec4(0.5); }
