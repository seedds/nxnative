// t_vk_provoking: target 0 (R32UI) the flat varying, the provoking vertex's index + 1; target 1 (R32UI) the primitive's ID + 1 (the primitive ID counts the primitives the
// device draws, in order), which tells the test which primitive a pixel belongs to
#version 450
layout(location = 0) flat in uint v;
layout(location = 0) out uint o_v;
layout(location = 1) out uint o_p;
void main() {
    o_v = v;
    o_p = uint(gl_PrimitiveID) + 1u;
}
