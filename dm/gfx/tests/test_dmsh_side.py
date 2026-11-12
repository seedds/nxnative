"""One compile for a pixel shader's alpha test and logic operation entry points (dmsh_side.py): the compile unit keeps each body once, and cutting its library gives a base with only the
plain entry points, which pso_offline.py keeps in the pipeline archive, and a side library, byte for byte what the compiler's own tool cuts for one function"""
import pathlib, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))   # its sibling test modules, also when the file runs alone (python -m unittest <file>, as CI does)
import dmsh_side, pso_offline
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'ios/tools'))
import ios_metal   # noqa: E402  (the iPhone's deployment target and math mode)
from test_dmsh_logic import BASE   # noqa: E402

ONE = BASE.split('\nstruct main0_out_u')[0] + '\n'


class Unit(unittest.TestCase):
    def test_one_body_per_entry_point(self):
        u, info = dmsh_side.unit(BASE)
        self.assertEqual(info, {'alpha': ['main0_a', 'main0_u_a', 'main0_m_a'], 'logic': ['main0_l', 'main0_u_l', 'main0_m_l']})
        for name in ('main0', 'main0_u', 'main0_m'): self.assertEqual(u.count(f' dm_body_{name}(float4 gl_FragCoord, constant spvDescriptorSetBuffer0& spvDescriptorSet0)'), 1)
        self.assertEqual(u.count('out.m_1 = float4(0.5);'), BASE.count('out.m_1 = float4(0.5);'))

    def test_a_parameter_it_cannot_pass(self):
        self.assertIsNone(dmsh_side.unit(ONE.replace('[[buffer(0)]])', '[[buffer(0), function_constant(c)]])'))[0])

    def test_no_pixel_shader(self):
        self.assertEqual(dmsh_side.unit('vertex float4 main0() { return float4(0); }'), (None, {'why': 'no fragment entry point main0'}))


@unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
class Cut(unittest.TestCase):
    FLAGS = (('macosx', ()), ('iphoneos', tuple(ios_metal.IOS_FLAGS)))

    def compile(self, d, src, sdk, flags):   # → (the .air, the library)
        msl = d / 'u.metal'; air = d / f'{sdk}.air'; lib = d / f'{sdk}.metallib'; msl.write_text(src)
        subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-O1', '-w', *flags, str(msl), '-o', str(air)], check=True)
        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)], check=True)
        return air, lib

    def test_one_function_as_the_compilers_tool_cuts_it(self):
        with tempfile.TemporaryDirectory() as t:
            d = pathlib.Path(t); u, info = dmsh_side.unit(ONE)
            for sdk, flags in self.FLAGS:
                with self.subTest(sdk=sdk):
                    air, lib = self.compile(d, u, sdk, flags)
                    base, side = dmsh_side.cut(lib.read_bytes(), set(info['alpha'] + info['logic']))
                    tool = d / 'tool.metallib'
                    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '--function-name=main0', '-o', str(tool)], check=True)
                    self.assertEqual(base, tool.read_bytes())
                    subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '--function-name=main0_a', '-o', str(tool)], check=True)
                    self.assertEqual(dmsh_side.cut(side, {'main0_l'})[0], tool.read_bytes())   # a cut of a cut

    def test_every_entry_point_on_its_side_and_the_base_without_constants(self):   # pso_offline.plan leaves a base with constants to the device, and metal-tt compiles all of a library's others
        with tempfile.TemporaryDirectory() as t:
            d = pathlib.Path(t); u, info = dmsh_side.unit(BASE)
            for sdk, flags in self.FLAGS:
                with self.subTest(sdk=sdk):
                    _, lib = self.compile(d, u, sdk, flags)
                    base, side = dmsh_side.cut(lib.read_bytes(), set(info['alpha'] + info['logic']))
                    (d / 'base.metallib').write_bytes(base); (d / 'side.metallib').write_bytes(side)
                    self.assertEqual(pso_offline.functions(d / 'base.metallib'), {'main0': False, 'main0_u': False, 'main0_m': False})
                    self.assertEqual(pso_offline.functions(d / 'side.metallib'), {**dict.fromkeys(info['alpha'], False), **dict.fromkeys(info['logic'], True)})

    def test_refuses_what_it_does_not_know(self):
        with self.assertRaises(ValueError): dmsh_side.cut(b'not a library', set())
        with tempfile.TemporaryDirectory() as t:
            d = pathlib.Path(t); _, lib = self.compile(d, dmsh_side.unit(ONE)[0], *self.FLAGS[0])
            with self.assertRaises(ValueError): dmsh_side.cut(lib.read_bytes(), {'main0_x'})
            with self.assertRaises(ValueError): dmsh_side.cut(lib.read_bytes()[:-1], set())


if __name__ == '__main__':
    unittest.main()
