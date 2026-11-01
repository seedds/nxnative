"""gl_shaders.py --vk (dm/gfx/tools/gl_shaders.py; docs/design/vulkan-backend.md 8.2, step P9b): the SPIR-V library the Vulkan backend's GL layer (dm/gfx/hostvk/guest_gl.c)
reads, made from t_vk_guest_gl's sample shaders (dm/gfx/tests/vk/shaders/gl): the table read from the modules without spirv-cross (vertex inputs, default uniform block,
samplers, fragment outputs, a GLSL bool kept), one module per stage named by the source's hash, the fragment stage's resources in descriptor set 1, the vertex stage's position
mapped to Vulkan's clip z before every return, NaN-preserving min / max / clamp, spirv-val's verdict, and, where the in-repo spirv-cross runs (a Mac), the same table as the
Metal library's. Skipped where glslangValidator is not installed (the nxnative-build image has it). Temporary files only; no game files or keys."""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOL = REPO / 'dm/gfx/tools/gl_shaders.py'
SAMPLES = REPO / 'dm/gfx/tests/vk/shaders/gl'
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_spv  # noqa: E402
import gl_shaders  # noqa: E402

GLSLANG = shutil.which('glslangValidator')
SPIRV_VAL = shutil.which('spirv-val')


def spirv_cross_runs():
    try:
        return subprocess.run([gl_shaders.SPIRV_CROSS, '--revision'], capture_output=True, text=True).returncode == 0
    except OSError:   # missing, or a Mac binary elsewhere
        return False


# The fields gl_shaders.py gives each sample (the bindings of b: and t: aside: --amb's numbering is glslang's to choose, the layer reads them from the table)
EXPECTED = {
    'quad.vert': ['a:aPos=0:vec2', 'a:aUV=1:vec2', 'b:*:20', 'u:uRect=0:vec4:0:0', 'u:uDepth=16:float:0:0'],
    'quad.frag': ['o:fragColor=0', 'b:*:16', 'u:uColor=0:vec4:0:0', 't:uTex=*:sampler2D'],
    'coord.frag': ['o:fragColor=0', 'b:*:8', 'u:uSize=0:vec2:0:0'],
    'stream.vert': ['a:VertexColor=0:vec4', 'a:VertexPosition=1:vec2', 'b:*:96', 'u:uTransform=0:mat4:0:0', 'u:uScale=64:float:2:16'],
    'stream.frag': ['o:fragColor=0', 'b:*:4', 'u:uInvert=0:bool:0:0'],
}


def masked(field):   # a b: or t: field with its binding as *
    if field.startswith('b:'): return 'b:*:' + field.split(':')[2]
    if field.startswith('t:'): name, rest = field.split('=', 1); return f'{name}=*:{rest.split(":", 1)[1]}'
    return field


def words(path):
    data = Path(path).read_bytes()
    return dmsh_spv.Module(data)


