// t_vk_descbench, push descriptors: the same reads as descbench.frag with every binding in the one pushed set 0
#version 450
layout(set = 0, binding = 1) uniform C2 { vec4 k; } c2;
layout(set = 0, binding = 2) uniform M { vec4 tint; } m;
layout(set = 0, binding = 3) uniform sampler2D t0;
layout(set = 0, binding = 4) uniform sampler2D t1;
layout(location = 0) out vec4 o;
void main() { o = texture(t0, vec2(0.5)) * m.tint + texture(t1, vec2(0.5)) * c2.k; }
