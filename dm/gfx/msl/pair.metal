// A storage pair's layout on this GPU (nvn_mtl.m pair_layout_ok): every sample s of pixel p of the 4-sample texture against texel (2p.x + (s & 1), 2p.y + (s >> 1)) of the
// single-sample one over the same bytes, compared as read; each sample that differs counts once
#include <metal_stdlib>
using namespace metal;
template <typename T> static void cmp(texture2d<T, access::read> one, texture2d_ms<T, access::read> four, device atomic_uint *bad, uint2 p) {
  if (p.x >= four.get_width() || p.y >= four.get_height()) return;
  for (uint s = 0; s < 4; s++)
    if (any(as_type<uint4>(one.read(uint2(2 * p.x + (s & 1), 2 * p.y + (s >> 1)))) != as_type<uint4>(four.read(p, s)))) atomic_fetch_add_explicit(bad, 1u, memory_order_relaxed);
}
kernel void pair_cmp_f(texture2d<float, access::read> one [[texture(0)]], texture2d_ms<float, access::read> four [[texture(1)]], device atomic_uint *bad [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { cmp(one, four, bad, p); }
kernel void pair_cmp_u(texture2d<uint, access::read> one [[texture(0)]], texture2d_ms<uint, access::read> four [[texture(1)]], device atomic_uint *bad [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { cmp(one, four, bad, p); }
kernel void pair_cmp_i(texture2d<int, access::read> one [[texture(0)]], texture2d_ms<int, access::read> four [[texture(1)]], device atomic_uint *bad [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { cmp(one, four, bad, p); }
