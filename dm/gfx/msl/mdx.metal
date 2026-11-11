// Indirect-count multi-draws masked on the GPU (nvn_mtl.m mdx_run)
#include <metal_stdlib>
using namespace metal;
struct mjob { device const uint *rec; device const uint *cnt; device uint *out; uint i, words; };   // i: the draw's index in its multi-draw; for an indirect mesh draw (words 0) the mesh grid's limit
kernel void mdx_mask(device const mjob *j [[buffer(0)]], uint t [[thread_position_in_grid]]) {
  mjob m = j[t]; if (!m.out) return;
  // An indirect mesh draw: NVN's {count, first} as Metal's threadgroups {x, y, z}: {count, 1, 1} at +0 for a task shader's, and at +16 {ceil(count / limit), 1, 1} for the object
  // function of a mesh shader with no task, which launches the workgroups in blocks of the limit (dmsh_meshstage.py main0_o, nvn_geometry.inc mtk_encode)
  if (!m.words) { uint c = m.rec[0]; m.out[0] = c; m.out[1] = 1u; m.out[2] = 1u; m.out[4] = c / m.i + (c % m.i != 0u); m.out[5] = 1u; m.out[6] = 1u; return; }
  bool live = m.i < *m.cnt;
  for (uint k = 0; k < m.words; k++) m.out[k] = live ? m.rec[k] : 0u; }
