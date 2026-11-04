"""Check shared-memory spills beyond the portable threadgroup-memory limit.

A rewritten 48 KB kernel must give each workgroup independent storage and preserve
its barriers and atomics. Kernels within the limit must remain unchanged.
"""

import ast
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi  # noqa: E402

tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [
    n
    for n in tree.body
    if (isinstance(n, ast.FunctionDef) and n.name == 'tg_spill')
    or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'TG_ELEM' for t in n.targets))
]
scope = {'re': re, 'abi': abi}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
tg_spill = scope['tg_spill']

HEAD = """#include <metal_stdlib>
#include <simd/simd.h>

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
"""
# The shape spirv-cross gives Mario Kart World's DrivePassShaderDefault kernels (2a89b6d4df731b8d: 64 threads, a 12,288-word array, threadgroup_barrier on it in half of them):
# each thread fills 192 words, then after the barrier reads its neighbour's
KERNEL = """
static inline __attribute__((always_inline))
uint sum_of(threadgroup spvUnsafeArray<uint, %(n)d>& _9, thread const int& _10)
{
    uint s = 0u;
    for (int i = 0; i < 192; i++) {
        s += _9[_10 * 192 + i];
    }
    return s;
}

kernel void main0(device uint* out [[buffer(1)]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, %(n)d> _37;
    threadgroup uint _38;
    int t = int(gl_LocalInvocationID.x);
    uint g = gl_WorkGroupID.y * 16u + gl_WorkGroupID.x;
    for (int i = 0; i < 192; i++) {
        _37[t * 192 + i] = g * 100000u + uint(t) * 1000u + uint(i);
    }
    if (t == 0) {
        _38 = 0u;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint _40 = atomic_fetch_add_explicit((threadgroup atomic_uint*)&_37[0], 0u, memory_order_relaxed);
    int nb = (t + 1) & 63;
    out[g * 64u + uint(t)] = sum_of(_37, nb) + (_40 - _40);
}
"""
BIG, SMALL = KERNEL % {'n': 12288}, KERNEL % {'n': 4096}


