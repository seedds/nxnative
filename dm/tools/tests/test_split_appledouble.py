"""External shader directories ignore AppleDouble metadata, with no game files."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


def shader_file(kind, marker):
    ctl, code = bytearray(0x880), bytearray(0x100)
    ctl[:4] = bytes.fromhex('34127698')
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    code[:4] = bytes.fromhex('78563412')
    struct.pack_into('<I', code, 0x30, 1 << 10 | 1)  # vertex SPH
    code[0x80] = marker
    if kind == 'glslc':
        data = bytearray(0x70) + ctl + code
        data[:4] = bytes.fromhex('91688619')
        struct.pack_into('<I', data, 0x44, len(data))
        return data
    record = b'LSLGSV' + bytes(2) + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl
    # Version 2, no map/hash tables, one uncompressed vertex shader, no preloads or indices.
    return struct.pack('<5I', 2, 0, 0, 0, 1) + struct.pack('<QIIB', 0, len(record), len(record), 0) + bytes(8) + record


class ExternalShaders(unittest.TestCase):
    def split(self, root, env):
        out = root / 'out'; out.mkdir(exist_ok=True)
        paths = [out / name for name in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
        result = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', root / 'RomFS', root / 'main', *paths],
                                env=env, capture_output=True, text=True)
        files = {str(p.relative_to(out)): p.read_bytes() for p in out.rglob('*') if p.is_file()}
        return result, files

    def fixture(self, root, kind, override=False):
        (root / 'RomFS').mkdir()
        (root / 'main').write_bytes(b'NSO0' + bytes(0xfc))  # three empty NSO segments
        folder, key = ('glslc_capture', 'DM_GLSLC_CAPTURE') if kind == 'glslc' else ('ue_shaders', 'DM_UE_SHADERS')
        source = root / ('external' if override else folder); source.mkdir()
        env = dict(os.environ, DM_GAME='01007BB017812000', J='1')
        for name in ('DM_GLSLC_CAPTURE', 'DM_UE_SHADERS', 'DM_GAMES_DIR'): env.pop(name, None)
        if override: env[key] = str(source)
        return source, env

    def test_sidecars_do_not_change_split_outputs(self):
        for kind in ('glslc', 'ushaderbytecode'):
            for override in (False, True):
                with self.subTest(kind=kind, override=override), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp); source, env = self.fixture(root, kind, override)
                    # A leading dot alone can be a real input; only AppleDouble's ._ prefix is metadata.
                    for marker, name in enumerate(('shader', '.hidden'), 1):
                        (source / f'{name}.{kind}').write_bytes(shader_file(kind, marker))
                    before, expected = self.split(root, env)
                    self.assertEqual(before.returncode, 0, before.stderr)
                    self.assertIn('shaders: 2 (', before.stdout)
                    self.assertIn(b'.hidden.', expected['map.tsv'])
                    for name in ('shader', '.hidden', 'orphan'):
                        (source / f'._{name}.{kind}').write_bytes(bytes.fromhex('00051607') + bytes(64))
                    after, actual = self.split(root, env)
                    self.assertEqual(after.returncode, 0, after.stderr)
                    self.assertEqual(actual, expected)

    def test_malformed_real_inputs_still_fail(self):
        for kind, diagnostic in (('glslc', 'no GLSLC output'), ('ushaderbytecode', 'not a version 2 shader code archive')):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); source, env = self.fixture(root, kind)
                (source / f'bad.{kind}').write_bytes(bytes.fromhex('00051607') + bytes(64))
                result, _ = self.split(root, env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)


if __name__ == '__main__': unittest.main()
