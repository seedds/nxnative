"""Compute handles loaded through c0's uniform-address table must retain their uniform slot and word offsets. Synthetic Maxwell only: Civilization VI's four remaining compute
shaders used this path, and lost 382 buffer fetches. The optional GPU check executes the translated fetch; t_tbuf_handle checks production binding of its two uniform words."""
import json, os, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_maxwell_ftz import EXIT, PT, RZ, TOOLS, code, s2r_tid   # noqa: E402
from test_dmsh_storage_offset import iadd_c, ldg, mov_c   # noqa: E402
from test_dmsh_hull_cbe import mov32i, stg   # noqa: E402
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi, dmsh_chain, dmsh_geometry, dmsh_reflect   # noqa: E402
from dmsh_chain import MAC_METAL   # noqa: E402


def lop_or(rd, a, b): return 0x5c40 << 48 | PT << 48 | 1 << 41 | b << 20 | PT << 16 | a << 8 | rd
def lop_and(rd, a, b): return 0x5c40 << 48 | PT << 48 | b << 20 | PT << 16 | a << 8 | rd
def tld_b(rd, handle): return 0xdd << 56 | PT << 51 | 1 << 31 | handle << 20 | PT << 16 | RZ << 8 | rd   # TLD.B.LZ Rrd, RZ, Rhandle, 1D, x


def kernel(first=12, second=13, offset=24, reverse=False, single=False, dynamic=False, masked=False, base=abi.C0_CS_UBO):
    address = base + abi.C0_ROW * first
    body = [mov_c(2, 0, address), mov_c(3, 0, address + 4), ldg(4, 2)]
    if not single:
        address = base + abi.C0_ROW * second
        if dynamic:
            body += [s2r_tid(1), iadd_c(6, 1, 0, address, cc=True), iadd_c(7, RZ, 0, address + 4, x=True)]
        else:
            body += [mov_c(6, 0, address), mov_c(7, 0, address + 4)]
        body += [ldg(5, 6) | offset << 20]
        if masked:
            body += [mov32i(9, 0xfff00000), lop_and(4, 4, 9), mov32i(9, 0xfffff), lop_and(5, 5, 9)]
        body += [lop_or(8, 5 if reverse else 4, 4 if reverse else 5)]
    body += [tld_b(10, 4 if single else 8), mov_c(20, 0, abi.C0_SSBO_CS), mov_c(21, 0, abi.C0_SSBO_CS + 4), stg(20, 10), EXIT]
    return code(*body)


