#!/usr/bin/env python3
# The shader build's compile cache as one file, cas.db, read and written in place. It was a folder of a few files per shader (Metroid Dread: 84,541 files), kept as a tar between builds and unpacked
# for each build that ran. Nothing is unpacked now: an entry goes in and comes out by its name, and a file a tool needs is written only then.
# SQLite, as it is in Python and takes several writers: dmsh_build.py's threads share one connection (its worker processes have one each: forked), and regress.py's chain builds and the NSP script can share a game's file at once (the wait is SQLite's).
# Two tables: entry (name, the blob it names, when it was written) and blob (the bytes, once each). A library kept under its MSL's digest and under its shader's digest was one APFS clone of the other; inside
# one file it would be two copies, so bytes are stored by their hash. A file removed from the cache gives its pages back at the next prune (incremental vacuum), with no copy of the file.
# It only saves work: a damaged file costs the work it held and is made anew, and a full drive is one [RED] line. An entry is written and read whole, so its file is never seen half written.
# Names, as in the folder this replaces: metallib/<target>/<digest>[_x].metallib (dmsh_mtlcache.py), shader/<digest>/<mac|ios>/<output>, index/<key>.json, variant/<digest>/<file> (dmsh_build.py).
# Not part of the translation chain: it decides no output (dmsh_chain.py), like dmsh_mtlcache.py.
from pathlib import Path
import collections, concurrent.futures as cf, contextlib, fcntl, hashlib, itertools, json, os, re, shutil, sqlite3, tarfile, threading, time

SCHEMA = ('CREATE TABLE IF NOT EXISTS blob(id INTEGER PRIMARY KEY, hash BLOB UNIQUE NOT NULL, data BLOB NOT NULL)',
          'CREATE TABLE IF NOT EXISTS entry(name TEXT PRIMARY KEY, blob INTEGER NOT NULL, mtime REAL NOT NULL) WITHOUT ROWID')
SHADER_ENTRY = re.compile(r'shader/([0-9a-f]{64})/')
WAIT = 600   # seconds another process's write may hold this one up
_DAMAGED = ('malformed', 'not a database', 'disk image')
ZSTD_LIBRARY = os.path.join(os.path.dirname(__file__), 'sqlite_zstd_bin/libsqlite_zstd.dylib')


def _load_zstd(db):
    enabled = False
    try:
        db.enable_load_extension(True)
        enabled = True
        db.load_extension(os.fspath(ZSTD_LIBRARY))
    except (AttributeError, NotImplementedError, OSError, sqlite3.Error) as e:
        raise RuntimeError(f'sqlite-zstd cannot load {ZSTD_LIBRARY}: {e}; use Python with SQLite extension loading and rebuild with dm/tools/sqlite_zstd_bin/build.sh <scratch-folder>') from e
    finally:
        if enabled: db.enable_load_extension(False)


