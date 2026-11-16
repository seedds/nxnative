#!/usr/bin/env python3
# Compressed shader library images (dm/tools/imgz.py, shader_lib.py compress/decompress, romfs.py's decode, the host's dm/gfx/nvn/msl_imgz.h).
import os, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
import imgz, romfs, shader_lib

METALLIB = b'MTLB' + bytes(range(256)) * 64 + b'render_target_array_index' + b'\0' * 4000
MSL = (b'#include <metal_stdlib>\nusing namespace metal;\n' + b'float4 f(float4 x) { return x * 2.0; }\n' * 400)
JSON = b'{"chain": "0123456789ab", "bindings": [' + b'{"slot": 1, "kind": "buffer"},' * 100 + b'{}]}'

DECODER = r'''
#include <stdio.h>
#include <errno.h>
#include "msl_image.h"
#include "msl_imgz.h"
// imgz_read <image> <name>...: each name's original bytes (decoded when compressed), or what refused it
int main(int argc, char **argv) {
    romfs_img *m = romfs_image_open(argv[1]);
    if (!m) { puts("refused"); return 1; }
    for (int i = 2; i < argc; i++) {
        const img_ent *x = msl_image_find(m, argv[i]); msl_map f;
        if (!x) { puts("missing"); continue; }
        if (!msl_image_map(m, x, &f)) { printf("unmapped %d\n", errno); continue; }
        if (msl_imgz_is(f.data, f.size)) {
            size_t n; const char *why; void *o = msl_imgz_decode(f.data, f.size, &n, &why);
            if (!o) printf("bad: %s", why); else { fwrite(o, 1, n, stdout); free(o); }
        } else fwrite(f.data, 1, f.size, stdout);
        putchar('\n'); munmap(f.base, f.base_len);
    }
    romfs_image_close(m);
    return 0;
}
'''


