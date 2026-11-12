#!/usr/bin/env python3
# The compile cache as one file (dm/tools/shader_cache.py): entries in and out, bytes stored once, several threads and processes, a damaged file, pruning, and taking in a cache from before it.
import fcntl, hashlib, os, subprocess, sys, tarfile, tempfile, threading, time, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import shader_cache

D1, D2, D3 = 'a' * 64, 'b' * 64, 'c' * 64
APPLE_PYTHON = '/usr/bin/python3'   # Xcode's, whose sqlite3 is Apple's /usr/lib/libsqlite3.dylib: a fork that opens a database there dies (GitHub issue #2354)
SERVED = STORE = None   # what the forked workers inherit, and each worker's own store


def worker_start():
    global STORE
    assert SERVED is not None; STORE = SERVED.worker()


def forked_work(i):
    s = STORE; assert s is not None
    for j in range(300): s.put(f'f{i}/{j}', f'{i}'.encode() * 1000); assert s.get(f'f{i}/{j}') == f'{i}'.encode() * 1000
    with s.batch():
        with s.batch(): s.put(f'f{i}/batch', b'b')
        assert s.has(f'f{i}/batch')
    try:
        with s.batch(): s.put(f'f{i}/undone', b'u'); raise KeyError(i)
    except KeyError: pass
    try: s._call('close')
    except ValueError: pass
    else: raise AssertionError('a worker closed the store')
    tmp = f'{WORK}/{i}'
    assert s.to_file(f'f{i}/0', tmp) and not s.to_file('none', tmp); s.put_file(f'f{i}/file', tmp)
    return s.get('before'), sorted(s.names(f'f{i}/'))[-2:], s.get(f'f{i}/file'), s.full


def dies_in_batch(i):
    s = STORE; assert s is not None
    with s.batch(): s.put('dead', b'd'); os._exit(9)


def forked_stores(path, work):
    """Four forked workers of a process that has opened the store, as dmsh_build.py makes them → their results, and what the store holds then"""
    import multiprocessing
    global SERVED, WORK
    WORK = work; store = shader_cache.Store(path); store.put('before', b'0'); SERVED = shader_cache.Served(store, 4)
    with multiprocessing.get_context('fork').Pool(4, initializer=worker_start) as pool:   # every worker forked here, on any Python (an executor of Python 3.9 forks them as work comes)
        SERVED.start()
        got = pool.map(forked_work, range(4), 1)
    store.put('after', b'1')   # this process's connection still works
    return got, len(store.names('f')), store.has('f0/undone'), store.get('after')


WORK = ''
EXPECT = ([(b'0', [f'f{i}/batch', f'f{i}/file'], f'{i}'.encode() * 1000, False) for i in range(4)], 1208, False, b'1')


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


