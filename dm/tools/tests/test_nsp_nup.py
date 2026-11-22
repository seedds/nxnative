"""NSP script with a NUP container (scripts/pack_nup.py): the container is read and checked, its versions and DLC packed into what the script's later steps read, its control data saved,
and the script asks for no keys. The NUPs are written here (SPEC v1, https://github.com/a55uka/nup). The run stops at the prerequisite check; no game files, keys or Xcode needed."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import nacp
import romfs_image
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
pack_nup = nsp.pack_nup
zstd = pack_nup.zstd

TID = 0x040074A01BF12000   # a game with a profile in dm/games, whose registered version is an update
VER = 458752


def npdm(tid):
    d = bytearray(0x100)
    d[:4] = b'META'
    struct.pack_into('<II', d, 0x70, 0x80, 0x20)
    d[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', d, 0x90, tid)
    return bytes(d)


def tlv(recs):
    return b''.join(struct.pack('<HHI', t, 0, len(v)) + v for t, v in recs)


def write_nup(path, packages, *, update_flags=0):
    """packages: [(kind, title ID, version, {'exefs': {name: bytes}, 'romfs': {path: bytes}}, meta bytes)] → a NUP file, every file of 200 bytes or more zstd-compressed"""
    dirs, files, slots, pool = [], [], [], bytearray()
    def walk(name, tree):   # tree: {name: bytes or dict}; dirs pre-order, children contiguous and sorted
        d = len(dirs); dirs.append([name, 0, 0])
        kids = sorted(tree.items(), key=lambda kv: kv[0].encode())
        first = len(slots); slots.extend([None] * len(kids)); dirs[d][1:] = [first, len(kids)]
        for j, (n, v) in enumerate(kids):
            if isinstance(v, bytes): slots[first + j] = ('f', len(files)); files.append((n, v))
        for j, (n, v) in enumerate(kids):
            if isinstance(v, dict): slots[first + j] = ('d', walk(n, v))
        return d
    def nest(flat):
        t = {}
        for p, v in flat.items():
            *ds, f = p.split('/'); c = t
            for x in ds: c = c.setdefault(x, {})
            c[f] = v
        return t
    recs = []
    for kind, tid, ver, tree, meta in packages:
        t = {k: v if k == 'exefs' else nest(v) for k, v in tree.items()}
        root = walk('', t)
        sub = {n: s[1] for n, s in zip(sorted(t), slots[dirs[root][1]:dirs[root][1] + dirs[root][2]])}
        recs.append([tid, root, sub.get('exefs', 0xFFFFFFFF), sub.get('romfs', 0xFFFFFFFF), ver, kind, meta])
    def name(s):
        b = s.encode(); off = len(pool); pool.extend(b); return off, len(b)
    toc_len = 0x30 + len(recs) * 0x40 + len(dirs) * 0x18 + len(files) * 0x50 + len(slots) * 4
    pool_off = toc_len
    names_d = [name(d[0]) for d in dirs]; names_f = [name(f[0]) for f in files]
    toc_size = (pool_off + len(pool) + 15) // 16 * 16
    pos = 0x80 + toc_size
    metas = []
    for r in recs:
        metas.append((pos, r[6])); pos = (pos + len(r[6]) + 15) // 16 * 16
    blob, blobs = pos, []
    ext = []
    for n, data in files:
        stored, codec = (zstd.compress(data), 1) if len(data) >= 200 else (data, 0)
        if not data: ext.append((0, 0, 0)); continue
        ext.append((pos, len(stored), codec)); blobs.append((pos, stored)); pos = (pos + len(stored) + 15) // 16 * 16
    toc = bytearray(struct.pack('<4sIQQQQQ', b'TOC\x01', len(recs), len(dirs), len(files), pool_off, len(pool), 0))
    for r, (moff, m) in zip(recs, metas):
        toc += struct.pack('<QIIIIBBHIQI20x', r[0], r[1], r[2], r[3], r[4], r[5], update_flags if r[5] == 1 else 0, 0, 0, moff if m else 0, len(m))
    for (o, ln), d in zip(names_d, dirs): toc += struct.pack('<IIIIQ', o, ln, d[1], d[2], 0)
    for (o, ln), (n, data), (eo, es, c) in zip(names_f, files, ext): toc += struct.pack('<IIQQQ32sBBH12x', o, ln, len(data), eo, es, hashlib.sha256(data).digest(), c, 0, 0)
    for k, i in slots: toc += struct.pack('<I', i if k == 'd' else len(dirs) + i)
    toc += pool; toc += b'\0' * (toc_size - len(toc))
    body = bytearray(pos)
    body[0x80:0x80 + toc_size] = toc
    for moff, m in metas: body[moff:moff + len(m)] = m
    for o, b in blobs: body[o:o + len(b)] = b
    uuid = hashlib.sha256(b''.join(struct.pack('<QIB', r[0], r[4], r[5]) for r in recs)).digest()[:16]
    body[:0x80] = struct.pack('<4sHHQQ32sQQII16s32s', b'NUP\x01', 0x80, 1, 0x80, toc_size, hashlib.sha256(toc).digest(), blob, pos, len(recs), 0, uuid, b'\0' * 32)
    Path(path).write_bytes(body)


def version(main=b'code', marker='marker', tid=TID):
    return {'exefs': {'main': main, 'main.npdm': npdm(tid), '.listing': b'not a module'}, 'romfs': {f'dir/{marker}': b'data' * 100, 'empty': b''}}


def control_nacp(name, jp, display):   # a console NACP's title entries (0x300 per language, Japanese the third) and display version
    b = bytearray(0x4000)
    b[:len(name)] = name.encode(); b[2 * 0x300:2 * 0x300 + len(jp.encode())] = jp.encode(); b[0x3060:0x3060 + len(display)] = display.encode()
    return bytes(b)


T_NAME = 5   # the packer's name label (SPEC 0x0005), which is not the game's name
META = tlv([(T_NAME, b'TG'), (pack_nup.T_NACP, control_nacp('Test Game', 'テス', '1.2.3')), (pack_nup.T_ICON, b'\xff\xd8 icon')])


@unittest.skipIf(zstd is None, 'no zstd module')
class Base(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.dir = Path(td.name).resolve() / 'game'; self.dir.mkdir()
        self.path = self.dir / 'game.nup'
        write_nup(self.path, [(0, TID, 0, version(marker='old'), META), (1, TID, VER, version(main=b'new code', marker='new'), b''),
                              (2, TID + 0x1001, 0, {'romfs': {'dlcfile': b'dlc'}}, b'')])

    def open(self):
        n = pack_nup.Nup(self.path); self.addCleanup(n.close); return n


class ExeFSNames(unittest.TestCase):
    def test_rejects_names_that_are_not_path_components(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td).resolve()
            target = root / 'sentinel'
            target.write_bytes(b'original')
            archive = root / 'game.nup'
            dst = root / 'build' / 'ExeFS'
            for name in (str(target), '../sentinel', 'nested/file', '', '.', '..', 'bad\0name'):
                with self.subTest(name=name):
                    tree = {'exefs': {name: b'overwritten'}}
                    write_nup(archive, [(0, TID, 0, tree, b'')])
                    try:
                        with pack_nup.Nup(archive) as n:
                            with self.assertRaisesRegex(pack_nup.Error, 'invalid ExeFS name'):
                                pack_nup.exefs(n, n.packages[0], dst)
                    finally:
                        self.assertEqual(target.read_bytes(), b'original')
                    self.assertFalse(dst.with_name('ExeFS.partial').exists())


class Read(Base):
    def test_scan(self):
        n = self.open()
        app, base, vers, dlc = pack_nup.scan(n)
        self.assertEqual((app, base.kind, sorted(vers), sorted(dlc)), (TID, 0, [VER], [TID + 0x1001]))

    def test_damaged_file_refused(self):
        b = bytearray(self.path.read_bytes()); b[0x90] ^= 1; self.path.write_bytes(b)   # a TOC byte
        with self.assertRaisesRegex(pack_nup.Error, 'SHA-256'): pack_nup.Nup(self.path)
        self.path.write_bytes(b[:-16])
        with self.assertRaisesRegex(pack_nup.Error, 'header says'): pack_nup.Nup(self.path)

    def test_folder_cycle_refused(self):   # a TOC whose hash matches but whose folder names an ancestor as a child: the tree walks would recurse for ever
        n = self.open(); ndirs, slots, dirs = n.ndirs, list(n.slots), list(n.dirs)
        slots_at = 0x80 + 0x30 + len(n.packages) * 0x40 + ndirs * 0x18 + len(n.files) * 0x50; n.close()
        orig = self.path.read_bytes(); toc_size = struct.unpack_from('<Q', orig, 0x10)[0]
        def folders(d): _, first, count = dirs[d]; return [first + j for j in range(count) if slots[first + j] < ndirs]   # the slots of d's folder children
        j = next(j for j in folders(0) if folders(slots[j])); sub = slots[j]   # the base's romfs/, which has a folder
        for at, to in ((j, 0), (folders(sub)[0], sub)):   # romfs/ made the root itself; then romfs/dir made romfs/ (a cycle of two)
            b = bytearray(orig); struct.pack_into('<I', b, slots_at + 4 * at, to)
            b[0x18:0x38] = hashlib.sha256(bytes(b[0x80:0x80 + toc_size])).digest(); self.path.write_bytes(b)
            with self.assertRaisesRegex(pack_nup.Error, 'contains itself'): pack_nup.Nup(self.path)
        r = subprocess.run([sys.executable, str(REPO / 'scripts/pack_nup.py'), str(self.path)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1); self.assertIn('contains itself', r.stderr); self.assertNotIn('Traceback', r.stderr)

    def test_damaged_content_refused(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        i = next(i for i, f in enumerate(n.files) if f[5] == 1)
        b = bytearray(self.path.read_bytes()); b[n.files[i][3] + n.files[i][4] - 1] ^= 0xFF; n.close(); self.path.write_bytes(b)
        n = self.open()
        with self.assertRaises(pack_nup.Error): n.read(i)

    def test_no_base_refused(self):
        write_nup(self.path, [(1, TID, VER, version(), b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'no base game'): pack_nup.scan(self.open())

    def test_addon_alone_is_not_a_standalone_program(self):
        write_nup(self.path, [(2, TID + 0x1001, 0, {'romfs': {'dlc': b'dlc'}}, b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'no base game'): pack_nup.scan(self.open())

    def test_standalone_updates_keep_identity_and_version_checks(self):
        for packages, error in (([(1, TID, 0, version(), b'')], 'update of version 0'),
                                ([(1, TID, VER, version(), b'')] * 2, 'two updates of version'),
                                ([(1, TID, VER, version(), b''), (1, TID + 0x2000, VER + 1, version(tid=TID + 0x2000), b'')], 'different applications')):
            with self.subTest(error=error):
                write_nup(self.path, packages, update_flags=pack_nup.STANDALONE)
                with pack_nup.Nup(self.path) as n, self.assertRaisesRegex(pack_nup.Error, error): pack_nup.scan(n)

    def test_folder_or_file(self):
        self.assertEqual(nsp.nup_file(self.dir), self.path)
        self.assertEqual(nsp.nup_file(self.path), self.path)
        (self.dir / 'game.nsp').write_bytes(b'x')
        self.assertIsNone(nsp.nup_file(self.dir))   # the packages win, as over a dump's layout


class Head(Base):
    def test_first_bytes_of_stored_and_compressed_files(self):   # the survey's manifest: a file of 200 bytes or more is zstd-compressed here, a shorter one stored
        n = self.open(); base = next(k for k in n.packages if k.kind == pack_nup.BASE)
        romfs = {name: i for name, _, i in n.children(base.romfs)}
        big = n.child(romfs[b'dir'], b'old')   # dir/old: 400 bytes
        self.assertEqual(n.files[big][5], 1)
        self.assertEqual(n.head(big), (b'data' * 100)[:32])
        self.assertEqual(n.head(big, 1000), b'data' * 100)   # never more than the file
        self.assertEqual(n.head(romfs[b'empty']), b'')
        write_nup(self.path, [(0, TID, 0, {'exefs': version()['exefs'], 'romfs': {'short': b'abc'}}, b'')])
        n = self.open(); k = n.packages[0]
        self.assertEqual(n.head(n.child(k.romfs, b'short')), b'abc')


NEW = 0x0400000000123000   # a title no profile has


class Survey(Base):
    def test_files_as_the_manifest_pass_leaves_them(self):   # ExeFS, romfs_manifest.tsv (path, size, first 32 bytes), the NROs whole, the update's control data, else the base game's
        nro = b'\0' * 16 + b'NRO0' + b'x' * 300
        write_nup(self.path, [(0, NEW, 0, version(tid=NEW), META),
                              (1, NEW, VER, {'exefs': version(main=b'new', tid=NEW)['exefs'], 'romfs': {'a/big': b'z' * 500, 'nro/m.nro': nro, 'empty': b''}}, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        out = self.dir / 'survey'
        self.assertEqual(nsp.survey_nup(n, vers[VER], base, out), 3)
        self.assertEqual((out / 'ExeFS/main').read_bytes(), b'new')
        self.assertFalse((out / 'ExeFS/.listing').exists())
        rows = (out / 'romfs_manifest.tsv').read_text().splitlines()
        self.assertEqual(rows[0], nsp.MANIFEST_HEAD.rstrip('\n'))
        self.assertEqual(sorted(rows[1:]), sorted([f'/a/big\t500\t{(b"z" * 32).hex()}', '/empty\t0\t', f'/nro/m.nro\t{len(nro)}\t{nro[:32].hex()}']))
        self.assertEqual((out / 'romfs_nro/nro/m.nro').read_bytes(), nro)
        self.assertEqual(nacp.name(out / 'control.nacp'), 'Test Game')   # the update has none: the base game's

    def test_a_nup_without_a_profile_is_surveyed(self):   # it used to stop: the survey that makes a provisional profile read NSPs only
        write_nup(self.path, [(1, NEW, VER, version(tid=NEW), META)], update_flags=pack_nup.STANDALONE)
        seen = {}
        def survey(nsp_dir, app, fam, keys, repo, py, env):
            seen.update(app=app, nup=fam['nup'], base=fam['base']); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'survey', side_effect=survey), mock.patch.object(nsp, 'check_capstone'), \
             mock.patch.object(nsp, 'say'), mock.patch.object(nsp, 'find_nx_extract', side_effect=AssertionError('built nx_extract')), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), mock.patch.object(nsp, 'package_reader', side_effect=AssertionError('read a package')):
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.path), '--survey-only']))
        self.assertEqual(seen['app'], NEW)
        self.assertIsNone(seen['base'])

    def test_same_build_reads_main_from_the_container(self):   # a known build under another title ID: the newest update's main, then the base game's, read from the packages
        def nso(bid): return b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50)
        write_nup(self.path, [(0, NEW, 0, version(main=nso('ab' * 16), tid=NEW), META), (1, NEW, VER, version(main=nso('cd' * 16), tid=NEW), b'')])
        app, fam = nsp.scan_nup(self.path); self.addCleanup(fam['nup'].close)
        seen, prof = [], mock.Mock(id='P', builds=lambda: {'v0': {'main': 'ab' * 16}})
        games = mock.Mock(by_build=lambda bid, skip=(): seen.append(bid) or (prof if bid == 'ab' * 16 else None))
        with mock.patch.dict(sys.modules, {'game': games}), mock.patch.object(nsp, 'PROG', mock.Mock()), \
             mock.patch.object(nsp, 'find_nx_extract', side_effect=AssertionError('built nx_extract')):
            self.assertEqual(nsp.same_build(self.dir, app, fam, None, REPO, {}), (prof, 'v0', 'ab' * 16))
        self.assertEqual(seen, ['cd' * 16, 'ab' * 16])


class ImageReuse(Base):
    def cached(self):
        n = self.open()
        _, base, versions, _ = pack_nup.scan(n)
        directory = pack_nup.romfs_of(versions[VER], base)
        img = self.dir / 'cached.img'
        pack_nup.pack(n, directory, img, say=lambda s: None)
        return n, directory, img

    def invalidate(self, img):
        stamp = Path(str(img) + '.inputs.json')
        row = json.loads(stamp.read_text())
        row['source'] = 'previous-packer'
        row['image'] = [999] + row['image']
        stamp.write_text(json.dumps(row))

    def test_old_five_field_stamp_is_verified_and_migrated_without_unpacking(self):
        n, d, img = self.cached()
        self.invalidate(img)
        before = romfs_image.file_identity(img)
        with mock.patch.object(n, 'copy', side_effect=AssertionError('unpacked')), \
             mock.patch.object(romfs_image, 'write', side_effect=AssertionError('rewrote image')):
            self.assertEqual(pack_nup.pack(n, d, img, say=lambda s: None), 'verified')
        self.assertEqual(romfs_image.file_identity(img), before)
        row = json.loads(Path(str(img) + '.inputs.json').read_text())
        self.assertEqual(row['image'], before)
        with mock.patch.object(pack_nup, 'verified', side_effect=AssertionError('verified twice')):
            self.assertEqual(pack_nup.pack(n, d, img, say=lambda s: None), 'kept')

    def test_image_of_an_older_layout_is_packed_again(self):   # the check compares romfs_image.layout's bytes, so a format change still repacks
        n, d, img = self.cached()
        self.invalidate(img)
        layout = romfs_image.layout
        def newer(ents, end):
            head, idx = layout(ents, end)
            return head, idx + b'\0'
        with mock.patch.object(romfs_image, 'layout', newer):
            self.assertIsNone(pack_nup.verified(n, pack_nup.entries(n, d), str(img)))
            self.assertEqual(pack_nup.pack(n, d, img, say=lambda s: None), 'packed')

    def test_ctime_only_change_reuses_after_payload_verification(self):
        n, d, img = self.cached()
        before = img.read_bytes()
        os.chmod(img, 0o600)
        with mock.patch.object(n, 'copy', side_effect=AssertionError('unpacked')):
            pack_nup.pack(n, d, img, say=lambda s: None)
        self.assertEqual(img.read_bytes(), before)

    def test_bad_payload_rejected_even_if_size_and_mtime_match(self):
        n, d, img = self.cached()
        good = img.read_bytes()
        st = img.stat()
        with img.open('r+b') as f:
            f.seek(romfs_image.HEAD.size)
            b = f.read(1)
            f.seek(romfs_image.HEAD.size)
            f.write(bytes([b[0] ^ 1]))
        os.utime(img, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.invalidate(img)
        self.assertIsNone(pack_nup.verified(n, pack_nup.entries(n, d), str(img)))
        with mock.patch.object(n, 'copy', wraps=n.copy) as copy:
            self.assertEqual(pack_nup.pack(n, d, img, say=lambda s: None), 'packed')
        self.assertGreater(copy.call_count, 0)
        self.assertEqual(img.read_bytes(), good)

    def test_wrong_index_or_truncated_image_is_not_reused(self):
        n, d, img = self.cached()
        good = img.read_bytes()
        for data in [good[:-1], good[:-1] + bytes([good[-1] ^ 1])]:
            with self.subTest(size=len(data)):
                img.write_bytes(data)
                self.assertIsNone(pack_nup.verified(n, pack_nup.entries(n, d), str(img)))

    def test_image_changed_during_verification_is_rejected(self):
        n, d, img = self.cached()
        before = romfs_image.file_identity(img)
        after = before.copy()
        after[-1] += 1
        with mock.patch.object(romfs_image, 'file_identity', side_effect=[before, after]):
            self.assertIsNone(pack_nup.verified(n, pack_nup.entries(n, d), str(img)))

    def test_extract_stage_reports_verified_reuse(self):
        n = self.open()
        app, base, versions, _ = pack_nup.scan(n)
        ex = self.dir / 'extract'
        version = 'v' + str(VER)
        dst = ex / version
        pack_nup.pack_version(n, versions[VER], base, app, dst, say=lambda s: None)
        self.invalidate(dst / 'RomFS.img')
        with mock.patch.object(nsp, 'PROG') as progress, \
             mock.patch.object(romfs_image, 'write', side_effect=AssertionError('rewrote image')):
            self.assertEqual(nsp.extract_nup(ex, version, versions[VER], base, app, n), dst)
        self.assertIn('reused (image checked)', progress.end.call_args.args[0])


class Pack(Base):
    def test_display_label_is_numeric_and_belongs_to_the_package(self):
        labels = [(b'1.1.1', '1.1.1'), (b'v1.8.0', '1.8.0'), (b'2.1', '2.1'), (b'4', '4'),
                  (b'', ''), (b'1.8.0 beta', ''), (b'1.2.3.4', ''), ('١.٢.٣'.encode(), ''),
                  (b'1.2\0', ''), (b'1' * 16, ''), (f'v{VER + 10}'.encode(), '')]
        packages = [(0, TID, 0, version(), tlv([(pack_nup.T_DISPLAY, b'9.9.9')]))]
        packages += [(1, TID, VER + i, version(), tlv([(pack_nup.T_DISPLAY, raw)]) if raw else b'')
                     for i, (raw, _) in enumerate(labels)]
        write_nup(self.path, packages)
        _, _, updates, _ = pack_nup.scan(self.open())
        for i, (raw, expected) in enumerate(labels):
            with self.subTest(raw=raw):
                self.assertEqual(pack_nup.display_label(updates[VER + i]), expected)

    def out(self): return self.dir / 'out'

    def test_standalone_updates_keep_their_versions_and_dlc(self):
        write_nup(self.path, [(1, TID, VER, version(marker='older'), META),
                              (1, TID, VER + 1, version(main=b'newest', marker='newest'), tlv([(pack_nup.T_ICON, b'new icon')])),
                              (2, TID + 0x1001, 0, {'romfs': {'dlc': b'dlc'}}, b'')], update_flags=pack_nup.STANDALONE)
        n = self.open(); app, base, vers, dlc = pack_nup.scan(n)
        self.assertIsNone(base)
        self.assertEqual((app, sorted(vers), sorted(dlc)), (TID, [VER, VER + 1], [TID + 0x1001]))
        app, fam = nsp.scan_nup(self.path); self.addCleanup(fam['nup'].close)
        have = nsp.nsp_versions(fam)
        self.assertEqual(set(have), {f'v{VER}', f'v{VER + 1}'})
        with mock.patch.object(nsp, 'say'):
            picks = nsp.pick_nsps(fam, have, sorted(have))
        self.assertTrue(all(b is None and u.version == int(v[1:]) for v, (b, u) in picks.items()))
        for ver in (VER, VER + 1):
            out = self.out() / f'v{ver}'
            pack_nup.pack_version(n, vers[ver], base, app, out, say=lambda s: None)
            self.assertEqual((out / 'ExeFS/main').read_bytes(), b'code' if ver == VER else b'newest')
            self.assertIn('dir/older' if ver == VER else 'dir/newest', romfs_image.read_index(out / 'RomFS.img'))
        self.assertEqual(nacp.name(self.out() / f'v{VER}/control.nacp'), 'Test Game')
        self.assertEqual(nacp.name(self.out() / f'v{VER + 1}/control.nacp'), 'Test Game')
        self.assertEqual((self.out() / f'v{VER + 1}/icon.jpg').read_bytes(), b'new icon')
        pack_nup.pack_dlc(n, app, dlc, self.out() / 'aoc', say=lambda s: None)
        self.assertIn('dlc', romfs_image.read_index(self.out() / 'aoc/1.img'))

    def test_standalone_update_needs_its_own_romfs(self):
        write_nup(self.path, [(1, TID, VER, version(), b''),
                              (1, TID, VER + 1, {'exefs': version()['exefs']}, b'')], update_flags=pack_nup.STANDALONE)
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no romfs/'):
            pack_nup.pack_version(n, vers[VER + 1], base, app, self.out())
        with self.assertRaisesRegex(nsp.BuildError, 'no romfs/'):
            nsp.extract_nup(self.out(), f'v{VER + 1}', vers[VER + 1], base, app, n)
        self.assertFalse(self.out().exists())

    def test_standalone_update_needs_main(self):
        tree = version(); del tree['exefs']['main']
        write_nup(self.path, [(1, TID, VER, tree, b'')], update_flags=pack_nup.STANDALONE)
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no exefs/main'):
            pack_nup.pack_version(n, vers[VER], base, app, self.out())
        self.assertFalse(self.out().exists())

    def test_empty_base_is_no_base(self):
        for tree in ({'exefs': {}}, {}, {'exefs': {}, 'romfs': {}}, {'exefs': {'.listing': b'not a file of the game'}}):
            with self.subTest(tree=tree):
                write_nup(self.path, [(0, 0, 0, tree, META), (1, TID, VER, version(), b'')])   # the update's flag is clear, as the older packer left it
                with pack_nup.Nup(self.path) as n:
                    app, base, vers, _ = pack_nup.scan(n)
                    self.assertIsNone(base)
                    self.assertEqual(app, TID)
                    self.assertEqual(set(nsp.nsp_versions({'base': base, 'updates': sorted(vers.items())})), {f'v{VER}'})
                    pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
                    self.assertIn('dir/marker', romfs_image.read_index(self.out() / 'RomFS.img'))
                    self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'code')
                    self.assertEqual(nacp.name(self.out() / 'control.nacp'), 'Test Game')   # application metadata stays on the base record (SPEC §5.1)

    def test_update_metadata_wins_over_the_empty_bases(self):
        write_nup(self.path, [(0, 0, 0, {}, tlv([(pack_nup.T_ICON, b'old icon')])), (1, TID, VER, version(), tlv([(pack_nup.T_ICON, b'new icon')]))])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertEqual((self.out() / 'icon.jpg').read_bytes(), b'new icon')

    def test_base_with_files_stays_a_base(self):
        for tree in ({'romfs': {'shared': b'data'}}, {'exefs': version()['exefs']}):
            with self.subTest(tree=tree):
                write_nup(self.path, [(0, TID, 0, tree, b''), (1, TID, VER, version(), b'')])
                with pack_nup.Nup(self.path) as n: self.assertIsNotNone(pack_nup.scan(n)[1])

    def test_selected_version_needs_romfs(self):
        tree = {'exefs': version()['exefs']}
        write_nup(self.path, [(0, TID, 0, tree, b''), (1, TID, VER, tree, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        with self.assertRaisesRegex(pack_nup.Error, 'no romfs/'):
            pack_nup.pack_version(n, vers[VER], base, app, self.out())
        self.assertFalse(self.out().exists())

    def test_update_without_romfs_uses_base(self):
        write_nup(self.path, [(0, TID, 0, version(marker='base'), b''),
                              (1, TID, VER, {'exefs': version(main=b'updated')['exefs']}, b'')])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertIn('dir/base', romfs_image.read_index(self.out() / 'RomFS.img'))
        self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'updated')

    def test_update_without_an_icon_takes_its_base_games(self):   # a build may pack only the update: Ocarina of Time's NUP has the icon on its base record, and the Mac app had none
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        u = self.out() / 'u'; pack_nup.pack_version(n, vers[VER], base, app, u, say=lambda s: None)
        self.assertEqual((u / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')
        self.assertFalse((u / 'control.nacp').exists())   # the NACP stays the package's own: its display version and languages are that version's
        self.assertEqual(nsp.app_icon(None, None, u, u), (u / 'icon.jpg', False))   # what the script reads when no folder holds the base game alone
        licons = lambda d: b''.join(bytes([len(t)]) + t.encode() + struct.pack('<I', len(b)) + b for t, b in d.items())   # the localized icons record (SPEC 0x000A)
        for base_meta, own, want in ((META, tlv([(pack_nup.T_ICON, b'new icon')]), b'new icon'),                                                  # the update's own icon stays
                                     (tlv([(pack_nup.T_LICONS, licons({'ja': b'ja icon', 'en-US': b'en icon'}))]), b'', b'en icon'),                # the base game's localized icons
                                     (b'', b'', None)):                                                                                             # none anywhere: none written, an earlier one goes
            write_nup(self.path, [(0, TID, 0, version(), base_meta), (1, TID, VER, version(), own)])
            n = self.open(); app, base, vers, _ = pack_nup.scan(n)
            pack_nup.pack_version(n, vers[VER], base, app, u, say=lambda s: None)
            self.assertEqual((u / 'icon.jpg').read_bytes() if (u / 'icon.jpg').exists() else None, want)

    def test_mismatched_program_ids_refused(self):
        write_nup(self.path, [(0, TID, 0, version(), b''), (1, TID, VER, version(tid=TID + 0x2000), b'')])
        with self.assertRaisesRegex(pack_nup.Error, 'different applications'): pack_nup.scan(self.open())

    def test_update_packed_on_its_own_with_control_data(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out() / 'u', say=lambda s: None)
        u = self.out() / 'u'
        self.assertEqual((u / 'ExeFS/main').read_bytes(), b'new code')
        self.assertFalse((u / 'ExeFS/.listing').exists())
        names = romfs_image.read_index(u / 'RomFS.img')
        self.assertIn('dir/new', names); self.assertNotIn('dir/old', names); self.assertEqual(names['empty'], (False, names['empty'][1], 0))
        self.assertFalse((u / 'control.nacp').exists())   # the update carries no control data: the script takes the base's
        pack_nup.pack_version(n, base, base, app, self.out() / 'b', say=lambda s: None)
        b = self.out() / 'b'
        self.assertEqual((nacp.name(b / 'control.nacp'), nacp.version(b / 'control.nacp')), ('Test Game', '1.2.3'))
        self.assertEqual((b / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')
        jp = (b / 'control.nacp').read_bytes()[2 * 0x300:2 * 0x300 + 0x200].split(b'\0')[0]
        self.assertEqual(jp.decode(), 'テス')

    def test_settings_read_the_base_games_languages_when_the_update_has_no_control_data(self):   # a Switch 2 game's settings had no Game language row (LNPANDY/NX2Apple#619)
        sys.path.insert(0, str(REPO / 'dm/games')); import settings
        vars(nsp)['nacp'] = nacp   # main sets it
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out() / 'u', say=lambda s: None)
        pack_nup.pack_version(n, base, base, app, self.out() / 'b', say=lambda s: None)
        u, b = self.out() / 'u', self.out() / 'b'
        nac = bytearray((b / 'control.nacp').read_bytes()); struct.pack_into('<I', nac, 0x302C, 0b101)   # American English and Japanese
        (b / 'control.nacp').write_bytes(nac)
        self.assertFalse((u / 'control.nacp').exists())
        control = nsp.languages_control(u, b)
        self.assertEqual(nacp.languages(control), ['en-US', 'ja'])
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'main.fixups.tsv').write_text('')
            row = lambda path: [s for s in settings.build(f'{TID:016X}', tmp, str(path))['settings'] if s['env'] == 'DM_LANG']
            self.assertEqual(row(control)[0]['options'], [['en-US', 'American English'], ['ja', 'Japanese']])
            self.assertEqual(row(control)[0]['note'], '')   # the list is the game's own
        self.assertEqual(nsp.languages_control(b, u), b / 'control.nacp')   # the packed version's own list wins when it has one
        nac = bytearray(nac); struct.pack_into('<I', nac, 0x302C, 0b11); (u / 'control.nacp').write_bytes(nac)
        self.assertEqual(nsp.languages_control(u, b), u / 'control.nacp')

    def test_ipa_settings_read_the_picked_file_and_the_version_the_packed_one(self):   # build_ipa.sh's own lines: CONTROL is the NSP script's languages_control
        self.assertIn('CONTROL=str(control)', (REPO / 'scripts/build_ipa_from_nsp.py').read_text(encoding='utf-8'))
        script = (REPO / 'dm/ios/tools/build_ipa.sh').read_text(encoding='utf-8')
        lines = script[script.index('VNACP=$(dirname "$ROMFS")'):]
        lines = lines[:lines.index('\n', lines.index('VNACP=${CONTROL:-$VNACP}'))]
        self.assertIn('[ "$PN" != 0 ] || VNACP=${CONTROL:-$VNACP}', script)   # a game of several programs: its first program's list
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); (tmp / 'u').mkdir(); (tmp / 'b').mkdir()
            (tmp / 'u/control.nacp').write_bytes(control_nacp('Test Game', 'テス', '1.2.3')); (tmp / 'b/control.nacp').write_bytes(control_nacp('Test Game', 'テス', '1.0.0'))
            def after(**env):
                out = subprocess.run(['bash', '-c', f't() {{ :; }}\n{lines}\necho "$VNACP|$DM_APP_VERSION"'], capture_output=True, text=True, check=True,
                                     env={'PATH': os.environ['PATH'], 'DM': str(REPO / 'dm'), 'W': str(tmp / 'w'), 'ROMFS': str(tmp / 'u/RomFS.img'), **env})
                return out.stdout.strip()
            self.assertEqual(after(CONTROL=str(tmp / 'b/control.nacp')), f'{tmp}/b/control.nacp|1.2.3')
            self.assertEqual(after(), f'{tmp}/u/control.nacp|1.2.3')   # by hand, without the NSP script

    def test_name_label_without_nacp_writes_none(self):   # the script then takes the profile's name, as for a dump
        write_nup(self.path, [(0, TID, 0, version(), tlv([(T_NAME, b'TG'), (pack_nup.T_ICON, b'\xff\xd8 icon')]))])
        n = self.open(); app, base, _, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
        self.assertFalse((self.out() / 'control.nacp').exists())
        self.assertEqual((self.out() / 'icon.jpg').read_bytes(), b'\xff\xd8 icon')

    def test_repeat_keeps_what_is_unchanged(self):
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        main, img = self.out() / 'ExeFS/main', self.out() / 'RomFS.img'
        before = main.stat().st_ino, img.stat().st_mtime_ns
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=lambda s: None)
        self.assertEqual((main.stat().st_ino, img.stat().st_mtime_ns), before)

    def test_dlc(self):
        n = self.open(); app, _, _, dlc = pack_nup.scan(n)
        aoc = self.out() / 'aoc'; aoc.mkdir(parents=True); (aoc / '7.img').write_bytes(b'gone')
        pack_nup.pack_dlc(n, app, dlc, aoc, say=lambda s: None)
        self.assertEqual(sorted(p.name for p in aoc.iterdir()), ['1.img', '1.img.inputs.json'])
        self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))

    def test_nsp_dlc_not_reused_after_nup(self):   # the NUP packs into the aoc/ the NSP extraction keyed: the same NSPs again extract again
        n = self.open(); app, _, _, dlc = pack_nup.scan(n)
        work, logs, p = self.out(), self.out() / 'logs', self.dir / 'dlc.nsp'
        logs.mkdir(parents=True); p.write_bytes(b'nsp')
        runs = []
        def extract(argv, log, **kw): runs.append(argv); Path(argv[4]).parent.mkdir(parents=True, exist_ok=True); Path(argv[4]).write_bytes(b'from the NSP')
        nsp_fam, nup_fam = {'dlc': [(TID + 0x1001, 0, p)]}, {'dlc': [(t, None, k) for t, k in dlc.items()], 'nup': n}
        with mock.patch.object(nsp, 'run', side_effect=extract):
            for fam in (nsp_fam, nsp_fam, nup_fam): aoc = nsp.extract_aoc('nx', 'keys', REPO, work, app, fam, logs)
            self.assertEqual(len(runs), 1)   # reused while the NSPs and aoc/ are unchanged
            self.assertIn('dlcfile', romfs_image.read_index(aoc / '1.img'))
            nsp.extract_aoc('nx', 'keys', REPO, work, app, nsp_fam, logs)
        self.assertEqual(len(runs), 2)
        self.assertEqual(sorted(f.name for f in aoc.iterdir()), ['1.img'])
        self.assertEqual((aoc / '1.img').read_bytes(), b'from the NSP')


ICON = b'\xff\xd8 icon'
BUILD_ID = bytes(range(32))
# a whole meta block in SPEC §5's numbers: NPDM 1, NACP 2, icon 3, display version 4, title 5, publisher 6, build ID 7 (32 bytes), source name 8
FULL = [(1, npdm(TID)), (2, control_nacp('Test Game', 'テス', '1.2.3')), (3, ICON), (4, b'1.2.3'), (5, b'TG'), (6, b'Pub'), (7, BUILD_ID), (8, b'game.nsp')]


def shifted(recs):   # the block as nup writes it since its PR #20 (merged 2026-11-04): every type one too low (NPDM 0 … localized icons 9)
    return tlv([(t - 1, v) for t, v in recs])


class ShiftedMeta(Base):
    """NUPs whose meta block numbers every record one too low (nup since its PR #20): told apart by the rule of nup PR #32 (a record of type 0, or one of type 7 that is not
    32 bytes) and read one type up; a block in SPEC §5's numbers is read as it is. A "NACP" that is not 0x4000 bytes is left out, never saved as control.nacp"""
    def out(self): return self.dir / 'out'

    def pack(self, meta):   # a base game with this meta block, packed; returns the lines said
        write_nup(self.path, [(0, TID, 0, version(), meta)])
        n = self.open(); app, base, _, _ = pack_nup.scan(n)
        said = []
        pack_nup.pack_version(n, base, base, app, self.out(), say=said.append)
        return said

    def control(self):
        return nacp.name(self.out() / 'control.nacp'), nacp.version(self.out() / 'control.nacp'), (self.out() / 'icon.jpg').read_bytes()

    def test_shifted_block_read_one_type_up(self):
        for what, recs in (('whole block', FULL), ('marked by its NPDM (type 0) alone', [r for r in FULL if r[0] != 8]),
                           ('marked by its source name (type 7, not 32 bytes) alone', [r for r in FULL if r[0] != 1])):
            with self.subTest(what):
                said = self.pack(shifted(recs))
                self.assertEqual(self.control(), ('Test Game', '1.2.3', ICON))   # it was the icon JPEG as control.nacp and the display version as icon.jpg
                lines = [s for s in said if 'one too low' in s]
                self.assertEqual(len(lines), 1, said)
                self.assertTrue(lines[0].startswith('game.nup (base): '), lines[0])

    def test_shifted_localized_icons(self):   # localized icons (SPEC 10, shifted 9) with no default icon: the American English one
        local = b''.join(bytes([len(t)]) + t + struct.pack('<I', len(i)) + i for t, i in ((b'ja', b'ja icon'), (b'en-US', b'us icon')))
        self.pack(shifted([(1, npdm(TID)), (2, control_nacp('Test Game', 'テス', '1.2.3')), (10, local)]))
        self.assertEqual(self.control(), ('Test Game', '1.2.3', b'us icon'))

    def test_spec_block_read_as_it_is(self):   # older nup: SPEC numbers, a 32-byte build ID at type 7 and no type 0
        for what, recs in (('whole block', FULL), ('build ID only', [r for r in FULL if r[0] in (2, 3, 7)])):
            with self.subTest(what):
                said = self.pack(tlv(recs))
                self.assertEqual(self.control(), ('Test Game', '1.2.3', ICON))
                self.assertFalse([s for s in said if 'one too low' in s or 'NACP' in s], said)

    def test_type_7_record_decides(self):   # the same records without an NPDM: a 32-byte type 7 is SPEC's build ID, another size the shifted source name
        for source, expected_type in ((BUILD_ID, 7), (b'game.nsp', 8)):
            with self.subTest(source=source):
                write_nup(self.path, [(0, TID, 0, version(), tlv([(7, source)]))])
                with pack_nup.Nup(self.path) as n:
                    self.assertEqual(n.packages[0].meta(), {expected_type: source})

    def test_wrong_size_nacp_left_out(self):   # a NACP record of another size is no NACP: never saved as control.nacp (the icon at SPEC's type 3 keeps the block as it is)
        for size in (0, 0x3FFF, 0x4001):
            with self.subTest(size=size):
                said = self.pack(tlv([(2, bytes(size)), (3, ICON), (5, b'TG')]))
                self.assertFalse((self.out() / 'control.nacp').exists())
                self.assertEqual((self.out() / 'icon.jpg').read_bytes(), ICON)
                self.assertEqual([s for s in said if 'NACP' in s], [f'game.nup (base): its NACP record is {size:,} bytes, not 16,384 (0x4000): it is not a NACP, left out'])

    def test_wrong_size_nacp_cannot_supply_scan_ids(self):
        for size in (0x3FFF, 0x4001):
            with self.subTest(size=size):
                payload = bytearray(size)
                struct.pack_into('<QQ', payload, pack_nup.NACP_AOC_BASE, TID + 0x5000, TID + 0x4000)
                records = tlv([(pack_nup.T_NACP, bytes(payload))])
                dlc_id = TID + 0x1001
                write_nup(self.path, [(0, TID, 0, version(), records),
                                      (2, dlc_id, 0, {'romfs': {'addon': b'data'}}, b'')])
                with pack_nup.Nup(self.path) as n:
                    app, base, _, dlc = pack_nup.scan(n)
                    self.assertEqual((app, n.application, n.aoc_base, list(dlc)), (TID, TID, TID + 0x1000, [dlc_id]))
                    pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
                self.assertFalse((self.out() / 'control.nacp').exists())

    def test_nacp_title_starting_with_meta(self):
        payload = control_nacp('METAL', 'テス', '1.2.3')
        for records in ([(2, payload), (7, bytes(20))], [(1, payload)]):
            with self.subTest(record_type=records[0][0]):
                self.pack(tlv(records))
                self.assertEqual(nacp.name(self.out() / 'control.nacp'), 'METAL')

    def test_records_decide_before_the_rule(self):
        """nup PR #32's rule misses a shifted block with no type 0 or 7 record, and takes a SPEC block with a type 0 record or a type 7 record of another size than 32 for a shifted one:
        where the NACP (0x4000 bytes) or the icon (a JPEG or PNG) is decides first, the rule only when they cannot"""
        nacp_ = control_nacp('Test Game', 'テス', '1.2.3')
        png = b'\x89PNG\r\n\x1a\n icon'
        for what, recs, shifted_, icon in (
                ('shifted, no type 0 or 7 (an update with no NPDM)', [(1, nacp_), (2, ICON), (3, b'1.2.3')], True, ICON),
                ('shifted, no type 0 or 7, icon only', [(2, png), (3, b'1.2.3'), (4, b'TG')], True, png),
                ('shifted, a NACP of the wrong size: the icon decides', [(1, b'short'), (2, ICON), (3, b'1.2.3')], True, ICON),
                ('SPEC, with a packer\'s own type 0', [(0, b'vendor'), (2, nacp_), (3, ICON)], False, ICON),
                ('SPEC, a 64-character build ID at type 7', [(1, npdm(TID)), (2, nacp_), (3, ICON), (7, b'ab' * 32)], False, ICON),
                ('SPEC, no NACP, a 20-byte build ID: the icon decides', [(1, npdm(TID)), (3, ICON), (7, bytes(20))], False, ICON),
                ('SPEC, an NPDM of 0x4000 bytes is no NACP', [(1, npdm(TID) + bytes(0x4000 - 0x100)), (2, nacp_), (3, ICON), (7, bytes(20))], False, ICON),
                ('neither place holds a NACP or an icon: the rule', [(0, npdm(TID)), (3, b'TG')], True, None),
                ('shifted, no NACP, an icon of 0x4000 bytes is no NACP', [(0, npdm(TID)), (2, ICON + bytes(0x4000 - len(ICON))), (3, b'1.2.3')], True, ICON + bytes(0x4000 - len(ICON))),
                ('SPEC, no NACP, an icon of 0x4000 bytes', [(1, npdm(TID)), (3, ICON + bytes(0x4000 - len(ICON))), (4, b'1.2.3')], False, ICON + bytes(0x4000 - len(ICON)))):
            with self.subTest(what):
                said = self.pack(tlv(recs))
                self.assertEqual(len([s for s in said if pack_nup.SHIFTED in s]), int(shifted_), said)
                if not shifted_ and any(t == 0 or t == 7 and len(v) != 32 for t, v in recs):   # the rule said shifted, the records not: a line says so
                    self.assertTrue(any('read as it is' in s for s in said), said)
                got = self.out() / 'icon.jpg'
                self.assertEqual(got.read_bytes() if got.exists() else None, icon)
                if any(v == nacp_ for _, v in recs): self.assertEqual(nacp.name(self.out() / 'control.nacp'), 'Test Game')
                else: self.assertFalse((self.out() / 'control.nacp').exists())

    def test_duplicate_and_odd_records(self):   # the first record of a type counts; an empty record is no icon; a truncated record is a clear error, not a misread
        self.pack(tlv([(2, control_nacp('First', 'テス', '1.0.0')), (2, control_nacp('Second', 'テス', '2.0.0')), (3, b''), (3, ICON)]))
        self.assertEqual(nacp.name(self.out() / 'control.nacp'), 'First')
        self.assertFalse((self.out() / 'icon.jpg').exists())
        self.pack(tlv([(2, control_nacp('Test Game', 'テス', '1.2.3')), (3, ICON)]) + b'\3\0\0')   # a trailing scrap shorter than a record header is ignored
        self.assertEqual(self.control(), ('Test Game', '1.2.3', ICON))
        cut = tlv([(2, control_nacp('Test Game', 'テス', '1.2.3')), (3, ICON)])[:-2]
        with self.assertRaisesRegex(pack_nup.Error, r'package 0: meta record 0x3 runs past its block'): self.pack(cut)

    def test_huge_icon(self):   # an icon of 48 MiB is copied as it is (sips scales it), in seconds
        big = ICON + bytes(48 << 20)
        self.pack(tlv([(2, control_nacp('Test Game', 'テス', '1.2.3')), (3, big)]))
        self.assertEqual((self.out() / 'icon.jpg').stat().st_size, len(big))

    def test_wrong_size_nacp_falls_back_to_the_base_records(self):   # a standalone update's own bad NACP leaves the empty base record's good one in place
        write_nup(self.path, [(0, 0, 0, {}, tlv([(2, control_nacp('Test Game', 'テス', '1.2.3'))])), (1, TID, VER, version(), tlv([(2, b'short'), (3, ICON)]))])
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        said = []
        pack_nup.pack_version(n, vers[VER], base, app, self.out(), say=said.append)
        self.assertEqual(self.control(), ('Test Game', '1.2.3', ICON))
        self.assertEqual(len([s for s in said if 'not a NACP' in s]), 1, said)

    def test_said_once_per_package(self):   # the survey and the extraction both read a version's control data
        write_nup(self.path, [(0, TID, 0, version(), shifted(FULL))])
        n = self.open(); _, base, _, _ = pack_nup.scan(n)
        said = []
        for _ in range(2): pack_nup.control(base, self.dir, say=said.append)
        self.assertEqual(len(said), 1, said)

    def test_build_says_the_line(self):   # the NSP script's extraction keeps the packing quiet, not the lines about the control data (a profile: no survey)
        write_nup(self.path, [(0, TID, 0, version(), shifted(FULL)), (1, TID, VER, version(main=b'new code'), tlv([(1, b'short'), (2, ICON)]))])
        said = []
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'check_prereqs'), mock.patch.object(nsp, 'say', side_effect=said.append), mock.patch.object(nsp, 'check_versions_aarch64', side_effect=Stop), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')):
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.path)]))
        self.assertEqual([s for s in said if 'one too low' in s or 'not a NACP' in s], [f'game.nup (update {VER}): {pack_nup.SHIFTED}',
                         f'game.nup (update {VER}): its NACP record is 5 bytes, not 16,384 (0x4000): it is not a NACP, left out'])

    def test_survey_reads_a_shifted_block(self):   # the NSP script's survey (a NUP with no profile) saves the same control data, and says the line through its own output
        write_nup(self.path, [(0, NEW, 0, version(tid=NEW), shifted(FULL))])
        n = self.open(); _, base, _, _ = pack_nup.scan(n)
        said = []
        with mock.patch.object(nsp, 'say', side_effect=said.append): nsp.survey_nup(n, base, base, self.dir / 'survey')
        self.assertEqual(nacp.name(self.dir / 'survey/control.nacp'), 'Test Game')
        self.assertTrue(any('one too low' in s for s in said), said)


