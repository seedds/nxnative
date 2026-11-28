// A vertex shader's libraries cut from one compile (dmsh_side.vs_unit, as dmsh_build.py's vs_libraries makes them) against the three compiles they replace, on the GPU.
// A vertex shader in the translator's shape, with an integer input and an early return, becomes the build's source (dmsh_entry.vs_depth, dmsh_geometry.capture), its pulling and
// shadow-input variants (dmsh_pull.py, dmsh_vin.py) and the unit; each is compiled offline with the chain's Mac flags (dmsh_chain.MAC_METAL), and the unit's library is cut
// (dmsh_side.cut). 2,000 random triangles (half positions, snorm normals, 8-bit integers) then go through every entry point:
//   · the unit's own entry point, its pulling and its shadow-input version, in both depth conventions: one draws depth, another draws with depth test EQUAL, every ordered pair,
//     and every covered pixel must pass (the hybrid swaps a pair's pipelines between passes, t_pull_inv.m; the body is inlined in all of them);
//   · each of the six against the entry point it replaces: the same depth, both ways round, and the same colour image bit for bit;
//   · the wide-line entries main0_wl / main0_z_wl, which run the body's out-of-line copy: the same depth and colour as the pulling library's own;
//   · the capture entry main0_gs, which calls the out-of-line copy and writes its capture epilogue once: every word of every record written, and each the plain source's to
//     within 16 units in the last place (a capture entry is not invariant: in a game's shader a few words of a few vertices may differ in their low bits,
//     docs/reference/shaders.md, One compile for a vertex shader's libraries);
//   · main0_gs again with the plain source and the unit compiled in Metal's safe math mode without contraction (-fmetal-math-mode=safe -ffp-contract=off): every captured
//     word the plain source's. The two hold the same arithmetic, so only the compiler's freedom to fuse a multiply and an add may tell them apart.
// Controls: a unit whose depth line multiplies by 0.50000006 must fail the comparison, so a pass means equal positions; a strict unit whose main0_gs scales x by 1 + 2^-23 after
// the call must capture other words
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)
static NSString *k_vs =
@"#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n"
"struct main0_out\n{\n    float4 out_c [[user(locn0)]];\n    float4 gl_Position [[position, invariant]];\n};\n\n"
"struct main0_in\n{\n    float4 m_0 [[attribute(0)]];\n    float4 m_1 [[attribute(1)]];\n    uint4 m_2 [[attribute(2)]];\n};\n\n"
"vertex main0_out main0(main0_in in [[stage_in]], constant float4x4& u [[buffer(0)]])\n{\n    main0_out out = {};\n"
"    float4 p = u * float4(in.m_0.xyz, 1.0);\n    p.xyz = fma(in.m_1.xyz, float3(0.0137), p.xyz);\n"
"    out.gl_Position = p;\n    out.out_c = float4(in.m_1.xyz * 0.5 + 0.5, 1.0);\n"
"    if ((in.m_2.x & 3u) == 3u)\n    {\n        return out;\n    }\n"
"    out.out_c.xyz = out.out_c.xyz * (float3(in.m_2.yzw & uint3(15u)) * 0.0625 + 0.03125) + p.zzz * 0.125;\n    return out;\n}\n";
static NSString *k_fs = @"#include <metal_stdlib>\nusing namespace metal;\nstruct I { float4 c [[user(locn0)]]; };\nfragment float4 fs(I in [[stage_in]]) { return in.c; }\n";
// The libraries, as the build makes them: a_* the three compiles of before, u_* the unit's pieces, c_* a unit with another depth line (the control)
static NSString *k_py =
@"import sys, os, subprocess\nsys.path.insert(0, '../tools')\nimport dmsh_chain, dmsh_entry, dmsh_geometry, dmsh_pull, dmsh_vin, dmsh_side\n"
"d = sys.argv[1]; src = open(os.path.join(d, 'v.metal')).read()\n"
"full = dmsh_entry.vs_depth(src)[1] + dmsh_geometry.capture(src)\n"
"pv, pinfo = dmsh_pull.variant(full); vv, shadows = dmsh_vin.variant(full)\n"
"unit, pn, vn = dmsh_side.vs_unit(full, pv, vv)\n"
"assert pn == {'entry': 'main0_p', 'entry_z': 'main0_zp', 'entry_wl': 'main0_wl', 'entry_z_wl': 'main0_z_wl'} and vn == {'entry': 'main0_v', 'entry_z': 'main0_zv'}, (pn, vn)\n"
"assert shadows == [{'location': 2, 'float': 16, 'sint': 17}], shadows\n"
"strict = [f for f in dmsh_chain.MAC_METAL if not f.startswith('-fmetal-math-mode=')] + ['-fmetal-math-mode=safe', '-ffp-contract=off']\n"
"def lib(name, text, flags=dmsh_chain.MAC_METAL):\n"
"    p = os.path.join(d, name); open(p + '.metal', 'w').write(text)\n"
"    for cmd in ([*flags, p + '.metal', '-o', p + '.air'], ['xcrun', '-sdk', 'macosx', 'metallib', p + '.air', '-o', p + '.metallib']):\n"
"        r = subprocess.run(cmd, capture_output=True, text=True)\n"
"        if r.returncode: sys.exit(name + ': ' + r.stderr)\n"
"    return open(p + '.metallib', 'rb').read()\n"
"for name, text in (('a_b', full), ('a_bp', pv), ('a_bv', vv)): lib(name, text)\n"
"other = unit.replace('fma(out.gl_Position.z, 0.5, ', 'fma(out.gl_Position.z, 0.50000006, '); assert other != unit\n"
"call = '    main0_out out = dm_vs_body_o('; i = unit.index(call); j = unit.index('\\n', i) + 1\n"
"moved = unit[:j] + '    out.gl_Position.x = out.gl_Position.x * 1.00000012f;\\n' + unit[j:]\n"
"lib('s_a_b', full, strict)\n"
"for stem, text, flags in (('u', unit, dmsh_chain.MAC_METAL), ('c', other, dmsh_chain.MAC_METAL), ('s_u', unit, strict), ('s_x', moved, strict)):\n"
"    for name, data in zip(('_b', '_bp', '_bv'), dmsh_side.cut(lib(stem, text, flags), set(pn.values()), set(vn.values()))): open(os.path.join(d, stem + name + '.metallib'), 'wb').write(data)\n";
typedef struct { uint64_t base; uint32_t stride, divisor; } dm_vstream_c;
typedef struct { dm_vstream_c s[16]; uint32_t attr[32]; } dm_vlayout_c;   // dmsh_pull.metal dm_vlayout
typedef struct { uint64_t ib; float vp[2]; float width; uint32_t prim, count; int32_t base; uint32_t isz, restart, ri, zclip, rect, pad; } wl_block;   // dmsh_pull.py dm_wline
enum { CAPW = 144 };
typedef struct { uint64_t dst, pad; uint32_t info[4]; uint32_t stride; int16_t slot[CAPW]; uint8_t end[12]; } capture_block;   // dmsh_geometry.py dm_geometry_capture
_Static_assert(sizeof(capture_block) == 336 && sizeof(wl_block) == 56, "the blocks as the shader declares them");
enum { NT = 2000, NV = NT * 3, W = 256, NSEG = 600, PLAIN = 0, PULL = 1, SHADOW = 2, WIDE = 3 };
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLFunction> g_fs;
static id<MTLBuffer> g_vb, g_lay, g_zero; static id<MTLDepthStencilState> g_less, g_equal; static float g_m[16];
typedef struct { __unsafe_unretained id<MTLRenderPipelineState> ps; int kind; } pipe_t;
static NSMutableArray *g_keep;
static MTLVertexDescriptor *layout(int shadow) {   // the attributes at the buffer the pulling layout block takes: half positions, snorm normals, 8-bit integers
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatHalf4; vd.attributes[1].format = MTLVertexFormatChar4Normalized; vd.attributes[1].offset = 8; vd.attributes[2].format = MTLVertexFormatUChar4; vd.attributes[2].offset = 12;
    for (int a = 0; a < 3; a++) vd.attributes[a].bufferIndex = 16;
    vd.layouts[16].stride = 16;
    if (shadow) {   // the integer input's shadows take the zero buffer: its format is of the input's own class (nvn_mtl.m vin_ct)
        vd.attributes[16].format = MTLVertexFormatFloat4; vd.attributes[17].format = MTLVertexFormatInt4; vd.attributes[16].bufferIndex = vd.attributes[17].bufferIndex = 29;
        vd.layouts[29].stride = 16; vd.layouts[29].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[29].stepRate = 0; }
    return vd;
}
static id<MTLLibrary> load(NSString *dir, NSString *name) {
    NSError *e = nil; id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingFormat:@"/%@.metallib", name]] error:&e];
    if (!l) { fprintf(stderr, "%s: %s\n", name.UTF8String, e.localizedDescription.UTF8String); exit(1); }
    return l;
}
static pipe_t pipe_of(id<MTLLibrary> lib, NSString *entry, int kind) {
    NSError *e = nil; MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:entry]; d.fragmentFunction = g_fs;
    if (!d.vertexFunction) { fprintf(stderr, "no entry point %s\n", entry.UTF8String); exit(1); }
    if (kind == PLAIN || kind == SHADOW) d.vertexDescriptor = layout(kind == SHADOW);
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { fprintf(stderr, "pipeline of %s: %s\n", entry.UTF8String, e.localizedDescription.UTF8String); exit(1); }
    [g_keep addObject:ps]; return (pipe_t){ ps, kind };
}
// first draws depth and colour, second draws with depth test EQUAL into a colour image of its own → the pixels first covered, those of them second passed, and whether the two
// colour images are the same bits (same: may be NULL)
static void pair(pipe_t first, pipe_t second, int *covered, int *passed, int *same) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:W mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:W mipmapped:NO]; dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> c[2] = { [dev newTextureWithDescriptor:td], [dev newTextureWithDescriptor:td] }, depth = [dev newTextureWithDescriptor:dd];
    wl_block wl = { 0, { W / 2.0f, W / 2.0f }, 3.0f, 1, NSEG * 2, 0, 0, 0, 0, 1, 0, 0 };   // a list of NSEG lines, 3 pixels wide
    id<MTLCommandBuffer> cb = [q commandBuffer];
    for (int pass = 0; pass < 2; pass++) {
        pipe_t p = pass ? second : first;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture = c[pass]; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = pass ? MTLLoadActionLoad : MTLLoadActionClear; rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:p.ps]; [en setDepthStencilState:pass ? g_equal : g_less]; [en setVertexBytes:g_m length:64 atIndex:0];
        if (p.kind == PULL || p.kind == WIDE) { [en setVertexBuffer:g_lay offset:0 atIndex:16]; [en useResource:g_vb usage:MTLResourceUsageRead stages:MTLRenderStageVertex]; } else [en setVertexBuffer:g_vb offset:0 atIndex:16];
        if (p.kind == SHADOW) [en setVertexBuffer:g_zero offset:0 atIndex:29];
        if (p.kind == WIDE) [en setVertexBytes:&wl length:sizeof wl atIndex:15];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:p.kind == WIDE ? NSEG * 6 : NV]; [en endEncoding];
    }
    [cb commit]; [cb waitUntilCompleted];
    EXPECT("no GPU error", 0, cb.error != nil);
    float *a = malloc(W * W * 16), *b = malloc(W * W * 16);
    [c[0] getBytes:a bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0]; [c[1] getBytes:b bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0];
    *covered = *passed = 0; for (int i = 0; i < W * W; i++) { *covered += a[4 * i + 3] != 0; *passed += a[4 * i + 3] != 0 && b[4 * i + 3] != 0; }
    if (same) *same = !memcmp(a, b, W * W * 16);  // NOLINT(bugprone-suspicious-memory-comparison): compares the rendered float bits, including signed zero.
    free(a); free(b);
}
static id<MTLBuffer> capture(id<MTLLibrary> lib) {   // main0_gs's records of every vertex, CAPW words each, every slot kept
    NSError *e = nil; MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"main0_gs"]; d.vertexDescriptor = layout(0); d.rasterizationEnabled = NO; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> ps = d.vertexFunction ? [dev newRenderPipelineStateWithDescriptor:d error:&e] : nil;
    if (!ps) { fprintf(stderr, "capture pipeline: %s\n", e ? e.localizedDescription.UTF8String : "no main0_gs"); exit(1); }
    id<MTLBuffer> cap = [dev newBufferWithLength:NV * CAPW * 4 options:MTLResourceStorageModeShared]; memset(cap.contents, 0xa5, cap.length);
    capture_block b = { cap.gpuAddress, 0, { 0, NV, 0, 1 }, CAPW, { 0 }, { 0 } }; for (int k = 0; k < CAPW; k++) b.slot[k] = (int16_t)k;
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:W mipmapped:NO]; dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.depthAttachment.texture = [dev newTextureWithDescriptor:dd]; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en setVertexBytes:g_m length:64 atIndex:0]; [en setVertexBuffer:g_vb offset:0 atIndex:16]; [en setVertexBytes:&b length:sizeof b atIndex:13];
    [en useResource:cap usage:MTLResourceUsageWrite stages:MTLRenderStageVertex];
    [en drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:NV]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("capture: no GPU error", 0, cb.error != nil);
    return cap;
}
int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); q = [dev newCommandQueue]; g_keep = [NSMutableArray array]; NSError *e = nil;
    char tmp[] = "/tmp/t_vs_unit.XXXXXX"; if (!mkdtemp(tmp)) return 1;
    NSString *dir = @(tmp);
    if (![k_vs writeToFile:[dir stringByAppendingPathComponent:@"v.metal"] atomically:NO encoding:NSUTF8StringEncoding error:nil] ||
        ![k_py writeToFile:[dir stringByAppendingPathComponent:@"libraries.py"] atomically:NO encoding:NSUTF8StringEncoding error:nil]) return 1;
    NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/env"]; task.arguments = @[@"python3", [dir stringByAppendingPathComponent:@"libraries.py"], dir];
    if (![task launchAndReturnError:&e]) { fprintf(stderr, "the libraries' script: %s\n", e.localizedDescription.UTF8String); return 1; }
    [task waitUntilExit];
    if (task.terminationStatus) { fprintf(stderr, "the libraries' script failed\n"); return 1; }
    id<MTLLibrary> ab = load(dir, @"a_b"), abp = load(dir, @"a_bp"), abv = load(dir, @"a_bv"), ub = load(dir, @"u_b"), ubp = load(dir, @"u_bp"), ubv = load(dir, @"u_bv"), cbl = load(dir, @"c_b"),
                 sab = load(dir, @"s_a_b"), sub = load(dir, @"s_u_b"), sxb = load(dir, @"s_x_b");
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    id<MTLLibrary> lf = [dev newLibraryWithSource:k_fs options:nil error:&e];
    if (!lf) { fprintf(stderr, "fragment: %s\n", e.localizedDescription.UTF8String); return 1; }
    g_fs = [lf newFunctionWithName:@"fs"];
    // vertex data: attribute 0 RGBA16F positions at offset 0, attribute 1 RGBA8 snorm at offset 8, attribute 2 RGBA8UI at offset 12, stride 16
    g_vb = [dev newBufferWithLength:NV * 16 options:MTLResourceStorageModeShared]; uint8_t *p = g_vb.contents;
    srand(9);  // NOLINT(bugprone-random-generator-seed): makes the GPU comparison repeatable.
    for (int v = 0; v < NV; v++) { _Float16 h[4]; for (int c = 0; c < 3; c++) h[c] = (_Float16)((float)(rand() % 20000) / 10000 - 1); h[3] = 1; memcpy(p + v * 16, h, 8);
        for (int c = 8; c < 16; c++) p[v * 16 + c] = (uint8_t)rand(); }
    const float M[16] = { 0.93f, 0.11f, -0.05f, 0, -0.13f, 0.91f, 0.1f, 0, 0.05f, -0.19f, 0.3f, 0, 0.013f, -0.021f, 0.5f, 1 }; memcpy(g_m, M, sizeof M);   // z stays inside both depth conventions
    dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = g_vb.gpuAddress; L.s[0].stride = 16; L.attr[0] = 0x29; L.attr[1] = 0x26 | 8u << 8; L.attr[2] = 0x27 | 12u << 8;   // NVN_FORMAT_RGBA16F, RGBA8SN, RGBA8UI
    g_lay = [dev newBufferWithBytes:&L length:sizeof L options:MTLResourceStorageModeShared];
    g_zero = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    MTLDepthStencilDescriptor *dl = [MTLDepthStencilDescriptor new]; dl.depthCompareFunction = MTLCompareFunctionLess; dl.depthWriteEnabled = YES;
    MTLDepthStencilDescriptor *de = [MTLDepthStencilDescriptor new]; de.depthCompareFunction = MTLCompareFunctionEqual; de.depthWriteEnabled = NO;
    g_less = [dev newDepthStencilStateWithDescriptor:dl]; g_equal = [dev newDepthStencilStateWithDescriptor:de];
    static const char *kinds[] = { "its own entry point", "the pulling one", "the shadow-input one" };
    char name[160]; int covered, passed, same;
    for (int z = 0; z < 2; z++) {
        NSString *own = z ? @"main0_z" : @"main0";
        pipe_t apart[3] = { pipe_of(ab, own, PLAIN), pipe_of(abp, own, PULL), pipe_of(abv, own, SHADOW) };
        pipe_t unit[3] = { pipe_of(ub, own, PLAIN), pipe_of(ubp, z ? @"main0_zp" : @"main0_p", PULL), pipe_of(ubv, z ? @"main0_zv" : @"main0_v", SHADOW) };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {   // the unit's three among themselves
            if (i == j) continue;
            pair(unit[i], unit[j], &covered, &passed, NULL);
            snprintf(name, sizeof name, "%s: the unit's %s after %s, depth equal: covered pixels that pass (of %d)", own.UTF8String, kinds[j], kinds[i], covered);
            EXPECT(name, covered, passed);
            checks++; if (covered < W * W / 2) { fprintf(stderr, "%s: only %d of %d pixels covered: the test draws too little\n", own.UTF8String, covered, W * W); failed++; }
        }
        for (int i = 0; i < 3; i++) for (int way = 0; way < 2; way++) {   // each against the entry point it replaces
            pair(way ? unit[i] : apart[i], way ? apart[i] : unit[i], &covered, &passed, &same);
            snprintf(name, sizeof name, "%s: %s, %s: covered pixels that pass (of %d)", own.UTF8String, kinds[i], way ? "the unit's first" : "its own compile's first", covered);
            EXPECT(name, covered, passed);
            snprintf(name, sizeof name, "%s: %s, %s: the same colour image", own.UTF8String, kinds[i], way ? "the unit's first" : "its own compile's first");
            EXPECT(name, 1, same);
        }
        NSString *wide = z ? @"main0_z_wl" : @"main0_wl";   // the wide-line entries run the out-of-line copy of the body
        pair(pipe_of(abp, wide, WIDE), pipe_of(ubp, wide, WIDE), &covered, &passed, &same);
        snprintf(name, sizeof name, "%s: the unit's after the pulling library's own: covered pixels that pass (of %d)", wide.UTF8String, covered);
        EXPECT(name, covered, passed);
        snprintf(name, sizeof name, "%s: the same colour image", wide.UTF8String); EXPECT(name, 1, same);
        checks++; if (covered < W * W / 20) { fprintf(stderr, "%s: only %d pixels covered: the test draws too little\n", wide.UTF8String, covered); failed++; }
    }
    pair(pipe_of(ab, @"main0", PLAIN), pipe_of(cbl, @"main0", PLAIN), &covered, &passed, &same);   // the control: another depth
    checks++; if (covered - passed < covered / 20 || same) { fprintf(stderr, "control: %d of %d pixels pass with another depth line (same image: %d): the comparison does not tell depths apart\n", passed, covered, same); failed++; }   // a unit in the last place of 0.5 moves the depth of about four pixels in ten
    id<MTLBuffer> ca = capture(ab), cu = capture(ub);
    const uint32_t *wa = ca.contents, *wu = cu.contents; size_t written = 0, differ = 0, far = 0;
    for (size_t i = 0; i < ca.length / 4; i++) {
        written += wu[i] != 0xa5a5a5a5u; differ += wa[i] != wu[i];
        uint32_t d = (wa[i] ^ wu[i]) >> 31 ? UINT32_MAX : wa[i] > wu[i] ? wa[i] - wu[i] : wu[i] - wa[i];   // the distance between two floats of one sign, in units in the last place
        far += d > 16; }
    EXPECT("main0_gs: every word of every record written", ca.length / 4, written);
    EXPECT("main0_gs: captured words more than 16 units in the last place from the plain source's", 0, far);
    printf("main0_gs: %zu of %zu captured words differ from the plain source's\n", differ, (size_t)(ca.length / 4));
    id<MTLBuffer> sa = capture(sab), su = capture(sub), sx = capture(sxb);   // compiled strict: the same words, and the control's other ones
    const uint32_t *ws = sa.contents, *wt = su.contents, *wx = sx.contents; size_t sdiffer = 0, xdiffer = 0;
    for (size_t i = 0; i < sa.length / 4; i++) { sdiffer += ws[i] != wt[i]; xdiffer += ws[i] != wx[i]; }
    EXPECT("main0_gs compiled strict: captured words that differ from the plain source's", 0, sdiffer);
    checks++; if (xdiffer < NV / 2) { fprintf(stderr, "control: x one unit in the last place larger changed %zu captured words, fewer than half the %d vertices: the comparison does not tell positions apart\n", xdiffer, NV); failed++; }
    printf("main0_gs compiled strict: %zu of %zu captured words differ from the plain source's; the control changes %zu\n", sdiffer, (size_t)(sa.length / 4), xdiffer);
    printf("t_vs_unit: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