@contextlib.contextmanager
def _ownership(path, exclusive=False):
    fd = os.open(f'{path}.lock', os.O_CREAT | os.O_RDWR, 0o600)
    try:
        try: fcntl.flock(fd, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        except BlockingIOError as e: raise RuntimeError(f'shader compile cache {path} is busy; finish other builds before conversion') from e
        except OSError:
            if exclusive: raise
            # A raw cache can still use SQLite on a drive without flock; conversion cannot.
        yield
    finally: os.close(fd)


class Store:
    def __init__(self, path, say=print):
        self.path, self.say = os.path.realpath(path), say
        self.lock = threading.RLock()   # one connection, for dmsh_build.py's threads: a read or a write of an entry takes microseconds next to a compile
        self.full = False
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.owner = _ownership(self.path)
        self.owner.__enter__()
        self.db: sqlite3.Connection
        try: self._open()
        except BaseException:
            self.close()
            raise

    def _open(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=WAIT, check_same_thread=False, isolation_level=None)
        try:
            self.compressed = self.db.execute("SELECT type FROM sqlite_master WHERE name='blob'").fetchone() == ('view',)
            if self.compressed: _load_zstd(self.db)
            self.db.execute('PRAGMA page_size=16384'); self.db.execute('PRAGMA auto_vacuum=INCREMENTAL')   # both only before the first table
            # WAL: a reader in another process does not wait for a writer, and a commit needs no fsync. A drive that cannot share its memory file stays on the rollback journal, unsynced: a stop costs the cache at worst
            if self.db.execute('PRAGMA journal_mode=WAL').fetchone()[0].lower() == 'wal': self.db.execute('PRAGMA synchronous=NORMAL')
            else: self.db.execute('PRAGMA synchronous=OFF')
            if not self.compressed:
                for s in SCHEMA: self.db.execute(s)
        except sqlite3.DatabaseError as e:
            if not self._damage(e): raise

    def forked(self):
        """A connection of its own in a forked process (dmsh_build.py's workers): SQLite's must not be used across a fork, not even closed, so the inherited one is only kept"""
        self._inherited = self.db
        self.lock = threading.RLock()
        self._open()

    def _damage(self, e):   # True when e says the file is damaged, which is now made anew
        if not any(w in str(e) for w in _DAMAGED): return False
        self.say(f'[RED] the shader compile cache {self.path} is damaged, and is made anew: {e}')
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

    def has(self, name): return bool(self._run('SELECT 1 FROM entry WHERE name=?', name))

    def get(self, name):   # an entry's bytes, None when there is none
        rows = self._run('SELECT b.data FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name=?', name)
        return bytes(rows[0][0]) if rows else None

    def names(self, prefix):   # the set of names starting with prefix
        return {r[0] for r in self._run('SELECT name FROM entry WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff')}

    def put(self, name, data, when=None):
        """An entry's bytes, replacing what it held. A cache that cannot be written (a full drive) is one [RED] line: the build goes on without it"""
        digest = hashlib.blake2b(data, digest_size=16).digest()
        with self.lock:
            try:
                with self.batch():
                    row = self.db.execute('SELECT id FROM blob WHERE hash=?', (digest,)).fetchone()
                    if not row:
                        self.db.execute('INSERT INTO blob(hash, data) VALUES(?, ?)', (digest, data))
                        row = self.db.execute('SELECT id FROM blob WHERE hash=?', (digest,)).fetchone()   # lastrowid does not follow a transparent view's trigger
                    blob = row[0]
                    self.db.execute('INSERT OR REPLACE INTO entry(name, blob, mtime) VALUES(?, ?, ?)', (name, blob, time.time() if when is None else when))
            except sqlite3.OperationalError as e:
                if any(w in str(e) for w in _DAMAGED): self._damage(e)
                elif not self.full: self.full = True; self.say(f'[RED] the shader compile cache {self.path} cannot be written, the build goes on without it: {e}')
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise

    def put_file(self, name, path):
        with open(path, 'rb') as f: self.put(name, f.read())

    def to_file(self, name, path):   # an entry written to path, whole or not at all; False when there is none
        data = self.get(name)
        if data is None: return False
        tmp = f'{path}.tmp-{os.getpid()}-{threading.get_ident()}'
        with open(tmp, 'wb') as f: f.write(data)
        os.replace(tmp, path)
        return True

    @contextlib.contextmanager
    def batch(self):
        """Several entries as one write: a stop leaves all of them or none (a shader's outputs and the index naming them), and thousands take one commit. Nests; the outermost commits"""
        with self.lock:
            if self.db.in_transaction: yield; return
            self.db.execute('BEGIN IMMEDIATE')
            try: yield
            except BaseException:
                if self.db.in_transaction: self.db.rollback()
                raise
            else: self.db.commit()

    def prune_shaders(self, keep, since):
        """Remove the shader/<64 hex digits>/ entries not in keep and all written before since, and the bytes nothing names any more, giving their pages back. Other names stay.
        Returns (shaders removed, bytes of their sources and reflection: their libraries are shared with metallib/)"""
        rows = self._run("SELECT e.name, e.mtime, length(b.data) FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name>='shader/' AND e.name<'shader0'")
        newest, size, names = {}, {}, {}
        for name, mtime, n in rows:
            m = SHADER_ENTRY.match(name)
            if not m: continue
            d = m.group(1); newest[d] = max(newest.get(d, 0), mtime); names.setdefault(d, []).append(name)
            if not name.endswith('.metallib'): size[d] = size.get(d, 0) + n
        gone = [d for d in newest if d not in keep and newest[d] < since]
        if not gone: return 0, 0
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

    def close(self):   # also folds the write-ahead log into the file, so that one file is left
        with self.lock:
            if hasattr(self, 'db'):
                with contextlib.suppress(sqlite3.Error): self.db.close()
            if self.owner is not None:
                self.owner.__exit__(None, None, None); self.owner = None


def _reclaim(db):
    # Batches bound the vacuum's WAL, including the first large conversion.
    while db.execute('PRAGMA freelist_count').fetchone()[0]:
        before = db.execute('PRAGMA freelist_count').fetchone()[0]
        db.execute('PRAGMA incremental_vacuum(1024)').fetchall()
        if db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()[0]:
            raise RuntimeError('shader cache reclamation is busy; it will retry after the next successful build')
        if db.execute('PRAGMA freelist_count').fetchone()[0] >= before:
            raise RuntimeError('shader cache needs incremental auto_vacuum to reclaim free pages')


def _remove_raw_part(path):
    for suffix in ('', '-journal', '-wal', '-shm'):
        with contextlib.suppress(FileNotFoundError): os.unlink(path + suffix)


def _parallel_blobs(db, workers, compress=False):
    """Bounded batches, native codec on independent memory connections; only the caller touches the cache."""
    # shortcut: the 8 MiB/256-row limit bounds encoded input, not decoded expansion; budget output bytes before supporting unusually large compressed blobs.
    local, connections = threading.local(), []
    def init():
        local.db = sqlite3.connect(':memory:', check_same_thread=False)
        connections.append(local.db); _load_zstd(local.db)
    def work(rows):
        result = []
        for ident, digest, data, marker in rows:
            if compress: data = local.db.execute('SELECT zstd_compress_col(?,3,-1,1)', (data,)).fetchone()[0]
            elif marker is not None: data = local.db.execute('SELECT zstd_decompress_col(?,0,?,1)', (data, marker)).fetchone()[0]
            result.append((ident, digest, data))
        return result
    def batches():
        last = None
        while True:
            where = 'WHERE _data_dict IS NULL' if compress else 'WHERE 1'
            if last is not None: where += ' AND id>?'
            # Keyset scans avoid keeping a reader snapshot open while the writer commits or vacuum runs.
            cur = db.execute(f'SELECT id,hash,data,{"NULL" if compress else "_data_dict"} FROM _blob_zstd NOT INDEXED {where} ORDER BY id LIMIT 256', () if last is None else (last,))
            rows, size = [], 0
            try:
                for row in cur:
                    rows.append(row); last = row[0]; size += len(row[2])
                    if size >= 8 * 1024**2: break
            finally: cur.close()
            if not rows: return
            yield rows
    try:
        with cf.ThreadPoolExecutor(workers, initializer=init) as pool:
            pending = collections.deque()
            for batch in batches():
                pending.append(pool.submit(work, batch))
                if len(pending) >= workers: yield pending.popleft().result()
            while pending: yield pending.popleft().result()
    finally:
        for connection in connections: connection.close()


def _compress_parallel(db, workers):
    for batch in _parallel_blobs(db, workers, compress=True):
        with db:
            db.execute('BEGIN IMMEDIATE')
            db.executemany('UPDATE _blob_zstd SET data=?,_data_dict=-1 WHERE id=? AND hash=? AND _data_dict IS NULL', ((data, ident, digest) for ident, digest, data in batch))


def _raw_copy(db, part, workers=1):
    out = sqlite3.connect(part)
    try:
        out.execute('PRAGMA page_size=16384'); out.execute('PRAGMA auto_vacuum=INCREMENTAL')
        for sql in SCHEMA: out.execute(sql)
        with out:
            out.executemany('INSERT INTO blob VALUES(?,?,?)', itertools.chain.from_iterable(_parallel_blobs(db, workers)) if workers > 1 else db.execute('SELECT id,hash,data FROM blob'))
            out.executemany('INSERT INTO entry VALUES(?,?,?)', db.execute('SELECT name,blob,mtime FROM entry'))
        # Check written bytes against the cache's content hashes without decoding the source again.
        for src, dst in itertools.zip_longest(db.execute('SELECT id,hash FROM blob ORDER BY id'), out.execute('SELECT id,hash,data FROM blob ORDER BY id')):
            if src is None or dst is None or src != dst[:2]: raise RuntimeError('raw shader cache validation failed: contents differ')
            if hashlib.blake2b(dst[2], digest_size=16).digest() != dst[1]: raise RuntimeError('raw shader cache validation failed: blob hash differs')
        sql = 'SELECT name,blob,mtime FROM entry ORDER BY name'
        for src, dst in itertools.zip_longest(db.execute(sql), out.execute(sql)):
            if src != dst: raise RuntimeError('raw shader cache validation failed: contents differ')
        if out.execute('PRAGMA quick_check').fetchone() != ('ok',) or out.execute('SELECT 1 FROM entry e LEFT JOIN blob b ON b.id=e.blob WHERE b.id IS NULL LIMIT 1').fetchone():
            raise RuntimeError('raw shader cache validation failed: database or entry references')
    finally: out.close()


def _compressed_copy(db, path, part, workers):
    """Read and compress on worker connections; write a compact replacement once."""
    local, connections = threading.local(), []
    def init():
        local.db = sqlite3.connect(Path(path).as_uri() + '?mode=ro', uri=True, timeout=0, check_same_thread=False)
        connections.append(local.db); _load_zstd(local.db)
    def work(span):
        return local.db.execute('SELECT id,hash,zstd_compress_col(data,3,-1,1) FROM blob WHERE id>=? AND id<=? ORDER BY id', span).fetchall()
    def spans():
        cur = db.execute('SELECT id,length(data) FROM blob ORDER BY id')
        first, size, count = None, 0, 0
        try:
            for ident, length in cur:
                if first is None: first = ident
                size += length; count += 1
                if size >= 8 * 1024**2 or count >= 256:
                    yield first, ident
                    first, size, count = None, 0, 0
            if first is not None: yield first, ident
        finally: cur.close()
    out = sqlite3.connect(part)
    try:
        out.execute('PRAGMA page_size=16384'); out.execute('PRAGMA auto_vacuum=INCREMENTAL')
        for sql in SCHEMA: out.execute(sql)
        # Write the private output directly; enable WAL only after installation.
        out.execute('PRAGMA synchronous=NORMAL')
        _load_zstd(out)
        out.execute('SELECT zstd_enable_transparent(?)', (json.dumps({'table': 'blob', 'column': 'data', 'compression_level': 3, 'dict_chooser': "'[nodict]'"}),)).fetchall()
        with cf.ThreadPoolExecutor(workers, initializer=init) as pool, out:
            pending = collections.deque()
            for span in spans():
                pending.append(pool.submit(work, span))
                if len(pending) >= workers:
                    out.executemany('INSERT INTO _blob_zstd(id,hash,data,_data_dict) VALUES(?,?,?,-1)', pending.popleft().result())
            while pending:
                out.executemany('INSERT INTO _blob_zstd(id,hash,data,_data_dict) VALUES(?,?,?,-1)', pending.popleft().result())
            out.executemany('INSERT INTO entry VALUES(?,?,?)', db.execute('SELECT name,blob,mtime FROM entry'))
    finally:
        out.close()
        for connection in connections: connection.close()


def maintain(path, *, compress=False, decompress=False, workers=1, say=print):
    """Maintain a compressed cache, optionally convert its format. Errors propagate: builds warn, the standalone command fails."""
    if compress and decompress: raise ValueError('choose compression or decompression, not both')
    if workers < 1: raise ValueError('workers must be positive')
    path = os.path.realpath(path)
    if not os.path.isfile(path): raise FileNotFoundError(f'no shader compile cache at {path}')
    start, before = time.monotonic(), os.stat(path).st_blocks * 512
    with _ownership(path, exclusive=compress or decompress):
        db = sqlite3.connect(path, timeout=0, isolation_level=None)
        part = path + ('.raw.part' if decompress else '.compressed.part')
        try:
            compressed = db.execute("SELECT type FROM sqlite_master WHERE name='blob'").fetchone() == ('view',)
            if not compressed and not compress: return
            if db.execute('PRAGMA journal_mode').fetchone()[0].lower() == 'wal': db.execute('PRAGMA synchronous=NORMAL')
            _load_zstd(db)
            if compress and not compressed:
                say(f'compressing shader compile cache into a compact replacement for {path}')
                _remove_raw_part(part)
                _compressed_copy(db, path, part, workers)
                if db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()[0]: raise RuntimeError('shader cache is busy; compressed conversion was not installed')
                db.close(); db = None
                os.replace(part, path)
                db = sqlite3.connect(path, timeout=0, isolation_level=None)
                db.execute('PRAGMA journal_mode=WAL').fetchall()
            elif decompress:
                say(f'restoring raw shader compile cache {path}')
                _remove_raw_part(part)
                if workers == 1: _raw_copy(db, part)
                else: _raw_copy(db, part, workers)
                if db.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()[0]: raise RuntimeError('shader cache is busy; raw conversion was not installed')
                db.close(); db = None
                os.replace(part, path)
                db = sqlite3.connect(path, timeout=0, isolation_level=None)
                db.execute('PRAGMA journal_mode=WAL').fetchall()
            else:
                # Otherwise the extension's GROUP BY sorts whole uncompressed payloads into temporary files.
                db.execute('CREATE INDEX IF NOT EXISTS blob_pending_size ON _blob_zstd(_data_dict,length(data)) WHERE _data_dict IS NULL')
                # v0.3.5 divides by average pending size; use its own compact/nodict encoding for the one deduplicated empty blob first.
                db.execute('UPDATE _blob_zstd SET data=zstd_compress_col(data,3,-1,1),_data_dict=-1 WHERE _data_dict IS NULL AND length(data)=0')
                if workers > 1: _compress_parallel(db, workers)
                else:
                    while db.execute('SELECT zstd_incremental_maintenance(2,1)').fetchone()[0]:
                        say(f'compressing shader compile cache: {time.monotonic() - start:.0f} s')
                _reclaim(db)
        finally:
            if db is not None: db.close()
            if compress or decompress: _remove_raw_part(part)
    after = os.stat(path).st_blocks * 512
    say(f'shader compile cache: {before / 2**20:.1f} -> {after / 2**20:.1f} MiB on disk, {time.monotonic() - start:.1f} s')


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


def main():
    import sys
    if len(sys.argv) != 2: sys.exit('usage: shader_cache.py <cas.db>: what it holds')
    s = Store(sys.argv[1])
    kinds = {}
    for name, in s._run('SELECT name FROM entry'): kinds[name.split('/', 1)[0]] = kinds.get(name.split('/', 1)[0], 0) + 1
    print(json.dumps({'entries': kinds, 'blobs': s._run('SELECT count(*) FROM blob')[0][0], 'bytes': os.path.getsize(sys.argv[1])}))
    s.close()


if __name__ == '__main__': main()
