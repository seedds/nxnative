// A fragment shader's depth against the depth the same surface stored (dmsh_build.py frag_z): each of 40 random triangles, plus one at depth 0, is drawn into depth, then
// drawn again by fragment shaders in a reduced translator shape that read that depth: a probe that records whether position.z is above or below the stored value, and the
// comparisons whose answer a same-surface pixel changes, `discard when z > stored` and `keep only when z <= stored` (a ground mist's shape), each also mirrored (stored < z,
// stored >= z), with z through the translator's flush of a denormal, as written and after frag_z. position.z must never be below the stored depth (leaving z >= x and z < x alone
// relies on it), and after frag_z all four keep every covered pixel. The control: with the pre-pass drawn slightly nearer, the surface is a different one, and after frag_z all
// four must still reject every pixel. The probe's count of pixels above is printed: on a GPU where it is 0, the as-written shaders drop nothing either.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static uint32_t next(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }   // xorshift32

static NSString *k_vs = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 gl_Position [[position, invariant]]; };\nstruct U { float4 v[3]; };\n"
    "vertex V vs(uint i [[vertex_id]], constant U& u [[buffer(0)]]) { V o; o.gl_Position = u.v[i]; return o; }\n"
    "fragment void depth_only() {}\n"
    "fragment float4 probe(float4 p [[position]], depth2d<float> d [[texture(0)]]) { float s = d.read(uint2(p.xy)); return float4(p.z > s ? 1.0 : 0.0, p.z < s ? 1.0 : 0.0, 0.0, 1.0); }\n";
