#!/usr/bin/env python3
"""Ordered not-equal at the SPIR-V → MSL boundary (PR #2042).

SPIRV-Cross used MSL's unordered != for OpFOrdNotEqual. These synthetic modules
keep ordered, unordered, inverted, integer and Boolean comparisons observable.
Scalar and vector f32/f16 results run on the GPU with the chain's Metal flags,
so folding an ordered expression back to != also fails. No game data is needed.
Set DMSH_TEST_GPU=1 for GPU readback and DMSH_TEST_METAL=1 for iOS compilation.
"""
import math
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
import dmsh_chain  # noqa: E402


def module(bits, width):
    types = []
    if width > 1:
        if width == 2:
            types.append('%u = OpTypeVector %uint 2')
        types += [f'%pred = OpTypeVector %bool {width}', f'%f = OpTypeVector %float {width}']
        if bits == 16:
            types.append(f'%value = OpTypeVector %half {width}')
    value = ('%value' if width > 1 else '%half') if bits == 16 else '%f'
    loads = []
    for name, index in (('a', '%ai'), ('b', '%bi')):
        loads += [f'%{name}p = OpAccessChain %ptr4 %input %zero {index}', f'%{name}4 = OpLoad %uint4 %{name}p']
        if width == 1:
            loads.append(f'%{name}u = OpCompositeExtract %u %{name}4 0')
        else:
            loads.append(f'%{name}u = OpVectorShuffle %u %{name}4 %{name}4 ' + ' '.join(map(str, range(width))))
        loads.append(f'%{name}f = OpBitcast %f %{name}u')
        if bits == 16:
            loads.append(f'%{name} = OpFConvert {value} %{name}f')
        else:
            loads.append(f'%{name} = OpCopyObject {value} %{name}f')
    compares = ['%ord = OpFOrdNotEqual %pred %a %b', '%unord = OpFUnordNotEqual %pred %a %b',
                '%inv = OpLogicalNot %pred %ord', '%integer = OpINotEqual %pred %au %bu', '%logical = OpLogicalNotEqual %pred %ord %unord']
    stores = []
    for k, name in enumerate(('ord', 'unord', 'inv', 'integer', 'logical')):
        stores.append(f'%{name}u = OpSelect %u %{name} %ones %zeros')
        components = []
        for lane in range(width):
            component = f'%{name}u'
            if width > 1:
                component = f'%{name}{lane}'
                stores.append(f'{component} = OpCompositeExtract %uint %{name}u {lane}')
            components.append(component)
        stores += [f'%{name}4 = OpCompositeConstruct %uint4 ' + ' '.join(components + ['%zero'] * (4 - width)),
                   f'%{name}i = OpIAdd %uint %oi %c{k}', f'%{name}p = OpAccessChain %ptr4 %output %zero %{name}i',
                   f'OpStore %{name}p %{name}4']
    constants = ('%ones = OpConstant %u 1\n%zeros = OpConstant %u 0' if width == 1 else
                 '%ones = OpConstantComposite %u ' + ' '.join(['%one'] * width) + '\n%zeros = OpConstantComposite %u ' + ' '.join(['%zero'] * width))
    text = '\n'.join([
        'OpCapability Shader', *(['OpCapability Float16'] if bits == 16 else []), 'OpMemoryModel Logical GLSL450',
        'OpEntryPoint GLCompute %main "main" %gid %input %output', 'OpExecutionMode %main LocalSize 64 1 1',
        'OpDecorate %gid BuiltIn GlobalInvocationId', 'OpDecorate %array ArrayStride 16', 'OpMemberDecorate %block 0 Offset 0',
        'OpDecorate %block Block', 'OpDecorate %input DescriptorSet 0', 'OpDecorate %input Binding 0',
        'OpDecorate %input NonWritable', 'OpDecorate %output DescriptorSet 0', 'OpDecorate %output Binding 1',
        'OpDecorate %output NonReadable', '%void = OpTypeVoid', '%fn = OpTypeFunction %void', '%uint = OpTypeInt 32 0',
        '%bool = OpTypeBool', '%float = OpTypeFloat 32', *(['%half = OpTypeFloat 16'] if bits == 16 else []),
        '%uint3 = OpTypeVector %uint 3', '%uint4 = OpTypeVector %uint 4', *types,
        '%array = OpTypeRuntimeArray %uint4', '%block = OpTypeStruct %array', '%ptr = OpTypePointer StorageBuffer %block',
        '%ptr4 = OpTypePointer StorageBuffer %uint4', '%gidptr = OpTypePointer Input %uint3',
        '%input = OpVariable %ptr StorageBuffer', '%output = OpVariable %ptr StorageBuffer', '%gid = OpVariable %gidptr Input',
        '%zero = OpConstant %uint 0', '%one = OpConstant %uint 1', '%two = OpConstant %uint 2', '%five = OpConstant %uint 5',
        *(f'%c{k} = OpConstant %uint {k}' for k in range(5)), constants,
        '%main = OpFunction %void None %fn', '%label = OpLabel', '%xyz = OpLoad %uint3 %gid',
        '%i = OpCompositeExtract %uint %xyz 0', '%ai = OpIMul %uint %i %two', '%bi = OpIAdd %uint %ai %one',
        '%oi = OpIMul %uint %i %five', *loads, *compares, *stores, 'OpReturn', 'OpFunctionEnd', ''])
    # SPIR-V forbids duplicate scalar/vector type declarations.
    uint_type = '%uint' if width == 1 else f'%uint{width}'
    text = text.replace('%u ', uint_type + ' ')
    if width == 1:
        text = text.replace('%pred ', '%bool ').replace('%f ', '%float ')
    return text


