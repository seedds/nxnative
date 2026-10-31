// t_vk_transform_feedback: a geometry shader that captures what it emits: every input point p becomes one triangle whose vertices k capture {p, k} (8 bytes) into buffer 0;
// only whole triangles are captured, so a buffer of 7 vertices takes two triangles (the Metal test's "incomplete final primitive is not captured")
#version 450
layout(points) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 8) out vec2 v;
void main() {
    for (int k = 0; k < 3; k++) {
        v = vec2(float(gl_PrimitiveIDIn), float(k));
        gl_Position = vec4(float(k) * 0.1, 0.0, 0.0, 1.0);
        EmitVertex();
    }
    EndPrimitive();
}
