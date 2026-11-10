"""Obsolete compiled cache entries leave after a full build; live outputs and writers stay."""
import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nsp_cache_prune', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
CHAIN, LIVE, OLD, NEW = '0123456789ab', 'a' * 64, 'b' * 64, 'c' * 64


class CompiledCache(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.sh = Path(tmp.name) / 'shader'; self.db = self.sh / 'cache/cas.db'
        self.mac = self.sh / 'cache' / f'b-{CHAIN}'; self.ios = self.sh / 'ios' / f'b-{CHAIN}'
        self.mac.mkdir(parents=True); self.ios.mkdir(parents=True)
        for d in (self.mac, self.ios):
            (d / 'inputs.tsv').write_text('game\t0400486024DEC000\ninputs\t0123456789ab\n')
        (self.mac / '_b_cas.txt').write_text(LIVE + '\n')
        (self.mac / 'key_b.metallib').write_bytes(b'Mac base')
        (self.mac / 'key_bp.metallib').write_bytes(b'Mac pulling')
        (self.ios / 'key_b.metallib').write_bytes(b'iPhone base')
        self.said = []
        patch = mock.patch.object(nsp, 'say', self.said.append); patch.start(); self.addCleanup(patch.stop)

    def put(self, entries, when=1.0):
        s = nsp.shader_cache.Store(self.db)
        try:
            for name, data in entries.items(): s.put(name, data, when)
        finally: s.close()

    def contents(self):
        s = nsp.shader_cache.Store(self.db)
        try: return {name: s.get(name) for name in s.names('')}
        finally: s.close()

    def test_obsolete_compiles_go_but_live_outputs_and_split_siblings_stay(self):
        keep = {f'metallib/mac/{LIVE}.metallib': b'Mac base',
                f'metallib/mac/{LIVE}_x.metallib': b'split sibling',
                f'metallib/mac/{NEW}.metallib': b'Mac pulling',
                f'metallib/ios/{LIVE}.metallib': b'iPhone base',
                'metallib/filled.json': b'{}', 'metallib/mac/not-a-digest.metallib': b'unknown',
                'variant/not-a-digest/metal': b'unknown variant'}
        obsolete = {f'metallib/mac/{OLD}.metallib': b'old library', f'metallib/mac/{OLD}_x2.metallib': b'old split',
                    f'variant/{OLD}/metal': b'old MSL', f'variant/{OLD}/mac.metallib': b'old variant'}
        self.put({**keep, **obsolete})
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), keep)
        self.assertTrue(any('removed old compiled shader cache entries: 4' in s for s in self.said))

    def test_mac_only_build_prunes_without_an_iphone_library(self):
        nsp.shutil.rmtree(self.ios)
        keep = {f'metallib/mac/{LIVE}.metallib': b'Mac base'}
        self.put({**keep, f'metallib/mac/{OLD}.metallib': b'old'})
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), keep)

    def test_packed_images_preserve_both_targets_and_every_variant(self):
        keep = {f'metallib/mac/{LIVE}.metallib': b'Mac pulling', f'metallib/ios/{LIVE}.metallib': b'iPhone base'}
        self.put({**keep, f'metallib/mac/{OLD}.metallib': b'old'})
        for d in (self.mac, self.ios): self.assertTrue(nsp.shader_lib.pack(d))
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), keep)

    def test_recent_entries_and_surviving_shader_or_stage_references_stay(self):
        keep = {f'shader/{LIVE}/mac/k_b.metallib': b'shared', f'metallib/mac/{LIVE}.metallib': b'shared',
                'stage/other/mac/k_bp.metallib': b'staged', f'metallib/mac/{NEW}.metallib': b'staged',
                f'variant/{LIVE}/metal': b'variant source', f'variant/{LIVE}/mac.metallib': b'Mac base'}
        self.put(keep)
        fresh = {f'metallib/mac/{OLD}.metallib': b'new', f'variant/{NEW}/metal': b'new source'}
        self.put(fresh, when=time.time() + 3600)
        self.put({f'variant/{NEW}/mac.metallib': b'older part'})
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), {**keep, **fresh, f'variant/{NEW}/mac.metallib': b'older part'})

    def test_another_open_store_defers_compiled_cleanup(self):
        old = {f'metallib/mac/{OLD}.metallib': b'old'}; self.put(old)
        other = nsp.shader_cache.Store(self.db)
        try:
            nsp.prune_shader_cache(self.sh, CHAIN)
            self.assertEqual(other.get(next(iter(old))), b'old')
        finally: other.close()
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), {})

    def test_a_drive_without_locks_keeps_compiles(self):
        old = {f'metallib/mac/{OLD}.metallib': b'old'}; self.put(old)
        with mock.patch.object(nsp.shader_cache.fcntl, 'flock', side_effect=OSError('no locks')):
            nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), old)

    def test_no_full_build_list_or_unstamped_target_keeps_compiles(self):
        old = {f'metallib/mac/{OLD}.metallib': b'old'}; self.put(old)
        (self.mac / '_b_cas.txt').unlink()
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), old)
        (self.mac / '_b_cas.txt').write_text(LIVE + '\n')
        (self.ios / 'inputs.tsv').unlink()
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), old)

    def test_failed_library_read_leaves_compiles_and_reports_the_error(self):
        old = {f'metallib/mac/{OLD}.metallib': b'old'}; self.put(old)
        original = nsp.shader_lib.romfs.Path.open
        def read(p, *args, **kwargs):
            if p.name.endswith('.metallib'): raise OSError('unreadable library')
            return original(p, *args, **kwargs)
        with mock.patch.object(nsp.shader_lib.romfs.Path, 'open', read): nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), old)
        self.assertTrue(any('[RED]' in s and 'unreadable library' in s for s in self.said))

    def test_unreadable_library_directory_keeps_all_compiles(self):
        entries = {f'metallib/mac/{LIVE}.metallib': b'Mac pulling', f'metallib/mac/{OLD}.metallib': b'old'}; self.put(entries)
        original = os.scandir
        def listing(path):
            if Path(path) == self.mac: raise PermissionError('cannot list Mac libraries')
            return original(path)
        with mock.patch.object(os, 'scandir', listing): nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), entries)
        self.assertTrue(any('[RED]' in s and 'cannot list Mac libraries' in s for s in self.said))

    def test_inaccessible_target_is_not_treated_as_absent(self):
        entries = {f'metallib/ios/{LIVE}.metallib': b'iPhone base', f'metallib/mac/{OLD}.metallib': b'old'}; self.put(entries)
        original = os.stat
        def stat(path, *args, **kwargs):
            if str(path) in (str(self.ios), str(self.ios) + '.img'): raise PermissionError('cannot inspect iPhone libraries')
            return original(path, *args, **kwargs)
        with mock.patch.object(os, 'stat', stat): nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertEqual(self.contents(), entries)
        self.assertTrue(any('[RED]' in s and 'cannot inspect iPhone libraries' in s for s in self.said))

    def assert_parent_lease(self):
        fd = os.open(self.db.parent, os.O_RDONLY)
        try:
            with self.assertRaises(BlockingIOError): nsp.shader_cache.fcntl.flock(fd, nsp.shader_cache.fcntl.LOCK_EX | nsp.shader_cache.fcntl.LOCK_NB)
        finally: os.close(fd)

    def test_packing_holds_a_shared_cache_lease(self):
        original = nsp.shader_lib.pack
        def pack(*args, **kwargs):
            self.assert_parent_lease()
            return original(*args, **kwargs)
        with mock.patch.object(nsp.shader_lib, 'pack', pack): nsp.pack_libraries(self.sh, CHAIN)

    def test_old_chain_removal_holds_a_shared_cache_lease(self):
        old = self.mac.parent / 'b-deadbeefabcd'; old.mkdir()
        original = nsp.rmtree
        def remove(path):
            self.assert_parent_lease()
            return original(path)
        with mock.patch.object(nsp, 'rmtree', remove): nsp.prune_stale_chains(self.sh, CHAIN)
        self.assertFalse(old.exists())

    def test_pruning_returns_the_obsolete_blobs_disk_pages(self):
        self.put({f'metallib/mac/{i:064x}.metallib': os.urandom(100000) for i in range(40)})
        big = self.db.stat().st_size
        nsp.prune_shader_cache(self.sh, CHAIN)
        self.assertLess(self.db.stat().st_size, big // 4)


if __name__ == '__main__': unittest.main()
