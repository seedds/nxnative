"""A non-finite colour under a zero blend weight returned as 0 (dmsh_rewrite.py blend_zero), as NVIDIA's blender takes it with "zero times anything is zero" on: main0 calls the guard
before every return and takes the host's word as its last parameter, every entry point the build makes afterwards (the shared body's callers, the side library's, main0_f) carries both,
what dmsh_entry.fs_uint reads of a shader is unchanged, the rule itself, and each text compiled by Metal's compiler when it is there. The same guard rounds a colour going into a
normalized target as NVIDIA's colour unit converts it (dm_rop; the GPU side in t_rop_round.m)."""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi   # noqa: E402
import dmsh_chain   # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'ios/tools'))
import ios_metal  # noqa: E402  (the iPhone's deployment target and math mode)
import dmsh_entry   # noqa: E402
import dmsh_fetch   # noqa: E402
import dmsh_rewrite   # noqa: E402
import dmsh_side   # noqa: E402
blend_zero = dmsh_rewrite.blend_zero

HEAD = '#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n'
# a light shader's shape as spirv-cross writes it: two colour outputs, an early return that writes zeros, a weighted average divided by its weight sum
PLAIN = HEAD + '''struct main0_out
{
    float4 m_79 [[color(0)]];
    float4 m_81 [[color(2)]];
};

struct main0_in
{
    float4 m_83 [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float _631 = in.m_83.x;
    float _633 = in.m_83.y;
    if (_633 < 0.0)
    {
        out.m_79 = float4(0.0);
        out.m_81 = float4(0.0);
        return out;
    }
    float _727 = 1.0 / _633;
    float _731 = _727 * _631;
    out.m_79 = float4(_731, _731, _731, in.m_83.z);
    out.m_81 = float4(_731, 0.0, 0.0, in.m_83.z);
    return out;
}
'''
INT_BITS = PLAIN.replace('out.m_81 = float4(_731, 0.0, 0.0, in.m_83.z);', 'out.m_81 = as_type<float4>(uint4(7u));')   # an output written from integer bits: fs_uint gives it main0_u
PARAM = dmsh_rewrite.BLEND_ZERO_PARAM
CALL = 'dm_blend0(out, dm_blend);'


def fn_text(msl, name):   # an entry point's whole text found by its name alone (entries() refuses a unit's _l, whose parameters take function constants)
    m = re.search(r'fragment\s+\w+\s+%s\s*\(' % name, msl)
    if not m: return None
    b = msl.find('{', dmsh_side._close(msl, m.end(), '(', ')'))
    return msl[m.start():dmsh_side._close(msl, b + 1, '{', '}')]


def built(src):   # the build's order (dmsh_build.py finish): blend_zero, then the integer entry points on the shared body
    out, n = blend_zero(src)
    fu = dmsh_entry.fs_uint(out)
    return (dmsh_side.shared_body(out) + fu if fu else out), n, bool(fu)


def bz(c, m):   # dm_bz on the CPU: the colour a zero weight leaves, m the output's bits of the word
    r, g, b, a = c
    if not ((m == 1 and a == 0.0) or (m == 2 and a == 1.0)): return c
    bits = lambda v: struct.unpack('<I', struct.pack('<f', v))[0]
    return tuple(0.0 if bits(v) & 0x7f800000 == 0x7f800000 else v for v in (r, g, b)) + (a,)


SCALES = {dmsh_abi.ROUND_8: (255,) * 4, dmsh_abi.ROUND_16: (65535,) * 4, dmsh_abi.ROUND_10_2: (1023, 1023, 1023, 3)}


def f32(v): return struct.unpack('<f', struct.pack('<f', v))[0]


def nvidia_unorm(x, s):   # NVIDIA's colour unit: the float32 product rounded to nearest even (NaN stored as 0)
    if x != x: return 0
    return round(f32(min(max(x, 0.0), 1.0) * s))   # Python's round is to nearest even; a float32 operand times an integer below 2^17 is exact in a double, then rounded to float32


def apple_unorm(x, s):   # Apple's: the exact product rounded to nearest (t_rop_round.m checks it on the GPU)
    if x != x: return 0
    return round(min(max(x, 0.0), 1.0) * s)


