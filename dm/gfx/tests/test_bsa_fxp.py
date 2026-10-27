"""Bethesda archive and NVN FXP readers, using synthetic records only."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bsa
import shader_split as split


def shader(stage, headerless=False):
    ctl, code = bytearray(0x848), bytearray(0x100)
    ctl[:4] = split.CTL_MAGIC
    struct.pack_into('<I', ctl, 0x20, len(ctl))
    struct.pack_into('<I', ctl, 0x704, len(code))
    if stage == 'cs':
        struct.pack_into('<3I', ctl, 0x748, 8, 4, 1)
        if not headerless: code[:4] = split.CS_MAGIC
    else:
        code[:4] = split.GFX_MAGIC
        struct.pack_into('<I', code, 0x30, (split.STAGES.index(stage) + 1) << 10 | 1)
    return struct.pack('<4I', 16, len(ctl), 16 + len(ctl), len(code)) + ctl + code, bytes(ctl), bytes(code)


def archive(payload, version=105, compressed=False, default=False, embedded=False):
    folder, name = b'shadersfx\0', b'test.fxp\0'
    if compressed:
        if version == 105:
            import lz4.frame
            body = lz4.frame.compress(payload)
        else:
            body = zlib.compress(payload)
        body = struct.pack('<I', len(payload)) + body
    else:
        body = payload
    if embedded:
        body = bytes([len(name) - 1]) + name[:-1] + body
    folder_size = 24 if version == 105 else 16
    start = 36 + folder_size + 1 + len(folder) + 16 + len(name)
    flags = 3 | (4 if default else 0) | (0x100 if embedded else 0)
    header = struct.pack('<4s8I', b'BSA\0', version, 36, flags, 1, 1, len(folder), len(name), 0)
    record = (struct.pack('<QIIQ', 0, 1, 0, 36 + folder_size + len(name)) if version == 105 else
              struct.pack('<QII', 0, 1, 36 + folder_size + len(name)))
    size = len(body) | (0x40000000 if compressed != default else 0)
    return header + record + bytes([len(folder)]) + folder + struct.pack('<QII', 0, size, start) + name + body


class FxpTests(unittest.TestCase):
    def test_all_stages_with_unaligned_records(self):
        for stage in split.STAGES:
            with self.subTest(stage=stage):
                data, ctl, code = shader(stage)
                found = split.fxp_shaders(b'odd' + data, '/shaders/test.fxp')
                self.assertEqual([(s, c, k) for _, s, c, k in found], [(stage, ctl, code)])

    def test_headerless_compute(self):
        data, ctl, code = shader('cs', headerless=True)
        self.assertEqual(split.fxp_shaders(data, '/test')[0][1:], ('cs', ctl, code))

    def test_embedded_record_with_unaligned_code(self):
        _, ctl, code = shader('vs')
        ctl = bytearray(ctl[:0x7d1])
        struct.pack_into('<I', ctl, 0x20, len(ctl))
        record = struct.pack('<4I', 16, len(ctl), 16 + len(ctl), len(code))
        image = bytes(128) + record + ctl + code + bytes(32)
        found = split.fxp_shaders(image, 'main')
        self.assertEqual(found, [('main#FXP@0x90', 'vs', bytes(ctl), code)])

    def test_unrelated_control_magic_is_ignored(self):
        self.assertEqual(split.fxp_shaders(bytes(32) + split.CTL_MAGIC + bytes(0x900), '/other'), [])

    def test_truncation_and_independent_code_length(self):
        data = shader('vs')[0]
        with self.assertRaisesRegex(SystemExit, 'truncated'):
            split.fxp_shaders(data[:-1], '/short')
        bad = bytearray(data)
        struct.pack_into('<I', bad, 16 + 0x704, 0x80)
        with self.assertRaisesRegex(SystemExit, 'code area length'):
            split.fxp_shaders(bad, '/bad length')


class BsaTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'test.bsa'
            p.write_bytes(data)
            return list(bsa.shader_files(p))

    def test_stored_and_compressed_members(self):
        payload = shader('ps')[0]
        for version in (103, 104, 105):
            for compressed in (False, True):
                for default in (False, True):
                    with self.subTest(version=version, compressed=compressed, default=default):
                        self.assertEqual(self.read(archive(payload, version, compressed, default)), [('shadersfx/test.fxp', payload)])

    def test_embedded_name_and_shader_extraction(self):
        data = archive(shader('vs')[0], compressed=True, embedded=True)
        name, payload = self.read(data)[0]
        self.assertEqual(name, 'shadersfx/test.fxp')
        self.assertEqual(split.fxp_shaders(payload, name)[0][1], 'vs')

    def test_file_dispatch_keeps_archive_reads_bounded(self):
        # A BSA can be gigabytes of texture/audio data. Its dispatcher must
        # return the streamed shaders instead of falling through to read-all.
        original_open = open
        class Reader:
            def __init__(self, file): self.file = file
            def __enter__(self): return self
            def __exit__(self, *args): self.file.close()
            def seek(self, *args): return self.file.seek(*args)
            def tell(self): return self.file.tell()
            def read(self, size=-1):
                if size < 0: raise AssertionError('whole-archive read')
                return self.file.read(size)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shaders.bsa'
            path.write_bytes(archive(shader('vs')[0]))
            with patch.object(split, 'open', lambda *a, **k: Reader(original_open(*a, **k)), create=True):
                rel, keys, pairs, trees = split.scan_file((str(path), path.name))
            self.assertEqual(rel, path.name)
            self.assertEqual(len(keys), 1)
            self.assertEqual(next(iter(keys.values()))[1], 'vs')

    def test_archive_inside_romfs_image(self):
        import romfs_image
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'RomFS'
            (source / 'Data').mkdir(parents=True)
            (source / 'Data/shaders.bsa').write_bytes(archive(shader('vs')[0]))
            image = root / 'romfs.img'
            romfs_image.pack(source, image)
            rel, keys, pairs, trees = split.scan_file(
                (str(image / 'Data/shaders.bsa'), 'Data/shaders.bsa'))
            self.assertEqual(rel, 'Data/shaders.bsa')
            self.assertEqual(len(keys), 1)
            self.assertEqual(next(iter(keys.values()))[1], 'vs')

    def test_truncated_archive_fails(self):
        data = archive(shader('vs')[0])
        for short in (data[:20], data[:70], data[:-1]):
            with self.subTest(length=len(short)), self.assertRaises(ValueError):
                self.read(short)


if __name__ == '__main__':
    unittest.main()
