#!/usr/bin/env python3
# The shader build's compile cache as one file, cas.db, read and written in place. It was a folder of a few files per shader (Metroid Dread: 84,541 files), kept as a tar between builds and unpacked
# for each build that ran. Nothing is unpacked now: an entry goes in and comes out by its name, and a file a tool needs is written only then.
# SQLite, as it is in Python and takes several writers: dmsh_build.py's threads share one connection, and regress.py's chain builds and the NSP script can share a game's file at once (the wait is SQLite's).
# Two tables: entry (name, the blob it names, when it was written) and blob (the bytes, once each). A library kept under its MSL's digest and under its shader's digest was one APFS clone of the other; inside
# one file it would be two copies, so bytes are stored by their hash. A file removed from the cache gives its pages back at the next prune (incremental vacuum), with no copy of the file.
# It only saves work: a damaged file costs the work it held and is made anew, and a full drive is one [RED] line. An entry is written and read whole, so its file is never seen half written.
# Names, as in the folder this replaces: metallib/<target>/<digest>[_x].metallib (dmsh_mtlcache.py), shader/<digest>/<mac|ios>/<output>, index/<key>.json, variant/<digest>/<file> (dmsh_build.py).
# Not part of the translation chain: it decides no output (dmsh_chain.py), like dmsh_mtlcache.py.
# Compressed entries (DMSH_CAS_ZSTD=1): the same two tables under other names, zentry and zblob, each blob a zstd frame (level 3) of the bytes, hashed as the bytes are. On Ocarina of Time's library, Metal
# sources compress 8.1x, libraries 1.9x and reflection 5.8x, so the file holds about 0.38 of what it did, during a build (beside the image it writes) and after. A store reads both pairs and takes
# the newer entry of a name; a write removes the name from the other pair. A store from before this, or a Python without zstd, does not see compressed entries: a miss, never wrong bytes.
# One in, one out (DMSH_ONE_IN_ONE_OUT=1, which also turns on DMSH_CAS_ZSTD and dmsh_mtlcache.py's DMSH_PREFILL_LAZY): when a build files a key's index/<key>.json under another digest, the key's
# older entry (shader/<old digest>/<label>/<key>_*, written before this store opened) goes in the same transaction, with the compiled libraries that only older metallib/ names still hold for its
# bytes. A game's one cas.db then stays about one set of entries in size across a chain change, instead of holding the old set until the prune after the build (prune_shaders). Libraries
# (b-<chain>.img) are never touched. Pages go back every EVICT_VACUUM bytes. Two builds of different chains sharing one evicting file each remove the other's entries: misses, never wrong bytes.
import collections, contextlib, hashlib, json, os, re, shutil, sqlite3, tarfile, threading, time

SCHEMA = ('CREATE TABLE IF NOT EXISTS blob(id INTEGER PRIMARY KEY, hash BLOB UNIQUE NOT NULL, data BLOB NOT NULL)',
          'CREATE TABLE IF NOT EXISTS entry(name TEXT PRIMARY KEY, blob INTEGER NOT NULL, mtime REAL NOT NULL) WITHOUT ROWID')
ZSCHEMA = tuple(s.replace(' blob(', ' zblob(').replace(' entry(', ' zentry(') for s in SCHEMA)
PLAIN, ZSTD = ('entry', 'blob'), ('zentry', 'zblob')   # (entry table, blob table)
SHADER_ENTRY = re.compile(r'shader/([0-9a-f]{64})/')
INDEX_ENTRY = re.compile(r'index/([^/]+)\.json')
WAIT = 600   # seconds another process's write may hold this one up
EVICT_VACUUM = 256 << 20   # bytes one in, one out frees between incremental vacuums
ZSTD_LEVEL = 3
_DAMAGED = ('malformed', 'not a database', 'disk image')


def _zstd_module():   # compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API; None without either
    try:
        from compression import zstd
        return zstd
    except ImportError:
        try:
            from backports import zstd   # pyright: ignore[reportMissingImports]  # an optional package, only Python before 3.14 needs it
            return zstd
        except ImportError: return None
