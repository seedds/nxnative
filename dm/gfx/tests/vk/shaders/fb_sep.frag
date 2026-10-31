// t_vk_feedback: fb_swz.frag through the separate texture slot 128 with the separate sampler slot 0 (the translator's binding 512 x fragment stage index 4 + 128; the Metal test's
// slot cases 0, 128 and 255)
#version 450
layout(set = 2, binding = 2176) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0).gbra; }
