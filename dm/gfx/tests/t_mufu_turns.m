// Ampere MUFU.SIN / COS take their argument in turns and use only its fraction. The translator lowers them as sin / cos((t - rint(t)) * 2pi) (InstEmitAmpereAlu.cs ATurnsToRadians);
// this test runs that lowering on the GPU, compiled with Metal's default fast math as the Mac libraries are, against a double-precision reference cos / sin(2pi * fract(t)).
// The angles are those of Donkey Kong Bananza's SSAO (a6f04605183d9915): its kernel turns by (10 (3 (x mod y) + x y) + 5.652) / 2pi turns for the pixel (x, y), computed in float
// as the shader does (one fma, one multiply), over the whole target at half the scene's size (838x471 for a 1676x943 scene, 1152x648 at the docked top of dynamic resolution), at its full-size effects (1676x943) and at 2560x1440. The earlier lowering,
// cos / sin(t * 2pi), returned (0, 0) past 2^20 turns, x y > 659,000: the lower right of a 1152x648 or larger target, where the kernel collapsed onto its pixel and the picture had no ambient
// occlusion; its count is printed for comparison, not checked (it is Metal's behaviour outside the range its fast sin / cos promise).
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void lowered(device const float *t [[buffer(0)]], device float4 *o [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
    "    float r = rint(t[i]);\n"            // the translator's IR: Round (GLSL RoundEven), FSub, FMul by 2pi, then Cos / Sin
    "    float f = t[i] - r;\n"
    "    float a = f * 6.283185482025146484375;\n"
    "    float b = t[i] * 6.283185482025146484375;\n"   // the earlier lowering
    "    o[i] = float4(cos(a), sin(a), cos(b), sin(b));\n"
    "}\n";

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void run(id<MTLDevice> dev, id<MTLComputePipelineState> ps, id<MTLCommandQueue> q, int w, int h) {
    size_t n = (size_t)w * h;
    id<MTLBuffer> bt = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bo = [dev newBufferWithLength:n * 16 options:MTLResourceStorageModeShared];
    float *t = bt.contents;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            long k = 3L * (y ? x % y : -1) + (long)x * y;           // the shader's x mod y is -1 for y = 0
            float a = fmaf((float)k, 10.0f, 5.651999950408935546875f);
            t[(size_t)y * w + x] = a * 0.15915493667125701904296875f;
        }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    [ce setBuffer:bt offset:0 atIndex:0];
    [ce setBuffer:bo offset:0 atIndex:1];
    [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [ce endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const float *o = bo.contents;
    double emax = 0, emax_old = 0; size_t dead = 0, dead_old = 0, at = 0;
    for (size_t i = 0; i < n; i++) {
        double fr = (double)t[i] - floor((double)t[i]);          // exact: t is a float
        double c = cos(2 * M_PI * fr), s = sin(2 * M_PI * fr);
        double e = fmax(fabs(o[4 * i] - c), fabs(o[4 * i + 1] - s));
        if (!(e <= emax)) { emax = e; at = i; }
        emax_old = fmax(emax_old, fmax(fabs(o[4 * i + 2] - c), fabs(o[4 * i + 3] - s)));
        if (hypot(o[4 * i], o[4 * i + 1]) < 0.5) dead++;
        if (hypot(o[4 * i + 2], o[4 * i + 3]) < 0.5) dead_old++;
    }
    printf("%dx%d: turns up to %.4g, largest error %.2g (pixel %zu,%zu), collapsed rotations %zu (earlier lowering: error %.2g, %zu collapsed)\n",
           w, h, t[n - 1], emax, at % w, at / w, dead, emax_old, dead_old);
    CHECK(emax < 2e-3, "%dx%d: cos / sin off by %g from the hardware's fraction at pixel (%zu, %zu), turns %.9g", w, h, emax, at % w, at / w, t[at]);
    CHECK(dead == 0, "%dx%d: %zu rotations collapsed to the origin", w, h, dead);
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("SKIP: no Metal device\n"); return 0; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:[MTLCompileOptions new] error:&err];   // default options: fast math, as xcrun metal compiles the Mac libraries
        if (!lib) { printf("FAIL: %s\n", err.description.UTF8String); return 1; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"lowered"];
        if (!fn) { printf("FAIL: no function lowered in the library\n"); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!ps) { printf("FAIL: %s\n", err.description.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        run(dev, ps, q, 838, 471);
        run(dev, ps, q, 1152, 648);
        run(dev, ps, q, 1676, 943);
        run(dev, ps, q, 2560, 1440);
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("t_mufu_turns: all passed\n");
    return 0;
}
