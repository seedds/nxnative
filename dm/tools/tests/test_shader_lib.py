#!/usr/bin/env python3
# A game's shader libraries as one file (dm/tools/shader_lib.py): the folder and image states, and a read by the host's C reader (dm/gfx/nvn/msl_image.h, dm/host/romfs_image.h).
import errno, os, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs, shader_lib

READER = r'''
#include <stdio.h>
#include "msl_image.h"
// msl_read <image> <name>...: each name's bytes, or "missing"; "misaligned" when a file's first byte is not on 16 bytes
int main(int argc, char **argv) {
    romfs_img *m = romfs_image_open(argv[1]);
    if (!m) { puts("refused"); return 1; }
    for (int i = 2; i < argc; i++) {
        const img_ent *x = msl_image_find(m, argv[i]);
        if (!x) { puts("missing"); continue; }
        msl_map f;
        if (!msl_image_map(m, x, &f)) { printf("unmapped %d\n", errno); continue; }
        if ((uintptr_t)f.data % 16) puts("misaligned");
        fwrite(f.data, 1, f.size, stdout); putchar('\n');
        munmap(f.base, f.base_len);
    }
    romfs_image_close(m);
    return 0;
}
'''


class Base(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name); self.lib = self.root / 'b-0123456789ab'; self.image = self.root / 'b-0123456789ab.img'
        self.said = []

    def folder(self, stamp=True, **files):
        self.lib.mkdir(exist_ok=True)
        for name, data in files.items(): (self.lib / name.replace('__', '.')).write_bytes(data)
        if stamp: (self.lib / 'inputs.tsv').write_text('# head\ngame\t0100\ninputs\tabc\n')

    def pack(self): return shader_lib.pack(self.lib, self.said.append)


