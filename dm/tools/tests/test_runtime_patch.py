"""Build identity, rewrite-site conflicts, cache lifecycle and the original scene allocation ABI."""
import importlib.util
from pathlib import Path
import os
import re
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'dm/tools'))
import runtime_patch
from nso_asm import B


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RuntimeManifest(unittest.TestCase):
    def setUp(self):
        self.build = 'ab' * 32
        self.module = types.SimpleNamespace(build_id=bytes.fromhex(self.build), text_end=32, image=bytearray(range(64)))
        self.fixups = {'BUILD_ID': self.build[:32], 'TEXT_END': '0x20'}
        self.writes = [('1440p', 4, bytes(range(4, 8)), b'\x1f\x20\x03\xd5')]

    def generate(self, **kw):
        args = dict(module=self.module, patched=bytes(self.module.image), fixups=self.fixups,
                    build=self.build, variable='DM_INTERNAL_RESOLUTION', writes=self.writes)
        return runtime_patch.manifest(**(args | kw))

    def test_default_has_no_writes(self):
        rows = self.generate().splitlines()
        self.assertIn('choice\tdefault', rows)
        self.assertFalse(any(r.startswith('write\tdefault\t') for r in rows))

    def test_build_and_layout_are_required(self):
        for fix in ({'BUILD_ID': 'cd' * 16, 'TEXT_END': '0x20'}, {'BUILD_ID': self.build[:32], 'TEXT_END': '0x24'}):
            with self.assertRaises(ValueError):
                self.generate(fixups=fix)
        with self.assertRaisesRegex(ValueError, 'build ID'):
            self.generate(build='cd' * 32)

    def test_changed_original_and_rewritten_sites_are_rejected(self):
        changed = bytes(64)
        with self.assertRaisesRegex(ValueError, 'rewritten'):
            self.generate(patched=changed)
        with self.assertRaisesRegex(ValueError, 'original'):
            self.generate(writes=[('1440p', 4, bytes(4), bytes(4))])

    def test_invalid_ranges_and_overlap(self):
        for off in (-4, 5, 32, 2**64):
            with self.assertRaises(ValueError):
                self.generate(writes=[('1440p', off, bytes(4), bytes(4))])
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            self.generate(writes=self.writes * 2)
        with self.assertRaises(ValueError):
            self.generate(writes=[('default', *self.writes[0][1:])])


class SceneStub(unittest.TestCase):
    def test_allocated_dimensions_and_return_preserve_other_registers(self):
        profile = load('mp4_stubs', ROOT / 'dm/games/010019A01E2F2000/patches.py')
        words, checked, exports = [], [], {}
        base = 0x2000000
        ctx = types.SimpleNamespace(here=lambda: base + 4 * len(words), emit=words.append,
                                    site=lambda off, original: checked.append((off, original)), runtime_exports=exports)
        profile.resolution_stub(ctx)
        # Decode each generated MOVZ/MOVK and branch using the ISA, independently of the encoder.
        sizes = []
        for k in range(0, len(words), 3):
            register = None
            for word in words[k:k + 2]:
                self.assertEqual(word & 31, 21)  # x21 is the packed scene descriptor; no flags/register spills
                if word & 0xff800000 == 0xd2800000:
                    register = (word >> 5) & 0xffff
                else:
                    self.assertEqual(word & 0xff800000, 0xf2800000)
                    shift = ((word >> 21) & 3) * 16
                    register = (register & ~(0xffff << shift)) | (((word >> 5) & 0xffff) << shift)
            assert register is not None
            sizes.append((register & 0xffffffff, register >> 32))
            self.assertEqual(words[k + 2], B(base + 4 * k + 8, profile.scene_site + 4))
        self.assertEqual(sizes, [(2560, 1440), (3840, 2160), (3200, 1800), (3200, 1792)])
        self.assertEqual({k: exports[k] for k in ('SCENE_1440', 'SCENE_2160', 'SCENE_1800', 'SCENE_D1792')}, {'SCENE_1440': base, 'SCENE_2160': base + 12, 'SCENE_1800': base + 24, 'SCENE_D1792': base + 36})
        self.assertEqual(checked, [(profile.scene_site, '358108aa')])

    def test_unknown_game_build_cannot_generate_manifest(self):
        profile = load('mp4_runtime', ROOT / 'dm/games/010019A01E2F2000/runtime_patches.py')
        with self.assertRaisesRegex(ValueError, 'requires main v1.1.0'):
            profile.generate(types.SimpleNamespace(build_id=bytes(32)), b'', {})

    def test_1440p_changes_only_the_scene_descriptor(self):   # the fixed 1600x900 descriptor (0x3317ec / 0x3317f8) stays: stock scanning depends on the display-sized record
        profile = load('mp4_runtime', ROOT / 'dm/games/010019A01E2F2000/runtime_patches.py')
        stub = 0x1b37a80
        writes = profile.scene_writes(stub)
        self.assertEqual([(c, off, orig.hex()) for c, off, orig, _ in writes], [('1440p', 0x33170c, '358108aa')])
        self.assertEqual([c for c, *_ in profile.scene_writes(stub + 12, '2160p')], ['2160p'])
        self.assertEqual([c[0] for c in profile.CHOICES], ['1440p', '2160p', '1800p', 'diag1792'])
        self.assertTrue(all((w, h) == (1600 * k, 900 * k) for c, _, w, h in profile.CHOICES if c == '1800p' for k in [2]))   # an exact 2:1 of the output
        self.assertEqual(writes[0][3], struct.pack('<I', B(0x33170c, stub)))
        self.assertFalse(any(0x3317ec <= off < 0x3317fc for _, off, _, _ in writes))

    def test_unvalidated_resolution_is_not_in_launcher(self):
        sys.path.insert(0, str(ROOT / 'dm/games'))
        import settings
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(settings.nacp, 'languages', return_value=['en-US']):
            (Path(tmp) / 'main.runtime.tsv').write_text('unvalidated experiment')
            rows = settings.build('010019A01E2F2000', tmp, 'unused')['settings']
        self.assertNotIn('DM_INTERNAL_RESOLUTION', [r['env'] for r in rows])
        self.assertFalse(any(v[0] == '120' for r in rows if r['env'] == 'DM_FPS_TARGET' for v in r['options']))