class Stop(Exception): pass


S1, S2 = 0x0100F00000010000, 0x0400F00000010001   # a Switch 2 Edition of a Switch 1 game: the Switch 1 program (index 0) and the Switch 2 program (index 1) of application S1


def program_nacp(owner=S1, aoc=S1 + 0x1000):   # a Switch 2 program's NACP: its application (SaveDataOwnerId) and add-on content base, as Super Mario Bros. Wonder's names them
    b = bytearray(control_nacp('Test Game S2', 'テス', '1.2.1')); struct.pack_into('<QQ', b, pack_nup.NACP_AOC_BASE, aoc, owner)
    return tlv([(pack_nup.T_NACP, bytes(b))])


def edition(s1_updates=(), s2_updates=(), nacp=None):
    """The packages of a Switch 2 Edition NUP (PR #2692's layout): the Switch 1 program with all the game data, the Switch 2 program whose RomFS holds one file of its own, the upgrade add-on.
    sN_updates: (version, marker) of updates"""
    def prog(tid, marker, main): return {'exefs': {'main': main, 'main.npdm': npdm(tid)}, 'romfs': {f'data/{marker}': b'data' * 100, 'System/RegionLangMask.txt': marker.encode()}}
    s2 = prog(S2, 's2', b'switch 2 code'); s2['romfs'] = {'System/RegionLangMask.txt': b'own'}
    return ([(0, S1, 0, prog(S1, 's1', b'switch 1 code'), META), (0, S2, 0, s2, program_nacp() if nacp is None else nacp)]
            + [(1, S1 + 0x800, v, prog(S1, m, b'switch 1 ' + m.encode()), b'') for v, m in s1_updates]
            + [(1, S2 + 0x800, v, prog(S2, m, b'switch 2 ' + m.encode()), b'') for v, m in s2_updates]
            + [(2, S1 + 0x1001, 0, {'romfs': {'System/AocUpgradeVersion.txt': b'1', 'data/upgrade': b'u'}}, b'')])


