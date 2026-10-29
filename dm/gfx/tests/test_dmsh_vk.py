"""The shader chain for the Vulkan target (docs/design/vulkan-backend.md, section 2) on synthetic Maxwell shaders only, no game files: the Vulkan translator
(dm/gfx/tools/dmsh_xlate_vk), the SPIR-V passes (dmsh_spv.py), the build and its census (dmsh_build_vk.py) and the version (dmsh_chain_vk.py).
Run in the chain image (docker/README.md has the command): the translator built into build/xlate_vk (XLATE_VK selects another dmsh_xlate_vk.dll), dotnet, SPIRV-Tools (spirv-val,
spirv-as, spirv-link) and spirv-cross on PATH (SPIRV_CROSS_VK selects another). Where one of them is missing, the tests that need it skip and say which; the portable checks still run.
Each class prints its counts (shaders translated, entry points, validation results, bindings checked)."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / 'dm/gfx/tools'
RYU = ROOT / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader'
sys.path.insert(0, str(TOOLS))
import dmsh_chain  # noqa: E402
import dmsh_build_vk  # noqa: E402
import dmsh_chain_vk  # noqa: E402
import dmsh_spv  # noqa: E402
spec = importlib.util.spec_from_file_location('test_image_atomic_bounds', ROOT / 'dm/tools/tests/test_image_atomic_bounds.py')
atomic_tests = importlib.util.module_from_spec(spec); spec.loader.exec_module(atomic_tests)   # its synthetic_shader: the compute shapes

DOTNET = os.environ.get('DOTNET', 'dotnet')
XLATE = Path(dmsh_chain_vk.XLATE_VK)
SPIRV_CROSS = dmsh_chain_vk.spirv_cross()


def missing(*tools):
    """The reason a test cannot run here, or '' (the translator, dotnet and the SPIR-V tools it names)"""
    gone = [t for t in tools if not shutil.which(t)]
    if 'xlate' in tools:
        gone.remove('xlate') if 'xlate' in gone else None
        if not XLATE.is_file(): gone.append(f'the Vulkan translator {XLATE} (build it as docker/README.md says)')
        if not shutil.which(DOTNET): gone.append('dotnet')
    return ', '.join(gone)


# Synthetic Maxwell shaders, encoded from the vendored translator's instruction table (as test_image_atomic_bounds.synthetic_shader does)
TABLE = (RYU / 'Decoders/InstTable.cs').read_text()
PT, RZ = 7, 255


def base(name):
    return int(re.search(r'Add\("([01x]{64})", InstName\.' + name + r'\s*,', TABLE)[1].replace('x', '0'), 2)


def mov(r, v): return base('Mov32i') | ((v & 0xffffffff) << 20) | (PT << 16) | (15 << 12) | r
def ald(d, attr, size=3): return base('Ald') | d | (RZ << 8) | (PT << 16) | (attr << 20) | (RZ << 39) | (size << 47)          # ALD.32 .. .128 Rd, a[attr]
def ast(s, attr, size=3): return base('Ast') | s | (RZ << 8) | (PT << 16) | (attr << 20) | (RZ << 39) | (size << 47)          # AST a[attr], Rs
def ipa(d, attr): return base('Ipa') | d | (RZ << 8) | (PT << 16) | (RZ << 20) | (attr << 28) | (RZ << 39) | (PT << 47)        # IPA.PASS Rd, a[attr]
def fmnmx(d, a, b, low=True): return base('Fmnmx') | d | (a << 8) | (PT << 16) | (b << 20) | (PT << 39) | ((0 if low else 1) << 42)
def iadd32i(d, a, imm): return base('Iadd32i') | d | (a << 8) | (PT << 16) | ((imm & 0xffffffff) << 20)
def tex2d(d, coord, handle, mask=15): return base('Tex') | d | (coord << 8) | (PT << 16) | (RZ << 20) | (2 << 28) | (mask << 31) | (handle << 36) | (PT << 51)   # TEX.2D, slot handle
def ldc(d, slot, offset, size=4): return base('Ldc') | d | (RZ << 8) | (PT << 16) | (offset << 20) | (slot << 36) | (size << 48)   # LDC.32 (size 5: .64) Rd, c[slot][offset]
def ldg(d, a): return base('Ldg') | d | (a << 8) | (PT << 16) | (1 << 45) | (4 << 48)                                               # LDG.E.32 Rd, [Ra]
def stg(s, a): return base('Stg') | s | (a << 8) | (PT << 16) | (1 << 45) | (4 << 48)                                               # STG.E.32 [Ra], Rs
def dadd(d, a, b): return base('Dadd') | d | (a << 8) | (PT << 16) | (b << 20)
def vote_any(d): return base('Vote') | d | (PT << 16) | (PT << 39) | (PT << 45) | (1 << 48)
def exit_(): return base('Exit') | (PT << 16) | 15


def code(instructions, header=b''):
    words = []
    for i, ins in enumerate(instructions + [exit_()]):
        if i % 3 == 0: words.append(0)   # the scheduling word before each group of three
        words.append(ins)
    return header + struct.pack('<%dQ' % len(words), *words)


def sph(stage, omap=0, imap=(), mrt=False):
    """A shader program header: stage 1 vertex, 4 geometry, 5 fragment; omap: the fragment outputs' component bits (4 per target); imap: (generic input, interpolation byte)"""
    w = [0] * 20
    w[0] = 1 | (3 << 5) | (stage << 10) | (int(mrt) << 14)
    for i, b in imap: w[6 + (i >> 2)] |= b << ((i & 3) * 8)
    w[18] = omap
    return struct.pack('<20I', *w)