class ScanSourcePatch(unittest.TestCase):
    def setUp(self):
        self.profile = load('mp4_scan_stubs_test', ROOT / 'dm/games/010019A01E2F2000/patches.py')
        self.runtime = load('mp4_scan_manifest_test', ROOT / 'dm/games/010019A01E2F2000/runtime_patches.py')

    def fixture(self):
        # Synthetic legal layout; no game bytes besides explicitly checked ISA
        # instructions. Stub positions come from the production emitter.
        text, base = 0x138ee20, 0x2000000
        original = bytearray(0x1824000)  # include the RO format and Entity registry tables
        original[self.profile.scene_site:self.profile.scene_site + 4] = bytes.fromhex('358108aa')
        for off, hexbytes in self.profile.scan_source_abi + self.profile.scene_control_abi + ((self.profile.window_extent_site, self.profile.window_extent_original),) + self.profile.window_extent_abi + ((self.profile.ui_camera_site, self.profile.ui_camera_original),) + self.profile.ui_camera_abi + ((self.profile.hud_layout_site, self.profile.hud_layout_original),) + self.profile.hud_layout_abi + self.profile.pause_ui_sites + self.profile.pause_ui_abi + (self.profile.pause_panel_site,) + self.profile.pause_panel_abi + tuple((a,h) for a,h,_,_ in self.profile.menu_visual_hooks()) + self.profile.menu_visual_abi():
            original[off:off + len(bytes.fromhex(hexbytes))] = bytes.fromhex(hexbytes)
        for off, before in self.runtime.OUTPUT_ABI:
            original[off:off + len(before)] = before
        for _, off, before, _ in self.runtime.output_writes('2880p_full', 5120, 2880):
            original[off:off + len(before)] = before
        for off, before, *_ in self.runtime.SHADOW_SITES:
            struct.pack_into('<I', original, off, before)
        original[self.profile.shadow_uv_site:self.profile.shadow_uv_site + 4] = bytes.fromhex('009942fd')
        for off, before, _ in self.runtime.LOD_SITES:
            struct.pack_into('<I', original, off, before)
        struct.pack_into('<I', original, self.profile.lod_distance_site, 0xbd43c920)
        struct.pack_into('<I', original, self.profile.fog_scale_site, 0xbd41a280)
        patched = bytearray(base + 32768)
        patched[:len(original)] = original
        words, checked, exports = [], [], {}
        context = types.SimpleNamespace(here=lambda: base + 4 * len(words), emit=words.append,
            site=lambda off, before: checked.append((off, before)), runtime_exports=exports)
        self.profile.scan_source_stub(context)
        for n, word in enumerate(words):
            struct.pack_into('<I', patched, base + n * 4, word)
        fixups = {'BUILD_ID': self.runtime.BUILD[:32], 'TEXT_END': hex(text), 'PATCH_OFF': hex(base),
                  **{'RUNTIME_' + name: hex(off) for name, off in exports.items()}}
        return types.SimpleNamespace(image=original, text_end=text, build_id=bytes.fromhex(self.runtime.BUILD)), patched, fixups

    def test_only_1800p_and_2160p_select_the_scan_adapter(self):
        module, patched, fixups = self.fixture()
        rows = self.runtime.generate(module, patched, fixups).splitlines()
        writes = [row.split('\t') for row in rows if row.startswith('write\t')]
        self.assertEqual([row[1] for row in writes if '_full' not in row[1]], ['1440p', '2160p', '1800p', 'diag1792', '1800p', '2160p'])
        self.assertEqual([row[2] for row in writes if row[1] == '1800p'], ['33170c', '336a8c'])
        self.assertEqual([row[2] for row in writes if row[1] == '2160p'], ['33170c', '336a8c'])
        self.assertFalse(any(row[1] == 'default' for row in writes))
        self.assertTrue(all(row[2] == '33170c' for row in writes if row[1] not in ('1800p', '2160p') and '_full' not in row[1]))

    def test_missing_corrupt_or_outside_stub_is_rejected(self):
        module, patched, fixups = self.fixture()
        for address in ('-1', '0x100', hex(len(patched) - 4)):
            with self.assertRaisesRegex(ValueError, 'scan-source stub'):
                self.runtime.generate(module, patched, fixups | {'RUNTIME_SCAN_SOURCE_V1': address})
        stub = int(fixups['RUNTIME_SCAN_SOURCE_V1'], 0)
        patched[stub + 20] ^= 1
        with self.assertRaisesRegex(ValueError, 'scan-source stub'):
            self.runtime.generate(module, patched, fixups)

    def test_original_abi_and_rewritten_abi_must_both_match(self):
        module, patched, fixups = self.fixture()
        for off, _ in self.profile.scan_source_abi:
            module.image[off] ^= 1
            with self.assertRaisesRegex(ValueError, 'ABI mismatch'):
                self.runtime.generate(module, patched, fixups)
            module.image[off] ^= 1
            patched[off] ^= 1
            with self.assertRaisesRegex(ValueError, 'ABI mismatch'):
                self.runtime.generate(module, patched, fixups)
            patched[off] ^= 1

    def test_full_outputs_change_scene_canvas_and_display_together(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # pyright: ignore[reportMissingImports]
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate(module, patched, fixups).splitlines() if r.startswith('write\t')]
        for choice, _, w, h in self.runtime.FULL_CHOICES:
            writes = {int(r[2], 16): bytes.fromhex(r[4]) for r in rows if r[1] == choice}
            self.assertEqual(len(writes), 40 if choice.endswith('_1610') else 39)  # 16:10 also fits the native UI camera
            self.assertEqual(next(md.disasm(writes[0x80be90], 0)).op_str, f'w0, #{hex(w)}')
            self.assertEqual(next(md.disasm(writes[0x80be98], 0)).op_str, f'w0, #{hex(h)}')
            for a, b, r in self.runtime.OUTPUT_PAIRS:
                self.assertEqual(next(md.disasm(writes[a], 0)).op_str, f'x{r}, #{hex(w)}')
                self.assertEqual(next(md.disasm(writes[b], 0)).op_str, f'x{r}, #{hex(h)}, lsl #32')
            self.assertIn(0x33170c, writes); self.assertIn(0x336a8c, writes)
            self.assertFalse(any(0x336a90 <= off < 0x336ab0 for off in writes), 'native scan size guard remains intact')
        with self.assertRaises(ValueError): self.runtime.output_writes('2880p_full', 7680, 4320)

    def test_window_extent_stub_and_aspect_variant_preserve_square_pixels(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate(module, patched, fixups).splitlines() if r.startswith('write\t')]
        for choice, export, w, h in self.runtime.FULL_CHOICES:
            self.assertEqual(w * 9, h * 16, '16:10 must crop a genuine 16:9 render, not squeeze its camera')
            stub = int(fixups['RUNTIME_WINDOW_' + export], 0)
            words = struct.unpack_from('<4I', patched, stub)
            ins = list(md.disasm(patched[stub:stub + 16], stub))
            self.assertEqual([(i.mnemonic, i.op_str) for i in ins[:3]],
                             [('mov', f'x0, #{hex(w)}'), ('movk', f'x0, #{hex(h)}, lsl #32'), ('mov', 'x1, x0')])
            self.assertEqual(words[3], B(stub + 12, self.profile.window_extent_site + 4))
            write, = [row for row in rows if row[1] == choice and int(row[2], 16) == self.profile.window_extent_site]
            self.assertEqual(bytes.fromhex(write[4]), struct.pack('<I', B(self.profile.window_extent_site, stub)))
        for off, _ in ((self.profile.window_extent_site, self.profile.window_extent_original),) + self.profile.window_extent_abi:
            module.image[off] ^= 1
            with self.assertRaisesRegex(ValueError, 'window extent ABI'): self.runtime.generate(module, patched, fixups)
            module.image[off] ^= 1
            patched[off] ^= 1
            with self.assertRaisesRegex(ValueError, 'window extent ABI'): self.runtime.generate(module, patched, fixups)
            patched[off] ^= 1
        for addr in ('-1', '0x100', hex(len(patched) - 4)):
            with self.assertRaisesRegex(ValueError, 'window extent stub'):
                self.runtime.generate(module, patched, fixups | {'RUNTIME_WINDOW_SCENE_F2880_W': addr})
        stub = int(fixups['RUNTIME_WINDOW_SCENE_F2880_W'], 0)
        patched[stub + 8] ^= 1
        with self.assertRaisesRegex(ValueError, 'window extent stub'): self.runtime.generate(module, patched, fixups)

    def test_ui_camera_fit_is_only_selected_for_1610_and_rejects_bad_abi(self):
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate(module, patched, fixups).splitlines() if r.startswith('write\t')]
        writes = [r for r in rows if int(r[2], 16) == self.profile.ui_camera_site]
        self.assertEqual([r[1] for r in writes], [c for c, *_ in self.runtime.FULL_CHOICES if c.endswith('_1610')])
        stub = int(fixups['RUNTIME_UI_CAMERA_FIT_1610'], 0)
        self.assertTrue(all(bytes.fromhex(r[4]) == struct.pack('<I', B(self.profile.ui_camera_site, stub)) for r in writes))
        for off, _ in ((self.profile.ui_camera_site, self.profile.ui_camera_original),) + self.profile.ui_camera_abi:
            for image in (module.image, patched):
                image[off] ^= 1
                with self.assertRaisesRegex(ValueError, 'UI camera ABI'): self.runtime.generate(module, patched, fixups)
                image[off] ^= 1
        for addr in ('-1', '0x100', hex(len(patched) - 4), hex(stub + 1)):
            with self.assertRaisesRegex(ValueError, 'UI camera stub'):
                self.runtime.generate(module, patched, fixups | {'RUNTIME_UI_CAMERA_FIT_1610': addr})
        patched[stub + 8] ^= 1
        with self.assertRaisesRegex(ValueError, 'UI camera stub'): self.runtime.generate(module, patched, fixups)

    def test_ui_camera_bounds_do_not_touch_depth_or_world_projection(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        code = b''.join(struct.pack('<I', w) for w in self.profile.ui_camera_words(0))
        ins = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(code, 0))
        # The single descriptor store covers +c..+1b, leaving mode/aspect and
        # near/far (+1c/+20) unchanged. No call into the world/scan camera.
        self.assertEqual([(i.mnemonic, i.op_str) for i in ins if i.mnemonic == 'stur'], [('stur', 'q0, [x8, #0xc]')])
        self.assertFalse(any(i.mnemonic in ('bl', 'blr', 'b') for i in ins))
        self.assertEqual([(i.mnemonic, i.op_str) for i in ins[-3:]], [('mov', 'w9, #2'), ('add', 'sp, sp, #0x30'), ('ret', '')])
        self.assertEqual(ins[1].op_str, ins[-4].op_str)  # complete q0/q1 preservation
        self.assertEqual(ins[2].op_str, ins[-5].op_str)  # complete q2 preservation

    @unittest.skipUnless(sys.platform == 'darwin' and __import__('platform').machine() == 'arm64', 'native Apple Silicon test')
    def test_ui_camera_native_execution_preserves_registers_and_fits_edges(self):
        import subprocess
        # Only verified ISA instructions, no proprietary assets/executable.
        original = bytes.fromhex(self.profile.ui_camera_abi[0][1] + self.profile.ui_camera_original)
        code = bytearray(256)
        code[:len(original)] = original
        code[128:128 + len(original)] = original
        struct.pack_into('<I', code, 128 + len(original) - 4, B(128 + len(original) - 4, 256))
        code += b''.join(struct.pack('<I', w) for w in self.profile.ui_camera_words(256))
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'ui.bin'; binary.write_bytes(code)
            test = Path(tmp) / 'ui-test'
            subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', str(ROOT / 'dm/tools/tests/t_mp4_ui_camera.c'), '-o', str(test)], check=True, capture_output=True)
            result = subprocess.run([str(test), str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('native UICamera checks', result.stdout)

    def test_hud_layout_is_independent_default_off_and_strictly_validated(self):
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate_hud(module, patched, fixups).splitlines()]
        self.assertEqual(rows[0][3], 'DM_HUD_SCALE')
        self.assertEqual([r[1] for r in rows if r[0] == 'choice'], ['default', '85'])
        stub = int(fixups['RUNTIME_HUD_LAYOUT_V1'], 0)
        self.assertEqual([r for r in rows if r[0] == 'write'],
                         [['write', '85', '231240', self.profile.hud_layout_original,
                           struct.pack('<I', B(self.profile.hud_layout_site, stub)).hex()]])
        base = self.runtime.generate(module, patched, fixups)
        self.assertNotIn('231240', base)  # baseline/scene manifests remain independent
        abi = ((self.profile.hud_layout_site, self.profile.hud_layout_original),) + self.profile.hud_layout_abi + (self.profile.scan_source_abi[1],)
        for off, _ in abi:
            for image in (module.image, patched):
                image[off] ^= 1
                with self.assertRaisesRegex(ValueError, 'HUD layout ABI'):
                    self.runtime.generate_hud(module, patched, fixups)
                image[off] ^= 1
        for addr in ('-1', '0x100', hex(stub+1), hex(len(patched)-4)):
            with self.assertRaisesRegex(ValueError, 'HUD layout stub'):
                self.runtime.generate_hud(module, patched, fixups | {'RUNTIME_HUD_LAYOUT_V1': addr})
        patched[stub+8] ^= 1
        with self.assertRaisesRegex(ValueError, 'HUD layout stub'): self.runtime.generate_hud(module, patched, fixups)
        module.build_id = bytes(32)
        with self.assertRaisesRegex(ValueError, 'requires main v1.1.0'): self.runtime.generate_hud(module, patched, fixups)

    def test_hud_layout_stub_preserves_registers_and_native_constructor_arguments(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        start = 0x2000000
        code = b''.join(struct.pack('<I', w) for w in self.profile.hud_layout_words(start))
        code_end = code.index(self.profile.hud_layout_extension)
        ins = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(code[:code_end], start))
        text = [(i.mnemonic, i.op_str) for i in ins]
        self.assertEqual(text[:2], [('add', 'x2, sp, #4'), ('sub', 'sp, sp, #0x2b0')])
        self.assertIn(('ldr', 'x0, [x19, #0x58]'), text)
        self.assertIn(('ldr', 'x1, [sp, #0x10]'), text)
        self.assertIn(('mrs', 'x17, nzcv'), text); self.assertIn(('msr', 'nzcv, x17'), text)
        self.assertEqual([i.op_str for i in ins if i.mnemonic == 'stp'], [i.op_str for i in ins if i.mnemonic == 'ldp'])
        self.assertEqual([i.op_str for i in ins if i.mnemonic == 'str' and i.op_str.startswith('q')], [i.op_str for i in ins if i.mnemonic == 'ldr' and i.op_str.startswith('q')])
        self.assertEqual(text[-2:], [('add', 'sp, sp, #0x2b0'), ('b', '#0x231244')])
        restore = next(i.address for i in ins if (i.mnemonic, i.op_str) == ('ldr', 'q0, [sp, #0xb0]'))
        self.assertIn(('cbz', f'x0, #{hex(restore)}'), text)

    @unittest.skipUnless(sys.platform == 'darwin' and __import__('platform').machine() == 'arm64', 'native Apple Silicon test')
    def test_hud_bridge_native_default_missing_and_enabled(self):
        import subprocess
        _, patched, fixups = self.fixture()
        # Replace native constructor continuation with RET and use a mock
        # bootstrap. Execute only our bridge; no assets, render loop or game.
        struct.pack_into('<I', patched, self.profile.hud_layout_site+4, 0xd65f03c0)
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp)/'bridge.bin'; binary.write_bytes(patched)
            # Manifest must still validate against the unmodified fixture.
            module, unmodified, _ = self.fixture()
            manifest = Path(tmp)/'hud.runtime.tsv'
            manifest.write_text(self.runtime.generate_hud(module, unmodified, fixups))
            test = Path(tmp)/'bridge-test'
            subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT/'dm/tools/tests/t_mp4_hud_bridge.c'), '-o', str(test)], check=True, capture_output=True)
            result = subprocess.run([str(test), str(binary), str(manifest)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('native HUD bridge checks', result.stdout)

    def test_hud_layout_cpu_policy(self):
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / 'hud-test'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(ROOT / 'dm/tools/tests/t_mp4_hud_layout.c'), '-lm', '-o', str(test)], check=True, capture_output=True)
            result = subprocess.run([str(test)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('HUD layout checks', result.stdout)

    def test_pause_ui_is_independent_default_off_and_validates_both_paths(self):
        module, patched, fixups=self.fixture()
        rows=[r.split('\t') for r in self.runtime.generate_pause(module,patched,fixups).splitlines()]
        self.assertEqual(rows[0][3],'DM_MENU_FIT')
        self.assertEqual([r[1] for r in rows if r[0]=='choice'],['default','1610'])
        self.assertEqual([r[2] for r in rows if r[0]=='write'],['916c0c']+[f'{off:x}' for off,_,_,_ in self.profile.menu_visual_hooks()])
        for off,_ in self.profile.pause_ui_sites+self.profile.pause_ui_abi+tuple((a,h) for a,h,_,_ in self.profile.menu_visual_hooks())+self.profile.menu_visual_abi()+(self.profile.scan_source_abi[1],):
            for image in (module.image,patched):
                image[off]^=1
                with self.assertRaisesRegex(ValueError,'pause UI ABI'):self.runtime.generate_pause(module,patched,fixups)
                image[off]^=1
        for kind in ('BOUNDS',):
            key='RUNTIME_PAUSE_UI_'+kind;stub=int(fixups[key],0)
            for addr in ('-1','0x100',hex(stub+1),hex(len(patched)-4)):
                with self.assertRaisesRegex(ValueError,'pause UI stub'):self.runtime.generate_pause(module,patched,fixups|{key:addr})
            patched[stub+8]^=1
            with self.assertRaisesRegex(ValueError,'pause UI stub'):self.runtime.generate_pause(module,patched,fixups)
            patched[stub+8]^=1
        module.build_id=bytes(32)
        with self.assertRaisesRegex(ValueError,'requires main v1.1.0'):self.runtime.generate_pause(module,patched,fixups)

    def test_menu_visual_guards_policy_and_native_bridges(self):
        import subprocess
        from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
        module,patched,fixups=self.fixture()
        manifest=self.runtime.generate_pause(module,patched,fixups)
        for index,(off,h,event,argument) in enumerate(self.profile.menu_visual_hooks()):
            key='RUNTIME_MENU_VISUAL_'+str(index);stub=int(fixups[key],0)
            for addr in ('-1',hex(stub+1),hex(len(patched)-4)):
                with self.assertRaisesRegex(ValueError,'pause UI stub'):self.runtime.generate_pause(module,patched,fixups|{key:addr})
            patched[stub+12]^=1
            with self.assertRaisesRegex(ValueError,'pause UI stub'):self.runtime.generate_pause(module,patched,fixups)
            patched[stub+12]^=1
            code=b''.join(struct.pack('<I',w) for w in self.profile.menu_visual_words(stub,index))
            end=code.index(self.profile.menu_visual_extension)
            ins=list(Cs(CS_ARCH_ARM64,CS_MODE_ARM).disasm(code[:end],stub));text=[(i.mnemonic,i.op_str) for i in ins]
            self.assertIn(('mrs','x17, nzcv'),text);self.assertIn(('msr','nzcv, x17'),text)
            self.assertEqual([i.op_str for i in ins if i.mnemonic=='str' and i.op_str.startswith('q')],[i.op_str for i in ins if i.mnemonic=='ldr' and i.op_str.startswith('q')])
            self.assertEqual(len([i for i in ins if i.mnemonic=='str' and i.op_str.startswith('q')]),32)
            self.assertEqual(text[-1],('b',f'#{off+4:#x}'))
            if argument.startswith('inline'):
                self.assertIn(('ldr','x1, [sp, #0x40]'),text)
                self.assertIn(('ldr','x0, [sp]') if argument=='inline_original' else ('mov','x0, x19'),text)
                self.assertEqual(text[-2],('ldrb','w9, [x0, #0x96]' if argument=='inline_original' else 'w9, [x19, #0x96]'))
        for off,h,event,argument in self.profile.menu_visual_hooks():
            if event in (1,5):
                struct.pack_into('<II',patched,off+4,0xa8c27bfd,0xd65f03c0) # balance displaced STP, return
            else:struct.pack_into('<I',patched,off+4,0xd65f03c0)
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'visual-policy'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(ROOT/'dm/tools/tests/t_mp4_menu_visual.c'),'-o',str(binary)],check=True,capture_output=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            if sys.platform=='darwin' and __import__('platform').machine()=='arm64':
                image=Path(tmp)/'visual.bin';image.write_bytes(patched)
                mf=Path(tmp)/'visual.runtime.tsv';mf.write_text(manifest)
                sites=Path(tmp)/'sites.txt';sites.write_text('\n'.join(hex(off) for off,_,_,_ in self.profile.menu_visual_hooks()))
                native=Path(tmp)/'visual-native'
                subprocess.run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',str(ROOT/'dm/tools/tests/t_mp4_menu_visual_bridge.c'),'-o',str(native)],check=True,capture_output=True)
                result=subprocess.run([str(native),str(image),str(mf),str(sites)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn('native menu visual bridge checks',result.stdout)

    def test_menu_trace_is_separate_default_off_and_checks_exact_native_transform_abi(self):
        module,patched,fixups=self.fixture()
        rows=[r.split('\t') for r in self.runtime.generate_menu_trace(module,patched,fixups).splitlines()]
        self.assertEqual(rows[0][3],'DM_MENU_TRACE')
        self.assertEqual([r[1] for r in rows if r[0]=='choice'],['default','1'])
        self.assertEqual([r[2] for r in rows if r[0]=='write'],['101fc14'])
        for off,_ in (self.profile.pause_panel_site,)+self.profile.pause_panel_abi+(self.profile.scan_source_abi[1],):
            for image in (module.image,patched):
                image[off]^=1
                with self.assertRaisesRegex(ValueError,'menu trace ABI'):self.runtime.generate_menu_trace(module,patched,fixups)
                image[off]^=1
        stub=int(fixups['RUNTIME_PAUSE_UI_PANEL'],0)
        for value in ('-1',hex(stub+1),hex(len(patched))):
            with self.assertRaisesRegex(ValueError,'menu trace stub'):self.runtime.generate_menu_trace(module,patched,fixups|{'RUNTIME_PAUSE_UI_PANEL':value})
        patched[stub]^=1
        with self.assertRaisesRegex(ValueError,'menu trace stub'):self.runtime.generate_menu_trace(module,patched,fixups)
        module.build_id=bytes(32)
        with self.assertRaisesRegex(ValueError,'main v1.1.0'):self.runtime.generate_menu_trace(module,patched,fixups)
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'menu-trace'
            subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(ROOT/'dm/tools/tests/t_mp4_menu_trace.c'),'-o',str(binary)],check=True,capture_output=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('all source bytes unchanged',result.stdout)

    def test_pause_ui_policy_and_bridge_instruction_contracts(self):
        import subprocess
        from capstone import Cs,CS_ARCH_ARM64,CS_MODE_ARM
        for kind in ('bounds','fov','panel'):
            code=b''.join(struct.pack('<I',w) for w in self.profile.pause_ui_words(0x2000000,kind))
            end=code.index(self.profile.pause_ui_extension)
            ins=list(Cs(CS_ARCH_ARM64,CS_MODE_ARM).disasm(code[:end],0x2000000))
            text=[(i.mnemonic,i.op_str) for i in ins]
            self.assertIn(('mrs','x17, nzcv'),text);self.assertIn(('msr','nzcv, x17'),text)
            self.assertEqual([i.op_str for i in ins if i.mnemonic=='str' and i.op_str.startswith('q')], [i.op_str for i in ins if i.mnemonic=='ldr' and i.op_str.startswith('q')])
            if kind=='bounds':self.assertEqual(text[-2:],[('add','x0, x19, #0x8c'),('b','#0x916c10')])
            elif kind=='panel':
                self.assertIn(('mov','x0, x19'),text)
                self.assertIn(('ldr','x1, [sp, #0x40]'),text)
                self.assertEqual(text[-2:],[('ldrb','w8, [x19, #0x2fb]'),('b','#0x101fc18')])
            else:
                self.assertEqual(text[0],('strb','wzr, [sp, #0x32c]'))
                self.assertIn(('mov','x0, x21'),text)
                self.assertIn(('sub','x1, x29, #0xe8'),text)
                self.assertEqual(text[-1],('b','#0x3fd50'))
        with tempfile.TemporaryDirectory() as tmp:
            test=Path(tmp)/'pause-test'
            subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(ROOT/'dm/tools/tests/t_mp4_pause_ui.c'),'-lm','-o',str(test)],check=True,capture_output=True)
            result=subprocess.run([str(test)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('pause UI policy checks',result.stdout)

    @unittest.skipUnless(sys.platform=='darwin' and __import__('platform').machine()=='arm64','native Apple Silicon test')
    def test_pause_native_bridges_default_scope_missing_and_enabled(self):
        import subprocess
        module,patched,fixups=self.fixture()
        manifest=self.runtime.generate_pause(module,patched,fixups)
        trace_manifest=self.runtime.generate_menu_trace(module,patched,fixups)
        struct.pack_into('<I',patched,0x101fc18,0xd65f03c0)
        # Stop after the borrowed-record site; no camera construction/game runs.
        struct.pack_into('<I',patched,0x3fd50,0xd65f03c0)
        struct.pack_into('<I',patched,0x916c10,0xd65f03c0)
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'pause.bin';binary.write_bytes(patched)
            mf=Path(tmp)/'pause.runtime.tsv';mf.write_text(manifest)
            tf=Path(tmp)/'trace.runtime.tsv';tf.write_text(trace_manifest)
            test=Path(tmp)/'pause-bridge'
            subprocess.run(['xcrun','clang','-O2','-Wall','-Wextra','-Werror',str(ROOT/'dm/tools/tests/t_mp4_pause_bridge.c'),'-o',str(test)],check=True,capture_output=True)
            result=subprocess.run([str(test),str(binary),str(mf),str(tf)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('native pause bridge checks',result.stdout)

    def test_scene_control_preserves_registers_flags_and_uses_original_stack_input(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # pyright: ignore[reportMissingImports]
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        words = self.profile.scene_control_words(0x2000000, 5120, 2880)
        code = b''.join(struct.pack('<I', w) for w in words)
        ins = list(md.disasm(code, 0x2000000))
        text = [(i.mnemonic, i.op_str) for i in ins]
        self.assertEqual(text[0], ('orr', 'x21, x9, x8, lsl #32'))
        self.assertIn(('ldr', 'w2, [sp, #0x2bc]'), text); self.assertIn(('ldr', 'w3, [sp, #0x2b8]'), text)
        self.assertIn(('mrs', 'x17, nzcv'), text); self.assertIn(('msr', 'nzcv, x17'), text)
        self.assertEqual([i.op_str for i in ins if i.mnemonic == 'stp'], [i.op_str for i in ins if i.mnemonic == 'ldp'])
        self.assertEqual([i.op_str for i in ins if i.mnemonic == 'str' and i.op_str.startswith('q')], [i.op_str for i in ins if i.mnemonic == 'ldr' and i.op_str.startswith('q')])
        self.assertIn(('b', '#0x331710'), text)
        self.assertIn(self.profile.scene_control_extension, code)

    def test_output_abi_and_changed_instructions_reject_corruption(self):
        module, patched, fixups = self.fixture()
        for off, before in self.runtime.OUTPUT_ABI:
            patched[off] ^= 1
            with self.assertRaisesRegex(ValueError, 'output ABI'): self.runtime.generate(module, patched, fixups)
            patched[off] ^= 1
        patched[0x80be90] ^= 1
        with self.assertRaisesRegex(ValueError, 'runtime site was rewritten'): self.runtime.generate(module, patched, fixups)

    def test_launcher_offers_only_matching_full_manifest_and_keeps_stable_default(self):
        sys.path.insert(0, str(ROOT / 'dm/games'))
        import settings
        module, patched, fixups = self.fixture()
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'main.fixups.tsv').write_text(''.join(f'# {k}\t{v}\n' for k, v in fixups.items()))
            image = folder / 'main.patched.bin'; image.write_bytes(patched)
            (folder / 'main.runtime.tsv').write_text(self.runtime.generate(module, patched, fixups))
            with mock.patch.dict(os.environ, {'SETTINGS_PLATFORM': 'mac'}), mock.patch.object(settings.nacp, 'languages', return_value=['en-US']):
                listed = settings.build('010019A01E2F2000', tmp, 'unused')['settings']
                row = next(r for r in listed if r['env'] == 'DM_INTERNAL_RESOLUTION')
                self.assertEqual(row['default'], '1800p')
                self.assertEqual([r[0] for r in row['options']], ['default', 'native_full', '1080p_full', '1440p_full', '1800p', '2160p_full', '2880p_full'])
                patched[0x80be90] ^= 1; image.write_bytes(patched)
                with self.assertRaisesRegex(ValueError, 'instructions differ'): settings.build('010019A01E2F2000', tmp, 'unused')

    def test_shadow_2x_and_4x_scale_tiles_atlas_packer_and_uv_together(self):
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate_shadow(module, patched, fixups).splitlines()]
        self.assertEqual(rows[0][3], 'DM_SHADOW_RESOLUTION')
        self.assertEqual([r[1] for r in rows if r[0] == 'choice'], ['default', '2x', '4x'])
        movz_w = lambda w: (w & 0xffe00000 == 0x52800000, (w >> 5) & 0xffff, w & 31)
        for choice, k, atlas, uv, hi in (('2x', 2, 0x1000, 'SHADOW_UV4096', 0x4580), ('4x', 4, 0x2000, 'SHADOW_UV8192', 0x4600)):
            writes = {int(r[2], 16): (int.from_bytes(bytes.fromhex(r[3]), 'little'), int.from_bytes(bytes.fromhex(r[4]), 'little')) for r in rows if r[0] == 'write' and r[1] == choice}
            self.assertEqual(len(writes), 11)
            self.assertEqual(movz_w(writes[0x137a6f0][1]), (True, k, 28))        # tile = k << level
            self.assertEqual(movz_w(writes[0x137a82c][1]), (True, atlas, 8))     # packer atlas size
            self.assertEqual(movz_w(writes[0x137a840][1]), (True, atlas, 13))
            cmp = writes[0x137a850][1]                                           # cmp w13,#atlas (lsl #12)
            self.assertEqual((cmp & 0xff800000, (cmp >> 22) & 1, ((cmp >> 10) & 0xfff) << 12, (cmp >> 5) & 31, cmp & 31), (0x71000000, 1, atlas, 13, 31))
            for off in (0xa1e8bc, 0xda3aa0):                                     # movz x,#atlas
                w = writes[off][1]; self.assertEqual((w & 0xffe00000, (w >> 5) & 0xffff), (0xd2800000, atlas))
            for off in (0xa1e8c0, 0xda3a98):                                     # movk x,#atlas,lsl #32
                w = writes[off][1]; self.assertEqual((w & 0xffe00000, (w >> 5) & 0xffff), (0xf2c00000, atlas))
            for off in (0xa1e8b8, 0xda3a94):                                     # orr x,xzr,#atlas | atlas << 32: the single bit moved up by log2(atlas / 2048)
                old, new = writes[off]
                self.assertEqual(new & ~(0x3f << 16), old & ~(0x3f << 16)); self.assertEqual(((old >> 16) & 0x3f) - ((new >> 16) & 0x3f), k // 2 if k == 2 else 2)
            stub = int(fixups['RUNTIME_' + uv], 16)
            self.assertEqual(writes[0x137abf4], (0xfd429900, B(0x137abf4, stub)))
            words = struct.unpack_from('<4I', patched, stub)
            self.assertEqual(words[1:3], (0xf2e7f008, 0x9e670100)); self.assertEqual((words[0] & 0xffe0001f, (words[0] >> 5) & 0xffff), (0xd2a00008, hi))
            self.assertEqual(struct.unpack('<2f', struct.pack('<Q', hi << 16 | 0x3f80 << 48)), (float(2048 * k), 1.0))
            self.assertEqual(words[3], B(stub + 12, 0x137abf8))
        patched[int(fixups['RUNTIME_SHADOW_UV8192'], 16) + 4] ^= 1
        with self.assertRaisesRegex(ValueError, 'mismatched rewritten shadow stub for 4x'): self.runtime.generate_shadow(module, patched, fixups)

    def test_model_lod_high_reads_level_zero_without_cross_fade(self):
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate_lod(module, patched, fixups).splitlines()]
        self.assertEqual(rows[0][3], 'DM_MODEL_LOD')
        writes = {int(r[2], 16): (int.from_bytes(bytes.fromhex(r[3]), 'little'), int.from_bytes(bytes.fromhex(r[4]), 'little')) for r in rows if r[0] == 'write'}
        ldrb = lambda w: (w & 0xffc00000 == 0x39400000, (w >> 10) & 0xfff, (w >> 5) & 31, w & 31)   # ldrb wt,[xn,#imm]
        self.assertEqual(ldrb(writes[0x6a670][0]), (True, 2, 9, 8))   # the transition byte
        self.assertEqual(ldrb(writes[0x6a678][0]), (True, 1, 9, 8))   # the LOD byte
        for off in (0x6a670, 0x6a678): self.assertEqual(writes[off][1], 0x52800008)   # mov w8,#0
        for choice, export, scale in (('x2', 'LOD_DISTANCE_X2', 0.5), ('x4', 'LOD_DISTANCE_X4', 0.25)):
            stub = int(fixups['RUNTIME_' + export], 16)
            (off, (old, new)), = [(int(r[2], 16), (int.from_bytes(bytes.fromhex(r[3]), 'little'), int.from_bytes(bytes.fromhex(r[4]), 'little'))) for r in rows if r[0] == 'write' and r[1] == choice]
            self.assertEqual((off, old, new), (0x13a268, 0xbd43c920, B(0x13a268, stub)))   # ldr s0,[x9,#0x3c8] -> stub
            words = struct.unpack_from('<4I', patched, stub)
            self.assertEqual((words[0], words[2], words[3]), (0xbd43c920, 0x1e220800, B(stub + 12, 0x13a26c)))   # the load, fmul s0,s0,s2, back
            fmov = words[1]   # fmov s2,#imm8: VFPExpandImm(imm8) for single precision
            self.assertEqual((fmov & 0xffe01fff, fmov & 31), (0x1e201000 | 2, 2))
            imm8 = (fmov >> 13) & 0xff; sign, exp, frac = imm8 >> 7, imm8 >> 4 & 7, imm8 & 15
            value = (-1) ** sign * (16 + frac) / 16 * 2.0 ** ((exp ^ 4) - 3 if exp & 4 else exp + 1)
            self.assertEqual(value, scale)
        self.assertEqual([r[1] for r in rows if r[0] == 'choice'], ['default', 'high', 'x2', 'x4'])
        patched[0x6a678] ^= 1
        with self.assertRaises(ValueError): self.runtime.generate_lod(module, patched, fixups)

    def test_fog_high_doubles_the_grid_scale_it_loads(self):
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate_fog(module, patched, fixups).splitlines()]
        self.assertEqual(rows[0][3], 'DM_FOG_QUALITY')
        self.assertEqual([r[1] for r in rows if r[0] == 'choice'], ['default', 'high'])
        stub = int(fixups['RUNTIME_FOG_SCALE_X2'], 16)
        (off, old, new), = [(int(r[2], 16), int.from_bytes(bytes.fromhex(r[3]), 'little'), int.from_bytes(bytes.fromhex(r[4]), 'little')) for r in rows if r[0] == 'write']
        ldr = lambda w: (w & 0xffc00000 == 0xbd400000, ((w >> 10) & 0xfff) * 4, (w >> 5) & 31, w & 31)   # ldr st,[xn,#imm]
        self.assertEqual((off, ldr(old), new), (0xf54720, (True, 0x1a0, 20, 0), B(0xf54720, stub)))
        words = struct.unpack_from('<4I', patched, stub)
        self.assertEqual((words[0], words[3]), (old, B(stub + 12, 0xf54724)))   # the displaced load, then back
        self.assertEqual((words[1] & 0xffe01fff, (words[1] >> 13) & 0xff), (0x1e201001, 0))   # fmov s1,#imm8 with imm8 0: 2.0
        self.assertEqual(words[2], 0x1e200800 | 1 << 16)   # fmul s0,s0,s1
        patched[stub + 8] ^= 1
        with self.assertRaisesRegex(ValueError, 'mismatched rewritten fog stub'): self.runtime.generate_fog(module, patched, fixups)

    def test_launcher_reads_the_shadow_choice_from_its_own_manifest(self):
        sys.path.insert(0, str(ROOT / 'dm/games'))
        import settings
        module, patched, fixups = self.fixture()
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'main.fixups.tsv').write_text(''.join(f'# {k}\t{v}\n' for k, v in fixups.items()))
            (folder / 'main.patched.bin').write_bytes(patched)
            (folder / 'main.runtime.tsv').write_text(self.runtime.generate(module, patched, fixups))
            self.assertEqual(runtime_patch.choices(folder, 'DM_SHADOW_RESOLUTION'), ())
            (folder / 'main.shadow.runtime.tsv').write_text(self.runtime.generate_shadow(module, patched, fixups))
            self.assertEqual(runtime_patch.choices(folder, 'DM_SHADOW_RESOLUTION'), ('default', '2x', '4x'))
            (folder / 'main.lod.runtime.tsv').write_text(self.runtime.generate_lod(module, patched, fixups))
            self.assertEqual(runtime_patch.choices(folder, 'DM_MODEL_LOD'), ('default', 'high', 'x2', 'x4'))
            (folder / 'main.fog.runtime.tsv').write_text(self.runtime.generate_fog(module, patched, fixups))
            self.assertEqual(runtime_patch.choices(folder, 'DM_FOG_QUALITY'), ('default', 'high'))
            self.assertIn('1800p', runtime_patch.choices(folder, 'DM_INTERNAL_RESOLUTION'))
            self.assertEqual(runtime_patch.choices(folder, 'DM_SCREEN_RATIO'), ('default', '1610'))   # the variant variable: the 16:10 suffix of the full choices
            self.assertIn('2160p_full_1610', runtime_patch.choices(folder, 'DM_INTERNAL_RESOLUTION'))
            with mock.patch.dict(os.environ, {'SETTINGS_PLATFORM': 'mac'}), mock.patch.object(settings.nacp, 'languages', return_value=['en-US']):
                row = next(r for r in settings.build('010019A01E2F2000', tmp, 'unused')['settings'] if r['env'] == 'DM_SHADOW_RESOLUTION')
            self.assertEqual(([o[0] for o in row['options']], row['default']), (['default', '2x', '4x'], 'default'))

    def test_bridge_spills_and_returns_with_only_x28_and_displaced_w8_changed(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # pyright: ignore[reportMissingImports]  # binary-analysis test dependency in the tool venv
        base = 0x2000000
        code = b''.join(struct.pack('<I', w) for w in self.profile.scan_source_words(base))
        ins = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(code, base))
        # Independent disassembly verifies register coverage, stack alignment,
        # branch targets and the private lookup's literal; no game call executes.
        integer_stores = [i.op_str for i in ins if i.mnemonic == 'stp']
        integer_loads = [i.op_str for i in ins if i.mnemonic == 'ldp']
        self.assertEqual(len(integer_stores), 9)
        self.assertEqual(integer_stores, integer_loads)
        self.assertEqual([i.op_str for i in ins if i.mnemonic == 'str' and i.op_str.startswith('q')],
                         [i.op_str for i in ins if i.mnemonic == 'ldr' and i.op_str.startswith('q')])
        self.assertEqual(sum(i.mnemonic == 'str' and i.op_str.startswith('q') for i in ins), 32)
        self.assertEqual(ins[0].op_str, 'sp, sp, #0x2a0')
        self.assertTrue(any(i.mnemonic == 'add' and i.op_str == 'sp, sp, #0x2a0' for i in ins))
        self.assertTrue(any(i.mnemonic == 'bl' and i.op_str == '#0x138df60' for i in ins))
        self.assertTrue(any(i.mnemonic == 'b' and i.op_str == '#0x336a90' for i in ins))
        self.assertFalse(any(i.mnemonic in ('svc', 'msr', 'mrs') or re.search(r'\bx18\b', i.op_str) for i in ins[:100]))
        self.assertIn(self.profile.scan_extension, code)


if __name__ == '__main__':
    unittest.main()
