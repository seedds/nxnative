#!/usr/bin/env python3
"""Shared memory byte and half-word stores as Metal byte stores (dmsh_rewrite.py shared_store_msl): the rewrite of the translator's helpers dmsh_shared_store8_<id> and
dmsh_shared_store16_<id> as spirv-cross writes them, its refusals, the device form tg_spill gives a kernel past Metal's 32 KB, and on this Mac's GPU 64 threads storing
neighbouring bytes and half-words into one array (every word as the stores leave it) and all of them storing one byte (one of their values, never a mix). With DMSH_TEST_XLATE (the
translator), a task or mesh shader's index byte through the translator and the rewrite."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # tg_spill's slot and limits

from dmsh_rewrite import shared_store_msl, tg_spill   # noqa: E402

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>
#include <metal_atomic>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];

    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    constexpr const thread T& operator [] (size_t pos) const thread { return elements[pos]; }
    device T& operator [] (size_t pos) device { return elements[pos]; }
    constexpr const device T& operator [] (size_t pos) const device { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup { return elements[pos]; }
};
'''
# The helpers as spirv-cross writes them (the translator's output for a mesh shader's index byte, 0x927 mode 0x80): byte offset, value, the shared memory array
def helper(bits, n=64, fid=0):
    return f'''
static inline __attribute__((always_inline))
void dmsh_shared_store{bits}_{fid}(thread const int& _37, thread const int& _38, threadgroup spvUnsafeArray<uint, {n}>& _18)
{{
    uint _42 = uint(int(uint(_37) >> uint(2)));
    int _44 = _37 & 3;
    int _46 = _44 << 3;
    bool _50;
    do
    {{
        float _53 = as_type<float>(_18[int(_42)]);
        int _55 = int(insert_bits(as_type<uint>(_53), uint(_38), uint(_46), {bits}u));
        uint _88 = as_type<uint>(_53);
        uint _93;
        do
        {{
            _93 = _88;
        }} while (!atomic_compare_exchange_weak_explicit((threadgroup atomic_uint*)&_18[int(_42)], &_93, uint(_55), memory_order_relaxed, memory_order_relaxed) && _93 == _88);
        uint _57 = _93;
        _50 = int(_57) != as_type<int>(_53);
    }} while (_50);
}}
'''
# 64 threads: byte t = 7t + 3 (words 0..15), the half-word at 64 + 2t = 0x101 t + 0x55 (words 16..47), and every thread 1 << (t % 8) into byte 192 (word 48), read back after a barrier
KERNEL = '''
kernel void check(device uint *out [[buffer(0)]], uint t [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 64> mem;
    mem[t] = 0u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    int o8 = int(t), v8 = int(t * 7u + 3u), o16 = int(64u + t * 2u), v16 = int(t * 0x101u + 0x55u), os = 192, vs = int(1u << (t % 8u));
    dmsh_shared_store8_0(o8, v8, mem);
    dmsh_shared_store16_1(o16, v16, mem);
    dmsh_shared_store8_0(os, vs, mem);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    out[t] = mem[t];
}
'''
RUNNER = r'''
#import <Metal/Metal.h>
#include <stdio.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 2; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&err];
        if (!ps) { printf("%s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLBuffer> out = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        for (int run = 0; run < 64 && !bad; run++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
            [en setComputePipelineState:ps]; [en setBuffer:out offset:0 atIndex:0];
            [en dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
            const uint32_t *w = out.contents; const uint8_t *b = out.contents;
            for (int t = 0; t < 64; t++) if (b[t] != (uint8_t)(t * 7 + 3)) { printf("byte %d: %#x\n", t, b[t]); bad = 1; }
            for (int t = 0; t < 64; t++) { uint16_t h = b[64 + 2 * t] | b[65 + 2 * t] << 8; if (h != (uint16_t)(t * 0x101 + 0x55)) { printf("half-word %d: %#x\n", t, h); bad = 1; } }
            if (__builtin_popcount(b[192]) != 1 || w[48] >> 8) { printf("shared byte: %#x (word %#x), not one of the values\n", b[192], w[48]); bad = 1; }
            for (int i = 49; i < 64; i++) if (w[i]) { printf("word %d: %#x\n", i, w[i]); bad = 1; }
        }
        return bad;
    }
}
'''


class Rewrite(unittest.TestCase):
    def test_byte_store(self):
        out = shared_store_msl(HEAD + helper(8) + KERNEL)
        body = out[out.index('void dmsh_shared_store8_0('):out.index('kernel void check')]
        self.assertIn('{\n    ((threadgroup uchar*)&_18[0])[uint(_37)] = uchar(uint(_38));\n}', body)
        self.assertNotIn('atomic', body)
        self.assertNotIn('dm_ushort_alias', out)
        self.assertEqual(out.count('static inline __attribute__((always_inline))'), 1)   # spirv-cross's line before it stays, alone

    def test_half_word_store(self):
        out = shared_store_msl(HEAD + helper(16, fid=1))
        self.assertIn('((threadgroup dm_ushort_alias*)&_18[0])[uint(_37) >> 1u] = ushort(uint(_38));', out)
        self.assertEqual(out.count('typedef ushort __attribute__((may_alias)) dm_ushort_alias;'), 1)
        self.assertLess(out.index('dm_ushort_alias;'), out.index('struct spvUnsafeArray'))   # declared at the top, after using namespace metal

    def test_device_array(self):   # a kernel whose shared memory tg_spill already moved
        out = shared_store_msl(HEAD + helper(8).replace('threadgroup spvUnsafeArray', 'device spvUnsafeArray'))
        self.assertIn('((device uchar*)&_18[0])[uint(_37)] = uchar(uint(_38));', out)

    def test_other_shaders_unchanged(self):
        plain = HEAD + '\nkernel void check(device uint *out [[buffer(0)]], uint t [[thread_index_in_threadgroup]])\n{\n    out[t] = t;\n}\n'
        self.assertEqual(shared_store_msl(plain), plain)

    def test_unexpected_signature_refused(self):
        with self.assertRaises(ValueError):
            shared_store_msl(HEAD + helper(8).replace('thread const int& _38, ', ''))
        with self.assertRaises(ValueError):
            shared_store_msl(HEAD + helper(8).replace('\nvoid dmsh_shared_store8_0(', '\nint dmsh_shared_store8_0('))

    def test_spilled_kernel_stores_into_device_memory(self):   # tg_spill after the rewrite: the cast follows the array to device memory
        big = abi.TG_MAX // 4 + 1024
        src = HEAD + helper(8, big) + f'''
kernel void main0(device uint *out [[buffer(0)]], uint t [[thread_index_in_threadgroup]])
{{
    threadgroup spvUnsafeArray<uint, {big}> mem;
    int o = int(t), v = 1;
    dmsh_shared_store8_0(o, v, mem);
    out[t] = mem[t];
}}
'''
        out, total = tg_spill(shared_store_msl(src))
        self.assertGreater(total, 0)
        self.assertIn('((device uchar*)&_18[0])[uint(_37)] = uchar(uint(_38));', out)
        if shutil.which('xcrun'):
            with tempfile.TemporaryDirectory() as d:
                (Path(d) / 's.metal').write_text(out)
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(Path(d) / 's.metal'), '-o', str(Path(d) / 's.air')], check=True)


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class OnGpu(unittest.TestCase):
    def test_stores_on_the_gpu(self):
        msl = shared_store_msl(HEAD + helper(8) + helper(16, fid=1) + KERNEL)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 's.metal').write_text(msl)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(d / 's.metal'), '-o', str(d / 's.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 's.air'), '-o', str(d / 's.metallib')], check=True)
            (d / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
            r = subprocess.run([str(d / 'run'), str(d / 's.metallib')], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Translated(unittest.TestCase):
    def test_index_byte_through_the_translator(self):   # 0x927 [R5 + 4], R6, mode 0x80 in a task or mesh shader's stage memory (test_dmsh_ampere_mem.StageMemory)
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import test_dmsh_ampere_mem as mem
        case = mem.StageMemory('test_a_byte_goes_through_its_word_atomically')
        src = case.msl(case.INDEX)
        self.assertRegex(src, r'\nvoid dmsh_shared_store8_\d+\(')
        out = shared_store_msl(src)
        self.assertRegex(out, r'\(\(threadgroup uchar\*\)&_\d+\[0\]\)\[uint\(_\d+\)\] = uchar\(uint\(_\d+\)\);')
        self.assertNotIn('atomic_compare_exchange', out)

    def test_payload_halfword_through_the_translator(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import test_dmsh_ampere_mem as mem
        case = mem.StageMemory('test_subword_payload_stores_use_their_width')
        write = (case.PAYLOAD[0] & ~0xff00 | 0xb400, case.PAYLOAD[1])
        src = case.msl(write)
        self.assertRegex(src, r'\nvoid dmsh_shared_store16_\d+\(')
        out = shared_store_msl(src)
        self.assertRegex(out, r'\(\(threadgroup dm_ushort_alias\*\)&_\d+\[0\]\)\[uint\(_\d+\) >> 1u\] = ushort\(uint\(_\d+\)\);')
        self.assertNotIn('atomic_compare_exchange', out)


if __name__ == '__main__':
    unittest.main()