class Programs(Base):
    """One application of several programs (issue/PR #2692): the program packed runs on its own RomFS, the others' RomFS go beside it as romfs_programs/<index>.img (the game mounts them by
    program index, nn::fs::MountRom(name, index, ...)), and the add-on is DLC only, indexed from the NACP's AddOnContentBaseId"""
    def out(self): return self.dir / 'out'

    def names(self, img): return set(romfs_image.read_index(img))

    def test_switch_2_program_by_default_with_the_other_beside_it(self):
        write_nup(self.path, edition())
        n = self.open(); app, base, vers, dlc = pack_nup.scan(n)
        self.assertEqual((app, n.application, n.aoc_base, sorted(n.siblings), vers), (S2, S1, S1 + 0x1000, [0], {}))
        pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
        self.assertEqual((self.out() / 'ExeFS/main').read_bytes(), b'switch 2 code')
        own, s1 = self.names(self.out() / 'RomFS.img'), self.names(self.out() / 'romfs_programs/0.img')
        self.assertIn('System/RegionLangMask.txt', own); self.assertNotIn('data/s1', own)   # its own files only
        self.assertIn('data/s1', s1); self.assertNotIn('data/upgrade', s1 | own)   # the add-on is laid over nothing: the game mounts it itself
        self.assertEqual(nacp.name(self.out() / 'control.nacp'), 'Test Game S2')
        pack_nup.pack_dlc(n, app, dlc, self.out() / 'aoc', say=lambda s: None)
        self.assertIn('data/upgrade', self.names(self.out() / 'aoc/1.img'))   # index 1 from S1 + 0x1000, not from the Switch 2 program's ID

    def test_program_picks_the_other(self):
        write_nup(self.path, edition())
        n = self.open(); app, base, _, _ = pack_nup.scan(n, 0)
        self.assertEqual((app, sorted(n.siblings)), (S1, [1]))
        pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
        self.assertIn('data/s1', self.names(self.out() / 'RomFS.img'))
        self.assertEqual(self.names(self.out() / 'romfs_programs/1.img'), {'System', 'System/RegionLangMask.txt'})
        with self.assertRaisesRegex(pack_nup.Error, r'no program 5; it holds 0 \(0100F00000010000\), 1 \(0400F00000010001\)'): pack_nup.scan(self.open(), 5)

    def test_both_programs_with_updates(self):   # updates of one version for both programs, as Horizon patches an application's programs together; the other program at its newest no higher
        write_nup(self.path, edition(s1_updates=[(VER - 1, 's1old'), (VER, 's1new')], s2_updates=[(VER, 's2new'), (VER + 1, 's2newest')]))
        n = self.open(); app, base, vers, _ = pack_nup.scan(n)
        self.assertEqual((app, sorted(vers)), (S2, [VER, VER + 1]))
        for k, want in ((base, 'data/s1'), (vers[VER], 'data/s1new'), (vers[VER + 1], 'data/s1new')):
            with self.subTest(version=k.version):
                out = self.out() / f'v{k.version}'
                pack_nup.pack_version(n, k, base, app, out, say=lambda s: None)
                self.assertIn(want, self.names(out / 'romfs_programs/0.img'))
        self.assertEqual((self.out() / f'v{VER}/ExeFS/main').read_bytes(), b'switch 2 s2new')
        n = self.open(); app, base, vers, _ = pack_nup.scan(n, 0)
        self.assertEqual((app, sorted(vers)), (S1, [VER - 1, VER]))
        pack_nup.pack_version(n, vers[VER - 1], base, app, self.out() / 's1', say=lambda s: None)
        self.assertEqual(self.names(self.out() / 's1/romfs_programs/1.img'), {'System', 'System/RegionLangMask.txt'})   # the Switch 2 program's base: it has no update of VER − 1 or lower

    def test_one_switch_2_program_alone(self):   # the layout of a NUP whose packer laid the add-on over the Switch 2 program's RomFS itself: one base, program index 1, DLC from its NACP
        tree = {'exefs': {'main': b'switch 2 code', 'main.npdm': npdm(S2)}, 'romfs': {'data/merged': b'm'}}
        write_nup(self.path, [(0, S2, 0, tree, program_nacp()), (2, S1 + 0x1001, 0, {'romfs': {'data/upgrade': b'u'}}, b'')])
        n = self.open(); app, base, vers, dlc = pack_nup.scan(n)
        self.assertEqual((app, n.application, n.siblings, sorted(dlc)), (S2, S1, {}, [S1 + 0x1001]))
        (self.out() / 'romfs_programs').mkdir(parents=True); (self.out() / 'romfs_programs/0.img').write_bytes(b'stale')
        pack_nup.pack_version(n, base, base, app, self.out(), say=lambda s: None)
        self.assertFalse((self.out() / 'romfs_programs').exists())   # a sibling of an earlier pack goes

    def test_refusals(self):
        other = edition(nacp=program_nacp(owner=0x0100F00000020000))
        two = edition() + [(0, S2, 0, {'exefs': {'main': b'x', 'main.npdm': npdm(S2)}, 'romfs': {'a': b'a'}}, b'')]
        dlc = edition(nacp=program_nacp(aoc=S1 + 0x5000))
        for packages, error in ((other, 'packages of different applications'), (two, '2 base packages of program 0400F00000010001'), (dlc, 'is not a DLC of 0100F00000010000')):
            with self.subTest(error=error):
                write_nup(self.path, packages)
                with pack_nup.Nup(self.path) as n, self.assertRaisesRegex(pack_nup.Error, error): pack_nup.scan(n)

    def test_script_reads_the_program_and_its_siblings(self):   # --program, --title-id (the program's or the application's), the survey's manifest, the disk estimate, the format check
        write_nup(self.path, edition(s1_updates=[(VER, 's1new')]))
        for program, want, app in ((None, S2, S2), (0, S1, S1), (None, S1, S2)):
            with self.subTest(program=program, want=want):
                got, fam = nsp.scan_nup(self.path, want, program); self.addCleanup(fam['nup'].close)
                self.assertEqual(got, app)
        with self.assertRaisesRegex(nsp.BuildError, 'no game'): nsp.scan_nup(self.path, 0x0100F00000020000)
        got, fam = nsp.scan_nup(self.path); n = fam['nup']; self.addCleanup(n.close)
        out = self.dir / 'survey'
        self.assertEqual(nsp.survey_nup(n, fam['base'], fam['base'], out), 2)
        rows = (out / 'romfs_manifest.tsv').read_text().splitlines()[1:]
        self.assertIn(f'/System/RegionLangMask.txt\t3\t{b"own".hex()}', rows)   # its own file wins over the other program's of that path
        self.assertTrue(any(r.startswith('/data/s1\t') for r in rows))
        self.assertEqual(pack_nup.romfs_bytes(n, fam['base'], fam['base']), 3 + 400 + 2)
        with mock.patch.object(nsp, 'PROG', mock.Mock()):
            d = nsp.extract_nup(self.dir / 'ex', 'v0', fam['base'], fam['base'], got, n)
            self.assertTrue((d / 'romfs_programs/0.img').is_file())
            bad = edition(); bad[0][3]['romfs']['data/x.zs'] = b'\0' * 64
            write_nup(self.path, bad); got, fam = nsp.scan_nup(self.path); self.addCleanup(fam['nup'].close)
            nsp.extract_nup(self.dir / 'ex', 'v0', fam['base'], fam['base'], got, fam['nup'])
            with mock.patch.object(nsp, 'ALLOW_DAMAGED', False), self.assertRaisesRegex(nsp.BuildError, r'v0 program 0: 1 file'): nsp.check_formats({'v0': d})


