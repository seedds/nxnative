// Indirect-count multi-draws masked on the GPU (nvn_mtl.m mdx_run)
#include <metal_stdlib>
using namespace metal;
struct mjob { device const uint *rec; device const uint *cnt; device uint *out; uint i, words; };
// An indirect mesh draw (words 0): NVN's {count, first} as Metal's threadgroups {count, 1, 1}, then MDX_MESH_PARTS draws of at most MDX_MESH_GRID workgroups each as {x, 1, 1, first}
// for a mesh shader with no task shader, whose threadgroups are the mesh grid (nvn_geometry.inc mtk_encode). bad[0] counts the draws with more workgroups than the parts hold
#define MDX_MESH_PARTS 8u
#define MDX_MESH_GRID 1024u
kernel void mdx_mask(device const mjob *j [[buffer(0)]], device atomic_uint *bad [[buffer(1)]], uint t [[thread_position_in_grid]]) {
  mjob m = j[t]; if (!m.out) return;
  if (!m.words) { uint c = m.rec[0], f = m.rec[1];
    m.out[0] = c; m.out[1] = 1u; m.out[2] = 1u; m.out[3] = 0u;
    for (uint k = 0; k < MDX_MESH_PARTS; k++) { uint o = k * MDX_MESH_GRID; device uint *p = m.out + 4u + 4u * k;
      p[0] = c > o ? min(c - o, MDX_MESH_GRID) : 0u; p[1] = 1u; p[2] = 1u; p[3] = f + o; }
    if (c > MDX_MESH_PARTS * MDX_MESH_GRID) atomic_fetch_add_explicit(&bad[0], 1u, memory_order_relaxed);
    return; }
  bool live = m.i < *m.cnt;
  for (uint k = 0; k < m.words; k++) m.out[k] = live ? m.rec[k] : 0u; }
