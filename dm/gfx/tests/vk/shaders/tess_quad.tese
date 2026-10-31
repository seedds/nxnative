// t_vk_tessellation: the quad domain with equal spacing, each generated vertex placed bilinearly between the 4 control points and its domain coordinate captured (transform
// feedback buffer 0, 16 bytes a vertex: the generated triangles counted exactly)
#version 450
layout(quads, equal_spacing, ccw) in;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 tc;
void main() {
    vec2 u = gl_TessCoord.xy;
    vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, u.x), b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, u.x);
    gl_Position = mix(a, b, u.y);
    tc = vec4(u, 0.0, 1.0);
}
