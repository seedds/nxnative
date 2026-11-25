"""Regression tests for MGFX v10 and raw-DEFLATE .zxnb shader readers using synthetic inputs."""
import contextlib
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz

ss = fz.ss


def net_str(text: bytes) -> bytes:
    """Encode bytes with a .NET 7-bit length prefix."""
    n = len(text)
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | 0x80 if n else b)
        if not n:
            return bytes(out) + text


def raw_deflate(data: bytes) -> bytes:
    comp = zlib.compressobj(wbits=-zlib.MAX_WBITS)
    return comp.compress(data) + comp.flush()


def mgfx_v10_effect(profile: int = 0x28, cb_name: bytes = b'global') -> bytes:
    """Build a spec-accurate MonoGame MGFX v10 effect with one vertex and one pixel shader."""
    out = bytearray(b'MGFX')
    out += struct.pack('<BBI', 10, profile, 0x12345678)
    # 1 constant buffer: name, i16 size, i32 param_count=1, {i32 param=0, u16 offset=0}
    out += struct.pack('<i', 1)
    out += net_str(cb_name) + struct.pack('<hi', 16, 1) + struct.pack('<iH', 0, 0)
    # 2 shaders: vertex (1), pixel (0)
    out += struct.pack('<i', 2)
    for is_vs, (ctl, code) in ((1, fz.VS), (0, fz.PS)):
        bc = struct.pack('<I', len(ctl)) + ctl + struct.pack('<I', len(code)) + code
        out += struct.pack('<BI', is_vs, len(bc)) + bc
        out += bytes((0, 1, 0, 0))   # 0 samplers, 1 cbuffer index (0), 0 attributes
    # 1 parameter: class=0, type=3 (Single), name, semantic, 0 annotations, 1x4, 0 elements, 0 members, 4 floats
    out += struct.pack('<i', 1)
    out += bytes((0, 3)) + net_str(b'Tint') + net_str(b'') + struct.pack('<i', 0) + bytes((1, 4))
    out += struct.pack('<ii', 0, 0) + bytes(16)
    # 1 technique, 2 passes (one VS+PS, one PS-only with -1 sentinel)
    out += struct.pack('<i', 1) + net_str(b'Main') + struct.pack('<ii', 0, 2)
    out += net_str(b'P0') + struct.pack('<iii', 0, 0, 1) + bytes((0, 0, 0))
    out += net_str(b'P1') + struct.pack('<iii', 0, -1, 1) + bytes((0, 0, 0))
    out += b'MGFX'
    return bytes(out)


def mgfx_v8_effect() -> bytes:
    """Build a spec-accurate MonoGame MGFX v8 effect (u8 counts, 255 none sentinel, no MGFX tail)."""
    out = bytearray(b'MGFX')
    out += struct.pack('<BBI', 8, 1, 0x12345678)
    out += bytes((1,)) + net_str(b'global') + struct.pack('<hBBH', 16, 1, 0, 0)
    out += bytes((1,))
    ctl, code = fz.PS
    bc = struct.pack('<I', len(ctl)) + ctl + struct.pack('<I', len(code)) + code
    out += struct.pack('<BI', 0, len(bc)) + bc + bytes((0, 1, 0, 0))
    out += bytes((0, 1)) + net_str(b'Main') + bytes((0, 1)) + net_str(b'P0') + bytes((0, 255, 0, 0, 0, 0))
    return bytes(out)


def xnb_effect(effect: bytes) -> bytes:
    reader = b'Microsoft.Xna.Framework.Content.EffectReader, MonoGame.Framework'
    body = b'\x01' + net_str(reader) + bytes(5) + b'\x01' + struct.pack('<I', len(effect)) + effect
    return b'XNBS\x05\x01' + struct.pack('<I', 10 + len(body)) + body


