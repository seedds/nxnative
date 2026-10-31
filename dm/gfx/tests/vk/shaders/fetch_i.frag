// t_vk_formats: the texel under the pixel through a signed integer texture of slot 0 (translator binding: set 2, 512 x fragment stage index 4 + slot)
#version 450
layout(set = 2, binding = 2048) uniform isampler2D t;
layout(location = 0) out ivec4 o;
void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }
