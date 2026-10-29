// t_vk_gpl_identity: two integer outputs (R32UI and RGBA8UI attachments) from float arithmetic on the inputs, whose bits show any difference in the last place
#version 450
layout(location = 0) in vec4 col;
layout(location = 1) flat in uint id;
layout(location = 2) noperspective in vec2 uv;
layout(location = 0) out uint o0;
layout(location = 1) out uvec4 o1;
void main() {
    float x = col.r * 1000.0 + uv.x * uv.y * 31.0 + inversesqrt(1.0 + col.g);
    o0 = floatBitsToUint(x * 1.0001) ^ id;
    o1 = uvec4(uint(gl_FragCoord.x) & 255u, uint(gl_FragCoord.y) & 255u, id >> 24, uint(x) & 255u);
}