class States(Base):
    def test_failed_write_removes_part_and_keeps_existing_image(self):
        self.image.write_bytes(b'old image')
        def full(): raise OSError(errno.ENOSPC, 'No space left on device')
        with self.assertRaises(OSError) as raised:
            shader_lib.write({'a': b'written', 'b': full}, str(self.image))
        self.assertEqual(raised.exception.errno, errno.ENOSPC)
        self.assertFalse(Path(f'{self.image}.part').exists())
        self.assertEqual(self.image.read_bytes(), b'old image')

    def test_failed_rename_removes_part_and_keeps_existing_image(self):
        self.image.write_bytes(b'old image')
        with mock.patch.object(shader_lib.os, 'replace', side_effect=OSError(errno.ENOSPC, 'No space left on device')):
            with self.assertRaises(OSError) as raised: shader_lib.write({'a': b'new'}, str(self.image))
        self.assertEqual(raised.exception.errno, errno.ENOSPC)
        self.assertFalse(Path(f'{self.image}.part').exists())
        self.assertEqual(self.image.read_bytes(), b'old image')

    def test_pack_replaces_the_folder_and_reads_in_place(self):
        self.folder(k_b__metallib=b'lib', k_b__json=b'{}', k_b__metal=b'src')
        self.assertTrue(self.pack())
        self.assertFalse(self.lib.exists()); self.assertTrue(self.image.is_file())
        v = shader_lib.view(self.lib)
        self.assertEqual(str(v), str(self.image))
        self.assertEqual((v / 'k_b.metallib').read_bytes(), b'lib')
        self.assertEqual(sorted(p.name for p in v.glob('*_b.*')), ['k_b.json', 'k_b.metal', 'k_b.metallib'])
        self.assertTrue(shader_lib.has_library(self.lib))
        self.assertEqual(sorted(os.listdir(self.root)), [self.image.name], 'no part or stamp beside it')

    def test_a_stamped_folder_replaces_the_image(self):   # a finished full build: a shader it no longer makes does not stay
        self.folder(old_b__metallib=b'old'); self.pack()
        self.folder(new_b__metallib=b'new')
        self.assertEqual(str(shader_lib.view(self.lib)), str(self.lib))
        self.pack()
        self.assertEqual([p.name for p in shader_lib.view(self.lib).glob('*.metallib')], ['new_b.metallib'])

    def test_a_partial_build_goes_over_the_image(self):
        self.folder(a_b__metallib=b'a1', b_b__metallib=b'b1'); self.pack()
        self.folder(stamp=False, a_b__metallib=b'a2')
        self.assertEqual(str(shader_lib.view(self.lib)), str(self.image), 'tools read the whole library, not the few keys')
        self.pack()
        v = shader_lib.view(self.lib)
        self.assertEqual(((v / 'a_b.metallib').read_bytes(), (v / 'b_b.metallib').read_bytes()), (b'a2', b'b1'))
        self.assertEqual(shader_lib.read_stamp(self.lib)['inputs'], 'abc', 'the stamp stays with the rest')

    def test_a_folder_alone_is_read_as_it_is(self):
        self.folder(k_b__metallib=b'lib')
        self.assertEqual(str(shader_lib.view(self.lib)), str(self.lib))
        self.assertEqual(shader_lib.read_stamp(self.lib)['game'], '0100')
        self.assertEqual(shader_lib.table_head(self.lib), '# chain b-0123456789ab inputs abc')

    def test_nothing_there(self):
        self.assertFalse(shader_lib.has_library(self.lib)); self.assertFalse(shader_lib.exists(self.lib))
        self.assertFalse(self.pack()); self.assertEqual(shader_lib.read_stamp(self.lib), {})
        self.assertEqual(shader_lib.table_head(self.lib), '# chain b-0123456789ab inputs none')

    def test_the_name_is_the_folders_for_an_image_too(self):
        self.folder(k_b__metallib=b'lib'); self.pack()
        self.assertEqual(shader_lib.table_head(self.image), '# chain b-0123456789ab inputs abc')
        self.assertEqual(shader_lib.name_of(self.image), 'b-0123456789ab')

    def test_detritus_stays_out(self):
        self.folder(k_b__metallib=b'lib')
        (self.lib / '._k_b.metallib').write_bytes(b'appledouble'); (self.lib / 'k_b.json.tmp-1-2').write_bytes(b'stopped')
        self.pack()
        self.assertEqual(sorted(p.name for p in shader_lib.view(self.lib).iterdir()), ['inputs.tsv', 'k_b.metallib'])

    def test_a_failed_pack_keeps_the_folder(self):
        self.folder(k_b__metallib=b'lib'); (self.lib / 'sub').mkdir()
        self.assertFalse(self.pack())
        self.assertTrue((self.lib / 'k_b.metallib').is_file()); self.assertFalse(self.image.exists()); self.assertFalse(Path(f'{self.image}.part').exists())
        self.assertIn('[RED]', self.said[0])

    def test_a_folder_that_changes_while_packed_stays(self):
        self.folder(k_b__metallib=b'lib')
        real = shader_lib.write
        def write(files, dst): real(files, dst); (self.lib / 'late_b.metallib').write_bytes(b'late')
        shader_lib.write = write
        self.addCleanup(setattr, shader_lib, 'write', real)
        self.pack()
        self.assertTrue((self.lib / 'late_b.metallib').is_file())

    def test_expand_gives_the_folder_back(self):
        self.folder(k_b__metallib=b'lib'); self.pack()
        shader_lib.expand(self.lib)
        self.assertFalse(self.image.exists()); self.assertEqual((self.lib / 'k_b.metallib').read_bytes(), b'lib'); self.assertTrue((self.lib / 'inputs.tsv').is_file())

    def test_remove_image(self):
        self.folder(k_b__metallib=b'lib'); self.pack()
        Path(f'{self.image}.part').write_bytes(b'x')
        shader_lib.remove_image(self.lib); shader_lib.remove_image(self.lib)
        self.assertEqual(os.listdir(self.root), [])


