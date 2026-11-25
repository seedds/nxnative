#!/usr/bin/env python3
"""Read NX archives through nxformat's C++17 reader and feed the existing NUP packing path.

The helper is compiled once per source revision in the system temporary directory.
Only the reader and Zstandard are linked; NXConvert and Rust are not needed.
"""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import pack_nup

Error = pack_nup.Error
NO_DIR = pack_nup.NO_DIR


def reader():
    root = Path(__file__).resolve().parents[1]
    sources = [root / 'dm/tools/nx_reader.cpp', *(root / 'dm/third_party/nxformat').glob('*.hpp')]
    digest = hashlib.sha256(b''.join(p.read_bytes() for p in sorted(sources))).hexdigest()[:20]
    cache = Path(tempfile.gettempdir()) / f'nxnative-nx-{os.getuid()}'
    cache.mkdir(mode=0o700, exist_ok=True)
    binary = cache / f'reader-{digest}'
    if binary.is_file(): return binary
    compiler = shutil.which('clang++') or shutil.which('c++')
    if not compiler: raise Error('reading NX needs a C++17 compiler (install Xcode command line tools)')
    flags = []
    brew = shutil.which('brew')
    if brew:
        prefix = subprocess.run([brew, '--prefix', 'zstd'], capture_output=True, text=True)
        if prefix.returncode == 0:
            flags = ['-I' + prefix.stdout.strip() + '/include', '-L' + prefix.stdout.strip() + '/lib']
    with tempfile.TemporaryDirectory(dir=cache) as tmp:
        output = Path(tmp) / 'reader'
        result = subprocess.run([compiler, '-std=c++17', '-O2', *flags, str(sources[0]), '-lzstd', '-o', str(output)], capture_output=True, text=True)
        if result.returncode: raise Error(f'building the NX reader failed (install Zstandard: brew install zstd):\n{result.stderr}')
        output.replace(binary)
    return binary


def is_nx(path):
    # Either independently committed header page can survive damage to the other.
    try:
        with open(path, 'rb') as f:
            first = f.read(4); f.seek(4096)
            return first == b'NXAR' or f.read(4) == b'NXAR'
    except OSError: return False


class Package:
    def __init__(self, owner, row):
        _, app, index, kind, flags, version, pid, addon, nacp, identity, self.content_id, icon, program_kind = row
        self.nup = owner
        self.app_index, self.index, self.kind, self.flags, self.version, self.program_id, self.title_id, self.nacp = map(int, (app, index, kind, flags, version, pid, addon, nacp))
        if self.kind == 3: self.kind = pack_nup.UPDATE
        self.program_kind = int(program_kind)
        self.icon = int(icon)
        self.identity = identity
        self.root = len(owner.dirs); owner.dirs.append([])
        self.exefs = self.romfs = NO_DIR
        self.deps = []
        self.native = None
        self.required_version = 0
        self.name = f'{owner.path.name} (NX snapshot {index}, version {version})'

    def meta(self, *, say=None):
        out = {}
        if self.nacp != NO_DIR: out[pack_nup.T_NACP] = self.nup.media(self.nacp)
        if self.icon != NO_DIR: out[pack_nup.T_ICON] = self.nup.media(self.icon)
        if self.native:
            control = self.native.control
            if b'control.nacp' in control: out.setdefault(pack_nup.T_NACP, self.nup.read(control[b'control.nacp']))
            icon = next((control[n] for n in (b'icon_AmericanEnglish.dat', b'icon_BritishEnglish.dat') if n in control), None)
            if icon is None: icon = next((i for name, i in sorted(control.items()) if name.startswith(b'icon_')), None)
            if icon is not None: out.setdefault(pack_nup.T_ICON, self.nup.read(icon))
        return out


