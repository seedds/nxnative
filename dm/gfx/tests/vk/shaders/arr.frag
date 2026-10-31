// t_vk_raster: an array of two textures (an array of driver-table handles, nvn_refl.h TEXA / SAMPA: element k is the texture of the c2 word at byte 0x20 + 8k, the combined slots 0
// and 1): the left half of the target from element 0, the right half from element 1
#version 450
layout(set = 2, binding = 2048) uniform sampler2D t[2];
layout(location = 0) out vec4 o;
void main() { ivec2 p = ivec2(gl_FragCoord.xy); o = p.x < 4 ? texelFetch(t[0], ivec2(0), 0) : texelFetch(t[1], ivec2(0), 0); }
