#!/usr/bin/env python3
# The compile cache as one file (dm/tools/shader_cache.py): entries in and out, bytes stored once, several threads and processes, a damaged file, pruning, and taking in a cache from before it.
import contextlib, os, sqlite3, subprocess, sys, tarfile, tempfile, threading, time, unittest
from unittest import mock
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import shader_cache

D1, D2, D3 = 'a' * 64, 'b' * 64, 'c' * 64
FORKED = None   # the store test_forked_workers_share_it's workers inherit


def forked_work(i):
    s = FORKED; assert s is not None
    for j in range(300): s.put(f'f{i}/{j}', f'{i}'.encode() * 1000); assert s.get(f'f{i}/{j}') == f'{i}'.encode() * 1000
    return s.get('before')


class Base(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name); self.path = self.root / 'cas.db'; self.said = []
        self.store = shader_cache.Store(self.path, self.said.append); self.addCleanup(self.store.close)

    def blobs(self): return self.store.db.execute('SELECT count(*) FROM blob').fetchone()[0]


class Entries(Base):
    def test_in_and_out(self):
        self.store.put('x/y', b'bytes')
        self.assertEqual(self.store.get('x/y'), b'bytes'); self.assertTrue(self.store.has('x/y'))
        self.assertIsNone(self.store.get('absent')); self.assertFalse(self.store.has('absent'))
        self.store.put('x/y', b'new'); self.assertEqual(self.store.get('x/y'), b'new')

    def test_empty_entry(self):
        self.store.put('e', b''); self.assertEqual(self.store.get('e'), b''); self.assertTrue(self.store.has('e'))
        out = self.root / 'out'; self.assertTrue(self.store.to_file('e', out)); self.assertEqual(out.read_bytes(), b'')

    def test_names_take_a_prefix_only(self):
        for n in ('shader/a/mac/x', 'shader/a/ios/x', 'shader/ab/mac/x', 'shader0'): self.store.put(n, b'1')
        self.assertEqual(self.store.names('shader/a/'), {'shader/a/mac/x', 'shader/a/ios/x'})

    def test_delete_prefix_takes_only_its_names_and_bytes_shared_stay(self):   # dmsh_build.py's staged outputs, once its image is written
        self.store.put('stage/x/mac/k_b.metallib', b'shared'); self.store.put('stage/x/mac/k_b.json', b'staged only')
        self.store.put('shader/d/mac/k_b.metallib', b'shared'); self.store.put('stage/xy', b'another stage')
        self.store.delete_prefix('stage/x/')
        self.assertEqual(self.store.names('stage/'), {'stage/xy'})
        self.assertEqual(self.store.get('shader/d/mac/k_b.metallib'), b'shared')
        self.assertEqual(self.blobs(), 2)   # 'staged only' gave its bytes back; 'shared' is still named

    def test_the_same_bytes_are_stored_once(self):   # a library under its MSL's digest and its shader's digest was one APFS clone
        self.store.put('metallib/mac/d.metallib', b'L' * 100000); self.store.put('shader/d/mac/k_b.metallib', b'L' * 100000)
        self.assertEqual(self.blobs(), 1)
        self.assertEqual(self.store.get('shader/d/mac/k_b.metallib'), b'L' * 100000)

    def test_to_file_and_from_file(self):
        src, dst = self.root / 'src', self.root / 'dst'; src.write_bytes(b'data')
        self.store.put_file('f', src)
        self.assertTrue(self.store.to_file('f', dst)); self.assertEqual(dst.read_bytes(), b'data')
        self.assertFalse(self.store.to_file('absent', self.root / 'none')); self.assertFalse((self.root / 'none').exists())

    def test_one_file_is_left(self):
        self.store.put('x', b'1'); self.store.close()
        self.assertEqual(set(os.listdir(self.root)), {'cas.db', 'cas.db.lock'})
        again = shader_cache.Store(self.path); self.addCleanup(again.close)
        self.assertEqual(again.get('x'), b'1')


def enable_compression(path):
    db = sqlite3.connect(path, isolation_level=None)
    try:
        db.enable_load_extension(True); db.load_extension(str(shader_cache.ZSTD_LIBRARY)); db.enable_load_extension(False)
        db.execute('SELECT zstd_enable_transparent(?)', ('{"table":"blob","column":"data","compression_level":3,"dict_chooser":"\'[nodict]\'"}',)).fetchall()
    finally: db.close()


class CompressedEntries(Entries):
    def setUp(self):
        super().setUp(); self.store.close(); enable_compression(self.path)
        self.store = shader_cache.Store(self.path, self.said.append); self.addCleanup(self.store.close)

    def test_distinct_insert_ids(self):
        for name, data in [('a', b'A'), ('b', b'B'), ('c', b'C'), ('alias', b'B')]: self.store.put(name, data)
        self.assertEqual([self.store.get(n) for n in ('a', 'b', 'c', 'alias')], [b'A', b'B', b'C', b'B'])
        self.assertEqual(self.blobs(), 3)

    def test_missing_extension_preserves_source(self):
        self.store.put('keep', b'precious'); self.store.close()
        before = self.path.read_bytes()
        with mock.patch.object(shader_cache, 'ZSTD_LIBRARY', self.root / 'absent.dylib'):
            with self.assertRaisesRegex(RuntimeError, 'sqlite-zstd'): shader_cache.Store(self.path)
        self.assertEqual(self.path.read_bytes(), before)
        s = shader_cache.Store(self.path); self.addCleanup(s.close)
        self.assertEqual(s.get('keep'), b'precious')


class Writers(Base):
    def test_threads_share_it(self):
        def work(i):
            for j in range(200):
                self.store.put(f't{i}/{j}', f'{i}.{j}'.encode()); self.assertEqual(self.store.get(f't{i}/{j}'), f'{i}.{j}'.encode())
        ts = [threading.Thread(target=work, args=(i,)) for i in range(8)]
        for t in ts: t.start()
        for t in ts: t.join()
        self.assertEqual(len(self.store.names('t')), 1600)

    def test_a_batch_is_all_or_nothing(self):
        with self.assertRaises(ZeroDivisionError):
            with self.store.batch():
                self.store.put('a', b'1'); self.store.put('b', b'2'); raise ZeroDivisionError
        self.assertEqual(self.store.names(''), set())
        with self.store.batch():
            with self.store.batch(): self.store.put('a', b'1')
            self.store.put('b', b'2')
        self.assertEqual(self.store.names(''), {'a', 'b'})

    def test_processes_share_it(self):   # regress.py's chain builds and the NSP script on one game's file
        code = ("import sys; sys.path.insert(0, sys.argv[1]); import shader_cache\ns = shader_cache.Store(sys.argv[2])\n"
                "for j in range(300): s.put(f'{sys.argv[3]}/{j}', sys.argv[3].encode() * 1000); assert s.get(f'{sys.argv[3]}/{j}') == sys.argv[3].encode() * 1000\ns.close()\n")
        ps = [subprocess.Popen([sys.executable, '-c', code, str(REPO / 'dm/tools'), str(self.path), f'p{i}']) for i in range(4)]
        self.assertEqual([p.wait() for p in ps], [0, 0, 0, 0])
        self.assertEqual(len(self.store.names('p')), 1200)

    def test_forked_workers_share_it(self):   # dmsh_build.py's worker processes: forks of a process that has the file open, each with a connection of its own
        import concurrent.futures as cf, multiprocessing
        global FORKED; FORKED = self.store
        self.store.put('before', b'0')
        with cf.ProcessPoolExecutor(4, mp_context=multiprocessing.get_context('fork'), initializer=self.store.forked) as ex:
            self.assertEqual(list(ex.map(forked_work, range(4))), [b'0'] * 4)
        self.assertEqual(len(self.store.names('f')), 1200)
        self.store.put('after', b'1'); self.assertEqual(self.store.get('after'), b'1')   # the inherited connection still works here

    def test_a_killed_writer_loses_only_what_it_had_not_committed(self):
        code = ("import os, sys; sys.path.insert(0, sys.argv[1]); import shader_cache\ns = shader_cache.Store(sys.argv[2]); s.put('done', b'1')\n"
                "s.db.execute('BEGIN IMMEDIATE'); s.put('half', b'2'); os.kill(os.getpid(), 9)\n")
        subprocess.run([sys.executable, '-c', code, str(REPO / 'dm/tools'), str(self.path)])
        self.assertEqual(self.store.names(''), {'done'})


class Damage(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.path = Path(t.name) / 'cas.db'; self.said = []

    def test_a_file_that_is_no_database_costs_the_work_it_held(self):
        self.path.write_bytes(b'not a database at all' * 2000)
        s = shader_cache.Store(self.path, self.said.append); self.addCleanup(s.close)
        self.assertIn('[RED]', self.said[0])
        s.put('x', b'1'); self.assertEqual(s.get('x'), b'1')

    def test_damage_found_in_use(self):
        s = shader_cache.Store(self.path, self.said.append)
        for i in range(300): s.put(f'x{i}', os.urandom(40000))
        s.close()
        b = bytearray(self.path.read_bytes()); b[16384:len(b) // 2] = b'\xee' * (len(b) // 2 - 16384); self.path.write_bytes(b)
        s = shader_cache.Store(self.path, self.said.append)
        for i in range(300): s.get(f'x{i}')   # a miss or an entry, never an exception
        s.put('after', b'1'); self.assertEqual(s.get('after'), b'1')
        s.close()
        self.assertTrue(any('[RED]' in m for m in self.said))


class Prune(Base):
    def shader(self, digest, when):
        self.store.put(f'shader/{digest}/mac/k_b.metal', digest.encode() * 10, when); self.store.put(f'shader/{digest}/mac/k_b.metallib', b'L' + digest.encode(), when)

    def test_old_ones_go_with_their_bytes(self):
        now = time.time()
        for d in (D1, D2): self.shader(d, now - 100)
        self.shader(D3, now + 100); self.store.put('shader/notadigest/mac/x', b'k', now - 100); self.store.put('metallib/mac/m.metallib', b'L' + D2.encode(), now - 100)
        before = self.blobs()
        self.assertEqual(self.store.prune_shaders({D1}, now), (1, 640))   # the source's bytes, not the library's
        self.assertEqual({n.split('/')[1] for n in self.store.names('shader/')}, {D1, D3, 'notadigest'})
        self.assertEqual(self.blobs(), before - 1)   # D2's source; its library's bytes stay under metallib/
        self.assertEqual(self.store.get('metallib/mac/m.metallib'), b'L' + D2.encode())

    def test_the_file_gives_back_its_pages(self):
        now = time.time()
        for i in range(40): self.store.put(f'shader/{i:064x}/mac/k_b.metal', os.urandom(100000), now - 100)
        self.store.db.execute('PRAGMA wal_checkpoint(TRUNCATE)'); big = self.path.stat().st_size
        self.assertEqual(self.store.prune_shaders(set(), now)[0], 40)
        self.store.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        self.assertLess(self.path.stat().st_size, big // 4)


class Adopt(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name); self.old = self.root / 'cas'; self.path = self.root / 'cas.db'; self.said = []

    def fill(self):
        for name, data in (('metallib/mac/ab/abcd.metallib', b'compiled'), ('metallib/mac/ab/abcd_x.metallib', b'cut'), ('metallib/ios/cd/cdef.metallib', b'ios'), ('metallib/filled.json', b'{}'),
                           (f'shader/{D1}/mac/k_b.metal', b'old chain'), ('index/k.json', b'{}'), ('variant/ee/metal', b'msl'), ('variant/ee/mac.metallib', b'v'), ('metallib/mac/ab/._abcd.metallib', b'appledouble')):
            p = self.old / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)

    def check(self):
        s = shader_cache.Store(self.path)
        self.assertEqual(s.names(''), {'metallib/mac/abcd.metallib', 'metallib/mac/abcd_x.metallib', 'metallib/ios/cdef.metallib'})
        self.assertEqual(s.get('metallib/mac/abcd.metallib'), b'compiled')
        s.close()
        self.assertEqual(sorted(os.listdir(self.root)), ['cas.db', 'cas.db.lock'])

    def test_a_folder(self):
        self.fill(); self.assertEqual(shader_cache.adopt(self.old, self.path, self.said.append), 3); self.check()

    def test_a_tar_read_without_unpacking(self):
        self.fill()
        with tarfile.open(f'{self.old}.tar', 'w', format=tarfile.PAX_FORMAT) as t: t.add(self.old, '.')
        import shutil; shutil.rmtree(self.old)
        self.assertEqual(shader_cache.adopt(self.old, self.path, self.said.append), 3); self.check()

    def test_a_damaged_tar_costs_what_it_held(self):
        Path(f'{self.old}.tar').write_bytes(b'not a tar' * 100)
        self.assertEqual(shader_cache.adopt(self.old, self.path, self.said.append), 0)
        self.assertFalse(Path(f'{self.old}.tar').exists()); self.assertTrue(any('[RED]' in m for m in self.said))

    def test_nothing_there(self):
        self.assertEqual(shader_cache.adopt(self.old, self.path, self.said.append), 0)
        self.assertEqual(os.listdir(self.root), []); self.assertFalse(shader_cache.exists(self.path))

    def test_what_a_stopped_build_left(self):
        for p in (f'{self.old}.gone', f'{self.old}.unpack'): os.makedirs(p); (Path(p) / 'x').write_bytes(b'1')
        Path(f'{self.old}.tar.part').write_bytes(b'1')
        shader_cache.adopt(self.old, self.path, self.said.append)
        self.assertEqual(os.listdir(self.root), [])


class CompressedWriters(Writers):
    def setUp(self):
        super().setUp(); self.store.close(); enable_compression(self.path)
        self.store = shader_cache.Store(self.path, self.said.append); self.addCleanup(self.store.close)


class CompressedPrune(Prune):
    def setUp(self):
        super().setUp(); self.store.close(); enable_compression(self.path)
        self.store = shader_cache.Store(self.path, self.said.append); self.addCleanup(self.store.close)


class Maintenance(Base):
    def test_initial_compression_skips_reclamation_and_handles_sparse_ids(self):
        self.fill()
        with contextlib.closing(sqlite3.connect(self.path)) as db, db:
            db.execute('UPDATE blob SET id=-4 WHERE id=1')
            db.execute('UPDATE entry SET blob=-4 WHERE blob=1')
            db.execute('UPDATE blob SET id=9999 WHERE id=2')
            db.execute('UPDATE entry SET blob=9999 WHERE blob=2')
        before = self.snapshot()
        with mock.patch.object(shader_cache, '_reclaim', side_effect=AssertionError('reclaimed initial conversion')):
            shader_cache.maintain(self.path, compress=True, workers=4, say=self.said.append)
        self.assertEqual(self.snapshot(), before)
        with contextlib.closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute('PRAGMA freelist_count').fetchone(), (0,))
        self.assertFalse(Path(f'{self.path}.compressed.part').exists())

    def test_failed_compressed_replacement_preserves_raw_source(self):
        self.fill(); before = self.snapshot()
        Path(f'{self.path}.compressed.part').write_bytes(b'abandoned')
        with mock.patch.object(shader_cache.os, 'replace', side_effect=OSError('disk full')):
            with self.assertRaisesRegex(OSError, 'disk full'):
                shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(Path(f'{self.path}.compressed.part').exists())
        with contextlib.closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute("SELECT type FROM sqlite_master WHERE name='blob'").fetchone(), ('table',))

    def test_compressed_partial_write_failure_preserves_source(self):
        self.fill(); before = self.snapshot()
        def fail_write(db, path, part, workers):
            Path(part).write_bytes(b'partial'); raise OSError('no space')
        with mock.patch.object(shader_cache, '_compressed_copy', side_effect=fail_write):
            with self.assertRaisesRegex(OSError, 'no space'):
                shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(Path(f'{self.path}.compressed.part').exists())

    def test_compressed_worker_failure_preserves_source(self):
        self.fill(); before = self.snapshot()
        with mock.patch.object(shader_cache.cf, 'ThreadPoolExecutor') as executor:
            executor.return_value.__enter__.return_value.submit.side_effect = RuntimeError('worker failed')
            with self.assertRaisesRegex(RuntimeError, 'worker failed'):
                shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(Path(f'{self.path}.compressed.part').exists())

    def test_compression_does_not_scan_output_for_quality_checks(self):
        self.fill(); before = self.snapshot(); statements = []; load = shader_cache._load_zstd
        def observe(db):
            load(db); db.set_trace_callback(statements.append)
        with mock.patch.object(shader_cache, '_load_zstd', side_effect=observe):
            shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        self.assertFalse(any('quick_check' in sql.lower() for sql in statements))
        self.assertFalse(any(sql.startswith('SELECT id,hash FROM blob') for sql in statements))
        self.assertEqual(self.snapshot(), before)

    def test_private_compression_uses_rollback_journal_then_installs_wal(self):
        self.fill(); before = self.snapshot(); seen = []; load = shader_cache._load_zstd
        def observe(db):
            load(db)
            if db.execute('PRAGMA database_list').fetchone()[2].endswith('.compressed.part'):
                seen.append((db.execute('PRAGMA journal_mode').fetchone()[0], db.execute('PRAGMA synchronous').fetchone()[0]))
        with mock.patch.object(shader_cache, '_load_zstd', side_effect=observe):
            shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        self.assertEqual(seen, [('delete', 1)])
        with contextlib.closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute('PRAGMA journal_mode').fetchone(), ('wal',))
        self.assertEqual(self.snapshot(), before)

    def test_wal_maintenance_uses_store_sync_policy(self):
        self.fill(); seen = []; compress = shader_cache._compressed_copy
        def observe(db, *args):
            seen.append((db.execute('PRAGMA journal_mode').fetchone()[0], db.execute('PRAGMA synchronous').fetchone()[0]))
            return compress(db, *args)
        with mock.patch.object(shader_cache, '_compressed_copy', side_effect=observe):
            shader_cache.maintain(self.path, compress=True, workers=4, say=self.said.append)
        self.assertEqual(seen, [('wal', 1)])

    def test_reversal_decodes_source_once(self):
        self.fill(); before = self.snapshot()
        shader_cache.maintain(self.path, compress=True, workers=4, say=self.said.append)
        with mock.patch.object(shader_cache, '_parallel_blobs', wraps=shader_cache._parallel_blobs) as decode:
            shader_cache.maintain(self.path, decompress=True, workers=4, say=self.said.append)
            self.assertEqual(decode.call_count, 1)
        self.assertEqual(self.snapshot(), before)

    def test_parallel_conversion_round_trip(self):
        self.fill(); before = self.snapshot()
        for workers in (2, 4):
            shader_cache.maintain(self.path, compress=True, workers=workers, say=self.said.append)
            self.assertEqual(self.snapshot(), before)
            shader_cache.maintain(self.path, decompress=True, workers=workers, say=self.said.append)
            self.assertEqual(self.snapshot(), before)

    def fill(self):
        for i in range(12): self.store.put(f'item/{i}', bytes([i]) + b'shader source' * 80000, when=123 + i)
        self.store.put('alias', bytes([3]) + b'shader source' * 80000, when=456)
        self.store.put('empty', b'', when=789)
        self.store.close()

    def snapshot(self):
        s = shader_cache.Store(self.path)
        try: return (s.db.execute('SELECT id,hash,data FROM blob ORDER BY id').fetchall(), s.db.execute('SELECT * FROM entry ORDER BY name').fetchall())
        finally: s.close()

    def test_round_trip_and_reclamation(self):
        self.fill(); before = self.snapshot(); size = self.path.stat().st_size
        shader_cache.maintain(self.path, compress=True, say=self.said.append)
        self.assertLess(self.path.stat().st_size, size // 3)
        self.assertEqual(self.snapshot(), before)
        shader_cache.maintain(self.path, decompress=True, say=self.said.append)
        self.assertEqual(self.snapshot(), before)
        with contextlib.closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute("SELECT type FROM sqlite_master WHERE name='blob'").fetchone(), ('table',))
        self.assertFalse(Path(f'{self.path}.raw.part').exists())

    def test_maintenance_compresses_only_new_rows(self):
        self.fill(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        def backing():
            with contextlib.closing(sqlite3.connect(self.path)) as db: return db.execute('SELECT id,data,_data_dict FROM _blob_zstd ORDER BY id').fetchall()
        before = backing(); shader_cache.maintain(self.path, say=self.said.append)
        self.assertEqual(backing(), before)
        s = shader_cache.Store(self.path); s.put('new', b'new' * 100000); s.close()
        self.assertIsNone(backing()[-1][2])
        shader_cache.maintain(self.path, say=self.said.append)
        self.assertIsNotNone(backing()[-1][2]); self.assertEqual(backing()[:-1], before)

    def test_empty_only_cache_and_appended_empty_blob(self):
        self.store.put('empty', b''); self.store.close()
        shader_cache.maintain(self.path, compress=True, say=self.said.append)
        shader_cache.maintain(self.path, say=self.said.append)
        self.assertEqual(self.snapshot()[0][0][2], b'')
        s = shader_cache.Store(self.path); s.delete_prefix('empty'); s.put('normal', b'data' * 1000); s.close()
        shader_cache.maintain(self.path, say=self.said.append)
        s = shader_cache.Store(self.path); s.put('empty', b''); s.close()
        shader_cache.maintain(self.path, say=self.said.append)
        s = shader_cache.Store(self.path)
        try: self.assertEqual(s.get('empty'), b''); self.assertEqual(s.get('normal'), b'data' * 1000)
        finally: s.close()
        shader_cache.maintain(self.path, decompress=True, say=self.said.append)
        self.assertEqual(len(self.snapshot()[0]), 2)

    def test_earlier_compressed_cache_handles_empty_pending_blobs(self):
        self.store.put('empty', b''); self.store.close(); enable_compression(self.path)
        shader_cache.maintain(self.path, say=self.said.append)
        self.assertEqual(self.snapshot()[0][0][2], b'')
        s = shader_cache.Store(self.path)
        try:
            sql = "SELECT '[nodict]',count(*),sum(length(data)) FROM _blob_zstd WHERE _data_dict IS NULL GROUP BY 1"
            plan = s.db.execute('EXPLAIN QUERY PLAN ' + sql).fetchall()
            self.assertTrue(any('USING COVERING INDEX blob_pending_size' in row[3] for row in plan), plan)
            ops = s.db.execute('EXPLAIN ' + sql).fetchall()
            root = s.db.execute("SELECT rootpage FROM sqlite_master WHERE name='_blob_zstd'").fetchone()[0]
            table_cursors = {r[2] for r in ops if r[1] == 'OpenRead' and r[3] == root}
            self.assertFalse(any(r[1] == 'Column' and r[2] in table_cursors and r[3] == 2 for r in ops), 'pending grouping must not read payload bytes')
        finally: s.close()

    def test_default_leaves_raw_and_missing_cache_absent(self):
        self.fill(); shader_cache.maintain(self.path, say=self.said.append)
        with contextlib.closing(sqlite3.connect(self.path)) as db: self.assertEqual(db.execute("SELECT type FROM sqlite_master WHERE name='blob'").fetchone(), ('table',))
        with self.assertRaises(FileNotFoundError): shader_cache.maintain(self.root / 'missing', compress=True)
        self.assertFalse((self.root / 'missing').exists())

    def test_busy_in_process_then_close_releases(self):
        self.store.put('keep', b'x')
        with self.assertRaisesRegex(RuntimeError, 'busy'): shader_cache.maintain(self.path, compress=True)
        self.store.close(); self.store.close()
        shader_cache.maintain(self.path, compress=True, say=self.said.append)

    def test_busy_across_processes(self):
        self.store.close()
        code = "import sys; sys.path.insert(0,sys.argv[1]); import shader_cache; s=shader_cache.Store(sys.argv[2]); print('ready',flush=True); sys.stdin.read()"
        p = subprocess.Popen([sys.executable, '-c', code, str(REPO / 'dm/tools'), str(self.path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            assert p.stdout
            self.assertEqual(p.stdout.readline().strip(), 'ready')
            start = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, 'busy'): shader_cache.maintain(self.path, compress=True)
            self.assertLess(time.monotonic() - start, 2)
        finally: p.communicate('')
        shader_cache.maintain(self.path, compress=True, say=self.said.append)

    def test_reversal_failure_and_abandoned_output_preserve_source(self):
        self.fill(); before = self.snapshot(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        with mock.patch.object(shader_cache.os, 'replace', side_effect=OSError('disk full')):
            with self.assertRaisesRegex(OSError, 'disk full'): shader_cache.maintain(self.path, decompress=True)
        self.assertEqual(self.snapshot(), before); self.assertFalse(Path(f'{self.path}.raw.part').exists())
        Path(f'{self.path}.raw.part').write_bytes(b'abandoned')
        shader_cache.maintain(self.path, decompress=True, say=self.said.append)
        self.assertEqual(self.snapshot(), before)

    def test_failed_compression_is_recoverable(self):
        self.fill(); before = self.snapshot(); enable_compression(self.path)
        with mock.patch.object(shader_cache, '_reclaim', side_effect=OSError('disk full')):
            with self.assertRaisesRegex(OSError, 'disk full'): shader_cache.maintain(self.path, compress=True)
        self.assertEqual(self.snapshot(), before)
        shader_cache.maintain(self.path, say=self.said.append)
        self.assertEqual(self.snapshot(), before)

    def test_sqlite_without_extension_loading_reports_actionable_error(self):
        db = mock.Mock()
        db.enable_load_extension.side_effect = NotImplementedError('disabled in this Python')
        with self.assertRaisesRegex(RuntimeError, 'sqlite-zstd.*Python'): shader_cache._load_zstd(db)

    def test_unsupported_locking_does_not_break_raw_but_refuses_conversion(self):
        self.store.close()
        with mock.patch.object(shader_cache.fcntl, 'flock', side_effect=OSError('not supported')):
            s = shader_cache.Store(self.path); s.put('keep', b'x'); s.close()
            with self.assertRaisesRegex(OSError, 'not supported'): shader_cache.maintain(self.path, compress=True)
        self.assertEqual(self.snapshot()[0][0][2], b'x')

    def test_reversal_validates_content_and_cleans_partial_writes(self):
        self.fill(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        before = self.snapshot()
        def fail_write(db, part):
            Path(part).write_bytes(b'half output'); raise OSError('no space')
        with mock.patch.object(shader_cache, '_raw_copy', side_effect=fail_write):
            with self.assertRaisesRegex(OSError, 'no space'): shader_cache.maintain(self.path, decompress=True)
        self.assertFalse(Path(f'{self.path}.raw.part').exists()); self.assertEqual(self.snapshot(), before)
        s = shader_cache.Store(self.path); s.db.execute('UPDATE blob SET hash=? WHERE id=1', (b'wrong hash',)); s.close()
        with self.assertRaisesRegex(RuntimeError, 'hash differs'): shader_cache.maintain(self.path, decompress=True)
        self.assertFalse(Path(f'{self.path}.raw.part').exists())

    def test_killed_reversal_retains_wal_and_retry_cleans_part(self):
        self.fill(); before = self.snapshot(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        code = "import os,sys; sys.path.insert(0,sys.argv[1]); import shader_cache\ndef stop(db,part):\n open(part,'wb').write(b'partial'); os.kill(os.getpid(),9)\nshader_cache._raw_copy=stop\nshader_cache.maintain(sys.argv[2],decompress=True)"
        p = subprocess.run([sys.executable, '-c', code, str(REPO / 'dm/tools'), str(self.path)], capture_output=True)
        self.assertEqual(p.returncode, -9); self.assertTrue(Path(f'{self.path}.raw.part').exists())
        self.assertEqual(self.snapshot(), before)
        shader_cache.maintain(self.path, decompress=True, say=self.said.append)
        self.assertEqual(self.snapshot(), before)

    def test_reversal_includes_committed_wal_from_killed_writer(self):
        self.store.close(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        code = "import os,sys; sys.path.insert(0,sys.argv[1]); import shader_cache; s=shader_cache.Store(sys.argv[2]); s.put('committed',b'from WAL'); os.kill(os.getpid(),9)"
        p = subprocess.run([sys.executable, '-c', code, str(REPO / 'dm/tools'), str(self.path)], capture_output=True)
        self.assertEqual(p.returncode, -9); self.assertTrue(Path(f'{self.path}-wal').is_file())
        shader_cache.maintain(self.path, decompress=True, say=self.said.append)
        self.assertEqual(self.snapshot()[0][0][2], b'from WAL')

    def test_failed_extension_enablement_preserves_raw(self):
        self.fill(); before = self.path.read_bytes()
        with mock.patch.object(shader_cache, '_load_zstd', side_effect=RuntimeError('sqlite-zstd missing')):
            with self.assertRaisesRegex(RuntimeError, 'missing'): shader_cache.maintain(self.path, compress=True)
        self.assertEqual(self.path.read_bytes(), before)

    def test_large_reclamation_checkpoints_between_bounded_batches(self):
        for i in range(40): self.store.put(f'large/{i}', bytes([i]) + b'x' * 1024**2)
        self.store.close(); enable_compression(self.path); reclaim = shader_cache._reclaim; batches = []; wal_sizes = []
        def observe(db):
            db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchall()
            def trace(sql):
                if sql.startswith('PRAGMA incremental_vacuum'): batches.append(sql)
                if sql.startswith('PRAGMA wal_checkpoint'):
                    wal = Path(f'{self.path}-wal'); wal_sizes.append(wal.stat().st_size if wal.exists() else 0)
            db.set_trace_callback(trace)
            try: reclaim(db)
            finally: db.set_trace_callback(None)
        with mock.patch.object(shader_cache, '_reclaim', side_effect=observe): shader_cache.maintain(self.path, compress=True, say=self.said.append)
        self.assertGreater(len(batches), 1)
        self.assertLess(max(wal_sizes), 64 * 1024**2)
        self.assertLess(self.path.stat().st_size, 1024**2)

    def test_maintenance_does_not_wait_for_writer(self):
        self.store.close(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        s = shader_cache.Store(self.path); self.addCleanup(s.close)
        s.put('committed pending', b'needs compression' * 1000)
        s.db.execute('BEGIN IMMEDIATE'); s.put('pending', b'bytes' * 1000)
        start = time.monotonic()
        with self.assertRaises(sqlite3.OperationalError): shader_cache.maintain(self.path, say=self.said.append)
        self.assertLess(time.monotonic() - start, 2)
        s.db.rollback()


if __name__ == '__main__': unittest.main()
