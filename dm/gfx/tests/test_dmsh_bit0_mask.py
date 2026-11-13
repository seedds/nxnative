#!/usr/bin/env python3
"""dmsh_rewrite.py bit0_mask: the 0 / -1 mask of a tested bit 0 is written from the bit (`-(x & 1)`), not from its compare (`b ? -1 : 0`), so the compare keeps one user,
its branch. Apple's GPU compiler for applegpu_g15 and later otherwise can pick a jump it cannot encode (`LLVM ERROR: Failed to encode.`), which ends the compiler service when
a pipeline with the shader is built: PRAGMATA's pixel shader c4ac3ca598cea32e. With DMSH_TEST_METAL=1 the reduced shader below goes through the Metal compiler and the
offline GPU compiler (applegpu-nt, the chain's flags): as translated it must fail to encode (skipped where a later toolchain compiles it), rewritten it must compile."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain
import dmsh_rewrite
bit0_mask = dmsh_rewrite.bit0_mask
MARK = '// bit 0 as a mask (bit0_mask)'


def reduced(fill=100):
    """c4ac3ca598cea32e reduced, in SPIRV-Cross's layout: `fill` uniform reads (they take the slots of the compiler's per-draw constant program), a discard and an early
    return on a per-pixel value, then bit 0 of a uniform word read at the top: its compare _32 feeds the predicate's mask (_40) and a branch around a uniform read"""
    q = '(*spvDescriptorSet0.fp_c3)._m0'
    body = ['    main0_out out = {};', '    float _20 = in.m_1.x;', '    int _21 = as_type<int>((*spvDescriptorSet0.fp_c10)._m0[2].w);',
            '    int _22 = _21 & 2097152;', '    _20 += float(_22);']
    body += [f'    _20 = fma(_20, in.m_1.y, {q}[{1000 + i}].x);' for i in range(fill)]
    body += [f'    bool _23 = {q}[830].x > 0.5;', f'    bool _24 = {q}[228].x > 0.5;', f'    int _40 = int({q}[3792].x);', f'    float _25 = {q}[818].x;',
             f'    bool _26 = {q}[836].x > 0.5;', '    bool _27 = isnan(_20);', '    bool _28 = _23 || _27;', '    bool _29 = _28 || _26;',
             '    if (!_29)', '    {', '        discard_fragment();', '    }', '    if (!_29)', '    {', '        return out;', '    }',
             '    int _31 = _21 & 1;', '    bool _32 = _31 != 0;',
             '    if (!_24)', '    {', '        _40 = _32 ? (-1) : 0;', '    }',
             '    int _33 = _40;', '    int _34 = ~_33;', '    float _35 = (_34 != 0) ? 0.0 : 1.0;', f'    float _36 = {q}[892].x;', '    float _41 = _35;',
             '    if (_32)', '    {', f'        _41 = {q}[2832].y;', '    }',
             '    float _37 = _41;', '    float _38 = fma(_36, _37, _25);', f'    out.m_2.x = _38 * {q}[3335].x;', '    return out;']
    return '\n'.join(['#include <metal_stdlib>', 'using namespace metal;', '', 'struct _16', '{', '    float4 _m0[4096];', '};', '',
                      'struct spvDescriptorSetBuffer0', '{', '    constant _16* fp_c10 [[id(5)]];', '    constant _16* fp_c3 [[id(7)]];', '};', '',
                      'struct main0_out', '{', '    float4 m_2 [[color(0)]];', '};', '', 'struct main0_in', '{', '    float4 m_1 [[user(locn0)]];', '};', '',
                      'fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])', '{'] + body + ['}', ''])


VERTEX = ('struct dm_vo { float4 p [[position]]; float4 m_1 [[user(locn0)]]; };\n'
          'vertex dm_vo dm_vs(uint vid [[vertex_id]]) { dm_vo o = {}; o.p = float4(float(vid), 0.0, 0.0, 1.0); return o; }\n')