class MgfxV10Readers(unittest.TestCase):
    def test_embedded_mgfx8_still_parses(self):
        rows = ss.mgfx_shaders(b'prefix' + mgfx_v8_effect() + b'suffix', 'main')
        self.assertEqual([row[1] for row in rows], ['ps'])
        self.assertEqual(rows[0][2:], fz.PS)

    def test_embedded_mgfx10_structural_and_profile_agnostic(self):
        # Include b'MGFX' inside a constant buffer name and a non-0x28 profile byte to confirm structural parsing.
        effect = mgfx_v10_effect(profile=0x29, cb_name=b'MGFX_cb')
        rows = ss.mgfx_shaders(b'prefix' + effect + b'suffix', 'main')
        self.assertEqual([row[1] for row in rows], ['vs', 'ps'])
        self.assertEqual(rows[0][2:], fz.VS)
        self.assertEqual(rows[1][2:], fz.PS)

    def test_embedded_mgfx10_missing_tail_reports_red(self):
        effect = mgfx_v10_effect()
        with self.assertRaises(SystemExit) as ctx:
            ss.mgfx_shaders(effect[:-4], 'main')
        self.assertIn('[RED]', str(ctx.exception))
        self.assertIn('trailing MGFX', str(ctx.exception))

    def test_deflated_zxnb_effect(self):
        xnb = xnb_effect(mgfx_v10_effect())
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(tmp) / 'Effect.zxnb'
            path.write_bytes(raw_deflate(xnb))
            _, shaders, _, extra = ss.scan_file((str(path), 'Content/Shaders/Effect.zxnb'))
        self.assertEqual({row[1] for row in shaders.values()}, {'vs', 'ps'})
        self.assertEqual(extra, {})
        self.assertEqual(log.getvalue(), '')

    def test_zxnb_skips_sound_effect_reader_and_non_effect_assets(self):
        sfx = b'XNBS\x05\x01' + struct.pack('<I', 0x20000) + b'\x01' + net_str(b'Microsoft.Xna.Framework.Content.SoundEffectReader') + bytes(0x20000)
        tex = b'XNBS\x05\x01' + struct.pack('<I', 0x20000) + b'\x01' + net_str(b'Microsoft.Xna.Framework.Content.Texture2DReader') + bytes(0x20000)
        for name, payload in (('LogoSFX.zxnb', sfx), ('Sprite.zxnb', tex)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
                path = Path(tmp) / name
                path.write_bytes(raw_deflate(payload))
                _, shaders, pairs, extra = ss.scan_file((str(path), f'Content/{name}'))
                self.assertEqual((shaders, pairs, extra, log.getvalue()), ({}, set(), {}, ''))

    def test_zxnb_damaged_effect_reports_unreadable(self):
        bad_tail = raw_deflate(xnb_effect(mgfx_v10_effect()[:-4]))
        truncated = raw_deflate(xnb_effect(mgfx_v10_effect()))[:-12]
        for label, payload in (('missing tail', bad_tail), ('truncated stream', truncated)):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
                path = Path(tmp) / 'Broken.zxnb'
                path.write_bytes(payload)
                _, shaders, pairs, extra = ss.scan_file((str(path), 'Content/Shaders/Broken.zxnb'))
                self.assertEqual((shaders, pairs), ({}, set()))
                self.assertIsInstance(extra, tuple)
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn('[RED]', log.getvalue())

    def test_non_deflate_zxnb_falls_through_cleanly(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(tmp) / 'Plain.zxnb'
            path.write_bytes(b'NOT_DEFLATE_DATA' * 16)
            _, shaders, pairs, extra = ss.scan_file((str(path), 'Content/Plain.zxnb'))
        self.assertEqual((shaders, pairs, extra, log.getvalue()), ({}, set(), {}, ''))

    def test_zxnb_effect_reader_past_512_bytes(self):
        # Multiple long type readers push .EffectReader well past the old 0x200-byte peek window.
        effect = mgfx_v10_effect()
        filler_readers = [
            f'Example.Game.Content.CustomReader{i}, Example.Game.Content, Version=1.0.0.0, Culture=neutral, PublicKeyToken=null'.encode()
            for i in range(6)
        ]
        effect_reader = b'Microsoft.Xna.Framework.Content.EffectReader, MonoGame.Framework'
        readers = filler_readers + [effect_reader]
        table = bytearray([len(readers)])
        for name in readers:
            table += net_str(name) + bytes(4)
        self.assertGreater(10 + len(table) - len(effect_reader) - 4, 512)
        body = bytes(table) + b'\x00' + bytes([len(readers)]) + struct.pack('<I', len(effect)) + effect
        xnb = b'XNBS\x05\x01' + struct.pack('<I', 10 + len(body)) + body
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            plain = Path(tmp) / 'Effect.xnb'
            packed = Path(tmp) / 'Effect.zxnb'
            plain.write_bytes(xnb)
            packed.write_bytes(raw_deflate(xnb))
            _, plain_shaders, _, plain_extra = ss.scan_file((str(plain), 'Content/Shaders/Effect.xnb'))
            _, zxnb_shaders, _, zxnb_extra = ss.scan_file((str(packed), 'Content/Shaders/Effect.zxnb'))
        self.assertEqual({row[1] for row in plain_shaders.values()}, {'vs', 'ps'})
        self.assertEqual({k: v[1:] for k, v in zxnb_shaders.items()}, {k: v[1:] for k, v in plain_shaders.items()})
        self.assertEqual((plain_extra, zxnb_extra, log.getvalue()), ({}, {}, ''))

    def test_zxnb_many_non_effect_readers_past_512_bytes_skipped(self):
        filler_readers = [
            f'Example.Game.Content.TextureReader{i}, Example.Game.Content, Version=1.0.0.0, Culture=neutral, PublicKeyToken=null'.encode()
            for i in range(8)
        ]
        table = bytearray([len(filler_readers)])
        for name in filler_readers:
            table += net_str(name) + bytes(4)
        body = bytes(table) + b'\x00' + bytes(0x10000)
        xnb = b'XNBS\x05\x01' + struct.pack('<I', 10 + len(body)) + body
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            packed = Path(tmp) / 'Sprite.zxnb'
            packed.write_bytes(raw_deflate(xnb))
            _, shaders, pairs, extra = ss.scan_file((str(packed), 'Content/Sprite.zxnb'))
        self.assertEqual((shaders, pairs, extra, log.getvalue()), ({}, set(), {}, ''))

    def test_zxnb_effect_reader_past_512_bytes_without_mgfx_reports_red(self):
        filler_readers = [
            f'Example.Game.Content.CustomReader{i}, Example.Game.Content, Version=1.0.0.0, Culture=neutral, PublicKeyToken=null'.encode()
            for i in range(6)
        ]
        readers = filler_readers + [b'Microsoft.Xna.Framework.Content.EffectReader, MonoGame.Framework']
        table = bytearray([len(readers)])
        for name in readers:
            table += net_str(name) + bytes(4)
        body = bytes(table) + b'\x00\x07\x00\x00\x00\x00'
        xnb = b'XNBS\x05\x01' + struct.pack('<I', 10 + len(body)) + body
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as log:
            packed = Path(tmp) / 'EmptyEffect.zxnb'
            packed.write_bytes(raw_deflate(xnb))
            _, shaders, pairs, extra = ss.scan_file((str(packed), 'Content/Shaders/EmptyEffect.zxnb'))
        self.assertEqual((shaders, pairs), ({}, set()))
        self.assertIsInstance(extra, tuple)
        self.assertEqual(extra[0], 'unreadable')
        self.assertIn('[RED]', log.getvalue())


if __name__ == '__main__':
    unittest.main()
