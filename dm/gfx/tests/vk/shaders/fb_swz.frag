// t_vk_feedback: a draw that samples the attachment it writes (agl's post effect, rendering.md "Same-pass feedback read through a snapshot"): the texel under the pixel through the
// combined texture of slot 0, written back with its channels rotated (r g b a → g b r a), so what was read before the draw is visible in what it wrote
#version 450
layout(set = 2, binding = 2048) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(t, ivec2(gl_FragCoord.xy), 0).gbra; }
