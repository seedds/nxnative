"""Larian v13 archives deliver real synthetic GLSLC programs to the shader split (#1679)."""
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
from unittest import mock

import lz4.block

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz
ss = fz.ss
import romfs_image


def bshd(data):
    return struct.pack('>I', 0x80000000 | len(data)) + zlib.compress(data)


def archive(files, *, flags=0):
    """Independent v13 fixture: (name, decoded bytes, compression flags, part)."""
    count = max((part for _, _, _, part in files), default=0) + 1
    parts = [bytearray() for _ in range(count)]
    table = bytearray()
    for name, raw, method, part in files:
        packed = zlib.compress(raw) if method & 15 == 1 else lz4.block.compress(raw, store_size=False) if method & 15 == 2 else raw
        table += struct.pack('<256s6I', name.encode(), len(parts[part]), len(packed), len(raw) if method & 15 else 0, part, method, zlib.crc32(packed))
        parts[part] += packed
    offset = len(parts[0])
    listing = struct.pack('<I', len(files)) + lz4.block.compress(bytes(table), store_size=False)
    header = struct.pack('<IIIHBB16s', 13, offset, len(listing), count, flags, 0, bytes(16))
    parts[0] += listing + header + struct.pack('<I4s', 40, b'LSPK')
    return parts


