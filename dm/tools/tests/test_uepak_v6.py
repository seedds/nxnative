"""Synthetic standard/compact Unreal pak v6 records, encryption and shader-job key transport.
No game bytes or user keys are used. Compact records are observed in Borderlands 3 Switch.
"""
from pathlib import Path
import hashlib
import os
import importlib.util
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib
import ctypes

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak

KEY = bytes(range(32))
def encrypt(data):
    if sys.platform != 'darwin': raise unittest.SkipTest('encrypted pak fixtures require CommonCrypto, as the reader does')
    cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
    cc.restype = ctypes.c_int32
    cc.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    out = ctypes.create_string_buffer(len(data)); moved = ctypes.c_size_t()
    if cc(0, 0, 2, KEY, len(KEY), None, data, len(data), out, len(data), ctypes.byref(moved)): raise ValueError('fixture encryption failed')
    return out.raw[:moved.value]

def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b

def fixture(path, version=6, compact=True, encrypted=False, deleted=False):
    """Two real payloads; one spans 64 KiB blocks at a nonzero file offset."""
    data = [('raw.bin', b'abcde' * 7, False), ('blocks.bin', b'x' * 65536 + b'y' * 111, True)]
    body, records = bytearray(bytes(23)), []
    for name, raw, compressed in data:
        off = len(body); chunks = [zlib.compress(raw[i:i+65536]) for i in range(0, len(raw), 65536)] if compressed else [raw]
        head = (18 if compact else 53) + (4 + 16 * len(chunks) if compressed else 0)
        stored = [encrypt(b + bytes(-len(b) % 16)) if encrypted else b for b in chunks]
        cur, blocks = head, []
        for plain, encoded in zip(chunks, stored):
            blocks.append((cur + (off if version < 5 else 0), cur + len(plain) + (off if version < 5 else 0))); cur += len(encoded)
        flags, method, size = int(encrypted), int(compressed), sum(map(len, stored)) if compressed else len(raw)
        entry = struct.pack('<qIIB' if compact else '<qqqi', off, size, len(raw), method)
        if not compact: entry += bytes(20)
        if compressed: entry += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', *b) for b in blocks)
        entry += struct.pack('<B', flags)
        if not compact: entry += struct.pack('<I', 65536)
        assert len(entry) == head
        body += entry + b''.join(stored)
        records.append(fstr(name) + entry)
    if deleted:
        entry = struct.pack('<qIIB', -1, 0, 0, 0) + b'\x02'
        records.append(fstr('deleted.bin') + entry)
    index = fstr('../../../') + struct.pack('<i', len(records)) + b''.join(records)
    if encrypted: index = encrypt(index + index[:(-len(index) % 16)])
    pos = len(body); body += index
    body += bytes([int(encrypted)]) + struct.pack('<IIQQ', uepak.PAK_MAGIC, version, pos, len(index)) + hashlib.sha1(index).digest()
    path.write_bytes(body)
    return dict((n, b) for n, b, _ in data)

