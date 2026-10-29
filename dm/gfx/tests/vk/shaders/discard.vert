// t_vk_rast_discard: every vertex writes its index and instance into storage buffer 0 (the Metal test's v), and the full-target triangle's corner
#version 450
layout(set = 1, binding = 0, std430) buffer B { uint p[]; };
void main() {
    uint n = uint(gl_VertexIndex), i = uint(gl_InstanceIndex);
    p[n + 16u * i] = 0x500u + n + 16u * i;
    vec2 q[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(q[n % 3u], 0.0, 1.0);
}
