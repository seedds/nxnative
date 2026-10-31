// t_vk_transform_feedback: Pokémon Legends: Z-A's skinning layout (the Metal test's .split fixture): three capture buffers with their own strides, a position (12 bytes) into
// buffer 0 and one packed word (4 bytes) into each of buffers 1 and 2; vertex n captures {n, n + 10, n + 20} and n + 40 twice
#version 450
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 12) out vec3 p;
layout(location = 1, xfb_buffer = 1, xfb_offset = 0, xfb_stride = 4) out float q;
layout(location = 2, xfb_buffer = 2, xfb_offset = 0, xfb_stride = 4) out float r;
void main() {
    float n = float(gl_VertexIndex);
    p = vec3(n, n + 10.0, n + 20.0); q = n + 40.0; r = n + 40.0;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0;
}