class Write(Base):   # dmsh_build.py writes the image from functions that read its outputs out of the shader cache: called ahead of the writer on a few threads, taken in order
    def test_functions_are_called_ahead_and_written_in_order(self):
        import threading
        data = {f'{i:04x}_b.metallib': os.urandom(1 + i % 97) for i in range(300)}
        threads, calls, lock = set(), [], threading.Lock()
        def source(name):
            def read():
                with lock: threads.add(threading.get_ident()); calls.append(name)
                return data[name]
            return read
        files: dict = {n: source(n) for n in data}
        files['path.json'] = str(self.root / 'on-disk'); (self.root / 'on-disk').write_bytes(b'from a path'); files['bytes.tsv'] = b'given'
        shader_lib.write(files, str(self.image))
        self.assertEqual(sorted(calls), sorted(data))   # each once
        self.assertNotIn(threading.get_ident(), threads); self.assertGreater(len(threads), 1)
        got = {p.name: p.read_bytes() for p in romfs.Path(str(self.image)).iterdir()}
        self.assertEqual(got, {**data, 'path.json': b'from a path', 'bytes.tsv': b'given'})
        plain = self.root / 'plain.img'; shader_lib.write({**data, 'path.json': b'from a path', 'bytes.tsv': b'given'}, str(plain))   # the bytes themselves, on no thread
        self.assertEqual(plain.read_bytes(), self.image.read_bytes())

    def test_write_reports_progress(self):
        events = []
        shader_lib.write({'a': b'1234', 'b': lambda: b'56789'}, str(self.image), on_progress=lambda d, t, b: events.append((d, t, b)))
        self.assertEqual([(d, t) for d, t, _ in events], [(0, 2), (1, 2), (2, 2)])
        self.assertEqual(events[0][2], 0)
        self.assertGreater(events[1][2], 0)
        self.assertGreater(events[2][2], events[1][2])
        empty = []
        shader_lib.write({}, str(self.root / 'empty.img'), on_progress=lambda d, t, b: empty.append((d, t, b)))
        self.assertEqual(empty, [(0, 0, 0)])

    def test_a_function_that_fails_or_has_no_bytes_leaves_no_image(self):
        def gone(): raise OSError('the cache lost it')
        for bad, error in ((gone, 'the cache lost it'), (lambda: None, 'no bytes to write')):
            with self.subTest(error=error), self.assertRaisesRegex(OSError, error):
                shader_lib.write({'a_b.metallib': b'lib', 'b_b.metallib': bad, **{f'c{i}_b.metallib': (lambda: b'x') for i in range(100)}}, str(self.image))
            self.assertFalse(self.image.exists())


class LeaveOutPulling(Base):
    def names(self): return sorted(p.name for p in shader_lib.view(self.lib).iterdir())

    def test_from_a_folder(self):
        self.folder(k_b__metallib=b'lib', k_bp__metallib=b'pull')
        self.assertTrue(shader_lib.pack(self.lib, self.said.append, leave_out=True))
        self.assertEqual(self.names(), ['_bp_left_out', 'inputs.tsv', 'k_b.metallib'])
        self.assertTrue(shader_lib.left_out_pulling(self.lib))

    def test_from_an_image_that_has_them(self):   # the image alone: nothing is built again
        self.folder(k_b__metallib=b'lib', k_bp__metallib=b'pull'); self.pack()
        self.assertFalse(shader_lib.left_out_pulling(self.lib))
        shader_lib.pack(self.lib, self.said.append, leave_out=True)
        self.assertEqual(self.names(), ['_bp_left_out', 'inputs.tsv', 'k_b.metallib'])
        self.assertEqual((shader_lib.view(self.lib) / 'k_b.metallib').read_bytes(), b'lib')

    def test_an_image_without_them_is_not_written_again(self):   # what keeps the IPA's copy of it reusable
        self.folder(k_b__metallib=b'lib', k_bp__metallib=b'pull'); shader_lib.pack(self.lib, self.said.append, leave_out=True)
        before = self.image.stat().st_ino, self.image.stat().st_mtime_ns
        self.assertTrue(shader_lib.pack(self.lib, self.said.append, leave_out=True))
        self.assertEqual((self.image.stat().st_ino, self.image.stat().st_mtime_ns), before)

    def test_a_full_build_that_wants_them_drops_the_marker(self):
        self.folder(k_b__metallib=b'lib', k_bp__metallib=b'pull'); shader_lib.pack(self.lib, self.said.append, leave_out=True)
        self.folder(k_b__metallib=b'lib', k_bp__metallib=b'pull'); self.pack()
        self.assertFalse(shader_lib.left_out_pulling(self.lib)); self.assertIn('k_bp.metallib', self.names())

    def test_pack_reports_progress_only_when_writing(self):
        self.folder(k_b__metallib=b'lib', k_bp__metallib=b'pull')
        events = []
        self.assertTrue(shader_lib.pack(self.lib, self.said.append, on_progress=lambda d, t, b: events.append((d, t, b))))
        self.assertEqual([(d, t) for d, t, _ in events], [(0, 3), (1, 3), (2, 3), (3, 3)])
        events.clear()
        with mock.patch.object(shader_lib.romfs, 'Image', side_effect=AssertionError('should not open image')):
            self.assertTrue(shader_lib.pack(self.lib, self.said.append, on_progress=lambda d, t, b: events.append((d, t, b))))
        self.assertEqual(events, [])
        self.assertTrue(shader_lib.pack(self.lib, self.said.append, leave_out=True, on_progress=lambda d, t, b: events.append((d, t, b))))
        self.assertEqual(events[0][:2], (0, 3))
        self.assertEqual(events[-1][:2], (3, 3))
        events.clear()
        self.assertTrue(shader_lib.pack(self.lib, self.said.append, leave_out=True, on_progress=lambda d, t, b: events.append((d, t, b))))
        self.assertEqual(events, [])


