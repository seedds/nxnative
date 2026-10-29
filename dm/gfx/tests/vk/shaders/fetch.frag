// t_vk_view_swizzle: the texel under the pixel, through the combined texture of slot 0 (translator binding: set 2, 512 x fragment stage index 4 + slot)
#version 450
layout(set = 2, binding = 2048) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }
