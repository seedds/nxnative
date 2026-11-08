#!/usr/bin/env python3
# The compile cache as one file (dm/tools/shader_cache.py): entries in and out, bytes stored once, several threads and processes, a damaged file, pruning, and taking in a cache from before it;
# compressed entries beside plain ones (DMSH_CAS_ZSTD), and one in, one out (DMSH_ONE_IN_ONE_OUT).
import json, os, sqlite3, subprocess, sys, tarfile, tempfile, threading, time, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import shader_cache

D1, D2, D3 = 'a' * 64, 'b' * 64, 'c' * 64


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
        self.assertEqual(os.listdir(self.root), ['cas.db'])
        again = shader_cache.Store(self.path); self.addCleanup(again.close)
        self.assertEqual(again.get('x'), b'1')


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
        self.assertEqual(sorted(os.listdir(self.root)), ['cas.db'])

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


@unittest.skipIf(shader_cache._zstd is None, 'no zstd: Python 3.14, or pip install backports.zstd')
class Compressed(unittest.TestCase):
    """DMSH_CAS_ZSTD: entries as zstd frames in zentry / zblob, read beside the plain tables; a store that cannot read them misses them"""
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name); self.path = self.root / 'cas.db'; self.said = []

    def store(self, **kw): return shader_cache.Store(self.path, self.said.append, **kw)

    def count(self, table):
        db = sqlite3.connect(self.path); n = db.execute(f'SELECT count(*) FROM {table}').fetchone()[0]; db.close(); return n

    def test_in_and_out_smaller(self):
        msl = b'float4 x = float4(0.0);\n' * 4000
        s = self.store(compress=True); s.put('shader/k_b.metal', msl); s.put('empty', b'')
        self.assertEqual((s.get('shader/k_b.metal'), s.get('empty')), (msl, b'')); self.assertTrue(s.has('empty')); s.close()
        s = self.store(compress=False)   # a plain store reads them too
        self.assertEqual((s.get('shader/k_b.metal'), s.names('')), (msl, {'shader/k_b.metal', 'empty'})); s.close()
        db = sqlite3.connect(self.path)
        self.assertLess(db.execute('SELECT max(length(data)) FROM zblob').fetchone()[0], len(msl) // 20); db.close()
        self.assertEqual(self.count('entry'), 0)

    def test_the_switches(self):
        for env, z in (({}, False), ({'DMSH_CAS_ZSTD': '1'}, True), ({'DMSH_ONE_IN_ONE_OUT': '1'}, True), ({'DMSH_ONE_IN_ONE_OUT': '1', 'DMSH_CAS_ZSTD': '0'}, False)):
            with self.subTest(env=env), mock.patch.dict(os.environ, env, clear=False):
                for k in ('DMSH_CAS_ZSTD', 'DMSH_ONE_IN_ONE_OUT'):
                    if k not in env: os.environ.pop(k, None)
                s = self.store(); self.assertEqual(s.z, z); self.assertEqual(s.evict, env.get('DMSH_ONE_IN_ONE_OUT') == '1'); s.close()

    def test_mixed_old_and_new_entries(self):   # a cache from before this, then a compressing build
        s = self.store(compress=False); s.put('a', b'old plain'); s.put('b', b'kept plain'); s.close()
        s = self.store(compress=True)
        self.assertEqual(s.get('b'), b'kept plain'); s.put('a', b'new'); self.assertEqual(s.names(''), {'a', 'b'}); s.close()
        s = self.store(compress=False); self.assertEqual((s.get('a'), s.get('b')), (b'new', b'kept plain')); s.close()
        db = sqlite3.connect(self.path); self.assertEqual([r[0] for r in db.execute('SELECT name FROM entry')], ['b']); db.close()

    def test_a_plain_write_replaces_a_compressed_entry(self):   # a store with compression off, or one without zstd
        s = self.store(compress=True); s.put('index/k.json', b'old'); s.close()
        s = self.store(compress=False); s.put('index/k.json', b'new'); self.assertEqual(s.get('index/k.json'), b'new'); s.close()
        self.assertEqual(self.count('zentry'), 0)
        s = self.store(compress=True); s.put('index/k.json', b'z'); s.close()
        with mock.patch.object(shader_cache, '_zstd', None):
            s = self.store(compress=True); self.assertFalse(s.z); s.put('index/k.json', b'plain'); s.close()
        s = self.store(); self.assertEqual(s.get('index/k.json'), b'plain'); s.close()

    def test_the_newer_of_a_name_in_both_pairs_is_read(self):   # a store from before this writes plain entries and does not know zentry
        s = self.store(compress=True); s.put('metallib/filled.json', b'older', 1000.0); s.close()
        db = sqlite3.connect(self.path)
        db.execute("INSERT INTO blob(hash, data) VALUES(x'00', x'6e65776572')"); db.execute("INSERT INTO entry VALUES('metallib/filled.json', last_insert_rowid(), 2000.0)"); db.commit(); db.close()
        s = self.store(compress=True); self.assertEqual(s.get('metallib/filled.json'), b'newer'); s.close()

    def test_a_store_without_zstd_misses_them(self):
        s = self.store(compress=True); s.put('metallib/mac/x.metallib', b'LIB'); s.close()
        with mock.patch.object(shader_cache, '_zstd', None):
            s = self.store(compress=True)
            self.assertFalse(s.z); self.assertIsNone(s.get('metallib/mac/x.metallib')); self.assertFalse(s.has('metallib/mac/x.metallib'))
            s.put('metallib/mac/y.metallib', b'PLAIN'); s.delete_prefix('metallib/mac/x'); s.close()
        self.assertTrue(any('zstd' in m for m in self.said), self.said)
        self.assertEqual(self.count('zentry'), 0)   # removals cover the pair it cannot read
        s = self.store(); self.assertEqual(s.get('metallib/mac/y.metallib'), b'PLAIN'); s.close()

    def test_a_damaged_frame_is_a_miss(self):
        s = self.store(compress=True); s.put('x', b'bytes' * 100); s.db.execute("UPDATE zblob SET data=x'00010203'")
        self.assertIsNone(s.get('x')); self.assertFalse(s.to_file('x', str(self.root / 'out'))); self.assertFalse((self.root / 'out').exists()); s.close()

    def test_removals_cover_both_pairs(self):
        t = time.time() - 100
        s = self.store(compress=False); s.put(f'shader/{D1}/mac/k_b.metal', b'p' * 999, t); s.put('stage/x/1', b'p1'); s.close()
        s = self.store(compress=True); s.put(f'shader/{D2}/mac/k_b.metal', b'z' * 999, t); s.put('stage/x/2', b'z2')
        self.assertEqual(s.prune_shaders(set(), time.time())[0], 2); s.delete_prefix('stage/'); self.assertEqual(s.names(''), set()); s.close()
        self.assertEqual([self.count(t) for t in ('entry', 'blob', 'zentry', 'zblob')], [0, 0, 0, 0])


class PruneRace(Base):
    def test_an_entry_written_again_after_the_scan_stays(self):   # another build sharing the file filed it anew between the scan and the transaction
        now = time.time(); name = f'shader/{D1}/mac/k_b.metal'
        self.store.put(name, b'old', now - 100)
        real = self.store._run
        def scan_then_rewrite(sql, *args):
            rows = real(sql, *args)
            if 'length(b.data)' in sql:
                other = shader_cache.Store(self.path, compress=False); other.put(name, b'filed again', now - 50); other.close()
            return rows
        with mock.patch.object(self.store, '_run', side_effect=scan_then_rewrite): self.store.prune_shaders(set(), now)
        self.assertEqual(self.store.get(name), b'filed again')
        self.assertEqual(self.store.db.execute('SELECT count(*) FROM blob WHERE id NOT IN (SELECT blob FROM entry)').fetchone()[0], 0)


class OneInOneOut(unittest.TestCase):
    """DMSH_ONE_IN_ONE_OUT: a key's new entry (index/<key>.json under another digest) replaces its older one in the same transaction"""
    OLD, NEW, OTHER = D1, D2, D3
    Z = False

    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup); self.path = Path(t.name) / 'cas.db'; self.said = []
        old = shader_cache.Store(self.path, compress=False, evict=False); w = 1000.0
        old.put(f'shader/{self.OLD}/mac/k1_b.metal', b'old msl k1', w); old.put(f'shader/{self.OLD}/mac/k1_b.metallib', b'old lib k1' * 40000, w)
        old.put(f'shader/{self.OLD}/mac/k1_b.json', b'{"old": 1}', w); old.put('metallib/mac/m1.metallib', b'old lib k1' * 40000, w)
        old.put(f'shader/{self.OLD}/mac/k9_b.metal', b'same digest, another key', w)
        old.put('index/k1.json', json.dumps({'digest': self.OLD}).encode(), w)
        old.put(f'shader/{self.OTHER}/mac/k3_b.metallib', b'unchanged lib k3', w); old.put('metallib/mac/m3.metallib', b'unchanged lib k3', w)
        old.put('index/k3.json', json.dumps({'digest': self.OTHER}).encode(), w)
        old.put('metallib/mac/side.metallib', b'a side library', w)
        old.close()

    def store(self, **kw): return shader_cache.Store(self.path, self.said.append, **{'compress': self.Z, 'evict': True, **kw})

    def publish(self, store, key, digest, files):   # as dmsh_build.py's publish_cache
        with store.batch():
            for n, data in files.items(): store.put(f'shader/{digest}/mac/{key}_{n}', data)
            store.put(f'index/{key}.json', json.dumps({'digest': digest}).encode())

    def names(self):
        s = shader_cache.Store(self.path, compress=False, evict=False); n = s.names(''); s.close(); return n

    def unnamed(self):
        db = sqlite3.connect(self.path); pairs = [p for p in (shader_cache.PLAIN, shader_cache.ZSTD) if db.execute('SELECT 1 FROM sqlite_master WHERE name=?', (p[0],)).fetchone()]
        n = sum(db.execute(f'SELECT count(*) FROM {b} WHERE id NOT IN (SELECT blob FROM {e})').fetchone()[0] for e, b in pairs); db.close(); return n

    def test_the_older_entry_and_its_unshared_library_go(self):
        s = self.store(); self.publish(s, 'k1', self.NEW, {'b.metal': b'new msl k1', 'b.metallib': b'new lib k1'}); s.close()
        names = self.names()
        self.assertFalse([n for n in names if n.startswith(f'shader/{self.OLD}/mac/k1_')])
        self.assertNotIn('metallib/mac/m1.metallib', names)
        self.assertIn(f'shader/{self.OLD}/mac/k9_b.metal', names)   # another key with the same digest keeps its files
        self.assertTrue({'metallib/mac/m3.metallib', 'metallib/mac/side.metallib', f'shader/{self.OTHER}/mac/k3_b.metallib', f'shader/{self.NEW}/mac/k1_b.metal'} <= names)
        self.assertEqual((s.evicted['shaders'], s.evicted['entries'], s.evicted['libraries']), (1, 3, 1))
        self.assertGreater(s.evicted['bytes'], 0); self.assertEqual(self.unnamed(), 0)
        self.assertTrue(any(m.startswith('one in, one out: 1 older') for m in self.said), self.said)

    def test_a_library_the_new_entry_shares_stays(self):   # unchanged MSL: the new entry names the same bytes
        s = self.store(); self.publish(s, 'k3', self.NEW, {'b.metallib': b'unchanged lib k3'}); s.close()
        names = self.names()
        self.assertNotIn(f'shader/{self.OTHER}/mac/k3_b.metallib', names); self.assertIn('metallib/mac/m3.metallib', names)
        s = self.store(evict=False); self.assertEqual((s.get(f'shader/{self.NEW}/mac/k3_b.metallib'), s.get('metallib/mac/m3.metallib')), (b'unchanged lib k3',) * 2); s.close()

    def test_off_by_default_and_for_the_same_digest(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            for k in ('DMSH_ONE_IN_ONE_OUT', 'DMSH_CAS_ZSTD'): os.environ.pop(k, None)
            s = shader_cache.Store(self.path, self.said.append)
        self.assertFalse(s.evict); self.publish(s, 'k1', self.NEW, {'b.metal': b'new msl k1'}); s.close()
        self.assertIn(f'shader/{self.OLD}/mac/k1_b.metal', self.names())
        s = self.store(); self.publish(s, 'k3', self.OTHER, {'b.metallib': b'unchanged lib k3'}); s.close()
        self.assertIn(f'shader/{self.OTHER}/mac/k3_b.metallib', self.names()); self.assertEqual(s.evicted['shaders'], 0)

    def test_entries_written_since_the_store_opened_stay(self):   # the same build filed the key twice, or another build shares the file
        s = self.store(); self.publish(s, 'k5', self.OLD, {'b.metal': b'first'}); self.publish(s, 'k5', self.NEW, {'b.metal': b'second'}); s.close()
        self.assertTrue({f'shader/{self.OLD}/mac/k5_b.metal', f'shader/{self.NEW}/mac/k5_b.metal'} <= self.names())

    def test_a_rolled_back_batch_evicts_nothing(self):
        s = self.store()
        with self.assertRaises(KeyboardInterrupt), s.batch():
            s.put('index/k1.json', json.dumps({'digest': self.NEW}).encode()); raise KeyboardInterrupt
        s.close()
        self.assertIn(f'shader/{self.OLD}/mac/k1_b.metal', self.names()); self.assertEqual(s.evicted['shaders'], 0)

    def test_an_eviction_error_skips_that_eviction_and_the_write_goes_on(self):   # a damaged frame, an odd name: the savepoint undoes the part done
        s = self.store(); real = s._drop_unnamed
        def fail_late(pair, blob):
            if pair[1] in ('blob', 'zblob') and s._evicting['entries']: raise ValueError('boom')
            return real(pair, blob)
        with mock.patch.object(s, '_drop_unnamed', side_effect=fail_late):
            self.publish(s, 'k1', self.NEW, {'b.metal': b'new msl k1'})
        s.put('after', b'still written'); s.close()
        names = self.names()
        self.assertIn(f'shader/{self.OLD}/mac/k1_b.metal', names); self.assertIn('metallib/mac/m1.metallib', names)   # all of the eviction undone
        self.assertIn(f'shader/{self.NEW}/mac/k1_b.metal', names); self.assertIn('after', names)
        self.assertTrue(any('stays (ValueError: boom)' in m for m in self.said), self.said); self.assertEqual(s.evicted['errors'], 1)
        self.assertEqual((s.evicted['shaders'], s.evicted['entries'], s.evicted['libraries']), (0, 0, 0))   # what the savepoint undid is not counted
        self.assertTrue(any(m.startswith('one in, one out: 0 older') and '1 left after an error' in m for m in self.said), self.said)

    def test_the_newer_older_index_decides(self):   # a store from before this filed the key plain after a compressing build filed it
        if shader_cache._zstd is None: self.skipTest('no zstd')
        db = sqlite3.connect(self.path); db.executescript(';'.join(shader_cache.ZSCHEMA))   # the compressing build's index, older (500) than the plain one (1000), which names OLD
        db.execute('INSERT INTO zblob(hash, data) VALUES(?, ?)', (b'z', shader_cache.Store._compress(json.dumps({'digest': self.OTHER}).encode())))
        db.execute("INSERT INTO zentry VALUES('index/k1.json', last_insert_rowid(), 500.0)"); db.commit(); db.close()
        s = self.store(); self.publish(s, 'k1', self.NEW, {'b.metal': b'new'}); s.close()   # the plain index (1000) names OLD
        self.assertFalse([n for n in self.names() if n.startswith(f'shader/{self.OLD}/mac/k1_')]); self.assertEqual(s.evicted['shaders'], 1)

    def test_bytes_stored_already_are_not_compressed_again(self):
        if not self.Z: self.skipTest('compressed writes')
        s = self.store()
        with mock.patch.object(shader_cache.Store, '_compress', wraps=shader_cache.Store._compress) as z:
            s.put('a', b'same bytes' * 100); s.put('b', b'same bytes' * 100); self.assertEqual(z.call_count, 1)
        self.assertEqual((s.get('a'), s.get('b')), (b'same bytes' * 100,) * 2); s.close()

    def test_the_summary_comes_only_from_a_store_that_filed_shaders(self):
        s = self.store(); s.put('metallib/mac/x.metallib', b'x'); s.close()
        self.assertFalse([m for m in self.said if m.startswith('one in, one out')])

    def test_pages_go_back_to_the_drive(self):
        with mock.patch.object(shader_cache, 'EVICT_VACUUM', 1):
            s = self.store(); s.db.execute('PRAGMA wal_checkpoint(TRUNCATE)'); before = os.path.getsize(self.path)
            self.publish(s, 'k1', self.NEW, {'b.metal': b'n'}); s.close()
        db = sqlite3.connect(self.path); self.assertEqual(db.execute('PRAGMA freelist_count').fetchone()[0], 0); db.close()
        self.assertLess(os.path.getsize(self.path), before)


@unittest.skipIf(shader_cache._zstd is None, 'no zstd: Python 3.14, or pip install backports.zstd')
class OneInOneOutCompressed(OneInOneOut):
    """The same with the build writing compressed entries over a plain cache from before: a plain library the new entry shares moves to the compressed pair"""
    Z = True

    def test_a_library_the_new_entry_shares_stays(self):
        super().test_a_library_the_new_entry_shares_stays()
        db = sqlite3.connect(self.path)
        self.assertEqual(db.execute("SELECT count(*) FROM entry WHERE name='metallib/mac/m3.metallib'").fetchone()[0], 0)
        self.assertEqual(db.execute("SELECT count(*) FROM zentry WHERE name='metallib/mac/m3.metallib'").fetchone()[0], 1)
        self.assertEqual(db.execute("SELECT count(*) FROM blob WHERE data=?", (b'unchanged lib k3',)).fetchone()[0], 0); db.close()

    def test_a_damaged_older_index_skips_the_eviction(self):
        s = self.store(compress=True, evict=False); s.put('index/k7.json', json.dumps({'digest': self.OLD}).encode(), 1000.0)
        s.db.execute("UPDATE zblob SET data=x'00010203' WHERE id=(SELECT blob FROM zentry WHERE name='index/k7.json')"); s.close()
        s = self.store(); self.publish(s, 'k7', self.NEW, {'b.metal': b'x'}); s.close()
        self.assertEqual(s.evicted['shaders'], 0); self.assertIn(f'shader/{self.NEW}/mac/k7_b.metal', self.names())


if __name__ == '__main__': unittest.main()
