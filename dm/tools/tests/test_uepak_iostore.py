"""IoStore shader resolution using synthetic TOCs, libraries and shader groups; no game files or keys."""
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


def directory(paths):
    """FIoDirectoryIndexResource for {chunk index: 'Dir/File'}: the root, one directory per first path part, their files"""
    none = 0xffffffff
    tops = sorted({v.split('/')[0] for v in paths.values()}); strs = tops + sorted(v.split('/')[1] for v in paths.values())
    dirs = [(none, 1 if tops else none, none, none)]; files = []
    for k, top in enumerate(tops):
        first = none
        for chunk, v in sorted(paths.items()):
            if v.split('/')[0] == top: files.append((strs.index(v.split('/')[1]), first, chunk)); first = len(files) - 1
        dirs.append((strs.index(top), none, k + 2 if k + 1 < len(tops) else none, first))
    b = fstr('../../../') + struct.pack('<i', len(dirs)) + b''.join(struct.pack('<4I', *d) for d in dirs)
    b += struct.pack('<i', len(files)) + b''.join(struct.pack('<3I', *f) for f in files)
    return b + struct.pack('<i', len(strs)) + b''.join(fstr(x) for x in strs)


def pak11(path, names):
    """A version 11 pak's footer and encoded index, as far as uepak.inline_shaders reads them: only the directory index's names"""
    d = b''.join(n.encode() + b'\0' for n in names)
    index = fstr('../../../') + struct.pack('<iQ', len(names), 0) + struct.pack('<i', 0) + struct.pack('<iQQ', 1, 0, len(d)) + bytes(20)
    path.write_bytes(d + index + bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 11, len(d), len(index)) + bytes(20) + bytes(32 * 5))


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

    def test_ue5_numbering_has_no_export_bundles(self):
        t = uepak.Toc(str(container(self.romfs, 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), b'a')], version=4)))
        self.assertEqual((t.export_bundles(), t.names()), ([], {}))

    def inline_pak(self, names=('Engine/GlobalShaderCache-SF_GLSL_SWITCH.bin',)):
        pak11(self.romfs / 'Game.pak', names)
        (self.romfs / 'Game.ucas').write_bytes(b'')
        return str(self.romfs / 'Game.pak')

    def test_inline_shaders_with_the_cooked_packages_in_a_container(self):
        self.assertTrue(uepak.inline_shaders(self.inline_pak()))

    def test_a_pak_with_an_archive_or_only_configs_is_not_inline(self):
        self.assertFalse(uepak.inline_shaders(self.inline_pak(('Engine/GlobalShaderCache-X.bin', 'ShaderArchive-Global-X.ushaderbytecode'))))
        self.assertFalse(uepak.inline_shaders(self.inline_pak(('Engine/Config/a.ini',))))

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

    def test_split_labels_a_shader_with_its_package(self):
        raw = b'scanned'
        toc = container(self.romfs, 'Game.utoc', [(chunk_id(1, uepak.IO_EXPORT_BUNDLE_427), raw)], version=3, directory=directory({0: 'Game/M_A.uasset'}))
        seen = []
        ss._PAKS.pop(str(toc), None); self.addCleanup(ss._PAKS.pop, str(toc), None)
        with patch.object(ss, 'ue_inline_shaders', side_effect=lambda d, label, out: seen.append((d, label))):
            ss.ue_io_shaders(str(toc), 'Game.utoc', (0,), [])
        self.assertEqual(seen, [(raw, '/Game.utoc#Game/M_A.uasset')])


if __name__ == '__main__': unittest.main()