PERSPECTIVE, FLAT = 0xAA, 0x55   # an input's four components interpolated with perspective, or constant
GENERIC0, GENERIC1, POSITION = 0x80, 0x90, 0x70

# key → (stage, code, facts): the shapes the chain treats apart
GRAPHICS = {
    'vs_basic': ('vs', code([ald(0, GENERIC0), ast(0, POSITION), ast(0, GENERIC0, 1)], sph(1)), {}),            # output 0 written in x, y only: vs_init's (0, 0, 0, 1) for z, w
    'vs_int': ('vs', code([ald(4, GENERIC0), ast(4, POSITION), ald(0, GENERIC1, 0), iadd32i(0, 0, 1), ast(0, GENERIC0, 0)], sph(1)), {}),   # input 1 only used as an integer
    'vs_tf': ('vs', code([ald(0, GENERIC0), ast(0, POSITION), ast(0, GENERIC0)], sph(1)), {}),                   # captured by transform feedback (the job's tf)
    'vs_vote': ('vs', code([ald(0, GENERIC0), vote_any(4), ast(0, POSITION), ast(4, GENERIC0, 0)], sph(1)), {}),  # a subgroup ballot in a vertex shader
    'vs_fp64': ('vs', code([ald(0, GENERIC0), dadd(4, 0, 0), ast(0, POSITION), ast(4, GENERIC0, 1)], sph(1)), {}),  # doubles: the _fp32 variant
    # a game uniform slot (c3: slot 0), the shader's constants (c1), and storage buffer 0 through its address in the driver bank (c0 + 0x110), read and written
    'vs_cbuf': ('vs', code([ldc(0, 3, 0x10), ldc(1, 1, 0), ldc(2, 0, 0x110, size=5), ldg(5, 2), stg(5, 2), ald(8, GENERIC0), ast(8, POSITION), ast(0, GENERIC0, 1)], sph(1)), {}),
    'ps_cbuf': ('ps', code([ipa(0, GENERIC0), ldc(1, 4, 0x20), ldc(2, 1, 0x200)], sph(5, omap=0xF, imap=[(0, PERSPECTIVE)])), {}),   # c1 + 0x200: past a shared bank's 256 bytes
    'ps_minmax': ('ps', code([ipa(0, GENERIC0), ipa(1, GENERIC0 + 4), fmnmx(2, 0, 1), fmnmx(3, 0, 1, low=False), mov(1, 0)], sph(5, omap=0xF, imap=[(0, PERSPECTIVE)])), {}),
    'ps_tex': ('ps', code([ipa(0, GENERIC0), ipa(1, GENERIC0 + 4), tex2d(0, 0, 8, mask=1), iadd32i(0, 0, 1)], sph(5, omap=0xF, imap=[(0, PERSPECTIVE)])), {'tex': {'0': '2d'}}),
    'ps_uint': ('ps', code([ipa(0, GENERIC0), iadd32i(0, 0, 1)], sph(5, omap=0xF, imap=[(0, FLAT)])), {}),          # an output from integer bits: main_u
    'ps_mixed': ('ps', code([ipa(0, GENERIC0), ipa(1, GENERIC1), iadd32i(4, 1, 1)], sph(5, omap=0xFF, imap=[(0, PERSPECTIVE), (1, FLAT)], mrt=True)), {}),   # main_u and main_m
    'gs_pass': ('gs', struct.pack('<5I', (1 << 24) | (4 << 10) | (3 << 5) | 1, 0, 1 << 24, 7 << 24, 1) + bytes(0x3c) + struct.pack('<QQ', 0, exit_()), {}),   # passthrough header
}
TF = [{'stride': 16, 'outputs': [[0, 4]]}, [], [], []]


def compute_shapes():
    """The image-atomic compute shapes of test_image_atomic_bounds (dimensions, reductions, compare-and-swap, source sizes, a bindless descriptor, the bounds modes)"""
    out = {}
    for dim, image in ((1, 'buffer'), (3, '2d'), (4, '2darray'), (5, '3d')):
        out[f'cs_return_{image}'] = (atomic_tests.synthetic_shader(0, 0, dim=dim, discard=False, consume_result=True), image, 'R32Uint')
        out[f'cs_cas_{image}'] = (atomic_tests.synthetic_shader(0, 0, dim=dim, discard=False, consume_result=True, cas=(3, 7)), image, 'R32Uint')
    for d, shape in enumerate(('1d', 'buffer', '1darray', '2d', '2darray', '3d')):
        for reduction in (False, True): out[f'cs_dim_{shape}_{int(reduction)}'] = (atomic_tests.synthetic_shader(1, 2, 3, reduction=reduction, dim=d), shape, 'R32Uint')
    for size in (0, 1, 6):
        out[f'cs_size_{size}'] = (atomic_tests.synthetic_shader(0, 0, size=size), '2darray', 'R32Uint')
    out['cs_bindless'] = (atomic_tests.synthetic_shader(0, 0, reduction=True, size=6, bindless='cbuf'), '2darray', 'R32Sint')
    for mode in (1, 2, 3): out[f'cs_mode_{mode}'] = (atomic_tests.synthetic_shader(0, 0, mode=mode), '2darray', 'R32Uint')
    return out


