// Indirect-count multi-draws masked on the GPU (nvn_mtl.m mdx_run)
#include <metal_stdlib>
using namespace metal;
struct mjob { device const uint *rec; device const uint *cnt; device uint *out; uint i, words; };
kernel void mdx_mask(device const mjob *j [[buffer(0)]], uint t [[thread_position_in_grid]]) {
  mjob m = j[t]; if (!m.out) return;
  if (!m.words) { m.out[0] = m.rec[0]; m.out[1] = 1u; m.out[2] = 1u; return; }   // an indirect mesh draw: NVN's {count, first} as Metal's threadgroups {x, y, z} (nvn_geometry.inc mesh_tasks_draw)
  bool live = m.i < *m.cnt;
  for (uint k = 0; k < m.words; k++) m.out[k] = live ? m.rec[k] : 0u; }
