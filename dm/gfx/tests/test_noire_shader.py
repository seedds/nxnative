"""L.A. Noire's split GLSLC arrays inside out.wad.nx, without game data."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split
import romfs_image


def vfp_archive():
    headers, controls, code = bytearray(), bytearray(), bytearray()
    expected = {}
    for stage, code_size in [('vs', 0x100), ('ps', 0x200)]:
        ctl, instructions = bytearray(0x838), bytearray(code_size)
        ctl[:4] = split.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<I', ctl, 0x704, len(instructions))
        instructions[:4] = split.GFX_MAGIC
        struct.pack_into('<I', instructions, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
        header = bytearray(0x120)
        header[:4] = b'\x91\x68\x86\x19'
        struct.pack_into('<4I', header, 0xc0, len(controls), len(code), len(instructions), len(ctl))
        expected[split.skey(ctl, instructions)] = (stage, bytes(ctl), bytes(instructions))
        headers.extend(header)
        controls.extend(ctl)
        code.extend(instructions)
    return struct.pack('<4Q', 0, len(headers), len(controls), len(code)) + headers + controls + code, expected


def wad_archive(shader_data, name=b'out/graphicsdata/programs.vfp.nx'):
    return struct.pack('<4s4I', b'WAD\x01', 1, 0, 20, len(shader_data)) + shader_data + struct.pack('<H', len(name)) + name


class NoireShaderArchiveTests(unittest.TestCase):
    def test_split_sections_reach_the_shader_map(self):
        archive, expected = vfp_archive()
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'RomFS'
            source.mkdir()
            path = source / 'out.wad.nx'
            path.write_bytes(wad_archive(archive))
            image = Path(tmp) / 'RomFS.img'
            romfs_image.pack(source, image)
            for root in (source, image):
                with self.subTest(root=root.name):
                    _, found, pairs, extra = split.scan_file_in((str(root / path.name), path.name))
                    self.assertEqual({key: record[1:4] for key, record in found.items()}, expected)
                    self.assertEqual(pairs, set())
                    self.assertEqual(extra, {})

    def test_requires_both_filename_and_signature(self):
        archive = wad_archive(vfp_archive()[0])
        with tempfile.TemporaryDirectory() as tmp:
            for name, data in [('other.wad.nx', archive), ('out.wad.nx', b'WAD\x02' + archive[4:])]:
                with self.subTest(name=name):
                    path = Path(tmp) / name
                    path.write_bytes(data)
                    self.assertEqual(split.scan_file_in((str(path), name)), (name, {}, set(), {}))

    def test_rejects_overlapping_code_spans(self):
        archive = bytearray(vfp_archive()[0])
        struct.pack_into('<I', archive, 32 + 0x120 + 0xc4, 1)
        with self.assertRaisesRegex(SystemExit, 'invalid VFP record spans'):
            split.noire_wad_shaders(wad_archive(archive), 'out.wad.nx')

    def test_rejects_truncated_wad_directory(self):
        with self.assertRaisesRegex(SystemExit, 'directory.*outside'):
            split.noire_wad_shaders(b'WAD\x01\x01\0\0\0', 'out.wad.nx')

    def test_rejects_last_member_overlapping_the_directory(self):
        archive = bytearray(wad_archive(vfp_archive()[0], b'asset.nx'))
        offset, size = struct.unpack_from('<2I', archive, 12)
        struct.pack_into('<2I', archive, 12, offset - 1, size + 1)
        with self.assertRaisesRegex(SystemExit, 'WAD last member or names outside archive data'):
            split.noire_wad_shaders(archive, 'out.wad.nx')

    def test_rejects_repeated_wad_member_span(self):
        archive, _ = vfp_archive()
        entry = struct.pack('<3I', 0, 32, len(archive))
        name = b'programs.vfp.nx'
        names = (struct.pack('<H', len(name)) + name) * 2
        wad = struct.pack('<4sI', b'WAD\x01', 2) + entry * 2 + archive + names
        with self.assertRaisesRegex(SystemExit, 'overlapping or unordered WAD shader members'):
            split.noire_wad_shaders(wad, 'out.wad.nx')

    def test_rejects_malformed_shader_member_names(self):
        archive, _ = vfp_archive()
        for name, reason in [(b'bad\xff', 'is not UTF-8'), (b'bad\t', 'would break the shader map'),
                             (b'bad\r', 'would break the shader map'), (b'bad\n', 'would break the shader map')]:
            with self.subTest(name=name), self.assertRaisesRegex(SystemExit, f'WAD shader member name {reason}'):
                split.noire_wad_shaders(wad_archive(archive, name + b'.vfp.nx'), 'out.wad.nx')

    def test_rejects_compute_records_without_code(self):
        header = bytearray(0x120)
        header[:4] = b'\x91\x68\x86\x19'
        ctl = bytearray(0x838)
        ctl[:4] = split.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        struct.pack_into('<3I', ctl, 0x748, 1, 1, 1)
        struct.pack_into('<4I', header, 0xc0, 0, 0, 0, len(ctl))
        archive = struct.pack('<4Q', 0, len(header), len(ctl), 0) + header + ctl
        with self.assertRaisesRegex(SystemExit, 'invalid VFP record spans'):
            split.noire_wad_shaders(wad_archive(archive), 'out.wad.nx')


if __name__ == '__main__':
    unittest.main()
