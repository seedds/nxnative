#!/usr/bin/env python3
"""One copy of a raw global memory lookup's search (dmsh_rewrite.py raw_noinline): the translator's dmsh_raw_find (GlobalToStorage GetRawFindFunction), as spirv-cross writes it, is
marked noinline outside mesh stages; final mesh code keeps it inline, including composed code whose inputs were already rewritten. The access helpers that check the last entry and
call it stay inline, and so does a function of the same shape without the name. The pass is idempotent, and with xcrun the rewritten shader compiles. test_dmsh_raw_last.py runs the
translator's own lookups on the GPU in both forms. Final mesh code keeps helpers with subgroup inputs out of line (mesh_subgroup_noinline) and passes those inputs by value: Ocarina of
Time's scan helpers, run on the GPU, give each lane its own prefix sum in both forms."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from dmsh_chain import MAC_METAL   # noqa: E402  (the build's Mac compile)
from dmsh_rewrite import mesh_subgroup_noinline, raw_noinline   # noqa: E402

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct _29
{
    uint _m0[1];
};
'''
INLINE, NOINLINE = 'static inline __attribute__((always_inline))', 'static __attribute__((noinline))'
# The search, shortened: the chunk's hint, else one step of the binary search
FIND = f'''
{INLINE}
int dmsh_raw_find(thread const int& _lo, thread const int& _hi, const device _29& cp_s2)
{{
    uint _h = cp_s2._m0[((uint(_lo) >> uint(21)) & 8191u) * 8u + 6u];
    if ((_h >> 13u) == (uint(_lo) >> uint(21)))
    {{
        return int(_h & 8191u);
    }}
    return (cp_s2._m0[4096 * 8] <= uint(_lo)) ? 4096 : 0;
}}
'''
# An access helper: the last entry's range, else the search; then the access through the entry's delta
def helper(name, find):
    return f'''
{INLINE}
int {name}(thread const int& _lo, thread const int& _hi, const device _29& cp_s2, thread uint& _last)
{{
    int _e = int(_last);
    if (cp_s2._m0[_e * 8] > uint(_lo))
    {{
        _e = {find}(_lo, _hi, cp_s2);
        _last = uint(_e);
    }}
    int _hl = _lo + int(cp_s2._m0[_e * 8 + 4]);
    int _hh = _hi + int(cp_s2._m0[_e * 8 + 5]) + int(uint(_hl) < uint(_lo));
    return int(*(reinterpret_cast<const device uint*>(as_type<ulong>(uint2(uint(_hl), uint(_hh))))));
}}
'''
OTHER = FIND.replace('dmsh_raw_find', '_70')   # the same search under a name the translator did not give
MAIN = '''
kernel void main0(const device _29& cp_s2 [[buffer(0)]], device uint* out [[buffer(1)]], uint gid [[thread_position_in_grid]])
{
    uint _last = 0u;
    int a = int(gid) * 4, b = 0, c = a + 16;
    out[gid] = uint(_63(a, b, cp_s2, _last) + _63(c, b, cp_s2, _last) + _64(a, b, cp_s2, _last) + _71(c, b, cp_s2, _last));
}
'''
SRC = HEAD + FIND + OTHER + helper('_63', 'dmsh_raw_find') + helper('_64', 'dmsh_raw_find') + helper('_71', '_70') + MAIN

# Ocarina of Time's scan helpers (mesh shader a76e46990e6799ba), as SPIRV-Cross wrote them: _87 takes a lane's exclusive prefix sum over its SIMD group from five steps of _97, a
# shuffle within a segment of lanes; each passes gl_SubgroupInvocationID on by reference. Out of line with that reference, Apple's compiler gave every lane the same sum.
SCAN_HEAD = '''#include <metal_stdlib>
using namespace metal;

template<typename T>
inline T spvFindUMSB(T x)
{
    return select(clz(T(0)) - (clz(x) + T(1)), T(-1), x == T(0));
}

inline uint4 spvSubgroupBallot(bool value)
{
    simd_vote vote = simd_ballot(value);
    return uint4(as_type<uint2>((simd_vote::vote_t)vote), 0, 0);
}

template<typename T>
inline T spvSubgroupShuffle(T value, ushort lane)
{
    return simd_shuffle(value, lane);
}

'''
SCAN = SCAN_HEAD + f'''{INLINE}
int _97(thread const int& _31631, thread const int& _31632, thread const int& _31633, thread int& _31634, thread uint& gl_SubgroupInvocationID)
{{
    int _31637 = _31632 & 31;
    int _31639 = _31633 & 31;
    uint _31641 = uint(int(uint(_31633) >> uint(8)));
    int _31643 = int(_31641) & 31;
    float _31645 = as_type<float>(gl_SubgroupInvocationID);
    int _31647 = as_type<int>(_31645) & _31643;
    int _31649 = ~_31643;
    int _31651 = _31639 & _31649;
    int _31653 = _31651 | _31647;
    int _31655 = ~_31643;
    int _31657 = _31637 & _31655;
    int _31659 = _31657 | _31647;
    bool _31661 = uint(_31659) <= uint(_31653);
    _31634 = _31661 ? (-1) : 0;
    float _31663 = spvSubgroupShuffle(as_type<float>(_31631), uint(_31659));
    uint _31665 = uint(_31661 ? as_type<int>(_31663) : _31631);
    return int(_31665);
}}

{INLINE}
void _87(thread const int& _29608, thread const int& _29609, thread const int& _29610, thread const int& _29611, thread const int& _29612, thread const int& _29613, thread const int& _29614, thread const int& _29615, thread const int& _29616, thread int& _29617, thread int& _29618, thread int& _29619, thread int& _29620, thread int& _29621, thread int& _29622, thread int& _29623, thread int& _29624, thread int& _29625, thread int& _29626, thread int& _29627, thread uint& gl_SubgroupInvocationID, thread uint4& gl_SubgroupLtMask)
{{
    float _29630 = as_type<float>(gl_SubgroupLtMask.x);
    uint _29632 = spvSubgroupBallot(true).x;
    int _29634 = as_type<int>(_29630) & int(_29632);
    bool _29636 = _29634 != 0;
    int _29638 = as_type<int>(_29630) & int(_29632);
    int _29640 = int(spvFindUMSB(uint(_29634)));
    int _29642 = int(spvFindUMSB(uint(_29638)));
    int _29644 = _29614;
    int _29646 = _29640;
    int _29648 = 31;
    int _29650;
    int _29810 = _97(_29644, _29646, _29648, _29650, gl_SubgroupInvocationID);
    int _29652 = _29810;
    int _29654 = _29636 ? _29652 : 0;
    int _29656 = _29642;
    int _29658 = _29642;
    int _29660 = 30;
    int _29662;
    int _29816 = _97(_29656, _29658, _29660, _29662, gl_SubgroupInvocationID);
    int _29664 = _29816;
    int _29666 = _29654;
    int _29668 = _29642;
    int _29670 = 31;
    int _29672;
    int _29819 = _97(_29666, _29668, _29670, _29672, gl_SubgroupInvocationID);
    int _29674 = _29819;
    int _29676 = (_29662 != 0) ? _29664 : _29642;
    int _29678 = (_29662 != 0) ? _29674 : 0;
    int _29680 = _29676;
    int _29682 = _29676;
    int _29684 = 30;
    int _29686;
    int _29831 = _97(_29680, _29682, _29684, _29686, gl_SubgroupInvocationID);
    int _29688 = _29831;
    int _29690 = int(uint(_29654) + uint(_29678));
    int _29692 = _29690;
    int _29694 = _29676;
    int _29696 = 31;
    int _29698;
    int _29837 = _97(_29692, _29694, _29696, _29698, gl_SubgroupInvocationID);
    int _29700 = _29837;
    int _29702 = (_29686 != 0) ? _29688 : _29676;
    int _29704 = (_29686 != 0) ? _29700 : 0;
    int _29706 = _29702;
    int _29708 = _29702;
    int _29710 = 30;
    int _29712;
    int _29849 = _97(_29706, _29708, _29710, _29712, gl_SubgroupInvocationID);
    int _29714 = _29849;
    int _29716 = int(uint(_29690) + uint(_29704));
    int _29718 = _29716;
    int _29720 = _29702;
    int _29722 = 31;
    int _29724;
    int _29855 = _97(_29718, _29720, _29722, _29724, gl_SubgroupInvocationID);
    int _29726 = _29855;
    int _29728 = (_29712 != 0) ? _29714 : _29702;
    int _29730 = (_29712 != 0) ? _29726 : 0;
    int _29732 = _29728;
    int _29734 = _29728;
    int _29736 = 30;
    int _29738;
    int _29867 = _97(_29732, _29734, _29736, _29738, gl_SubgroupInvocationID);
    int _29740 = _29867;
    int _29742 = int(uint(_29716) + uint(_29730));
    int _29744 = _29742;
    int _29746 = _29728;
    int _29748 = 31;
    int _29750;
    int _29873 = _97(_29744, _29746, _29748, _29750, gl_SubgroupInvocationID);
    int _29752 = _29873;
    int _29754 = (_29738 != 0) ? _29740 : _29728;
    int _29756 = (_29738 != 0) ? _29752 : 0;
    int _29758 = _29754;
    int _29760 = _29754;
    int _29762 = 30;
    int _29764;
    int _29885 = _97(_29758, _29760, _29762, _29764, gl_SubgroupInvocationID);
    int _29766 = _29885;
    int _29768 = int(uint(_29742) + uint(_29756));
    int _29770 = _29768;
    int _29772 = _29754;
    int _29774 = 31;
    int _29776;
    int _29891 = _97(_29770, _29772, _29774, _29776, gl_SubgroupInvocationID);
    int _29778 = _29891;
    int _29780 = (_29764 != 0) ? _29778 : 0;
    int _29782 = int(uint(_29768) + uint(_29780));
    _29617 = _29782;
    _29618 = _29615;
    _29619 = 0;
    _29620 = _29716;
    _29621 = _29768;
    _29622 = _29754;
    _29623 = _29702;
    _29624 = _29742;
    _29625 = _29728;
    _29626 = _29764;
    _29627 = int(_29632);
}}


kernel void k(device int* out [[buffer(0)]], uint lane [[thread_index_in_simdgroup]], uint gid [[thread_position_in_grid]])
{{
    uint gl_SubgroupInvocationID = lane;
    uint4 gl_SubgroupLtMask = uint4(extract_bits(0xFFFFFFFFu, 0, lane), 0u, 0u, 0u);
    int a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = (lane & 1u) ? 5 : 3, a7 = 7, a8 = 0;
    int o[11];
    _87(a0, a1, a2, a3, a4, a5, a6, a7, a8, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], gl_SubgroupInvocationID, gl_SubgroupLtMask);
    out[gid] = o[0];
}}
'''
SCAN_RUNNER = '''#import <Metal/Metal.h>
#include <stdio.h>
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[1])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; if (!ps) { puts(e.description.UTF8String); return 2; }
    id<MTLBuffer> out = [d newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:out offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
    const int *got = out.contents; int bad = 0;
    for (int i = 0; i < 64; i++) { int lane = i % 32, want = 4 * lane - (lane & 1); if (got[i] != want) bad++; }   // lanes hold 3, 5, 3, 5, ...: the exclusive sum before lane L
    printf("bad %d of 64; lanes 0-7:", bad); for (int i = 0; i < 8; i++) printf(" %d", got[i]); puts(""); return 0;
} }
'''


def attribute(src, name):
    return re.search(r'\n([^\n]*)\nint %s\(' % name, src).group(1)


class RawNoinline(unittest.TestCase):
    def test_mesh_keeps_the_search_inline(self):
        for source in (SRC, raw_noinline(SRC)):
            with self.subTest(already_noinline=source != SRC):
                out = raw_noinline(source, stage='ms')
                self.assertEqual(out, SRC)
                self.assertEqual(raw_noinline(out, stage='ms'), out)

    def test_other_stages_keep_one_search(self):
        for stage in ('ts', 'vs', 'ps', 'cs', 'rt'):
            with self.subTest(stage=stage):
                self.assertEqual(raw_noinline(SRC, stage=stage), raw_noinline(SRC))

    def test_marks(self):
        out = raw_noinline(SRC)
        self.assertEqual(attribute(out, 'dmsh_raw_find'), NOINLINE)
        for name in ('_63', '_64', '_71'):
            self.assertEqual(attribute(out, name), INLINE, name)   # the last entry's check and the access stay at each call
        self.assertEqual(attribute(out, '_70'), INLINE, 'not named dmsh_raw_find')
        self.assertEqual(out.replace(NOINLINE, INLINE), SRC)

    def test_unchanged(self):
        plain = HEAD + OTHER + helper('_71', '_70') + MAIN
        self.assertEqual(raw_noinline(plain), plain)
        self.assertEqual(raw_noinline(raw_noinline(SRC)), raw_noinline(SRC))

    def test_mesh_subgroup_noinline(self):
        one_line = '\nstatic inline __attribute__((always_inline)) float dm_zsnap(float z) { return z; }\n'
        body = '\nstatic inline __attribute__((always_inline)) void dm_stage_body(uint gl_SubgroupInvocationID) {}\n'
        subgroup = SRC.replace('int _71(thread const int&', 'int _71(thread uint& gl_SubgroupInvocationID, thread const int&')
        src = raw_noinline(subgroup, stage='ms') + one_line + body
        out = mesh_subgroup_noinline(src)
        self.assertEqual(attribute(out, 'dmsh_raw_find'), INLINE)
        self.assertEqual(attribute(out, '_71'), NOINLINE)
        for name in ('_63', '_64', '_70'):
            self.assertEqual(attribute(out, name), INLINE, name)
        self.assertIn(one_line, out)
        self.assertIn(body, out)
        self.assertEqual(mesh_subgroup_noinline(out), out)
        self.assertIn('int _71(uint gl_SubgroupInvocationID, thread const int&', out)   # the builtin by value
        self.assertEqual(out.replace(NOINLINE, INLINE).replace('_71(uint gl_Subgroup', '_71(thread uint& gl_Subgroup'), src)

    def test_subgroup_inputs_by_value(self):
        out = mesh_subgroup_noinline(SCAN)
        self.assertNotIn('thread uint& gl_Subgroup', out)
        self.assertNotIn('thread uint4& gl_Subgroup', out)
        self.assertIn(', uint gl_SubgroupInvocationID, uint4 gl_SubgroupLtMask)', out)
        self.assertEqual(out.count(NOINLINE), 2)

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_scan_on_gpu(self):
        with tempfile.TemporaryDirectory() as w:
            w = Path(w)
            (w / 'r.m').write_text(SCAN_RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            for form, src in (('inline', SCAN), ('noinline', mesh_subgroup_noinline(SCAN))):   # as spirv-cross writes it, and as a final mesh library takes it
                (w / 'k.metal').write_text(src)
                r = subprocess.run(MAC_METAL + [str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
                if 'unable to find utility' in r.stderr: self.skipTest('no Metal toolchain')
                self.assertEqual(r.returncode, 0, r.stderr[-3000:])
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True)
                out = subprocess.run([str(w / 'r'), str(w / 'k.metallib')], capture_output=True, text=True)
                self.assertIn('bad 0 of 64', out.stdout, form + ': ' + out.stdout + out.stderr)

    @unittest.skipUnless(shutil.which('xcrun'), 'no xcrun')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'raw.metal'; p.write_text(raw_noinline(SRC))
            r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(p), '-o', str(Path(d) / 'raw.air')], capture_output=True, text=True)
            if 'unable to find utility' in r.stderr: self.skipTest('no Metal toolchain')
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
