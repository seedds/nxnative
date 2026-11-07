"""IoStore shader resolution using synthetic TOCs, libraries and shader groups; no game files or keys."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def chunk_id(n, kind):
    return struct.pack('<QHBB', n, 0, 0, kind)


LIB = chunk_id(1, uepak.IO_SHADER_LIBRARY)
GROUP = chunk_id(2, uepak.IO_SHADER_CODE)
OTHER = chunk_id(3, uepak.IO_SHADER_CODE)


def library(groups):
    """One shader per group: (chunk ID, decoded bytes, stored bytes), frequency pixel."""
    n = len(groups)
    b = struct.pack('<II', 1, 1) + bytes(20) + struct.pack('<I', n) + bytes(20 * n)
    b += struct.pack('<I', n) + b''.join(gid for gid, _, _ in groups)
    b += struct.pack('<III', 1, 0, n) + struct.pack('<I', n)
    b += b''.join(struct.pack('<Q', 3 | i << 4) for i in range(n))
    b += struct.pack('<I', n)
    b += b''.join(struct.pack('<IIII', i, 1, len(raw), len(stored)) for i, (_, raw, stored) in enumerate(groups))
    return b + struct.pack(f'<I{n}I', n, *range(n))


def container(root, name, chunks, encrypted=False, compressed=False, version=8, directory=b''):
    """A minimal TOC with each chunk in a separate 4 KB block. Oodle's test stand-in is zlib."""
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    head = bytearray(144); head[:16] = uepak.TOC_MAGIC; head[16] = version
    struct.pack_into('<9I', head, 20, 144, len(chunks), len(chunks), 12, int(compressed), 32, 4096, len(directory), 1)
    head[80] = (2 if encrypted else 0) | (8 if directory else 0)
    ids, ranges, blocks, data = bytearray(), bytearray(), bytearray(), bytearray()
    for i, (gid, raw) in enumerate(chunks):
        stored = zlib.compress(raw) if compressed else raw
        ids += gid; ranges += (i * 4096).to_bytes(5, 'big') + len(raw).to_bytes(5, 'big')
        blocks += len(data).to_bytes(5, 'little') + len(stored).to_bytes(3, 'little') + len(raw).to_bytes(3, 'little') + bytes([int(compressed)])
        data += stored
    path.write_bytes(head + ids + ranges + blocks + (b'Oodle' + bytes(27) if compressed else b'') + directory)
    path.with_suffix('.ucas').write_bytes(data)
    return path


def fstr(s):
    return struct.pack('<i', len(s) + 1) + s.encode() + b'\0'


def directory(paths, mount='../../../'):
    """FIoDirectoryIndexResource for {chunk index: 'Dir/File'}: the root, one directory per first path part, their files"""
    none = 0xffffffff
    tops = sorted({v.split('/')[0] for v in paths.values()}); strs = tops + sorted(v.split('/')[1] for v in paths.values())
    dirs = [(none, 1 if tops else none, none, none)]; files = []
    for k, top in enumerate(tops):
        first = none
        for chunk, v in sorted(paths.items()):
            if v.split('/')[0] == top: files.append((strs.index(v.split('/')[1]), first, chunk)); first = len(files) - 1
        dirs.append((strs.index(top), none, k + 2 if k + 1 < len(tops) else none, first))
    b = fstr(mount) + struct.pack('<i', len(dirs)) + b''.join(struct.pack('<4I', *d) for d in dirs)
    b += struct.pack('<i', len(files)) + b''.join(struct.pack('<3I', *f) for f in files)
    return b + struct.pack('<i', len(strs)) + b''.join(fstr(x) for x in strs)


def pak11(path, names):
    """A version 11 pak's footer and encoded index, as far as uepak.inline_shaders reads them: only the directory index's names"""
    d = b''.join(n.encode() + b'\0' for n in names)
    index = fstr('../../../') + struct.pack('<iQ', len(names), 0) + struct.pack('<i', 0) + struct.pack('<iQQ', 1, 0, len(d)) + bytes(20)
    path.write_bytes(d + index + bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 11, len(d), len(index)) + bytes(20) + bytes(32 * 5))