class ReadOnly(Base):
    def overlay(self, source=None):
        s = shader_cache.Store(self.root / 'regress/cas.db', self.said.append, readonly=source or self.path)
        self.addCleanup(s.close)
        return s

    def test_fallback_local_precedence_and_source_unchanged(self):
        self.store.put(f'shader/{D1}/mac/k_b.metal', b'source')
        self.store.put('metallib/mac/a.metallib', b'library')
        self.store.put('variant/v/mac.metallib', b'variant')
        self.store.put('stage/build/mac/k', b'private'); self.store.put('metallib/filled.json', b'private'); self.store.put('index/k.json', b'private')
        self.store.close(); before = self.path.read_bytes(); stamp = self.path.stat().st_mtime_ns
        s = self.overlay()
        self.assertEqual(s.get(f'shader/{D1}/mac/k_b.metal'), b'source')
        self.assertTrue(s.has('metallib/mac/a.metallib'))
        self.assertEqual(s.names('variant/'), {'variant/v/mac.metallib'})
        for name in ('stage/build/mac/k', 'metallib/filled.json', 'index/k.json'):
            self.assertIsNone(s.get(name)); self.assertFalse(s.has(name)); self.assertNotIn(name, s.names(''))
        s.put('metallib/mac/a.metallib', b'local'); s.put('metallib/mac/new.metallib', b'new')
        self.assertEqual(s.get('metallib/mac/a.metallib'), b'local')
        self.assertEqual(s.names('metallib/mac/'), {'metallib/mac/a.metallib', 'metallib/mac/new.metallib'})
        s.delete_prefix('variant/'); self.assertEqual(s.get('variant/v/mac.metallib'), b'variant')
        with self.assertRaises(shader_cache.sqlite3.OperationalError): s.readonly.db.execute('DELETE FROM entry')
        s.close()
        self.assertEqual(self.path.read_bytes(), before); self.assertEqual(self.path.stat().st_mtime_ns, stamp)

    def test_committed_wal_is_visible_while_normal_build_is_open(self):
        s = self.overlay()
        self.store.put('metallib/mac/new.metallib', b'committed in WAL')
        self.assertEqual(s.get('metallib/mac/new.metallib'), b'committed in WAL')
        self.assertTrue(s.has('metallib/mac/new.metallib'))

    def test_missing_or_damaged_source_is_never_created_or_repaired(self):
        missing = self.root / 'absent/cas.db'; s = self.overlay(missing)
        self.assertIsNone(s.get('metallib/mac/x.metallib')); s.close()
        self.assertFalse(missing.parent.exists())
        damaged = self.root / 'damaged.db'; damaged.write_bytes(b'not a database' * 3000)
        before = damaged.read_bytes(); s = self.overlay(damaged)
        self.assertIsNone(s.get('metallib/mac/x.metallib')); s.put('new', b'works')
        self.assertEqual(s.get('new'), b'works'); self.assertEqual(damaged.read_bytes(), before)
        self.assertEqual(len(self.said), 1)

    def test_historical_builder_uses_harness_reader_including_forked_workers(self):
        self.store.put('metallib/mac/a.metallib', b'library'); self.store.close()
        before = hashlib.sha256(self.path.read_bytes()).digest()
        script = self.root / 'old/build.py'; script.parent.mkdir()
        (script.parent / 'shader_cache.py').write_text('raise AssertionError("historical cache reader was used")\n')
        script.write_text('''import atexit, multiprocessing, os, shader_cache, sys
s = shader_cache.Store(sys.argv[1]); atexit.register(s.close)
assert s.get('metallib/mac/a.metallib') == b'library'
served = shader_cache.Served(s, 1)
def worker():
    remote = served.worker()
    assert remote.get('metallib/mac/a.metallib') == b'library'
    remote.put('metallib/mac/new.metallib', b'new')
p = multiprocessing.get_context('fork').Process(target=worker)
p.start(); served.start(); p.join(20)
assert p.exitcode == 0, p.exitcode
''')
        dest = self.root / 'regress.db'
        r = subprocess.run([sys.executable, shader_cache.__file__, '--run', '--readonly', str(self.path), str(script), str(dest)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        s = shader_cache.Store(dest); self.addCleanup(s.close)
        self.assertEqual(s.get('metallib/mac/new.metallib'), b'new')
        self.assertIsNone(s.get('metallib/mac/a.metallib'))   # reading the fallback does not copy it
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).digest(), before)


