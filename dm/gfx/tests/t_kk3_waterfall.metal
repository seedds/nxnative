// PRAGMATA 965043fe128ef98f, loop 1: its ballot waterfall (one key per iteration: ballot the lanes, take the lowest, shuffle its key, the lanes that match leave), from
// the r9 MSL (lines 928-1073 of 965043fe128ef98f_b.metal) as built, in a test kernel with a 4096 iteration cap (capc). test_dmsh_metalwa.py: dmsh_metalwa KK3 must rewrite its
// four ballots; with DMSH_TEST_METAL=1, t_kk3_waterfall.m runs it on lane patterns (macOS 26: -O1 never converges with two keys in a SIMD group, -O0 and KK3 do)
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>
#include <metal_atomic>

using namespace metal;

// Implementation of the unsigned GLSL findMSB() function
template<typename T>
inline T spvFindUMSB(T x)
{
    uint dm_lg = 0u; (void)dm_lg;
    return select(clz(T(0)) - (clz(x) + T(1)), T(-1), x == T(0));
}

inline uint4 spvSubgroupBallot(bool value)
{
    uint dm_lg = 0u; (void)dm_lg;
    simd_vote vote = simd_ballot(value);
    // simd_ballot() returns a 64-bit integer-like object, but
    // SPIR-V callers expect a uint4. We must convert.
    // FIXME: This won't include higher bits if Apple ever supports
    // 128 lanes in an SIMD-group.
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0, 0);
}

template<typename T>
inline T spvSubgroupShuffle(T value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return simd_shuffle(value, lane);
}

template<>
inline bool spvSubgroupShuffle(bool value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return !!simd_shuffle((ushort)value, lane);
}

template<uint N>
inline vec<bool, N> spvSubgroupShuffle(vec<bool, N> value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return (vec<bool, N>)simd_shuffle((vec<ushort, N>)value, lane);
}

template<>
inline ulong spvSubgroupShuffle(ulong value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return as_type<ulong>(spvSubgroupShuffle(as_type<uint2>(value), lane));
}

template<>
inline ulong2 spvSubgroupShuffle(ulong2 value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return ulong2(spvSubgroupShuffle(value.x, lane), spvSubgroupShuffle(value.y, lane));
}

inline ulong3 spvSubgroupShuffle(ulong3 value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return ulong3(spvSubgroupShuffle(value.xy, lane), spvSubgroupShuffle(value.z, lane));
}

inline ulong4 spvSubgroupShuffle(ulong4 value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return ulong4(spvSubgroupShuffle(value.xy, lane), spvSubgroupShuffle(value.zw, lane));
}

template<uint N>
inline vec<long, N> spvSubgroupShuffle(vec<long, N> value, ushort lane)
{
    uint dm_lg = 0u; (void)dm_lg;
    return vec<long, N>(spvSubgroupShuffle(vec<ulong, N>(value), lane));
}

static inline __attribute__((always_inline)) bool capc(thread uint& c, uint cap) { c += 1u; return c < cap; }
static inline __attribute__((always_inline))
int _77(thread const int& _3368, thread const int& _3369, thread const int& _3370, thread int& _3371, thread uint& gl_SubgroupInvocationID)
{
    uint dm_lg = 0u; (void)dm_lg;
    int _3374 = _3370 & 31;
    uint _3376 = uint(int(uint(_3370) >> uint(8)));
    int _3378 = int(_3376) & 31;
    float _3380 = as_type<float>(gl_SubgroupInvocationID);
    int _3382 = as_type<int>(_3380) & _3378;
    int _3384 = ~_3378;
    int _3386 = _3374 & _3384;
    int _3388 = _3386 | _3382;
    int _3390 = ~_3378;
    int _3392 = _3369 & _3390;
    int _3394 = _3392 | _3382;
    bool _3396 = uint(_3394) <= uint(_3388);
    _3371 = _3396 ? (-1) : 0;
    float _3398 = spvSubgroupShuffle(as_type<float>(_3368), uint(_3394));
    uint _3400 = uint(_3396 ? as_type<int>(_3398) : _3368);
    return int(_3400);
}