class LspkShaders(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.program = fz.glslc([fz.VS, fz.PS])

    def write(self, parts, name='Materials.pak'):
        path = self.root / name
        for i, data in enumerate(parts):
            dest = path if not i else path.with_name(f'{path.stem}_{i}{path.suffix}')
            dest.write_bytes(data)
        return str(path)

    def scan(self, path):
        jobs, owned = ss.lspk_jobs([(path, 'Data/Materials.pak')], parts=1)
        self.assertEqual(len(jobs), 1)
        with contextlib.redirect_stdout(io.StringIO()) as log:
            result = ss.scan_file(jobs[0])
        return result, log.getvalue(), owned

    def test_each_compression_and_external_part_keeps_real_stages_and_pair(self):
        for flags in (0, 1, 2, 0x21, 0x42):
            for part in (0, 1):
                with self.subTest(flags=flags, part=part):
                    path = self.write(archive([('Shaders/main.bshd', bshd(self.program), flags, part)]))
                    (_, keys, pairs, extra), log, owned = self.scan(path)
                    self.assertEqual({v[1]: v[2:] for v in keys.values()}, {'vs': fz.VS, 'ps': fz.PS})
                    by_stage = {v[1]: k for k, v in keys.items()}
                    self.assertEqual(pairs, {(by_stage['vs'], by_stage['ps'])})
                    self.assertEqual((extra, log), ({}, ''))
                    self.assertIn(path, owned)
                    if part: self.assertIn(str(self.root / 'Materials_1.pak'), owned)

    def test_nonshader_members_are_not_decoded(self):
        # Compression 15 is unsupported, but an unrelated file must not hide shaders.
        path = self.write(archive([('readme.txt', b'not compressed', 15, 0), ('shader.BSHD', bshd(self.program), 0, 0)]), 'Materials.PAK')
        (_, keys, pairs, extra), log, _ = self.scan(path)
        self.assertEqual((len(keys), len(pairs), extra, log), (2, 1, {}, ''))

    def test_shaderless_archive_and_empty_file_list_are_consumed(self):
        for files in ([], [('names.bshd', bshd(b'reflection names only'), 0, 0)], [('readme', b'plain', 0, 0)]):
            with self.subTest(files=len(files)):
                path = self.write(archive(files))
                jobs, owned = ss.lspk_jobs([(path, 'Data/Materials.pak')])
                self.assertEqual(owned, {path})
                for job in jobs:
                    self.assertEqual(ss.scan_file(job)[1:], ({}, set(), {}))

    def test_malformed_header_and_file_list_are_named_unreadable(self):
        good = archive([('shader.bshd', bshd(self.program), 0, 0)])[0]
        cases = {}
        for label, offset, value in [('header size', -8, 4), ('version', -40, 15), ('list offset', -36, len(good)), ('list length', -32, 3)]:
            bad = bytearray(good); struct.pack_into('<I', bad, len(bad) + offset, value); cases[label] = bad
        bad = bytearray(good); lo = struct.unpack_from('<I', bad, len(bad) - 36)[0]
        struct.pack_into('<I', bad, lo, 0xffffffff); cases['count budget'] = bad
        bad = bytearray(good); struct.pack_into('<I', bad, lo, 1000000); cases['impossible LZ4 expansion'] = bad
        bad = bytearray(good); bad[lo + 4] = 0; cases['corrupt list'] = bad
        cases['solid archive'] = archive([('shader.bshd', bshd(self.program), 0, 0)], flags=4)[0]
        for label, data in cases.items():
            with self.subTest(case=label):
                path = self.write([data])
                (_, keys, pairs, extra), log, _ = self.scan(path)
                self.assertEqual((keys, pairs), ({}, set()))
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn('LSPK', log)
                self.assertNotIn('Traceback', log)

    def test_bad_inner_streams_do_not_publish_partial_shaders(self):
        packed = bshd(self.program)
        cases = [packed[:-1], packed + b'trailing', b'\0\0\0\0' + packed[4:],
                 struct.pack('>I', 0x80000000 | (len(self.program) - 1)) + packed[4:],
                 struct.pack('>I', 0xffffffff) + packed[4:]]
        for bad in cases:
            with self.subTest(size=len(bad), header=bad[:4].hex()):
                path = self.write(archive([('good.bshd', packed, 0, 0), ('bad.bshd', bad, 0, 0)]))
                (_, keys, pairs, extra), log, _ = self.scan(path)
                self.assertEqual((keys, pairs), ({}, set()))
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn('bad.bshd', log)

    def test_missing_or_truncated_part_is_unreadable(self):
        for missing in (True, False):
            with self.subTest(missing=missing):
                path = self.write(archive([('shader.bshd', bshd(self.program), 2, 1)]))
                part = self.root / 'Materials_1.pak'
                if missing: part.unlink()
                else: part.write_bytes(part.read_bytes()[:-1])
                (_, keys, pairs, extra), log, _ = self.scan(path)
                self.assertEqual((keys, pairs), ({}, set()))
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn('shader.bshd', log)

    def test_bad_member_ranges_sizes_and_compression_are_unreadable(self):
        for field, value in [(1, 0xffffffff), (2, 0xffffffff), (3, 1), (4, 2), (5, 3)]:
            with self.subTest(field=field):
                data = archive([('shader.bshd', bshd(self.program), 1, 0)])[0]
                off = struct.unpack_from('<I', data, len(data) - 36)[0]
                table = bytearray(lz4.block.decompress(data[off + 4:-40], uncompressed_size=280))
                struct.pack_into('<I', table, 256 + (field - 1) * 4, value)
                listing = struct.pack('<I', 1) + lz4.block.compress(bytes(table), store_size=False)
                header = bytearray(data[-40:]); struct.pack_into('<I', header, 8, len(listing))
                path = self.write([data[:off] + listing + header])
                (_, keys, pairs, extra), log, _ = self.scan(path)
                self.assertEqual((keys, pairs), ({}, set()))
                self.assertEqual(extra[0], 'unreadable')
                self.assertIn('LSPK', log)

    def test_compute_shader_remains_unpaired(self):
        ctl, code = fz.VS
        code = ss.CS_MAGIC + code[4:]
        path = self.write(archive([('compute.bshd', bshd(fz.glslc([(ctl, code)])), 0, 0)]))
        (_, keys, pairs, extra), log, _ = self.scan(path)
        self.assertEqual([v[1:] for v in keys.values()], [('cs', ctl, code)])
        self.assertEqual((pairs, extra, log), (set(), {}, ''))

    def test_budget_is_checked_before_decompressing(self):
        path = self.write(archive([('shader.bshd', bshd(self.program), 1, 0)]))
        # A small injected resource budget makes a valid member over-budget.
        with mock.patch.object(ss, 'LSPK_LIMIT', 1024):
            (_, keys, pairs, extra), log, _ = self.scan(path)
        self.assertEqual((keys, pairs), ({}, set()))
        self.assertEqual(extra[0], 'unreadable')
        self.assertIn('budget', log)

    def test_cli_extracts_archived_program_without_scanning_part_as_loose_file(self):
        rom = self.root / 'RomFS'; rom.mkdir()
        self.write(archive([('shader.bshd', bshd(self.program), 2, 1)]))
        for p in self.root.glob('Materials*.pak'): p.rename(rom / p.name)
        main = self.root / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
        image = self.root / 'RomFS.img'; romfs_image.pack(rom, image)
        results = []
        for i, source in enumerate((rom, image)):
            dest = self.root / f'out{i}'; dest.mkdir()
            outputs = [dest / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            proc = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', source, main, *outputs],
                                  capture_output=True, text=True, env=dict(os.environ, DM_GAME='0100C1F0051B6000', J='2'))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn('shaders: 2 (', proc.stdout)
            self.assertNotIn('[RED]', proc.stdout + proc.stderr)
            rows = [r for r in outputs[1].read_text().splitlines() if r and not r.startswith('#')][1:]
            self.assertEqual(len(rows), 2)
            self.assertTrue(all('shader.bshd' in row for row in rows))
            results.append({str(p.relative_to(dest)): p.read_bytes() for p in dest.rglob('*') if p.is_file()})
        self.assertEqual(results[0], results[1])


if __name__ == '__main__': unittest.main()