class AppSubset(Base):
    def test_only_what_the_host_reads(self):
        self.folder(k_b__metallib=b'lib', k_b__json=b'{}', k_b__metal=b'src', k_bp__metallib=b'pull', _b_build__json=b'[]', _b_cas__txt=b'd')
        for packed in (False, True):
            if packed: self.pack()
            out = self.root / f'app{packed}.img'
            self.assertEqual(shader_lib.subset(self.lib, out), 4)
            self.assertEqual(sorted(romfs.Path(out).iterdir()), sorted(romfs.Path(out) / n for n in ('inputs.tsv', 'k_b.json', 'k_b.metallib', 'k_bp.metallib')))
            self.assertEqual(shader_lib.subset(self.lib, out, ('_bp.metallib',)), 3)

    def test_refuses_a_library_with_no_shaders(self):
        self.folder(k_b__json=b'{}')
        with self.assertRaises(ValueError): shader_lib.subset(self.lib, self.root / 'app.img')


class SharedPayloads(Base):
    def test_share_keys_dedup_mixed_sources_and_skip_alias_loaders(self):
        payload = b'same compiled shader' * 100
        source = self.root / 'source.metallib'; source.write_bytes(payload)
        calls, events = [], []
        def load(name):
            def read(): calls.append(name); return payload
            return read
        files = {
            'a_b.metallib': load('a_b.metallib'),
            'b_b.metallib': source,
            'c_b.metallib': load('c_b.metallib'),
            'd_b.metallib': payload,
            'e_b.metallib': payload,   # unshared (None)
            'f_b.metallib': b'unique',
        }
        share = {'a_b.metallib': 'k1', 'b_b.metallib': 'k1', 'c_b.metallib': 'k1', 'd_b.metallib': 'k1', 'e_b.metallib': None, 'f_b.metallib': 'k2'}
        shader_lib.write(files, str(self.image), on_progress=lambda d, t, b: events.append((d, t, b)), share=share)
        image = romfs.Image(self.image)
        ranges = [image.entry(image.find(n))[:2] for n in ('a_b.metallib', 'b_b.metallib', 'c_b.metallib', 'd_b.metallib')]
        self.assertEqual(len(set(ranges)), 1, 'entries sharing a key must share one payload range')
        self.assertNotEqual(image.entry(image.find('e_b.metallib'))[:2], ranges[0])
        self.assertNotEqual(image.entry(image.find('f_b.metallib'))[:2], ranges[0])
        self.assertEqual(calls, ['a_b.metallib'], 'only the first sorted entry for a share key is loaded')
        self.assertEqual([(d, t) for d, t, _ in events], [(i, len(files)) for i in range(len(files) + 1)])
        for name in files:
            want = b'unique' if name == 'f_b.metallib' else payload
            self.assertEqual((romfs.Path(self.image) / name).read_bytes(), want)

    def test_empty_shared_files_and_failed_first_entry(self):
        shader_lib.write({'a': b'', 'b': b''}, str(self.image), share={'a': 1, 'b': 1})
        image = romfs.Image(self.image)
        self.assertEqual(image.entry(image.find('a'))[:2], image.entry(image.find('b'))[:2])
        self.assertEqual((romfs.Path(self.image) / 'b').read_bytes(), b'')
        before = self.image.read_bytes()
        def boom(): raise OSError(errno.EIO, 'read failed')
        with self.assertRaises(OSError):
            shader_lib.write({'a': boom, 'b': lambda: b'never'}, str(self.image), share={'a': 1, 'b': 1})
        self.assertEqual(self.image.read_bytes(), before)

    def test_pack_preserves_sharing_across_partial_update_and_leave_out(self):
        shader_lib.write(
            {'a_b.metallib': b'same', 'b_b.metallib': b'same', 'c_b.metallib': b'same', 'k_bp.metallib': b'pull', 'inputs.tsv': b'game\t0100\ninputs\tabc\n'},
            str(self.image),
            share={'a_b.metallib': 1, 'b_b.metallib': 1, 'c_b.metallib': 1},
        )
        self.folder(stamp=False, a_b__metallib=b'updated')
        self.assertTrue(self.pack())
        image = romfs.Image(self.image)
        self.assertEqual(image.entry(image.find('b_b.metallib'))[:2], image.entry(image.find('c_b.metallib'))[:2])
        self.assertNotEqual(image.entry(image.find('a_b.metallib'))[:2], image.entry(image.find('b_b.metallib'))[:2])
        self.assertEqual((romfs.Path(self.image) / 'a_b.metallib').read_bytes(), b'updated')
        self.assertEqual((romfs.Path(self.image) / 'b_b.metallib').read_bytes(), b'same')
        self.assertTrue(shader_lib.pack(self.lib, self.said.append, leave_out=True))
        image = romfs.Image(self.image)
        self.assertEqual(image.entry(image.find('b_b.metallib'))[:2], image.entry(image.find('c_b.metallib'))[:2])
        self.assertEqual((romfs.Path(self.image) / 'c_b.metallib').read_bytes(), b'same')

    def test_subset_preserves_shared_payloads_without_rereading_duplicates(self):
        payload = b'shared' * 500
        shader_lib.write(
            {'a_b.metallib': payload, 'b_b.metallib': payload, 'c_b.metal': payload, 'inputs.tsv': b'game\t0100\ninputs\tabc\n'},
            str(self.image),
            share={'a_b.metallib': 1, 'b_b.metallib': 1, 'c_b.metal': 1},
        )
        app = self.root / 'app.img'
        real_open = romfs.open
        opened = []
        def count_open(path, *args, **kwargs):
            opened.append(os.path.basename(str(path)))
            return real_open(path, *args, **kwargs)
        with mock.patch.object(shader_lib.romfs, 'open', side_effect=count_open):
            self.assertEqual(shader_lib.subset(self.lib, app), 3)
        self.assertEqual(opened.count('b_b.metallib'), 0, 'duplicate entry in source image must not be opened again')
        self.assertEqual(opened.count('a_b.metallib'), 1)
        image = romfs.Image(app)
        self.assertEqual(image.entry(image.find('a_b.metallib'))[:2], image.entry(image.find('b_b.metallib'))[:2])
        self.assertEqual((romfs.Path(app) / 'b_b.metallib').read_bytes(), payload)


