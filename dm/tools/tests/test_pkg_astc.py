"""pkg_astc.py: Supergiant packages with their BC7 textures re-encoded as ASTC 4x4; synthetic packages, and one real encode when Xcode's TextureConverter is there."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import lz4.block

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
spec = importlib.util.spec_from_file_location('pkg_astc', REPO / 'dm/tools/pkg_astc.py')
assert spec is not None and spec.loader is not None
pkg = importlib.util.module_from_spec(spec); spec.loader.exec_module(pkg)


def texture(fmt, w, h, data, mips=1, platform=b'w'):
    return b'XNB' + platform + b'\x06\x00' + struct.pack('<I', len(data) + 30) + struct.pack('<5I', fmt, w, h, mips, len(data)) + data


def package(*chunks, compressed=True):
    out = struct.pack('>I', 0x20000007 if compressed else 7)
    for c in chunks:
        z = lz4.block.compress(c, store_size=False)
        out += b'\x01' + struct.pack('>I', len(z)) + z
    return out


def chunks(data):
    out, pos = [], 4
    while pos < len(data):
        assert data[pos] == 1
        n = struct.unpack('>I', data[pos + 1:pos + 5])[0]
        out.append(lz4.block.decompress(data[pos + 5:pos + 5 + n], uncompressed_size=0x2000000)); pos += 5 + n
    return out


class Packages(unittest.TestCase):
    def test_only_bc7_data_is_replaced(self):
        bc7 = bytes(range(64))                      # 8x8: four 16-byte blocks
        rgba = b'\x11' * (4 * 4 * 4)
        c0 = b'\xad\x05bin\\a' + texture(28, 8, 8, bc7) + b'\xad\x05bin\\b' + texture(0, 4, 4, rgba) + b'\xff'
        c1 = b'\xad\x05bin\\c' + texture(28, 4, 8, bc7[:32]) + b'\xff'
        seen = []
        def convert(blocks, w, h):
            seen.append((w, h, len(blocks))); return blocks[::-1]
        out, n = pkg.convert_package(package(c0, c1), convert)
        self.assertEqual(n, 2)
        self.assertEqual(seen, [(8, 8, 64), (4, 8, 32)])
        self.assertEqual(chunks(out), [c0.replace(bc7, bc7[::-1]), c1.replace(bc7[:32], bc7[:32][::-1])])

    def test_mip_levels_are_converted_one_by_one(self):
        levels = [b'\x01' * 64, b'\x02' * 16, b'\x03' * 16]   # 8x8, 4x4, 2x2 (one block each from 4x4 down)
        c0 = texture(28, 8, 8, b''.join(levels), mips=3)
        seen = []
        def convert(blocks, w, h):
            seen.append((w, h, blocks)); return blocks
        pkg.convert_package(package(c0), convert)
        self.assertEqual(seen, [(8, 8, levels[0]), (4, 4, levels[1]), (2, 2, levels[2])])

    def test_left_alone(self):
        bad = texture(28, 8, 8, b'\0' * 48)                   # data size is not 8x8's four blocks
        bad = bad[:6] + struct.pack('<I', 48 + 30) + bad[10:]
        for data in (package(bad), package(texture(28, 8, 8, b'\0' * 64), compressed=False)):
            out, n = pkg.convert_package(data, lambda b, w, h: b'\xee' * len(b))
            self.assertEqual((out, n), (data, 0))

    def test_converter_must_keep_the_size(self):
        with self.assertRaises(ValueError):
            pkg.convert_package(package(texture(28, 4, 4, b'\0' * 16)), lambda b, w, h: b'\0' * 32)


@unittest.skipUnless(pkg.texture_converter(), 'needs Xcode\'s TextureConverter')
class Encoder(unittest.TestCase):
    def test_bc7_to_astc_keeps_the_picture(self):
        tc = pkg.texture_converter()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            w = h = 16
            px = bytes(v for y in range(h) for x in range(w) for v in (x * 16, y * 16, 200 - x * 8, 40 + y * 12))
            (tmp / 'src.tga').write_bytes(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28)
                                          + b''.join(px[i + 2:i + 3] + px[i + 1:i + 2] + px[i:i + 1] + px[i + 3:i + 4] for i in range(0, len(px), 4)))
            pkg.run([tc, '--compression_format=BC7', '--compressor=NVTT', '--max_mipmaps=1', '--alpha_mode=Preserve', '--file_format=KTX',
                     '--disable_annotation', f'--output={tmp / "bc7.ktx"}', str(tmp / 'src.tga')])
            bc7 = pkg.ktx_level(tmp / 'bc7.ktx')[1]
            astc = pkg.bc7_astc(bc7, w, h, tc, tmp / 'cache')
            self.assertEqual(len(astc), len(bc7)); self.assertNotEqual(astc, bc7)
            self.assertEqual(pkg.bc7_astc(bc7, w, h, tc, tmp / 'cache'), astc)   # from the cache
            def decoded(blocks, fmt, name):
                (tmp / f'{name}.ktx').write_bytes(pkg.ktx(blocks, w, h, fmt))
                pkg.run([tc, '--mode=decompress', '--file_format=KTX', f'--output={tmp / name}.out.ktx', str(tmp / f'{name}.ktx')])
                return pkg.ktx_level(tmp / f'{name}.out.ktx')[1]
            src, back = decoded(bc7, pkg.GL_BPTC, 'bc7'), decoded(astc, pkg.GL_ASTC_4x4, 'astc')
            # Each channel, alpha included, stays where it was: a swap or a dropped alpha is off by 50 and more. This ramp climbs 16 levels a pixel, far steeper than a game
            # texture (Hades' GUI atlas: about 1 RMS); the bound is for mistakes, not for the encoder's quality.
            for ch in range(4):
                d = [src[i] - back[i] for i in range(ch, len(src), 4)]
                self.assertLessEqual((sum(x * x for x in d) / len(d)) ** .5, 12, f'channel {ch}')


if __name__ == '__main__':
    unittest.main()