// position.z into a temporary, the translator's flush of a denormal, then the comparison with the stored depth into a bool
static NSString *mist(NSString *cmp, NSString *drop) {
    return [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nstruct main0_out { float4 c [[color(0)]]; };\n"
        "fragment main0_out main0(float4 gl_FragCoord [[position]], depth2d<float> d [[texture(0)]])\n{\n    main0_out out = {};\n"
        "    float _102 = gl_FragCoord.z;\n    float _112 = abs(_102);\n    bool _116 = _112 < 1.1754943508222875079687365372222e-38;\n    float _118 = _116 ? 0.0 : _102;\n"
        "    float _124 = d.read(uint2(gl_FragCoord.xy));\n    bool _126 = %@;\n    if (%@_126) discard_fragment();\n"
        "    out.c = float4(1.0);\n    return out;\n}\n", cmp, drop];
}

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    enum { NS = 4 };   // `z > stored` drops, `z <= stored` keeps, and their mirrors
    NSString *src[2 * NS] = { mist(@"_118 > _124", @""), mist(@"_118 <= _124", @"!"), mist(@"_124 < _118", @""), mist(@"_124 >= _118", @"!") };   // [NS..] after frag_z
    char dir[] = "/tmp/t_frag_z.XXXXXX"; if (!mkdtemp(dir)) return 1;
    NSString *root = @(dir); if (!root) return 1;
    // frag_z alone, as test_dmsh_frag_z.py takes it: dmsh_build.py is not imported whole
    NSMutableArray<NSString *> *args = [NSMutableArray arrayWithObjects:@"python3", @"-c", @"import ast, re, sys; t = ast.parse(open('../tools/dmsh_build.py').read()); s = {'re': re}; "
        "exec(compile(ast.Module(body=[n for n in t.body if isinstance(n, ast.FunctionDef) and n.name == 'frag_z'], type_ignores=[]), 'dmsh_build.py', 'exec'), s); "
        "[open(o, 'w').write(s['frag_z'](open(i).read())) for i, o in zip(sys.argv[1::2], sys.argv[2::2])]", nil];
    for (int i = 0; i < NS; i++) {
        NSString *a = [root stringByAppendingPathComponent:[NSString stringWithFormat:@"%d.metal", i]], *b = [root stringByAppendingPathComponent:[NSString stringWithFormat:@"%dz.metal", i]];
        [src[i] writeToFile:a atomically:NO encoding:NSUTF8StringEncoding error:nil]; [args addObject:a]; [args addObject:b];
    }
    NSTask *py = [NSTask new]; py.executableURL = [NSURL fileURLWithPath:@"/usr/bin/env"]; py.arguments = args;
    if (![py launchAndReturnError:&e]) { fprintf(stderr, "frag_z: %s\n", e.localizedDescription.UTF8String); return 1; }
    [py waitUntilExit];
    if (py.terminationStatus) { fprintf(stderr, "frag_z failed\n"); return 1; }
    int rewritten = 0;
    for (int i = 0; i < NS; i++) {
        NSString *s = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:[NSString stringWithFormat:@"%dz.metal", i]] encoding:NSUTF8StringEncoding error:nil];
        if (!s) { fprintf(stderr, "frag_z wrote no %dz.metal\n", i); return 1; }
        src[NS + i] = s; rewritten += ![s isEqualToString:src[i]];
    }
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    EXPECT("frag_z rewrote all four comparisons", NS, rewritten);

    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES;   // the chain's -fpreserve-invariance
    id<MTLLibrary> lv = [dev newLibraryWithSource:k_vs options:o error:&e];
    if (!lv) { fprintf(stderr, "compile: %s\n", e.localizedDescription.UTF8String); return 1; }
    enum { W = 384, H = 216, NT = 41, NP = 1 + 2 * NS };   // pipelines: 0 probe, 1..NS as written, NS+1..2NS after frag_z
    MTLPixelFormat DF = MTLPixelFormatDepth32Float;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lv newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lv newFunctionWithName:@"depth_only"]; pd.depthAttachmentPixelFormat = DF;
    id<MTLRenderPipelineState> pdepth = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    id<MTLRenderPipelineState> pp[NP];
    pd.depthAttachmentPixelFormat = MTLPixelFormatInvalid; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    for (int k = 0; k < NP; k++) {
        id<MTLLibrary> l = k ? [dev newLibraryWithSource:src[k - 1] options:o error:&e] : lv;
        if (!l) { fprintf(stderr, "compile %d: %s\n", k, e.localizedDescription.UTF8String); return 1; }
        pd.fragmentFunction = [l newFunctionWithName:k ? @"main0" : @"probe"]; pp[k] = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!pp[k]) { fprintf(stderr, "pipeline %d: %s\n", k, e.localizedDescription.UTF8String); return 1; }
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionLessEqual; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dw = [dev newDepthStencilStateWithDescriptor:dd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:DF width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:td];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> col = [dev newTextureWithDescriptor:td];
    // kept[near][k]: pixels the pipeline kept, with the pre-pass the same triangle (near 0) or the same triangle 1e-4 nearer (near 1, the control)
    uint64_t covered = 0, above = 0, below = 0, kept[2][NP] = {{0}}, zero_covered = 0, zero_kept[NP] = {0}, near_covered = 0;
    uint8_t *px = malloc(W * H * 4); if (!px) return 1;
    uint32_t seed = 7;   // the same triangles every run
    for (int t = 0; t < NT; t++) {
        simd_float4 v[3], vn[3];
        for (int i = 0; i < 3; i++) {
            float w = 0.5f + (next(&seed) % 1000) / 200.f, x = -1.5f + (next(&seed) % 3000) / 1000.f, y = -1.5f + (next(&seed) % 3000) / 1000.f, zn = 0.94f + (next(&seed) % 590) / 10000.f;
            if (t == NT - 1) zn = 0;   // the last triangle lies at depth 0
            v[i] = simd_make_float4(x * w, y * w, zn * w, w); vn[i] = simd_make_float4(x * w, y * w, (zn - 1e-4f) * w, w);
        }
        for (int near = 0; near < 2; near++) {
            if (near && t == NT - 1) continue;   // nothing is nearer than 0
            for (int k = near ? 1 : 0; k < NP; k++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *r = [MTLRenderPassDescriptor renderPassDescriptor];
                r.depthAttachment.texture = depth; r.depthAttachment.loadAction = MTLLoadActionClear; r.depthAttachment.clearDepth = 1; r.depthAttachment.storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:r];
                [en setRenderPipelineState:pdepth]; [en setDepthStencilState:dw]; [en setVertexBytes:near ? vn : v length:sizeof v atIndex:0];
                [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
                r = [MTLRenderPassDescriptor renderPassDescriptor];
                r.colorAttachments[0].texture = col; r.colorAttachments[0].loadAction = MTLLoadActionClear; r.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); r.colorAttachments[0].storeAction = MTLStoreActionStore;
                en = [cb renderCommandEncoderWithDescriptor:r];
                [en setRenderPipelineState:pp[k]]; [en setVertexBytes:v length:sizeof v atIndex:0]; [en setFragmentTexture:depth atIndex:0];
                [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "command buffer failed\n"); return 1; }
                [col getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                for (int i = 0; i < W * H; i++) {
                    uint8_t *c = px + i * 4; if (!c[3]) continue;
                    kept[near][k]++; if (!near && t == NT - 1) zero_kept[k]++;
                    if (k == 0) { covered++; if (t == NT - 1) zero_covered++; else near_covered++; above += c[0] > 127; below += c[1] > 127; }
                }
            }
        }
    }
    free(px);
    printf("%s: %llu covered pixels, position.z above the stored depth on %llu (%.2f %%), below on %llu; kept as written / after frag_z: `z > stored` %llu / %llu, "
           "`z <= stored` %llu / %llu, `stored < z` %llu / %llu, `stored >= z` %llu / %llu; pre-pass nearer, after frag_z: %llu %llu %llu %llu of %llu\n", dev.name.UTF8String, covered, above,
           100.0 * above / covered, below, kept[0][1], kept[0][1 + NS], kept[0][2], kept[0][2 + NS], kept[0][3], kept[0][3 + NS], kept[0][4], kept[0][4 + NS],
           kept[1][1 + NS], kept[1][2 + NS], kept[1][3 + NS], kept[1][4 + NS], near_covered);
    EXPECT("the test covers pixels, the depth-0 triangle too", 1, covered > 0 && zero_covered > 0);
    EXPECT("position.z is never below the depth the same surface stored", 0, below);
    for (int s = 1; s <= NS; s++) {
        EXPECT("as written, the comparison drops exactly the pixels whose position.z is above", covered - above, kept[0][s]);
        EXPECT("after frag_z, the same surface keeps every covered pixel", covered, kept[0][s + NS]);
        EXPECT("after frag_z, the triangle at depth 0 keeps every pixel", zero_covered, zero_kept[s + NS]);
        EXPECT("control: after frag_z, a surface behind the stored depth is rejected everywhere", 0, kept[1][s + NS]);
    }
    printf("== %d checks, %d failed ==\n", checks, failed); return failed != 0;
} }