class Bounded(Base):
    def fill(self):
        for i in range(12):
            d = f'{i:064x}'; data = os.urandom(220000)
            self.store.put(f'shader/{d}/mac/k_b.metal', data, i)
            self.store.put(f'shader/{d}/mac/k_b.json', b'{}', i)
            self.store.put(f'metallib/mac/{d}.metallib', data, i)
            self.store.put(f'metallib/mac/{d}_x.metallib', b'split', i)
            self.store.put(f'variant/{d}/mac.metallib', data, i)
            self.store.put(f'variant/{d}/metal', b'variant source', i)
        self.store.put('metallib/filled.json', b'{}', 99)
        self.store.put('stage/stopped/mac/k', os.urandom(300000), 99)

    def test_physical_limit_dedup_and_whole_groups(self):
        self.fill()
        self.store.trim(1 << 20)
        self.assertLessEqual(self.path.stat().st_size, 1 << 20)
        self.assertFalse(self.store.has('metallib/filled.json')); self.assertFalse(self.store.names('stage/'))
        names = self.store.names('')
        self.assertIn(f'metallib/mac/{11:064x}.metallib', names)
        for i in range(12):
            d = f'{i:064x}'
            self.assertEqual(f'shader/{d}/mac/k_b.metal' in names, f'shader/{d}/mac/k_b.json' in names)
            self.assertEqual(f'metallib/mac/{d}.metallib' in names, f'metallib/mac/{d}_x.metallib' in names)
            self.assertEqual(f'variant/{d}/mac.metallib' in names, f'variant/{d}/metal' in names)
        self.assertEqual(self.store.db.execute('SELECT count(*) FROM blob WHERE id NOT IN (SELECT blob FROM entry)').fetchone()[0], 0)

    def test_failed_build_is_trimmed_when_last_user_leaves(self):
        with self.assertRaises(RuntimeError):
            with shader_cache.bounded(self.path, 1 << 20, self.said.append):
                self.fill()
                raise RuntimeError('build failed')
        self.assertLessEqual(self.path.stat().st_size, 1 << 20)

    def test_replaced_unreferenced_blobs_are_reclaimed_before_evicting_live_entries(self):
        for i in range(12): self.store.put('index/k.json', os.urandom(200000), i)
        self.store.put('index/k.json', b'current', 99)
        self.assertEqual(self.store.trim(1 << 20), 0)
        self.assertLessEqual(self.path.stat().st_size, 1 << 20)
        self.assertEqual(self.store.get('index/k.json'), b'current')

    def test_another_process_using_cache_prevents_eviction(self):
        self.fill(); self.store.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        code = "import fcntl,sys; f=open(sys.argv[1], 'a'); fcntl.flock(f,fcntl.LOCK_SH); print('held',flush=True); sys.stdin.read()"
        p = subprocess.Popen([sys.executable, '-c', code, str(self.path) + '.lock'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(p.stdout.readline().strip(), 'held')
            before = self.store.names('')
            with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
            self.assertEqual(self.store.names(''), before)
        finally:
            p.communicate(timeout=10)
        with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
        self.assertLessEqual(self.path.stat().st_size, 1 << 20)

    def test_writer_is_protected_without_a_live_harness(self):
        # No parent bounded() lease: the builder itself must protect its staged outputs until it exits.
        source = self.root / 'normal.db'; source.write_bytes(b'')
        code = ("import atexit,os,sys; sys.path.insert(0,sys.argv[1]); import shader_cache\n"
                "s=shader_cache.Store(sys.argv[2], readonly=sys.argv[3]); atexit.register(s.close)\n"
                "for i in range(12): s.put(f'stage/build/{i}',os.urandom(200000))\n"
                "s.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')\n"
                "print('ready',flush=True); sys.stdin.read()\n")
        p = subprocess.Popen([sys.executable, '-c', code, str(REPO / 'dm/tools'), str(self.path), str(source)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(p.stdout.readline().strip(), 'ready')
            before = self.store.names('stage/')
            with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
            self.assertEqual(self.store.names('stage/'), before)
            self.assertGreater(self.path.stat().st_size, 1 << 20)
        finally:
            p.communicate(timeout=10)
        self.assertEqual(p.returncode, 0)
        with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
        self.assertLessEqual(self.path.stat().st_size, 1 << 20)


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

    def test_forked_workers_share_it(self):   # dmsh_build.py's worker processes: forks of a process that has the file open, their calls made by that process
        self.store.close()
        self.assertEqual(forked_stores(self.path, str(self.root)), EXPECT)

    def test_forked_workers_with_apples_sqlite(self):   # the same with a Python whose SQLite is Apple's, where a fork that opens the file is killed
        probe = "import _sqlite3, subprocess; print(subprocess.run(['otool', '-L', _sqlite3.__file__], capture_output=True, text=True).stdout)"
        try: libs = subprocess.run([APPLE_PYTHON, '-c', probe], capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.TimeoutExpired): libs = ''
        if '/usr/lib/libsqlite3.dylib' not in libs: self.skipTest(f"no {APPLE_PYTHON} with Apple's SQLite")
        self.store.close()
        code = "import sys; sys.path.insert(0, sys.argv[1]); import test_shader_cache as t; assert t.forked_stores(sys.argv[2], sys.argv[3]) == t.EXPECT, 'results differ'"
        r = subprocess.run([APPLE_PYTHON, '-c', code, str(Path(__file__).parent), str(self.path), str(self.root)], capture_output=True, text=True, timeout=300)
        self.assertEqual((r.returncode, r.stderr), (0, ''))

    def test_worker_dying_in_a_batch(self):   # its batch is rolled back and the store goes on
        import concurrent.futures as cf, multiprocessing
        global SERVED; SERVED = shader_cache.Served(self.store, 2)
        with cf.ProcessPoolExecutor(2, mp_context=multiprocessing.get_context('fork'), initializer=worker_start) as ex:
            ex.submit(int).result(); SERVED.start()
            with self.assertRaises(cf.process.BrokenProcessPool): ex.submit(dies_in_batch, 0).result()
        t = threading.Thread(target=lambda: self.store.put('alive', b'a')); t.start(); t.join(20)
        self.assertFalse(t.is_alive()); self.assertFalse(self.store.has('dead')); self.assertTrue(self.store.has('alive'))


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
