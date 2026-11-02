// t_vk_index_convert: a vertex at a pixel centre of the 64x64 target chosen by a hash of its index (any 32-bit index, the base vertex added), no vertex buffers
#version 450
void main() {
    uint h = uint(gl_VertexIndex) * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    gl_Position = vec4((vec2(float(h & 63u), float((h >> 6) & 63u)) + 0.5) / 32.0 - 1.0, 0.5, 1.0);
    gl_PointSize = 1.0;
}
