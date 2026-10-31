// t_vk_feedback: a draw that reads the depth attachment of its pass (soft particles: depth tested, not written) and writes the depth under the pixel into red
#version 450
layout(set = 2, binding = 2048) uniform sampler2D d;
layout(location = 0) out vec4 o;
void main() { o = vec4(texelFetch(d, ivec2(gl_FragCoord.xy), 0).r, 0.0, 0.0, 1.0); }
