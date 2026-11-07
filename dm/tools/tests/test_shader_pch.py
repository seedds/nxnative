"""The shader libraries' precompiled Metal header (dm/gfx/tools/dmsh_pch.py, GitHub issue #1036): a compile uses it when it builds and gives the same bytes on a test shader, runs
again without it when it fails, so what a failure reports is the plain compile's, and DMSH_PCH=0 turns it off. With a Metal compiler (each SDK skipped without one), synthetic shaders
compile to byte-identical output with and without it, in the flags of the real builds; a shader with an error raises as a plain compile does. The source goes on standard input, so a
library does not depend on its file's name. No game files."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
sys.path.insert(0, str(REPO / 'dm/ios/tools'))
import dmsh_pch  # noqa: E402
import ios_metal  # noqa: E402

MAC = ['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance']   # dmsh_build.py MAC_METAL (that file is a script that runs a build when imported)
SHADERS = {
    'vertex': 'vertex float4 main0(uint v [[vertex_id]], constant float4* c [[buffer(0)]]) { return c[v]; }\n',
    'fragment_many': dmsh_pch.PROBE.split('using namespace metal;\n')[1],
    'compute_write': 'kernel void main0(texture2d<float, access::write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(float4(1), p); }\n',
    'depth': 'fragment float4 main0(depth2d<float> d [[texture(0)]], sampler s [[sampler(0)]], float4 p [[position]]) { return d.sample(s, p.xy); }\n',
    'not_in_the_table': 'kernel void main0(texture2d<uint, access::read_write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(t.read(p), p); }\n',
}


def has_sdk(sdk):
    return shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True).returncode == 0


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nxpch-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        env = mock.patch.dict(os.environ, {'WORK': str(self.tmp)}); env.start(); self.addCleanup(env.stop); os.environ.pop('DMSH_PCH', None)
        headers = mock.patch.object(dmsh_pch, '_headers', {}); headers.start(); self.addCleanup(headers.stop)


class Choice(Tmp):
    def setUp(self):
        super().setUp()
        for n in 'ab': (self.tmp / f'{n}.metal').write_text(n)
        cwd = os.getcwd(); os.chdir(self.tmp); self.addCleanup(os.chdir, cwd)

    def result(self, code=0, err=b''):
        return subprocess.CompletedProcess([], code, b'', err)

    def test_a_good_header_is_used_and_the_compile_runs_once(self):
        with mock.patch.object(dmsh_pch, 'make', return_value=('/p.pch', None)) as make, mock.patch.object(subprocess, 'run', return_value=self.result()) as run:
            dmsh_pch.compile(['metal', '-c'], 'a.metal', 'a.air'); dmsh_pch.compile(['metal', '-c'], 'b.metal', 'b.air')
        make.assert_called_once()   # one header per convention
        self.assertEqual([c.args[0] for c in run.call_args_list], [['metal', '-c', '-include-pch', '/p.pch', '-x', 'metal', '-', '-o', n + '.air'] for n in 'ab'])

    def test_a_failure_with_the_header_runs_the_plain_compile(self):
        with mock.patch.object(dmsh_pch, 'make', return_value=('/p.pch', None)), mock.patch.object(subprocess, 'run', side_effect=[self.result(1), self.result()]) as run:
            dmsh_pch.compile(['metal', '-c'], 'a.metal', 'a.air')
        self.assertEqual(run.call_args_list[1].args[0], ['metal', '-c', '-x', 'metal', '-', '-o', 'a.air'])
        self.assertTrue(run.call_args_list[1].kwargs.get('check'))

    def test_a_header_that_cannot_be_used_is_said_once_and_compiles_stay_plain(self):
        with mock.patch.object(dmsh_pch, 'make', return_value=(None, 'a test shader compiled with it differs')) as make, mock.patch.object(subprocess, 'run', return_value=self.result()) as run, mock.patch('builtins.print') as say:
            dmsh_pch.compile(['metal', '-c'], 'a.metal', 'a.air'); dmsh_pch.compile(['metal', '-c'], 'b.metal', 'b.air')
        make.assert_called_once(); say.assert_called_once()
        self.assertIn('differs', say.call_args.args[0])
        self.assertTrue(all('-include-pch' not in c.args[0] for c in run.call_args_list))

    def test_dmsh_pch_0_turns_it_off(self):
        with mock.patch.dict(os.environ, {'DMSH_PCH': '0'}), mock.patch.object(dmsh_pch, 'make') as make, mock.patch.object(subprocess, 'run', return_value=self.result()) as run:
            dmsh_pch.compile(['metal', '-c'], 'a.metal', 'a.air')
        make.assert_not_called()
        self.assertEqual(run.call_args.args[0], ['metal', '-c', '-x', 'metal', '-', '-o', 'a.air'])

    def test_each_run_reads_the_source_from_its_start(self):
        read = []
        def run(cmd, stdin, **kw):
            read.append(stdin.read()); return self.result(len(read) == 1)   # the first run fails, having read the whole source
        with mock.patch.object(dmsh_pch, 'make', return_value=('/p.pch', None)), mock.patch.object(subprocess, 'run', side_effect=run):
            dmsh_pch.compile(['metal', '-c'], 'a.metal', 'a.air')
        self.assertEqual(read, [b'a', b'a'])

    def test_make_refuses_a_header_whose_test_shader_differs(self):
        def run(cmd, **kw):
            if '-o' in cmd and cmd[cmd.index('-o') + 1].endswith('.air'):
                Path(cmd[cmd.index('-o') + 1]).write_bytes(b'with' if '-include-pch' in cmd else b'without')
            return subprocess.CompletedProcess(cmd, 0, '', '')
        with mock.patch.object(subprocess, 'run', side_effect=run):
            self.assertEqual(dmsh_pch.make(['metal', '-c']), (None, 'a test shader compiled with it differs'))

    def test_the_header_declares_no_name(self):
        self.assertEqual({ln.split('(')[0].split(' ')[0] for ln in dmsh_pch.HEADER.splitlines()}, {'#include', 'static_assert'})


class RealCompiler(Tmp):
    def check_identical(self, metal):
        pch, why = dmsh_pch.make(metal)
        if not pch: self.fail(why)
        self.assertTrue(Path(pch).is_file())
        for name, body in SHADERS.items():
            src = self.tmp / f'{name}.metal'; src.write_text('#pragma clang diagnostic ignored "-Wmissing-prototypes"\n\n#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n' + body)
            plain, used = self.tmp / f'{name}.plain.air', self.tmp / f'{name}.air'
            with open(src, 'rb') as f: subprocess.run([*metal, '-x', 'metal', '-', '-o', plain], stdin=f, check=True, capture_output=True)   # as compile runs it: an .air records a named source's path
            with mock.patch.dict(dmsh_pch._headers, {tuple(metal): pch}), mock.patch.object(subprocess, 'run', wraps=subprocess.run) as run:
                dmsh_pch.compile(metal, str(src), str(used))
            self.assertIn('-include-pch', run.call_args_list[0].args[0]); self.assertEqual(run.call_count, 1, f'{name}: the compile with the header failed')
            self.assertEqual(plain.read_bytes(), used.read_bytes(), name)

    @unittest.skipUnless(has_sdk('macosx'), 'no macOS Metal compiler')
    def test_mac_output_is_byte_identical(self):
        self.check_identical(MAC)

    @unittest.skipUnless(has_sdk('iphoneos'), 'no iOS Metal compiler')
    def test_ios_output_is_byte_identical(self):
        self.check_identical(ios_metal.METAL)

    def check_no_name(self, metal, header):
        pch = dmsh_pch.make(metal)[0] if header else None
        if header: self.assertTrue(pch)
        out = []
        for name in ('0123456789abcdef_bl', 'fedcba9876543210_bl'):   # a function constant brings a static initializer, which clang names after the file
            d = self.tmp / name; d.mkdir(exist_ok=True); src = d / f'{name}.metal'; air = d / f'{name}.{header}.air'
            src.write_text('#include <metal_stdlib>\nusing namespace metal;\nconstant bool fc [[function_constant(0)]];\nfragment float4 main0(float4 d [[color(0), function_constant(fc)]]) { return fc ? d : float4(1); }\n')
            with mock.patch.dict(dmsh_pch._headers, {tuple(metal): pch}): dmsh_pch.compile(metal, str(src), str(air))
            out.append(air.read_bytes())
        self.assertEqual(out[0], out[1])

    @unittest.skipUnless(has_sdk('macosx'), 'no macOS Metal compiler')
    def test_a_mac_library_does_not_depend_on_the_source_name(self):
        self.check_no_name(MAC, True); self.check_no_name(MAC, False)

    @unittest.skipUnless(has_sdk('iphoneos'), 'no iOS Metal compiler')
    def test_an_ios_library_does_not_depend_on_the_source_name(self):
        self.check_no_name(ios_metal.METAL, True); self.check_no_name(ios_metal.METAL, False)

    @unittest.skipUnless(has_sdk('macosx'), 'no macOS Metal compiler')
    def test_a_shader_with_an_error_raises_as_a_plain_compile_does(self):
        src = self.tmp / 'bad.metal'; src.write_text('#include <metal_stdlib>\nfragment float4 main0() { return undeclared; }\n')
        with self.assertRaises(subprocess.CalledProcessError):
            dmsh_pch.compile(MAC, str(src), str(self.tmp / 'bad.air'))

    @unittest.skipUnless(has_sdk('macosx'), 'no macOS Metal compiler')
    def test_a_missing_header_falls_back(self):
        src = self.tmp / 'ok.metal'; src.write_text('#include <metal_stdlib>\nfragment float4 main0() { return float4(1); }\n')
        with mock.patch.dict(dmsh_pch._headers, {tuple(MAC): str(self.tmp / 'gone.pch')}):
            dmsh_pch.compile(MAC, str(src), str(self.tmp / 'ok.air'))
        self.assertTrue((self.tmp / 'ok.air').is_file())


class BracketDepth(Tmp):
    """-fbracket-depth=1024 in every library compile (dmsh_build.py METAL_DEPTH, ios_metal.DEPTH): Ocarina of Time's compute shaders 1f78730053feb036 and e172bd7ec218ab8e nest 259
    brackets deep, past the front end's 256. The records do not name it, so it must change no output of a source within the default limit"""
    DEEP = '#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(device uint* o [[buffer(0)]]) { o[0] = ' + '(' * 300 + '1u' + ')' * 300 + '; }\n'

    def test_the_build_and_ios_take_the_same_depth(self):   # dmsh_build.py is a script, so its value is read from its source
        tree = ast.parse((REPO / 'dm/gfx/tools/dmsh_build.py').read_text())
        mac = next(ast.literal_eval(n.value) for n in ast.walk(tree) if isinstance(n, ast.Assign) and [getattr(t, 'id', None) for t in n.targets] == ['METAL_DEPTH'])
        self.assertEqual(mac, ios_metal.DEPTH)
        self.assertNotIn('bracket', ios_metal.RECORD)

    def check(self, metal):
        deep = self.tmp / 'deep.metal'; deep.write_text(self.DEEP)
        with self.assertRaises(subprocess.CalledProcessError): subprocess.run([*metal, '-x', 'metal', str(deep), '-o', str(self.tmp / 'deep0.air')], check=True, capture_output=True)
        dmsh_pch.compile(metal + ios_metal.DEPTH, str(deep), str(self.tmp / 'deep.air'))
        self.assertTrue((self.tmp / 'deep.air').is_file())
        for name, body in SHADERS.items():   # within the limit: the same bytes
            src = self.tmp / f'{name}.metal'; src.write_text('#include <metal_stdlib>\nusing namespace metal;\n' + body)
            out = []
            for extra in ([], ios_metal.DEPTH):
                with open(src, 'rb') as f: out.append(subprocess.run([*metal, *extra, '-x', 'metal', '-', '-o', '-'], stdin=f, check=True, capture_output=True).stdout)
            self.assertEqual(out[0], out[1], name)

    @unittest.skipUnless(has_sdk('macosx'), 'no macOS Metal compiler')
    def test_mac(self): self.check(MAC)

    @unittest.skipUnless(has_sdk('iphoneos'), 'no iOS Metal compiler')
    def test_ios(self): self.check(ios_metal.METAL)


if __name__ == '__main__':
    unittest.main()
