"""Indexed AST.P: translate a synthetic hull and check every patch word on Metal."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_geometry, dmsh_tess  # noqa: E402


def instruction(op, rd=0, ra=255, rb=0, address=0, high=0):
    return struct.pack('<QQ', op | 7 << 12 | rd << 16 | ra << 24 | rb << 32 | address << 40,
                       0x000fe20000000000 | high)


def hull(immediate, components):
    header = bytearray(0x80)
    struct.pack_into('<3I', header, 0, 1 | 4 << 5 | 2 << 10, 128 << 24, 1 << 24)
    # A position input and output keep this a complete hull through the production lowering.
    header[0x17] = header[0x35] = 0xf0
    # c4 is bank 4: its encoding in the immediate cbuf operand is bit 56.
    code = [struct.pack('<QQ', 0x0100000000077a02, 0x000fe20000000f00)]  # MOV R7, c4[0]
    for i in range(components):
        code.append(struct.pack('<QQ', 0x0100000000007a02 | (2 + i) << 16 | (i + 1) << 40,
                                0x000fe20000000f00))
    code += [instruction(0x321, rd=16, rb=255, address=0x70, high=3 << 10),
             instruction(0x322, rb=16, address=0x70, high=255 | 3 << 10),
             instruction(0x322, ra=7, rb=2, address=immediate, high=255 | (components - 1) << 10 | 1 << 12),
             struct.pack('<QQ', 0x000000000000794d, 0x000fea0003800000)]
    return bytes(header) + b''.join(code)


GPU = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <string.h>
int main(int argc, const char **argv) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 77;
    NSError *error = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    id<MTLFunction> f = [lib newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> p = f ? [d newComputePipelineStateWithFunction:f error:&error] : nil;
    if (!p) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    unsigned immediate = (unsigned)strtoul(argv[2], NULL, 0), count = (unsigned)atoi(argv[3]);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLArgumentEncoder> ae = [f newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [d newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> cb = [d newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0]; [ae setBuffer:cb offset:0 atIndex:0];
    id<MTLBuffer> cp = [d newBufferWithLength:33*16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> patch = [d newBufferWithLength:32*16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> factors = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> indices = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> capture = [d newBufferWithLength:144*4 options:MTLResourceStorageModeShared];
    memset(indices.contents, 0, indices.length); memset(capture.contents, 0, capture.length);
    uint32_t indirect[] = {1,0,0,0};
    for (unsigned trial = 0; trial < 131; trial++) {
        // Every valid word, the end, and two wrapping/negative offsets. A vector may cross a map boundary.
        uint32_t address = trial < 129 ? trial*4 : trial == 129 ? 0xfffffffcu : 0xffffffe8u;
        ((uint32_t *)cb.contents)[0] = address;
        for (unsigned i=0; i<count; i++) ((float *)cb.contents)[i+1] = 3.5f + i;
        for (unsigned i=0; i<128; i++) ((float *)patch.contents)[i] = -99.0f;
        for (unsigned i=0; i<8; i++) ((_Float16 *)factors.contents)[i] = (_Float16)-99.0f;
        id<MTLCommandBuffer> cmd = [q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
        [e setComputePipelineState:p]; [e setBuffer:args offset:0 atIndex:0];
        [e useResource:cb usage:MTLResourceUsageRead];
        [e setBuffer:indices offset:0 atIndex:24]; [e setBuffer:capture offset:0 atIndex:25];
        [e setBuffer:factors offset:0 atIndex:26]; [e setBuffer:patch offset:0 atIndex:27];
        [e setBuffer:cp offset:0 atIndex:28]; [e setBytes:indirect length:sizeof indirect atIndex:29];
        [e setThreadgroupMemoryLength:16 atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [e endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
        if (cmd.error) { fprintf(stderr, "%s\n", cmd.error.description.UTF8String); return 1; }
        for (unsigned word=0; word<134; word++) {
            float expected = -99.0f;
            for (unsigned i=0; i<count; i++) {
                uint32_t at = address + immediate + i*4;
                if (at < 0x200 && at/4 == word) expected = 3.5f + i;
            }
            float got = word < 6 ? (float)((_Float16 *)factors.contents)[word] : ((float *)patch.contents)[word-6];
            if (got != expected) {
                fprintf(stderr, "address %#x + %#x, count %u, word %u: %g != %g\n", address, immediate, count, word, got, expected); return 1;
            }
        }
    }
    return 0;
} }
'''


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class IndexedPatchStore(unittest.TestCase):
    def build(self, work, immediate, components):
        (work / 'shader.bin').write_bytes(hull(immediate, components))
        (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='hs', arch='sm86', code=str(work / 'shader.bin'),
                                                        tex={}, img={}, attr={}, fout={}, debug=True)]))
        r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), str(work)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((work / 'k.spv').is_file(), r.stdout + r.stderr)
        dmsh_geometry.flatten_sets(work / 'k.spv')
        r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), str(work / 'k.spv'), '--msl', '--msl-version', '20300',
                            '--msl-argument-buffers'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        source, _ = dmsh_tess.hull(r.stdout, work / 'k.spv')
        (work / 'shader.metal').write_text(source)

    def test_indexed_patch_store_lowers_to_a_metal_hull(self):
        for immediate, components in [(0, 1), (0x10, 4), (0x1f8, 4)]:
            with self.subTest(immediate=immediate, components=components), tempfile.TemporaryDirectory() as d:
                self.build(Path(d), immediate, components)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') == '1' and shutil.which('xcrun'), 'set DMSH_TEST_GPU=1 on a Mac')
    def test_every_patch_word_and_crossing_vector_on_the_gpu(self):
        with tempfile.TemporaryDirectory() as d:
            work = Path(d); (work / 'check.m').write_text(GPU)
            r = subprocess.run(['clang', '-fobjc-arc', str(work / 'check.m'), '-framework', 'Foundation', '-framework', 'Metal',
                                '-o', str(work / 'check')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            for immediate, components in [(0, 1), (0x10, 4), (0x1f8, 4)]:
                with self.subTest(immediate=immediate, components=components):
                    self.build(work, immediate, components)
                    r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=metal4.0', '-fmodules-cache-path=' + str(work / 'modules'), '-c', str(work / 'shader.metal'),
                                        '-o', str(work / 'shader.air')], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(work / 'shader.air'), '-o', str(work / 'shader.metallib')], check=True)
                    r = subprocess.run([str(work / 'check'), str(work / 'shader.metallib'), str(immediate), str(components)], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
