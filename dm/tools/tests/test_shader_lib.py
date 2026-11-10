#!/usr/bin/env python3
# A game's shader libraries as one file (dm/tools/shader_lib.py): the folder and image states, and a read by the host's C reader (dm/gfx/nvn/msl_image.h, dm/host/romfs_image.h).
import io, os, subprocess, sys, tempfile, unittest
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
    def test_mixed_sources_share_bytes_without_losing_names(self):
        payload = b'same compiled shader' * 100
        source = self.root / 'source.metallib'; source.write_bytes(payload)
        old = self.root / 'old.img'; shader_lib.write({'old': payload}, str(old))
        calls = []
        def staged(): calls.append(1); return payload
        files = {'a_b.metallib': payload, 'b_b.metallib': source, 'c_b.metallib': staged,
                 'd_b.metallib': str(romfs.Path(old) / 'old'), 'e_b.metallib': payload + b'different'}
        shader_lib.write(files, str(self.image))
        image = romfs.Image(self.image)
        entries = [image.entry(image.find(n))[:2] for n in list(files)[:4]]
        self.assertEqual(len(set(entries)), 1, 'identical files must share one payload in the image')
        self.assertNotEqual(image.entry(image.find('e_b.metallib'))[:2], entries[0])
        self.assertEqual(calls, [1], 'a staged output is fetched once')
        for name in files:
            self.assertEqual((romfs.Path(self.image) / name).read_bytes(), payload + (b'different' if name.startswith('e_') else b''))

    def test_duplicate_payloads_do_not_accumulate_in_the_temporary_image(self):
        payload = b'x' * (256 << 10)
        real_open, peak = open, [0]
        class Measured:
            def __init__(self, f): self.f = f
            def __enter__(self): self.f.__enter__(); return self
            def __exit__(self, *args): return self.f.__exit__(*args)
            def __getattr__(self, name): return getattr(self.f, name)
            def write(self, data):
                n = self.f.write(data); self.f.flush()
                peak[0] = max(peak[0], os.fstat(self.f.fileno()).st_size)
                return n
        def opened(path, mode='r', *args, **kwargs):
            f = real_open(path, mode, *args, **kwargs)
            return Measured(f) if str(path) == str(self.image) + '.part' and 'w' in mode else f
        files = {f'{i:016x}_b.metallib': payload for i in range(16)}
        with mock.patch.object(shader_lib.builtins, 'open', side_effect=opened): shader_lib.write(files, str(self.image))
        self.assertLessEqual(peak[0], len(payload) + 4096, 'duplicates must not consume temporary payload space')
        self.assertLess(self.image.stat().st_size, len(payload) + 4096)

    def test_digest_collisions_compare_every_byte_and_keep_distinct_ranges(self):
        first = b'x' * (8 << 20) + b'a'; other = first[:-1] + b'b'
        with mock.patch.object(shader_lib.hashlib, 'blake2b') as hashed:
            hashed.return_value.digest.return_value = b'forced collision'
            shader_lib.write({'a': first, 'b': other, 'c': first}, str(self.image))
        image = romfs.Image(self.image)
        self.assertNotEqual(image.entry(image.find('a'))[:2], image.entry(image.find('b'))[:2])
        self.assertEqual(image.entry(image.find('a'))[:2], image.entry(image.find('c'))[:2])
        self.assertEqual((romfs.Path(self.image) / 'b').read_bytes(), other)

    def test_streamed_paths_stay_bounded_and_are_not_reopened(self):
        payload = b'x' * (9 << 20)
        class Bounded(io.BytesIO):
            def read(self, n=-1):
                if not 0 < n <= 8 << 20: raise AssertionError('unbounded input read')
                return super().read(n)
        with mock.patch.object(shader_lib.romfs, 'open', return_value=Bounded(payload)) as opened:
            shader_lib.write({'large': self.root / 'large'}, str(self.image))
        self.assertEqual(opened.call_count, 1)
        self.assertEqual((romfs.Path(self.image) / 'large').read_bytes(), payload)

    def test_an_input_changed_between_passes_cannot_replace_a_complete_image(self):
        shader_lib.write({'old': b'published'}, str(self.image)); before = self.image.read_bytes()
        class Changing(io.BytesIO):
            def seek(self, offset, whence=0):
                if offset == 0 and whence == 0 and self.tell():
                    super().seek(0); self.write(b'new')
                return super().seek(offset, whence)
        with mock.patch.object(shader_lib.romfs, 'open', return_value=Changing(b'old')):
            with self.assertRaisesRegex(OSError, 'changed while writing'):
                shader_lib.write({'new': self.root / 'changing'}, str(self.image))
        self.assertEqual(self.image.read_bytes(), before)

    def test_a_duplicate_that_grows_between_passes_cannot_publish_its_prefix(self):
        for initial in (b'prefix', b''):
            with self.subTest(initial=initial):
                shader_lib.write({'old': b'published'}, str(self.image)); before = self.image.read_bytes()
                class Growing(io.BytesIO):
                    grew = False
                    def seek(self, offset, whence=0):
                        if offset == 0 and whence == 0 and not self.grew:
                            super().seek(0, 2); self.write(b' appended'); self.grew = True
                        return super().seek(offset, whence)
                with mock.patch.object(shader_lib.romfs, 'open', return_value=Growing(initial)):
                    with self.assertRaisesRegex(OSError, 'changed while writing'):
                        shader_lib.write({'a': initial, 'b': self.root / 'growing'}, str(self.image))
                self.assertEqual(self.image.read_bytes(), before)

    def test_a_partial_rebuild_can_replace_one_shared_file(self):
        self.folder(a_b__metallib=b'same', b_b__metallib=b'same'); self.pack()
        self.folder(stamp=False, a_b__metallib=b'new'); self.pack()
        self.assertEqual((shader_lib.view(self.lib) / 'a_b.metallib').read_bytes(), b'new')
        self.assertEqual((shader_lib.view(self.lib) / 'b_b.metallib').read_bytes(), b'same')

    def test_app_subset_preserves_shared_payloads(self):
        self.folder(a_b__metallib=b'same', b_b__metallib=b'same', a_b__metal=b'source'); self.pack()
        app = self.root / 'app.img'; shader_lib.subset(self.lib, app)
        image = romfs.Image(app)
        self.assertEqual(image.entry(image.find('a_b.metallib'))[:2], image.entry(image.find('b_b.metallib'))[:2])
        self.assertEqual((romfs.Path(app) / 'b_b.metallib').read_bytes(), b'same')


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
        shader_lib.write(names, str(self.image))
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
