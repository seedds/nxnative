"""The host's NDVE movie network (dm/gfx/msl/ndve_engine.metal, nvn_ndve.inc; shaders.md, "The NDVE movie network by the host") against the translated shaders' arithmetic:
on this Mac's GPU, every kernel the host runs on random inputs and weights at the network's sizes, bit for bit against reference kernels that compute each HMMA.16816 through
dmsh_build.py's own helper (test_dmsh_hmma's helper rewritten by hmma_msl, the HMMA_BODY every translated shader gets), in PTX mma.m16n8k16's fragment layout, with the flags
TensorPass gives a chain (C = 0 and the accumulator slot on its first call, the slot on the others), in the order the game's code runs them (taps, then 16-channel blocks).
On the CPU: the kernels the host binds are in the source, and the packing of the input picture."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_dmsh_hmma import HEAD, HELPER, hmma_msl   # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / 'dm/gfx/msl/ndve_engine.metal'
INC = ROOT / 'dm/gfx/nvn/nvn_ndve.inc'

# The references (a function that calls the helper opens its body on a line of its own, as spirv-cross writes them: hmma_msl finds the callers so):
# one SIMD-group a tile of 16 output pixels of a row by 8 output channels (an m16n8 HMMA tile), A, B and C as each lane's fragment words
REF = r'''
static uint pk(device const half *p, ulong i, ulong j) { return uint(as_type<ushort>(p[i])) | uint(as_type<ushort>(p[j])) << 16; }
static uint pkt(threadgroup const half *p, uint i, uint j) { return uint(as_type<ushort>(p[i])) | uint(as_type<ushort>(p[j])) << 16; }
static half lo(uint w) { return as_type<half>(ushort(w & 0xffff)); }
static half hi(uint w) { return as_type<half>(ushort(w >> 16)); }
static int chain_flags(int c, int n) { return n == 1 ? 8 : c == 0 ? 8 | 4 : c == n - 1 ? 2 : 2 | 4; }   // TensorPass: C = 0 and D to slot 0, C and D in slot 0, C from it
struct Dims { uint W, H; };   // output size; for the transposed convolutions the input's

// KS = 3 (stride 1) or 2 (stride 2): out (x, y, n) = bias + sum over taps, then 16-channel blocks; D: dout (raw f16, out_16's) or out (ReLU'd)
template <int KS, int K, int N>
static void ref_conv(device const half *in, device half *out, device half *dout, device const half *w, device const half *b, constant Dims &a, uint tile, uint lane,
                     threadgroup spvUnsafeArray<uint, 1024> &scr, int blk)
{
    const uint tx = a.W / 16, nq = N / 8;
    const uint q = tile % nq, x0 = (tile / nq) % tx * 16, y = tile / nq / tx;
    if (y >= a.H) return;
    const int l = int(lane), g = l / 4, t = l % 4, n0 = int(q) * 8;
    const ulong IW = KS == 3 ? a.W + 2 : 2 * a.W + 2;
    int d0 = 0, d1 = 0, c = 0;
    const int calls = KS * KS * K / 16;
    for (int tap = 0; tap < KS * KS; tap++) for (int kb = 0; kb < K; kb += 16, c++) {
        const int dy = tap / KS, dx = tap % KS;
        int A[4];
        for (int r = 0; r < 4; r++) {
            const uint x = x0 + g + 8 * (r & 1); const int k = kb + 2 * t + 8 * (r >> 1);
            const ulong p = KS == 3 ? (ulong)(y + dy) * IW + x + dx : (ulong)(2 * y + dy + 1) * IW + 2 * x + dx + 1;
            A[r] = int(pk(in, p * K + k, p * K + k + 1));
        }
        int B0 = int(pk(w, (ulong)(tap * K + kb + 2 * t) * N + n0 + g, (ulong)(tap * K + kb + 2 * t + 1) * N + n0 + g));
        int B1 = int(pk(w, (ulong)(tap * K + kb + 2 * t + 8) * N + n0 + g, (ulong)(tap * K + kb + 2 * t + 9) * N + n0 + g));
        int f = chain_flags(c, calls), z = 0, e1;
        d0 = dmsh_hmma16816_f16(l, blk, A[0], A[1], A[2], A[3], B0, B1, d0, d1, f, z, z, z, z, z, z, e1, scr);
        d1 = e1;
    }
    for (int r = 0; r < 2; r++) {
        const uint x = x0 + g + 8 * r, dw = uint(r ? d1 : d0); const int n = n0 + 2 * t;
        if (dout) { dout[((ulong)y * a.W + x) * N + n] = lo(dw); dout[((ulong)y * a.W + x) * N + n + 1] = hi(dw); continue; }
        const float v0 = max(float(lo(dw)) + float(b[n]), 0.0f), v1 = max(float(hi(dw)) + float(b[n + 1]), 0.0f);
        device half *o = out + ((ulong)(y + 1) * (a.W + 2) + x + 1) * N + n;
        o[0] = half(v0); o[1] = half(v1);
    }
}
// 2x2 stride-2 transposed: out (2x + dx, 2y + dy, n) = the chain over the 16-channel blocks of in(x, y) x w[2 dy + dx] from C = skip(2x + dx, 2y + dy, n); bias, ReLU
template <int K, int N>
static void ref_up(device const half *in, device const half *skip, device half *out, device const half *w, device const half *b, constant Dims &a, uint tile, uint lane,
                   threadgroup spvUnsafeArray<uint, 1024> &scr, int blk)
{
    const uint tx = a.W / 16, nq = N / 8;
    const uint q = tile % nq, tap = (tile / nq) % 4, x0 = (tile / nq / 4) % tx * 16, y = tile / nq / 4 / tx;
    if (y >= a.H) return;
    const int l = int(lane), g = l / 4, t = l % 4, n0 = int(q) * 8, dy = int(tap) >> 1, dx = int(tap) & 1;
    const ulong IW = a.W + 2, OW = 2 * a.W + 2;
    int d0, d1;
    { const ulong p0 = (ulong)(2 * y + dy + 1) * OW + 2 * (x0 + g) + dx + 1, p1 = p0 + 16;
      d0 = int(pk(skip, p0 * N + n0 + 2 * t, p0 * N + n0 + 2 * t + 1)); d1 = int(pk(skip, p1 * N + n0 + 2 * t, p1 * N + n0 + 2 * t + 1)); }
    for (int kb = 0, c = 0; kb < K; kb += 16, c++) {
        int A[4];
        for (int r = 0; r < 4; r++) {
            const ulong p = (ulong)(y + 1) * IW + x0 + g + 8 * (r & 1) + 1; const int k = kb + 2 * t + 8 * (r >> 1);
            A[r] = int(pk(in, p * K + k, p * K + k + 1));
        }
        int B0 = int(pk(w, (ulong)(tap * K + kb + 2 * t) * N + n0 + g, (ulong)(tap * K + kb + 2 * t + 1) * N + n0 + g));
        int B1 = int(pk(w, (ulong)(tap * K + kb + 2 * t + 8) * N + n0 + g, (ulong)(tap * K + kb + 2 * t + 9) * N + n0 + g));
        int f = c == 0 ? 4 : c == K / 16 - 1 ? 2 : 2 | 4, z = 0, e1;
        d0 = dmsh_hmma16816_f16(l, blk, A[0], A[1], A[2], A[3], B0, B1, d0, d1, f, z, z, z, z, z, z, e1, scr);
        d1 = e1;
    }
    for (int r = 0; r < 2; r++) {
        const uint x = x0 + g + 8 * r, dw = uint(r ? d1 : d0); const int n = n0 + 2 * t;
        const float v0 = max(float(lo(dw)) + float(b[n]), 0.0f), v1 = max(float(hi(dw)) + float(b[n + 1]), 0.0f);
        device half *o = out + ((ulong)(2 * y + dy + 1) * OW + 2 * x + dx + 1) * N + n;
        o[0] = half(v0); o[1] = half(v1);
    }
}
#define REFK(name, call) \
kernel void name(device const half *in [[buffer(0)]], device half *out [[buffer(1)]], device const half *w [[buffer(2)]], device const half *b [[buffer(3)]], \
                 constant Dims &a [[buffer(4)]], device const half *skip [[buffer(5)]], device half *dout [[buffer(6)]], uint lane [[thread_index_in_simdgroup]], \
                 uint sg [[simdgroup_index_in_threadgroup]], uint tg [[threadgroup_position_in_grid]]) { \
    threadgroup spvUnsafeArray<uint, 1024> scr; call; }
REFK(ref_conv3_16_16, (ref_conv<3, 16, 16>(in, out, nullptr, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_conv3_32_32, (ref_conv<3, 32, 32>(in, out, nullptr, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_conv3_64_64, (ref_conv<3, 64, 64>(in, out, nullptr, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_down_16_32, (ref_conv<2, 16, 32>(in, out, nullptr, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_down_32_64, (ref_conv<2, 32, 64>(in, out, nullptr, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_up_64_32, (ref_up<64, 32>(in, skip, out, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_up_32_16, (ref_up<32, 16>(in, skip, out, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
REFK(ref_conv3_raw, (ref_conv<3, 16, 16>(in, out, dout, w, b, a, tg * 4 + sg, lane, scr, int(sg) * 256)))
// 5a7d69b0's unpacking: output pixel (2x + sx, 2y + sy), s = 2 sy + sx, component c = (rgba(c) + D[n]) + bias[n], n = (2s, 2s + 1, 8 + 2s, 9 + 2s)[c]
kernel void ref_unpack(device const half *d [[buffer(0)]], device const half *b [[buffer(3)]], constant Dims &a [[buffer(4)]], texture2d<float, access::read> rgba [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= 2 * a.W || p.y >= 2 * a.H) return;
    const uint s = 2 * (p.y & 1) + (p.x & 1); device const half *dd = d + ((ulong)(p.y / 2) * a.W + p.x / 2) * 16;
    const float4 t = rgba.read(p); const uint n[4] = {2 * s, 2 * s + 1, 8 + 2 * s, 9 + 2 * s};
    float4 v; for (int c = 0; c < 4; c++) v[c] = (t[c] + float(dd[n[c]])) + float(b[n[c]]);
    dst.write(v, p);
}
// The resampler's network: one SIMD-group an 8x8 output block, six blocks of H_g = W1_g X (one call each 16 x 8, C = 0), Y_j = H_0 W2_0j + H_1 W2_1j (a chain of two),
// ReLU between blocks; then the epilogue of 4f41ccf2 as the engine has it (a sample of the picture plus the delta, dithered)
struct RsConst { int4 rin; int4 rout; float4 s2; float4 s3; int4 c4; int4 c5; };
kernel void ref_resample(device const half *luma [[buffer(0)]], device const half *w [[buffer(2)]], constant RsConst &cc [[buffer(3)]],
                         texture2d<float, access::sample> pic [[texture(0)]], texture2d<float, access::read> dith [[texture(1)]], texture2d<float, access::write> dst [[texture(2)]],
                         uint sg [[simdgroup_index_in_threadgroup]], uint2 tgp [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]])
{
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear, min_filter::nearest, mag_filter::linear, mip_filter::linear);
    threadgroup spvUnsafeArray<uint, 1024> scr;
    threadgroup half X[4][16][16], H[4][2][16][16], Y[4][16][16];
    const int blk = int(sg) * 256, l = int(lane), g = l / 4, t = l % 4;
    const uint bx = tgp.x * 4 + sg, by = tgp.y;
    for (uint i = lane; i < 256; i += 32) {
        const int r = int(i >> 4), c = int(i & 15);
        X[sg][r][c] = luma[min(max(0, int(6 * by) - 5 + r), cc.rin.w) * 1920 + min(max(0, int(6 * bx) - 5 + c), cc.rin.z)];
    }
    simdgroup_barrier(mem_flags::mem_threadgroup);
    threadgroup const half *x = &X[sg][0][0];
    for (int k6 = 0; k6 < 6; k6++) {
        device const half *wb = w + 1024 * k6;
        for (int gg = 0; gg < 2; gg++) for (int nb = 0; nb < 2; nb++) {   // stage 1: H_gg[m][8 nb + col] = sum_k W1[m][k] X[k][8 nb + col]
            int A[4];
            for (int r = 0; r < 4; r++) { const int m = g + 8 * (r & 1), k = 2 * t + 8 * (r >> 1);
                A[r] = int(pk(wb, 256 * gg + (k < 8 ? 0 : 128) + 8 * m + k % 8, 256 * gg + (k < 8 ? 0 : 128) + 8 * m + k % 8 + 1)); }
            int B0 = int(pkt(x, (2 * t) * 16 + 8 * nb + g, (2 * t + 1) * 16 + 8 * nb + g)), B1 = int(pkt(x, (2 * t + 8) * 16 + 8 * nb + g, (2 * t + 9) * 16 + 8 * nb + g));
            int z = 0, f = 8, e1;
            uint d0 = uint(dmsh_hmma16816_f16(l, blk, A[0], A[1], A[2], A[3], B0, B1, z, z, f, z, z, z, z, z, z, e1, scr)), d1 = uint(e1);
            H[sg][gg][g][8 * nb + 2 * t] = lo(d0); H[sg][gg][g][8 * nb + 2 * t + 1] = hi(d0); H[sg][gg][g + 8][8 * nb + 2 * t] = lo(d1); H[sg][gg][g + 8][8 * nb + 2 * t + 1] = hi(d1);
        }
        simdgroup_barrier(mem_flags::mem_threadgroup);
        for (int j = 0; j < (k6 < 5 ? 2 : 1); j++) {   // stage 2: Y[m][8 j + n] = sum_gg sum_k H_gg[m][k] W2_gg[k][j][n]
            int d0 = 0, d1 = 0;
            for (int gg = 0; gg < 2; gg++) {
                int A[4];
                for (int r = 0; r < 4; r++) { const int m = g + 8 * (r & 1), k = 2 * t + 8 * (r >> 1); A[r] = int(pkt(&H[sg][gg][0][0], m * 16 + k, m * 16 + k + 1)); }
                device const half *wp = wb + 512 + 256 * gg + 128 * j;
                int B0 = int(pk(wp, 8 * (2 * t) + g, 8 * (2 * t + 1) + g)), B1 = int(pk(wp, 8 * (2 * t + 8) + g, 8 * (2 * t + 9) + g));
                int f = gg == 0 ? 8 | 4 : 2, z = 0, e1;
                d0 = dmsh_hmma16816_f16(l, blk, A[0], A[1], A[2], A[3], B0, B1, d0, d1, f, z, z, z, z, z, z, e1, scr); d1 = e1;
            }
            const half v[4] = {lo(uint(d0)), hi(uint(d0)), lo(uint(d1)), hi(uint(d1))};
            for (int e = 0; e < 4; e++) Y[sg][g + 8 * (e >> 1)][8 * j + 2 * t + (e & 1)] = k6 < 5 ? half(max(float(v[e]), 0.0f)) : v[e];
        }
        simdgroup_barrier(mem_flags::mem_threadgroup);
        for (uint i = lane; i < 256; i += 32) X[sg][i >> 4][i & 15] = Y[sg][i >> 4][i & 15];
        simdgroup_barrier(mem_flags::mem_threadgroup);
    }
    for (uint i = lane; i < 64; i += 32) {   // the delta of pixel (8 bx + col, 8 by + row) is Y6[row][col]
        const uint row = i / 8, col = i % 8, px = 8 * bx + col, py = 8 * by + row;
        const float delta = float(Y[sg][row][col]);
        const float2 uv = float2((float(px) + 0.5f) * cc.s2.z, (float(py) + 0.5f) * cc.s2.w);
        const float3 s = pic.sample(smp, uv, level(0.0f)).xyz;
        const float3 off = fma(dith.read(uint2(px & 511, py & 511)).xyz, 2.0f, -1.0f);
        const float3 v = -0.0f + (s + delta);
        const float3 k = rint(fma(v, 255.0f, off));
        dst.write(float4(k * 0.0039215688593685626983642578125f, 1.0f), uint2(px, cc.c4.w ? uint(cc.c4.z - int(py)) : py));
    }
}
'''

RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdlib.h>
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLLibrary> eng, ref;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint16_t f2h(float f) { __fp16 v = (__fp16)f; uint16_t h; memcpy(&h, &v, 2); return h; }
static float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }
static float uni(void) { return ((int)(rnd() % 20001) - 10000) / 10000.0f; }   // in [-1, 1]
static id<MTLComputePipelineState> pso(id<MTLLibrary> lib, NSString *name) {
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithName:name]; if (!f) { printf("no function %s\n", name.UTF8String); exit(1); }
    id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:f error:&e]; if (!p) { printf("%s: %s\n", name.UTF8String, e.localizedDescription.UTF8String); exit(1); }
    return p;
}
// a padded f16 NHWC tensor (zero border, 16 spare rows) of random activations: about a third zero (ReLU'd), the rest in [-1, 1] (negative ones too: signed zeros of products)
static id<MTLBuffer> tensor(int W, int H, int C, int fill) {
    size_t n = (size_t)(W + 2) * (H + 2 + 16) * C;
    id<MTLBuffer> b = [dev newBufferWithLength:n * 2 options:MTLResourceStorageModeShared]; memset(b.contents, 0, n * 2);
    uint16_t *h = b.contents;
    if (fill) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < C; c++) h[((size_t)(y + 1) * (W + 2) + x + 1) * C + c] = rnd() % 3 == 0 ? 0 : f2h(uni());
    return b;
}
static id<MTLBuffer> weights(int n, float scale) {
    id<MTLBuffer> b = [dev newBufferWithLength:n * 2 options:MTLResourceStorageModeShared]; uint16_t *h = b.contents;
    for (int i = 0; i < n; i++) h[i] = rnd() % 16 == 0 ? 0 : f2h(uni() * scale);
    return b;
}
static id<MTLTexture> tex(MTLPixelFormat f, int w, int h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; d.storageMode = MTLStorageModeShared; return [dev newTextureWithDescriptor:d];
}
static void fill_tex(id<MTLTexture> t, int mask10) {
    size_t n = t.width * t.height; uint32_t *p = malloc(n * 4);
    for (size_t i = 0; i < n; i++) p[i] = mask10 ? (rnd() & 0x3fffffff) | (3u << 30) : rnd();
    [t replaceRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0 withBytes:p bytesPerRow:t.width * 4]; free(p);
}
typedef void (^enc_fn)(id<MTLComputeCommandEncoder>);
static void run(enc_fn f) {
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; f(ce); [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { printf("GPU error: %s\n", cb.error.localizedDescription.UTF8String); exit(1); }
}
static int fails;
static void same(const char *what, const void *a, const void *b, size_t n, int esz) {
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i++) if (memcmp((const char *)a + i * esz, (const char *)b + i * esz, esz)) { if (!bad) first = i; bad++; }
    if (bad) { fails++; printf("FAIL %s: %zu of %zu differ (first at %zu)\n", what, bad, n, first); } else printf("ok   %s: %zu identical\n", what, n);
}
typedef struct { uint32_t W, H, tiles_x, tiles_y; } LayerArgs;
typedef struct { uint32_t W, H; } Dims;
// a convolution layer: engine kernel (its dispatch as nvn_ndve.inc encodes it) and reference, on the same input; KS 3 or 2 (stride 2), up: the transposed one
static void layer(const char *ek, const char *rk, int KS, int up, int K, int N, int ry, int W, int H, int staged) {
    // W, H: the output's size (the input's for up)
    const int IW = KS == 2 && !up ? 2 * W : W, IH = KS == 2 && !up ? 2 * H : H, OW = up ? 2 * W : W, OH = up ? 2 * H : H;
    id<MTLBuffer> in = tensor(IW, IH, K, 1), skip = up ? tensor(OW, OH, N, 1) : nil, oe = tensor(OW, OH, N, 0), orf = tensor(OW, OH, N, 0);
    const int taps = up ? 4 : KS * KS; id<MTLBuffer> w = weights(taps * K * N, 3.0f / (taps * K)), b = weights(N, 0.5f);
    LayerArgs a = {(uint32_t)W, (uint32_t)H, (uint32_t)W / 8, (uint32_t)((H + ry - 1) / ry)}; Dims d = {(uint32_t)W, (uint32_t)H};
    id<MTLComputePipelineState> pe = pso(eng, @(ek)), pr = pso(ref, @(rk));
    run(^(id<MTLComputeCommandEncoder> ce) {
        [ce setComputePipelineState:pe]; [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:oe offset:0 atIndex:1]; [ce setBuffer:w offset:0 atIndex:2]; [ce setBuffer:b offset:0 atIndex:3];
        [ce setBytes:&a length:sizeof a atIndex:4]; if (skip) [ce setBuffer:skip offset:0 atIndex:5];
        uint32_t groups = staged ? (a.W + 31) / 32 * a.tiles_y : (a.tiles_x * a.tiles_y + 3) / 4;
        [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        [ce setComputePipelineState:pr]; [ce setBuffer:orf offset:0 atIndex:1]; [ce setBytes:&d length:sizeof d atIndex:4]; [ce setBuffer:skip ? skip : in offset:0 atIndex:5]; [ce setBuffer:nil offset:0 atIndex:6];
        uint32_t tiles = (uint32_t)(W / 16) * (N / 8) * H * (up ? 4 : 1);
        [ce dispatchThreadgroups:MTLSizeMake((tiles + 3) / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    });
    char what[160]; snprintf(what, sizeof what, "%s (%dx%dx%d -> %dx%dx%d)", ek, IW, IH, K, OW, OH, N);
    same(what, oe.contents, orf.contents, oe.length / 2, 2);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice(); if (!dev || ![dev supportsFamily:MTLGPUFamilyApple7]) { printf("no Apple7 Metal device\n"); return 2; }
        q = [dev newCommandQueue]; NSError *e = nil;
        eng = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e]; ref = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&e];
        if (!eng || !ref) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        // the network's layers at their sizes (nvn_ndve.inc k_ndve_l)
        layer("conv3s_16_16", "ref_conv3_16_16", 3, 0, 16, 16, 4, 960, 540, 1);
        layer("conv3s_32_32", "ref_conv3_32_32", 3, 0, 32, 32, 2, 480, 270, 1);
        layer("conv3s_64_64", "ref_conv3_64_64", 3, 0, 64, 64, 1, 240, 135, 1);
        layer("down_16_32", "ref_down_16_32", 2, 0, 16, 32, 2, 480, 270, 0);
        layer("down_32_64", "ref_down_32_64", 2, 0, 32, 64, 1, 240, 135, 0);
        layer("up_64_32", "ref_up_64_32", 2, 1, 64, 32, 1, 240, 135, 0);
        layer("up_32_16", "ref_up_32_16", 2, 1, 32, 16, 2, 480, 270, 0);
        // the last layer: the convolution's D unpacked and added to the picture, into RGB10A2
        {
            id<MTLBuffer> in = tensor(960, 540, 16, 1), w = weights(9 * 16 * 16, 3.0f / 144), b = weights(16, 0.05f), dr = [dev newBufferWithLength:960 * 540 * 16 * 2 options:MTLResourceStorageModeShared];
            id<MTLTexture> rgba = tex(MTLPixelFormatRGBA8Unorm, 1920, 1080), oe = tex(MTLPixelFormatRGB10A2Unorm, 1920, 1080), orf = tex(MTLPixelFormatRGB10A2Unorm, 1920, 1080);
            fill_tex(rgba, 0);
            LayerArgs a = {960, 540, 120, 135}; Dims d = {960, 540};
            id<MTLComputePipelineState> pe = pso(eng, @"out_16"), pr = pso(ref, @"ref_conv3_raw"), pu = pso(ref, @"ref_unpack");
            run(^(id<MTLComputeCommandEncoder> ce) {
                [ce setComputePipelineState:pe]; [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:w offset:0 atIndex:2]; [ce setBuffer:b offset:0 atIndex:3]; [ce setBytes:&a length:sizeof a atIndex:4];
                [ce setTexture:rgba atIndex:0]; [ce setTexture:oe atIndex:1];
                [ce dispatchThreadgroups:MTLSizeMake((a.tiles_x * a.tiles_y + 3) / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
                [ce setComputePipelineState:pr]; [ce setBuffer:in offset:0 atIndex:1]; [ce setBytes:&d length:sizeof d atIndex:4]; [ce setBuffer:in offset:0 atIndex:5]; [ce setBuffer:dr offset:0 atIndex:6];
                [ce dispatchThreadgroups:MTLSizeMake(960 / 16 * 2 * 540 / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
                [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [ce setComputePipelineState:pu]; [ce setBuffer:dr offset:0 atIndex:0]; [ce setTexture:orf atIndex:1];
                [ce dispatchThreads:MTLSizeMake(1920, 1080, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
            });
            uint32_t *pa = malloc(1920 * 1080 * 4), *pb = malloc(1920 * 1080 * 4);
            [oe getBytes:pa bytesPerRow:1920 * 4 fromRegion:MTLRegionMake2D(0, 0, 1920, 1080) mipmapLevel:0]; [orf getBytes:pb bytesPerRow:1920 * 4 fromRegion:MTLRegionMake2D(0, 0, 1920, 1080) mipmapLevel:0];
            same("out_16 (960x540x16 + RGBA8 1920x1080 -> RGB10A2)", pa, pb, 1920 * 1080, 4); free(pa); free(pb);
        }
        // the input packing: f16 of the RGBA8 read's float, c / 255 correctly rounded
        {
            id<MTLTexture> rgba = tex(MTLPixelFormatRGBA8Unorm, 1920, 1080); fill_tex(rgba, 0);
            id<MTLBuffer> out = tensor(960, 540, 16, 0); LayerArgs a = {960, 540, 0, 0};
            run(^(id<MTLComputeCommandEncoder> ce) {
                [ce setComputePipelineState:pso(eng, @"pack_input")]; [ce setTexture:rgba atIndex:0]; [ce setBuffer:out offset:0 atIndex:1]; [ce setBytes:&a length:sizeof a atIndex:4];
                [ce dispatchThreadgroups:MTLSizeMake(30, 135, 1) threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];
            });
            uint8_t *px = malloc(1920 * 1080 * 4); [rgba getBytes:px bytesPerRow:1920 * 4 fromRegion:MTLRegionMake2D(0, 0, 1920, 1080) mipmapLevel:0];
            id<MTLBuffer> want = tensor(960, 540, 16, 0); uint16_t *h = want.contents;
            for (int y = 0; y < 540; y++) for (int x = 0; x < 960; x++) for (int s = 0; s < 4; s++) for (int c = 0; c < 4; c++)
                h[((size_t)(y + 1) * 962 + x + 1) * 16 + 4 * s + c] = f2h((float)px[((size_t)(2 * y + (s >> 1)) * 1920 + 2 * x + (s & 1)) * 4 + c] / 255.0f);
            same("pack_input (RGBA8 1920x1080 -> 960x540x16)", out.contents, want.contents, out.length / 2, 2); free(px);
        }
        // the resampler: the engine's luma of a random picture, then the network and its epilogue, the engine's against the reference
        {
            id<MTLTexture> pic = tex(MTLPixelFormatRGB10A2Unorm, 1920, 1080), dith = tex(MTLPixelFormatRGB10A2Unorm, 512, 512), oe = tex(MTLPixelFormatRGBA8Unorm, 2560, 1440), orf = tex(MTLPixelFormatRGBA8Unorm, 2560, 1440);
            fill_tex(pic, 1); fill_tex(dith, 1);
            id<MTLBuffer> luma = [dev newBufferWithLength:1920 * 1080 * 2 options:MTLResourceStorageModeShared], w = weights(6 * 1024, 0.35f);
            int32_t c[24] = {0, 0, 1919, 1079, 0, 0, 2559, 1439}; float s2[4] = {0, 0, 1.0f / 2560, 1.0f / 1440}; memcpy(c + 8, s2, 16); c[18] = 1439; c[19] = 0; c[20] = 1; const int32_t *cp = c;
            run(^(id<MTLComputeCommandEncoder> ce) {
                [ce setComputePipelineState:pso(eng, @"rs_luma")]; [ce setTexture:pic atIndex:0]; [ce setBuffer:luma offset:0 atIndex:1];
                [ce dispatchThreadgroups:MTLSizeMake(60, 135, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
                [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
                for (int i = 0; i < 2; i++) {
                    [ce setComputePipelineState:pso(i ? ref : eng, i ? @"ref_resample" : @"resample")];
                    [ce setBuffer:luma offset:0 atIndex:0]; [ce setBuffer:w offset:0 atIndex:2]; [ce setBytes:cp length:96 atIndex:3];
                    [ce setTexture:pic atIndex:0]; [ce setTexture:dith atIndex:1]; [ce setTexture:i ? orf : oe atIndex:2];
                    [ce dispatchThreadgroups:MTLSizeMake(80, 180, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
                }
            });
            uint32_t *pa = malloc(2560 * 1440 * 4), *pb = malloc(2560 * 1440 * 4);
            [oe getBytes:pa bytesPerRow:2560 * 4 fromRegion:MTLRegionMake2D(0, 0, 2560, 1440) mipmapLevel:0]; [orf getBytes:pb bytesPerRow:2560 * 4 fromRegion:MTLRegionMake2D(0, 0, 2560, 1440) mipmapLevel:0];
            same("resample (RGB10A2 1920x1080 -> RGBA8 2560x1440)", pa, pb, 2560 * 1440, 4); free(pa); free(pb);
        }
        printf("%d failed\n", fails);
        return fails != 0;
    }
}
'''


def kernels_bound():   # the kernels nvn_ndve.inc binds by name
    m = re.search(r'k_ndve_kern\[NK_N\] = \{([^}]*)\}', INC.read_text())
    return re.findall(r'"(\w+)"', m.group(1))


class EngineSource(unittest.TestCase):
    def test_every_kernel_the_host_binds_is_in_the_source(self):
        src = ENGINE.read_text()
        defined = set(re.findall(r'\bkernel void (\w+)\(', src)) | set(re.findall(r'^(?:CONV|CONV3S|UP)\((\w+),', src, re.M))
        bound = kernels_bound()
        self.assertEqual(len(bound), 12)
        self.assertEqual(sorted(set(bound) - defined), [])

    def test_weight_offsets_follow_from_the_layers(self):
        """ndve_offsets' rule (each layer's weights, then its biases, each padded to 256 bytes but the last biases) on the include's layer and shape tables gives the offsets
        at which Ocarina of Time binds each network dispatch's weights and biases (measured in the game, PR #1894) and ends at the 0x3c320 bytes of weights the NDVE archive's
        network table states"""
        src = INC.read_text()
        kinds = re.findall(r'NK_(\w+)', re.search(r'enum \{ (NK_PACK[^}]*)\}', src).group(1))
        shape = [tuple(map(int, t)) for t in re.findall(r'\{(\d+), (\d+), (\d+)\}', re.search(r'k_ndve_shape\[NK_N\]\[3\] = \{(.*)\};', src).group(1))]
        layers = re.findall(r'\{NK_(\w+), [^{}]*?, (\d+)\}', re.search(r'k_ndve_l\[\] = \{(.*?)\};', src, re.S).group(1))
        w, b, o = {}, {}, 0
        for i, (k, p) in enumerate(layers[1:], 1):
            taps, ki, n = shape[kinds.index(k)]
            w[int(p)] = o; o += (taps * ki * n * 2 + 255) & ~255
            b[int(p)] = o; o += n * 2 if i == len(layers) - 1 else (n * 2 + 255) & ~255
        self.assertEqual([w[p] for p in range(1, 13)], [0x0, 0x1300, 0x2600, 0x3700, 0x8000, 0xc100, 0x1e200, 0x30300, 0x34400, 0x38d00, 0x39e00, 0x3b100])
        self.assertEqual([b[p] for p in range(1, 13)], [0x1200, 0x2500, 0x3600, 0x7f00, 0xc000, 0x1e100, 0x30200, 0x34300, 0x38c00, 0x39d00, 0x3b000, 0x3c300])
        self.assertEqual(o, int(re.search(r'NDVE_WBYTES = (0x[0-9a-f]+)', src).group(1), 16))
        self.assertEqual(o, 0x3c320)

    def test_safe_math(self):   # the epilogues' additions in the game's order, nothing contracted: the pragma comes before anything else
        self.assertTrue(ENGINE.read_text().split('\n#pragma METAL fp math_mode(safe)\n')[0].lstrip().startswith('//'))


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class EngineOnGpu(unittest.TestCase):
    def test_bit_exact_with_the_translated_hmma(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            # the engine as the host build compiles it (host_msl.py), the references with the chain's flags (test_dmsh_hmma.compile_and_run)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-o', str(d / 'eng.metallib'), str(ENGINE)], check=True)
            (d / 'ref.metal').write_text('#pragma METAL fp math_mode(safe)\n' + hmma_msl(HEAD + HELPER + REF))
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(d / 'ref.metal'), '-o', str(d / 'ref.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'ref.air'), '-o', str(d / 'ref.metallib')], check=True)
            (d / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
            r = subprocess.run([str(d / 'run'), str(d / 'eng.metallib'), str(d / 'ref.metallib')], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
