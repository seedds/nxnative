"""Shin'en's SHN!D files (Fast Fusion) reach the split: a 0x44-byte header, a metadata block holding each shader's NVN2 control section whole, and a code pool holding their
code areas back to back in the same order, each block LZ4 unless stored as is. Synthetic bytes only."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

import lz4.block

from dm.tools.tests.test_split_gzip import ss
from dm.tools.tests.test_split_viper import nvn2_stage


def shn(stages, meta_extra=b'', pool_extra=b'', compress=True):
    """An SHN!D file: the metadata (some leading bytes, the control sections, meta_extra) and the pool (the code areas in order, pool_extra), each LZ4 unless compress is off"""
    meta = b'SCENOUNC' + bytes(0x18) + b''.join(c for c, _ in stages) + meta_extra
    pool = b''.join(k for _, k in stages) + pool_extra
    m = lz4.block.compress(meta, store_size=False) if compress else meta
    p = lz4.block.compress(pool, store_size=False) if compress and pool else pool
    head = bytearray(0x44); head[:8] = ss.SHN_D
    struct.pack_into('<2I', head, 0x08, len(m), len(meta))
    struct.pack_into('<3I', head, 0x24, 0x44 + len(m), len(p), len(pool))
    return bytes(head) + m + p


class ShinenShn(unittest.TestCase):
    def test_every_control_section_takes_its_code_in_order(self):
        vs, ps, big = nvn2_stage(0x0461, 0x10), nvn2_stage(0x1462, 0x30), nvn2_stage(0x0461, 0x200)
        out = ss.shn_shaders(shn([vs, ps, big], meta_extra=b'\x07' * 40, pool_extra=b'\xee' * 0x100), '/sets2/x.w3d')
        self.assertEqual([(w, s) for w, s, _, _ in out], [('/sets2/x.w3d#SHN#0', 'vs'), ('/sets2/x.w3d#SHN#1', 'ps'), ('/sets2/x.w3d#SHN#2', 'vs')])
        self.assertEqual([(c, k) for _, _, c, k in out], [vs, ps, big])
        self.assertTrue(all(ss.check(w, s, c, k) is None for w, s, c, k in out))

    def test_stored_blocks_and_files_without_shaders(self):
        vs = nvn2_stage(0x0461)
        self.assertEqual([(c, k) for _, _, c, k in ss.shn_shaders(shn([vs], compress=False), 'x')], [vs])
        self.assertEqual(ss.shn_shaders(shn([]), 'font'), [])   # a font, a texture, light probes: no control section in the metadata

    def test_a_pool_too_short_is_a_red_line(self):
        vs = nvn2_stage(0x0461)
        ctl = bytearray(vs[0]); struct.pack_into('<I', ctl, 0x44, len(vs[1]) + 0x100)
        with self.assertRaises(SystemExit) as e: ss.shn_shaders(shn([(bytes(ctl), vs[1])]), 'x')
        self.assertIn('[RED]', str(e.exception)); self.assertIn('pool', str(e.exception))

    def test_the_scan_takes_the_file(self):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'blur.shdr'; path.write_bytes(shn([nvn2_stage(0x0461), nvn2_stage(0x1462)]))
            rel, keys, pairs, extra = ss.scan_file((str(path), 'shaders/blur.shdr'))
        self.assertEqual(log.getvalue(), ''); self.assertEqual(extra, {})
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs'])


if __name__ == '__main__': unittest.main()
