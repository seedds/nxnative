#!/usr/bin/env python3
# The compile cache as one file (dm/tools/shader_cache.py): entries in and out, bytes stored once, several threads and processes, a damaged file, pruning, and taking in a cache from before it.
import os, subprocess, sys, tarfile, tempfile, threading, time, unittest
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


if __name__ == '__main__': unittest.main()
