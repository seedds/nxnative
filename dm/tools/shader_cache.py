#!/usr/bin/env python3
# The shader build's compile cache as one file, cas.db, read and written in place. It was a folder of a few files per shader (Metroid Dread: 84,541 files), kept as a tar between builds and unpacked
# for each build that ran. Nothing is unpacked now: an entry goes in and comes out by its name, and a file a tool needs is written only then.
# SQLite, as it is in Python and takes several writers: dmsh_build.py's threads share one connection, and regress.py's chain builds and the NSP script can share a game's file at once (the wait is SQLite's).
# Two tables: entry (name, the blob it names, when it was written) and blob (the bytes, once each). A library kept under its MSL's digest and under its shader's digest was one APFS clone of the other; inside
# one file it would be two copies, so bytes are stored by their hash. A file removed from the cache gives its pages back at the next prune (incremental vacuum), with no copy of the file.
# It only saves work: a damaged file costs the work it held and is made anew, and a full drive is one [RED] line. An entry is written and read whole, so its file is never seen half written.
# Names, as in the folder this replaces: metallib/<target>/<digest>[_x].metallib (dmsh_mtlcache.py), shader/<digest>/<mac|ios>/<output>, index/<key>.json, variant/<digest>/<file> (dmsh_build.py).
# Not part of the translation chain: it decides no output (dmsh_chain.py), like dmsh_mtlcache.py.
import contextlib, hashlib, json, os, re, shutil, sqlite3, tarfile, threading, time

SCHEMA = ('CREATE TABLE IF NOT EXISTS blob(id INTEGER PRIMARY KEY, hash BLOB UNIQUE NOT NULL, data BLOB NOT NULL)',
          'CREATE TABLE IF NOT EXISTS entry(name TEXT PRIMARY KEY, blob INTEGER NOT NULL, mtime REAL NOT NULL) WITHOUT ROWID')
SHADER_ENTRY = re.compile(r'shader/([0-9a-f]{64})/')
WAIT = 600   # seconds another process's write may hold this one up
_DAMAGED = ('malformed', 'not a database', 'disk image')


class Store:
    def __init__(self, path, say=print):
        self.path, self.say = os.fspath(path), say
        self.lock = threading.RLock()   # one connection, for dmsh_build.py's threads: a read or a write of an entry takes microseconds next to a compile
        self.full = False
        self._open()

    def _open(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=WAIT, check_same_thread=False, isolation_level=None)
        try:
            self.db.execute('PRAGMA page_size=16384'); self.db.execute('PRAGMA auto_vacuum=INCREMENTAL')   # both only before the first table
            # WAL: a reader in another process does not wait for a writer, and a commit needs no fsync. A drive that cannot share its memory file stays on the rollback journal, unsynced: a stop costs the cache at worst
            if self.db.execute('PRAGMA journal_mode=WAL').fetchone()[0].lower() == 'wal': self.db.execute('PRAGMA synchronous=NORMAL')
            else: self.db.execute('PRAGMA synchronous=OFF')
            for s in SCHEMA: self.db.execute(s)
        except sqlite3.DatabaseError as e:
            if not self._damage(e): raise

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
                    blob = row[0] if row else self.db.execute('INSERT INTO blob(hash, data) VALUES(?, ?)', (digest, data)).lastrowid
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

    def close(self):   # also folds the write-ahead log into the file, so that one file is left
        with self.lock, contextlib.suppress(sqlite3.Error): self.db.close()


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