class HostReader(Base):
    def reader(self):
        exe = self.root / 'msl_read'; src = self.root / 'msl_read.c'; src.write_text(READER)
        subprocess.run(['clang', '-Wall', '-Werror', '-Wno-unused-function', '-I', str(REPO / 'dm/gfx/nvn'), '-o', exe, src], check=True)
        return exe

    def test_the_host_finds_every_file_in_place(self):
        names = {f'{i:016x}_b.metallib': bytes([i % 251]) * (i * 37 + 1) for i in range(300)}
        names['z'] = b'short name'; names['zz'] = b'longer name'
        self.lib.mkdir()
        for n, d in names.items(): (self.lib / n).write_bytes(d)
        self.pack()
        asked = sorted(names) + ['absent', 'zzz', '0', 'y']
        got = subprocess.run([self.reader(), self.image, *asked], capture_output=True, check=True).stdout
        want = b''.join(names[n] + b'\n' if n in names else b'missing\n' for n in asked)
        self.assertEqual(got, want)

    def test_the_host_maps_shared_payloads_by_every_name(self):
        names = {f'{i:016x}_b.metallib': b'shared shader' for i in range(12)}
        shader_lib.write(names, str(self.image), share={n: 'shared' for n in names})
        image = romfs.Image(self.image)
        self.assertEqual(len({image.entry(image.find(n))[:2] for n in names}), 1)
        got = subprocess.run([self.reader(), self.image, *names], capture_output=True, check=True).stdout
        self.assertEqual(got, b'shared shader\n' * len(names))

    def test_the_host_refuses_a_cut_copy(self):
        self.folder(k_b__metallib=b'lib' * 100); self.pack()
        data = self.image.read_bytes(); self.image.write_bytes(data[:len(data) // 2])
        r = subprocess.run([self.reader(), self.image, 'k_b.metallib'], capture_output=True)
        self.assertEqual((r.returncode, r.stdout), (1, b'refused\n'))

    def test_an_empty_file_is_not_a_library(self):
        self.lib.mkdir(); (self.lib / 'e_b.metallib').write_bytes(b''); self.pack()
        self.assertEqual(subprocess.run([self.reader(), self.image, 'e_b.metallib'], capture_output=True).stdout, b'unmapped 22\n')


if __name__ == '__main__': unittest.main()
