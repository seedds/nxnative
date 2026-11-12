"""Pak v9's frozen memory image and optional legacy index, using synthetic files only."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/gfx/tools'), str(Path(__file__).parent)]
import uepak
from test_uepak_v8 import entry, pak as pak8
from test_uepak_key import aes_encrypt


def frozen_pak(path, files, key=None):
    """Build a UE4 memory image with holes in both sparse maps and backward string pointers.
    files = [(path, raw bytes, compress)]; file data has serialized headers, never frozen headers."""
    body, index, labels = bytearray(), bytearray(88), {}

    def alloc(data):
        index.extend(bytes(-len(index) % 8))
        at = len(index); index.extend(data)
        return at
    def ptr(at, target):
        struct.pack_into('<q', index, at, ((target - at) << 1) | 1)
    def array(at, target, n):
        ptr(at, target); struct.pack_into('<ii', index, at + 8, n, n)
    def string(at, text):
        raw = (text + '\0').encode('utf-16-le')
        array(at, alloc(raw), len(raw) // 2)
    def sparse(at, stride, count):
        # Slot 0 is free and deliberately contains invalid pointers and indices.
        target = alloc(bytes([255]) * stride + bytes(stride * count))
        array(at, target, count + 1)
        bits = alloc(struct.pack('<I', ((1 << count) - 1) << 1))
        ptr(at + 16, bits)
        struct.pack_into('<iiii', index, at + 24, count + 1, 32, 0, 1)
        return [target + stride * (i + 1) for i in range(count)]

    string(0, '../../../')
    entries = alloc(bytes(80 * len(files))); array(16, entries, len(files))
    directories = {}
    for i, (name, data, comp) in enumerate(files):
        off, blocks = len(body), []
        chunks = [zlib.compress(data[j:j + 64]) for j in range(0, len(data), 64)] if comp else []
        cur = 53 + (4 + 16 * len(chunks) if comp else 0)
        for chunk in chunks: blocks.append((cur, cur + len(chunk))); cur += len(chunk)
        stored = b''.join(chunks) if comp else data
        body.extend(entry(off, len(stored), len(data), int(comp), blocks, 64 if comp else 0) + stored)
        at = entries + i * 80
        struct.pack_into('<qqq', index, at, off, len(stored), len(data))
        if blocks: array(at + 48, alloc(b''.join(struct.pack('<qq', *b) for b in blocks)), len(blocks))
        struct.pack_into('<IiB', index, at + 64, 64 if comp else 0, int(comp), 0)
        dn, fn = name.rsplit('/', 1)
        directories.setdefault(dn + '/', []).append((fn, i))
    for at, (dn, names) in zip(sparse(32, 80, len(directories)), directories.items()):
        string(at, dn)
        # Allocate names first to force signed backward pointer displacements.
        strings = [alloc((fn + '\0').encode('utf-16-le')) for fn, _ in names]
        for item, text, (fn, i) in zip(sparse(at + 16, 32, len(names)), strings, names):
            array(item, text, len((fn + '\0').encode('utf-16-le')) // 2)
            struct.pack_into('<i', index, item + 16, i)
            labels[dn + fn] = item
    raw = bytes(index)
    encoded = aes_encrypt(key, raw + bytes(-len(raw) % 16)) if key else raw
    names = b'Zlib'.ljust(32, b'\0') + bytes(32 * 4)
    footer = bytes(16) + bytes([bool(key)]) + struct.pack('<IIQQ', uepak.PAK_MAGIC, 9, len(body), len(raw)) + bytes(20) + b'\1' + names
    path.write_bytes(body + encoded + footer)
    return raw, labels


class Version9(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name) / 'pakchunk0-Switch.pak'
        self.data = b'LSLG' + bytes(range(256)) * 3
        self.files = [('Game/Content/\u706b.uexp', self.data, True), ('Engine/Config/a.ini', b'[x]\n', False)]
        self.index, self.labels = frozen_pak(self.p, self.files)

    def test_footer(self):
        with self.p.open('rb') as f:
            enc, _, _, methods, ver = uepak.footer(f)
            self.assertTrue(uepak.frozen_index(f, ver))
        self.assertEqual((enc, ver, methods), (0, 9, ['Zlib', '', '', '', '']))

    def test_sparse_maps_utf16_and_multiblock_files(self):
        p = uepak.Pak(str(self.p)); self.addCleanup(p.f.close)
        self.assertEqual(p.mount, '../../../')
        self.assertEqual(set(p.files), {f[0] for f in self.files})
        for name, data, _ in self.files: self.assertEqual(p.read(name), data)
        self.assertTrue(uepak.inline_shaders(str(self.p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=True), [str(self.p)])

    def test_nonfrozen_v9_still_uses_serialized_entries(self):
        pak8(self.p, self.files)
        data = bytearray(self.p.read_bytes())
        struct.pack_into('<I', data, len(data) - 200, 9)
        data[-160:-160] = b'\0'
        self.p.write_bytes(data)
        p = uepak.Pak(str(self.p)); self.addCleanup(p.f.close)
        for name, raw, _ in self.files: self.assertEqual(p.read(name), raw)

    def test_bad_frozen_flag_and_wrong_footer_layout(self):
        original = self.p.read_bytes()
        for pos, value in ((len(original) - 161, 2), (len(original) - 201, 8)):
            with self.subTest(pos=pos):
                data = bytearray(original); data[pos] = value; self.p.write_bytes(data)
                with self.p.open('rb') as f, self.assertRaises(ValueError): uepak.footer(f)

    def test_damaged_pointer_array_bitmap_and_file_reference(self):
        changes = [(0, '<q', 0), (0, '<q', (len(self.index) << 2) | 1), (24, '<i', -1), (56, '<i', 0),
                   (self.labels[self.files[0][0]] + 16, '<i', 500)]
        for at, fmt, value in changes:
            with self.subTest(at=at, value=value):
                data = bytearray(self.index); struct.pack_into(fmt, data, at, value)
                with self.assertRaises(ValueError): uepak.frozen_pak_index(data, ['Zlib'], 'synthetic index')

    def test_shader_library_and_iostore_classification(self):
        frozen_pak(self.p, [('Engine/GlobalShaderCache-GLSL_SWITCH.bin', b'global', False)])
        for containers in (False, True):
            with patch.object(uepak, 'inline_in_containers', return_value=containers):
                self.assertIs(uepak.inline_shaders(str(self.p)), True if containers else None)
        frozen_pak(self.p, [('Engine/ShaderArchive-Global.ushaderbytecode', b'library', False)])
        with patch.object(uepak, 'inline_in_containers', return_value=True): self.assertFalse(uepak.inline_shaders(str(self.p)))
        frozen_pak(self.p, [('Stream/demo.awb', b'audio', False)])
        self.assertIsNone(uepak.inline_shaders(str(self.p)))
        self.assertEqual(uepak.unreal_paks(self.tmp.name, inline=False), [])

    def test_empty_and_deleted_files(self):
        frozen_pak(self.p, [])
        self.assertIsNone(uepak.inline_shaders(str(self.p)))
        index, _ = frozen_pak(self.p, [('Game/gone.uexp', b'', False)])
        data = bytearray(index)
        entries = 16 + (struct.unpack_from('<q', data, 16)[0] >> 1)
        data[entries + 72] = 2
        self.assertEqual(uepak.frozen_pak_index(data, ['Zlib'], 'index'), ('../../../', {}))

    @unittest.skipUnless(sys.platform == 'darwin', 'CommonCrypto')
    def test_encrypted_index_and_key_search(self):
        key = bytes(range(32))
        frozen_pak(self.p, self.files, key)
        self.assertFalse(uepak.inline_shaders(str(self.p)))
        self.assertEqual(uepak.find_key(b'', 0, 0, str(self.p), first=[bytes(32), key]), key)
        self.assertEqual(uepak.find_key(bytes(64) + key + bytes(64), 0, 160, str(self.p)), key)
        p = uepak.Pak(str(self.p), key); self.addCleanup(p.f.close)
        self.assertEqual(p.read(self.files[0][0]), self.data)
        self.assertTrue(uepak.inline_shaders(str(self.p), key))


if __name__ == '__main__': unittest.main()
