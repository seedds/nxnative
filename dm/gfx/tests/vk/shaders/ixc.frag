// t_vk_index_convert: target 0 (added with ONE + ONE) counts the primitives over a pixel, front-facing in red and back-facing in green, a step of 1/255 each; target 1 (no blending)
// holds the last primitive drawn over it, its ID + 1 in the red, green and blue bytes (the primitive ID counts a draw's primitives in order; a restart does not reset it)
#version 450
layout(location = 0) out vec4 count;
layout(location = 1) out vec4 last;
void main() {
    count = gl_FrontFacing ? vec4(1.0 / 255.0, 0.0, 0.0, 0.0) : vec4(0.0, 1.0 / 255.0, 0.0, 0.0);
    uint id = uint(gl_PrimitiveID) + 1u;
    last = vec4(float(id & 255u), float((id >> 8) & 255u), float((id >> 16) & 255u), 255.0) / 255.0;
}
