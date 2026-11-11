#include <metal_stdlib>
using namespace metal;
struct vk_input_in {
    int signed_value [[attribute(0)]];
    uint unsigned_value [[attribute(1)]];
};
vertex void vk_input(vk_input_in in [[stage_in]], device uint2* result [[buffer(0)]], uint id [[vertex_id]]) {
    result[id] = uint2(in.signed_value, in.unsigned_value);
}
