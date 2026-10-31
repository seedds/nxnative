// t_vk_transform_feedback: the Metal test's capture fixture (test_transform_feedback.py): every vertex n captures five floats {n, n + 10, n + 40, n + 50, n + 60} interleaved
// into transform feedback buffer 0, 20 bytes a vertex; a point, no fragment output needed (rasterizer discard or a colour mask of 0)
#version 450
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 20) out float a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 4) out vec4 b;
void main() {
    float n = float(gl_VertexIndex);
    a = n; b = vec4(n + 10.0, n + 40.0, n + 50.0, n + 60.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0;
}
