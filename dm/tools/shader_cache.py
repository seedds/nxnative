#!/usr/bin/env python3
# The shader build's compile cache, one file per translation chain version (a generation): <cache folder>/cas-<chain>.db, read and written in place. It was a folder of a few files per shader (Metroid Dread: 84,541 files), kept as a tar between builds and unpacked
# for each build that ran. Nothing is unpacked now: an entry goes in and comes out by its name, and a file a tool needs is written only then.
# SQLite, as it is in Python and takes several writers: dmsh_build.py's threads share one connection (its worker processes use that one too, through this process: Served). Regression builds share their own per-game file, with read-only reuse of the player's cache.
# Generations: a chain's build writes only its own file. Its per-shader entries never hit another chain's (their digests cover the chain), and the Metal compiles it reuses from an older generation
# (keyed by the MSL, the convention and the compiler alone) are copied into its file when it takes them (for_chain). After a successful build the NSP script removes the other generations whole
# (drop_generations): an old chain, a stopped build of one, or a development build of another chain is one file, removed or deleted by hand without a vacuum. cas.db, the one file from before
# generations, holding every chain's entries, is the oldest generation and goes the same way.
# Two tables: entry (name, the blob it names, when it was written) and blob (the bytes, once each). A library kept under its MSL's digest and under its shader's digest was one APFS clone of the other; inside
# one file it would be two copies, so bytes are stored by their hash. A file removed from the cache gives its pages back at the next prune (incremental vacuum), with no copy of the file.
# It only saves work: a damaged file costs the work it held and is made anew, and a full drive is one [RED] line. An entry is written and read whole, so its file is never seen half written.
# Names, as in the folder this replaces: metallib/<target>/<digest>[_x].metallib (dmsh_mtlcache.py), shader/<digest>/<mac|ios>/<output>, index/<key>.json, variant/<digest>/<file> (dmsh_build.py).
# Not part of the translation chain: it decides no output (dmsh_chain.py), like dmsh_mtlcache.py.
import contextlib, fcntl, hashlib, json, multiprocessing, os, re, shutil, sqlite3, tarfile, threading, time
from pathlib import Path

SCHEMA = ('CREATE TABLE IF NOT EXISTS blob(id INTEGER PRIMARY KEY, hash BLOB UNIQUE NOT NULL, data BLOB NOT NULL)',
          'CREATE TABLE IF NOT EXISTS entry(name TEXT PRIMARY KEY, blob INTEGER NOT NULL, mtime REAL NOT NULL) WITHOUT ROWID')
SHADER_ENTRY = re.compile(r'shader/([0-9a-f]{64})/')
WAIT = 600   # seconds another process's write may hold this one up
_DAMAGED = ('malformed', 'not a database', 'disk image')
DBMOVED = 1032   # SQLITE_READONLY_DBMOVED, an error's sqlite_errorcode (Python 3.11+)
READ_ONLY = None   # set only by --run: the harness supplies this module to older SQLite-based builders too
GENERATION = re.compile(r'cas-([0-9a-f]{12})\.db')   # a chain's file in a cache folder
LEGACY = 'cas.db'   # the one file of every chain, from before generations
TARGETS = ('mac', 'ios')   # metallib/<target>/


def generations(folder):
    """The compile cache files of a cache folder, newest first (by when each was last written): cas-<chain>.db, then cas.db"""
    folder = os.fspath(folder)
    try: names = os.listdir(folder)
    except OSError: return []
    def when(p):
        try: return max(os.path.getmtime(p + s) for s in ('', '-wal') if os.path.exists(p + s))
        except (OSError, ValueError): return 0
    gens = sorted((os.path.join(folder, n) for n in names if GENERATION.fullmatch(n)), key=when, reverse=True)
    return gens + ([os.path.join(folder, LEGACY)] if LEGACY in names else [])


