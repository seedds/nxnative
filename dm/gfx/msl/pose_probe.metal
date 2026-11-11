// Diagnostic only: snapshots the exact effective GPU binding after its draw pass.
#include <metal_stdlib>
using namespace metal;
struct PoseCopy { device const uchar *source; device uchar *destination; uint size, pad; };
kernel void pose_copy(constant PoseCopy& job [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i < job.size) job.destination[i] = job.source[i];
}
