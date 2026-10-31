// t_vk_transform_feedback: xfb5.vert's layout from another program (another pipeline in the same capture and pass): vertex n captures {n + 100, n + 110, n + 140, n + 150, n + 160}
#version 450
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 20) out float a;
layout(location = 1, xfb_buffer = 0, xfb_offset = 4) out vec4 b;
void main() {
    float n = float(gl_VertexIndex) + 100.0;
    a = n; b = vec4(n + 10.0, n + 40.0, n + 50.0, n + 60.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0;
}
