// The clear triangle's vertex stage (docs/design/vulkan-backend.md 2.7; vk_pipeline.c hvk_clear_tri): one triangle over the whole pass at the clear's depth, which the depth / stencil
// clear writes with the depth test ALWAYS. Push constants: the colour value (the fragment stage's) and the depth
#version 450
layout(push_constant) uniform P { uvec4 v; float z; } p;
void main() {
    vec2 q = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(q * 2.0 - 1.0, p.z, 1.0);
}