def mac_flags(): return list(dmsh_chain.MAC_METAL[5:])   # the build's Mac compile, after `xcrun -sdk macosx metal -c`


def toolchain():
    """the directory of the Metal toolchain's tools, when it has the offline GPU compiler"""
    if not shutil.which('xcrun'): return None
    r = subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True, text=True)
    d = Path(r.stdout.strip()).parent if r.returncode == 0 else None
    return d if d and (d / 'applegpu-nt').exists() else None


def gpu_compile(msl, flags, arch='applegpu_g16s'):
    """(ok, output) of the MSL as a render pipeline's fragment function through metal, metallib and applegpu-nt, as the compiler service builds it"""
    sdk = subprocess.run(['xcrun', '-sdk', 'macosx', '--show-sdk-path'], capture_output=True, text=True).stdout.strip(); env = dict(os.environ, SDKROOT=sdk)
    with tempfile.TemporaryDirectory() as d:
        Path(d, 'k.metal').write_text(msl + VERTEX)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-w', *flags, 'k.metal', '-o', 'k.air'], capture_output=True, text=True, cwd=d, env=env)
        if r.returncode: raise AssertionError('metal: ' + r.stderr)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', 'k.air', '-o', 'k.metallib'], capture_output=True, text=True, cwd=d, env=env)
        if r.returncode: raise AssertionError('metallib: ' + r.stderr)
        Path(d, 'p.mtlp-json').write_text(json.dumps({'libraries': {'paths': [{'path': 'k.metallib', 'label': 'L'}]}, 'pipelines': {'render_pipelines': [
            {'vertex_function': 'alias:L#dm_vs', 'fragment_function': 'alias:L#main0', 'color_attachments': [{'pixel_format': 'RGBA32Float'}]}]}}))
        r = subprocess.run([str(toolchain() / 'applegpu-nt'), '-arch', arch, '-platform_version', 'macos', '26.0', '26.0', '-N', 'p.mtlp-json', 'k.metallib', '-o', 'k.bin'],
                           capture_output=True, text=True, cwd=d, env=env)
        return r.returncode == 0, r.stdout + r.stderr