def aes_encrypt(key, data):
    import ctypes
    cc = ctypes.CDLL('/usr/lib/libSystem.B.dylib').CCCrypt
    out = ctypes.create_string_buffer(len(data)); moved = ctypes.c_size_t()
    assert cc(0, 0, 2, key, len(key), None, data, len(data), out, len(data), ctypes.byref(moved)) == 0
    return out.raw[:moved.value]


def pad16(b):
    return b + bytes(-len(b) % 16)


def pak11_encrypted(path, names, key, mount='../../../'):
    """pak11 with its index and directory index encrypted (DRAGON BALL Sparking! ZERO); no names: the empty pak beside a container, mount point "/\""""
    d = pad16(b''.join(n.encode() + b'\0' for n in names))
    index = pad16(fstr(mount) + struct.pack('<iQ', len(names), 0) + struct.pack('<i', 0) + struct.pack('<iQQ', 1, 0, len(d)) + bytes(20))
    path.write_bytes(aes_encrypt(key, d) + aes_encrypt(key, index) + bytes(16) + b'\1' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 11, len(d), len(index)) + bytes(20) + bytes(32 * 5))


class IoStore(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name); self.romfs = self.root / 'RomFS'; self.romfs.mkdir()
        self.work = self.root / 'work'

    def run_job(self, decode=None):
        # The NSO and pak paths are outside this test's scope; TOC parsing, resolution and archive writing are real.
        with patch.object(uepak, 'nso_image', return_value=(b'', 0, 0)), patch.object(uepak, 'unreal_paks', return_value=[]):
            return uepak.job(str(self.romfs), 'unused-main', str(self.work), decode=decode or (lambda data, size: data))

    def shaders(self, entry):
        b = (self.work / 'archives' / entry['name']).read_bytes(); a: dict = uepak.archive(b)
        self.assertEqual(a['maps'], [(0, len(a['shaders']))])
        self.assertEqual(a['indices'], list(range(len(a['shaders']))))
        self.assertTrue(all(e[1] == e[2] and e[3] == 3 for e in a['shaders']))
        return [b[a['code'] + e[0]:a['code'] + e[0] + e[1]] for e in a['shaders']]

    def test_single_container_and_finish(self):
        raw = b'LSLG-single-container'
        container(self.romfs, 'Game.utoc', [(LIB, library([(GROUP, raw, raw)])), (GROUP, raw)])
        entries, records = self.run_job()
        self.assertEqual((len(entries), records), (1, 0)); self.assertEqual(self.shaders(entries[0]), [raw])
        out = self.root / 'finished'; self.assertEqual(uepak.finish(str(self.work), str(out)), [entries[0]['name']])
        self.assertEqual((out / entries[0]['name']).read_bytes(), (self.work / 'archives' / entries[0]['name']).read_bytes())
        self.assertEqual(json.loads((self.work / 'job.json').read_text())['records'], [])

    def test_patch_library_uses_base_and_patch_groups(self):
        base, updated = b'LSLG-unchanged', b'LSLG-updated'
        container(self.romfs, 'Game.utoc', [(LIB, library([(GROUP, base, base)])), (GROUP, base)])
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, base, base), (OTHER, updated, updated)])), (OTHER, updated)])
        entries, records = self.run_job()
        self.assertEqual((len(entries), records), (1, 0))
        self.assertEqual(entries[0]['pak'], 'Game_0_P.utoc'); self.assertEqual(self.shaders(entries[0]), [base, updated])

    def test_numeric_patch_priority_and_group_only_override(self):
        raw = b'LSLG-new'
        # Deliberately create in a different order from mount order; 10 must beat 2.
        container(self.romfs, 'Game_10_P.utoc', [(GROUP, raw)])
        container(self.romfs, 'Game_2_P.utoc', [(LIB, library([(GROUP, raw, raw)])), (GROUP, b'LSLG-old')])
        container(self.romfs, 'Game.utoc', [(LIB, library([(OTHER, b'obsolete', b'obsolete')]))])
        entries, _ = self.run_job()
        self.assertEqual(len(entries), 1); self.assertEqual(self.shaders(entries[0]), [raw])
        self.assertEqual(entries[0]['pak'], 'Game_2_P.utoc')

    def test_unnumbered_and_lowercase_patch_priority(self):
        for name in ('A_P.utoc', 'A_p.utoc'):
            with self.subTest(name=name), tempfile.TemporaryDirectory(dir=self.romfs) as td:
                root = Path(td); raw = b'LSLG-patch'
                container(root, 'Z.utoc', [(LIB, library([(GROUP, b'LSLG-base!', b'LSLG-base!')])), (GROUP, b'LSLG-base!')])
                container(root, name, [(LIB, library([(GROUP, raw, raw)])), (GROUP, raw)])
                entries, _ = self.run_job()
                self.assertEqual(self.shaders(entries[0]), [raw])

    def test_two_compression_layers_across_containers(self):
        raw = b'LSLG' + b'compressed shader' * 50; stored = zlib.compress(raw)
        self.assertLess(len(stored), len(raw))
        container(self.romfs, 'Game.utoc', [(GROUP, stored)], compressed=True)
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, raw, stored)]))], compressed=True)
        calls = []
        def decode(data, size):
            out = zlib.decompress(data); self.assertEqual(len(out), size); calls.append(size); return out
        entries, _ = self.run_job(decode)
        self.assertEqual(self.shaders(entries[0]), [raw]); self.assertEqual(len(calls), 3)

    def test_missing_group_names_library_and_chunk(self):
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, b'missing', b'missing')]))])
        with self.assertRaisesRegex(ValueError, rf'Game_0_P.utoc: shader group 0 \({GROUP.hex()}\).*no container'):
            self.run_job()

    def test_encrypted_group_is_refused_before_data_read(self):
        raw = b'LSLG-encrypted'
        container(self.romfs, 'Game.utoc', [(GROUP, raw)], encrypted=True)
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, raw, raw)]))])
        with patch.object(uepak, 'aes_decrypt', side_effect=AssertionError('must not decrypt with a dummy key')) as aes:
            with self.assertRaisesRegex(ValueError, 'Game.utoc:.*encrypted.*no key'):
                self.run_job()
            aes.assert_not_called()

    def test_encrypted_library_is_detected_but_refused_on_read(self):
        container(self.romfs, 'Game.utoc', [(LIB, library([]))], encrypted=True)
        self.assertEqual(uepak.io_shader_tocs(str(self.romfs)), [str(self.romfs / 'Game.utoc')])
        with self.assertRaisesRegex(ValueError, 'Game.utoc:.*encrypted.*no key'):
            self.run_job()

    def test_superseded_encrypted_library_is_not_read(self):
        raw = b'LSLG-patch'
        container(self.romfs, 'Game.utoc', [(LIB, library([]))], encrypted=True)
        container(self.romfs, 'Game_0_P.utoc', [(LIB, library([(GROUP, raw, raw)])), (GROUP, raw)])
        entries, _ = self.run_job()
        self.assertEqual(len(entries), 1); self.assertEqual(self.shaders(entries[0]), [raw])

    def test_irrelevant_encrypted_and_ue4_containers_are_not_read(self):
        container(self.romfs, 'unrelated.utoc', [(OTHER, b'unrelated')], encrypted=True)
        container(self.romfs, 'ue4.utoc', [(LIB, b'not a UE5 library')], version=2)
        (self.romfs / 'not-a-toc.utoc').write_bytes(b'not a toc')
        self.assertEqual(uepak.io_shader_tocs(str(self.romfs)), [])
        self.assertEqual(self.run_job(), ([], 0))

    def test_ue427_export_bundles_are_named_by_the_directory_index(self):
        paths = {0: 'Engine/M_A.uasset', 1: 'Game/M_B.uasset', 2: 'Game/T_C.ubulk'}
        chunks = [(chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), b'a'), (chunk_id(2, uepak.IO_EXPORT_BUNDLE_427), b'b'), (chunk_id(2, 3), b'bulk')]
        t = uepak.Toc(str(container(self.romfs, 'Game.utoc', chunks, version=3, directory=directory(paths))))
        self.assertEqual(t.export_bundles(), [0, 1])
        self.assertEqual(t.names(), paths)

    def test_ue5_numbers_export_bundles_as_type_1(self):
        chunks = [(chunk_id(1, uepak.IO_EXPORT_BUNDLE), b'a'), (chunk_id(2, 2), b'bulk')]
        t = uepak.Toc(str(container(self.romfs, 'Game.utoc', chunks, version=6)))
        self.assertEqual((t.export_bundles(), t.names()), ([0], {}))

    def sage(self, name=b'Global/FShader/0:/Engine/Private/X.usf|MainPS\0', code=b'\x11' * 0x40):
        ctl = uepak.CTL_NVN2 + bytes(0x3c) + struct.pack('<I', len(code)) + bytes(0x98)
        return bytes(0x3e) + struct.pack('<I', len(name)) + name + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl + bytes(12), ctl, code

    def test_a_switch_2_record_gives_its_name_control_and_code(self):
        rec, ctl, code = self.sage()
        self.assertEqual(uepak.sage_record(rec), ('Global/FShader/0:/Engine/Private/X.usf|MainPS', ctl, code))

    def test_a_record_whose_lengths_disagree_is_refused(self):
        rec, ctl, code = self.sage()
        self.assertIsNone(uepak.sage_record(rec.replace(struct.pack('<I', len(code)) + code, struct.pack('<I', len(code) + 4) + code, 1)))
        self.assertIsNone(uepak.sage_record(b'no record here' * 8))

    @staticmethod
    def table(streams):   # FShaderMapResourceCode: hashes, then {i32 length, code, i32 raw size, u8 frequency}
        out = struct.pack('<I', len(streams)) + bytes(20 * len(streams)) + struct.pack('<I', len(streams))
        for b, raw, f in streams: out += struct.pack('<I', len(b)) + b + struct.pack('<IB', raw, f)
        return out

    def test_shader_map_tables_are_walked_whole(self):
        a, b = b'\x8c\x0a' + b'A' * 10, b'\x8c\x0a' + b'B' * 20
        d = b'\x8c\x0a stray stream bytes ' + self.table([(a, 40, 3), (b, 80, 5)]) + b'tail'
        at = d.index(a)
        self.assertEqual(uepak.shader_map_entries(d), [(at, len(a), 40, 3), (at + len(a) + 5 + 4, len(b), 80, 5)])

    def test_a_table_that_does_not_hold_is_not_taken(self):
        a = b'\x8c\x0a' + b'A' * 10
        t = self.table([(a, 40, 3), (a, 40, 3)])
        self.assertEqual(uepak.shader_map_entries(t[:-30]), [])   # its second entry runs past the end
        self.assertEqual(uepak.shader_map_entries(struct.pack('<I', 9) + t[4:]), [])   # the two counts differ

    def inline_pak(self, names=('Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin',)):
        pak11(self.romfs / 'Game.pak', names)
        (self.romfs / 'Game.ucas').write_bytes(b'')
        return str(self.romfs / 'Game.pak')

    def test_inline_shaders_with_the_cooked_packages_in_a_container(self):
        container(self.romfs, 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), b'b'), (chunk_id(2, 3), b'bulk')], version=3)   # Mario & Luigi: Brothership
        self.assertTrue(uepak.inline_shaders(self.inline_pak()))
        container(self.romfs, 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE), b'b'), (chunk_id(2, 2), b'bulk')], version=6)   # Yoshi and the Mysterious Book
        self.assertTrue(uepak.inline_shaders(self.inline_pak()))

    def resonance(self, *patch_chunks):
        """FINAL FANTASY RESONANCE's layout: the pak names the global shader cache, the container has export bundles and the shader libraries, global.utoc only script objects"""
        bundle = (chunk_id(1, uepak.IO_EXPORT_BUNDLE), b'b')
        container(self.romfs, 'Game.utoc', [bundle] if patch_chunks else [bundle, (LIB, b'l'), (GROUP, b'g')], version=8)
        container(self.romfs, 'global.utoc', [(chunk_id(4, 5), b's')], version=8)
        if patch_chunks: container(self.romfs, 'Game_0_P.utoc', list(patch_chunks), version=8)
        return self.inline_pak()

    def test_a_pak_beside_shader_libraries_is_not_inline(self):
        pak = self.resonance()
        self.assertFalse(uepak.inline_shaders(pak))
        self.assertEqual(uepak.unreal_paks(str(self.romfs), inline=False), [pak])   # the NSP script's decode step runs (profile_guess.build_sources: ue_decode)
        self.assertEqual(uepak.unreal_paks(str(self.romfs), inline=True), [])   # and its ue_legacy does not
        self.assertEqual(ss.ue_legacy_jobs([(pak, 'Game.pak'), (str(self.romfs / 'Game.utoc'), 'Game.utoc')]), ([], set()))

    def test_a_pak_beside_shader_libraries_in_a_patch_container_is_not_inline(self):
        self.assertFalse(uepak.inline_shaders(self.resonance((LIB, b'l'))))

    def test_a_global_cache_with_no_export_bundles_the_split_reads_is_not_inline(self):
        """Taken as inline, the split would read the global shaders alone and report no loss; not inline, the NSP script stops at "none … holds a shader code archive\""""
        pak = self.inline_pak()
        self.assertFalse(uepak.inline_shaders(pak))   # no container
        container(self.romfs / 'Other', 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE), b'b')], version=8)
        self.assertFalse(uepak.inline_shaders(pak))   # one in another folder: ue_legacy_jobs reads only the pak's own
        container(self.romfs, 'Game.utoc', [(chunk_id(4, 5), b's')], version=8)
        self.assertFalse(uepak.inline_shaders(pak))   # one without export bundles

    def test_a_pak_with_an_archive_or_only_configs_is_not_inline(self):
        self.assertFalse(uepak.inline_shaders(self.inline_pak(('Engine/GlobalShaderCache-X.bin', 'ShaderArchive-Global-X.ushaderbytecode'))))
        self.assertFalse(uepak.inline_shaders(self.inline_pak(('Engine/Config/a.ini',))))

    KEY = bytes(range(40, 72))

    def sparking_zero(self):
        """DRAGON BALL Sparking! ZERO's layout: pakchunk0's encrypted index names the global shader cache, its encrypted container holds the export bundles (data and directory index encrypted),
        and pakchunk1's pak is empty (mount point "/") beside a container of its own"""
        k = self.KEY
        p0, p1 = self.romfs / 'pakchunk0-Switch.pak', self.romfs / 'pakchunk1-Switch.pak'
        pak11_encrypted(p0, ['Engine/GlobalShaderCache-SP_GLSL_SWITCH.bin'], k); pak11_encrypted(p1, [], k, mount='/')
        raw = b'scanned bundle..' * 2
        toc = container(self.romfs, 'pakchunk0-Switch.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE), raw), (chunk_id(2, 6), b'header' + bytes(10))], encrypted=True, version=5,
                        directory=aes_encrypt(k, pad16(directory({0: 'Game/M_A.uasset'}))))
        ucas = toc.with_suffix('.ucas'); ucas.write_bytes(aes_encrypt(k, ucas.read_bytes()))   # each block a multiple of 16 bytes, so ECB over the file is ECB per block
        container(self.romfs, 'pakchunk1-Switch.utoc', [(chunk_id(3, uepak.IO_EXPORT_BUNDLE), bytes(16))], encrypted=True, version=5)
        img = bytes(0x1000) + os.urandom(64) + k + os.urandom(64)   # main: the key in its read-only data, after a text of 0x1000 bytes
        return str(p0), str(p1), toc, raw, (img, 0x1000, len(img))

    def test_an_encrypted_index_is_read_with_mains_key(self):
        p0, p1, _, _, img = self.sparking_zero()
        self.assertTrue(uepak.inline_shaders(p0, self.KEY))
        self.assertFalse(uepak.inline_shaders(p0))   # no key: counted as one with shader code archives (Octopath Traveler 0), as before
        self.assertIsNone(uepak.inline_shaders(p1, self.KEY))   # names no file: neither kind
        with patch.object(uepak, 'nso_image', return_value=img):
            self.assertEqual(uepak.unreal_paks(str(self.romfs), inline=True, mains=['main']), [p0])
            self.assertEqual(uepak.unreal_paks(str(self.romfs), inline=False, mains=['main']), [])   # no decode step
            self.assertEqual(len(uepak.unreal_paks(str(self.romfs), mains=['main'])), 2)

    def test_split_reads_an_encrypted_container_with_the_key_of_the_pak_beside_it(self):
        p0, _, toc, raw, img = self.sparking_zero()
        with patch.object(uepak, 'nso_image', return_value=img), patch.object(uepak, 'Pak') as pk:
            pk.return_value.mount, pk.return_value.files = '../../../', {}
            files = [(str(f), f.name) for f in sorted(self.romfs.iterdir())]
            jobs, taken = ss.ue_legacy_jobs(files, mains=['main'])
        io = [(Path(p).name, a) for p, _, a in jobs if a[0] == ss.UE_IO]
        # every container in the inline pak's folder is scanned, pakchunk1's too (its empty pak names nothing; the engine mounts all of them), each with the key
        self.assertEqual(io, [('pakchunk0-Switch.utoc', (ss.UE_IO, (0,), self.KEY)), ('pakchunk1-Switch.utoc', (ss.UE_IO, (0,), self.KEY))])
        self.assertIn(str(toc), taken)
        self.assertEqual(uepak.Toc(str(toc)).names(), {})   # without the key the directory index is not read
        seen = []
        ss._PAKS.pop(str(toc), None); self.addCleanup(ss._PAKS.pop, str(toc), None)
        with patch.object(ss, 'ue_inline_shaders', side_effect=lambda d, label, out: seen.append((d, label))):
            ss.ue_io_shaders(str(toc), 'pakchunk0-Switch.utoc', (0,), [], self.KEY)
        self.assertEqual(seen, [(raw, '/pakchunk0-Switch.utoc#Game/M_A.uasset')])

    def split_jobs(self, tocs):
        with patch.object(uepak, 'Pak') as pk:
            pk.return_value.mount, pk.return_value.files = '../../../', {}
            return ss.ue_legacy_jobs([(self.inline_pak(), 'Game.pak')] + [(str(t), t.name) for t in tocs])

    def test_split_takes_the_bundles_and_the_container_beside_an_inline_pak(self):
        a, b = chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), chunk_id(2, uepak.IO_EXPORT_BUNDLE_427)
        toc = container(self.romfs, 'Game.utoc', [(a, b'a'), (b, b'b'), (chunk_id(2, 3), b'bulk')], version=3)
        jobs, taken = self.split_jobs([toc])
        self.assertEqual(sorted(i for _, _, (tag, idx) in jobs if tag == ss.UE_IO for i in idx), [0, 1])
        self.assertEqual(taken, {str(self.romfs / n) for n in ('Game.pak', 'Game.utoc', 'Game.ucas')})

    def test_split_scans_a_patch_containers_copy_of_a_bundle_only(self):
        a, b = chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), chunk_id(2, uepak.IO_EXPORT_BUNDLE_427)
        base = container(self.romfs, 'Game.utoc', [(a, b'old'), (b, b'b')], version=3)
        patched = container(self.romfs, 'Game_0_P.utoc', [(a, b'new')], version=3)
        jobs, _ = self.split_jobs([patched, base])
        got = sorted((Path(p).name, i) for p, _, (tag, idx) in jobs if tag == ss.UE_IO for i in idx)
        self.assertEqual(got, [('Game.utoc', 1), ('Game_0_P.utoc', 0)])

    def test_names_are_joined_to_a_deeper_mount_point(self):   # DRAGON BALL Sparking! ZERO's pakchunk2-17: labelled as their paks' files would be
        toc = container(self.romfs, 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE), b'b')], version=5, directory=directory({0: 'HUD/T_A.uasset'}, '../../../Game/Content/L10N/ja/'))
        self.assertEqual(uepak.Toc(str(toc)).names(), {0: 'Game/Content/L10N/ja/HUD/T_A.uasset'})

    def test_split_labels_a_shader_with_its_package(self):
        raw = b'scanned'
        toc = container(self.romfs, 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), raw)], version=3, directory=directory({0: 'Game/M_A.uasset'}))
        seen = []
        ss._PAKS.pop(str(toc), None); self.addCleanup(ss._PAKS.pop, str(toc), None)
        with patch.object(ss, 'ue_inline_shaders', side_effect=lambda d, label, out: seen.append((d, label))):
            ss.ue_io_shaders(str(toc), 'Game.utoc', (0,), [])
        self.assertEqual(seen, [(raw, '/Game.utoc#Game/M_A.uasset')])


