"""Star Fox's data.zip reaches the split: a zip behind a VZIP preamble whose stored members are chains of 64 KB LZ4 chunks, each against the previous one, holding the engine's
shader binaries (a GLSLC-magic header, the code area, then the NVN2 control section). Synthetic bytes only."""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

import lz4.block

from dm.tools.tests.test_split_gzip import REPO, ss

CHUNK = 0x10000


def nvn2_stage(sph_type, code_words=0x40):
    """(control section, code area) of one NVN2 stage: a 0x80-byte SPH (word 0 the stage) and instructions, no code header; the control's version 0xc9 at +4, its own length at +0x30, the code's at +0x44"""
    code = struct.pack('<I', sph_type) + bytes(0x7c) + bytes.fromhex('0f00000000000000') * code_words
    ctl = bytearray(0xe0); ctl[:4] = ss.CTL_MAGIC; struct.pack_into('<I', ctl, 4, 0xc9); struct.pack_into('<I', ctl, 0x30, len(ctl)); struct.pack_into('<I', ctl, 0x44, len(code))
    return bytes(ctl), code


def viper(ctl, code):   # the engine's shader binary: magic, code length, control length, 0, a 64-bit hash, code, control
    return b'\x91\x68\x86\x19' + struct.pack('<IIIQ', len(code), len(ctl), 0, 0x1234567887654321) + code + ctl


def chained(data):
    """The member as the engine stores it: [u32 length][LZ4 block] per 64 KB chunk, each block compressed against the previous chunk; a chunk that would not shrink is stored raw"""
    out, prev = bytearray(), b''
    for at in range(0, len(data), CHUNK):
        chunk = data[at:at + CHUNK]
        block = lz4.block.compress(chunk, store_size=False, dict=prev) if prev else lz4.block.compress(chunk, store_size=False)
        if len(block) >= len(chunk): block = chunk
        out += struct.pack('<I', len(block)) + block; prev = chunk
    return bytes(out)


def vzip(members, preamble=0x40):
    """A zip with a VZIP preamble: members = [(name, bytes on disk, file size, CRC of the data, method)]"""
    out = bytearray(b'VZIP' + bytes(preamble - 4)); central = bytearray()
    for name, disk, size, crc, method in members:
        nm = name.encode(); at = len(out)
        out += struct.pack('<4sHHHHHIIIHH', b'PK\x03\x04', 45, 0, method, 0, 0, crc, len(disk), size, len(nm), 0) + nm + disk
        central += struct.pack('<4sHHHHHHIIIHHHHHII', b'PK\x01\x02', 45, 45, 0, method, 0, 0, crc, len(disk), size, len(nm), 0, 0, 0, 0, 0, at) + nm
    cd = len(out); out += central
    out += struct.pack('<4sHHHHIIH', b'PK\x05\x06', 0, 0, len(members), len(members), len(central), cd, 0)
    return bytes(out)


def member(name, data, chain=True):
    disk = chained(data) if chain else data
    return (name, disk, len(data), zlib.crc32(data) & 0xffffffff, 0)


class ViperZip(unittest.TestCase):
    def scan(self, data, names, rel='data.zip'):
        with tempfile.TemporaryDirectory() as t, contextlib.redirect_stdout(io.StringIO()) as log:
            path = Path(t) / 'data.zip'; path.write_bytes(data)
            jobs, zips = ss.zip_jobs([(str(path), rel)])
            self.assertEqual(zips, {str(path)}); self.assertEqual(sorted(n for _, _, (_, ns) in jobs for n in ns), sorted(names))
            result = ss.scan_file((str(path), rel, (ss.ZIP, tuple(names))))
        return result, log.getvalue()

    def test_chained_members_hold_the_shaders(self):
        vs, ps = viper(*nvn2_stage(0x0461)), viper(*nvn2_stage(0x1462))
        big = viper(*nvn2_stage(0x0461, code_words=CHUNK // 4))   # over three chunks: the second and third decode against their predecessors
        self.assertGreater(len(big), 2 * CHUNK)
        data = vzip([member('8a2c93ccb633872a', vs), member('f34d11d5e85cc264', big), member('aa31b510cc6cebaa', ps, chain=False), member('d723ca0f3b93c5c0', b'{\n\t"segments": []\n}')])
        (rel, keys, pairs, extra), log = self.scan(data, ['8a2c93ccb633872a', 'f34d11d5e85cc264', 'aa31b510cc6cebaa', 'd723ca0f3b93c5c0'])
        self.assertEqual(log, ''); self.assertEqual(extra, {})
        self.assertEqual(sorted(v[1] for v in keys.values()), ['ps', 'vs', 'vs'])
        self.assertTrue(all('#VIPER' in v[0] for v in keys.values()))
        self.assertEqual({v[0].rsplit('/', 1)[1].split('#')[0] for v in keys.values()}, {'8a2c93ccb633872a', 'f34d11d5e85cc264', 'aa31b510cc6cebaa'})
        self.assertEqual(pairs, set())   # single-stage programs pair at draw time
        self.assertEqual(ss.group('/data.zip/8a2c93ccb633872a'), ss.group('/data.zip/x'))

    def test_a_damaged_chunk_is_a_red_line_not_an_exception(self):
        disk = bytearray(chained(viper(*nvn2_stage(0x0461)))); disk[8] ^= 0xff
        data = vzip([('8a2c93ccb633872a', bytes(disk), len(viper(*nvn2_stage(0x0461))), 0, 0)])
        (rel, keys, pairs, extra), log = self.scan(data, ['8a2c93ccb633872a'])
        self.assertEqual(keys, {}); self.assertEqual(extra[0], 'unreadable'); self.assertIn('8a2c93ccb633872a', log); self.assertIn('[RED]', log)

    def test_a_shader_binary_that_does_not_add_up_is_not_one(self):
        ctl, code = nvn2_stage(0x1462)
        self.assertIsNone(ss.viper_shader(viper(ctl, code) + b'\0', 'x'))   # a trailing byte: the lengths do not add up
        self.assertIsNone(ss.viper_shader(viper(ctl[:0x20], code), 'x'))   # too short a control section
        self.assertEqual([(s, len(c), len(k)) for _, s, c, k in ss.viper_shader(viper(ctl, code), 'x')], [('ps', len(ctl), len(code))])

    def test_cli_counts_the_members(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); rom = root / 'RomFS'; rom.mkdir(); main = root / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
            (rom / 'data.zip').write_bytes(vzip([member('8a2c93ccb633872a', viper(*nvn2_stage(0x0461))), member('aa31b510cc6cebaa', viper(*nvn2_stage(0x1462)))]))
            dest = root / 'out'; dest.mkdir(); outputs = [dest / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            proc = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *outputs], capture_output=True, text=True, env=dict(os.environ, DM_GAME='0400E88025B1A000', J='2'))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr); self.assertNotIn('[RED]', proc.stdout + proc.stderr)
            self.assertIn('shaders: 2 (', proc.stdout)
            self.assertIn('/data.zip/8a2c93ccb633872a#VIPER', (dest / 'map.tsv').read_text())


if __name__ == '__main__': unittest.main()