class Nx:
    def __init__(self, path, want=None):
        self.path = Path(path)
        self.source_identity = self.identity_of_source()
        self.dirs, self.files, self.packages, self.apps = [], [], [], {}
        self.application, self.aoc_base, self.siblings = None, None, {}   # pack_nup.scan sets them
        self._closed = False
        self._native = []
        self._errors = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([str(reader()), str(self.path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._errors)
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.input, self.output = self.proc.stdin, self.proc.stdout
        try:
            by_index, paths, fingerprint = {}, {}, hashlib.sha256()
            while True:
                line = self.output.readline()
                if not line: self.failed()
                if line == b'END\n': break
                fingerprint.update(line)
                row = line.decode('ascii').split()
                if row[0] == 'A':
                    _, a, tid, platform, icon, metadata = row
                    self.apps[int(a)] = dict(id=int(tid), platform=int(platform), icon=int(icon), metadata=int(metadata))
                elif row[0] == 'S':
                    k = Package(self, row); by_index[k.index] = k; paths[k.index] = {'': k.root}; self.packages.append(k)
                elif row[0] == 'R': by_index[int(row[1])].deps.append((int(row[2]), row[3]))
                else:
                    k = by_index[int(row[1])]; path = bytes.fromhex(row[2]); parent, _, name = path.rpartition(b'/')
                    parent_dir = paths[k.index][parent.decode('utf-8')]
                    if row[0] == 'D':
                        index = len(self.dirs); self.dirs.append([]); paths[k.index][path.decode('utf-8')] = index
                        if not parent and name in (b'exefs', b'romfs'): setattr(k, name.decode(), index)
                        self.dirs[parent_dir].append((name, True, index))
                    else:
                        _, _, _, file_id, size, sha = row
                        index = len(self.files); self.files.append((name, int(size), bytes.fromhex(sha), int(file_id)))
                        self.dirs[parent_dir].append((name, False, index))
            self.check_source()
            fingerprint.update(Path(__file__).read_bytes())
            fingerprint.update(reader().name.encode('ascii'))
            native_sources = Path(__file__).resolve().parents[1] / 'dm/tools/nx_extract'
            for name in ('Program.cs', 'nx_extract.csproj', 'NxTree.cs'): fingerprint.update((native_sources / name).read_bytes())
            self.toc_sha = fingerprint.digest()
            candidates = [a for a, app in self.apps.items() if want is None or app['id'] == want]
            if len(candidates) != 1: raise Error(f'{self.path}: choose one application with --title-id; found {len(candidates)} matching applications')
            a = candidates[0]
            if self.apps[a]['platform'] not in (1, 2): raise Error(f'{self.path}: unknown NX platform')
            self.packages = [k for k in self.packages if k.app_index == a]
            programs = {k.program_id for k in self.packages if k.kind != pack_nup.ADDON}
            if len(programs) != 1:
                primary = {k.program_id for k in self.packages if k.kind != pack_nup.ADDON and k.program_kind == 0}
                if len(primary) != 1: raise Error(f'{self.path}: NX must identify one primary executable program')
                program = primary.pop()
                self.packages = [k for k in self.packages if k.kind == pack_nup.ADDON or k.program_id == program]
            for k in self.packages:
                if k.kind != pack_nup.ADDON and k.exefs == NO_DIR or k.kind == pack_nup.ADDON and not k.title_id:
                    required = [by_index[j] for kind, identity in k.deps if kind == 1
                                for j in by_index if by_index[j].identity == identity and by_index[j].app_index == a]
                    if any(kind == 1 for kind, _ in k.deps) and len(required) != 1:
                        raise Error(f'{k.name}: missing or ambiguous exact native base dependency')
                    base_index = required[0].index if required else -1
                    k.native = NativeTree(self, k, base_index)
                    self._native.append(k.native)
                if k.kind == pack_nup.UPDATE: k.flags = pack_nup.STANDALONE
            # Keep every DLC candidate for version-dependent selection, then give
            # the common scanner one eligible version per add-on title ID.
            self.addons = [k for k in self.packages if k.kind == pack_nup.ADDON]
            self.by_identity = {k.identity: k for k in self.packages}
            latest = max((k.version for k in self.packages if k.kind != pack_nup.ADDON), default=0)
            self.packages = [k for k in self.packages if k.kind != pack_nup.ADDON] + list(self.dlc_for_version(latest).values())
            app, _, _, _ = pack_nup.scan(self)
            if app != self.apps[a]['id']: raise Error(f'{self.path}: NPDM and NX application IDs disagree')
        except BaseException:
            self.close(); raise

    def identity_of_source(self):
        info = self.path.stat()
        return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns

    def check_source(self):
        if self.identity_of_source() != self.source_identity: raise Error(f'{self.path}: NX source changed while opening')

    def dlc_for_version(self, version):
        out = {}
        for k in self.addons:
            eligible = k.required_version <= version
            for kind, identity in k.deps:
                if kind not in (1, 2): continue
                target = self.by_identity.get(identity)
                if target is None: raise Error(f'{k.name}: missing required dependency {identity}')
                eligible = eligible and target.version <= version
            if eligible and (k.title_id not in out or k.version > out[k.title_id].version): out[k.title_id] = k
        return out

    def failed(self):
        self.proc.wait(); self._errors.seek(0)
        raise Error(self._errors.read().decode(errors='replace').strip() or f'{self.path}: NX reader stopped')

    def request(self, command):
        try:
            self.input.write(command.encode('ascii') + b'\n'); self.input.flush()
            line = self.output.readline()
            if not line: self.failed()
            size = int(line); data = self.output.read(size)
            if len(data) != size: self.failed()
            return data
        except (BrokenPipeError, ValueError): self.failed()

    def media(self, index): return self.request(f'M {index}')
    def children(self, d): return sorted(e for e in self.dirs[d] if e[0] not in pack_nup.ARTIFACTS)
    def child(self, d, name): return next((i for n, isdir, i in self.children(d) if n == name and not isdir), None)
    def tree_bytes(self, d): return sum(self.tree_bytes(i) if isdir else self.files[i][1] for _, isdir, i in self.children(d))
    def head(self, i, n=32):
        _, size, _, fid = self.files[i]
        return self.file_range(fid, 0, min(n, size))
    def copy(self, i, out):
        name, size, sha, fid = self.files[i]; digest = hashlib.sha256()
        for offset in range(0, size, 4 << 20):
            data = self.file_range(fid, offset, min(4 << 20, size-offset))
            digest.update(data); out.write(data)
        if sha is not None and digest.digest() != sha: raise Error(f'{self.path}: {name!r}: SHA-256 mismatch')
        return size
    def file_range(self, fid, offset, size):
        if isinstance(fid, tuple):
            tree, index = fid
            return tree.request(f'F {index} {offset} {size}')
        return self.request(f'F {fid} {offset} {size}')

    def read(self, i):
        out = io.BytesIO(); self.copy(i, out); return out.getvalue()
    def close(self):
        if self._closed: return
        self._closed = True
        for tree in self._native: tree.close()
        self.input.close(); self.output.close()
        if self.proc.poll() is None: self.proc.terminate()
        self.proc.wait(); self._errors.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()


class NativeTree:
    # The existing native executable opens the NCA files through a second pinned
    # NX reader, and serves the same immutable file interface as a materialized tree.
    request = Nx.request
    failed = Nx.failed

    def __init__(self, owner, package, base_index):
        root = Path(__file__).resolve().parents[1]
        native = root / 'dm/tools/nx_extract_bin/nx_extract'
        sources = root / 'dm/tools/nx_extract'
        expected = hashlib.sha256(b''.join((sources / name).read_bytes() for name in ('Program.cs', 'nx_extract.csproj', 'NxTree.cs'))).hexdigest()[:16]
        result = subprocess.run([str(native), '--source-sha'], capture_output=True, text=True, timeout=60)
        if result.returncode or result.stdout.strip() != expected: raise Error('NX native reader does not match its sources: rebuild with dm/tools/native_tools.sh nx_extract')
        owner.check_source()
        self.path = owner.path
        self._errors = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([str(native), '--nx-tree', str(reader()), str(owner.path), str(package.index), str(base_index), f'{package.program_id:016X}'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._errors)
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.input, self.output = self.proc.stdin, self.proc.stdout
        self.control = {}
        owner.dirs[package.root] = []
        paths, code = {b'': package.root}, []
        try:
            while True:
                line = self.output.readline()
                if not line: self.failed()
                if line == b'END\n': break
                row = line.decode('ascii').split()
                if row[0] not in ('T', 'D', 'F'): self.failed()
                if row[0] == 'T':
                    package.title_id, package.required_version = map(int, row[1:]); continue
                path = bytes.fromhex(row[1]); parent, _, name = path.rpartition(b'/')
                if row[0] == 'D':
                    index = len(owner.dirs); owner.dirs.append([]); paths[path] = index
                    owner.dirs[paths[parent]].append((name, True, index))
                    if path in (b'exefs', b'romfs'): setattr(package, path.decode(), index)
                else:
                    _, _, fid, size = row
                    index = len(owner.files); owner.files.append((name, int(size), None, (self, int(fid))))
                    owner.dirs[paths[parent]].append((name, False, index))
                    if parent == b'control': self.control[name] = index
                    if parent == b'exefs': code.append(index)
            owner.check_source()
            # Code modules are small and their digests preserve ExeFS reuse.
            # RomFS reads rely on LibHac's native hash trees and NX block checks.
            for i in code:
                name, size, _, fid = owner.files[i]
                owner.files[i] = (name, size, hashlib.sha256(owner.read(i)).digest(), fid)
        except BaseException:
            self.close(); raise

    def close(self):
        self.input.close(); self.output.close()
        if self.proc.poll() is None: self.proc.terminate()
        self.proc.wait(); self._errors.close()