def jobs_for(work):
    jobs = []
    for key, (stage, data, facts) in GRAPHICS.items():
        p = work / (key + '.bin'); p.write_bytes(data)
        jobs.append({**dict(key=key, stage=stage, code=str(p), tex={}, img={}, attr={}, fout={}, debug=True, reduced=True), **facts, **({'tf': TF} if key == 'vs_tf' else {})})
    for key, (data, image, fmt) in compute_shapes().items():
        p = work / (key + '.bin'); p.write_bytes(data)
        jobs.append(dict(key=key, stage='cs', code=str(p), tex={}, img={'0': dict(type=image, fmt=fmt)}, attr={}, fout={}, local=[1, 1, 1], debug=True, reduced=True))
    return jobs


STAGE = {'vs': 0, 'hs': 1, 'ds': 2, 'gs': 3, 'ps': 4, 'cs': 0}


def check_bindings(test, stage, m, where):
    """Every descriptor of a module where the binding model of 2.3 puts it (dmsh_xlate_vk/Program.cs); → the number checked"""
    s, n = STAGE[stage], 0
    for r in dmsh_spv.resources(m):
        nm, b = r['name'], r['binding']
        with test.subTest(module=where, binding=nm):
            if r['set'] == 0:
                test.assertEqual(r['class'], 'ubo')
                if b == 0: test.assertEqual(nm, 'support_buffer')
                else: test.assertEqual(((b - 1) // 18, (b - 1) % 18), (s, int(re.fullmatch(r'[a-z]+_c(\d+)', nm).group(1))))
            elif r['set'] == 1:
                test.assertEqual(r['class'], 'ssbo'); test.assertEqual((b // 16, b % 16), (s, int(re.fullmatch(r'[a-z]+_s(\d+)', nm).group(1))))
            elif r['set'] == 2:
                test.assertIn(r['class'], ('combined', 'texture', 'sampler')); test.assertEqual(b // 512, s)
                if b % 512 < 256: test.assertEqual(b % 512, slot_of(nm, image=False))
            elif r['set'] == 3:
                test.assertIn(r['class'], ('storage_image', 'storage_texel_buffer', 'texel_buffer')); test.assertEqual(b // 512, s)
                if b % 512 < 8: test.assertEqual(b % 512, slot_of(nm, image=True))
                elif b % 512 < 264: test.assertEqual(b % 512 - 8, slot_of(nm, image=False))
            else: test.fail(f'descriptor set {r["set"]}')
        n += 1
    return n


def slot_of(name, image):   # the NVN slot dmsh_build.py decode_tex reads from a binding's name
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name); h = int(m.group(2), 16)
    for w in (h & 0x3fff, (h >> 14) & 0x3fff):
        b = w * 4
        if image and 0x120 <= b < 0x160 and b % 8 == 0: return (b - 0x120) // 8
        if not image and 0x20 <= b < 0x168 and b % 8 == 0: return (b - 0x20) // 8
        if not image and 0x168 <= b < 0x568 and b % 8 == 0: return 128 + (b - 0x168) // 8
    return -1


def first_stores(m):
    """The OpStore instructions right after the vertex entry point's first OpLabel and OpVariables: (variable, value)"""
    fn = next(e[2] for e in m.entry_points() if e[0] == 'vs')
    k = m.entry_insert_point(fn); out = []
    while m.ins[k][0] & 0xffff == dmsh_spv.STORE: out.append((m.ins[k][1], m.ins[k][2])); k += 1
    return out


class Portable(unittest.TestCase):   # no tools needed
    def test_metal_chain_untouched(self):
        self.assertFalse([f for f in dmsh_chain.FILES if 'dmsh_xlate_vk' in f or f.endswith(('_vk.py', '_vk.lock', 'dmsh_spv.py'))], 'a Vulkan chain file in the Metal version (DMSH_CHAIN)')
        self.assertIn(f'"{dmsh_chain.version()}"', (TOOLS.parent / 'nvn/dmsh_chain.h').read_text())

    def test_vulkan_version_covers_its_files(self):
        for f in ('tools/dmsh_build_vk.py', 'tools/dmsh_spv.py', 'tools/dmsh_xlate_vk/Program.cs', 'tools/dmsh_xlate_vk/dmsh_xlate_vk.csproj', 'tools/dmsh_build.py',
                  'tools/dmsh_chain_vk.lock', 'tools/dmsh_facts.py', 'tools/c1_guard.py', 'tools/cb_extent.py'):
            self.assertIn(f, dmsh_chain_vk.FILES)
        self.assertTrue(set(f for f in dmsh_chain.FILES if 'third_party' in f) <= set(dmsh_chain_vk.FILES))
        self.assertIn(f'"{dmsh_chain_vk.version()}"', Path(dmsh_chain_vk.HEADER).read_text(), 'dm/gfx/nvn/dmsh_chain_vk.h is stale: run dm/gfx/tools/dmsh_chain_vk.py --header')

    def test_lock_rows(self):   # the dlls and the source hash, and per-platform tool rows; no Metal compiler
        lock = dmsh_chain_vk.lock()
        for k in dmsh_chain_vk.DLLS + ('xlate_source',): self.assertRegex(lock.get(k, ''), r'^[0-9a-f]{16}$', k)
        self.assertEqual(lock['xlate_source'], dmsh_chain_vk.xlate_source_sha(), 'dmsh_chain_vk.lock holds another translator source: rebuild and run --update-lock in the chain image')
        self.assertTrue(any(k.startswith('spirv-tools_') for k in lock) and any(k.startswith('spirv-cross_') for k in lock) and any(k.startswith('glslang_') for k in lock))
        self.assertFalse([k for k in lock if 'metal' in k])

    def test_kinds_are_nvn_refl_kinds(self):   # every reflection kind names a binding kind of nvn_refl.h
        kinds = dmsh_build_vk.KINDS
        enums = set(re.findall(r'\bNVN_RB_\w+', (TOOLS.parent / 'nvn/nvn_refl.h').read_text()))
        self.assertFalse(set(kinds.values()) - enums)
        self.assertEqual(len(kinds), 19)

    def test_passes_on_assembly(self):   # the passes on a hand-written module (no translator): a vertex shader with a subgroup mask, test_subgroup.py's
        if missing('spirv-as', 'spirv-val'): self.skipTest('needs ' + missing('spirv-as', 'spirv-val'))
        asm = (Path(__file__).resolve().parent / 'test_subgroup.py').read_text().split("ASSEMBLY = '''", 1)[1].split("'''", 1)[0].replace('ACCESS', 'OpAccessChain')
        asm = asm.replace('%position = OpVariable %po Output', '%position = OpVariable %po Output\n%color = OpVariable %po Output').replace(
            'OpDecorate %position BuiltIn Position', 'OpDecorate %position BuiltIn Position\nOpDecorate %color Location 0').replace('%mask %vertex %position', '%mask %vertex %position %color')
        with tempfile.TemporaryDirectory() as d:
            src, spv = Path(d) / 'a.spvasm', Path(d) / 'a.spv'
            src.write_text(asm); subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(src), '-o', str(spv)], check=True, capture_output=True)
            m = dmsh_spv.Module.read(spv)
            done = dmsh_spv.apply(m, 'vs', pad={3: ('float', 4, 0)}, fp_controls=True)
            self.assertEqual((done['invariant'], done['vs_init'], done['vs_pad'], done['float_controls']), (1, 1, [3], 1))
            m.write(str(spv)); ok, why = dmsh_spv.validate(str(spv)); self.assertTrue(ok, why)
            self.assertEqual(len(first_stores(m)), 2)
            self.assertEqual(dmsh_spv.interface(m)[-1]['location'], 3)


@unittest.skipIf(missing('xlate', 'spirv-val'), 'needs ' + missing('xlate', 'spirv-val'))
class Translator(unittest.TestCase):
    """The Vulkan translator on every synthetic shape: spirv-val clean (Vulkan 1.3), bindings where 2.3 puts them, the passes' marks"""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-vk-xlate-'); cls.work = Path(cls.tmp.name)
        cls.jobs = jobs_for(cls.work)
        (cls.work / 'jobs.json').write_text(json.dumps(cls.jobs))
        cls.result = subprocess.run([DOTNET, str(XLATE), str(cls.work / 'jobs.json'), str(cls.work)], capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def module(self, key, sfx=''): return dmsh_spv.Module.read(self.work / f'{key}{sfx}.spv')

    def test_source_sha_matches_lock(self):
        r = subprocess.run([DOTNET, str(XLATE), '--source-sha'], capture_output=True, text=True, check=True)
        self.assertEqual(r.stdout.strip(), dmsh_chain_vk.xlate_source_sha())
        self.assertEqual(r.stdout.strip(), dmsh_chain_vk.lock()['xlate_source'])

    def test_every_shape_translates_and_validates(self):
        self.assertEqual(self.result.returncode, 0, self.result.stderr)
        counts = {'shaders': 0, 'modules': 0, 'valid': 0, 'entry points': 0, 'bindings': 0}
        for job in self.jobs:
            for sfx in ('', '_fp32'):
                p = self.work / f'{job["key"]}{sfx}.spv'
                if not p.exists(): continue
                m = dmsh_spv.Module.read(p); dmsh_spv.apply(m, job['stage']); p2 = self.work / f'{job["key"]}{sfx}.passes.spv'; m.write(str(p2))
                for q in (p, p2):
                    ok, why = dmsh_spv.validate(str(q)); self.assertTrue(ok, f'{q.name}: {why}'); counts['valid'] += 1
                counts['modules'] += 1; counts['entry points'] += len(m.entry_points())
                counts['bindings'] += check_bindings(self, job['stage'], m, p.name)
            counts['shaders'] += 1
        print(f'\nVulkan translator: {counts["shaders"]} shaders translated, {counts["modules"]} modules, {counts["entry points"]} entry points, '
              f'{counts["valid"]} spirv-val --target-env vulkan1.3 runs clean (before and after the passes), {counts["bindings"]} bindings checked against 2.3')
        self.assertEqual(counts['shaders'], len(GRAPHICS) + len(compute_shapes()))

    def test_stages(self):
        for job in self.jobs:
            self.assertEqual([e[0] for e in self.module(job['key']).entry_points()], [job['stage']], job['key'])
            self.assertEqual([e[1] for e in self.module(job['key']).entry_points()], ['main'])

    def test_image_bindings(self):   # an image in slot 0 of the compute stage: set 3, binding 0; the combined texture of slot 0 in the pixel stage: set 2, binding 4 * 512
        for key in compute_shapes():
            imgs = [r for r in dmsh_spv.resources(self.module(key)) if r['class'] in ('storage_image', 'storage_texel_buffer')]
            self.assertEqual([(r['set'], r['binding']) for r in imgs], [(3, 0)], key)
        tex = [r for r in dmsh_spv.resources(self.module('ps_tex')) if r['class'] == 'combined']
        self.assertEqual([(r['set'], r['binding'], r['dim']) for r in tex], [(2, 4 * 512, '2d')])
        res = json.loads((self.work / 'ps_tex.res.json').read_text())
        self.assertEqual([(t['set'], t['binding']) for t in res['textures']], [(2, 2048)])
        self.assertEqual(res['sbuf_align'], 64)

    def test_buffer_bindings(self):   # uniform buffers in set 0 at 1 + 18 s + slot, storage buffers in set 1 at 16 s + index, the support buffer at set 0, binding 0
        got = {r['name']: (r['set'], r['binding'], r['class']) for r in dmsh_spv.resources(self.module('vs_cbuf'))}
        self.assertEqual({k: got[k] for k in ('support_buffer', 'vp_c1', 'vp_c3', 'vp_s0')}, {'support_buffer': (0, 0, 'ubo'), 'vp_c1': (0, 2, 'ubo'), 'vp_c3': (0, 4, 'ubo'), 'vp_s0': (1, 0, 'ssbo')})
        self.assertTrue(got['vp_s0'] and [r for r in dmsh_spv.resources(self.module('vs_cbuf')) if r['name'] == 'vp_s0'][0]['writable'])
        got = {r['name']: (r['set'], r['binding']) for r in dmsh_spv.resources(self.module('ps_cbuf'))}
        self.assertEqual({k: got[k] for k in ('fp_c1', 'fp_c4')}, {'fp_c1': (0, 1 + 4 * 18 + 1), 'fp_c4': (0, 1 + 4 * 18 + 4)})
        res = json.loads((self.work / 'vs_cbuf.res.json').read_text())
        self.assertEqual([(b['set'], b['binding'], b['cb_slot'], b['cb_offset']) for b in res['sbufs']], [(1, 0, 0, 0x110 // 4)])

    @unittest.skipIf(missing(SPIRV_CROSS), 'needs ' + missing(SPIRV_CROSS))
    def test_same_decisions_as_metal_reflect(self):   # the Vulkan reflection's kinds and slots are what dmsh_build.py reflect makes of the analysis rendering of the same module
        import ast, dmsh_geometry
        shared = dmsh_build_vk.shared({'re': re, 'os': os, 'sys': sys, 'struct': struct, 'json': json, 'hashlib': __import__('hashlib'), 'collections': __import__('collections'),
                                       'inspect': __import__('inspect'), 'dmsh_geometry': dmsh_geometry})
        node = next(n for n in ast.parse((TOOLS / 'dmsh_build.py').read_text()).body if isinstance(n, ast.FunctionDef) and n.name == 'reflect')   # the Metal reflection
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), shared)
        REFLECT, scope = shared['reflect'], types.SimpleNamespace(s=shared)
        compared = 0
        for job in self.jobs:
            if job['stage'] not in ('vs', 'ps', 'cs'): continue
            spv = self.work / (job['key'] + '.spv'); flat = self.work / (job['key'] + '.flat.spv')
            dmsh_spv.flatten(dmsh_spv.Module.read(spv)).write(str(flat))
            r = subprocess.run([SPIRV_CROSS, *dmsh_build_vk.dmsh_atomic.cross_args(scope.s['XC'], str(flat)), str(flat)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, job['key'] + ': ' + r.stderr)
            res = json.loads((self.work / (job['key'] + '.res.json')).read_text())
            metal = {(b['msl_name'][:-5] if b['msl_name'].endswith('Smplr') else b['msl_name'], b['kind'], b.get('reg'), b.get('off')) for b in REFLECT(job['key'], job['stage'], r.stdout, res)}
            vk = {(b['name'], b['kind'], b.get('reg'), b.get('off')) for b in dmsh_build_vk.Build.bindings(scope, job['stage'], dmsh_spv.Module.read(spv), res)}
            self.assertEqual(vk, metal, job['key']); compared += len(vk)
        print(f'\nreflection: {compared} bindings decided as dmsh_build.py reflect decides them on the analysis MSL')

    def test_target_neutral_atomics(self):   # no Metal image-atomics guard: the coordinate is not compared with the image size before the atomic
        m = self.module('cs_dim_2d_0')
        self.assertTrue(m.of(dmsh_spv.IMAGE_TEXEL_POINTER)); self.assertFalse(m.of(dmsh_spv.IMAGE_QUERY_SIZE))

    def test_precise(self):
        m = self.module('ps_minmax'); g = m.glsl_set()
        before = sorted(i[4] for i in m.of(dmsh_spv.EXT_INST) if i[3] == g and i[4] in (37, 40, 43, 79, 80, 81))
        self.assertTrue(set(before) & {37, 40}, before)
        dmsh_spv.precise(m)
        after = sorted(i[4] for i in m.of(dmsh_spv.EXT_INST) if i[3] == g and i[4] in (37, 40, 43, 79, 80, 81))
        self.assertFalse(set(after) & {37, 40, 43}); self.assertTrue({79, 80} <= set(after))

    def test_invariant_and_vs_init(self):
        for key in [k for k, v in GRAPHICS.items() if v[0] == 'vs']:
            m = self.module(key); dmsh_spv.apply(m, 'vs')
            dec, mdec = m.decorations(), m.member_decorations()
            pos = [v for v, _, _ in m.variables(dmsh_spv.SC_OUTPUT) if dec.get(v, {}).get(dmsh_spv.D_BUILTIN) == [0]]
            members = [k for k, d in mdec.items() if d.get(dmsh_spv.D_BUILTIN) == [0]]
            self.assertTrue(all(dmsh_spv.D_INVARIANT in dec[v] for v in pos) and all(dmsh_spv.D_INVARIANT in mdec[k] for k in members) and (pos or members), key)
            outs = {x['id'] for x in dmsh_spv.interface(m) if x['storage'] == 'out'}
            self.assertEqual({v for v, _ in first_stores(m)}, outs, key)

    def test_vs_basic_output_default(self):   # output 0 is written in x and y only: z and w read (0, 1) from the store at entry
        m = self.module('vs_basic'); dmsh_spv.vs_init(m); defs = m.defs()
        (var, value), = first_stores(m)
        parts = defs[value][3:]
        self.assertEqual([defs[p][3] for p in parts], [0, 0, 0, 0x3f800000])

    def test_transform_feedback(self):
        m = self.module('vs_tf')
        self.assertIn(53, [i[1] for i in m.of(dmsh_spv.CAPABILITY)])                    # TransformFeedback
        self.assertIn(11, [i[2] for i in m.of(dmsh_spv.EXECUTION_MODE)])                # Xfb
        xfb = {d: v for d, v in next(d for v, d in m.decorations().items() if 36 in d).items() if d in (35, 36, 37)}
        self.assertEqual(xfb, {36: [0], 37: [16], 35: [0]})                            # XfbBuffer 0, XfbStride 16, Offset 0

    def test_subgroup_in_vertex_stage(self):
        caps = {i[1] for i in self.module('vs_vote').of(dmsh_spv.CAPABILITY)}
        self.assertTrue(caps & {61, 64}, caps)                                          # GroupNonUniform(Ballot / Vote)

    def test_fp64_variant(self):
        self.assertTrue(dmsh_spv.uses_float64(self.module('vs_fp64')))
        self.assertFalse(dmsh_spv.uses_float64(self.module('vs_fp64', '_fp32')))
        self.assertFalse((self.work / 'vs_basic_fp32.spv').exists())

    def test_geometry_is_ordinary(self):   # passthrough reported unsupported: a geometry entry point, no passthrough decoration (PassthroughNV, 5250)
        m = self.module('gs_pass')
        self.assertEqual(m.entry_points()[0][0], 'gs'); self.assertFalse([i for i in m.of(dmsh_spv.DECORATE) if i[2] == 5250])
        self.assertFalse((self.work / 'gs_pass.reader.spv').exists())

    def test_census_reads_bits(self):
        rows = dmsh_spv.census(self.module('ps_tex'))
        self.assertEqual([(r['op'], r['use']) for r in rows], [('sample', 'bits')])
        self.assertEqual([r['use'] for r in dmsh_spv.census(self.module('ps_minmax'))], [])

    def test_shadow_inputs(self):   # an integer input (translated as uint) gets a float shadow at 16 and a signed one at 17
        job = next(j for j in self.jobs if j['key'] == 'vs_int')
        with tempfile.TemporaryDirectory() as d:
            j = dict(job, key='vs_int_u', attr={'1': 'uint'}); (Path(d) / 'j.json').write_text(json.dumps([j]))
            subprocess.run([DOTNET, str(XLATE), str(Path(d) / 'j.json'), d], check=True, capture_output=True)
            m = dmsh_spv.Module.read(Path(d) / 'vs_int_u.spv'); dmsh_spv.apply(m, 'vs')
            ins = {x['location']: x['base'] for x in dmsh_spv.interface(m) if x['storage'] == 'in'}
            self.assertEqual(ins.get(1), 'uint')
            sh = dmsh_spv.shadow_inputs(m)
            self.assertEqual(sh, [{'location': 1, 'float': 16, 'sint': 17}])
            p = Path(d) / 'bv.spv'; m.write(str(p)); ok, why = dmsh_spv.validate(str(p)); self.assertTrue(ok, why)
            ins = {x['location']: x['base'] for x in dmsh_spv.interface(m) if x['storage'] == 'in'}
            self.assertEqual((ins[16], ins[17]), ('float', 'sint'))
            self.assertEqual(dmsh_spv.shadow_inputs(dmsh_spv.Module.read(self.work / 'vs_basic.spv')), 'no integer input')


GAME_ID = '0100FFFFFFFF0000'   # a title ID no game has: the synthetic profile


def record(data, stage, tf=None):
    """A shader package record (dmsh_build.py records / job_for): the magic, a header to +0x38, the code area (graphics: a 0x30-byte header, then the program), the control section"""
    ctl = bytearray(0x880); ctl[0:4] = bytes.fromhex('34127698')
    for b, e in enumerate(tf or []):
        if not e: continue
        struct.pack_into('<H', ctl, 0x6dc + 2 * b, e['stride']); ctl[0x6e8 + b] = len(e['outputs'])
        for k, (loc, n) in enumerate(e['outputs']): ctl[0xdc + 0x80 * b + k] = 0x20 + loc; ctl[0x4dc + 0x80 * b + k] = n
    return b'BTCDBTCD' + bytes(0x30) + (data if stage == 'cs' else bytes(0x30) + data) + bytes(ctl)


@unittest.skipIf(missing('xlate', 'spirv-val', 'spirv-link', SPIRV_CROSS), 'needs ' + missing('xlate', 'spirv-val', 'spirv-link', SPIRV_CROSS))
class Build(unittest.TestCase):
    """dmsh_build_vk.py end to end on a synthetic package and profile: the outputs, the reflection, the stamp and tables, the census, the cache"""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='dm-vk-build-'); t = Path(cls.tmp.name)
        cls.profile = t / 'games' / GAME_ID; cls.profile.mkdir(parents=True); cls.pkg = t / 'pkg'; cls.pkg.mkdir(); cls.cache = t / 'cache'
        (cls.profile / 'game.tsv').write_text('# synthetic profile of dm/gfx/tests/test_dmsh_vk.py\nname\tsynthetic\n')
        shapes = {k: (v[0], v[1], v[2]) for k, v in GRAPHICS.items()}
        shapes.update({k: ('cs', d, {'img': {'0': {'type': image, 'fmt': fmt}}, 'local': [1, 1, 1]}) for k, (d, image, fmt) in list(compute_shapes().items())[:4]})
        cls.keys = {}
        rows, facts, packs = [], [], {}
        for n, (name, (stage, data, f)) in enumerate(shapes.items()):
            key = '%016x' % (0xa0 + n); cls.keys[name] = key
            packs.setdefault(stage, []).append(record(data, stage, TF if name == 'vs_tf' else None))
            rows.append(f'{stage}.bin\t{len(packs[stage]) - 1}\t{stage}\t{key}\tsynthetic {name}')
            if f: facts.append(f'{key}\t{json.dumps(f, sort_keys=True, separators=(",", ":"))}')
        for stage, recs in packs.items(): (cls.pkg / f'{stage}.bin').write_bytes(b''.join(recs))
        (cls.profile / 'shader_map.tsv').write_text('package\tindex\tstage\tdm_ctl_sha16\tsource\n' + '\n'.join(rows) + '\n')
        (cls.profile / 'shader_map.tsv.conflicts.tsv').write_text('')
        (cls.profile / 'shader_facts.tsv').write_text('# synthetic answer table\n' + '\n'.join(facts) + '\n')
        (cls.profile / 'shader_pairs.tsv').write_text(f'{cls.keys["vs_basic"]}\t{cls.keys["ps_mixed"]}\n')   # ps_mixed reads input 1, which vs_basic does not write: vs_pad
        cls.env = dict(os.environ, DM_GAME=str(cls.profile), J='4')
        cls.run1 = subprocess.run([sys.executable, str(TOOLS / 'dmsh_build_vk.py'), str(cls.pkg), str(cls.cache)], env=cls.env, capture_output=True, text=True)
        cls.lib = cls.cache / (dmsh_chain_vk.PREFIX + dmsh_chain_vk.version())

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def refl(self, name): return json.loads((self.lib / (self.keys[name] + '.json')).read_text())

    def test_build(self):
        self.assertEqual(self.run1.returncode, 0, self.run1.stdout + self.run1.stderr)
        print('\n' + self.run1.stdout)
        valid = 0
        for name, key in self.keys.items():
            for sfx in ('', '_bv', '_fp32'):
                p = self.lib / f'{key}{sfx}.spv'
                if p.exists(): ok, why = dmsh_spv.validate(str(p)); self.assertTrue(ok, f'{name}{sfx}: {why}'); valid += 1
        print(f'build outputs spirv-val clean: {valid}')

    def test_reflection(self):
        for name in self.keys:
            r = self.refl(name)
            self.assertEqual((r['chain'], r['target'], r['dm_ctl']), (dmsh_chain_vk.version(), 'vk', self.keys[name]))
            self.assertFalse([b for b in r['bindings'] if b['kind'] not in dmsh_build_vk.KINDS], name)
            for b in r['bindings']: self.assertTrue(isinstance(b['set'], int) and isinstance(b['binding'], int))
        tex = [(b['kind'], b.get('reg'), b['set'], b['binding']) for b in self.refl('ps_tex')['bindings'] if b['set'] == 2]
        self.assertEqual(tex, [('tex', 0, 2, 2048), ('samp_tex', 0, 2, 2048)])
        img = [(b['kind'], b.get('reg'), b.get('atomic'), b['rw']) for b in self.refl('cs_return_buffer')['bindings'] if b['set'] == 3]
        self.assertEqual(img, [('img', 0, True, True)])
        self.assertEqual(self.refl('ps_mixed')['interp']['flat'], 0b10)

    def test_buffers(self):
        b = {(x['kind'], x.get('reg')): x for x in self.refl('vs_cbuf')['bindings']}
        self.assertEqual((b[('cbuf', 0)]['binding'], b[('c1', None)]['binding'], b[('sbuf', 0)]['set'], b[('sbuf', 0)]['rw']), (4, 2, 1, True))
        c1 = (self.profile / 'c1_full_vk.tsv').read_text().splitlines()[1:]
        self.assertEqual(c1, [self.keys['ps_cbuf']])   # c1 + 0 lies in the 256 bytes a shared bank gives any shader, c1 + 0x200 does not (c1_guard.py)
        ext = dict(l.split('\t') for l in (self.profile / 'cb_extent_vk.tsv').read_text().splitlines()[2:])
        self.assertEqual((ext[self.keys['vs_cbuf']], ext[self.keys['ps_cbuf']]), ('0:32', '1:48'))

    def test_integer_outputs(self):
        self.assertEqual(self.refl('ps_uint')['entries'], ['main', 'main_u'])
        r = self.refl('ps_mixed'); self.assertEqual((r['entries'], r['entry_m'], r['uint_mask_m']), (['main', 'main_u', 'main_m'], 'main_m', 0b10))
        m = dmsh_spv.Module.read(self.lib / (self.keys['ps_mixed'] + '.spv'))
        self.assertEqual(sorted(e[1] for e in m.entry_points()), ['main', 'main_m', 'main_u'])
        self.assertEqual(self.refl('ps_minmax')['entries'], ['main'])

    def test_integer_inputs(self):
        r = self.refl('vs_int')
        self.assertEqual(r['int_inputs'], [1]); self.assertEqual({v['location']: v['type'] for v in r['vertex_inputs']}, {0: 'float', 1: 'uint'})
        self.assertEqual(r['vin']['shadow'], [{'location': 1, 'float': 16, 'sint': 17}])
        self.assertTrue((self.lib / (self.keys['vs_int'] + '_bv.spv')).exists())

    def test_pad_and_variants(self):
        self.assertEqual(self.refl('vs_basic')['vs_pad'], [1])
        self.assertEqual(self.refl('vs_fp64')['fp32'], self.keys['vs_fp64'] + '_fp32')
        self.assertEqual(self.refl('vs_tf')['tf_strides'], [16, 0, 0, 0])
        m = dmsh_spv.Module.read(self.lib / (self.keys['vs_tf'] + '.spv'))   # the layout from the control section (tf_layout) reached the translator: Xfb decorations
        self.assertEqual(sorted({d for d in (i[2] for i in m.of(dmsh_spv.DECORATE)) if d in (36, 37)}), [36, 37])
        self.assertFalse(self.refl('gs_pass')['analysis'])

    def test_stamp_and_tables(self):
        sys.path.insert(0, str(ROOT / 'dm/games')); import game
        self.assertEqual(dmsh_chain.read_stamp(str(self.lib)), {'game': GAME_ID, 'inputs': dmsh_chain.inputs(game.load(str(self.profile)))})
        for t in ('c1_full_vk.tsv', 'cb_extent_vk.tsv'):
            self.assertTrue((self.profile / t).read_text().startswith(f'# chain {self.lib.name} inputs '), t)

    def test_census(self):
        r = subprocess.run([sys.executable, str(TOOLS / 'dmsh_build_vk.py'), '--census', str(self.cache)], env=self.env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr); print('\n' + r.stdout.strip())
        rows = [l.split('\t') for l in (self.lib / 'census.tsv').read_text().splitlines()[2:]]
        self.assertEqual([(x[0], x[2], x[3], x[8]) for x in rows], [(self.keys['ps_tex'], 'tex', '0', 'bits')])

    def test_cache_hit(self):   # a second build takes every shader from the content-addressed cache, with the same outputs
        before = {p.name: p.read_bytes() for p in self.lib.glob('*.spv')}
        r = subprocess.run([sys.executable, str(TOOLS / 'dmsh_build_vk.py'), str(self.pkg), str(self.cache)], env=self.env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f'shader cache: hit {len(self.keys)}, built 0', r.stdout)
        self.assertEqual({p.name: p.read_bytes() for p in self.lib.glob('*.spv')}, before)


if __name__ == '__main__':
    unittest.main()
