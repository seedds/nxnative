// t_vk_tessellation: the triangle domain with equal spacing, each generated vertex the barycentric mix of the 3 control points, its domain coordinate captured (buffer 0, 16 bytes)
#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 tc;
void main() {
    vec3 u = gl_TessCoord;
    gl_Position = u.x * gl_in[0].gl_Position + u.y * gl_in[1].gl_Position + u.z * gl_in[2].gl_Position;
    tc = vec4(u, 1.0);
}
