// A fragment shader's colour outputs drawn one at a time (profile key split_outputs; nvn_mtl.m osplit_msl, osplit_named): the guarded copy of a shader's MSL declares the
// function constant and guards each line that writes a colour output, and is refused for a shader with one colour output, one that writes its depth, one without its output
// struct, and one that already takes the constant's index. A two-output draw made as two pieces, each specialized to one output with a write mask on that output's attachment,
// writes the same bytes as the draw made whole, and the guarded copy with the constant unset draws as the original. The keys DM_NVN_SPLIT_OUTPUTS gives match a shader
// by its 16 digits, whatever its library suffix, and a key that is not 16 digits is left out
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

// The translator's shape: an output struct <entry>_out, a local `out`, one line per write
static NSString *const k_src = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vmain(uint v [[vertex_id]]) { VOut o; float2 p = float2((v << 1) & 2, v & 2); o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
    "struct main0_out\n{\n    float4 out_attr0 [[color(0)]];\n    float4 out_attr1 [[color(1)]];\n};\n\n"
    "fragment main0_out main0(VOut in [[stage_in]])\n{\n    main0_out out = {};\n    float2 uv = in.pos.xy * (1.0 / 64.0);\n"
    "    out.out_attr0 = float4(uv.x * uv.y, uv.x + 0.25, uv.y * 0.5, 1.0);\n"
    "    out.out_attr1.xy = uv * 0.75;\n"
    "    out.out_attr1.zw = float2(uv.x - uv.y, 0.5);\n"
    "    return out;\n}\n";

static id<MTLLibrary> compile(NSString *msl) {
    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES; NSError *e = nil;
    id<MTLLibrary> l = [g_dev newLibraryWithSource:msl options:o error:&e];
    if (!l) fprintf(stderr, "compile: %s\n", e.localizedDescription.UTF8String);
    return l;
}
static id<MTLRenderPipelineState> pipeline(id<MTLLibrary> vl, id<MTLFunction> f, int only) {   // only: the one attachment written, -1 = both
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [vl newFunctionWithName:@"vmain"]; d.fragmentFunction = f;
    for (int c = 0; c < 2; c++) { d.colorAttachments[c].pixelFormat = MTLPixelFormatRGBA16Float; d.colorAttachments[c].writeMask = only < 0 || only == c ? MTLColorWriteMaskAll : MTLColorWriteMaskNone; }
    NSError *e = nil; id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String);
    return p;
}
// Draws the full-screen triangle once per pipeline into two fresh 64x64 attachments (cleared to a colour no draw writes) and reads both back
static NSData *draw_with(NSArray<id<MTLRenderPipelineState>> *ps) {
    id<MTLTexture> t[2]; MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int c = 0; c < 2; c++) { t[c] = [g_dev newTextureWithDescriptor:td]; rp.colorAttachments[c].texture = t[c]; rp.colorAttachments[c].loadAction = MTLLoadActionClear;
        rp.colorAttachments[c].storeAction = MTLStoreActionStore; rp.colorAttachments[c].clearColor = MTLClearColorMake(-3, -3, -3, -3); }
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    for (id<MTLRenderPipelineState> p in ps) { [en setRenderPipelineState:p]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }
    [en endEncoding];
    id<MTLBuffer> b = [g_dev newBufferWithLength:2 * 64 * 64 * 8 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    for (int c = 0; c < 2; c++) [bl copyFromTexture:t[c] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:b
                                  destinationOffset:(NSUInteger)c * 64 * 64 * 8 destinationBytesPerRow:64 * 8 destinationBytesPerImage:64 * 64 * 8];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return [NSData dataWithBytes:b.contents length:b.length];
}
static id<MTLFunction> only(id<MTLLibrary> l, int c) {
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; [cv setConstantValue:&c type:MTLDataTypeInt atIndex:OSPLIT_FC]; NSError *e = nil;
    id<MTLFunction> f = [l newFunctionWithName:@"main0" constantValues:cv error:&e];
    if (!f) fprintf(stderr, "specialize %d: %s\n", c, e.localizedDescription.UTF8String);
    return f;
}

