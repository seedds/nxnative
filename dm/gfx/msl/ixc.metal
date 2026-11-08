// Indirect draws Metal cannot take as they are: records converted to indexed indirect draws (nvn_mtl.m ixc_run)
#include <metal_stdlib>
using namespace metal;
struct ajob { device const uint *rec; device uint *args; uint prim, cap; };
kernel void ixc_arrays(device const ajob *j [[buffer(0)]], uint i [[threadgroup_position_in_grid]]) {
  device const uint *s = j[i].rec; uint n = s[0], c = j[i].prim == 7 ? n / 4 * 6 : n >= 3 ? (n - 2) * 3 : 0;
  device uint *d = j[i].args; d[0] = min(c, j[i].cap); d[1] = s[1]; d[2] = 0; d[3] = s[2]; d[4] = s[3]; }
struct ejob { device const uint *rec; device const uchar *ib; device uint *out; device uint *args; uint prim, isz, ilen, cap; };
kernel void ixc_expand(constant ejob &j [[buffer(0)]], uint i [[thread_position_in_grid]]) {
  uint n = j.rec[0], m = min(j.prim == 7 ? n / 4 * 6 : j.prim == 6 ? (n >= 3 ? (n - 2) * 3 : 0) : n, j.cap);
  if (i == 0) { j.args[0] = m; j.args[1] = j.rec[1]; j.args[2] = 0; j.args[3] = j.rec[3]; j.args[4] = j.rec[4]; }
  if (i >= m) return;
  uint t = i / 3, c = i % 3, p = j.prim == 7 ? t / 2 * 4 + (c ? c + (t & 1) : 0) : j.prim == 6 ? (c ? t + c : 0) : i, x = j.rec[2] + p;
  j.out[i] = (ulong)x * j.isz + j.isz > j.ilen ? 0u : j.isz == 1 ? uint(j.ib[x]) : j.isz == 2 ? uint(((device const ushort *)j.ib)[x]) : ((device const uint *)j.ib)[x]; }
// DM_NVN_DRAW_CLAMP=1 (nvn_mtl.m dcl_prepass): a plain DrawArraysIndirect {count, instances, first, base instance} / DrawElementsIndirect {count, instances, first index, base vertex,
// base instance} record copied into the frame arena before the render pass; a count x instances over maxtotal becomes 0 x 0 (draws nothing). A record the GPU wrote from bad
// data (count x instances near 2^32) otherwise keeps the GPU busy until macOS resets it. Each such record adds 1 to hits[0]; the first also leaves its words (1-5) there
struct cjob { device const uint *rec; device uint *out; device atomic_uint *hits; uint words, maxtotal; };
kernel void dcl_clamp(device const cjob *j [[buffer(0)]], uint i [[threadgroup_position_in_grid]]) {
  device const uint *s = j[i].rec; device uint *d = j[i].out; bool ix = j[i].words == 5u;
  uint c = s[0], n = s[1]; bool over = (ulong)c * n > j[i].maxtotal;
  d[0] = over ? 0u : c; d[1] = over ? 0u : n; d[2] = s[2]; d[3] = s[3]; d[4] = ix ? s[4] : 0u;
  if (over && atomic_fetch_add_explicit(j[i].hits, 1u, memory_order_relaxed) == 0u) {
    device uint *h = (device uint *)j[i].hits; h[1] = c; h[2] = n; h[3] = s[2]; h[4] = s[3]; h[5] = ix ? s[4] : 0u; } }
