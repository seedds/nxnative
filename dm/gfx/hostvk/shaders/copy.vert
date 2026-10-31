// Draw-based texture copies (docs/design/vulkan-backend.md 2.7; step P8; vk_pipeline.c hvk_copy_draw): one triangle over the destination region, which the viewport is
#version 450
void main() {
    vec2 q = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
}
