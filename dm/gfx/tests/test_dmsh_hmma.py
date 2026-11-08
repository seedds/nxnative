"""Ampere HMMA.16816 with an f16 accumulator through simdgroup_matrix (dmsh_build.py hmma_msl): the rewrite of the translator's helper dmsh_hmma16816_f16 as spirv-cross
writes it, its arguments and scratch array, the matrices and shared memory it gives the functions that call it, the compile with the chain's Metal flags, and on this Mac's
GPU the rewritten body against a CPU reference of PTX mma.m16n8k16 (f16 sources and accumulator): one call on random fragments, four SIMD-groups a threadgroup, and a run
of calls with TensorPass's flags (an accumulator kept in its slot between calls, A kept from the call before, C = 0, A loaded from shared memory where an LDSM read it,
plain and transposed). With DMSH_TEST_XLATE (the translator), the flags it gives two chained HMMAs on an LDSM's matrices."""
import ast
import bisect
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
NAMES = {'hmma_msl', 'split_params', 'block_end', 'BRACE', 'FUNC_OPEN', 'HMMA_BODY', 'HMMA_ARGS', 'HMMA_CALL', 'TG_ARRAY'}
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in NAMES)
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) in NAMES for t in n.targets))]
scope = {'re': re, 'bisect': bisect}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
hmma_msl = scope['hmma_msl']

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];

    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    constexpr const thread T& operator [] (size_t pos) const thread { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup { return elements[pos]; }
};
'''
# The helper as spirv-cross writes it (Ocarina of Time's e6be82d835fbe4d3, cut down to its signature and a body of the same shape): lane, block, A (4), B (2), C (2), flags,
# A's LDSM address, A's four offsets, A's row stride, out D1, the scratch
HELPER = '''
static inline __attribute__((always_inline))
int dmsh_hmma16816_f16(thread const int& _10, thread const int& _11, thread const int& _12, thread const int& _13, thread const int& _14, thread const int& _15, thread const int& _16, thread const int& _17, thread const int& _18, thread const int& _19, thread const int& _20, thread const int& _21, thread const int& _22, thread const int& _23, thread const int& _24, thread const int& _25, thread const int& _26, thread int& _27, threadgroup spvUnsafeArray<uint, 1024>& _34)
{
    uint _40 = uint(int(uint(_10) >> uint(2)));
    _34[_11 + int(_40)] = uint(_12);
    if (_40 > 3u)
    {
        _27 = 0;
    }
    simdgroup_barrier(mem_flags::mem_threadgroup);
    _27 = int(_34[_11]);
    return int(_34[_11 + 1]);
}
'''
KERNEL = '''
kernel void check(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                  uint gid [[thread_position_in_grid]])
{
    threadgroup spvUnsafeArray<uint, 1024> scr;
    int l = int(lane), blk = int(sg * 256), z = 0;
    int a0 = int(in[gid * 8 + 0]), a1 = int(in[gid * 8 + 1]), a2 = int(in[gid * 8 + 2]), a3 = int(in[gid * 8 + 3]);
    int b0 = int(in[gid * 8 + 4]), b1 = int(in[gid * 8 + 5]), c0 = int(in[gid * 8 + 6]), c1 = int(in[gid * 8 + 7]);
    int d1;
    int d0 = dmsh_hmma16816_f16(l, blk, a0, a1, a2, a3, b0, b1, c0, c1, z, z, z, z, z, z, z, d1, scr);
    out[gid * 2] = uint(d0); out[gid * 2 + 1] = uint(d1);
}
'''
# A run of four calls a SIMD-group, with the flags TensorPass gives them (TensorPass.Flag*): A0's tile lies in shared memory row-major, 48 bytes a row, read as an LDSM's
# matrices 0..3 at rows 0, 8 and columns 0, 8 (offsets 0, 384, 16, 400); A1 lies there transposed (A1's columns are the rows, 32 bytes apart: matrices at 0, 16, 256, 272).
#   1. A0 B0 + C       A where the LDSM read it, D left in slot 0          (flags 16 | 4)
#   2. A0 B1 + slot 0  A kept from call 1, C from slot 0: E0               (1 | 2 | 16)
#   3. A1 B2           C = 0, A through the scratch, D left in slot 1      (8 | 4 | 1 << 8)
#   4. A1 B3 + slot 1  A transposed where the LDSM read it, C slot 1: E1   (2 | 16 | 32 | 1 << 8)
# The registers each call is given are what the translator's body would compute with (a stale A or C included), so the run also checks that the flags are what is read.
# Input a SIMD-group: halves A0 (16 x 16, row-major), A1, B0..B3 (16 x 8, k-major), C (16 x 8): 1152 halves
SEQ_KERNEL = '''
static uint pk(device const ushort* p, int i, int j) { return uint(p[i]) | uint(p[j]) << 16; }
kernel void check_seq(device const ushort *in [[buffer(0)]], device uint *out [[buffer(1)]], uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                      uint gid [[thread_position_in_grid]])
{
    threadgroup spvUnsafeArray<uint, 1280> shm;
    threadgroup spvUnsafeArray<uint, 1024> scr;
    device const ushort* x = in + sg * 1152 + (gid / 128) * 4 * 1152;
    device const ushort *A0 = x, *A1 = x + 256, *B0 = x + 512, *B1 = x + 640, *B2 = x + 768, *B3 = x + 896, *C = x + 1024;
    threadgroup ushort* sh = (threadgroup ushort*)&shm[0];
    int base0 = int(sg) * 1280, base1 = base0 + 768;   // bytes: 16 rows of 48, then 16 rows of 32
    for (int i = int(lane); i < 256; i += 32) { int r = i / 16, c = i % 16; sh[base0 / 2 + r * 24 + c] = A0[i]; sh[base1 / 2 + c * 16 + r] = A1[i]; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    int l = int(lane), g = l / 4, t = l % 4, blk = int(sg) * 256;
    int a00 = int(pk(A0, g * 16 + 2 * t, g * 16 + 2 * t + 1)), a01 = int(pk(A0, (g + 8) * 16 + 2 * t, (g + 8) * 16 + 2 * t + 1));
    int a02 = int(pk(A0, g * 16 + 8 + 2 * t, g * 16 + 9 + 2 * t)), a03 = int(pk(A0, (g + 8) * 16 + 8 + 2 * t, (g + 8) * 16 + 9 + 2 * t));
    int a10 = int(pk(A1, g * 16 + 2 * t, g * 16 + 2 * t + 1)), a11 = int(pk(A1, (g + 8) * 16 + 2 * t, (g + 8) * 16 + 2 * t + 1));
    int a12 = int(pk(A1, g * 16 + 8 + 2 * t, g * 16 + 9 + 2 * t)), a13 = int(pk(A1, (g + 8) * 16 + 8 + 2 * t, (g + 8) * 16 + 9 + 2 * t));
    int b00 = int(pk(B0, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b01 = int(pk(B0, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int b10 = int(pk(B1, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b11 = int(pk(B1, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int b20 = int(pk(B2, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b21 = int(pk(B2, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int b30 = int(pk(B3, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b31 = int(pk(B3, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int c0 = int(pk(C, g * 8 + 2 * t, g * 8 + 2 * t + 1)), c1 = int(pk(C, (g + 8) * 8 + 2 * t, (g + 8) * 8 + 2 * t + 1));
    int addr0 = base0 + ((l & 7) + 8 * ((l >> 3) & 1)) * 48 + 16 * (l >> 4), addr1 = base1 + (l & 7) * 32 + ((l >> 3) & 1) * 16 + (l >> 4) * 256;
    int f1 = 16 | 4, f2 = 1 | 2 | 16, f3 = 8 | 4 | 1 << 8, f4 = 2 | 16 | 32 | 1 << 8, z = 0;
    int o0 = 0, o1 = 384, o2 = 16, o3 = 400, s0 = 48, p0 = 0, p1 = 16, p2 = 256, p3 = 272, s1 = 32;
    int e1, e2, e3, e4;
    int d1 = dmsh_hmma16816_f16(l, blk, a00, a01, a02, a03, b00, b01, c0, c1, f1, addr0, o0, o1, o2, o3, s0, e1, scr);
    int d2 = dmsh_hmma16816_f16(l, blk, a00, a01, a02, a03, b10, b11, d1, e1, f2, addr0, o0, o1, o2, o3, s0, e2, scr);
    int d3 = dmsh_hmma16816_f16(l, blk, a10, a11, a12, a13, b20, b21, z, z, f3, z, z, z, z, z, z, e3, scr);
    int d4 = dmsh_hmma16816_f16(l, blk, a00, a01, a02, a03, b30, b31, d3, e3, f4, addr1, p0, p1, p2, p3, s1, e4, scr);
    out[gid * 4] = uint(d2); out[gid * 4 + 1] = uint(e2); out[gid * 4 + 2] = uint(d4); out[gid * 4 + 3] = uint(e4);
}
'''
# Call 4 is given A0's registers on purpose: A1 must come from shared memory (FlagADirect), transposed (FlagATrans)
# A chain held in slot 0 while calls outside every chain run (Ocarina of Time's agl_ndsr_vr networks, GitHub issue #1537): such a call has slot 0 in its flags
# and must leave slot 0's accumulator alone. E0 = A0 B0 + C kept in slot 0 (flags 4), then E1 = A1 B2 (C = 0, 8) and + A1 B3 (C through the registers, 0), both outside
# every chain, then E0 += A0 B1 from slot 0 (2). The same outputs and reference as the run above
CLOBBER_KERNEL = '''
static uint pk(device const ushort* p, int i, int j) { return uint(p[i]) | uint(p[j]) << 16; }
kernel void check_seq(device const ushort *in [[buffer(0)]], device uint *out [[buffer(1)]], uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                      uint gid [[thread_position_in_grid]])
{
    threadgroup spvUnsafeArray<uint, 1280> shm;
    threadgroup spvUnsafeArray<uint, 1024> scr;
    device const ushort* x = in + sg * 1152 + (gid / 128) * 4 * 1152;
    device const ushort *A0 = x, *A1 = x + 256, *B0 = x + 512, *B1 = x + 640, *B2 = x + 768, *B3 = x + 896, *C = x + 1024;
    threadgroup ushort* sh = (threadgroup ushort*)&shm[0];
    int base0 = int(sg) * 1280, base1 = base0 + 768;   // bytes: 16 rows of 48, then 16 rows of 32
    for (int i = int(lane); i < 256; i += 32) { int r = i / 16, c = i % 16; sh[base0 / 2 + r * 24 + c] = A0[i]; sh[base1 / 2 + c * 16 + r] = A1[i]; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    int l = int(lane), g = l / 4, t = l % 4, blk = int(sg) * 256;
    int a00 = int(pk(A0, g * 16 + 2 * t, g * 16 + 2 * t + 1)), a01 = int(pk(A0, (g + 8) * 16 + 2 * t, (g + 8) * 16 + 2 * t + 1));
    int a02 = int(pk(A0, g * 16 + 8 + 2 * t, g * 16 + 9 + 2 * t)), a03 = int(pk(A0, (g + 8) * 16 + 8 + 2 * t, (g + 8) * 16 + 9 + 2 * t));
    int a10 = int(pk(A1, g * 16 + 2 * t, g * 16 + 2 * t + 1)), a11 = int(pk(A1, (g + 8) * 16 + 2 * t, (g + 8) * 16 + 2 * t + 1));
    int a12 = int(pk(A1, g * 16 + 8 + 2 * t, g * 16 + 9 + 2 * t)), a13 = int(pk(A1, (g + 8) * 16 + 8 + 2 * t, (g + 8) * 16 + 9 + 2 * t));
    int b00 = int(pk(B0, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b01 = int(pk(B0, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int b10 = int(pk(B1, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b11 = int(pk(B1, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int b20 = int(pk(B2, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b21 = int(pk(B2, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int b30 = int(pk(B3, (2 * t) * 8 + g, (2 * t + 1) * 8 + g)), b31 = int(pk(B3, (2 * t + 8) * 8 + g, (2 * t + 9) * 8 + g));
    int c0 = int(pk(C, g * 8 + 2 * t, g * 8 + 2 * t + 1)), c1 = int(pk(C, (g + 8) * 8 + 2 * t, (g + 8) * 8 + 2 * t + 1));
    int addr0 = base0 + ((l & 7) + 8 * ((l >> 3) & 1)) * 48 + 16 * (l >> 4), addr1 = base1 + (l & 7) * 32 + ((l >> 3) & 1) * 16 + (l >> 4) * 256;
    int f1 = 4, f2 = 8, f3 = 0, f4 = 2, z = 0;
    int e1, e2, e3, e4;
    int d1 = dmsh_hmma16816_f16(l, blk, a00, a01, a02, a03, b00, b01, c0, c1, f1, z, z, z, z, z, z, e1, scr);
    int d2 = dmsh_hmma16816_f16(l, blk, a10, a11, a12, a13, b20, b21, z, z, f2, z, z, z, z, z, z, e2, scr);
    int d3 = dmsh_hmma16816_f16(l, blk, a10, a11, a12, a13, b30, b31, d2, e2, f3, z, z, z, z, z, z, e3, scr);
    int d4 = dmsh_hmma16816_f16(l, blk, a00, a01, a02, a03, b10, b11, d1, e1, f4, z, z, z, z, z, z, e4, scr);
    out[gid * 4] = uint(d4); out[gid * 4 + 1] = uint(e4); out[gid * 4 + 2] = uint(d3); out[gid * 4 + 3] = uint(e3);
}
'''
HALF = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }
static uint16_t f2h(float f) { __fp16 v = (__fp16)f; uint16_t h; memcpy(&h, &v, 2); return h; }
static id<MTLComputePipelineState> pipeline(id<MTLDevice> dev, const char *path, NSString *name) {
    NSError *e = nil; NSData *lib_data = [NSData dataWithContentsOfFile:@(path)];
    id<MTLLibrary> lib = [dev newLibraryWithData:dispatch_data_create(lib_data.bytes, lib_data.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&e];
    if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); exit(1); }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&e];
    if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); exit(1); }
    return ps;
}
static void run(id<MTLDevice> dev, id<MTLComputePipelineState> ps, id<MTLBuffer> in, id<MTLBuffer> out, int groups) {
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:out offset:0 atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
'''
# The GPU check: 64 threadgroups of 128 lanes, random halves in [-1, 1]; the reference sums in float and rounds once; the matrix hardware rounds the f16 accumulator as
# it goes, as Ampere's f16 accumulator does, so the tolerance is 0.01 + 0.4 % of the value
RUNNER = HALF + r'''
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 2; }
        id<MTLComputePipelineState> ps = pipeline(dev, argv[1], @"check");
        const int groups = 64, tg = 128, n = groups * tg;
        id<MTLBuffer> in = [dev newBufferWithLength:n * 32 options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
        uint32_t *w = in.contents; srand(11);
        for (int i = 0; i < n * 8; i++) w[i] = f2h((rand() % 2001 - 1000) / 1000.0f) | (uint32_t)f2h((rand() % 2001 - 1000) / 1000.0f) << 16;
        run(dev, ps, in, out, groups);
        uint32_t *o = out.contents; long bad = 0; double worst = 0;
        for (int warp = 0; warp < n / 32; warp++) {
            float A[16][16], B[16][8], C[16][8];
            for (int L = 0; L < 32; L++) {
                const uint32_t *f = w + (warp * 32 + L) * 8; int g = L / 4, t = L % 4;
                for (int r = 0; r < 4; r++) { int row = g + 8 * (r & 1), k = 2 * t + 8 * (r >> 1); A[row][k] = h2f(f[r] & 0xffff); A[row][k + 1] = h2f(f[r] >> 16); }
                for (int r = 0; r < 2; r++) { int k = 2 * t + 8 * r; B[k][g] = h2f(f[4 + r] & 0xffff); B[k + 1][g] = h2f(f[4 + r] >> 16); }
                for (int r = 0; r < 2; r++) { int row = g + 8 * r; C[row][2 * t] = h2f(f[6 + r] & 0xffff); C[row][2 * t + 1] = h2f(f[6 + r] >> 16); }
            }
            for (int L = 0; L < 32; L++) {
                int g = L / 4, t = L % 4; const uint32_t *r = o + (warp * 32 + L) * 2;
                for (int q2 = 0; q2 < 2; q2++) for (int j = 0; j < 2; j++) {
                    int row = g + 8 * q2, col = 2 * t + j; float ref = C[row][col];
                    for (int k = 0; k < 16; k++) ref += A[row][k] * B[k][col];
                    double d = fabs(h2f((r[q2] >> (16 * j)) & 0xffff) - ref);
                    if (d > worst) worst = d;
                    bad += d > 0.01 + fabs(ref) * 0.004;
                }
            }
        }
        printf("%d SIMD-groups, max abs error %.4g, %ld outputs over tolerance\n", n / 32, worst, bad);
        return bad != 0;
    }
}
'''
# The run of calls: E0 = A0 (B0 + B1) + C and E1 = A1 (B2 + B3), each two HMMAs with an f16 accumulator in between: the tolerance is twice the single call's
SEQ_RUNNER = HALF + r'''
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 2; }
        id<MTLComputePipelineState> ps = pipeline(dev, argv[1], @"check_seq");
        const int groups = 16, warps = groups * 4;
        id<MTLBuffer> in = [dev newBufferWithLength:warps * 1152 * 2 options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:warps * 32 * 16 options:MTLResourceStorageModeShared];
        uint16_t *h = in.contents; srand(7);
        for (int i = 0; i < warps * 1152; i++) h[i] = f2h((rand() % 2001 - 1000) / 1000.0f);
        run(dev, ps, in, out, groups);
        uint32_t *o = out.contents; long bad = 0; double worst = 0;
        for (int w = 0; w < warps; w++) {
            const uint16_t *x = h + w * 1152;
            for (int L = 0; L < 32; L++) {
                int g = L / 4, t = L % 4; const uint32_t *r = o + (w * 32 + L) * 4;
                for (int e = 0; e < 2; e++) for (int q2 = 0; q2 < 2; q2++) for (int j = 0; j < 2; j++) {
                    int row = g + 8 * q2, col = 2 * t + j;
                    const uint16_t *A = x + 256 * e, *Ba = x + 512 + 256 * e, *Bb = Ba + 128;
                    float ref = e == 0 ? h2f(x[1024 + row * 8 + col]) : 0;
                    for (int k = 0; k < 16; k++) ref += h2f(A[row * 16 + k]) * (h2f(Ba[k * 8 + col]) + h2f(Bb[k * 8 + col]));
                    double d = fabs(h2f((r[e * 2 + q2] >> (16 * j)) & 0xffff) - ref);
                    if (d > worst) worst = d;
                    bad += d > 0.02 + fabs(ref) * 0.008;
                }
            }
        }
        printf("%d SIMD-groups, max abs error %.4g, %ld outputs over tolerance\n", warps, worst, bad);
        return bad != 0;
    }
}
'''


def compile_and_run(test, msl, runner):
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / 'h.metal').write_text(msl)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(d / 'h.metal'), '-o', str(d / 'h.air')], check=True)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'h.air'), '-o', str(d / 'h.metallib')], check=True)
        (d / 'run.m').write_text(runner)
        subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
        r = subprocess.run([str(d / 'run'), str(d / 'h.metallib')], capture_output=True, text=True)
        if r.returncode == 2: test.skipTest(r.stdout.strip())
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class HmmaRewrite(unittest.TestCase):
    def test_body_replaced_with_its_arguments(self):
        out = hmma_msl(HEAD + HELPER + KERNEL)
        body = out[out.index('int dmsh_hmma16816_f16('):out.index('kernel void check')]
        self.assertEqual(body.count('simdgroup_multiply_accumulate('), 4)
        self.assertIn('threadgroup uint* s = &_34[uint(_11)];', body)        # the scratch array at the block offset
        self.assertIn('uint g = uint(_10) >> 2', body)                        # the lane
        self.assertIn('s[g * 8 + t] = uint(_12)', body)                       # A0
        self.assertIn('s[192 + (g + 8) * 4 + t] = uint(_19)', body)           # C1
        self.assertIn('const int f = int(_20)', body)                         # the flags
        self.assertIn('_27 = as_type<int>(d1);', body)                        # the out argument
        self.assertIn('return as_type<int>(d0);', body)
        self.assertNotIn('_40', body)                                         # the translator's body is gone, the braces of its blocks too
        self.assertEqual(out.count('static inline __attribute__((always_inline))'), 1)   # spirv-cross's line before it stays, alone

    def test_callers_get_their_matrices_and_shared_memory(self):
        out = hmma_msl(HEAD + HELPER + SEQ_KERNEL)
        kernel = out[out.index('kernel void check_seq'):]
        self.assertIn('{\n    simdgroup_half8x8 dm_acc[16], dm_am[4];\n', kernel)   # first in the body
        self.assertEqual(kernel.count(', scr, dm_acc, dm_am, shm)'), 4)              # every call: the matrices, then the one other threadgroup array
        self.assertIn('thread simdgroup_half8x8 (&acc)[16], thread simdgroup_half8x8 (&am)[4], threadgroup spvUnsafeArray<uint, 1280>& dm_shm)', out)
        self.assertIn('adirect = (f & 16) != 0', out)

    def test_two_other_arrays_load_a_through_the_scratch(self):
        src = SEQ_KERNEL.replace('    threadgroup spvUnsafeArray<uint, 1024> scr;\n', '    threadgroup spvUnsafeArray<uint, 1024> scr;\n    threadgroup spvUnsafeArray<uint, 16> other;\n')
        out = hmma_msl(HEAD + HELPER + src)
        self.assertNotIn('dm_shm', out)
        self.assertIn('adirect = false', out)
        self.assertEqual(out.count(', scr, dm_acc, dm_am)'), 4)

    def test_kernel_unchanged_but_its_calls(self):
        out = hmma_msl(HEAD + HELPER + KERNEL)
        self.assertIn(KERNEL.strip().replace('d1, scr)', 'd1, scr, dm_acc, dm_am)').replace('{\n    threadgroup', '{\n    simdgroup_half8x8 dm_acc[16], dm_am[4];\n    threadgroup', 1), out)

    def test_other_shaders_unchanged(self):
        src = HEAD + KERNEL.replace('dmsh_hmma16816_f16(l, blk, a0, a1, a2, a3, b0, b1, c0, c1, z, z, z, z, z, z, z, d1, scr)', 'a0')
        self.assertEqual(hmma_msl(src), src)

    def test_unexpected_signature_refused(self):
        with self.assertRaises(ValueError):
            hmma_msl(HEAD + HELPER.replace('thread const int& _26, ', ''))

    def test_block_end_as_a_scan_of_every_character(self):   # block_end steps from brace to brace; it gives what a scan of every character gives, a start past the end and an open block included
        def scan(src, start):
            depth = 0
            for i in range(start, len(src)):
                depth += {'{': 1, '}': -1}.get(src[i], 0)
                if depth == 0: return i
            return None
        def found(src, start):
            try: return scope['block_end'](src, start)
            except ValueError: return None
        for src in ('{a{b}c}d', '{{}', 'a{}', '}{}', '', '{'):
            for start in range(len(src) + 2): self.assertEqual(found(src, start), scan(src, start), (src, start))
        src = HEAD + HELPER + SEQ_KERNEL   # every brace of a shader, and some other characters
        for start in [i for i, ch in enumerate(src) if ch in '{}'] + list(range(0, len(src), 97)):
            self.assertEqual(found(src, start), scan(src, start), start)

    def test_function_openings_as_rfind_finds_them(self):   # hmma_msl finds a call's function among the openings found once (FUNC_OPEN, bisect): the last line holding only a brace that ends before it
        for src in (HEAD + HELPER + SEQ_KERNEL, '\n{\n{\nx\n{\n', 'x\n{', '\n{\n'):
            opens = [o.start() for o in scope['FUNC_OPEN'].finditer(src)]
            for at in range(len(src) + 1):
                k = bisect.bisect_right(opens, at - 3) - 1
                self.assertEqual(opens[k] if k >= 0 else -1, src.rfind('\n{\n', 0, at), (src[:40], at))


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class HmmaOnGpu(unittest.TestCase):
    def test_matches_reference(self):
        compile_and_run(self, hmma_msl(HEAD + HELPER + KERNEL), RUNNER)

    def test_flagged_run_matches_reference(self):
        compile_and_run(self, hmma_msl(HEAD + HELPER + SEQ_KERNEL), SEQ_RUNNER)

    def test_calls_outside_a_chain_leave_its_slot_alone(self):
        compile_and_run(self, hmma_msl(HEAD + HELPER + CLOBBER_KERNEL), SEQ_RUNNER)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)
def waiting(i): return (i[0] | 0x3f << 52, i[1])   # waits on every scoreboard (control bits 116..121)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class TranslatorFlags(unittest.TestCase):
    """R2 = thread x * 16 (local size 32: the lane), LDSM.16.M88.4 R36, [R2]: rows 16 bytes apart, matrix m at 128 m. HMMA R46 = R36..39 x R4 + 0, then HMMA R46 = R36..39 x R6 +
    R46, whose result is stored: the first gets C = 0, A from where the LDSM read it, D left in slot 0; the second A kept, C from slot 0, D to the registers.
    Encodings: Ocarina of Time's NDVE network (cs_movie.bin), with the registers changed"""
    S2R = (0x000e260000002100, 0x0000000000097919)    # S2R R9, SR_TID.X
    IMAD = (0x000fe200078e02ff, 0x0000001009027824)   # IMAD R2, R9, 16, RZ
    LDSM = (0x000e220000000200, 0x000000000224783b)   # LDSM.16.M88.4 R36, [R2]
    HMMA0 = (0x041ff000000008ff, 0x00000004242e723c)  # HMMA.16816.F16 R46, R36, R4, RZ
    HMMA1 = (0x000fe2000000082e, 0x00000006242e723c)  # HMMA.16816.F16 R46, R36, R6, R46
    STS = (0x000fe8000000c800, 0x0000082e09008388)    # STS.U32 [R9 * 16 + 8], R46
    STS1 = (0x000fe8000000c800, 0x00000c2f09008388)   # STS.U32 [R9 * 16 + 12], R47: D's two words, as a network stores them
    EXIT = (0x000fea0003800000, 0x000000000000794d)

    def test_chain_reuse_and_direct_a(self):
        code = b''.join(inst(*waiting(i)) for i in (self.S2R, self.IMAD, self.LDSM, self.HMMA0, self.HMMA1, self.STS, self.STS1)) + inst(*self.EXIT)
        with tempfile.TemporaryDirectory(prefix='tensor_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=[0, 0, 3, 0x40, 0], shared=0x1000)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            sys.path.insert(0, str(TOOLS)); import dmsh_geometry
            dmsh_geometry.flatten_sets(work / 'k.spv')
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
        msl = r.stdout[r.stdout.index('kernel void main0'):]
        consts = dict(re.findall(r'int (_\d+) = (-?\d+);', msl))
        calls = [[a.strip() for a in c.split(',')] for c in re.findall(r'dmsh_hmma16816_f16\(([^()]*)\)', msl)]
        self.assertEqual(len(calls), 2)
        value = lambda a: int(consts.get(a, a))
        self.assertEqual([value(a) for a in calls[0][10:17] if a != calls[0][11]], [8 | 4 | 16, 0, 128, 256, 384, 16])   # C = 0, D to slot 0, A direct; offsets, stride
        self.assertEqual(value(calls[1][10]), 1 | 2 | 16)                                                               # A kept, C from slot 0
        copies = dict(re.findall(r'int (_\d+) = (_\d+);', msl))   # spirv-cross passes each argument through a copy of its own
        root = lambda a: root(copies[a]) if a in copies else a
        self.assertEqual(root(calls[0][11]), root(calls[1][11]))                                                        # the LDSM's row address
        self.assertRegex(msl, r'int %s = as_type<int>\(\w+\) \* 16;' % root(calls[0][11]))                              # R2


if __name__ == '__main__':
    unittest.main()