_zstd = _zstd_module()


def _env_on(*names):   # the first of these environment switches that is set decides (1 on, anything else off); none set: off
    for n in names:
        if n in os.environ: return os.environ[n] == '1'
    return False


class Store:
    def __init__(self, path, say=print, compress=None, evict=None):
        """compress: write compressed entries (header); evict: one in, one out (header). Unset, DMSH_CAS_ZSTD / DMSH_ONE_IN_ONE_OUT decide, off by default"""
        self.path, self.say = os.fspath(path), say
        self.lock = threading.RLock()   # one connection, for dmsh_build.py's threads: a read or a write of an entry takes microseconds next to a compile
        self.full = False
        want_z = _env_on('DMSH_CAS_ZSTD', 'DMSH_ONE_IN_ONE_OUT') if compress is None else compress
        if want_z and _zstd is None: say('the shader compile cache needs zstd for compressed entries (Python 3.14, or pip install backports.zstd): its entries stay uncompressed')
        self.z = bool(want_z and _zstd is not None)
        self.w = ZSTD if self.z else PLAIN   # the pair this store writes
        self.evict = _env_on('DMSH_ONE_IN_ONE_OUT') if evict is None else bool(evict)
        self.opened = time.time()   # one in, one out takes only entries written before this
        self.evicted, self._evicting, self._unvacuumed = collections.Counter(), collections.Counter(), 0   # shaders, entries, libraries, bytes: committed, and in the open transaction
        self._open()

    def _open(self):
        self.tables = self.pairs = [PLAIN]
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=WAIT, check_same_thread=False, isolation_level=None)
        try:
            self.db.execute('PRAGMA page_size=16384'); self.db.execute('PRAGMA auto_vacuum=INCREMENTAL')   # both only before the first table
            # WAL: a reader in another process does not wait for a writer, and a commit needs no fsync. A drive that cannot share its memory file stays on the rollback journal, unsynced: a stop costs the cache at worst
            if self.db.execute('PRAGMA journal_mode=WAL').fetchone()[0].lower() == 'wal': self.db.execute('PRAGMA synchronous=NORMAL')
            else: self.db.execute('PRAGMA synchronous=OFF')
            for s in SCHEMA + (ZSCHEMA if self.z else ()): self.db.execute(s)
            self._find_tables()
            if self.evict:   # which entries name a blob, for eviction; the write-ahead log goes back to 64 MB after each checkpoint
                for e, _ in self.pairs: self.db.execute(f'CREATE INDEX IF NOT EXISTS {e}_blob ON {e}(blob)')
                self.db.execute('PRAGMA journal_size_limit=67108864')
        except sqlite3.DatabaseError as e:
            if not self._damage(e): raise

    def _find_tables(self):   # tables: the pairs in the file (removals cover each); pairs: those this store reads, the compressed one first. Another process may add zentry later
        self.tables = ([ZSTD] if self.db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='zentry'").fetchone() else []) + [PLAIN]
        self.pairs = [p for p in self.tables if p == PLAIN or _zstd is not None]

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

    @staticmethod
    def _bytes(pair, data):   # a blob as stored → the entry's bytes; None for a damaged compressed frame (a miss, as a damaged file costs the work it held)
        if pair != ZSTD: return bytes(data)
        assert _zstd is not None   # a store reads the compressed pair only with zstd
        try: return _zstd.decompress(data)
        except Exception: return None   # zstd.ZstdError, or a frame of another kind

    def has(self, name): return any(self._run(f'SELECT 1 FROM {e} WHERE name=?', name) for e, _ in self.pairs)

    def get(self, name):   # an entry's bytes, None when there is none; of a name in both pairs, the newer
        best = None
        for pair in self.pairs:
            rows = self._run(f'SELECT e.mtime, b.data FROM {pair[0]} e JOIN {pair[1]} b ON b.id=e.blob WHERE e.name=?', name)
            if rows and (best is None or rows[0][0] > best[0]):
                data = self._bytes(pair, rows[0][1])
                if data is not None: best = (rows[0][0], data)
        return best[1] if best else None

    def names(self, prefix):   # the set of names starting with prefix
        return {r[0] for e, _ in self.pairs for r in self._run(f'SELECT name FROM {e} WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff')}

    def put(self, name, data, when=None):
        """An entry's bytes, replacing what it held. A cache that cannot be written (a full drive) is one [RED] line: the build goes on without it"""
        digest = hashlib.blake2b(data, digest_size=16).digest()
        stored = data if not self.z else None if self._run(f'SELECT 1 FROM {self.w[1]} WHERE hash=?', digest) else self._compress(data)   # bytes stored already: no frame to make
        with self.lock:
            try:
                with self.batch():
                    before = self._index_digest(name) if self.evict else None
                    if self.evict and INDEX_ENTRY.fullmatch(name): self._evicting['filed'] += 1
                    self._write(name, digest, stored, time.time() if when is None else when, data)
                    if before and before != _digest_of(data): self._evict_key(name, before)
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

    @staticmethod
    def _compress(data):
        assert _zstd is not None   # self.z only with zstd
        return _zstd.compress(data, ZSTD_LEVEL)

    def _write(self, name, digest, stored, when, data):
        """One entry into the pair this store writes, in a transaction; the name leaves the other pair, whose entry is older. stored: the bytes as this pair keeps them, None when put found
        them stored already (compressed here when they went meanwhile)"""
        e, b = self.w
        replaced = self.db.execute(f'SELECT blob FROM {e} WHERE name=?', (name,)).fetchone()
        row = self.db.execute(f'SELECT id FROM {b} WHERE hash=?', (digest,)).fetchone()
        blob = row[0] if row else self.db.execute(f'INSERT INTO {b}(hash, data) VALUES(?, ?)', (digest, self._compress(data) if stored is None else stored)).lastrowid
        self.db.execute(f'INSERT OR REPLACE INTO {e}(name, blob, mtime) VALUES(?, ?, ?)', (name, blob, when))
        if self.evict and replaced and replaced[0] != blob: self._evicting['bytes'] += self._drop_unnamed(self.w, replaced[0])   # else its bytes go at the next prune
        for other in self.tables:
            if other == self.w: continue
            gone = self.db.execute(f'SELECT blob FROM {other[0]} WHERE name=?', (name,)).fetchone()
            if not gone: continue
            self.db.execute(f'DELETE FROM {other[0]} WHERE name=?', (name,))
            if self.evict: self._evicting['bytes'] += self._drop_unnamed(other, gone[0])

    # --- one in, one out (header): inside put's transaction
    def _index_digest(self, name):   # the digest an older index/<key>.json entry names (written before this store opened); None for another name, or none
        if not INDEX_ENTRY.fullmatch(name): return None
        best = None   # of a name in both pairs, the newer, as get() reads it
        for pair in self.pairs:
            row = self.db.execute(f'SELECT e.mtime, b.data FROM {pair[0]} e JOIN {pair[1]} b ON b.id=e.blob WHERE e.name=? AND e.mtime<?', (name, self.opened)).fetchone()
            if row and (best is None or row[0] > best[0]): best = (row[0], pair, row[1])
        if best is None: return None
        data = self._bytes(best[1], best[2])
        return None if data is None else _digest_of(data)

    def _evict_key(self, name, digest):
        """_evict_entry in a savepoint: a failure that is not a damaged file (a damaged frame, an odd name, a full drive) undoes only this eviction, as one line, and the write goes on"""
        self.db.execute('SAVEPOINT evict'); counted = collections.Counter(self._evicting)
        try:
            m = INDEX_ENTRY.fullmatch(name)
            if m: self._evict_entry(m.group(1), digest)
        except Exception as e:
            self.db.execute('ROLLBACK TO evict'); self.db.execute('RELEASE evict'); self._evicting = counted   # nothing of it happened
            if isinstance(e, sqlite3.DatabaseError) and any(w in str(e) for w in _DAMAGED): raise
            self._evicting['errors'] += 1; self.say(f'one in, one out: the older entry of {name} stays ({type(e).__name__}: {e})')
        else: self.db.execute('RELEASE evict')

    def _drop_unnamed(self, pair, blob):   # a blob of pair once no entry names it; → its stored size (0 when it is still named, or gone)
        if self.db.execute(f'SELECT 1 FROM {pair[0]} WHERE blob=? LIMIT 1', (blob,)).fetchone(): return 0
        size = self.db.execute(f'SELECT length(data) FROM {pair[1]} WHERE id=?', (blob,)).fetchone()
        self.db.execute(f'DELETE FROM {pair[1]} WHERE id=?', (blob,))
        return size[0] if size else 0

    def _evict_entry(self, key, digest):
        """The key's entry under its older digest, and the compiled libraries only older metallib/ names still hold for its bytes. Another key with the same digest keeps its files. Bytes that
        the pair this store writes holds too (a plain library that a compressed entry of this build names) keep their metallib/ names, moved to that pair"""
        prefix, counts = f'shader/{digest}/', self._evicting
        for pair in self.pairs:
            e, b = pair
            rows = [(n, x) for n, x in self.db.execute(f'SELECT name, blob FROM {e} WHERE name>=? AND name<? AND mtime<?', (prefix, prefix + '\U0010ffff', self.opened))
                    if n.count('/') == 3 and n.split('/')[3].startswith(key + '_')]   # shader/<digest>/<label>/<key>_<output>
            if not rows: continue
            self.db.executemany(f'DELETE FROM {e} WHERE name=?', ((n,) for n, _ in rows))
            counts['shaders'] += 1; counts['entries'] += len(rows)
            for blob in {x for _, x in rows}:
                others = self.db.execute(f'SELECT name, mtime FROM {e} WHERE blob=?', (blob,)).fetchall()
                if not all(n.startswith('metallib/') and m < self.opened for n, m in others): continue   # a newer entry, or another shader's, still names these bytes
                if others and pair != self.w:
                    h = self.db.execute(f'SELECT hash FROM {b} WHERE id=?', (blob,)).fetchone()
                    there = h and self.db.execute(f'SELECT id FROM {self.w[1]} WHERE hash=?', h).fetchone()
                    if there:   # the same bytes in the pair this store writes: the library stays, named there
                        self.db.executemany(f'INSERT OR IGNORE INTO {self.w[0]}(name, blob, mtime) VALUES(?, ?, ?)', ((n, there[0], m) for n, m in others))
                        self.db.executemany(f'DELETE FROM {e} WHERE name=?', ((n,) for n, _ in others))
                        counts['bytes'] += self._drop_unnamed(pair, blob); continue
                self.db.executemany(f'DELETE FROM {e} WHERE name=?', ((n,) for n, _ in others))
                counts['libraries'] += len(others); counts['bytes'] += self._drop_unnamed(pair, blob)

    @contextlib.contextmanager
    def batch(self):
        """Several entries as one write: a stop leaves all of them or none (a shader's outputs and the index naming them), and thousands take one commit. Nests; the outermost commits"""
        with self.lock:
            if self.db.in_transaction: yield; return
            self.db.execute('BEGIN IMMEDIATE')
            try: yield
            except BaseException:
                if self.db.in_transaction: self.db.rollback()
                self._evicting.clear()
                raise
            else: self.db.commit()
            if self._evicting:
                self.evicted.update(self._evicting); self._unvacuumed += self._evicting['bytes']; self._evicting.clear()
            if self._unvacuumed >= EVICT_VACUUM: self._vacuum()

    def _vacuum(self):
        """After a commit, outside its error handling. It waits at most a second for another process holding the file (the build's threads wait on the lock meanwhile): a vacuum that
        cannot run loses nothing, and the next one takes its pages"""
        self.db.execute('PRAGMA busy_timeout=1000')
        try: self.db.execute('PRAGMA incremental_vacuum').fetchall(); self._unvacuumed = 0   # each step frees a page: run it to its end
        except sqlite3.Error as e: self._unvacuumed = EVICT_VACUUM // 2; self.say(f'one in, one out: incremental vacuum skipped: {e}')   # tried again after half the amount
        finally: self.db.execute(f'PRAGMA busy_timeout={WAIT * 1000}')

    def prune_shaders(self, keep, since):
        """Remove the shader/<64 hex digits>/ entries not in keep and all written before since, and the bytes nothing names any more, giving their pages back. Other names stay.
        An entry goes by its name and the time scanned, so one written again meanwhile (another build sharing the file) stays, and the bytes go by what no entry names inside the transaction.
        Covers both pairs, also one this store cannot read. Returns (shaders removed, bytes of their sources and reflection as stored: their libraries are shared with metallib/)"""
        newest, size, names = {}, {}, {}
        with self.lock: self._find_tables()
        for e, b in self.tables:
            for name, mtime, n in self._run(f"SELECT e.name, e.mtime, length(b.data) FROM {e} e JOIN {b} b ON b.id=e.blob WHERE e.name>='shader/' AND e.name<'shader0'"):
                m = SHADER_ENTRY.match(name)
                if not m: continue
                d = m.group(1); newest[d] = max(newest.get(d, 0), mtime); names.setdefault(d, []).append((e, name, mtime))
                if not name.endswith('.metallib'): size[d] = size.get(d, 0) + n
        gone = [d for d in newest if d not in keep and newest[d] < since]
        if not gone: return 0, 0
        with self.lock, self.batch():
            for e, b in self.tables:
                self.db.executemany(f'DELETE FROM {e} WHERE name=? AND mtime=?', ((n, m) for d in gone for t, n, m in names[d] if t == e))
                self.db.execute(f'DELETE FROM {b} WHERE id NOT IN (SELECT blob FROM {e})')
        self._run('PRAGMA incremental_vacuum')
        return len(gone), sum(size.get(d, 0) for d in gone)

    def delete_prefix(self, prefix):
        """Remove every entry whose name starts with prefix, and the bytes nothing names any more, giving their pages back (dmsh_build.py's staged outputs once its image is written)"""
        with self.lock:
            try:
                with self.batch():
                    self._find_tables()
                    for e, b in self.tables:
                        self.db.execute(f'DELETE FROM {e} WHERE name>=? AND name<?', (prefix, prefix + '\U0010ffff'))
                        self.db.execute(f'DELETE FROM {b} WHERE id NOT IN (SELECT blob FROM {e})')
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise
        self._run('PRAGMA incremental_vacuum')

    def close(self):   # also folds the write-ahead log into the file, so that one file is left
        with self.lock:
            if self.evict and self._unvacuumed: self._vacuum()
            e = self.evicted
            if e['filed']:   # a build's store: what it replaced (the other tools that open the file file no shader)
                self.say(f'one in, one out: {e["shaders"]:,} older shader entries replaced ({e["entries"]:,} files), {e["libraries"]:,} compiled libraries with them, {e["bytes"] / 1e9:.2f} GB given back'
                         + (f', {e["errors"]} left after an error' if e['errors'] else ''))
            with contextlib.suppress(sqlite3.Error): self.db.close()


def _digest_of(index_json):   # the digest an index/<key>.json entry names, None when it names none
    try: return json.loads(index_json).get('digest')
    except (ValueError, AttributeError, TypeError): return None


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
    for e, _ in s.pairs:
        for name, in s._run(f'SELECT name FROM {e}'): kinds[name.split('/', 1)[0]] = kinds.get(name.split('/', 1)[0], 0) + 1
    print(json.dumps({'entries': kinds, 'blobs': sum(s._run(f'SELECT count(*) FROM {b}')[0][0] for _, b in s.pairs),
                      'compressed': s._run('SELECT count(*) FROM zentry')[0][0] if ZSTD in s.pairs else 0, 'bytes': os.path.getsize(sys.argv[1])}))
    s.close()


if __name__ == '__main__': main()
