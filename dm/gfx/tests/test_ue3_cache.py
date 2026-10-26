"""Unreal Engine 3 shader caches in the shader split (shader_split.py ue3_chunks, ue3_package) and the profile key shader_key_skip (game.py), without game data."""
from pathlib import Path
import os
import struct
import sys
import types
import unittest
import zlib

os.environ.setdefault('DM_GAME', '010015100B514000')   # shader_split loads a profile at import; any does
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'games'))
import shader_split   # noqa: E402
import game   # noqa: E402

TAG = struct.pack('<I', 0x9E2A83C1)


def chunk(blocks, block_size=0x20000):   # one compressed chunk: header, one (compressed, raw) pair per block, then the blocks' data
    head = TAG + struct.pack('<III', block_size, sum(len(c) for c, _ in blocks), sum(u for _, u in blocks))
    return head + b''.join(struct.pack('<II', len(c), u) for c, u in blocks) + b''.join(c for c, _ in blocks)


class Package(unittest.TestCase):
    def test_zlib_and_stored_blocks_are_decoded_behind_the_summary(self):
        summary = TAG + b'\x01\x02\x03\x04' + b'summary bytes'   # the package's own tag and a summary stored as it is
        a, b, c = b'A' * 3000, bytes(range(256)) * 8, b'stored block'
        d = summary + chunk([(zlib.compress(a), len(a)), (zlib.compress(b), len(b))], block_size=4096) + chunk([(c, len(c))])   # 5,048 bytes in blocks of up to 4 KB: two
        chunks = shader_split.ue3_chunks(d)
        self.assertEqual([len(x) for x in chunks], [2, 1])
        front = d[:chunks[0][0][0]]   # everything before the first block's data stays as it is: the summary, and the first chunk's header (the records are found by their magic)
        self.assertTrue(front.startswith(summary))
        self.assertEqual(shader_split.ue3_package(d, 'test'), front + a + b + c)

    def test_a_package_stored_whole_is_returned_as_it_is(self):
        d = TAG + b'\0' * 64 + b'no chunk here'
        self.assertEqual(shader_split.ue3_chunks(d), [])
        self.assertEqual(shader_split.ue3_package(d, 'test'), d)

    def test_an_inconsistent_chunk_header_is_not_taken(self):
        bad = TAG + struct.pack('<III', 0x20000, 10, 100) + struct.pack('<II', 9, 100) + b'x' * 10   # the block sizes do not add up to the chunk's
        self.assertEqual(shader_split.ue3_chunks(TAG + b'summary' + bad), [])

    def test_an_unknown_block_format_leaves_the_package_out(self):
        raw = b'R' * 100
        d = TAG + b'summary' + chunk([(b'\x47\x00' + b'?' * 20, len(raw))])
        self.assertEqual(shader_split.ue3_package(d, 'test'), b'')


class KeySkip(unittest.TestCase):
    def profile(self, value):
        return types.SimpleNamespace(id='test', get=lambda k, default=None: value if k == 'shader_key_skip' else default)

    def test_values(self):
        self.assertEqual(game.shader_key_skip(self.profile('')), (0, 0))
        self.assertEqual(game.shader_key_skip(self.profile('0x770+0x78')), (0x770, 0x78))
        self.assertEqual(game.shader_key_skip(self.profile('1904+120')), (0x770, 0x78))

    def test_bad_values_stop(self):
        for v in ('0x770', '0x10+0x8', '0x700+0x10', '0x870+0x20', 'x+y'):   # no length; inside the header; over the code length at +0x704; past the section's end; not numbers
            with self.subTest(v=v), self.assertRaises(SystemExit):
                game.shader_key_skip(self.profile(v))


if __name__ == '__main__':
    unittest.main()