def rop(x, s):   # dm_rop on the CPU: what the shader returns, a float32 that Apple's conversion stores as nvidia_unorm
    return f32(nvidia_unorm(x, s) / s)


class BlendZero(unittest.TestCase):
    def test_every_return_of_main0(self):
        out, n = blend_zero(PLAIN)
        self.assertEqual(n, 2)
        e = dmsh_side.entry(out, 'main0'); b = out[e['head']:e['end']]
        self.assertEqual(b.count(CALL), 2)
        self.assertEqual(len(re.findall(r'\breturn\b', b)), 2)
        self.assertTrue(re.search(r'\n    dm_blend0\(out, dm_blend\);\n    return out;\n\}$', b))   # the last line is still the return other stages look for
        self.assertEqual(e['params'][-1], PARAM)
        self.assertEqual(e['args'].split(', ')[-1], 'dm_blend')
        self.assertLess(out.index('struct main0_out'), out.index('void dm_blend0(thread main0_out& dm_o'))   # the helper takes main0's local output: after its struct, before main0
        self.assertLess(out.index('void dm_blend0('), out.index('fragment main0_out main0('))

    def test_each_float4_colour_by_its_index(self):
        out, _ = blend_zero(PLAIN)
        bits = dmsh_abi.BLEND_ZERO_BITS
        rb = dmsh_abi.BLEND_ROUND_BITS
        self.assertIn('dm_o.m_79 = dm_rop(dm_bz(dm_o.m_79, m >> 0u), r >> 0u);', out)
        self.assertIn(f'dm_o.m_81 = dm_rop(dm_bz(dm_o.m_81, m >> {2 * bits}u), r >> {2 * rb}u);', out)   # colour 2's bits, not the second output's
        mixed = PLAIN.replace('float4 m_81 [[color(2)]];', 'uint4 m_81 [[color(2)]];')
        out, n = blend_zero(mixed)
        self.assertEqual(n, 2); self.assertNotIn('dm_o.m_81', out)
        dual = PLAIN.replace('[[color(2)]]', '[[color(0), index(1)]]')
        self.assertNotIn('dm_o.m_81', blend_zero(dual)[0])   # a second blend source is a weight, not a colour that is stored

    def test_what_fs_uint_reads_is_unchanged(self):
        # fs_uint tells an integer writer by its stores to out: the guard is a call, so a shader gets the same entry points with it as without
        for src in (PLAIN, INT_BITS):
            out, _ = blend_zero(src)
            self.assertEqual(bool(dmsh_entry.fs_uint(out)), bool(dmsh_entry.fs_uint(src)))
        self.assertTrue(dmsh_entry.fs_uint(INT_BITS))

    def test_every_entry_point_carries_the_guard_and_the_word(self):
        for src in (PLAIN, INT_BITS):
            text, n, shared = built(src)
            self.assertEqual(shared, src is INT_BITS)
            u, info = dmsh_side.unit(text)
            self.assertIsNotNone(u, info)
            f, meta = dmsh_fetch.fold(u, [], om_only_ok=True)
            self.assertIsNotNone(f, meta)
            b = dmsh_side.body(u)
            self.assertEqual(u[b[0]:b[2]].count(CALL), 2)   # in the body, once for each return: every entry point calls the body
            self.assertIn('constant dm_blend_state& dm_blend', u[b[0]:b[1]])
            names = ['main0'] + info['alpha'] + info['logic'] + (['main0_u'] if shared else [])
            self.assertIn('main0_a', names); self.assertIn('main0_l', names)
            for name in names:
                t = fn_text(u, name)
                self.assertIn(PARAM, t, name)
                self.assertRegex(t, r'dm_body_main0\w*\([^;]*\bdm_blend\)', name)   # main0_u's own versions call main0_u's body, which calls main0's
                self.assertNotIn('dm_blend0(', t, name)   # not twice
            t = fn_text(f, 'main0_f')
            self.assertIn(PARAM, t); self.assertRegex(t, r'dm_body_main0\([^;]*\bdm_blend\)')

    def test_not_applied(self):
        ints = PLAIN.replace('float4 m_79 [[color(0)]];', 'uint4 m_79 [[color(0)]];').replace('float4 m_81 [[color(2)]];', 'uint4 m_81 [[color(2)]];')
        self.assertEqual(blend_zero(ints), (ints, 0))
        kernel = HEAD + 'kernel void main0(uint i [[thread_position_in_grid]]) {}\n'
        self.assertEqual(blend_zero(kernel), (kernel, 0))
        once, _ = blend_zero(PLAIN)
        self.assertEqual(blend_zero(once), (once, 0))   # a second pass
        shared = dmsh_side.shared_body(PLAIN)
        self.assertEqual(blend_zero(shared), (shared, 0))   # the build runs it before the body is shared: after, main0's parameter would not reach the body
        inline = PLAIN.replace('        return out;\n    }', '        { return out; }\n    }')
        self.assertEqual(blend_zero(inline), (inline, 0))   # all or nothing: an unguarded return would let the NaN through on one path

    def test_rule(self):
        nan, inf = float('nan'), float('inf')
        self.assertEqual(bz((nan, inf, 0.5, 0.0), 1), (0.0, 0.0, 0.5, 0.0))   # SRC_ALPHA at alpha 0
        self.assertEqual(bz((-inf, 2.0, nan, 1.0), 2), (0.0, 2.0, 0.0, 1.0))   # ONE_MINUS_SRC_ALPHA at alpha 1
        for m, a in ((0, 0.0), (1, 0.5), (1, 1.0), (2, 0.0), (3, 0.0), (3, 1.0)):   # no zero weight: NVIDIA's blender keeps the NaN too
            r = bz((nan, 1.0, inf, a), m)
            self.assertTrue(r[0] != r[0]); self.assertEqual(r[1:], (1.0, inf, a))
        for m in range(4): self.assertEqual(bz((0.25, -3.0, 3.0e38, 0.0), m), (0.25, -3.0, 3.0e38, 0.0))   # finite: never changed
        h = dmsh_rewrite.BLEND_ZERO
        self.assertIn('if (!((m == 1u && c.w == 0.0f) || (m == 2u && c.w == 1.0f))) return c;', h)
        self.assertIn('(as_type<uint3>(c.xyz) & uint3(0x7f800000u)) == uint3(0x7f800000u)', h)
        self.assertIn('if ((m | r) == 0u) return;', h)   # a draw with no such weight and no normalized target pays two loads and one test
        self.assertIn('struct dm_blend_state { uint zero; uint round; };', h)   # the host's two words (nvn_mtl.m bz_bind)
        self.assertEqual(PARAM, f'constant dm_blend_state& dm_blend [[buffer({dmsh_abi.BLEND_BUFFER})]]')
        self.assertNotIn(dmsh_abi.BLEND_BUFFER, (dmsh_abi.ALPHA_BUFFER, dmsh_abi.LOGIC_BUFFER, 0))

    def test_rounding_rule(self):
        flags = f32(66.5 / 255)   # Sonic Frontiers' G-buffer flags
        self.assertEqual(flags, 0.260784327983856201171875)
        self.assertEqual((nvidia_unorm(flags, 255), apple_unorm(flags, 255)), (66, 67))
        differ = 0
        for cls, sc in SCALES.items():
            for s in set(sc):
                for k in range(0, s, max(1, s // 300)):
                    for x in (f32((k + 0.5) / s), f32(k / s), f32((k + 0.25) / s)):
                        y = rop(x, s)
                        self.assertEqual(apple_unorm(y, s), nvidia_unorm(x, s), (cls, s, k, x))   # what the shader returns is stored as NVIDIA stores x
                        self.assertEqual(rop(y, s), y)   # idempotent
                        differ += apple_unorm(x, s) != nvidia_unorm(x, s)
        self.assertGreater(differ, 100)   # the half steps are where the two differ
        for s in (255, 65535, 1023, 3):
            self.assertEqual([nvidia_unorm(x, s) for x in (float('nan'), -1.0, 0.0, 1.0, 2.0, float('inf'))], [0, 0, 0, s, s, s])
        h = dmsh_rewrite.BLEND_ZERO
        self.assertIn('rint(precise::clamp(c, 0.0f, 1.0f) * s) / s', h)
        self.assertEqual(sorted((dmsh_abi.ROUND_8, dmsh_abi.ROUND_16, dmsh_abi.ROUND_10_2)), [1, 2, 3])
        self.assertLessEqual(3, (1 << dmsh_abi.BLEND_ROUND_BITS) - 1)

    @unittest.skipUnless(shutil.which('xcrun'), 'no Metal compiler')
    def test_compiles(self):
        texts = []
        for name, src in (('plain', PLAIN), ('int_bits', INT_BITS)):
            text, n, _ = built(src)
            self.assertGreater(n, 0)
            u, info = dmsh_side.unit(text)
            f, meta = dmsh_fetch.fold(u, [], om_only_ok=True)
            texts += [(name, text), (name + '_unit', u), (name + '_fold', f)]   # each text a library is compiled from
        for sdk, flags in (('macosx', (dmsh_chain.MAC_MIN_FLAG,)), ('iphoneos', tuple(ios_metal.IOS_FLAGS))):
            for name, out in texts:
                with self.subTest(sdk=sdk, variant=name), tempfile.TemporaryDirectory() as d:
                    p = Path(d) / f'{name}.metal'; p.write_text(out)
                    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-O1', '-fpreserve-invariance', '-fwrapv', *flags, str(p), '-o', str(Path(d) / f'{name}.air')],
                                       capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, name + ': ' + r.stderr[:500])


# t_blend_zero.m's shader: NaN, +Inf, -Inf and 2 by column, at alpha 0 (columns 0-3) and alpha 1 (4-7), with the guard
REPLAY = HEAD + '''struct main0_out
{
    float4 m_0 [[color(0)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    uint x = uint(gl_FragCoord.x);
    uint k = x & 3u;
    float v = as_type<float>(k == 0u ? 0x7fc00000u : (k == 1u ? 0x7f800000u : (k == 2u ? 0xff800000u : 0x40000000u)));
    out.m_0 = float4(v, v, v, x < 4u ? 0.0 : 1.0);
    return out;
}
'''
VERTEX = '\nvertex float4 vertex_main(uint i [[vertex_id]]) { float2 v[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(v[i], 0, 1); }\n'


def fixture(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    src, n = blend_zero(REPLAY)
    if n != 1: raise SystemExit('the replay shader took no guard')
    (out / 'blend.metal').write_text(src + VERTEX)



def rop_values():   # t_rop_round.m's colours by column: half steps of each width (where NVIDIA and Apple differ), then the edges
    v = [f32((round(i * 254 / 99) + 0.5) / 255) for i in range(100)]
    v += [f32((round(i * 65534 / 79) + 0.5) / 65535) for i in range(80)]
    v += [f32((round(i * 1022 / 59) + 0.5) / 1023) for i in range(60)]
    v += [f32((k + 0.5) / 3) for k in range(3)]
    v += [0.0, 1.0, -0.5, 1.5, float('nan'), float('inf'), -float('inf'), 0.5, f32(1 / 3), f32(66 / 255), f32(67 / 255), f32(66.5 / 255), 1e-30]
    assert len(v) == 256
    return v


ROP_SHADER = HEAD + '''struct main0_out
{
    float4 m_0 [[color(0)]];
};

constant uint dm_vals[256] = {%s};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float v = as_type<float>(dm_vals[uint(gl_FragCoord.x) & 255u]);
    out.m_0 = float4(v, v, v, v);
    return out;
}
'''


def rop_fixture(out):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    bits = [struct.unpack('<I', struct.pack('<f', v))[0] for v in rop_values()]
    src, n = blend_zero(ROP_SHADER % ', '.join(f'0x{b:08x}u' for b in bits))
    if n != 1: raise SystemExit('the replay shader took no guard')
    (out / 'rop.metal').write_text(src + VERTEX)
    (out / 'rop.bin').write_bytes(struct.pack('<256I', *bits))

if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == '--fixture-rop': rop_fixture(sys.argv[2])
    else: unittest.main()
