// Keep-busy work for DM_NVN_GPU_BUSY (nvn_gpu_busy.m): each thread runs a dependent chain of n multiply-adds and writes nothing unless the result is impossible, so the
// dispatch costs GPU time and almost no memory traffic. A few threadgroups occupy a few of the GPU's cores while the game's frame needs the rest
#include <metal_stdlib>
using namespace metal;
kernel void gpu_busy(device uint *sink [[buffer(0)]], constant uint &n [[buffer(1)]], uint id [[thread_position_in_grid]]) {
  float a = float(id) * 0.001f;
  for (uint i = 0; i < n; i++) a = fma(a, 0.999999f, 0.000001f);
  if (a < -1.0f) sink[0] = id;   // never true (a stays in [0, 1]): keeps the chain
}