class Bit0Mask(unittest.TestCase):
    def test_the_mask_is_taken_from_the_bit(self):
        src = reduced(4); out = bit0_mask(src)
        self.assertNotIn('_40 = _32 ? (-1) : 0;', out)
        self.assertIn('        _40 = -_31;   ' + MARK, out)
        self.assertIn('    bool _32 = _31 != 0;', out)   # the compare stays for its branch
        self.assertIn('    if (_32)', out)
        self.assertEqual(out.replace('-_31;   ' + MARK, '_32 ? (-1) : 0;'), src)   # nothing else changed

    def test_an_unsigned_bit_is_converted(self):
        src = reduced(4).replace('    int _31 = _21 & 1;', '    uint _31 = uint(_21) & 1u;').replace('_31 != 0;', '_31 != 0u;')
        self.assertIn('        _40 = -int(_31);   ' + MARK, bit0_mask(src))

    def test_a_parenthesized_operand_is_bit_0(self):
        for e in ('(_21 | _22) & 1', '(_24 ? _21 : _22) & 1', 'int(_21 ^ 3) & 1', '_21 >> 4 & 1'):
            src = reduced(4).replace('    int _31 = _21 & 1;', '    int _31 = %s;' % e)
            self.assertIn('        _40 = -_31;   ' + MARK, bit0_mask(src), e)

    def test_a_declared_mask_is_rewritten_too(self):
        src = reduced(4).replace('    int _33 = _40;', '    int _39 = _32 ? (-1) : 0;\n    int _33 = _40 + _39;')
        self.assertIn('    int _39 = -_31;   ' + MARK, bit0_mask(src))

    def test_other_tests_keep_their_select(self):
        base = reduced(4)
        for old, new in (('    int _31 = _21 & 1;', '    int _31 = _21 & 2;'),          # another bit: -(x & 2) is not its mask
                         ('    int _31 = _21 & 1;', '    int _31 = _21 & 3;'),
                         ('    bool _32 = _31 != 0;', '    bool _32 = _31 == 0;'),     # the inverse test
                         ('    bool _32 = _31 != 0;', '    bool _32 = _31 != 1;'),
                         ('    int _31 = _21 & 1;', '    int _31 = _21 | 1;'),
                         ('    int _31 = _21 & 1;', '    int _31 = _21 | _22 & 1;'),    # | (b & 1): not bit 0 alone
                         ('    int _31 = _21 & 1;', '    int _31 = _24 ? _21 : _22 & 1;'),
                         ('_40 = _32 ? (-1) : 0;', '_40 = _32 ? 1 : 0;'),
                         ('_40 = _32 ? (-1) : 0;', '_40 = _32 ? 0 : (-1);')):
            src = base.replace(old, new); self.assertNotEqual(src, base)
            self.assertIs(bit0_mask(src), src, new)

    def test_a_temporary_written_again_is_left(self):
        base = reduced(4)
        for extra in ('    _31 = 0;', '    _31 |= 4;', '    _32 = false;', '    _31++;'):
            src = base.replace('    if (!_24)', extra + '\n    if (!_24)')
            self.assertIs(bit0_mask(src), src, extra)

    def test_each_function_is_read_on_its_own(self):
        # the build's extra entry points repeat a body, with the same temporaries: both copies are rewritten
        src = reduced(4); twice = src + src.replace('main0_out main0(', 'main0_out main0_u(').split('struct main0_in\n{\n    float4 m_1 [[user(locn0)]];\n};\n\n')[1]
        self.assertEqual(twice.count('_40 = _32 ? (-1) : 0;'), 2)
        self.assertEqual(bit0_mask(twice).count('_40 = -_31;'), 2)
        # a bit of one function and a compare of another are no pair
        other = src + '\nstatic inline int dm_f(int _51)\n{\n    bool _32 = _51 != 0;\n    int _52 = _32 ? (-1) : 0;\n    return _52;\n}\n'
        self.assertIn('    int _52 = _32 ? (-1) : 0;', bit0_mask(other))

    def test_a_shader_without_the_idiom_is_the_same_object(self):
        for src in ('', reduced(4).replace('_32 ? (-1) : 0', '0'), '#include <metal_stdlib>\nkernel void main0()\n{\n    int _5 = 3 & 1;\n}\n'):
            self.assertIs(bit0_mask(src), src)

    def test_the_build_runs_it(self):
        build = (TOOLS / 'dmsh_build.py').read_text()
        self.assertEqual(build.count('src = dmsh_rewrite.bit0_mask(src)'), 1)
        self.assertLess(build.index('src = dmsh_rewrite.bit0_mask(src)'), build.index('src = dmsh_rewrite.border_emu(src)'))   # before the entry points are added
        self.assertEqual(build.count('dmsh_rewrite.bit0_mask(dmsh_rewrite.precise('), 2)   # a geometry shader's kernel and its reader (geometry_finish)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and toolchain(), 'set DMSH_TEST_METAL=1 to compile with the Metal compiler and applegpu-nt')
    def test_the_gpu_compiler_encodes_the_rewritten_shader(self):
        src = reduced()
        chain = mac_flags(); self.assertEqual(chain[0], '-O1')
        ok, log = gpu_compile(src, chain)
        if ok: self.skipTest('this toolchain compiles the shader as translated')
        self.assertIn('Failed to encode', log)
        for flags in (chain, ['-O2', *chain[1:]]):
            for arch in ('applegpu_g15g', 'applegpu_g16s', 'applegpu_g17g'):
                ok, log = gpu_compile(bit0_mask(src), flags, arch)
                self.assertTrue(ok, f'{flags} {arch}: {log}')


if __name__ == '__main__':
    unittest.main()
