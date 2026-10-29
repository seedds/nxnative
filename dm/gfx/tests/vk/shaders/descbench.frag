// t_vk_descbench, cached sets: a per-draw bank (c2's role, dynamic), a material's constant buffer and two textures, in the universal layout's split (buffers in set 0, textures in set 1)
#version 450
layout(set = 0, binding = 1) uniform C2 { vec4 k; } c2;
layout(set = 0, binding = 2) uniform M { vec4 tint; } m;
layout(set = 1, binding = 0) uniform sampler2D t0;
layout(set = 1, binding = 1) uniform sampler2D t1;
layout(location = 0) out vec4 o;
void main() { o = texture(t0, vec2(0.5)) * m.tint + texture(t1, vec2(0.5)) * c2.k; }
