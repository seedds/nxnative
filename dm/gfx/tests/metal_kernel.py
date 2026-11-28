"""Run one compute function on this Mac's GPU over a small buffer of 32-bit words, compiled with the shader chain's own Mac flags, and read the words back.
Used by the tests that check an atomic's arithmetic on the GPU (test_dmsh_float_add.py, test_dmsh_ampere_alu.py)."""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_chain   # noqa: E402

RUNNER = r'''#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
// r <metallib> <function> <argument buffer 0|1|2> <threads> <threads per group> <words...>: the words are buffer 0 (or entry 0 of the argument buffer; mode 2 puts the sblen table at entry 0 and words at entry 1)
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[1])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
    id<MTLFunction> f = [l newFunctionWithName:@(av[2])];
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:f error:&e]; if (!ps) { puts(e.description.UTF8String); return 2; }
    int n = ac - 6, mode = atoi(av[3]); id<MTLBuffer> b = [d newBufferWithLength:4 * (n > 4 ? n : 4) options:MTLResourceStorageModeShared]; memset(b.contents, 0, b.length);
    for (int i = 0; i < n; i++) ((unsigned *)b.contents)[i] = (unsigned)strtoul(av[6 + i], 0, 16);
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
    [ce setComputePipelineState:ps];
    if (mode) {
        id<MTLArgumentEncoder> ae = [f newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> ab = [d newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared]; [ae setArgumentBuffer:ab offset:0];
        if (mode == 2) {
            id<MTLBuffer> sb = [d newBufferWithLength:256 options:MTLResourceStorageModeShared]; memset(sb.contents, 0, 256);
            ((unsigned *)sb.contents)[1] = (unsigned)(n > 4 ? n : 4);
            [ae setBuffer:sb offset:0 atIndex:0]; [ae setBuffer:b offset:0 atIndex:1];
            [ce useResource:sb usage:MTLResourceUsageRead];
        } else [ae setBuffer:b offset:0 atIndex:0];
        [ce setBuffer:ab offset:0 atIndex:0]; [ce useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    } else [ce setBuffer:b offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(atoi(av[4]), 1, 1) threadsPerThreadgroup:MTLSizeMake(atoi(av[5]), 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
    for (int i = 0; i < n; i++) printf("%08x%c", ((const unsigned *)b.contents)[i], i + 1 < n ? ' ' : '\n');
    return 0;
} }
'''


def run(msl, function, words, threads, per_group=32, argument_buffer=False):
    """words (ints) after threads run function of msl; argument_buffer: the function takes them as entry 0 of an argument buffer at index 0, as translated shaders do"""
    with tempfile.TemporaryDirectory() as w:
        w = Path(w); (w / 'k.metal').write_text(msl); (w / 'r.m').write_text(RUNNER)
        subprocess.run([*dmsh_chain.mac_metal(), str(w / 'k.metal'), '-o', str(w / 'k.air')], check=True, capture_output=True)   # the chain's flags: its deployment target is its language version
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True, capture_output=True)
        subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
        mode = 2 if (argument_buffer and 'cp_c27' in msl) else int(argument_buffer)
        out = subprocess.run([str(w / 'r'), str(w / 'k.metallib'), function, str(mode), str(threads), str(per_group), *(f'{x:08x}' for x in words)], capture_output=True, text=True)
        if out.returncode: raise AssertionError(out.stdout + out.stderr)
        return [int(x, 16) for x in out.stdout.split()]