class Rule(unittest.TestCase):
    def test_over_the_limit_goes_to_a_device_slice(self):
        out, slice_ = tg_spill(HEAD + BIG)
        self.assertEqual(slice_, 49152)
        self.assertIn(
            f'device uchar* dmsh_tgmem [[buffer({abi.TGMEM_BUFFER})]], uint3 dmsh_tgn [[threadgroups_per_grid]])', out
        )  # the existing position is reused
        self.assertIn(
            f'device spvUnsafeArray<uint, 12288>& _37 = *(device spvUnsafeArray<uint, 12288>*)(dmsh_tgmem + {abi.TGMEM_HEAD} + dmsh_tgl * 49152u);',
            out,
        )
        self.assertIn(
            'dmsh_tgl = (ulong(gl_WorkGroupID.z) * dmsh_tgn.y + gl_WorkGroupID.y) * dmsh_tgn.x + gl_WorkGroupID.x', out
        )
        self.assertIn('    threadgroup uint _38;', out)  # the small one stays
        self.assertIn('uint sum_of(device spvUnsafeArray<uint, 12288>& _9,', out)  # a helper taking it
        self.assertIn('(device atomic_uint*)&_37[0]', out)
        self.assertIn('threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);', out)
        self.assertNotIn('threadgroup spvUnsafeArray<uint, 12288>', out)

    def test_within_the_limit_unchanged(self):
        src = HEAD + SMALL
        self.assertEqual(tg_spill(src), (src, 0))

    def test_within_the_limit_with_other_types_unchanged(self):
        # Types the rule cannot size do not fail a kernel that needs no move.
        src = HEAD + SMALL.replace(
            '    threadgroup uint _38;\n',
            '    threadgroup atomic_uint _38;\n    threadgroup uint3 _39[16];\n    threadgroup S _41;\n',
        )
        self.assertEqual(tg_spill(src), (src, 0))

    def test_position_added_when_the_kernel_has_none(self):
        src = HEAD + BIG.replace(', uint3 gl_WorkGroupID [[threadgroup_position_in_grid]]', '').replace(
            'gl_WorkGroupID', 'uint3(0u)'
        )
        out, _ = tg_spill(src)
        self.assertIn('uint3 dmsh_tgp [[threadgroup_position_in_grid]])', out)
        self.assertIn('(ulong(dmsh_tgp.z) * dmsh_tgn.y + dmsh_tgp.y) * dmsh_tgn.x + dmsh_tgp.x', out)

    def test_existing_grid_builtin_reused(self):
        src = HEAD + BIG.replace(
            'uint3 gl_WorkGroupID', 'uint3 grid [[threadgroups_per_grid]], uint3 gl_WorkGroupID', 1
        )
        out, _ = tg_spill(src)
        self.assertEqual(out.count('[[threadgroups_per_grid]]'), 1)
        self.assertIn('* grid.y', out)
        self.assertIn('* grid.x', out)

    def test_empty_parameter_list(self):
        out, size = tg_spill('kernel void main0()\n{\n    threadgroup uint data[12288];\n}\n')
        self.assertEqual(size, 49152)
        self.assertNotIn('main0(,', out)

    def test_multiline_signature_and_indentation(self):
        source = HEAD + BIG.replace('[[buffer(1)]], ', '[[buffer(1)]],\n    ')
        source = source.replace(')\n{', ') {').replace('    threadgroup ', '\tthreadgroup ')
        rewritten, size = tg_spill(source)
        self.assertEqual(size, 49152)
        self.assertIn('device spvUnsafeArray<uint, 12288>& _37', rewritten)

    def test_vector_array_sizes_include_metal_padding(self):
        for element, count, expected in [('float3', 4096, 65536), ('half2', 12288, 49152), ('half4', 8192, 65536)]:
            with self.subTest(element=element):
                source = f'kernel void main0()\n{{\n    threadgroup {element} data[{count}];\n}}\n'
                rewritten, size = tg_spill(source)
                self.assertEqual(size, expected)
                self.assertIn(f'device {element} (&data)[{count}]', rewritten)

    def test_rest_over_the_limit_refused(self):
        src = HEAD + BIG.replace('    threadgroup uint _38;\n', '    threadgroup uint _38[9000];\n')
        with self.assertRaises(ValueError):
            tg_spill(src)


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class OnGpu(unittest.TestCase):
    RUNNER = r"""#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError *error = nil;
        if (!device) {
            printf("no Metal device\n");
            return 2;
        }
        NSData *library_data = [NSData dataWithContentsOfFile:@(argv[1])];
        dispatch_data_t data = dispatch_data_create(library_data.bytes, library_data.length, nil,
                                                   DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
        if (!library) {
            printf("library: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            printf("pipeline: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        const uint32_t slice_bytes = (uint32_t)atoi(argv[2]);
        const uint32_t header_bytes = (uint32_t)atoi(argv[3]);
        const uint32_t buffer_index = (uint32_t)atoi(argv[4]);
        const uint32_t slice_count = (uint32_t)atoi(argv[5]);
        const uint32_t grid_width = 16, grid_height = 64, threads_per_group = 64;
        const uint32_t output_count = grid_width * grid_height * threads_per_group;
        id<MTLBuffer> storage = [device newBufferWithLength:header_bytes + (size_t)slice_bytes * slice_count
                                                  options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:output_count * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
        uint32_t *header = storage.contents;
        memset(header, 0, header_bytes);
        header[0] = slice_bytes * slice_count;
        memset(output.contents, 0, output.length);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBuffer:storage offset:0 atIndex:buffer_index];
        [encoder dispatchThreadgroups:MTLSizeMake(grid_width, grid_height, 1)
                threadsPerThreadgroup:MTLSizeMake(threads_per_group, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.error) {
            printf("command buffer: %s\n", commands.error.localizedDescription.UTF8String);
            return 1;
        }
        const uint32_t *values = output.contents;
        long wrong_outputs = 0;
        for (uint32_t group = 0; group < grid_width * grid_height; group++) {
            for (uint32_t thread = 0; thread < threads_per_group; thread++) {
                uint32_t neighbour = (thread + 1) & 63;
                uint32_t expected = 0;
                for (uint32_t word = 0; word < 192; word++) {
                    expected += group * 100000u + neighbour * 1000u + word;
                }
                // Excess groups must leave output untouched, without corrupting valid groups' slices.
                if (group >= slice_count) {
                    expected = 0;
                }
                wrong_outputs += values[group * threads_per_group + thread] != expected;
            }
        }
        printf("%u threadgroups, %ld wrong outputs, flag %u\n", grid_width * grid_height, wrong_outputs, header[1]);
        return wrong_outputs != 0 || header[1] != (slice_count < grid_width * grid_height);
    }
}
"""

    def test_rewritten_kernel_runs(self):
        src = HEAD + BIG.replace(
            'uint3 gl_WorkGroupID', 'uint3 grid [[threadgroups_per_grid]], uint3 gl_WorkGroupID', 1
        )
        out, slice_ = tg_spill(src)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 't.metal').write_text(out)
            subprocess.run(
                [
                    'xcrun',
                    '-sdk',
                    'macosx',
                    'metal',
                    '-c',
                    '-O1',
                    '-fpreserve-invariance',
                    str(d / 't.metal'),
                    '-o',
                    str(d / 't.air'),
                ],
                check=True,
            )
            subprocess.run(
                ['xcrun', '-sdk', 'macosx', 'metallib', str(d / 't.air'), '-o', str(d / 't.metallib')], check=True
            )
            (d / 'run.m').write_text(self.RUNNER)
            subprocess.run(
                [
                    'clang',
                    '-fobjc-arc',
                    '-O1',
                    str(d / 'run.m'),
                    '-framework',
                    'Metal',
                    '-framework',
                    'Foundation',
                    '-o',
                    str(d / 'run'),
                ],
                check=True,
            )
            r = subprocess.run(
                [
                    str(d / 'run'),
                    str(d / 't.metallib'),
                    str(slice_),
                    str(abi.TGMEM_HEAD),
                    str(abi.TGMEM_BUFFER),
                    '1024',
                ],
                capture_output=True,
                text=True,
            )
            if r.returncode == 2:
                self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            print(r.stdout.strip())
            r = subprocess.run(
                [str(d / 'run'), str(d / 't.metallib'), str(slice_), str(abi.TGMEM_HEAD), str(abi.TGMEM_BUFFER), '17'],
                capture_output=True,
                text=True,
            )
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            print(r.stdout.strip())


if __name__ == '__main__':
    unittest.main()
