#!/usr/bin/env python3
# The compile cache as one file (dm/tools/shader_cache.py): entries in and out, bytes stored once, several threads and processes, a damaged file, pruning, and taking in a cache from before it.
import contextlib, hashlib, json, os, sqlite3, subprocess, sys, tarfile, tempfile, threading, time, unittest
from pathlib import Path
from unittest import mock

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


@unittest.skipIf(sys.version_info < (3, 11), 'an sqlite3 error carries its code from Python 3.11')
class Renumbered(unittest.TestCase):
    """A new file that takes another inode at its first write (an exFAT drive under macOS 26.6.2, GitHub issue #2434): SQLite's real SQLITE_READONLY_DBMOVED, made here by
    putting a copy of the file in its place. DM_EXFAT_DIR=<a folder on an exFAT volume> also runs test_on_the_drive there"""
    def setUp(self):
        t = tempfile.TemporaryDirectory(dir=os.environ.get('DM_EXFAT_DIR')); self.addCleanup(t.cleanup)
        self.path = Path(t.name) / 'sub' / 'cas.db'
        self.renumber = 0; self.opened = 0; connect = shader_cache.sqlite3.connect; test = self

        class Drive(sqlite3.Connection):
            def execute(self, sql, *args):
                cursor = super().execute(sql, *args)
                if sql.startswith('PRAGMA auto_vacuum') and test.renumber:
                    test.renumber -= 1; copy = test.path.with_name('copy'); copy.write_bytes(test.path.read_bytes()); os.replace(copy, test.path)
                return cursor

        def counted(*a, **k): self.opened += 1; return connect(*a, factory=Drive, **k)
        patcher = mock.patch.object(shader_cache.sqlite3, 'connect', counted); patcher.start(); self.addCleanup(patcher.stop)

    def check(self, opened):
        s = shader_cache.Store(self.path); self.addCleanup(s.close)
        if opened: self.assertEqual(self.opened, opened)
        for i in range(200): s.put(f'x/{i}', os.urandom(5000))
        self.assertEqual(len(s.names('x/')), 200)
        self.assertEqual([s.db.execute(f'PRAGMA {p}').fetchone()[0] for p in ('page_size', 'auto_vacuum', 'journal_mode')], [16384, 2, 'wal'])

    def test_a_renumbered_file_is_opened_again(self): self.renumber = 1; self.check(2)

    def test_a_file_that_moves_again_fails(self):
        self.renumber = 2
        with self.assertRaises(sqlite3.OperationalError) as e: shader_cache.Store(self.path)
        self.assertEqual((e.exception.sqlite_errorcode, self.opened), (shader_cache.DBMOVED, 2))

    def test_on_the_drive(self): self.check(0 if os.environ.get('DM_EXFAT_DIR') else 1)   # opened once where the file keeps the inode SQLite notes, as on APFS; twice on such a drive


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
        assert s.readonly is not None and s.readonly.db is not None   # the store opened over a read-only one, open
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

    def test_fallback_reader_prevents_source_conversion_until_closed(self):
        self.store.put('metallib/mac/a.metallib', b'library'); self.store.close()
        s = self.overlay()
        with self.assertRaisesRegex(RuntimeError, 'busy'): shader_cache.maintain(self.path, compress=True, say=self.said.append)
        s.close()
        shader_cache.maintain(self.path, compress=True, say=self.said.append)
        s = self.overlay()
        self.assertEqual(s.get('metallib/mac/a.metallib'), b'library')


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
        self.store.close()
        with self.assertRaisesRegex(RuntimeError, 'build failed'):
            with shader_cache.bounded(self.path, 1 << 20, self.said.append):
                self.store = shader_cache.Store(self.path, self.said.append)
                try:
                    self.fill()
                    raise RuntimeError('build failed')
                finally: self.store.close()
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
        assert p.stdout is not None
        try:
            self.assertEqual(p.stdout.readline().strip(), 'held')
            before = self.store.names('')
            with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
            self.assertEqual(self.store.names(''), before)
        finally:
            p.communicate(timeout=10)
        self.store.close()
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
        assert p.stdout is not None
        try:
            self.assertEqual(p.stdout.readline().strip(), 'ready')
            before = self.store.names('stage/')
            with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
            self.assertEqual(self.store.names('stage/'), before)
            self.assertGreater(self.path.stat().st_size, 1 << 20)
        finally:
            p.communicate(timeout=10)
        self.assertEqual(p.returncode, 0)
        self.store.close()
        with shader_cache.bounded(self.path, 1 << 20, self.said.append): pass
        self.assertLessEqual(self.path.stat().st_size, 1 << 20)