class StandaloneCLI(Base):
    def test_default_and_explicit_versions_and_missing_base(self):
        write_nup(self.path, [(1, TID, VER, version(), META),
                              (1, TID, VER + 1, version(main=b'newest'), b'')], update_flags=pack_nup.STANDALONE)
        work = self.dir / 'cli'
        cmd = [sys.executable, str(REPO / 'scripts/pack_nup.py'), str(self.path), '--work-dir', str(work)]
        for args, ver, main in (([], VER + 1, b'newest'), (['--version', str(VER)], VER, b'code')):
            r = subprocess.run(cmd + args, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual((work / f'extract/v{ver}/ExeFS/main').read_bytes(), main)
            self.assertIn(pack_nup.NO_BASE, r.stdout)
        r = subprocess.run(cmd + ['--version', '0'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn('no base game (version 0)', r.stderr)
        self.assertNotIn('Traceback', r.stderr)
        self.assertFalse((work / 'extract/v0').exists())


class ExternalDlc(Base):
    def setUp(self):
        super().setUp()
        self.source = self.dir / 'player-dlc'; self.source.mkdir()
        self.work = self.dir / 'build'
        self.romfs = self.dir / 'dlc-romfs'; self.romfs.mkdir()
        self.marker = self.romfs / 'marker'

    def image(self, name, content):
        self.marker.write_bytes(content)
        img = self.source / name
        romfs_image.pack(self.romfs, img)
        return img

    def test_rebuild_uses_only_current_selected_images(self):
        old = self.open()
        app, _, _, dlc = pack_nup.scan(old)
        cached = self.work / 'aoc'
        pack_nup.pack_dlc(old, app, dlc, cached, say=lambda _: None)   # a stamped image from a previous pack
        (cached / '4.img').write_bytes((cached / '1.img').read_bytes())   # an earlier NSP extraction had only a directory-wide key
        (self.work / 'aoc.key').write_text('previous extraction')
        one = self.image('1.img', b'original')
        two = self.image('2.IMG', b'other')
        (self.source / '._1.img').write_bytes(b'AppleDouble')

        staged = nsp.stage_external_dlc(self.work, self.source)
        assert staged is not None
        self.assertEqual(sorted(p.name for p in staged.iterdir()), ['1.img', '2.img'])
        self.assertEqual((staged / '1.img').read_bytes(), one.read_bytes())
        self.assertEqual((staged / '2.img').read_bytes(), two.read_bytes())
        with mock.patch.object(nsp.shutil, 'copyfile', side_effect=AssertionError('copied unchanged DLC')):
            self.assertEqual(nsp.stage_external_dlc(self.work, self.source), staged)

        before = one.stat()
        self.image(one.name, b'replaced')
        os.utime(one, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertEqual(one.stat().st_size, before.st_size)
        self.assertEqual(nsp.stage_external_dlc(self.work, self.source), staged)
        self.assertEqual(nsp.rfs.Path(staged / '1.img/marker').read_bytes(), b'replaced')
        two.unlink()
        self.assertEqual(nsp.stage_external_dlc(self.work, self.source), staged)
        self.assertEqual(sorted(p.name for p in staged.iterdir()), ['1.img'])
        self.assertEqual((staged / '1.img').read_bytes(), one.read_bytes())
        self.assertEqual(nsp.rfs.Path(staged / '1.img/marker').read_bytes(), b'replaced')

        nsp.rmtree(staged.parent)   # a front end can discard the stage after a successful build
        self.assertEqual(nsp.stage_external_dlc(self.work, self.source), staged)
        self.assertEqual(nsp.rfs.Path(staged / '1.img/marker').read_bytes(), b'replaced')
        one.unlink()
        self.assertIsNone(nsp.stage_external_dlc(self.work, self.source))
        self.assertEqual(list(staged.iterdir()), [])
        self.assertEqual(sorted(p.name for p in cached.iterdir()), ['1.img', '1.img.inputs.json', '4.img'])

    def test_invalid_image_keeps_the_previous_stage(self):
        one = self.image('1.img', b'original')
        staged = nsp.stage_external_dlc(self.work, self.source)
        assert staged is not None
        before = one.read_bytes()
        self.image(one.name, b'replaced')
        broken = self.source / '2.img'; broken.write_bytes(b'not a RomFS image')
        with self.assertRaisesRegex(nsp.BuildError, r'invalid RomFS image .*2\.img'):
            nsp.stage_external_dlc(self.work, self.source)
        self.assertEqual(list(staged.iterdir()), [staged / '1.img'])
        self.assertEqual((staged / '1.img').read_bytes(), before)
        self.assertFalse(staged.parent.with_suffix('.partial').exists())
        broken.unlink()
        self.assertEqual(nsp.stage_external_dlc(self.work, self.source), staged)
        self.assertEqual((staged / '1.img').read_bytes(), one.read_bytes())

    def test_invalid_index_is_rejected(self):
        img = self.image('2001.img', b'dlc')
        with self.assertRaisesRegex(nsp.BuildError, r'indices 1 to 2000: .*2001\.img'):
            nsp.stage_external_dlc(self.work, self.source)
        self.assertTrue(img.is_file())

    def test_option_requires_a_nup_without_embedded_dlc(self):
        for target in (self.source, self.path):
            with self.subTest(target=target), self.assertRaisesRegex(nsp.BuildError, 'requires a NUP without embedded DLC'):
                nsp.main(nsp.parse_args([str(target), '--aoc-dir', str(self.source)]))


class Main(Base):
    def test_aoc_dir_supplies_the_builds_dlc(self):
        write_nup(self.path, [(0, TID, 0, version(), META), (1, TID, VER, version(), b'')])
        romfs = self.dir / 'dlc-romfs'; romfs.mkdir()
        (romfs / 'marker').touch()
        source = self.dir / 'aoc'; source.mkdir()
        romfs_image.pack(romfs, source / '1.img')
        work = self.dir / f'{TID:016X}_build'
        stale = work / 'aoc'; stale.mkdir(parents=True)
        romfs_image.pack(romfs, stale / '2.img')
        from game import Game
        game = Game(REPO / 'dm/games' / f'{TID:016X}')
        sources = {'graphics': ('gl', 'test'), 'gl_capture': (False, ''), 'gl_fixed': (False, '')}
        with mock.patch.object(nsp, 'check_prereqs'), mock.patch.object(nsp, 'check_versions_aarch64'), \
             mock.patch.object(nsp, 'build_mismatch', return_value=[]), mock.patch.object(nsp, 'build_sources', return_value=sources), \
             mock.patch.object(nsp, 'rewrite', return_value=game), mock.patch.object(nsp, 'chain_header'), \
             mock.patch.object(nsp, 'say'), mock.patch.object(nsp, 'build_gl_shaders', side_effect=Stop) as shaders:
            for extra, expected in ((['--aoc-dir', str(source)], [work / 'external-dlc/aoc/1.img']), ([], [])):
                with self.subTest(extra=extra), self.assertRaises(Stop):
                    nsp.main(nsp.parse_args([str(self.path)] + extra))
                self.assertEqual(shaders.call_args.args[2][2:], expected)

    def test_app_version_from_a_nup_when_only_the_update_is_extracted(self):
        tid, ver = 0x0400486024DEC000, 131072
        from game import Game
        row = Game(REPO / 'dm/games' / f'{tid:016X}').builds()[f'v{ver}']
        update = version(tid=tid)
        for module, bid in row.items():
            if module != 'display': update['exefs'][module] = b'NSO0' + bytes(0x3c) + bytes.fromhex(bid)
        npdm64 = bytearray(update['exefs']['main.npdm']); npdm64[0xc] = 1
        update['exefs']['main.npdm'] = bytes(npdm64)
        label = tlv([(pack_nup.T_DISPLAY, b'v1.1.1')])
        base_label = tlv([(pack_nup.T_DISPLAY, b'1.0.0')])
        update_nacp = tlv([(pack_nup.T_NACP, control_nacp('Test Game', '', '2.3.4'))])
        sources = {'graphics': ('gl', 'test'), 'gl_capture': (False, '')}
        registered = row['display']
        for expected, display, base_meta, update_meta in (('1.1.1', '?', base_label, label), ('1.0', '?', base_label, b''),
                                                          ('1.2.3', '?', META, label), ('1.1.1', registered, META, label),
                                                          ('1.1.1', registered, b'', b''), ('2.3.4', registered, META, label + update_nacp)):
            with self.subTest(expected=expected, display=display):
                write_nup(self.path, [(0, tid, 0, version(tid=tid), base_meta), (1, tid, ver, update, update_meta)])
                with mock.patch.object(Game, 'builds', return_value={f'v{ver}': {**row, 'display': display}}), \
                     mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), \
                     mock.patch.object(nsp, 'check_prereqs'), mock.patch.object(nsp, 'build_sources', return_value=sources), \
                     mock.patch.object(nsp, 'say'), mock.patch.object(nsp, 'say_game', side_effect=Stop) as app:
                    with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.path), '--platform', 'ios']))
                self.assertEqual(app.call_args.args[2], expected)
                self.assertFalse((self.dir / f'{tid:016X}_build/extract/v0').exists())

    def run_main(self, target, *, extract=False, package=None):
        seen, said = {}, []
        def stop(env, *a):
            seen.update(env)
            if not extract: raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=stop), mock.patch.object(nsp, 'say', side_effect=said.append), \
             mock.patch.object(nsp, 'check_versions_aarch64', side_effect=Stop), \
             mock.patch.object(nsp, 'ask', side_effect=AssertionError('asked for something')), mock.patch.object(nsp, 'package_reader', side_effect=AssertionError('read a package')):
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(target)] + (['--package', package] if package else [])))
        return Path(seen['DM_ROOT']), said

    def test_needs_no_keys_and_builds_beside_the_nup(self):
        for target in (self.dir, self.path):
            work, said = self.run_main(target)
            self.assertEqual(work, self.dir / f'{TID:016X}_build')
            self.assertTrue(any(s.startswith('a NUP: game.nup') for s in said), said)

    def test_standalone_update_reaches_extraction_without_a_base(self):
        write_nup(self.path, [(1, TID, VER, version(), META)], update_flags=pack_nup.STANDALONE)
        for target in (self.dir, self.path):
            work, said = self.run_main(target, extract=True)
            self.assertFalse((work / 'extract/v0').exists())
            self.assertEqual((work / f'extract/v{VER}/ExeFS/main').read_bytes(), b'code')
            self.assertEqual(nacp.name(work / f'extract/v{VER}/control.nacp'), 'Test Game')
            self.assertFalse(any(s.startswith('base:') for s in said))
            self.assertIn(pack_nup.NO_BASE, said)
            with self.assertRaisesRegex(nsp.BuildError, 'version to pack .* is not in the folder'):
                self.run_main(target, package='v0')

    def test_both_forms_of_no_base_pack_alike(self):   # Mario's profile needs v0 and registers the update: without a base game it is packed from a derived profile, whichever the form
        tid, ver = 0x010015100B514000, 327680
        for packages, flags in (([(1, tid, ver, version(tid=tid), META)], pack_nup.STANDALONE), ([(0, 0, 0, {'exefs': {}}, META), (1, tid, ver, version(tid=tid), b'')], 0)):
            with self.subTest(flags=flags):
                write_nup(self.path, packages, update_flags=flags)
                work, said = self.run_main(self.path, extract=True)
                self.assertFalse((work / 'extract/v0').exists())
                self.assertEqual((work / f'extract/v{ver}/ExeFS/main').read_bytes(), b'code')
                self.assertEqual(nacp.name(work / f'extract/v{ver}/control.nacp'), 'Test Game')
                self.assertTrue(any('the folder has no v0' in s for s in said), said)
                self.assertIn(pack_nup.NO_BASE, said)

    def test_a_version_it_cannot_pack_is_a_red_line(self):   # through cli(): the disk check reads the RomFS folder before any extraction
        exefs_only = {'exefs': version()['exefs']}
        for packages, flags, package in (([(1, TID, VER, exefs_only, b'')], pack_nup.STANDALONE, None),   # the only update
                                         ([(1, TID, VER - 1, version(), b''), (1, TID, VER, exefs_only, b'')], pack_nup.STANDALONE, None),   # the newest, picked by default
                                         ([(0, TID, 0, exefs_only, b''), (1, TID, VER, version(), b'')], 0, 'v0')):   # a base with code and no RomFS
            with self.subTest(flags=flags, package=package):
                write_nup(self.path, packages, update_flags=flags)
                argv = [str(self.path)] + (['--package', package] if package else [])
                with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROD_KEYS', ''), \
                     mock.patch.object(nsp, 'check_prereqs'), mock.patch.object(nsp, 'say'), mock.patch.object(nsp.PROG, 'say') as red:
                    self.assertEqual(nsp.cli(argv), 1)
                self.assertRegex(red.call_args.args[0], r'^\[RED\] .*no romfs/')


if __name__ == '__main__':
    unittest.main()
