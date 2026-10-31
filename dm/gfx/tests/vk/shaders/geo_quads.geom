// t_vk_geometry: the Metal test's expansion (t_geometry.m replay): two invocations per input point, each emitting a 2x2-pixel quad as a triangle strip, at the point's 4-pixel
// cell of a 32x32 target, invocation i 2 pixels to the right of invocation 0
#version 450
layout(points, invocations = 2) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    vec2 o = gl_in[0].gl_Position.xy * 4.0 + vec2(float(gl_InvocationID) * 2.0, 0.0);
    for (int k = 0; k < 4; k++) {
        vec2 px = o + vec2(float(k & 1), float(k >> 1)) * 2.0;
        gl_Position = vec4(px.x / 16.0 - 1.0, 1.0 - px.y / 16.0, 0.0, 1.0);   // pixel rows down from the top (the upper-left window origin: NDC +y is the top row)
        EmitVertex();
    }
    EndPrimitive();
}