def enable_compression(path):
    db = sqlite3.connect(path, isolation_level=None)
    try:
        shader_cache._load_zstd(db)
        shader_cache._enable_compression(db)
    finally: db.close()


class CompressedReadOnly(ReadOnly):
    def setUp(self):
        super().setUp(); self.store.close(); enable_compression(self.path)
        self.store = shader_cache.Store(self.path, self.said.append); self.addCleanup(self.store.close)


class CompressedEntries(Entries):
    def setUp(self):
        super().setUp(); self.store.close(); enable_compression(self.path)
        self.store = shader_cache.Store(self.path, self.said.append); self.addCleanup(self.store.close)

    def test_distinct_insert_ids(self):
        for name, data in [('a', b'A'), ('b', b'B'), ('c', b'C'), ('alias', b'B')]: self.store.put(name, data)
        self.assertEqual([self.store.get(n) for n in ('a', 'b', 'c', 'alias')], [b'A', b'B', b'C', b'B'])
        self.assertEqual(self.blobs(), 3)

    def test_missing_codec_preserves_source(self):
        self.store.put('keep', b'precious'); self.store.close()
        before = self.path.read_bytes()
        with mock.patch.object(shader_cache, '_load_zstd', side_effect=RuntimeError('Zstd unavailable')):
            with self.assertRaisesRegex(RuntimeError, 'Zstd'): shader_cache.Store(self.path)
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

    def full_after(self, pages):   # the file may grow by this many pages more: SQLite then answers SQLITE_FULL, as on a full drive
        n = self.store.db.execute('PRAGMA page_count').fetchone()[0]; self.store.db.execute(f'PRAGMA max_page_count={n + pages}')

    def test_a_full_drive_fails_the_whole_batch(self):   # SQLite ends the transaction itself on SQLITE_FULL: an earlier write must not be lost while a later one lands outside the batch
        self.store.put('before', b'0'); self.full_after(2)
        with self.store.batch():
            self.store.put('a', b'small'); self.store.put('b', b'x' * 200000); self.store.put('c', b'small too')
        self.assertTrue(self.store.full); self.assertEqual(self.store.names(''), {'before'})
        self.assertEqual(len([m for m in self.said if m.startswith('[RED]')]), 1)
        self.store.put('d', b'1'); self.assertFalse(self.store.has('d'))   # the build goes on without the cache

    def test_a_failed_commit_fails_the_batch(self):
        class Db:   # the connection, whose commit finds the drive full
            def __init__(self, db): self.db = db
            def __getattr__(self, name): return getattr(self.db, name)
            def commit(self): raise sqlite3.OperationalError('database or disk is full')
        real = self.store.db
        with mock.patch.object(self.store, 'db', Db(real)), self.store.batch(): self.store.put('a', b'1')
        self.assertTrue(self.store.full); self.assertFalse(real.in_transaction); self.assertEqual(self.store.names(''), set())

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
        if isinstance(self, CompressedWriters):
            probe = subprocess.run([APPLE_PYTHON, '-c', "try:\n from compression import zstd\nexcept ImportError:\n from backports import zstd"], capture_output=True, text=True, timeout=60)
            if probe.returncode: self.skipTest(f'{APPLE_PYTHON} has no Zstd codec')
        self.store.close()
        code = "import sys; sys.path.insert(0, sys.argv[1]); import test_shader_cache as t; assert t.forked_stores(sys.argv[2], sys.argv[3]) == t.EXPECT, 'results differ'"
        r = subprocess.run([APPLE_PYTHON, '-c', code, str(Path(__file__).parent), str(self.path), str(self.root)], capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stderr)

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
    def test_opt_in_compression_metrics_include_sizes_workers_and_stages(self):
        self.fill(); before = self.snapshot()
        with mock.patch.dict(os.environ, {'DM_SHADER_CACHE_COMPRESSION_METRICS': '1'}):
            shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        prefix = 'shader cache compression metrics: '
        metrics, = [json.loads(line[len(prefix):]) for line in self.said if line.startswith(prefix)]
        self.assertEqual(metrics['raw_payload_bytes'], sum(len(row[2]) for row in before[0]))
        self.assertEqual(metrics['blob_count'], len(before[0]))
        with contextlib.closing(sqlite3.connect(self.path)) as db:
            encoded, = db.execute('SELECT sum(length(data)) FROM _blob_zstd').fetchone()
        self.assertEqual(metrics['compressed_payload_bytes'], encoded)
        self.assertEqual(metrics['payload_bytes_saved'], metrics['raw_payload_bytes'] - encoded)
        self.assertAlmostEqual(metrics['payload_compression_ratio'], metrics['raw_payload_bytes'] / encoded, places=5)
        self.assertEqual(metrics['workers_requested'], 2)
        self.assertGreater(metrics['batch_count'], 1)
        self.assertGreater(metrics['workers_used'], 0)
        for name in ('worker_result_wait_s', 'blob_inserts_s', 'entry_copy_s', 'commit_s', 'output_setup_s',
                     'batch_boundary_reads_s', 'source_checkpoint_and_install_s', 'worker_cpu_s', 'worker_busy_wall_s'):
            self.assertGreaterEqual(metrics[name], 0)
        self.assertEqual(metrics['database_after_bytes'], self.path.stat().st_size)
        self.assertEqual(self.snapshot(), before)

    def test_compression_metrics_default_off_and_empty_cache(self):
        self.store.close()
        with mock.patch.dict(os.environ, {'DM_SHADER_CACHE_COMPRESSION_METRICS': '0'}):
            shader_cache.maintain(self.path, compress=True, say=self.said.append)
        self.assertFalse(any('compression metrics:' in line for line in self.said))
        shader_cache.maintain(self.path, decompress=True, say=self.said.append)
        with mock.patch.dict(os.environ, {'DM_SHADER_CACHE_COMPRESSION_METRICS': '1'}):
            shader_cache.maintain(self.path, compress=True, say=self.said.append)
        prefix = 'shader cache compression metrics: '
        metrics, = [json.loads(line[len(prefix):]) for line in self.said if line.startswith(prefix)]
        self.assertEqual(metrics['blob_count'], 0)
        self.assertEqual(metrics['batch_count'], 0)
        self.assertIsNone(metrics['payload_compression_ratio'])
        self.assertEqual(metrics['average_active_worker_jobs'], 0)

    def test_compression_writes_ready_batch_before_unfinished_oldest(self):
        first, ready = shader_cache.cf.Future(), shader_cache.cf.Future()
        first.result = mock.Mock(side_effect=AssertionError('waited for oldest batch'))
        rows = [(2, b'hash', b'encoded')]; ready.set_result(rows)
        source, cursor, out = mock.Mock(), mock.MagicMock(), mock.MagicMock()
        source.execute.return_value = cursor
        cursor.fetchone.side_effect = [(1, 8 * 1024**2), (2, 8 * 1024**2), None]
        def observe(sql, batch):
            self.assertEqual(batch, rows)
            raise RuntimeError('observed ready batch')
        out.executemany.side_effect = observe
        with mock.patch.object(shader_cache.sqlite3, 'connect', return_value=out), mock.patch.object(shader_cache, '_load_zstd'), mock.patch.object(shader_cache.cf, 'ThreadPoolExecutor') as executor:
            executor.return_value.__enter__.return_value.submit.side_effect = [first, ready]
            with self.assertRaisesRegex(RuntimeError, 'observed ready batch'):
                shader_cache._compressed_copy(source, str(self.path), str(self.path) + '.compressed.part', 2)
        first.result.assert_not_called()

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

    def test_decompression_metrics_and_mixed_pending_payloads(self):
        self.fill()
        shader_cache.maintain(self.path, compress=True, workers=2, say=self.said.append)
        s = shader_cache.Store(self.path); s.put('pending', b'raw pending' * 10000); s.close()
        before = self.snapshot()
        with mock.patch.dict(os.environ, {'DM_SHADER_CACHE_COMPRESSION_METRICS': '1'}):
            shader_cache.maintain(self.path, decompress=True, workers=4, say=self.said.append)
        prefix = 'shader cache decompression metrics: '
        metrics, = [json.loads(line[len(prefix):]) for line in self.said if line.startswith(prefix)]
        self.assertEqual(metrics['operation'], 'decompression')
        self.assertEqual(metrics['workers_requested'], 4)
        self.assertEqual(metrics['output_payload_bytes'], sum(len(row[2]) for row in before[0]))
        self.assertEqual(metrics['blob_count'], len(before[0]))
        self.assertGreater(metrics['output_payload_bytes'], metrics['input_payload_bytes'])
        self.assertGreaterEqual(metrics['worker_cpu_s'], 0)
        self.assertEqual(self.snapshot(), before)

    def test_reversal_reads_on_workers_without_payload_rescans(self):
        self.fill(); before = self.snapshot()
        shader_cache.maintain(self.path, compress=True, workers=4, say=self.said.append)
        with mock.patch.object(shader_cache, '_parallel_blobs', side_effect=AssertionError('coordinator decoded blobs')):
            shader_cache.maintain(self.path, decompress=True, workers=4, say=self.said.append)
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
        with contextlib.closing(sqlite3.connect(self.path)) as db:
            self.assertEqual(db.execute('SELECT count(*) FROM _blob_zstd WHERE _data_dict IS NULL').fetchone()[0], 0)
            plan = db.execute('EXPLAIN QUERY PLAN SELECT id FROM _blob_zstd WHERE _data_dict IS NULL AND id>0 ORDER BY id LIMIT 256').fetchall()
            self.assertTrue(any('blob_pending' in row[3] for row in plan), plan)

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

    def test_codec_without_sqlite_extension_loading_reads_legacy_frames(self):
        with contextlib.closing(sqlite3.connect(':memory:')) as db:
            shader_cache._load_zstd(db)
            # Produced by the former sqlite-zstd 0.3.5 backend, compact and without a dictionary.
            encoded = bytes.fromhex('0010f50000b06c656761637920736861646572207061796c6f61640a0100fe90fe6c02')
            raw = b'legacy shader payload\n' * 100
            self.assertEqual(db.execute('SELECT zstd_decompress_col(?,0,-1,1)', (encoded,)).fetchone()[0], raw)
            self.assertEqual(db.execute('SELECT zstd_decompress_col(zstd_compress_col(?,3,-1,1),0,-1,1)', (raw,)).fetchone()[0], raw)
            self.assertEqual(db.execute('SELECT zstd_decompress_col(?,0,NULL,1)', (raw,)).fetchone()[0], raw)
            with self.assertRaises(sqlite3.Error): db.execute('SELECT zstd_decompress_col(?,0,7,1)', (encoded,)).fetchone()

    def test_missing_codec_reports_dependency_and_preserves_raw(self):
        import builtins
        original = builtins.__import__
        def without_zstd(name, *args, **kwargs):
            if name in ('compression', 'backports'): raise ImportError('unavailable')
            return original(name, *args, **kwargs)
        self.fill(); before = self.path.read_bytes()
        with mock.patch('builtins.__import__', side_effect=without_zstd):
            with self.assertRaisesRegex(RuntimeError, 'Python 3.14.*pip install backports.zstd'):
                shader_cache.maintain(self.path, compress=True)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertFalse(Path(f'{self.path}.compressed.part').exists())

    def test_unsupported_locking_does_not_break_raw_but_refuses_conversion(self):
        self.store.close()
        with mock.patch.object(shader_cache.fcntl, 'flock', side_effect=OSError('not supported')):
            s = shader_cache.Store(self.path); s.put('keep', b'x'); s.close()
            with self.assertRaisesRegex(OSError, 'not supported'): shader_cache.maintain(self.path, compress=True)
        self.assertEqual(self.snapshot()[0][0][2], b'x')

    def test_reversal_cleans_partial_writes_and_preserves_source(self):
        self.fill(); shader_cache.maintain(self.path, compress=True, say=self.said.append)
        before = self.snapshot()
        def fail_write(db, part):
            Path(part).write_bytes(b'half output'); raise OSError('no space')
        with mock.patch.object(shader_cache, '_raw_copy', side_effect=fail_write):
            with self.assertRaisesRegex(OSError, 'no space'): shader_cache.maintain(self.path, decompress=True)
        self.assertFalse(Path(f'{self.path}.raw.part').exists()); self.assertEqual(self.snapshot(), before)
        with contextlib.closing(sqlite3.connect(self.path)) as db, db:
            db.execute('UPDATE _blob_zstd SET data=? WHERE id=1', (b'not zstd',))
        damaged_source = self.path.read_bytes()
        with self.assertRaises(sqlite3.Error): shader_cache.maintain(self.path, decompress=True, workers=4)
        self.assertEqual(self.path.read_bytes(), damaged_source)
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

    def test_failed_codec_enablement_preserves_raw(self):
        self.fill(); before = self.path.read_bytes()
        with mock.patch.object(shader_cache, '_load_zstd', side_effect=RuntimeError('Zstd missing')):
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