# Signed zeros, ordinary numbers, infinities, and signed quiet/signaling NaNs.
EDGE = (0, 0x80000000, 0x3f800000, 0xbf800000, 0x40000000, 0xc0000000, 0x7f800000, 0xff800000,
        0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fffffff)
ROWS = [tuple(EDGE[(i + k) % len(EDGE)] for k in range(4)) + tuple(EDGE[(j + k) % len(EDGE)] for k in range(4))
        for i in range(len(EDGE)) for j in range(len(EDGE))]


def expected(bits, width):
    values = []
    for row in ROWS:
        columns = [[] for _ in range(5)]
        for lane in range(width):
            a, b = (struct.unpack('<f', struct.pack('<I', word))[0] for word in (row[lane], row[lane + 4]))
            if bits == 16:
                a, b = (struct.unpack('<e', struct.pack('<e', value))[0] for value in (a, b))
            nan = math.isnan(a) or math.isnan(b)
            ordered, unordered = not nan and a != b, nan or a != b
            for column, value in zip(columns, (ordered, unordered, not ordered, row[lane] != row[lane + 4], ordered != unordered)):
                column.append(int(value))
        values.extend(value for column in columns for value in column + [0] * (4 - width))
    return values


RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) { fprintf(stderr, "no Metal device\n"); return 2; }
    NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> ps = f ? [d newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) { fprintf(stderr, "pipeline: %s\n", e.description.UTF8String); return 1; }
    NSData *data = [NSData dataWithContentsOfFile:@(argv[2])];
    NSUInteger rows = data.length / 32;
    id<MTLBuffer> input = [d newBufferWithBytes:data.bytes length:data.length options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [d newBufferWithLength:rows * 80 options:MTLResourceStorageModeShared];
    id<MTLArgumentEncoder> args = [f newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> bindings = [d newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    [args setArgumentBuffer:bindings offset:0]; [args setBuffer:input offset:0 atIndex:0]; [args setBuffer:output offset:0 atIndex:1];
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:bindings offset:0 atIndex:0];
    [ce useResource:input usage:MTLResourceUsageRead]; [ce useResource:output usage:MTLResourceUsageWrite];
    [ce dispatchThreads:MTLSizeMake(rows, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fprintf(stderr, "gpu: %s\n", cb.error.description.UTF8String); return 1; }
    return [[NSData dataWithBytes:output.contents length:output.length] writeToFile:@(argv[3]) atomically:YES] ? 0 : 1;
} }
'''


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs spirv-as and spirv-val')
class OrderedNotEqual(unittest.TestCase):
    @classmethod
    def checked(cls, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(f'{command!r}\n{result.stdout}\n{result.stderr}')
        return result.stdout

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='spirv_ordered_ne_')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        cls.shaders = []
        for bits in (32, 16):
            for width in range(1, 5):
                stem = cls.work / f'f{bits}x{width}'
                stem.with_suffix('.spvasm').write_text(module(bits, width))
                cls.checked(['spirv-as', '--target-env', 'vulkan1.2', str(stem.with_suffix('.spvasm')), '-o', str(stem.with_suffix('.spv'))])
                cls.checked(['spirv-val', '--target-env', 'vulkan1.2', str(stem.with_suffix('.spv'))])
                msl = cls.checked([dmsh_chain.SPIRV_CROSS, *dmsh_chain.CROSS_MSL, str(stem.with_suffix('.spv'))])
                stem.with_suffix('.metal').write_text(msl)
                cls.shaders.append((bits, width, stem))

    def test_all_scalar_and_vector_modules_translate(self):
        self.assertEqual(len(self.shaders), 8)

    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and shutil.which('xcrun'), 'set DMSH_TEST_METAL=1 for iOS compilation')
    def test_ios_compiles_scalar_and_vector_comparisons(self):
        sys.path.insert(0, str(TOOLS.parents[1] / 'ios/tools'))
        import ios_metal
        for bits, width, stem in self.shaders:
            with self.subTest(bits=bits, width=width):
                self.checked([*ios_metal.METAL, str(stem.with_suffix('.metal')), '-o', str(stem.with_suffix('.ios.air'))])

    @unittest.skipUnless(os.environ.get('DMSH_TEST_GPU') and sys.platform == 'darwin', 'set DMSH_TEST_GPU=1 for Metal GPU readback')
    def test_gpu_preserves_ordered_and_unordered_results(self):
        (self.work / 'run.m').write_text(RUNNER)
        self.checked(['clang', '-fobjc-arc', '-O1', str(self.work / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(self.work / 'run')])
        data = [word for row in ROWS for word in row]
        (self.work / 'input').write_bytes(struct.pack(f'<{len(data)}I', *data))
        for bits, width, stem in self.shaders:
            with self.subTest(bits=bits, width=width):
                self.checked([*dmsh_chain.MAC_METAL, str(stem.with_suffix('.metal')), '-o', str(stem.with_suffix('.air'))])
                self.checked(['xcrun', '-sdk', 'macosx', 'metallib', str(stem.with_suffix('.air')), '-o', str(stem.with_suffix('.metallib'))])
                self.checked([str(self.work / 'run'), str(stem.with_suffix('.metallib')), str(self.work / 'input'), str(self.work / 'output')])
                blob = (self.work / 'output').read_bytes()
                got = struct.unpack(f'<{len(blob) // 4}I', blob)
                want = expected(bits, width)
                self.assertEqual(len(got), len(want))
                mismatches = [(i, a, b) for i, (a, b) in enumerate(zip(got, want)) if a != b]
                self.assertEqual(len(mismatches), 0, f'wrong results (word, got, expected): {mismatches[:4]}')


if __name__ == '__main__':
    unittest.main()