@unittest.skipUnless(GLSLANG, 'glslangValidator is not installed')
class VkLibrary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.tmp.name)
        cls.run_ = subprocess.run([sys.executable, str(TOOL), str(SAMPLES), str(cls.out), '--vk'], capture_output=True, text=True)
        cls.lib = cls.out / 'gl-vk'
        cls.rows = {}
        if cls.run_.returncode == 0:
            for line in (cls.lib / 'gl_shaders.tsv').read_text().splitlines():
                if line.startswith('#') or not line: continue
                key, stage, fields, where = line.split('\t')
                cls.rows[where.lstrip('/')] = (key, stage, fields.split(' '))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_runs(self):
        self.assertEqual(self.run_.returncode, 0, self.run_.stdout + self.run_.stderr)
        self.assertIn('vk: 5 shaders (2 vertex)', self.run_.stdout)
        self.assertFalse((self.out / 'gl').exists(), 'no Metal library with --vk')

    def test_table(self):
        self.assertEqual(sorted(self.rows), sorted(EXPECTED))
        for name, want in EXPECTED.items():
            with self.subTest(shader=name):
                key, stage, fields = self.rows[name]
                self.assertEqual(stage, 'vert' if name.endswith('.vert') else 'frag')
                self.assertEqual(key, hashlib.sha256((SAMPLES / name).read_bytes()).hexdigest()[:16], 'the key: the source text\'s hash, as the host hashes it')
                self.assertEqual([masked(f) for f in fields], want)
        _, _, qf = self.rows['quad.frag']
        b = [f for f in qf if f.startswith('b:')][0].split(':')[1]; t = [f for f in qf if f.startswith('t:')][0].split('=')[1].split(':')[0]
        self.assertNotEqual(b, t, 'the block and the sampler of one stage have bindings of their own')

    def modules(self):
        for name, (key, stage, _) in self.rows.items():
            yield name, stage, self.lib / f'gl_{key}_{stage[0]}.spv'

    def test_descriptor_sets(self):
        for name, stage, path in self.modules():
            with self.subTest(shader=name):
                m = words(path)
                sets = [i[3] for i in m.of(dmsh_spv.DECORATE) if i[2] == dmsh_spv.D_DESCRIPTOR_SET]
                self.assertTrue(sets, 'every sample has a default uniform block')
                self.assertEqual(set(sets), {1 if stage == 'frag' else 0})

    def test_clip_z(self):   # before each return of a vertex shader: the position's z set to (z + w) * 0.5
        for name, stage, path in self.modules():
            if stage != 'vert': continue
            with self.subTest(shader=name):
                m = words(path)
                half = struct.unpack('<I', struct.pack('<f', 0.5))[0]
                halves = {i[2] for i in m.of(dmsh_spv.CONSTANT) if len(i) == 4 and i[3] == half}
                rets = [k for k, i in enumerate(m.ins) if i[0] & 0xffff == gl_shaders.OP_RETURN]
                self.assertTrue(rets)
                for k in rets:
                    store, ins, mul = m.ins[k - 1], m.ins[k - 2], m.ins[k - 3]
                    self.assertEqual(store[0] & 0xffff, dmsh_spv.STORE)
                    self.assertEqual((ins[0] & 0xffff, ins[-1]), (gl_shaders.OP_COMPOSITE_INSERT, 2))
                    self.assertEqual(mul[0] & 0xffff, gl_shaders.OP_FMUL); self.assertIn(mul[4], halves)

    def test_precise(self):   # coord.frag's min and clamp: NMin / NClamp, as the Metal path's precise::
        key, stage, _ = self.rows['coord.frag']
        m = words(self.lib / f'gl_{key}_f.spv'); g = m.glsl_set()
        ops = [i[4] for i in m.of(dmsh_spv.EXT_INST) if i[3] == g]
        self.assertIn(79, ops); self.assertIn(81, ops)
        self.assertFalse({37, 40, 43} & set(ops), 'no FMin / FMax / FClamp left')

    @unittest.skipUnless(SPIRV_VAL, 'spirv-val is not installed')
    def test_spirv_val(self):
        for name, _, path in self.modules():
            with self.subTest(shader=name):
                r = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.0', str(path)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_early_returns_and_refusals(self):   # every return of main gets the clip-space code; a struct uniform is refused as the Metal path refuses it
        with tempfile.TemporaryDirectory() as tmp:
            src = (b'#version 320 es\nin vec2 aPos;\nuniform float uK;\nvoid main() {\n    gl_Position = vec4(aPos, 0.25, 1.0);\n'
                   b'    if (uK > 0.5) { gl_Position.x += 1.0; return; }\n    gl_Position.y += 1.0;\n}\n')
            r = gl_shaders.translate_vk(src, 'vert', 'early', tmp)
            self.assertIsInstance(r, tuple, r)
            m = words(r[0])
            rets = sum(1 for i in m.ins if i[0] & 0xffff == gl_shaders.OP_RETURN)
            inserts = sum(1 for i in m.ins if i[0] & 0xffff == gl_shaders.OP_COMPOSITE_INSERT and i[-1] == 2)
            self.assertEqual(inserts, rets); self.assertGreaterEqual(rets, 1)
            bad = b'#version 320 es\nprecision highp float;\nstruct L { vec4 c; };\nuniform L uL;\nout vec4 o;\nvoid main() { o = uL.c; }\n'
            self.assertIn('structs are not implemented', gl_shaders.translate_vk(bad, 'frag', 'bad', tmp))

    @unittest.skipUnless(spirv_cross_runs(), 'the in-repo spirv-cross does not run here (a Mac binary)')
    def test_same_table_as_metal(self):   # spirv-cross --reflect (the Metal path) and the module read by reflect (--vk) give the same fields
        with tempfile.TemporaryDirectory() as tmp:
            for name in EXPECTED:
                with self.subTest(shader=name):
                    src, stage = (SAMPLES / name).read_bytes(), name.rsplit('.', 1)[1]
                    a, b = gl_shaders.translate(src, stage, 'm', tmp), gl_shaders.translate_vk(src, stage, 'v', tmp)
                    self.assertIsInstance(a, tuple, a); self.assertIsInstance(b, tuple, b)
                    self.assertEqual(sorted(a[1]), sorted(b[1]))


if __name__ == '__main__':
    unittest.main()