int main(void) { @autoreleasepool {
    mtl_init();
    uint32_t outs = 0;
    NSString *g = osplit_msl(k_src, @"main0", &outs);
    CHECK(g && outs == 3, "a shader with two colour outputs gets a guarded copy covering both");
    NSString *decl = [NSString stringWithFormat:@"constant int dm_output_only_fc [[function_constant(%d)]];", OSPLIT_FC];
    CHECK([g containsString:decl], "the copy declares the function constant");
    CHECK([g containsString:@"    if (dm_output_only < 0 || dm_output_only == 0) out.out_attr0 = "], "a write to output 0 is guarded by 0, its indent kept");
    CHECK([g containsString:@"if (dm_output_only < 0 || dm_output_only == 1) out.out_attr1.xy = "] && [g containsString:@"if (dm_output_only < 0 || dm_output_only == 1) out.out_attr1.zw = "],
          "every line that writes part of output 1 is guarded by 1");
    CHECK([g containsString:@"vertex VOut vmain(uint v [[vertex_id]]) { VOut o;"] && [g containsString:@"    main0_out out = {};\n"], "lines that write no output are left as they were");
    uint32_t o2 = 0;
    CHECK(!osplit_msl([k_src stringByReplacingOccurrencesOfString:@"    float4 out_attr1 [[color(1)]];\n" withString:@""], @"main0", &o2), "one colour output: refused");
    CHECK(!osplit_msl([[k_src stringByReplacingOccurrencesOfString:@"    float4 out_attr1 [[color(1)]];\n" withString:@"    float4 out_attr1 [[color(1)]];\n    float depth [[depth(any)]];\n"]
                       stringByReplacingOccurrencesOfString:@"    return out;" withString:@"    out.depth = 0.5;\n    return out;"], @"main0", &o2), "a shader that writes its depth: refused");
    CHECK(!osplit_msl(k_src, @"main1", &o2), "no output struct for the entry point: refused");
    CHECK(!osplit_msl([@"constant bool other [[function_constant(90)]];\n" stringByAppendingString:k_src], @"main0", &o2), "the constant's index already taken: refused");

    id<MTLLibrary> lw = compile(k_src), lg = g ? compile(g) : nil;
    CHECK(lw && lg, "the original and its guarded copy compile");
    if (lw && lg) {
        MTLFunctionConstantValues *none = [MTLFunctionConstantValues new]; NSError *e = nil;
        id<MTLFunction> dflt = [lg newFunctionWithName:@"main0" constantValues:none error:&e], f0 = only(lg, 0), f1 = only(lg, 1);
        if (!dflt) fprintf(stderr, "unspecialized: %s\n", e.localizedDescription.UTF8String);
        id<MTLRenderPipelineState> whole = pipeline(lw, [lw newFunctionWithName:@"main0"], -1), gwhole = dflt ? pipeline(lw, dflt, -1) : nil,
            p0 = f0 ? pipeline(lw, f0, 0) : nil, p1 = f1 ? pipeline(lw, f1, 1) : nil;
        CHECK(whole && gwhole && p0 && p1, "the whole pipeline and the pieces build");
        if (whole && gwhole && p0 && p1) {
            NSData *w = draw_with(@[whole]), *s = draw_with(@[p0, p1]), *gd = draw_with(@[gwhole]), *a = draw_with(@[p0]);
            CHECK([s isEqualToData:w], "the two pieces write the same bytes as the whole draw");
            CHECK([gd isEqualToData:w], "the guarded copy with the constant unset draws as the original");
            const __fp16 *h = a.bytes; int kept = 1; for (int i = 64 * 64 * 4; i < 2 * 64 * 64 * 4; i++) kept &= h[i] == (__fp16)-3;
            CHECK(kept && memcmp(a.bytes, w.bytes, 64 * 64 * 8) == 0, "the piece of output 0 writes attachment 0 alone");
        }
    }

    setenv("DM_NVN_SPLIT_OUTPUTS", "4744589bb8f9f7dd,4744589bb8f9f7d,0123456789abcdef", 1);   // the second is one digit short
    CHECK(osplit_named("4744589bb8f9f7dd_b") && osplit_named("0123456789abcdef_b"), "a named shader matches by its 16 digits, whatever its library suffix");
    CHECK(!osplit_named("4744589bb8f9f7de_b") && !osplit_named("4744589bb8f9f7d0_b"), "another shader does not match, nor a key one digit short");
    CHECK(g_osplit_nk == 2, "the key that is not 16 digits is left out");
    printf("t_split_outputs: %d/%d checks passed\n", checks - failed, checks);
    return failed != 0;
} }
