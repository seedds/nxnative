// Vulkan fans: preserve rim-first provoking vertices and compact restart-separated runs after GPU index writes.
#include <metal_stdlib>
using namespace metal;
struct Fan { ulong offset; uint count, isz, restart, inst; int base; uint first_inst; };
static uint index_at(device const uchar *src, uint i, uint isz) {
    return isz == 2 ? ((device const ushort *)src)[i] : ((device const uint *)src)[i];
}
kernel void vk_fan(device const uchar *input [[buffer(0)]], device uint *out [[buffer(1)]], device uint *args [[buffer(2)]],
                   constant Fan &p [[buffer(3)]], uint i [[thread_position_in_grid]]) {
    device const uchar *src = input + p.offset;
    if (!p.restart) {
        if (i < p.count - 2) {
            out[3*i] = index_at(src, i+1, p.isz); out[3*i+1] = index_at(src, i+2, p.isz); out[3*i+2] = index_at(src, 0, p.isz);
        }
        if (i) return;
        args[0] = (p.count - 2) * 3;
    } else {
        if (i) return;
        uint n = 0, have = 0, anchor = 0, previous = 0, stop = p.isz == 2 ? 0xffffu : 0xffffffffu;
        for (uint j = 0; j < p.count; j++) {
            uint v = index_at(src, j, p.isz);
            if (v == stop) { have = 0; continue; }   // compare before applying baseVertex
            if (!have) anchor = v;
            else if (have >= 2) { out[n++] = previous; out[n++] = v; out[n++] = anchor; }
            previous = v; if (have < 2) have++;
        }
        args[0] = n;
    }
    // MTLDrawIndexedPrimitivesIndirectArguments; baseVertex stays signed in its own word.
    args[1] = p.inst; args[2] = 0; args[3] = as_type<uint>(p.base); args[4] = p.first_inst;
}