class PakV6Tests(unittest.TestCase):
    def test_compact_stored_and_relative_multiblock(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'a.pak'; want = fixture(p)
            pak = uepak.Pak(str(p)); self.assertTrue(pak.compact)
            for name, raw in want.items(): self.assertEqual(pak.read(name), raw)
            self.assertTrue(uepak.inline_shaders(str(p)))

    def test_encrypted_index_and_individually_padded_blocks(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'a.pak'; want = fixture(p, encrypted=True)
            with self.assertRaisesRegex(ValueError, 'no key'): uepak.Pak(str(p))
            pak = uepak.Pak(str(p), KEY)
            for name, raw in want.items(): self.assertEqual(pak.read(name), raw)
            self.assertEqual(uepak.find_key(KEY + bytes(4), 0, 36, str(p)), KEY)

    def test_standard_v6_is_not_mistaken_for_compact(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'a.pak'; want = fixture(p, compact=False)
            pak = uepak.Pak(str(p)); self.assertFalse(pak.compact)
            for name, raw in want.items(): self.assertEqual(pak.read(name), raw)

    def test_version4_absolute_offsets_unchanged(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'a.pak'; want = fixture(p, version=4, compact=False)
            pak = uepak.Pak(str(p))
            for name, raw in want.items(): self.assertEqual(pak.read(name), raw)

    def test_deleted_record_has_its_own_flag(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'a.pak'; fixture(p, deleted=True)
            entry = uepak.Pak(str(p)).entry('deleted.bin')
            self.assertTrue(entry['deleted']); self.assertFalse(entry['encrypted'])

    def test_corrupt_block_range_rejected(self):
        index = fstr('a') + struct.pack('<qIIBiqqB', 0, 3, 65536, 1, 1, 38, 1000, 0)
        with self.assertRaisesRegex(ValueError, 'out of range'):
            uepak.legacy6_entries(index, 0, 1, 4096, compact=True)

    def test_trailing_nonpadding_rejected(self):
        index = fstr('a') + struct.pack('<qIIBB', 0, 1, 1, 0, 0) + b'bad'
        with self.assertRaisesRegex(ValueError, 'unconsumed'):
            uepak.legacy6_entries(index, 0, 1, 4096, compact=True)

    def test_inline_oodle_shader_and_stored_shader(self):
        spec = importlib.util.spec_from_file_location('shader_split_oodle_test', REPO/'dm/gfx/tools/shader_split.py')
        assert spec is not None and spec.loader is not None
        ss = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '010015100B514000'}): spec.loader.exec_module(ss)
        code = bytes(256); ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
        record = b'LSLGSC\0\0' + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl
        for offset in (8, 10, None):
            stream = (b'\x8c\x0c' + bytes(offset - 2) + b'LSLG' + b'compressed-body') if offset else b'\x8c\x0c' + b'encoded-with-no-literal-signature'
            data = struct.pack('<QI', len(record), len(stream)) + stream
            found = []
            with patch.object(ss, 'oodle', return_value=record) as decode:
                ss.ue_inline_shaders(data, 'synthetic-inline', found)
                decode.assert_called_once_with(stream, len(record), probe=True)
            self.assertEqual(len(found), 1); self.assertEqual(found[0][1], 'cs')
            self.assertEqual(found[0][2:], (bytes(ctl), code))
        found = []
        ss.ue_inline_shaders(struct.pack('<I', len(record)) + record, 'synthetic-stored', found)
        self.assertEqual(len(found), 1)

    def test_false_oodle_signature_does_not_drop_valid_stored_shader(self):
        spec = importlib.util.spec_from_file_location('shader_split_probe_test', REPO/'dm/gfx/tools/shader_split.py')
        assert spec is not None and spec.loader is not None
        ss = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '010015100B514000'}): spec.loader.exec_module(ss)
        code = bytes(256); ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
        record = b'LSLGSC\0\0' + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl
        fake = b'\x8c\x0c' + b'not-compressed'
        data = struct.pack('<QI', 1000, len(fake)) + fake + struct.pack('<I', len(record)) + record
        found = []
        with patch.object(ss, 'oodle', return_value=None): ss.ue_inline_shaders(data, 'probe', found)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0][2:], (bytes(ctl), code))

    def test_unrecognized_probe_needs_no_decoder_but_literal_shader_does(self):
        spec = importlib.util.spec_from_file_location('shader_split_no_decoder_test', REPO/'dm/gfx/tools/shader_split.py')
        assert spec is not None and spec.loader is not None
        ss = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '010015100B514000'}): spec.loader.exec_module(ss)
        with patch.dict(os.environ, {}, clear=True):
            self.assertIsNone(ss.oodle(b'\x8c\x0c' + bytes(20), 1000, probe=True))
            with self.assertRaises(SystemExit): ss.oodle(b'\x8c\x0c' + bytes(6) + b'LSLG', 1000, probe=True)
        self.assertIsNone(ss._OODLE)

    def test_invalid_inline_oodle_lengths_are_not_decoded(self):
        spec = importlib.util.spec_from_file_location('shader_split_bad_oodle_test', REPO/'dm/gfx/tools/shader_split.py')
        assert spec is not None and spec.loader is not None
        ss = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '010015100B514000'}): spec.loader.exec_module(ss)
        stream = b'\x8c\x0c' + bytes(6) + b'LSLG' + b'not-a-stream'
        found = []
        with patch.object(ss, 'oodle') as decode:
            ss.ue_inline_shaders(struct.pack('<QI', 1 << 40, len(stream)) + stream, 'bad', found)
            decode.assert_not_called()
        self.assertFalse(found)

    def test_encrypted_shader_jobs_receive_memory_only_key(self):
        spec = importlib.util.spec_from_file_location('shader_split_v6_test', REPO/'dm/gfx/tools/shader_split.py')
        assert spec is not None and spec.loader is not None
        ss = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '010015100B514000'}): spec.loader.exec_module(ss)
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)/'a.pak'; fixture(p, encrypted=True)
            with patch.object(uepak, 'nso_image', return_value=(KEY + bytes(4), 0, 36)):
                jobs, paths = ss.ue_legacy_jobs([(str(p), 'a.pak')], main_nsos=['fake-main'])
            self.assertEqual(paths, {str(p)}); self.assertTrue(jobs)
            self.assertTrue(all(j[2][2] == KEY for j in jobs))

if __name__ == '__main__': unittest.main()
