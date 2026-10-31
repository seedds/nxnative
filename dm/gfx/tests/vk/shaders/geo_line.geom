// t_vk_geometry: Big Brain Academy's line renderer in small (shaders.md, "Geometry shaders with no Metal stage"): a line strip with adjacency in, each segment a quad 2 pixels
// wide around it, as a triangle strip; positions in pixels of a 32x32 target (attribute 0 through the vertex stage)
#version 450
layout(lines_adjacency) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    vec2 a = gl_in[1].gl_Position.xy, b = gl_in[2].gl_Position.xy, d = normalize(b - a), n = vec2(-d.y, d.x);
    vec2 q[4] = vec2[](a - n, a + n, b - n, b + n);
    for (int k = 0; k < 4; k++) { gl_Position = vec4(q[k].x / 16.0 - 1.0, 1.0 - q[k].y / 16.0, 0.0, 1.0); EmitVertex(); }   // rows down from the top (upper-left origin)
    EndPrimitive();
}
