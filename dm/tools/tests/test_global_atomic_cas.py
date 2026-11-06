"""Synthetic Maxwell global CAS -> SPIR-V -> Metal tests, without game inputs.

Set DMSH_TEST_XLATE to an offline translator, DMSH_TEST_METAL=1 to compile,
and DMSH_TEST_GPU=1 to execute on the Mac. Optional DMSH_TEST_METAL_SDKS
is macosx,iphoneos. The GPU checks the old word, replacement and sentinels.
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RYU = ROOT / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader'


def shader(compare=7, replacement=99, *, offset=0, advance=0, pred_inv=False, runtime_pred=False,
           extended=True, size64=False, modifiers=0, discard=False, source=6, dest=4):
    table = (RYU / 'Decoders/InstTable.cs').read_text()
    def base(name):
        match = re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', table)
        assert match is not None, name
        return int(match[1].replace('x', '0'), 2)
    def mov(r, n):
        return base('Mov32i') | ((n & 0xffffffff) << 20) | (7 << 16) | (15 << 12) | r
    def ldc(r, off):
        return base('Ldc') | (4 << 48) | (off << 20) | (7 << 16) | (255 << 8) | r
    ops = [ldc(2, 0x140), ldc(3, 0x144), mov(6, compare), mov(7, replacement), mov(4, 123)]
    if advance:
        ops.append(base('Iadd32i') | ((advance & 0xffffffff) << 20) | (7 << 16) | (2 << 8) | 2)
    if runtime_pred:
        # P0 = c0[0x150] == 1. This cannot fold during translation, unlike PT.
        ops += [ldc(10, 0x150), mov(11, 1),
                base('Isetp') | (2 << 49) | (7 << 39) | (11 << 20) | (7 << 16) | (10 << 8) | 7]
    # gm107.c: signed byte displacement [47:28], E[48], b64[49].
    # R6 compares, R7 replaces, R4 receives the old value; !PT skips both effects.
    ops.append(base('AtomCas') | (int(extended) << 48) | (int(size64) << 49) |
               (modifiers << 50) | ((offset & 0xfffff) << 28) | (source << 20) |
               ((0 if runtime_pred else 7) << 16) | (int(pred_inv) << 19) | (2 << 8) | (255 if discard else dest))
    # Store the returned value separately, through the unadvanced base address.
    ops += [ldc(8, 0x140), ldc(9, 0x144),
            base('Stg') | (4 << 48) | (1 << 45) | (32 << 20) | (7 << 16) | (8 << 8) | dest,
            base('Exit') | (7 << 16) | 15]
    words = []
    for i, op in enumerate(ops):
        if i % 3 == 0: words.append(0)
        words.append(op)
    return struct.pack('<%dQ' % len(words), *words)


def opcodes(path):
    b = path.read_bytes(); w = struct.unpack('<%dI' % (len(b) // 4), b)
    i = 5; result = []
    while i < len(w):
        result.append(w[i] & 65535); i += w[i] >> 16
    return result


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'offline translator not configured')
class Compiler(unittest.TestCase):
    def translate(self, work, **kwargs):
        code = work / 'cas.bin'; code.write_bytes(shader(**kwargs))
        jobs = work / 'jobs.json'
        jobs.write_text(json.dumps([dict(key='cas', stage='cs', code=str(code), local=[1, 1, 1], debug=True)]))
        return subprocess.run([os.environ['DMSH_TEST_XLATE'], str(jobs), str(work)], capture_output=True, text=True)

    def test_global_cas_old_value_and_memory(self):
        # Missing CAS, reversed operands, ignored displacement/predicate, or lost
        # return value each changes one of these literal expected GPU results.
        cases = [({}, 0, 99, 7), ({'compare': 8}, 0, 7, 7),
                 ({'offset': 4, 'compare': 9}, 1, 99, 9),
                 ({'offset': -4, 'advance': 8, 'compare': 9}, 1, 99, 9),
                 ({'pred_inv': True}, 0, 7, 123),
                 ({'runtime_pred': True}, 0, 99, 7),
                 ({'runtime_pred': True, 'pred_inv': True}, 0, 7, 123),
                 ({'extended': False}, 0, 99, 7),
                 ({'dest': 6}, 0, 99, 7), ({'dest': 7}, 0, 99, 7),
                 ({'discard': True}, 0, 99, 123)]
        for kwargs, index, value, old in cases:
            with self.subTest(**kwargs), tempfile.TemporaryDirectory(prefix='dm-global-cas-') as tmp:
                work = Path(tmp); result = self.translate(work, **kwargs)
                self.assertEqual(result.returncode, 0, result.stderr)
                spv = work / 'cas.spv'
                if not kwargs.get('pred_inv') or kwargs.get('runtime_pred'):
                    self.assertIn(230, opcodes(spv), 'ATOM.CAS must emit OpAtomicCompareExchange, not a silent no-op')
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(spv)], check=True)
                msl = work / 'cas.metal'
                subprocess.run([str(ROOT / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'), '--msl', '--msl-version',
                                '20300', '--msl-argument-buffers', str(spv), '--output', str(msl)], check=True)
                if os.environ.get('DMSH_TEST_METAL') == '1':
                    self.metal(work, msl, index, value, old)

    def metal(self, work, msl, index, value, old):
        for sdk in os.environ.get('DMSH_TEST_METAL_SDKS', 'macosx').split(','):
            air = work / (sdk + '.air'); lib = work / (sdk + '.metallib')
            flags = ['-mios-version-min=18.0', '-fmetal-math-mode=relaxed'] if sdk == 'iphoneos' else []
            subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-O1', '-fpreserve-invariance', *flags,
                            str(msl), '-o', str(air)], check=True)
            subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)], check=True)
            if sdk == 'macosx' and os.environ.get('DMSH_TEST_GPU') == '1':
                exe = work / 'cas-gpu'
                subprocess.run(['clang', '-fobjc-arc', '-O1', '-Wall', '-Werror',
                                str(ROOT / 'dm/gfx/tests/t_global_atomic_cas.m'), '-framework', 'Foundation',
                                '-framework', 'Metal', '-o', str(exe)], check=True)
                src = msl.read_text()
                ids = [re.search(r'\b' + name + r' \[\[id\((\d+)\)\]\]', src) for name in ('cp_c0', 'cp_s0')]
                c0, s0 = ids
                assert s0 is not None, src
                subprocess.run([str(exe), str(lib), str(int(c0[1]) if c0 else -1), s0[1],
                                str(index), str(value), str(old)], check=True,
                               env=dict(os.environ, MTL_DEBUG_LAYER='1'))

    def test_unverified_forms_fail_translation(self):
        for kwargs in ({'size64': True}, {'modifiers': 1}, {'modifiers': 2}, {'modifiers': 3},
                       {'source': 255}, {'source': 5}):
            with self.subTest(**kwargs), tempfile.TemporaryDirectory(prefix='dm-global-cas-reject-') as tmp:
                result = self.translate(Path(tmp), **kwargs)
                self.assertNotEqual(result.returncode, 0, 'unsupported CAS must not silently succeed')
                self.assertIn('NotSupportedException', result.stderr)

    def test_signed_extended_address_expression(self):
        # Storage lowering can cancel a known buffer base and hide an erroneous
        # high word. Evaluate the actual address-emitter IR before that lowering.
        with tempfile.TemporaryDirectory(prefix='dm-cas-address-') as tmp:
            work = Path(tmp)
            (work / 'address.cs').write_text((ROOT / 'dm/tools/tests/fixtures/atomic_address.cs').read_text())
            (work / 'address.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>'
                '<OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework>'
                '</PropertyGroup></Project>')
            subprocess.run(['dotnet', 'build', str(work / 'address.csproj'), '-o', str(work / 'out'),
                            '--nologo', '-v:q'], check=True, capture_output=True, text=True)
            assembly = Path(os.environ['DMSH_TEST_XLATE']).parent / 'Ryujinx.Graphics.Shader.dll'
            result = subprocess.run(['dotnet', str(work / 'out/address.dll'), str(assembly)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__': unittest.main()
