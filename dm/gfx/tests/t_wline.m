// Wide lines (nvn_wline.inc, dmsh_pull.py <entry>_wl): a vertex shader in the translator's shape goes through dmsh_pull.variant, and its wide-line entry draws lines 4 pixels
// wide into a 64x64 target, 6 vertices a segment, with the line block the host fills (wl_block). Checked by the pixels covered: a line list (a horizontal and a vertical
// segment: 4 rows / 4 columns, ends on the major axis), a loop (its closing segment) against a strip (none), an indexed strip with 16-bit indices, a base vertex and a restart
// index (the segments touching it dropped), 8-bit indices, two instances, and a segment with one endpoint behind the eye (the visible part exactly 4 rows, to the edge, and
// its output, the vertex ID, interpolated along it as the clipper would). A 45-degree segment: a parallelogram 4 rows high in each of its 32 columns, and with the
// multisampled rule the rectangle 4 pixels across it, pixel for pixel
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static int checks, failed;
#define EXPECT(name, want, got) do { long w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %ld, got %ld\n", name, w, g); failed++; } } while (0)
static NSString *k_vs =
@"#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n"
"struct main0_out\n{\n    float4 out_c [[user(locn0)]];\n    float4 gl_Position [[position, invariant]];\n};\n\n"
"struct main0_in\n{\n    float4 m_0 [[attribute(0)]];\n};\n\n"
"vertex main0_out main0(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint gl_InstanceIndex [[instance_id]], uint gl_VertexIndex [[vertex_id]])\n{\n    main0_out out = {};\n"
"    if (in.m_0.z > 5.0)\n    {\n        return out;\n    }\n"   // an early return, never taken: the body runs as a function with all its returns
"    float4 p = in.m_0;\n    p.x += float(gl_InstanceIndex) * u.x * p.w;\n    out.gl_Position = p;\n    out.out_c = float4(float(gl_VertexIndex));\n    return out;\n}\n";
static NSString *k_fs = @"#include <metal_stdlib>\nusing namespace metal;\nstruct I { float4 c [[user(locn0)]]; };\nfragment float4 fs(I in [[stage_in]]) { return float4(in.c.x + 1.0); }\n";
typedef struct { uint64_t base; uint32_t stride, divisor; } dm_vstream_c;
typedef struct { dm_vstream_c s[16]; uint32_t attr[16]; } dm_vlayout_c;
typedef struct { uint64_t ib; float vp[2]; float width; uint32_t prim, count; int32_t base; uint32_t isz, restart, ri, zclip, rect, pad; } wl_block;   // nvn_wline.inc
enum { W = 64 };
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLRenderPipelineState> ps;
static float cov[W][W];
static uint32_t g_rect;   // the multisampled rule: a rectangle across the segment   // [row][column], row 0 at the top
// Draws verts (clip positions) as primitive prim (1 lines, 2 loop, 3 strip), count elements, indexed when isz (indices idx), base: first vertex / base vertex
static void draw(const float (*verts)[4], int nv, int prim, int count, const void *idx, uint32_t isz, int32_t base, int restart, uint32_t ri, int instances, float shift) {
    id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:(NSUInteger)nv * 16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = idx ? [dev newBufferWithBytes:idx length:(NSUInteger)count * isz options:MTLResourceStorageModeShared] : nil;
    dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = vb.gpuAddress; L.s[0].stride = 16; L.attr[0] = 0x2E;   // NVN_FORMAT_RGBA32F, offset 0, stream 0
    int nseg = prim == 1 ? count / 2 : prim == 3 ? count - 1 : count;
    wl_block b = {ib ? ib.gpuAddress : 0, {W / 2.0f, W / 2.0f}, 4.0f, (uint32_t)prim, (uint32_t)count, base, idx ? isz : 0, (uint32_t)restart, ri, 1, g_rect, 0};
    float u[4] = {shift, 0, 0, 0};
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:W height:W mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en setCullMode:MTLCullModeNone];
    [en setVertexBytes:u length:sizeof u atIndex:0]; [en setVertexBytes:&L length:sizeof L atIndex:16]; [en setVertexBytes:&b length:sizeof b atIndex:15];
    [en useResource:vb usage:MTLResourceUsageRead stages:MTLRenderStageVertex]; if (ib) [en useResource:ib usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)nseg * 6 instanceCount:(NSUInteger)instances];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    [t getBytes:cov bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0];
}
static long covered(int x0, int y0, int x1, int y1) { long n = 0; for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += cov[y][x] != 0; return n; }
int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); q = [dev newCommandQueue]; NSError *e = nil;
    char dir[] = "/tmp/t_wline.XXXXXX"; if (!mkdtemp(dir)) return 1;
    NSString *in = [@(dir) stringByAppendingPathComponent:@"v.metal"], *out = [@(dir) stringByAppendingPathComponent:@"vp.metal"];
    [k_vs writeToFile:in atomically:NO encoding:NSUTF8StringEncoding error:nil];
    char cmd[2048]; snprintf(cmd, sizeof cmd, "python3 -c \"import sys; sys.path.insert(0, '../tools'); import dmsh_pull; v, i = dmsh_pull.variant(open(sys.argv[1]).read()); assert i['entry_wl'] == 'main0_wl', i; open(sys.argv[2], 'w').write(v)\" '%s' '%s'", in.UTF8String, out.UTF8String);
    if (system(cmd)) { fprintf(stderr, "dmsh_pull.variant failed\n"); return 1; }
    NSString *pull = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:nil];
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES;
    id<MTLLibrary> lp = [dev newLibraryWithSource:pull options:o error:&e], lf = lp ? [dev newLibraryWithSource:k_fs options:o error:&e] : nil;
    if (!lp || !lf) { fprintf(stderr, "compile: %s\n", e.localizedDescription.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [lp newFunctionWithName:@"main0_wl"]; d.fragmentFunction = [lf newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
    ps = [dev newRenderPipelineStateWithDescriptor:d error:&e]; if (!ps) { fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    // pixel x = (ndc x + 1) * 32, row = (1 - ndc y) * 32
    const float list[4][4] = {{-0.5f, 0, 0, 1}, {0.5f, 0, 0, 1}, {-0.75f, -0.5f, 0, 1}, {-0.75f, 0.5f, 0, 1}};
    draw(list, 4, 1, 4, NULL, 0, 0, 0, 0, 1, 0);
    EXPECT("line list: horizontal segment, rows 30-33, columns 16-47", 128, covered(16, 30, 48, 34));
    EXPECT("line list: vertical segment, columns 6-9, rows 16-47", 128, covered(6, 16, 10, 48));
    EXPECT("line list: nothing else", 256, covered(0, 0, W, W));
    const float sq[4][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}, {-0.5f, 0.5f, 0, 1}};
    draw(sq, 4, 2, 4, NULL, 0, 0, 0, 0, 1, 0);
    EXPECT("line loop: its closing segment (left edge)", 4 * 24, covered(14, 20, 18, 44));
    EXPECT("line loop: the inside empty", 0, covered(20, 20, 44, 44));
    draw(sq, 4, 3, 4, NULL, 0, 0, 0, 0, 1, 0);
    EXPECT("line strip: no closing segment", 0, covered(14, 20, 18, 44));
    EXPECT("line strip: its right edge", 4 * 24, covered(46, 20, 50, 44));
    const float bars[5][4] = {{0, 0, 9, 1}, {-0.5f, 0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}, {-0.5f, -0.5f, 0, 1}, {0.5f, -0.5f, 0, 1}};   // vertex 0: never referenced (base vertex 1)
    const uint16_t ix16[5] = {0, 1, 0xffff, 2, 3};
    draw(bars, 5, 3, 5, ix16, 2, 1, 1, 0xffff, 1, 0);
    EXPECT("indexed strip with restart: top bar", 128, covered(16, 14, 48, 18));
    EXPECT("indexed strip with restart: bottom bar", 128, covered(16, 46, 48, 50));
    EXPECT("indexed strip with restart: no segment across the restart index", 256, covered(0, 0, W, W));
    const uint8_t ix8[2] = {1, 0};
    draw(list, 4, 1, 2, ix8, 1, 0, 0, 0, 1, 0);
    EXPECT("8-bit indices: the horizontal segment", 128, covered(16, 30, 48, 34));
    EXPECT("8-bit indices: nothing else", 128, covered(0, 0, W, W));
    const float shortseg[2][4] = {{-0.75f, 0, 0, 1}, {-0.25f, 0, 0, 1}};
    draw(shortseg, 2, 1, 2, NULL, 0, 0, 0, 0, 2, 1.0f);
    EXPECT("instance 0", 64, covered(8, 30, 24, 34));
    EXPECT("instance 1", 64, covered(40, 30, 56, 34));
    EXPECT("two instances: nothing else", 128, covered(0, 0, W, W));
    const float behind[2][4] = {{-0.5f, 0, 0.25f, 1}, {1.0f, 0, -0.25f, -1}};   // P(t) projects to y = 0, x from -0.5 to +inf as w goes to 0; z / w = 0.25
    draw(behind, 2, 1, 2, NULL, 0, 0, 0, 0, 1, 0);
    EXPECT("an endpoint behind the eye: rows 30-33 from column 16 to the edge", 4 * 48, covered(16, 30, W, 34));
    EXPECT("an endpoint behind the eye: nothing else", 4 * 48, covered(0, 0, W, W));
    // column 32's centre, x = 1 / 64, is P(t) at t = (0.5 + x) / (1.5 + 2x): its output interpolated there as the clipper would (perspective-correct), from the clipped endpoint's
    { double x = 1.0 / 64, t = (0.5 + x) / (1.5 + 2 * x);
      EXPECT("an endpoint behind the eye: the output along the segment, in thousandths", lround(t * 1000), lround((cov[31][32] - 1.0f) * 1000)); }
    const float diag[2][4] = {{-0.5f, -0.5f, 0, 1}, {0.5f, 0.5f, 0, 1}};
    draw(diag, 2, 1, 2, NULL, 0, 0, 0, 0, 1, 0);
    EXPECT("45 degrees: a parallelogram, 4 rows in each of 32 columns", 128, covered(0, 0, W, W));
    g_rect = 1;
    draw(diag, 2, 1, 2, NULL, 0, 0, 0, 0, 1, 0);
    { long want = 0, same = 0;   // the pixel centres inside the ideal rectangle: (16, 48) to (48, 16), 2 pixels either side
      for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) { double cx = x + 0.5 - 16, cy = y + 0.5 - 48, along = (cx - cy) / sqrt(2.0), across = (cx + cy) / sqrt(2.0);
          int tie = x - y + 32 == 0 || x - y + 32 == 64;   // a centre on an end edge: the rasterizer's tie rule decides it
          int in = along > 0 && along < 32 * sqrt(2.0) && fabs(across) < 2; want += in && !tie; same += tie || in == (cov[y][x] != 0); }
      EXPECT("45 degrees, multisampled: pixels that differ from the ideal rectangle (centres on its edges aside)", 0, W * W - same);
      long n = covered(0, 0, W, W); EXPECT("45 degrees, multisampled: as many pixels as the ideal rectangle, up to its 8 edge centres", 1, n >= want && n <= want + 8); }
    draw(list, 4, 1, 4, NULL, 0, 0, 0, 0, 1, 0);
    EXPECT("multisampled: an axis-aligned segment as before", 256, covered(0, 0, W, W));
    g_rect = 0;
    printf("t_wline: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
