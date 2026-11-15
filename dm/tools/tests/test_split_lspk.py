"""LSPK indexed reads and BSHD stage/pair extraction, using synthetic packages."""
import contextlib
import io
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import lz4.block
from dm.tools.tests.test_split_gzip import fz, ss
import lspk


def bshd(sections, compressed=True):
    data = bytearray(b'\xff\xff\xff\xff\x01' + bytes([len(sections)]))
    at = 6 + len(sections) * 9
    for stage, body in sections: data += struct.pack('<BII', stage, at, len(body)); at += len(body)
    data += b''.join(body for _, body in sections)
    return struct.pack('>I', len(data) | (0x80000000 if compressed else 0)) + (zlib.compress(data) if compressed else data)


def package(path, members, parts=1):
    """members: [(name, body, compression method, part)]."""
    areas = [bytearray() for _ in range(parts)]; table = bytearray()
    for name, body, method, part in members:
        packed = body if method == 0 else zlib.compress(body) if method == 1 else lz4.block.compress(body, store_size=False)
        table += name.encode().ljust(256, b'\0') + struct.pack('<6I', len(areas[part]), len(packed), len(body) if method else 0, part, method, 0)
        areas[part] += packed
    offset = len(areas[0]); stored = struct.pack('<I', len(members)) + lz4.block.compress(table, store_size=False)
    areas[0] += stored + struct.pack('<IIIHBB16sI4s', 13, offset, len(stored), parts, 2, 0, bytes(16), 40, b'LSPK')
    for part, data in enumerate(areas):
        target = path if part == 0 else path.with_name(path.stem + f'_{part}' + path.suffix)
        target.write_bytes(data)
    return path


class Lspk(unittest.TestCase):
    def test_stored_zlib_lz4_and_numbered_parts(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = package(Path(tmp) / 'a.pak', [('a', b'raw', 0, 0), ('b', b'zlib' * 50, 1, 1), ('c', b'lz4' * 70, 2, 0)], parts=2)
            parsed = lspk.package(str(p)); assert parsed is not None
            paths, entries = parsed
            self.assertEqual(paths, [str(p), str(p.with_name('a_1.pak'))])
            self.assertEqual([lspk.read(e) for e in entries], [b'raw', b'zlib' * 50, b'lz4' * 70])

    def test_another_pak_format_is_not_claimed(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'other.pak'; p.write_bytes(b'another archive')
            self.assertIsNone(lspk.package(str(p)))

    def test_empty_package_is_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = package(Path(tmp) / 'empty.pak', [])
            self.assertEqual(lspk.package(str(p)), ([str(p)], []))

    def test_invalid_header_and_table_ranges(self):
        for field, value, why in [(0, 14, 'version'), (4, 0xffffffff, 'file table'), (8, 0xffffffff, 'file table'), (32, 0, 'header size')]:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                p = package(Path(tmp) / 'bad.pak', [('a', b'x', 0, 0)])
                d = bytearray(p.read_bytes()); struct.pack_into('<I', d, len(d) - 40 + field, value); p.write_bytes(d)
                with self.assertRaisesRegex(ValueError, why): lspk.package(str(p))

    def test_oversized_table_is_rejected_before_decode(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = package(Path(tmp) / 'bad.pak', [('a', b'x', 0, 0)])
            d = bytearray(p.read_bytes()); struct.pack_into('<I', d, 1, 0xffffffff); p.write_bytes(d)
            with self.assertRaisesRegex(ValueError, 'file count'): lspk.package(str(p))

    def test_member_cannot_extend_into_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = package(Path(tmp) / 'bad.pak', [('a', b'x', 0, 0)])
            d = p.read_bytes(); table = bytearray(b'a'.ljust(256, b'\0') + struct.pack('<6I', 0, 2, 0, 0, 0, 0))
            stored = struct.pack('<I', 1) + lz4.block.compress(table, store_size=False)
            p.write_bytes(d[:1] + stored + struct.pack('<IIIHBB16sI4s', 13, 1, len(stored), 1, 0, 0, bytes(16), 40, b'LSPK'))
            with self.assertRaisesRegex(ValueError, 'range'): lspk.package(str(p))

    def test_zlib_must_end_at_declared_size(self):
        for body, size in [(zlib.compress(b'actual'), 2), (zlib.compress(b'actual') + b'tail', 6), (zlib.compress(b'actual')[:-1], 6)]:
            with self.subTest(size=size), tempfile.TemporaryDirectory() as tmp:
                p = Path(tmp) / 'body'; p.write_bytes(body)
                with self.assertRaises(ValueError): lspk.read(('x', str(p), 0, len(body), size, 1))

    def test_lz4_must_have_declared_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'body'; body = lz4.block.compress(b'actual', store_size=False); p.write_bytes(body)
            with self.assertRaisesRegex(ValueError, 'decoded size'): lspk.read(('x', str(p), 0, len(body), 10, 2))

    def test_bshd_records_preserve_pair_and_ignore_other_platforms(self):
        vs, ps = fz.glslc([fz.VS]), fz.glslc([fz.PS])
        for compressed in (False, True):
            with self.subTest(compressed=compressed):
                found = ss.bshd_shaders(bshd([(1, vs), (3, ps)], compressed), '/a.bshd')
                self.assertEqual([s[1] for s in found], ['vs', 'ps'])
                self.assertEqual(ss.var_pairs(found), {(ss.skey(*fz.VS), ss.skey(*fz.PS))})
        self.assertEqual(ss.bshd_shaders(bshd([(1, b'UA5 another GPU')]), '/other.bshd'), [])

    def test_bshd_stage_and_program_must_agree(self):
        with self.assertRaisesRegex(SystemExit, 'disagrees'):
            ss.bshd_shaders(bshd([(3, fz.glslc([fz.VS]))]), '/bad.bshd')

    def test_bshd_stage_must_be_inside_the_record(self):
        d = bytearray(bshd([(1, b'body')], compressed=False)); struct.pack_into('<I', d, 11, 0xffffffff)
        with self.assertRaisesRegex(ValueError, 'range'): lspk.bshd_sections(d, '/bad.bshd')

    def test_jobs_read_only_shader_members_and_skip_parts(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = package(Path(tmp) / 'data.pak', [('sound.wem', b'nonshader', 0, 0), ('Shaders/NX/a.bshd', bshd([(1, fz.glslc([fz.VS])), (3, fz.glslc([fz.PS]))]), 2, 1)], parts=2)
            jobs, consumed = ss.lspk_jobs([(str(p), p.name), (str(p.with_name('data_1.pak')), 'data_1.pak')])
            self.assertEqual(consumed, {str(p), str(p.with_name('data_1.pak'))})
            self.assertEqual(len(jobs), 1)
            result = ss.scan_file(jobs[0])
            self.assertEqual(len(result[1]), 2)
            self.assertEqual(len(result[2]), 1)
            self.assertEqual(result[3], {})

    def test_invalid_package_is_reported_as_unreadable(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            p = package(Path(tmp) / 'bad.pak', [('a', b'x', 0, 0)])
            d = bytearray(p.read_bytes()); struct.pack_into('<I', d, len(d) - 40, 99); p.write_bytes(d)
            jobs, consumed = ss.lspk_jobs([(str(p), p.name)])
            self.assertEqual(consumed, {str(p)})
            self.assertEqual(ss.scan_file(jobs[0])[3][0], 'unreadable')


if __name__ == '__main__': unittest.main()
