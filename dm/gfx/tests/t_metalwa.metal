// t_metalwa.metal: three small kernels, written for this test in the shape SPIRV-Cross gives translated shaders, around the macOS 26 GPU compiler bugs dmsh_metalwa.py works
// around (KosmicKrisp's KK_WORKAROUND_3 and _9). t_metalwa.m runs them; test_dmsh_metalwa.py compiles them as built, at -O0 and rewritten. Every loop is bounded (4096 rounds).
#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

inline uint4 spvSubgroupBallot(bool value)
{
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0, 0);
}

// KK3: a ballot in a branch whose value merges after it. in[0] is a lane mask (the harness passes the odd lanes); a lane in it must get exactly that mask, the others 2
kernel void kk3_branch(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]], uint tid [[thread_position_in_grid]])
{
    bool c = ((in[0] >> gl_SubgroupInvocationID) & 1u) != 0u;
    uint m;
    if (c)
    {
        m = spvSubgroupBallot(true).x;
    }
    else
    {
        m = 2u;
    }
    out[tid] = m;
}

// KK3: a ballot waterfall, one key per round: the pending lanes ballot, the lowest one's key is taken, the lanes holding it leave. Every lane must come out after as many rounds as
// its SIMD group has distinct keys among its active lanes (act[tid] != 0)
kernel void kk3_waterfall(device const uint* key [[buffer(0)]], device uint* out [[buffer(1)]], device const uint* act [[buffer(2)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]], uint tid [[thread_position_in_grid]])
{
    if (act[tid] == 0u)
    {
        out[tid] = 0u;
        return;
    }
    uint k = key[tid];
    bool pending = true;
    uint rounds = 0u;
    bool more;
    do
    {
        rounds++;
        uint m = 0u;
        if (pending)
        {
            m = spvSubgroupBallot(true).x;
        }
        if (pending)
        {
            uint lk = simd_shuffle(k, ushort(ctz(m)));
            if (lk == k)
            {
                pending = false;
            }
        }
        more = spvSubgroupBallot(pending).x != 0u;
    } while (more && rounds < 4096u);
    out[tid] = rounds;
}

// KK9: the only break of a loop, after an election: the ballot before the break must hold the elected lane alone (1 << lane)
kernel void kk9_break(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]], uint tid [[thread_position_in_grid]])
{
    uint n = in[1];
    uint rec = 0xdeadu;
    for (uint i = 0u; i < n; i++)
    {
        if (simd_is_first())
        {
            rec = spvSubgroupBallot(true).x;
            break;
        }
    }
    out[tid] = rec;
}
