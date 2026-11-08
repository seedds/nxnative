// A texel fetch past the edge of a sampled 2D texture (dmsh_rewrite.py fetch_clamp): a kernel in a reduced translator shape reads an 8x6 float texture (2 mip levels) and an
// 8x6 depth texture, declared texture2d<float> as the translator declares a depth texture, at points inside, past each edge, past a corner and far out, through fetch_clamp.
// Every texel holds a distinct value, so each read must return exactly the texel nearest inside the level it reads, on mip level 1 too (the clamp takes that level's size).
// A read-only image beside them (the translator's <stage>_i_ name, also declared texture2d) must keep its read as written. Without the clamp, Metal returns 0 past the edge.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static int checks, failed;
#define EXPECT(name, want, got) do { double w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %g, got %g\n", name, w, g); failed++; } } while (0)

// x, y and mip level of each read; the kernel writes one float per read and texture
static const int P[][3] = { {3, 4, 0}, {-3, 4, 0}, {11, 4, 0}, {3, -2, 0}, {3, 9, 0}, {-1, -1, 0}, {8, 8, 0}, {1000, -1000, 0}, {-2147483000, 5, 0}, {6, 1, 1}, {-1, 2, 1}, {2, 5, 1} };
enum { NP = sizeof P / sizeof *P, W = 8, H = 6 };   // not square: a width and height swapped in the clamp reads the wrong texel

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    NSMutableString *src = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(texture2d<float> cp_t_tcb_8 [[texture(0)]], texture2d<float> cp_t_tcb_C [[texture(1)]], texture2d<float> cp_i_tcb_4A_ [[texture(2)]], constant int4* p [[buffer(0)]],"
        " device float* o [[buffer(1)]], uint i [[thread_position_in_grid]])\n{\n"
        "    int _10 = p[i].x;\n    int _11 = p[i].y;\n    uint _12 = uint(p[i].z);\n"
        "    o[3 * i] = cp_t_tcb_8.read(uint2(int2(_10, _11)), _12).x;\n"];
    [src appendString:@"    o[3 * i + 1] = _12 == 0u ? cp_t_tcb_C.read(uint2(int2(_10, _11)), 0).x : -1.0;\n"];
    [src appendString:@"    o[3 * i + 2] = cp_i_tcb_4A_.read(uint2(int2(_10, _11))).x;\n}\n"];
    char dir[] = "/tmp/t_fetch_clamp.XXXXXX"; if (!mkdtemp(dir)) return 1;
    NSString *root = @(dir); if (!root) return 1;
    NSString *in = [root stringByAppendingPathComponent:@"k.metal"], *out = [root stringByAppendingPathComponent:@"kc.metal"];
    [src writeToFile:in atomically:NO encoding:NSUTF8StringEncoding error:nil];
    // fetch_clamp from its module, as test_dmsh_fetch_clamp.py takes it
    NSTask *py = [NSTask new]; py.executableURL = [NSURL fileURLWithPath:@"/usr/bin/env"];
    NSString *prog = @"import sys; sys.path.insert(0, '../tools'); from dmsh_rewrite import fetch_clamp; "
        "open(sys.argv[2], 'w').write(fetch_clamp(open(sys.argv[1]).read()))";
    py.arguments = @[@"python3", @"-c", prog, in, out];
    if (![py launchAndReturnError:&e]) { fprintf(stderr, "fetch_clamp: %s\n", e.localizedDescription.UTF8String); return 1; }
    [py waitUntilExit];
    if (py.terminationStatus) { fprintf(stderr, "fetch_clamp failed\n"); return 1; }
    NSString *clamped = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:nil];
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    if (!clamped) { fprintf(stderr, "fetch_clamp wrote nothing\n"); return 1; }
    EXPECT("fetch_clamp rewrote the two texture reads", 2, (double)([clamped componentsSeparatedByString:@"get_width"].count - 1));
    EXPECT("fetch_clamp left the image read as written", 1, (double)[clamped containsString:@"cp_i_tcb_4A_.read(uint2(int2(_10, _11))).x"]);
    id<MTLLibrary> lib = [dev newLibraryWithSource:clamped options:nil error:&e];
    if (!lib) { fprintf(stderr, "compile: %s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"]; if (!fn) { fprintf(stderr, "no main0\n"); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&e];
    if (!ps) { fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }

    // the float texture: level 0 holds 100 + x + 8y, level 1 (4x3) holds 500 + x + 4y; the depth texture holds (x + 8y + 1) / 128
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:W height:H mipmapped:YES];
    td.mipmapLevelCount = 2; td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    float l0[W * H], l1[4 * 3];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) l0[x + W * y] = 100 + x + W * y;
    for (int y = 0; y < 3; y++) for (int x = 0; x < 4; x++) l1[x + 4 * y] = 500 + x + 4 * y;
    [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:l0 bytesPerRow:W * 4];
    [t replaceRegion:MTLRegionMake2D(0, 0, 4, 3) mipmapLevel:1 withBytes:l1 bytesPerRow:16];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> d = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> db = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    for (int i = 0; i < W * H; i++) ((float *)db.contents)[i] = (i + 1) / 128.0f;
    id<MTLBuffer> pb = [dev newBufferWithLength:sizeof(int) * 4 * NP options:MTLResourceStorageModeShared], ob = [dev newBufferWithLength:sizeof(float) * 3 * NP options:MTLResourceStorageModeShared];
    for (int i = 0; i < NP; i++) for (int c = 0; c < 3; c++) ((int *)pb.contents)[4 * i + c] = P[i][c];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromBuffer:db sourceOffset:0 sourceBytesPerRow:W * 4 sourceBytesPerImage:W * H * 4 sourceSize:MTLSizeMake(W, H, 1) toTexture:d destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [bl endEncoding];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setTexture:t atIndex:0]; [ce setTexture:d atIndex:1]; [ce setTexture:t atIndex:2]; [ce setBuffer:pb offset:0 atIndex:0]; [ce setBuffer:ob offset:0 atIndex:1];
    [ce dispatchThreads:MTLSizeMake(NP, 1, 1) threadsPerThreadgroup:MTLSizeMake(NP, 1, 1)]; [ce endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "command buffer failed\n"); return 1; }
    const float *o = ob.contents;
    for (int i = 0; i < NP; i++) {
        int lv = P[i][2], nw = W >> lv, nh = H >> lv;
        int x = P[i][0] < 0 ? 0 : P[i][0] > nw - 1 ? nw - 1 : P[i][0], y = P[i][1] < 0 ? 0 : P[i][1] > nh - 1 ? nh - 1 : P[i][1];
        char name[128]; snprintf(name, sizeof name, "float texture read at (%d, %d) level %d returns the texel at (%d, %d)", P[i][0], P[i][1], lv, x, y);
        EXPECT(name, lv ? 500 + x + 4 * y : 100 + x + W * y, o[3 * i]);
        if (!lv) { snprintf(name, sizeof name, "depth texture read at (%d, %d) returns the texel at (%d, %d)", P[i][0], P[i][1], x, y); EXPECT(name, (x + W * y + 1) / 128.0f, o[3 * i + 1]); }
    }
    printf("%s: %d reads of each texture past and inside the edges\n== %d checks, %d failed ==\n", dev.name.UTF8String, NP, checks, failed);
    return failed != 0;
} }