kernel void wf(device const uint* vin [[buffer(0)]], device uint* vout [[buffer(1)]], device const uint* act [[buffer(2)]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]], uint tid [[thread_position_in_grid]])
{
    uint4 gl_SubgroupLtMask = uint4(extract_bits(0xFFFFFFFF, 0, min(gl_SubgroupInvocationID, 32u)), uint3(0));
    if (act[tid] == 0u) { vout[tid*4+0] = 0xDEADu; return; }
    float _171 = as_type<float>(vin[tid]);
    int _420 = 0, _236 = 0, _512 = 0; uint dm_lg = 0u;
    bool _718 = false; bool _566 = false;
    _566 = false;
    uint _570 = spvSubgroupBallot(true).x;
    bool _572 = 0 != int(_570);
    bool _574 = !_572;
    int _710;
    int _714;
    bool _716;
    if (!_574)
    {
        int _576 = 1;
        int _578 = 0;
        int _580 = 0;
        int _582 = 0;
        bool _584;
        int _604;
        int _648;
        int _698;
        do
        {
            int _586 = _576;
            int _588 = _578;
            int _590 = _420;
            int _592 = _236;
            int _594 = _512;
            int _596 = _580;
            int _598 = _582;
            int _600 = _586 & 1;
            bool _602 = _600 != 0;
            _604 = int(uint(_588) + 1u);
            bool _606 = !_602;
            int _608 = _594;
            int _610 = _586;
            int _612 = _598;
            int _614 = _592;
            int _616 = _596;
            int _618 = _586;
            int _620 = _590;
            int _622 = _592;
            int _624 = _596;
            int _626 = _586;
            int _628 = _598;
            if (!_606)
            {
                uint _630 = spvSubgroupBallot(true).x;
                int _632 = reverse_bits(int(_630));
                int _634 = int(spvFindUMSB(uint(_632)));
                bool _636 = _634 != (-1);
                int _638 = _634 ^ 31;
                int _640 = _636 ? _638 : _634;
                int _642 = as_type<int>(_171);
                int _644 = _640;
                int _646 = 31;
                int _1680 = _77(_642, _644, _646, _648, gl_SubgroupInvocationID);
                int _650 = _1680;
                bool _652 = _650 != as_type<int>(_171);
                int _654 = _652 ? 0 : 1065353216;
                int _656 = _592;
                _620 = _650;
                if (!_652)
                {
                    float _658 = as_type<float>(gl_SubgroupLtMask.x);
                    _656 = as_type<int>(_658);
                }
                int _660 = _656;
                if (!_652)
                {
                    uint _662 = spvSubgroupBallot(true).x;
                    _608 = int(_662);
                }
                int _664 = _608;
                int _666 = _664;
                if (!_652)
                {
                    _610 = 0;
                }
                int _668 = _610;
                _618 = _668;
                _626 = _668;
                if (!_652)
                {
                    _612 = _604;
                }
                int _670 = _612;
                bool _672 = _654 != 0;
                _628 = _670;
                if (_672)
                {
                    _614 = _660;
                }
                int _674 = _614;
                _622 = _674;
                if (!_652)
                {
                    int _676 = _664 & _674;
                    _666 = _676;
                }
                int _678 = _666;
                int _680 = _652 ? 0 : 1065353216;
                int _682 = _596;
                if (!_652)
                {
                    int _684 = popcount(_678);
                    _682 = _684;
                }
                int _686 = _682;
                bool _688 = _680 != 0;
                if (_688)
                {
                    _616 = _686;
                }
                int _690 = _616;
                _624 = _690;
            }
            int _692 = _618;
            int _694 = _620;
            int _696 = _622;
            _698 = _624;
            int _700 = _626;
            int _702 = _628;
            int _704 = _700 & 1;
            bool _706 = _704 != 0;
            uint _708 = spvSubgroupBallot(_706).x;
            _584 = int(_708) != 0;
            _576 = _692;
            _578 = _604;
            _420 = _694;
            _236 = _696;
            _512 = int(_708);
            _580 = _698;
            _582 = _702;
            _710 = _702;
        } while ((_584) && capc(dm_lg, 4096u));
        bool _712 = _698 != 0;
        _714 = _604;
        _716 = _712;
        _718 = true;
    }
    else
    {
        _714 = 0;
        _710 = 0;
        _716 = false;
    }
    _718 = false;
    int _720 = _714;
    int _722 = _710;
    vout[tid*4+0] = uint(_714); vout[tid*4+1] = uint(_710); vout[tid*4+2] = dm_lg; vout[tid*4+3] = uint(_716);
}
