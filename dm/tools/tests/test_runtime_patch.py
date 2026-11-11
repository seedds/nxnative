"""Build identity, rewrite-site conflicts, cache lifecycle and the original scene allocation ABI."""
import importlib.util
from pathlib import Path
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
        self.assertEqual(exports, {'SCENE_1440': base, 'SCENE_2160': base + 12, 'SCENE_1800': base + 24, 'SCENE_D1792': base + 36})
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


if __name__ == '__main__':
    unittest.main()
