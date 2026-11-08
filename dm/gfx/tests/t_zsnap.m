// A fragment's gl_FragCoord.z against the depth the rasterizer stored for the same triangle (dmsh_build.py frag_z_snap). NVIDIA stores the z its pixel shader reads; Apple's GPU stores a value
// at or below the [[position]].z its fragment shader reads, so a shader testing a fragment against a depth image of its own surface (Ocarina of Time's mist: discard when z > the stored
// depth) fails at scattered pixels. This test draws non-overlapping triangles depth-only into a Depth32Float target, then the same triangles again with a fragment shader that reads the
// stored depth at its pixel and counts: z above it (the failure, printed), z below it (none: the window dm_zsnap uses looks only below), and z through dm_zsnap above it (none). A second
// reader draws the same triangles 4e-6 farther, a distinct surface: through dm_zsnap it must stay above the stored depth at every pixel. dm_zsnap comes from the rewrite's source,
// written by test_dmsh_zsnap.py --fixture <dir> (make check_t_zsnap). Five triangle sets: perspective, w = 1, constant depth, depths below 0.001, depths above 0.999.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static const char *SRC =
    "using namespace metal;\n"
    "struct V { float4 p [[position, invariant]]; };\n"
    "vertex V vs(uint vid [[vertex_id]], const device float4 *v [[buffer(0)]], constant float &dz [[buffer(1)]]) { V o; o.p = v[vid]; o.p.z += dz * o.p.w; return o; }\n"
    "fragment void depth_only() {}\n"
    "fragment float4 reader(float4 p [[position]], depth2d<float> d [[texture(0)]]) {\n"
    "    float zw = fwidth(p.z);\n"
    "    float s = d.read(uint2(p.xy));\n"
    "    float z = dm_zsnap(p.z, s, zw);\n"
    "    return float4(1.0, p.z > s ? 1.0 : 0.0, p.z < s ? 1.0 : 0.0, z > s ? 1.0 : 0.0);\n"
    "}\n";

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: t_zsnap <dir with zsnap.h>\n"); return 2; }
    NSString *helper = [NSString stringWithContentsOfFile:[@(argv[1]) stringByAppendingPathComponent:@"zsnap.h"] encoding:NSUTF8StringEncoding error:nil];
    if (!helper) { fprintf(stderr, "no zsnap.h in %s\n", argv[1]); return 2; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    MTLCompileOptions *co = [MTLCompileOptions new]; co.preserveInvariance = YES;   // the chain's -fpreserve-invariance
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@\n%s", helper, SRC];
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:co error:&e];
    if (!lib) { printf("FAIL: compile: %s\n", e.localizedDescription.UTF8String); return 1; }
    const int W = 768, H = 432, N = 2000, cols = 50, rows = (N + cols - 1) / cols;
    id<MTLRenderPipelineState> (^pso)(NSString *, BOOL) = ^(NSString *fs, BOOL colour) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; NSError *err = nil;
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:fs];
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float; if (colour) d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        if (!p) { printf("FAIL: pipeline %s: %s\n", fs.UTF8String, err.localizedDescription.UTF8String); exit(1); }
        return p; };
    id<MTLRenderPipelineState> pd = pso(@"depth_only", NO), pr = pso(@"reader", YES);
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionLessEqual; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dsw = [dev newDepthStencilStateWithDescriptor:dd];
    dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO;
    id<MTLDepthStencilState> dsn = [dev newDepthStencilStateWithDescriptor:dd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:td], spare = [dev newTextureWithDescriptor:td];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> res = [dev newTextureWithDescriptor:td];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    float *v = malloc(sizeof(float) * 12 * N), *px = malloc((size_t)W * H * 16);
    const char *names[] = {"perspective", "w = 1", "constant depth", "depth < 0.001", "depth > 0.999"};
    long plain_total = 0;
    for (int mode = 0; mode < 5; mode++) {
        srand48(1 + mode);
        float cw = 2.0f / cols, ch = 2.0f / rows;
        for (int i = 0; i < N; i++) {
            float x0 = -1 + (i % cols) * cw, y0 = -1 + (i / cols) * ch, zc = 0;
            for (int k = 0; k < 3; k++) {
                float x = x0 + cw * (k == 1 ? 0.95f : 0.02f), y = y0 + ch * (k == 2 ? 0.95f : 0.02f);
                float w = mode == 1 ? 1.0f : 0.5f + 3 * (float)drand48();
                float z = mode == 3 ? 0.001f * (float)drand48() : mode == 4 ? 0.999f + 0.000999f * (float)drand48() : (float)drand48();
                if (mode == 2) { if (k == 0) zc = z; z = zc; }
                float *o = v + (i * 3 + k) * 4; o[0] = x * w; o[1] = y * w; o[2] = z * w; o[3] = w;
            }
        }
        id<MTLBuffer> vb = [dev newBufferWithBytes:v length:sizeof(float) * 12 * N options:MTLResourceStorageModeShared];
        for (int far = 0; far < 2; far++) {
            float dz = far ? 4e-6f : 0;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
            rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            float zero = 0; [en setRenderPipelineState:pd]; [en setDepthStencilState:dsw]; [en setVertexBuffer:vb offset:0 atIndex:0]; [en setVertexBytes:&zero length:4 atIndex:1];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 * N]; [en endEncoding];
            rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = res; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = spare; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
            en = [cb renderCommandEncoderWithDescriptor:rp];
            [en setRenderPipelineState:pr]; [en setDepthStencilState:dsn]; [en setVertexBuffer:vb offset:0 atIndex:0]; [en setVertexBytes:&dz length:4 atIndex:1];
            [en setFragmentTexture:depth atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 * N]; [en endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            [res getBytes:px bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            long cov = 0, above = 0, below = 0, snapped_above = 0;
            for (long i = 0; i < (long)W * H; i++) {
                const float *c = px + i * 4;
                if (c[0] == 0) continue;
                cov++; above += c[1] != 0; below += c[2] != 0; snapped_above += c[3] != 0;
            }
            CHECK(cov > 100000, "%s%s: only %ld pixels covered", names[mode], far ? ", farther" : "", cov);
            if (!far) {
                plain_total += above;
                printf("%s: %ld pixels, gl_FragCoord.z above the stored depth at %ld (%.2f%%), through dm_zsnap at %ld\n", names[mode], cov, above, 100.0 * above / cov, snapped_above);
                CHECK(below == 0, "%s: gl_FragCoord.z below the stored depth at %ld pixels; dm_zsnap's window looks only below z", names[mode], below);
                CHECK(snapped_above == 0, "%s: through dm_zsnap, z above its own surface's stored depth at %ld pixels", names[mode], snapped_above);
            } else {
                printf("%s, 4e-6 farther: z through dm_zsnap above the stored depth at %ld of %ld pixels\n", names[mode], snapped_above, cov);
                CHECK(snapped_above == cov, "%s: dm_zsnap took a surface 4e-6 farther as the stored one at %ld pixels", names[mode], cov - snapped_above);
            }
        }
    }
    printf("%s: gl_FragCoord.z above the stored depth of its own surface at %ld pixels in all (the rasterizer's own; 0 would mean this GPU stores the z its fragments read)\n",
           dev.name.UTF8String, plain_total);
    printf(fails ? "t_zsnap: %d failed\n" : "t_zsnap: ok\n", fails);
    return fails != 0;
} }