class Codec(unittest.TestCase):
    def test_round_trip_every_algorithm(self):
        for c in imgz.ALG:
            z = imgz.encode(MSL, c)
            self.assertTrue(imgz.is_dmz(z), c); self.assertLess(len(z), len(MSL)); self.assertEqual(imgz.decode(z), MSL)

    def test_small_or_incompressible_stays_plain(self):
        self.assertEqual(imgz.encode(b'MTLB', 'lzbitmap'), b'MTLB')
        rnd = os.urandom(5000); self.assertEqual(imgz.encode(rnd, 'lzbitmap'), rnd)
        self.assertEqual(imgz.encode(b'', 'lzfse'), b'')

    def test_never_twice(self):
        z = imgz.encode(MSL, 'lzfse'); self.assertEqual(imgz.encode(z, 'lzfse'), z)

    def test_plain_decodes_to_itself(self): self.assertEqual(imgz.decode(METALLIB), METALLIB)

    def test_damaged_refused(self):
        z = bytearray(imgz.encode(MSL, 'lzfse'))
        with self.assertRaises(ValueError): imgz.decode(bytes(z[:len(z) // 2]))   # cut stream
        bad = bytearray(z); bad[8:16] = struct.pack('<Q', len(MSL) + 1)
        with self.assertRaises(ValueError): imgz.decode(bytes(bad))               # size that the stream does not make
        bad = bytearray(z); bad[4:8] = struct.pack('<I', 0x999)
        with self.assertRaises(ValueError): imgz.decode(bytes(bad))               # unknown algorithm

    def test_codec_choice(self):
        self.assertEqual(imgz.codec_for('k_bx.metallib'), 'lzbitmap'); self.assertEqual(imgz.codec_for('k_b.metal'), 'lzfse')
        self.assertEqual(imgz.codec_for('k_b.json'), 'lzfse'); self.assertIsNone(imgz.codec_for('inputs.tsv'))


class Image(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name); self.lib = self.root / 'b-0123456789ab'; self.image = self.root / 'b-0123456789ab.img'
        self.lib.mkdir()
        self.files = {'k_b.metallib': METALLIB, 'k_b.metal': MSL, 'k_b.json': JSON, 'k_bx.metallib': METALLIB[::-1] + METALLIB, 'inputs.tsv': b'game\t0100\ninputs\tabc\n', 'tiny_b.metallib': b'MTLB'}
        for n, d in self.files.items(): (self.lib / n).write_bytes(d)
        self.assertTrue(shader_lib.pack(self.lib, print))
        self.plain = self.image.read_bytes()

    def raw(self, name):   # the stored bytes, not decoded
        img = romfs.Image(str(self.image)); k = img.find(name); a, b = img.entry(k)[:2]
        with open(self.image, 'rb') as f: f.seek(a); return f.read(b)

    def test_compress_in_place_and_tools_read_the_originals(self):
        before, after = shader_lib.compress(self.lib, jobs=3)
        self.assertLess(after, before); self.assertLess(self.image.stat().st_size, len(self.plain))
        self.assertTrue(imgz.is_dmz(self.raw('k_b.metal'))); self.assertTrue(imgz.is_dmz(self.raw('k_b.metallib')))
        self.assertEqual(self.raw('inputs.tsv'), self.files['inputs.tsv']); self.assertEqual(self.raw('tiny_b.metallib'), b'MTLB')
        v = shader_lib.view(self.lib)
        for n, d in self.files.items():
            self.assertEqual((v / n).read_bytes(), d, n)
            with romfs.open(os.path.join(str(v), n), 'rb') as f: self.assertEqual(f.read(), d)
        self.assertEqual((v / 'k_b.metal').read_text(), MSL.decode())
        self.assertEqual(shader_lib.read_stamp(self.lib).get('inputs'), 'abc')
        self.assertEqual(sorted(os.listdir(self.root)), [self.image.name], 'no .part left')

    def test_decompress_gives_the_same_image_back(self):
        out = self.root / 'z.img'; shader_lib.compress(self.lib, out)
        self.assertEqual(self.image.read_bytes(), self.plain, 'the source image is not touched when out is given')
        os.replace(out, self.image); shader_lib.decompress(self.lib)
        self.assertEqual(self.image.read_bytes(), self.plain)

    def test_sizes_are_the_originals(self):
        shader_lib.compress(self.lib)
        v = shader_lib.view(self.lib)
        for n, d in self.files.items(): self.assertEqual((v / n).stat().st_size, len(d), n)
        self.assertTrue(shader_lib.is_compressed(self.lib))

    def test_any_image_name_decodes(self):   # not only b-<chain>.img: a copy under another name reads the same
        out = self.root / 'z.img'; shader_lib.compress(self.lib, out)
        self.assertEqual(romfs.Path(str(out)).joinpath('k_b.metal').read_bytes(), MSL)

    def test_a_partial_build_keeps_the_image_compressed(self):   # pack (and dmsh_build publish_library through write) copies image files as stored
        shader_lib.compress(self.lib)
        self.lib.mkdir(); (self.lib / 'new_b.metallib').write_bytes(METALLIB)
        self.assertTrue(shader_lib.pack(self.lib, print))
        self.assertTrue(imgz.is_dmz(self.raw('k_b.metal'))); self.assertEqual(self.raw('new_b.metallib'), METALLIB)
        self.assertEqual((shader_lib.view(self.lib) / 'k_b.metallib').read_bytes(), METALLIB)

    def test_subset_and_expand_are_plain(self):   # an app image (pack_ipa / pack_app) and a folder hold the original bytes
        shader_lib.compress(self.lib)
        sub = self.root / 'app' / 'b-0123456789ab.img'; sub.parent.mkdir(); shader_lib.subset(self.lib, sub)
        self.assertFalse(shader_lib.is_compressed(sub)); self.assertEqual(romfs.Path(str(sub)).joinpath('k_b.metallib').read_bytes(), METALLIB)
        shader_lib.expand(self.lib)
        for n, d in self.files.items(): self.assertEqual((self.lib / n).read_bytes(), d, n)

    def test_damaged_file_is_an_os_error_and_a_failed_code_leaves_no_part(self):
        shader_lib.compress(self.lib)
        img = romfs.Image(str(self.image)); a, b = img.entry(img.find('k_b.metal'))[:2]
        with open(self.image, 'r+b') as f: f.seek(a + 8); f.write(struct.pack('<Q', len(MSL) - 3))
        romfs._image.cache_clear()
        with self.assertRaises(OSError): (shader_lib.view(self.lib) / 'k_b.metal').read_bytes()
        with self.assertRaises(ValueError): shader_lib.decompress(self.lib)
        self.assertFalse(Path(str(self.image) + '.part').exists())

    def test_a_damaged_header_is_an_os_error(self):   # DMZ1 with an unknown algorithm: refused as the host refuses it, never read as the stored bytes
        shader_lib.compress(self.lib)
        img = romfs.Image(str(self.image)); a = img.entry(img.find('k_b.metallib'))[0]
        with open(self.image, 'r+b') as f: f.seek(a + 4); f.write(struct.pack('<I', 0x999))
        romfs._image.cache_clear()
        v = shader_lib.view(self.lib)
        with self.assertRaises(OSError): (v / 'k_b.metallib').read_bytes()
        self.assertTrue((v / 'k_b.metallib').is_file())
        self.assertTrue(shader_lib.is_compressed(self.lib))

    def test_a_game_romfs_is_read_as_stored(self):   # only a shader library image (flat, with a Metal library) decodes: a game file that starts with DMZ1 is its own bytes
        import romfs_image
        src = self.root / 'game'; (src / 'data').mkdir(parents=True)
        z = imgz.encode(MSL, 'lzfse'); (src / 'data' / 'x.bin').write_bytes(z); (src / 'y.metallib').write_bytes(z)
        image = self.root / 'RomFS.img'; romfs_image.pack(src, image)
        p = romfs.Path(str(image))
        self.assertEqual((p / 'data' / 'x.bin').read_bytes(), z); self.assertEqual((p / 'y.metallib').read_bytes(), z)
        self.assertEqual((p / 'y.metallib').stat().st_size, len(z))
        before = image.read_bytes()
        with self.assertRaises(ValueError): shader_lib.compress(str(image)[:-4])   # not a shader library: its folders would be lost
        self.assertEqual(image.read_bytes(), before)

    def test_detritus_does_not_block(self):
        self.lib.mkdir(); (self.lib / '.DS_Store').write_bytes(b'x')
        shader_lib.compress(self.lib)
        self.assertTrue(imgz.is_dmz(self.raw('k_b.metal')))

    def test_a_folder_over_the_image_is_refused(self):
        (self.lib).mkdir(); (self.lib / 'new_b.metallib').write_bytes(b'x')
        with self.assertRaises(ValueError): shader_lib.compress(self.lib)

    def test_mixed_image(self):   # compressed and plain files side by side (a partial build packed over a compressed image): the tools and the host reader take both
        shader_lib.compress(self.lib)
        self.lib.mkdir(); (self.lib / 'new_b.metallib').write_bytes(METALLIB); (self.lib / 'new_b.metal').write_bytes(MSL)
        self.assertTrue(shader_lib.pack(self.lib, print))
        self.assertTrue(imgz.is_dmz(self.raw('k_b.metallib'))); self.assertEqual(self.raw('new_b.metallib'), METALLIB); self.assertEqual(self.raw('new_b.metal'), MSL)
        v = shader_lib.view(self.lib)
        for n, d in dict(self.files, **{'new_b.metallib': METALLIB, 'new_b.metal': MSL}).items():
            self.assertEqual((v / n).read_bytes(), d, n); self.assertEqual(romfs.stat(os.path.join(str(v), n)).st_size, len(d), n)
        exe = self.reader()
        out = subprocess.run([exe, str(self.image), 'k_b.metallib', 'new_b.metallib'], capture_output=True, check=True).stdout
        self.assertEqual(out, METALLIB + b'\n' + METALLIB + b'\n')

    def reader(self):   # the host's reader (msl_image.h, msl_imgz.h) as a small program
        t = tempfile.mkdtemp(); self.addCleanup(__import__('shutil').rmtree, t, True)
        src, exe = os.path.join(t, 'r.c'), os.path.join(t, 'r')
        Path(src).write_text(DECODER)
        subprocess.run(['clang', '-Wall', '-Werror', '-Wno-unused-function', '-I', str(REPO / 'dm/gfx/nvn'), '-o', exe, src], check=True)
        return exe

    def test_the_host_reader_decodes(self):
        shader_lib.compress(self.lib)
        exe = self.reader()
        names = ['k_b.metallib', 'k_bx.metallib', 'k_b.metal', 'k_b.json', 'inputs.tsv', 'tiny_b.metallib']
        out = subprocess.run([exe, str(self.image)] + names, capture_output=True, check=True).stdout
        self.assertEqual(out, b''.join(self.files[n] + b'\n' for n in names))
        # a damaged stream: the size check refuses it (no wrong bytes reach Metal)
        img = romfs.Image(str(self.image)); a, b = img.entry(img.find('k_b.metallib'))[:2]
        with open(self.image, 'r+b') as f: f.seek(a + 8); f.write(struct.pack('<Q', len(METALLIB) - 7))
        out = subprocess.run([exe, str(self.image), 'k_b.metallib'], capture_output=True, check=True).stdout
        self.assertTrue(out.startswith(b'bad: '), out[:80])


if __name__ == '__main__': unittest.main()
