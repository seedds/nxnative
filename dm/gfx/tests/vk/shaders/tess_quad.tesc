// t_vk_tessellation: a quad patch of 4 control points passed through, its outer and inner levels from uniform slot 0 of the control stage (c3: the translator's binding
// 1 + 18 x stage 1 + 3)
#version 450
layout(vertices = 4) out;
layout(set = 0, binding = 22, std140) uniform L { vec4 outer; vec4 inner; } lv;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = lv.outer.x; gl_TessLevelOuter[1] = lv.outer.y; gl_TessLevelOuter[2] = lv.outer.z; gl_TessLevelOuter[3] = lv.outer.w;
        gl_TessLevelInner[0] = lv.inner.x; gl_TessLevelInner[1] = lv.inner.y;
    }
}