class _ReadOnly:
    """Live SQLite readers of other compile cache files, asked in order: mode=ro sees a writer's committed WAL, never migrates, repairs or writes them. Mutable index/stage bookkeeping is local to each build.
    prefixes: the names they serve (an older generation serves only its Metal compiles: a per-shader entry of another chain never hits)"""
    def __init__(self, paths, say, prefixes=('shader/', 'variant/', 'metallib/')):
        self.say, self.lock, self.prefixes, self.dbs = say, threading.RLock(), prefixes, []
        for p in map(os.fspath, paths):
            if not os.path.isfile(p): continue
            try: self.dbs.append((p, sqlite3.connect(Path(p).resolve().as_uri() + '?mode=ro', uri=True, timeout=WAIT, check_same_thread=False, isolation_level=None)))
            except sqlite3.Error as e: self.say(f'[RED] the read-only shader cache {p} cannot be read and is ignored: {e}')

    def _failed(self, i, e):
        p, db = self.dbs.pop(i)
        self.say(f'[RED] the read-only shader cache {p} cannot be read and is ignored: {e}')
        with contextlib.suppress(sqlite3.Error): db.close()

    def shared(self, name): return name.startswith(self.prefixes) and name != 'metallib/filled.json'

    def _each(self, sql, *args):   # each file's rows, in order
        with self.lock:
            i = 0
            while i < len(self.dbs):
                try: rows = self.dbs[i][1].execute(sql, args).fetchall()
                except sqlite3.Error as e: self._failed(i, e); continue
                i += 1; yield rows

    def get(self, name):
        if self.shared(name):
            for rows in self._each('SELECT b.data FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name=?', name):
                if rows: return bytes(rows[0][0])
        return None

    def has(self, name): return self.shared(name) and any(rows for rows in self._each('SELECT 1 FROM entry WHERE name=?', name))

    def names(self, prefix):
        return {n for rows in self._each('SELECT name FROM entry WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff') for n, in rows if self.shared(n)}

    def close(self):
        with self.lock:
            for _, db in self.dbs:
                with contextlib.suppress(sqlite3.Error): db.close()
            self.dbs = []


class _Files:   # an entry from or to a file, by get and put
    def get(self, name) -> 'bytes | None': raise NotImplementedError
    def put(self, name, data, when=None): raise NotImplementedError

    def put_file(self, name, path):
        with open(path, 'rb') as f: self.put(name, f.read())

    def to_file(self, name, path):   # an entry written to path, whole or not at all; False when there is none
        data = self.get(name)
        if data is None: return False
        tmp = f'{path}.tmp-{os.getpid()}-{threading.get_ident()}'
        with open(tmp, 'wb') as f: f.write(data)
        os.replace(tmp, path)
        return True