RUNNER = r'''#import <Metal/Metal.h>
#include <stdio.h>
int main(int ac, char **av) { @autoreleasepool {
    if (ac != 5) return 2;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[1])] error:&e];
    if (!l) { puts(e.description.UTF8String); return 2; }
    id<MTLFunction> f = [l newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> p = [d newComputePipelineStateWithFunction:f error:&e];
    if (!p) { puts(e.description.UTF8String); return 2; }
    id<MTLArgumentEncoder> a = [f newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [d newBufferWithLength:a.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bias = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLBuffer> data = [d newBufferWithLength:256 options:MTLResourceStorageModeShared];
    ((float *)data.contents)[0] = 37.5f;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Float width:64 resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    id<MTLTexture> tex = [data newTextureWithDescriptor:td offset:0 bytesPerRow:256];
    [a setArgumentBuffer:args offset:0]; [a setTexture:tex atIndex:atoi(av[2])];
    [a setBuffer:out offset:0 atIndex:atoi(av[3])]; [a setBuffer:bias offset:0 atIndex:atoi(av[4])];
    id<MTLCommandBuffer> c = [[d newCommandQueue] commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
    [ce setComputePipelineState:p]; [ce setBuffer:args offset:0 atIndex:0];
    [ce useResource:tex usage:MTLResourceUsageRead]; [ce useResource:out usage:MTLResourceUsageWrite]; [ce useResource:bias usage:MTLResourceUsageRead];
    [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
    if (c.error) { puts(c.error.description.UTF8String); return 2; }
    printf("%.1f\n", ((float *)out.contents)[0]); return 0;
} }
'''


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class ComputeUniformHandles(unittest.TestCase):
    def translate(self, work, stage='cs', **options):
        blob = kernel(**options)
        if stage == 'vs': blob = struct.pack('<I', 1 | 1 << 10) + bytes(0x4c) + blob
        (work / 'k.bin').write_bytes(blob)
        (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage=stage, code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[1, 1, 1], debug=True)]))
        r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), str(work)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        res = json.loads((work / 'k.res.json').read_text())
        dmsh_geometry.flatten_sets(work / 'k.spv')
        r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', dmsh_chain.MSL_VERSION, '--msl-argument-buffers', '--msl-texture-buffer-native', str(work / 'k.spv')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return res, r.stdout, dmsh_reflect.reflect('k', stage, r.stdout, res)

    def test_two_words_keep_their_slots_in_both_orders(self):
        for reverse in (False, True):
            for offset in (0, 16, 24, 32, 40):
                with self.subTest(reverse=reverse, offset=offset), tempfile.TemporaryDirectory() as d:
                    res, _, binds = self.translate(Path(d), reverse=reverse, offset=offset)
                    self.assertFalse(any('Failed' in n for n in res['notes']), res['notes'])
                    b, = [b for b in binds if b['kind'] == abi.KIND_TBUF_UBO]
                    self.assertEqual({(b['reg'], b['off']), (b['sreg'], b['soff'])}, {(12, 0), (13, offset)})
                    self.assertIn('tbx', b)

    def test_masked_words_keep_their_slots(self):
        for reverse in (False, True):
            with self.subTest(reverse=reverse), tempfile.TemporaryDirectory() as d:
                res, _, binds = self.translate(Path(d), reverse=reverse, masked=True)
                self.assertFalse(any('Failed' in n for n in res['notes']), res['notes'])
                b, = [b for b in binds if b['kind'] == abi.KIND_TBUF_UBO]
                self.assertEqual((b['reg'], b['off'], b['sreg'], b['soff']), (13, 24, 12, 0))

    def test_single_word(self):
        with tempfile.TemporaryDirectory() as d:
            res, _, binds = self.translate(Path(d), single=True, first=0)
            self.assertFalse(any('Failed' in n for n in res['notes']), res['notes'])
            b, = [b for b in binds if b['kind'] == abi.KIND_TBUF_UBO]
            self.assertEqual((b['reg'], b['off']), (0, 0)); self.assertNotIn('sreg', b)

    def test_other_storage_origins_are_not_uniform_handles(self):
        for base in (abi.C0_CS_UBO - abi.C0_ROW, abi.C0_CS_UBO + 4, abi.C0_SSBO_CS):
            with self.subTest(base=base), tempfile.TemporaryDirectory() as d:
                _, _, binds = self.translate(Path(d), single=True, first=0, base=base)
                self.assertFalse(any(b['kind'] == abi.KIND_TBUF_UBO for b in binds), binds)

    def test_reserved_slots_are_not_game_uniform_banks(self):
        with tempfile.TemporaryDirectory() as d:
            _, _, binds = self.translate(Path(d), single=True, first=abi.CBSIZE_SLOTS)
            self.assertFalse(any(b['kind'] == abi.KIND_TBUF_UBO for b in binds), binds)

    def test_dynamic_word_is_not_a_fixed_handle(self):
        with tempfile.TemporaryDirectory() as d:
            res, _, _ = self.translate(Path(d), dynamic=True)
            self.assertTrue(any('Failed' in n for n in res['notes']), res['notes'])

    def test_word_past_the_uniform_address_space_is_not_fixed(self):
        with tempfile.TemporaryDirectory() as d:
            res, _, _ = self.translate(Path(d), offset=65536)
            self.assertTrue(any('Failed' in n for n in res['notes']), res['notes'])

    def test_graphics_c0_is_not_the_compute_uniform_table(self):
        with tempfile.TemporaryDirectory() as d:
            _, _, binds = self.translate(Path(d), stage='vs', single=True)
            self.assertFalse(any(b['kind'] == abi.KIND_TBUF_UBO for b in binds), binds)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac with Xcode')
    def test_translated_fetch_on_gpu(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d); _, msl, binds = self.translate(work)
            (work / 'k.metal').write_text(msl); (work / 'r.m').write_text(RUNNER)
            subprocess.run(MAC_METAL + [str(work / 'k.metal'), '-o', str(work / 'k.air')], check=True, capture_output=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(work / 'k.air'), '-o', str(work / 'k.metallib')], check=True, capture_output=True)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(work / 'r.m'), '-o', str(work / 'r')], check=True, capture_output=True)
            ids = [str(next(b['arg_id'] for b in binds if b['kind'] == kind)) for kind in (abi.KIND_TBUF_UBO, abi.KIND_SBUF, abi.KIND_TBIAS)]
            r = subprocess.run([str(work / 'r'), str(work / 'k.metallib'), *ids], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, '37.5'), r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
