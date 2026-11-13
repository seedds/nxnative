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
        original = bytearray(0x13c2000)  # include the verified RO format table
        original[self.profile.scene_site:self.profile.scene_site + 4] = bytes.fromhex('358108aa')
        for off, hexbytes in self.profile.SCAN_SOURCE_ABI + self.profile.SCENE_CONTROL_ABI:
            original[off:off + len(bytes.fromhex(hexbytes))] = bytes.fromhex(hexbytes)
        for off, before in self.runtime.OUTPUT_ABI:
            original[off:off + len(before)] = before
        for _, off, before, _ in self.runtime.output_writes('2880p_full', 5120, 2880):
            original[off:off + len(before)] = before
        for off, before, _ in self.runtime.SHADOW_SITES:
            struct.pack_into('<I', original, off, before)
        original[self.profile.SHADOW_UV_SITE:self.profile.SHADOW_UV_SITE + 4] = bytes.fromhex('009942fd')
        patched = bytearray(base + 8192)
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
        self.assertEqual([row[1] for row in writes if not row[1].endswith('_full')], ['1440p', '2160p', '1800p', 'diag1792', '1800p', '2160p'])
        self.assertEqual([row[2] for row in writes if row[1] == '1800p'], ['33170c', '336a8c'])
        self.assertEqual([row[2] for row in writes if row[1] == '2160p'], ['33170c', '336a8c'])
        self.assertFalse(any(row[1] == 'default' for row in writes))
        self.assertTrue(all(row[2] == '33170c' for row in writes if row[1] not in ('1800p', '2160p') and not row[1].endswith('_full')))

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
        for off, _ in self.profile.SCAN_SOURCE_ABI:
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
            self.assertEqual(len(writes), 38)  # scene, scan bridge, 30 canvas and six scalar sites
            self.assertEqual(next(md.disasm(writes[0x80be90], 0)).op_str, f'w0, #{hex(w)}')
            self.assertEqual(next(md.disasm(writes[0x80be98], 0)).op_str, f'w0, #{hex(h)}')
            for a, b, r in self.runtime.OUTPUT_PAIRS:
                self.assertEqual(next(md.disasm(writes[a], 0)).op_str, f'x{r}, #{hex(w)}')
                self.assertEqual(next(md.disasm(writes[b], 0)).op_str, f'x{r}, #{hex(h)}, lsl #32')
            self.assertIn(0x33170c, writes); self.assertIn(0x336a8c, writes)
            self.assertFalse(any(0x336a90 <= off < 0x336ab0 for off in writes), 'native scan size guard remains intact')
        with self.assertRaises(ValueError): self.runtime.output_writes('2880p_full', 7680, 4320)

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
        self.assertIn(self.profile.SCENE_CONTROL_EXTENSION, code)

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

    def test_shadow_2x_doubles_tiles_atlas_packer_and_uv_scale_together(self):
        module, patched, fixups = self.fixture()
        rows = [r.split('\t') for r in self.runtime.generate_shadow(module, patched, fixups).splitlines()]
        self.assertEqual(rows[0][3], 'DM_SHADOW_RESOLUTION')
        self.assertEqual([r[1] for r in rows if r[0] == 'choice'], ['default', '2x'])
        writes = {int(r[2], 16): (int.from_bytes(bytes.fromhex(r[3]), 'little'), int.from_bytes(bytes.fromhex(r[4]), 'little')) for r in rows if r[0] == 'write'}
        self.assertEqual(len(writes), 11)
        movz_w = lambda w: (w & 0xffe00000 == 0x52800000, (w >> 5) & 0xffff, w & 31)
        self.assertEqual(movz_w(writes[0x137a6f0][1]), (True, 2, 28))       # tile = 2 << k
        self.assertEqual(movz_w(writes[0x137a82c][1]), (True, 0x1000, 8))   # packer atlas width
        self.assertEqual(movz_w(writes[0x137a840][1]), (True, 0x1000, 13))
        cmp = writes[0x137a850][1]                                          # cmp w13,#1,lsl #12
        self.assertEqual((cmp & 0xff800000, (cmp >> 22) & 1, (cmp >> 10) & 0xfff, (cmp >> 5) & 31, cmp & 31), (0x71000000, 1, 1, 13, 31))
        for off in (0xa1e8bc, 0xda3aa0):                                    # movz x,#0x1000
            w = writes[off][1]; self.assertEqual((w & 0xffe00000, (w >> 5) & 0xffff), (0xd2800000, 0x1000))
        for off in (0xa1e8c0, 0xda3a98):                                    # movk x,#0x1000,lsl #32
            w = writes[off][1]; self.assertEqual((w & 0xffe00000, (w >> 5) & 0xffff), (0xf2c00000, 0x1000))
        for off in (0xa1e8b8, 0xda3a94):                                    # orr x,xzr,#0x0000100000001000: one bit per 32-bit element, at bit 12
            old, new = writes[off]
            self.assertEqual(new & ~(0x3f << 16), old & ~(0x3f << 16)); self.assertEqual(((old >> 16) & 0x3f) - ((new >> 16) & 0x3f), 1)
        stub = int(fixups['RUNTIME_SHADOW_UV4096'], 16)
        self.assertEqual(writes[0x137abf4], (0xfd429900, B(0x137abf4, stub)))
        words = struct.unpack_from('<4I', patched, stub)
        self.assertEqual(words[:3], (0xd2a8b008, 0xf2e7f008, 0x9e670100))  # x8 = 4096.0f | 1.0f << 32; fmov d0,x8
        self.assertEqual(struct.unpack('<2f', struct.pack('<Q', 0x4580 << 16 | 0x3f80 << 48)), (4096.0, 1.0))
        self.assertEqual(words[3], B(stub + 12, 0x137abf8))
        patched[stub + 4] ^= 1
        with self.assertRaisesRegex(ValueError, 'mismatched rewritten shadow stub'): self.runtime.generate_shadow(module, patched, fixups)

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
            self.assertEqual(runtime_patch.choices(folder, 'DM_SHADOW_RESOLUTION'), ('default', '2x'))
            self.assertIn('1800p', runtime_patch.choices(folder, 'DM_INTERNAL_RESOLUTION'))
            with mock.patch.dict(os.environ, {'SETTINGS_PLATFORM': 'mac'}), mock.patch.object(settings.nacp, 'languages', return_value=['en-US']):
                row = next(r for r in settings.build('010019A01E2F2000', tmp, 'unused')['settings'] if r['env'] == 'DM_SHADOW_RESOLUTION')
            self.assertEqual(([o[0] for o in row['options']], row['default']), (['default', '2x'], 'default'))

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
        self.assertIn(self.profile.SCAN_EXTENSION, code)


if __name__ == '__main__':
    unittest.main()