class Store(_Files):
    def __init__(self, path, say=print, *, readonly=None, older=()):
        """readonly: another cache (a file, or a cache folder's generations) whose shared entries are read, never copied (regress.py's fallback to the player's).
        older: the files of earlier generations of this one (for_chain), whose Metal compiles are copied into this file when they are read"""
        self.path, self.say = os.fspath(path), say
        self.lock = threading.RLock()   # one connection, for dmsh_build.py's threads: a read or a write of an entry takes microseconds next to a compile
        self.readers, self.wal, self.opener = {}, False, threading.get_ident()   # each other thread's own connection for get, read-only (_reader), where the file's log lets a reader stand beside the writer
        self.full = False   # set by the first write that fails (a full drive): no later write is tried, and the batch it was part of is rolled back
        self.outer = False   # inside the outermost batch: SQLite may end the transaction itself on a failed write, so its own flag cannot tell
        source = readonly if readonly is not None else READ_ONLY
        if source and os.path.realpath(source) == os.path.realpath(self.path): raise ValueError('the writable and read-only shader caches must differ')
        self.use_lock = None
        if source:
            # The writer keeps its own lease, independent of the harness: a killed harness may leave its builder (and forked workers) alive.
            os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
            self.use_lock = open(self.path + '.lock', 'a')
            fcntl.flock(self.use_lock, fcntl.LOCK_SH)
        own = os.path.realpath(self.path)
        sources = generations(source) if source and os.path.isdir(source) else [source] if source else []
        self.readonly = _ReadOnly([p for p in sources if os.path.realpath(p) != own], say) if source else None
        older = [p for p in map(os.fspath, older) if os.path.realpath(p) != own]
        self.older = _ReadOnly(older, say, ('metallib/',)) if older else None
        self._open()

    def _connect(self): return sqlite3.connect(self.path, timeout=WAIT, check_same_thread=False, isolation_level=None)

    def _open(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.db = self._connect()
        try:
            try: self._prepare()
            except sqlite3.OperationalError as e:
                # SQLite notes the file's inode when it opens it and refuses a write once the path names another (SQLITE_READONLY_DBMOVED, "attempt to write a readonly database").
                # A new file on an exFAT drive under macOS 26.6.2 took another inode at SQLite's first write, past SQLite's own guard for such drives (a byte written at open; why it missed is open,
                # docs/reference/packing.md). The file is opened again, once, so that SQLite notes the number it now keeps. A file that moves again fails as before
                if getattr(e, 'sqlite_errorcode', None) != DBMOVED: raise
                with contextlib.suppress(sqlite3.Error): self.db.close()
                self.db = self._connect(); self._prepare()
        except sqlite3.DatabaseError as e:
            if not self._damage(e): raise

    def _prepare(self):   # the file's settings and tables; each step does nothing to a file that has them, so a reopened file takes it again
        self.db.execute('PRAGMA page_size=16384'); self.db.execute('PRAGMA auto_vacuum=INCREMENTAL')   # both only before the first table
        # WAL: a reader in another process does not wait for a writer, and a commit needs no fsync. A drive that cannot share its memory file stays on the rollback journal, unsynced: a stop costs the cache at worst
        self.wal = self.db.execute('PRAGMA journal_mode=WAL').fetchone()[0].lower() == 'wal'
        self.db.execute('PRAGMA synchronous=NORMAL' if self.wal else 'PRAGMA synchronous=OFF')
        for s in SCHEMA: self.db.execute(s)

    def _damage(self, e):   # True when e says the file is damaged, which is now made anew
        if not any(w in str(e) for w in _DAMAGED): return False
        self.say(f'[RED] the shader compile cache {self.path} is damaged, and is made anew: {e}')
        self._close_readers()
        with contextlib.suppress(sqlite3.Error): self.db.close()
        for suffix in ('', '-wal', '-shm', '-journal'):
            with contextlib.suppress(FileNotFoundError): os.unlink(self.path + suffix)
        self._open()
        return True

    def _run(self, sql, *args):   # a statement's rows, or [] when the file was damaged
        with self.lock:
            try: return self.db.execute(sql, args).fetchall()
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise
                return []

    def has(self, name):
        return bool(self._run('SELECT 1 FROM entry WHERE name=?', name)) or any(r is not None and r.has(name) for r in (self.older, self.readonly))

    @contextlib.contextmanager
    def _reader(self):
        """This thread's own read-only connection, or None when the read belongs on the one connection: in the thread that opened the store (a store used by one thread opens no
        other connection), inside a batch (the lock is then this thread's, and its writes are not yet the file's), or on a file without the write-ahead log. The lock is taken to
        ask, so a batch under way in another thread is over first, as the read waited for it before: what the connection then reads is everything written up to here. Entries read
        one after the other on the one connection come out at 190 MB/s (a library's bytes lie on a chain of pages, read one by one), eight threads with a connection each read
        780 MB/s: the library image's write (shader_lib.py write) reads its 300,000 files so"""
        me, reader = threading.get_ident(), None
        with self.lock:
            if me != self.opener and self.wal and not self.outer and not self.db.in_transaction:
                reader = self.readers.get(me)
                if reader is None:
                    try: db = sqlite3.connect(Path(self.path).resolve().as_uri() + '?mode=ro', uri=True, timeout=WAIT, check_same_thread=False, isolation_level=None)
                    except sqlite3.Error: pass
                    else: reader = self.readers[me] = (db, threading.RLock())
                if reader: reader[1].acquire()   # reserve under the registry lock: recovery cannot retire it before the query begins
        try: yield reader[0] if reader else None
        finally:
            if reader: reader[1].release()   # before a failed query takes self.lock again through _run

    def get(self, name):
        """An entry's bytes, None when there is none. One an older generation holds is copied into this file first, so that it stays when that generation goes"""
        sql, rows = 'SELECT b.data FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name=?', None
        with self._reader() as db:
            if db is not None:
                try: rows = db.execute(sql, (name,)).fetchall()
                except sqlite3.Error: pass   # asked again below, on the one connection, which also finds a damaged file
        if rows is None: rows = self._run(sql, name)
        if rows: return bytes(rows[0][0])
        if self.older and (data := self.older.get(name)) is not None:
            self.put(name, data); return data
        return self.readonly.get(name) if self.readonly else None

    def names(self, prefix):   # the set of names starting with prefix
        local = {r[0] for r in self._run('SELECT name FROM entry WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff')}
        for r in (self.older, self.readonly):
            if r is not None: local |= r.names(prefix)
        return local

    def put(self, name, data, when=None):
        """An entry's bytes, replacing what it held. A cache that cannot be written (a full drive) is one [RED] line: the build goes on without it, and the batch this write was part of is rolled back"""
        if self.full: return   # inside a failed batch, a write would land outside it: SQLite may have rolled the transaction back already
        digest = hashlib.blake2b(data, digest_size=16).digest()
        with self.lock:
            try:
                with self.batch():
                    row = self.db.execute('SELECT id FROM blob WHERE hash=?', (digest,)).fetchone()
                    blob = row[0] if row else self.db.execute('INSERT INTO blob(hash, data) VALUES(?, ?)', (digest, data)).lastrowid
                    self.db.execute('INSERT OR REPLACE INTO entry(name, blob, mtime) VALUES(?, ?, ?)', (name, blob, time.time() if when is None else when))
            except sqlite3.OperationalError as e: self._unwritable(e)
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise

    def _unwritable(self, e):
        if any(w in str(e) for w in _DAMAGED): self._damage(e)
        elif not self.full: self.full = True; self.say(f'[RED] the shader compile cache {self.path} cannot be written, the build goes on without it: {e}')

    @contextlib.contextmanager
    def batch(self):
        """Several entries as one write: a stop leaves all of them or none (a shader's outputs and the index naming them), and thousands take one commit. Nests; the outermost commits.
        A write that fails in it, or its commit, fails all of it: it is rolled back and full is set, which a caller holding files for it checks once the batch is over (dmsh_build.py's commit)"""
        with self.lock:
            if self.outer or self.db.in_transaction: yield; return
            was = self.full
            self.db.execute('BEGIN IMMEDIATE'); self.outer = True
            try: yield
            except BaseException:
                if self.db.in_transaction: self.db.rollback()
                raise
            else:
                if self.full and not was:
                    if self.db.in_transaction: self.db.rollback()
                else:
                    try: self.db.commit()
                    except sqlite3.OperationalError as e:
                        with contextlib.suppress(sqlite3.Error):
                            if self.db.in_transaction: self.db.rollback()
                        self._unwritable(e)
            finally: self.outer = False

    def prune_shaders(self, keep, since):
        """Remove the shader/<64 hex digits>/ entries not in keep and all written before since, and the bytes nothing names any more, giving their pages back. Other names stay.
        Returns (shaders removed, bytes of their sources and reflection: their libraries are shared with metallib/)"""
        # The names and times first, from the entry table alone: reading each entry's size with them walked the blob table too, 13 s of every build of Ocarina of Time (an 11.5 GB
        # file, 128,544 such entries) that had nothing to remove, the builds that reuse every shader included. The sizes are read for what goes
        rows = self._run("SELECT name, mtime FROM entry WHERE name>='shader/' AND name<'shader0'")
        newest, names = {}, {}
        for name, mtime in rows:
            m = SHADER_ENTRY.match(name)
            if not m: continue
            d = m.group(1); newest[d] = max(newest.get(d, 0), mtime); names.setdefault(d, []).append(name)
        gone = [d for d in newest if d not in keep and newest[d] < since]
        if not gone: return 0, 0
        size = {}
        for d in gone:
            size[d] = sum(n for name, n in self._run('SELECT e.name, length(b.data) FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name>=? AND e.name<?', f'shader/{d}/', f'shader/{d}0')
                          if not name.endswith('.metallib'))
        with self.lock, self.batch():
            self.db.executemany('DELETE FROM entry WHERE name=?', ((n,) for d in gone for n in names[d]))
            self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
        self._run('PRAGMA incremental_vacuum')
        return len(gone), sum(size.get(d, 0) for d in gone)

    def delete_prefix(self, prefix):
        """Remove every entry whose name starts with prefix, and the bytes nothing names any more, giving their pages back (dmsh_build.py's staged outputs once its image is written)"""
        with self.lock:
            try:
                with self.batch():
                    self.db.execute('DELETE FROM entry WHERE name>=? AND name<?', (prefix, prefix + '\U0010ffff'))
                    self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise
        self._run('PRAGMA incremental_vacuum')

    def holds(self, prefix): return bool(self._run('SELECT 1 FROM entry WHERE name>=? AND name<? LIMIT 1', prefix, prefix + '\U0010ffff'))

    def take(self, path, prefix):
        """Copy the entries of the cache file path whose names start with prefix into this one, as they are (bytes, names, times); an entry this file holds stays. Returns the number taken"""
        hi = prefix + '\U0010ffff'
        with self.lock:
            self.db.execute('ATTACH DATABASE ? AS old', (os.fspath(path),))
            try:
                with self.batch():
                    self.db.execute('INSERT OR IGNORE INTO main.blob(hash, data) SELECT b.hash, b.data FROM old.entry e JOIN old.blob b ON b.id=e.blob WHERE e.name>=? AND e.name<?', (prefix, hi))
                    n = self.db.execute('INSERT OR IGNORE INTO main.entry(name, blob, mtime) SELECT e.name, m.id, e.mtime FROM old.entry e JOIN old.blob b ON b.id=e.blob JOIN main.blob m ON m.hash=b.hash '
                                        'WHERE e.name>=? AND e.name<?', (prefix, hi)).rowcount
            finally: self.db.execute('DETACH DATABASE old')
        return 0 if self.full else n

    def _close_readers(self, thread=None):   # every thread's connection, or one thread's
        with self.lock:
            readers = [self.readers.pop(thread)] if thread in self.readers else [] if thread is not None else list(self.readers.values())
            if thread is None: self.readers = {}
            for db, reading in readers:   # keep the registry locked while retiring: no connection to the old file may be published during recovery
                with reading, contextlib.suppress(sqlite3.Error): db.close()   # an active execute/fetchall finishes before its connection is closed

    def forget(self): self._close_readers(threading.get_ident())   # a thread that reads no more (Served's, when its worker is gone)

    def close(self):   # also folds the write-ahead log into the file, so that one file is left
        with self.lock:
            self._close_readers()   # first: the last connection to close is the one that folds the log
            with contextlib.suppress(sqlite3.Error): self.db.close()
        for r in (self.readonly, self.older):
            if r: r.close()
        if self.use_lock:
            self.use_lock.close(); self.use_lock = None

    def trim(self, limit):
        """Bound the physical database size, oldest-written groups first; split libraries, shader outputs and variants leave whole. Called only with no builders using this regression cache."""
        if limit < 1 << 20: raise ValueError('the shader cache limit must be at least 1 MiB')
        self._run('PRAGMA wal_checkpoint(TRUNCATE)')
        self._run('PRAGMA incremental_vacuum')
        before = os.path.getsize(self.path)
        if before <= limit: return 0
        with self.batch(): self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
        self._run('PRAGMA incremental_vacuum'); self._run('PRAGMA wal_checkpoint(TRUNCATE)')
        rows = self._run('SELECT e.name,e.blob,e.mtime,length(b.data) FROM entry e JOIN blob b ON b.id=e.blob')
        groups, refs, sizes = {}, {}, {}
        for name, blob, when, size in rows:
            parts = name.split('/')
            group = '/'.join(parts[:2]) if parts[0] in ('shader', 'variant', 'stage') else re.sub(r'_x\d*(?=\.metallib$)', '', name)
            ns, newest = groups.setdefault(group, ([], when)); ns.append((name, blob)); groups[group] = ns, max(newest, when)
            refs[blob] = refs.get(blob, 0) + 1; sizes[blob] = size
        # A stopped build's stage is not a reusable result. Prefill bookkeeping must not claim evicted libraries are still present.
        ordered = iter(sorted(groups, key=lambda g: (g != 'metallib/filled.json' and not g.startswith('stage/'), groups[g][1], g)))
        removed = 0
        while os.path.getsize(self.path) > limit:
            names, freed = [], 0
            target = max(os.path.getsize(self.path) - limit, before // 10)
            for group in ordered:
                for name, blob in groups[group][0]:
                    names.append((name,)); refs[blob] -= 1
                    if not refs[blob]: freed += sizes[blob]
                if freed >= target: break
            if not names: break
            with self.batch():
                self.db.executemany('DELETE FROM entry WHERE name=?', names)
                self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
            removed += len(names)
            self._run('PRAGMA incremental_vacuum'); self._run('PRAGMA wal_checkpoint(TRUNCATE)')
        self.say(f'regression shader cache: removed {removed:,} entries, {before / (1 << 30):.2f} → {os.path.getsize(self.path) / (1 << 30):.2f} GiB (limit {limit / (1 << 30):.2f} GiB)')
        return removed


@contextlib.contextmanager
def bounded(path, limit, say=print, *, legacy=None):
    """Builds share this per-game lock; only an idle cache is trimmed, before and after a build, including a failed one. Active builds may temporarily exceed the limit."""
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    with open(str(path) + '.lock', 'a') as lock:
        def trim():
            try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError: return
            try:
                if legacy is not None: adopt(legacy, path, say)
                if path.is_file():
                    store = Store(path, say)
                    try: store.trim(limit() if callable(limit) else limit)
                    finally: store.close()
            finally: fcntl.flock(lock, fcntl.LOCK_UN)
        trim()
        fcntl.flock(lock, fcntl.LOCK_SH)
        try: yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
            trim()


class _Gone(Exception): pass    # a worker's end closed inside its batch
class _Abort(Exception): pass   # a worker's batch raised


class Served:
    """A store for forked worker processes (dmsh_build.py's), which must not open the file themselves: Apple's SQLite (/usr/lib/libsqlite3.dylib, which Xcode's Python and Pythons built
    from source use) calls os_signpost when it opens a database, and in a fork of a process that has opened one, that call reads the parent's logging state at an address the child does
    not have (SIGSEGV in _os_log_preferences_refresh, GitHub issue #2354). So the process that opened the store makes every call: each worker has a pipe to a thread here, which stands
    where that worker's thread stood when the build finished its shaders in threads, on the one connection and under its lock. A worker's batch is that thread's batch, so it is whole
    or absent as before, and is rolled back when the worker dies inside it.
    Made before the fork; start() here once every worker is forked (it starts threads); worker() in each worker gives its store (get, has, names, put, put_file, to_file, delete_prefix, batch, full)"""
    OPS = ('get', 'has', 'names', 'put', 'delete_prefix')

    def __init__(self, store, n):
        ctx = multiprocessing.get_context('fork')
        self.store, self.pipes, self.taken = store, [ctx.Pipe() for _ in range(n)], ctx.Value('i', 0)

    def start(self):
        for here, there in self.pipes:
            there.close()   # so that a dead worker ends its thread's read
            threading.Thread(target=self._serve, args=(here,), daemon=True).start()

    def worker(self):
        with self.taken.get_lock(): i = self.taken.value; self.taken.value += 1
        for j, (here, there) in enumerate(self.pipes):
            here.close()
            if j != i: there.close()
        return Remote(self.pipes[i][1])

    def _serve(self, conn):
        try:
            with contextlib.suppress(_Gone, EOFError, OSError): self._calls(conn, False)
        finally: self.store.forget()

    def _calls(self, conn, inside):   # answers a worker's calls: until its end closes, or inside its batch until the batch ends
        s = self.store
        while True:
            try: op, args = conn.recv()
            except (EOFError, OSError):
                if inside: raise _Gone from None
                return
            if op == 'end' and inside: return
            if op == 'abort' and inside: raise _Abort
            try:
                if op == 'begin' and not inside:
                    try:
                        with s.batch(): conn.send((True, None, s.full)); self._calls(conn, True)
                    except _Abort: pass
                    value = None   # the answer to the batch's end, once it is committed or rolled back
                elif op in self.OPS: value = getattr(s, op)(*args)
                else: raise ValueError(f'shader cache: no call {op!r}')
            except _Gone: raise
            except Exception as e: conn.send((False, e, s.full))
            else: conn.send((True, value, s.full))


class Remote(_Files):
    """A worker's store (Served.worker): each call is made by the process that opened the file"""
    def __init__(self, conn): self.conn, self.lock, self.inside, self.full = conn, threading.RLock(), False, False

    def _call(self, op, *args):
        with self.lock:
            self.conn.send((op, args)); ok, value, self.full = self.conn.recv()
        if not ok: raise value
        return value

    def get(self, name): return self._call('get', name)
    def has(self, name): return self._call('has', name)
    def names(self, prefix): return self._call('names', prefix)
    def put(self, name, data, when=None): self._call('put', name, data, when)
    def delete_prefix(self, prefix): self._call('delete_prefix', prefix)

    @contextlib.contextmanager
    def batch(self):   # as Store.batch: nests, and the lock is held for all of it
        with self.lock:
            if self.inside: yield; return
            self._call('begin'); self.inside = True
            try: yield
            except BaseException: self.inside = False; self._call('abort'); raise
            else: self.inside = False; self._call('end')


def _adopted_name(name):
    """What a file of the old folder is called here, None for one not taken. A shader/ or index/ entry's digest covers the chain's code, which the change to this file moved, so none can hit. A variant/ entry from
    before shader compiles read their source from standard input (f2a50f35) holds its source's file name in its function constants, so its bytes differ from a fresh compile's: few, and cheap to compile again.
    The compile results are the ones worth keeping, and their digests cover the MSL and the compiler alone"""
    m = re.fullmatch(r'metallib/([^/]+)/[0-9a-f]{2}/([^/]+\.metallib)', name)
    return f'metallib/{m.group(1)}/{m.group(2)}' if m else None


def adopt(old, path, say=print):
    """A compile cache from before this file (the folder old, or old.tar: tree_tar.py) into the file path, and the old one removed, so a build folder converts by itself at its next build. A damaged tar costs the work it held.
    Returns the number of entries taken"""
    folder, tar = os.fspath(old), f'{os.fspath(old)}.tar'
    leftovers = [p for p in (f'{folder}.gone', f'{folder}.unpack', f'{tar}.part') if os.path.lexists(p)]
    if not (os.path.isdir(folder) or os.path.isfile(tar) or leftovers): return 0
    n, store = 0, None
    def take(name, read):
        nonlocal n, store
        new = _adopted_name(name.removeprefix('./'))
        if new is None or os.path.basename(name).startswith('._'): return
        store = store or Store(path, say)
        store.put(new, read()); n += 1
    def in_batches(items):
        for i, item in enumerate(items):
            if not i % 2000 and store is not None and store.db.in_transaction: store.db.commit()
            if store is not None and not store.db.in_transaction: store.db.execute('BEGIN IMMEDIATE')
            yield item
        if store is not None and store.db.in_transaction: store.db.commit()
    try:
        if os.path.isfile(tar):
            say(f'moving the compile cache {tar} into one file read in place')
            with tarfile.open(tar, 'r:') as t:
                for m in in_batches(t):
                    source = t.extractfile(m) if m.isfile() else None
                    if source: take(m.name, source.read)
    except (tarfile.TarError, OSError) as e: say(f'[RED] could not read {tar}, the build goes on without it: {e}')
    if os.path.isdir(folder):   # the newer of the two, when a stopped build left both
        say(f'moving the compile cache {folder} into one file read in place')
        def files():
            for d, _, fs in os.walk(folder):
                for f in fs: yield os.path.join(d, f)
        for p in in_batches(files()):
            def read(p=p):
                with open(p, 'rb') as f: return f.read()
            take(os.path.relpath(p, folder), read)
    if store is not None: store.close()
    for p in [folder, tar] + leftovers:
        try:
            if os.path.isdir(p) and not os.path.islink(p): shutil.rmtree(p, onexc=lambda f, q, e: None if isinstance(e, FileNotFoundError) else (_ for _ in ()).throw(e))
            elif os.path.lexists(p): os.unlink(p)
        except OSError as e: say(f'[RED] could not remove the old compile cache {p}: {e}')
    return n


def exists(path): return os.path.isfile(os.fspath(path))


def chain_file(folder, chain): return os.path.join(os.fspath(folder), f'cas-{chain}.db')


def for_chain(folder, chain, say=print, **kw):
    """The cache of chain's build in a cache folder: its own file, cas-<chain>.db, over the folder's other generations (their Metal compiles, copied in when read)"""
    return Store(chain_file(folder, chain), say, older=generations(folder), **kw)


def _file_size(p): return sum(os.path.getsize(p + s) for s in ('', '-wal') if os.path.isfile(p + s))


def drop_generations(folder, chain, say=print, remove=None):
    """After a successful build of chain: remove the cache folder's other generations whole, the files of older chains and cas.db (the one file from before generations).
    The build copied in every Metal compile it reused; a target it compiled nothing for (iOS after a Mac-only build, the Mac after an iPhone-only one) first takes the compiles of the newest
    other generation that holds some, the work its next build of that target can reuse. Nothing goes without the chain's own file, or when copying fails (a full drive).
    remove: what takes a file away (the NSP script's: into the build folder's obsolete/), else it is deleted. Returns the names of the files removed"""
    cur = chain_file(folder, chain)
    others = [p for p in generations(folder) if os.path.realpath(p) != os.path.realpath(cur)]
    if not others or not os.path.isfile(cur): return []
    store = Store(cur, say)
    try:
        for t in TARGETS:
            prefix = f'metallib/{t}/'
            if store.holds(prefix): continue
            for p in others:
                src = _ReadOnly([p], say)
                try: has = bool(next(src._each('SELECT 1 FROM entry WHERE name>=? AND name<? LIMIT 1', prefix, prefix + '\U0010ffff'), None))
                finally: src.close()
                if has:
                    n = store.take(p, prefix)
                    if n: say(f'kept {n:,} {t} Metal compiles of {os.path.basename(p)}: this build compiled none for {t}')
                    break
        if store.full: return []
    finally: store.close()
    gone, size = [], 0
    for p in others:
        n = _file_size(p)
        try:
            for s in ('', '-wal'):
                if os.path.exists(p + s): (remove or os.unlink)(p + s)
            for s in ('-shm', '-journal', '.lock'):
                with contextlib.suppress(FileNotFoundError): os.unlink(p + s)
        except OSError as e: say(f'[RED] could not remove the old shader compile cache {p}: {e}'); continue
        gone.append(os.path.basename(p)); size += n
    if gone: say(f'removed old shader compile caches {", ".join(gone)} ({size / 1e9:.2f} GB){" to obsolete/" if remove else ""}')
    return gone


def main():
    import sys
    if sys.argv[1:2] == ['--run']:
        # The cache format decides no shader output: use this harness's reader for SQLite-based historical builders too, without changing their translator or chain identity.
        import argparse, runpy
        ap = argparse.ArgumentParser(description='Run a shader builder with read-only fallback to another compile cache.')
        ap.add_argument('--readonly', required=True); ap.add_argument('script'); ap.add_argument('args', nargs=argparse.REMAINDER)
        a = ap.parse_args(sys.argv[2:])
        global READ_ONLY
        READ_ONLY = a.readonly
        sys.modules['shader_cache'] = sys.modules[__name__]
        sys.path.insert(0, os.path.dirname(os.path.abspath(a.script)))
        sys.argv = [a.script, *a.args]
        runpy.run_path(a.script, run_name='__main__')
        return
    if len(sys.argv) != 2: sys.exit('usage: shader_cache.py <cas.db>: what it holds')
    s = Store(sys.argv[1])
    kinds = {}
    for name, in s._run('SELECT name FROM entry'): kinds[name.split('/', 1)[0]] = kinds.get(name.split('/', 1)[0], 0) + 1
    print(json.dumps({'entries': kinds, 'blobs': s._run('SELECT count(*) FROM blob')[0][0], 'bytes': os.path.getsize(sys.argv[1])}))
    s.close()


if __name__ == '__main__': main()