FAKE_HOST = """#!{python} -I
import struct, sys, zlib
sys.stderr.write({say!r}); sys.stderr.flush()
if {gone!r}: sys.exit(1)
i, o = sys.stdin.buffer, sys.stdout.buffer
while h := i.read(16):
    c, n = struct.unpack('<QQ', h); b = i.read(c)
    try: s = zlib.decompress(b[2:])
    except zlib.error: s = b''
    o.write(struct.pack('<Q', n if len(s) == n else 0) + (s if len(s) == n else b'')); o.flush()
"""


class OodleTables(unittest.TestCase):
    """shader_map_entries's tables in ue_inline_shaders, through a stand-in for the Mac host's DM_OODLE_SERVE: an Oodle stream here is its two first bytes and zlib"""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(__import__('shutil').rmtree, self.tmp)
        self.addCleanup(self.stop)
        self.host()

    def stop(self):
        if ss._OODLE:
            p = ss._OODLE[0]; assert p.stdin
            p.stdin.close(); p.wait(); ss._OODLE = None

    def host(self, say='', gone=False):
        self.stop()
        p = self.tmp / 'host'; p.write_text(FAKE_HOST.format(python=sys.executable, say=say, gone=gone)); p.chmod(0o755)
        env = patch.dict(os.environ, {'DM_OODLE_HOST': str(p), 'DM_OODLE_MODULES': 'unused'}); env.start(); self.addCleanup(env.stop)

    @staticmethod
    def record():
        code = struct.pack('<I', 0x0461) + bytes(0x7c)
        name = b'Global/FShader/0:/Engine/Private/X.usf|MainVS\0'
        ctl = uepak.CTL_NVN2 + bytes(0x3c) + struct.pack('<I', len(code)) + bytes(0x98)
        return bytes(0x3e) + struct.pack('<I', len(name)) + name + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl + bytes(12)

    @staticmethod
    def table(streams):
        out = struct.pack('<I', len(streams)) + bytes(20 * len(streams)) + struct.pack('<I', len(streams))
        for b, raw, f in streams: out += struct.pack('<I', len(b)) + b + struct.pack('<IB', raw, f)
        return out

    def real(self):
        rec = self.record()
        return b'\x8c\x0a' + zlib.compress(rec), len(rec), 1

    def export(self):   # an entry of a split package's export list: name number 1, flags 8, size 0xb8c, offset 30650 (OCTOPATH TRAVELER II)
        return self.table([(struct.pack('<q', 0xb8c), 30650, 0)])

    def scan(self, d):
        out = []
        ss.ue_inline_shaders(d, '/Game.pak#M', out)
        return out

    def test_an_export_entry_is_taken_for_a_table_by_the_walk(self):
        self.assertEqual(len(uepak.shader_map_entries(bytes(16) + self.export())), 1)
        self.assertEqual(uepak.shader_map_entries(bytes(16) + self.export(), lambda row: False), [])

    def test_a_stream_the_decoder_refuses_is_no_table(self):
        out = self.scan(bytes(16) + self.export() + self.table([self.real()]))
        self.assertEqual([st for _, st, _, _ in out], ['vs'])

    def test_every_entry_of_a_proved_table_is_read(self):
        self.assertEqual(len(self.scan(bytes(16) + self.table([self.real(), self.real(), self.real()]))), 3)

    def test_a_refused_table_hides_nothing_inside_it(self):
        inner = self.table([self.real()])
        self.assertEqual(len(self.scan(bytes(16) + self.table([(b'\x8c\x0b' + inner, 1 << 20, 0)]))), 1)

    def test_a_stored_stream_that_is_no_record_is_no_table(self):
        self.assertEqual(self.scan(bytes(16) + self.table([(b'\x8c\x0a' + bytes(10), 12, 0)])), [])

    def test_a_stored_record_is_a_table(self):
        rec = b'\x8c\x0a' + self.record()
        self.assertEqual(len(self.scan(bytes(16) + self.table([(rec, len(rec), 1)]))), 1)

    @staticmethod
    def lslg():   # a Switch vertex shader's LSLG record (uepak.lslg): tag, the code area, the control section
        ctl, code = bytearray(0x880), bytearray(0x100)
        ctl[:4] = bytes.fromhex('34127698'); struct.pack_into('<I', ctl, 0x20, len(ctl)); struct.pack_into('<I', ctl, 0x704, len(code))
        code[:4] = bytes.fromhex('78563412'); struct.pack_into('<I', code, 0x30, 1 << 10 | 1)
        return b'LSLGSV' + bytes(2) + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + ctl

    def test_lslg_records_in_an_oodle_table(self):
        """Unreal Engine 5.1 for the Switch (DRAGON BALL Sparking! ZERO's SP_GLSL_SWITCH): the shader map's tables in Oodle, each entry an LSLG record; one stored as it is is taken once"""
        rec = self.lslg()
        packed = (b'\x8c\x0a' + zlib.compress(rec), len(rec), 1)
        out = self.scan(bytes(16) + self.table([packed, packed]))
        self.assertEqual([st for _, st, _, _ in out], ['vs', 'vs'])
        out = self.scan(bytes(16) + self.table([packed, (rec, len(rec), 1)]))
        self.assertEqual(len(out), 2)
        self.assertEqual(len({w for w, *_ in out}), 2)

    def test_a_later_entry_the_decoder_refuses_is_red(self):
        with self.assertRaisesRegex(SystemExit, r"^\[RED\] the game's decoder in the Mac host failed on a 8 → 30650 byte stream"):
            self.scan(bytes(16) + self.table([self.real(), (struct.pack('<q', 0xb8c), 30650, 0)]))

    def test_a_stream_that_decodes_to_another_layout_is_red(self):
        other = zlib.compress(b'no record of this layout' * 4)
        with self.assertRaisesRegex(SystemExit, r"^\[RED\] /Game\.pak#M: the shader record at .* is not a record of the known layout"):
            self.scan(bytes(16) + self.table([(b'\x8c\x0a' + other, 96, 1)]))

    def test_a_host_that_is_gone_is_red(self):
        self.host(say='crashed', gone=True)
        with self.assertRaisesRegex(SystemExit, r"^\[RED\] the game's decoder in the Mac host failed.*\n.*crashed"):
            self.scan(bytes(16) + self.export())

    def test_a_game_with_no_decoder_has_no_table(self):
        self.host(say='DM_OODLE_JOB: main has no "OodleLZ_Decompress" name and no streaming decoder to find (0 functions)', gone=True)
        self.assertEqual(self.scan(bytes(16) + self.export()), [])

    def test_a_package_that_does_not_read_costs_only_itself(self):
        bad = bytes(16) + self.table([self.real(), (struct.pack('<q', 0xb8c), 30650, 0)])
        good = bytes(16) + self.table([self.real()])
        out, said = [], io.StringIO()
        with contextlib.redirect_stdout(said):
            extra = ss.ue_packages([('/Game.pak#A', lambda: bad), ('/Game.pak#B', lambda: good)], out)
        self.assertEqual({w.split('#')[1] for w, *_ in out}, {'A', 'B'})   # the first entry of A was read before its second failed
        assert extra
        self.assertEqual((extra[0], extra[2]), ('unreadable', 1))
        self.assertTrue(extra[1].startswith("/Game.pak#A: the game's decoder"), extra[1])
        self.assertEqual(said.getvalue().count('[RED] /Game.pak#A: '), 1)

    def test_only_the_first_failures_of_a_job_get_a_line(self):
        def fail(): raise ValueError('cut short')
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            extra = ss.ue_packages([(f'/Game.pak#{i}', fail) for i in range(20)], [])
        self.assertEqual(extra, ('unreadable', '/Game.pak#0: ValueError: cut short', 20))
        self.assertEqual(said.getvalue().count('\n'), 8)

    def test_nothing_lost_gives_no_extra(self):
        self.assertIsNone(ss.ue_packages([('/Game.pak#A', lambda: b'')], []))

    def test_a_job_keeps_what_it_read_and_says_how_many_it_lost(self):
        p = self.tmp / 'Game.pak'; p.write_bytes(bytes(16))
        ctl = uepak.sage_record(self.record()); assert ctl
        def job(path, rel, names, out):
            out.append(('/Game.pak#B', 'vs', ctl[1], ctl[2]))
            return ('unreadable', '/Game.pak#A: why', 3)
        with patch.object(ss, 'ue_legacy_shaders', side_effect=job):
            rel, keys, _, extra = ss.scan_file((str(p), 'Game.pak', (ss.UE_LEGACY, ('A', 'B'))))
        self.assertEqual((rel, len(keys), extra), ('Game.pak', 1, ('unreadable', '/Game.pak#A: why', 3)))


if __name__ == '__main__': unittest.main()
