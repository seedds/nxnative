// Experimental primitive only: not connected to game replay.
#include <metal_stdlib>
using namespace metal;
struct rc_mesh_job {
    device const ulong *xslots, *yslots;
    device const uint *record;
    device uint *args;
    ulong xconstant, yconstant;
    uint nx, ny, not_equal, pad;
};
kernel void rc_mesh_mask(device const rc_mesh_job *jobs [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    rc_mesh_job j = jobs[i];
    ulong x = j.xconstant, y = j.yconstant;
    for (uint k = 0; k < j.nx; ++k) x += *((device const ulong *)j.xslots[k]);
    for (uint k = 0; k < j.ny; ++k) y += *((device const ulong *)j.yslots[k]);
    bool live = j.not_equal ? x != y : x == y;
    j.args[0] = live ? j.record[0] : 0u;
    j.args[1] = 1u; j.args[2] = 1u; j.args[3] = 0u;
    j.args[4] = j.record[1];
}

// Standard draw arguments: preserve index offset, signed base vertex and instances.
kernel void rc_draw_mask(device const rc_mesh_job *jobs [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    rc_mesh_job j=jobs[i]; ulong x=j.xconstant,y=j.yconstant;
    for(uint k=0;k<j.nx;k++)x+=*((device const ulong*)j.xslots[k]);
    for(uint k=0;k<j.ny;k++)y+=*((device const ulong*)j.yslots[k]);
    bool live=j.not_equal?x!=y:x==y;
    for(uint k=0;k<j.pad;k++)j.args[k]=(k==0&&!live)?0u:j.record[k];
}
