// Adjacent lazy depth/stencil clears must touch exactly their requested layers.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { W = 8, LAYERS = LC_MAX + 4, LEVELS = 2, OLD_D = 0x3f400000, QUARTER = 0x3e800000, HALF = 0x3f000000 };
typedef struct { op_hdr rt_h; p_set_rt rt; op_hdr clear_h; p_clear_ds clear; } clear_record;
static void append_op(uint8_t **p, uint32_t op, const void *payload, size_t n) {
    op_hdr h = {op, (uint32_t)(sizeof h + n)}; memcpy(*p, &h, sizeof h); *p += sizeof h;
    if (n) { memcpy(*p, payload, n); *p += n; }
}

int main(int argc, char **argv) { @autoreleasepool {
    int ab = 0, enabled = 1, default_off = 0, tbdr = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ab")) ab = 1;
        else if (!strcmp(argv[i], "--disabled")) enabled = 0;
        else if (!strcmp(argv[i], "--default")) { default_off = 1; enabled = 0; }
        else if (!strcmp(argv[i], "--tbdr")) { tbdr = 1; enabled = 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }
    setenv("DM_NVN_TBDR", tbdr ? "1" : "0", 1);
    if (default_off) unsetenv("DM_NVN_LC_BATCH");
    else setenv("DM_NVN_LC_BATCH", ab ? "ab" : enabled || tbdr ? "1" : "0", 1);
    if (ab) setenv("DM_AB", "8", 1);
    // Count all actual encoders, including standalone clears that g_stat_pass omits.
    setenv("DM_NVN_GPUPROF", "0", 1);
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void planes(texture2d_array<uint,access::read> s [[texture(0)]], depth2d_array<float,access::read> d [[texture(1)]], device uint2 *o [[buffer(0)]], constant uint &lv [[buffer(1)]], uint3 p [[thread_position_in_grid]]) { uint w=d.get_width(lv); o[(p.z*w+p.y)*w+p.x]=uint2(as_type<uint>(d.read(p.xy,p.z,lv)),s.read(p.xy,p.z,lv).r); }"
        "kernel void planes_ms(texture2d_ms_array<uint,access::read> s [[texture(0)]], depth2d_ms_array<float,access::read> d [[texture(1)]], device uint2 *o [[buffer(0)]], uint3 p [[thread_position_in_grid]]) { uint w=d.get_width(), ns=d.get_num_samples(); for (uint i=0;i<ns;i++) o[((p.z*w+p.y)*w+p.x)*ns+i]=uint2(as_type<uint>(d.read(p.xy,p.z,i)),s.read(p.xy,p.z,i).r); }"
        options:nil error:&error];
    id<MTLFunction> fn = [lib newFunctionWithName:@"planes"];
    id<MTLComputePipelineState> reader = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
    CHECK(reader != nil, "literal depth/stencil reader compiles");
    if (!reader) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    fn = [lib newFunctionWithName:@"planes_ms"];
    id<MTLComputePipelineState> reader_ms = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
    CHECK(reader_ms != nil, "multisample depth/stencil reader compiles");
    if (!reader_ms) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    const struct { const char *name; int layers[4], levels[4]; uint32_t depth[4]; int depth_mask, stencil_mask, passes, unknown_view, observer, boundary, mixed_masks, target_view, fallback, format_view, swizzle_view, cube, msaa; } cases[] = {
        {"contiguous depth clears batch with untouched outer layers and stencil", {1,2,3,4}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,1},
        {"noncontiguous layers remain separate", {0,2,4,6}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,4},
        {"different clear values remain separate", {1,2,3,4}, {0,0,0,0}, {QUARTER,HALF,QUARTER,HALF}, 1,0,4},
        {"different mip levels remain separate", {1,2,3,4}, {0,1,0,1}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,4},
        {"matching depth and stencil clears batch", {1,2,3,4}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,255,1},
        {"stencil-only clears batch without changing depth", {1,2,3,4}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 0,255,1},
        {"two compatible pairs form two exact batches", {0,1,2,3}, {0,0,0,0}, {QUARTER,QUARTER,HALF,HALF}, 1,0,2},
        {"three-layer prefix batches without consuming different final value", {0,1,2,3}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,HALF}, 1,0,2},
        {"unknown view mode retains separate clears", {1,2,3,4}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,4,1},
        {"copy observes first clear before same layers are cleared again", {1,2,1,2}, {0,0,0,0}, {QUARTER,QUARTER,HALF,HALF}, 1,0,2,0,1},
        {"LC_MAX flush and final two-layer run keep exact contents", {0}, {0}, {QUARTER}, 1,0,2,0,0,1},
        {"plane-mask mismatch splits two otherwise compatible pairs", {1,2,3,4}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,2,0,0,0,1},
        {"explicit native array target batches with level and layer views", {0,1,2,3}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,1,0,0,0,0,1},
        {"mixed nonidentity target overrides retain separate clears", {0,1,2,3}, {0,0,0,0}, {QUARTER,QUARTER,QUARTER,QUARTER}, 1,0,4,0,0,0,0,2},
        {.name = "explicit identical format views retain separate clears", .layers = {1,2,3,4},
            .depth = {QUARTER,QUARTER,QUARTER,QUARTER}, .depth_mask = 1, .passes = 4, .format_view = 1},
        {.name = "explicit identity swizzles retain separate clears", .layers = {1,2,3,4},
            .depth = {QUARTER,QUARTER,QUARTER,QUARTER}, .depth_mask = 1, .passes = 4, .swizzle_view = 1},
        {.name = "multisample array layers retain separate clears and preserve every sample", .layers = {1,2,3,4},
            .depth = {QUARTER,QUARTER,QUARTER,QUARTER}, .depth_mask = 1, .passes = 4, .msaa = 1},
        {.name = "cube faces retain separate clears", .layers = {1,2,3,4},
            .depth = {QUARTER,QUARTER,QUARTER,QUARTER}, .depth_mask = 1, .passes = 4, .cube = 1},
        {.name = "single four-layer view uses standalone clear and preserves stencil and other mip/layers",
            .layers = {1}, .levels = {1}, .depth = {QUARTER}, .depth_mask = 1, .passes = 1, .fallback = 1},
        {.name = "depth larger than colour clears beyond the small pass without touching stencil",
            .layers = {2}, .levels = {1}, .depth = {QUARTER}, .depth_mask = 1, .passes = 1, .fallback = 2},
    };
    for (int phase = 0; phase < (ab ? 2 : 1); phase++) {
        atomic_store(&g_ab_phase, (long)phase);
        for (int format = 0; format < 2; format++) {
        for (unsigned ci = 0; ci < sizeof cases / sizeof *cases; ci++) {
            int levels = cases[ci].msaa ? 1 : LEVELS, samples = cases[ci].msaa ? 4 : 1;
            int layer_count = cases[ci].cube ? 6 : LAYERS;
            tex_t t = {.magic = MAGIC('T','E','X',0), .serial = 99700 + phase * 100 + format * 32 + ci};
            t.d = (nvn_texdesc){.target = cases[ci].msaa ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY : cases[ci].cube ? NVN_TEXTURE_TARGET_CUBEMAP : NVN_TEXTURE_TARGET_2D_ARRAY, .format = format ? NVN_FORMAT_DEPTH24_STENCIL8 : NVN_FORMAT_DEPTH32F_STENCIL8,
                .width = W, .height = W, .depth = cases[ci].cube ? 1 : LAYERS, .levels = levels, .samples = samples};
            if (cases[ci].target_view) t.d.samples = 0;   // Jamboree's native sample count
            MTLTextureDescriptor *desc = tex_desc(&t, 0); desc.usage |= MTLTextureUsagePixelFormatView;
            id<MTLTexture> base = [g_dev newTextureWithDescriptor:desc];
            CHECK(base && base.arrayLength == layer_count, "layered depth/stencil texture allocates");
            if (!base) continue;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            for (int lv = 0; lv < levels; lv++) for (int s = 0; s < layer_count; s++) {
                MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
                pd.depthAttachment.texture = pd.stencilAttachment.texture = base;
                pd.depthAttachment.level = pd.stencilAttachment.level = lv;
                pd.depthAttachment.slice = pd.stencilAttachment.slice = s;
                pd.depthAttachment.loadAction = pd.stencilAttachment.loadAction = MTLLoadActionClear;
                pd.depthAttachment.storeAction = pd.stencilAttachment.storeAction = MTLStoreActionStore;
                pd.depthAttachment.clearDepth = .75; pd.stencilAttachment.clearStencil = 0xa0 + s;
                [[cb renderCommandEncoderWithDescriptor:pd] endEncoding];
            }
            [cb commit]; [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusCompleted, "poisoned layers reach GPU");
            res_add(base); mtl_tex_init(&t); serial_put(t.serial, base);
            tex_t color = {.magic = MAGIC('T','E','X',0), .serial = t.serial + 1000};
            if (cases[ci].fallback == 2) {
                color.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                    .width = W / 4, .height = W / 4, .depth = 1, .levels = 1, .samples = 1};
                id<MTLTexture> ct = [g_dev newTextureWithDescriptor:tex_desc(&color, 0)];
                CHECK(ct != nil, "smaller colour attachment allocates"); if (!ct) return 1;
                res_add(ct); mtl_tex_init(&color); serial_put(color.serial, ct);
            }
            state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
            uint64_t ops[((LC_MAX + 2) * sizeof(clear_record) + 4 * sizeof(op_hdr) + 2 * sizeof(p_set_rt) + sizeof(p_copy_t2t)) / 8];
            uint8_t *p = (uint8_t *)ops; p_set_rt empty = {0};
            int nr = cases[ci].boundary ? LC_MAX + 2 : cases[ci].fallback ? 1 : 4;
            for (int k = 0; k < nr; k++) {
                clear_record record = {0}, *r = &record;
                r->rt_h = (op_hdr){OP_SET_RT, sizeof(op_hdr) + sizeof(p_set_rt)};
                r->rt.depth = &t; r->rt.has_dview = 1;
                r->rt.dview = (view_t){.set_levels = 1, .base_level = cases[ci].boundary ? 0 : (uint8_t)cases[ci].levels[k], .nlevels = 1,
                    .set_layers = 1, .min_layer = cases[ci].boundary ? (uint16_t)k : (uint16_t)cases[ci].layers[k], .nlayers = cases[ci].fallback == 1 ? 4 : 1};
                if (cases[ci].fallback == 2) { r->rt.n = 1; r->rt.color[0] = &color; }
                r->rt.dview.set_dsmode = cases[ci].unknown_view;
                if (cases[ci].format_view) { r->rt.dview.set_format = 1; r->rt.dview.format = t.d.format; }
                if (cases[ci].swizzle_view) { r->rt.dview.set_swizzle = 1;
                    for (int i = 0; i < 4; i++) r->rt.dview.swz[i] = i + 2; }
                if (cases[ci].target_view) {
                    r->rt.dview.set_target = 1;
                    r->rt.dview.target = cases[ci].target_view == 2 && (k & 1) ? NVN_TEXTURE_TARGET_2D : NVN_TEXTURE_TARGET_2D_ARRAY;
                }
                r->clear_h = (op_hdr){OP_CLEAR_DS, sizeof(op_hdr) + sizeof(p_clear_ds)};
                uint32_t bits = cases[ci].boundary ? QUARTER : cases[ci].depth[k];
                float depth; memcpy(&depth, &bits, 4);
                r->clear = (p_clear_ds){.depth = depth, .depth_mask = cases[ci].depth_mask, .stencil = 0x3c,
                    .stencil_mask = cases[ci].mixed_masks && k >= 2 ? 255 : cases[ci].stencil_mask};
                memcpy(p, r, sizeof *r); p += sizeof *r;
                if (cases[ci].fallback == 2) {
                    p_clear_color c = {.index = 0, .mask = 15, .rgba = {.25,.5,.75,1}};
                    append_op(&p, OP_CLEAR_COLOR, &c, sizeof c);
                }
                if (cases[ci].observer && k == 1) {
                    // Queue both first clears before the real copy observes them.
                    append_op(&p, OP_SET_RT, &empty, sizeof empty);
                    p_copy_t2t copy = {.src = &t, .dst = &t, .sr = {0,0,1,W,W,2}, .dr = {0,0,layer_count-3,W,W,2}};
                    append_op(&p, OP_COPY_T2T, &copy, sizeof copy);
                }
            }
            // Queue the final clear too, then let the real OP_END flush the queue.
            append_op(&p, OP_SET_RT, &empty, sizeof empty); append_op(&p, OP_END, NULL, 0);
            CHECK((size_t)(p - (uint8_t *)ops) <= sizeof ops, "replay records fit command memory");
            int enc0 = g_gp_n; uint64_t pass0 = g_stat_pass, load0 = g_bw_load, store0 = g_bw_store;
            mtl_exec((const uint8_t *)ops);
            int want_passes = (ab ? phase : enabled) ? cases[ci].passes : nr, render_encoders = 0;
            int want_encoders = want_passes + !!cases[ci].fallback;   // The standalone clear is not in g_stat_pass.
            for (int e = enc0; e < g_gp_n; e++) render_encoders += g_gp_lab[e].kind == 'R';
            printf("case %u phase %d format %#x: %s: %d render encoders, want %d\n", ci, phase, t.d.format, cases[ci].name, render_encoders, want_encoders);
            CHECK(render_encoders == want_encoders, "actual clear-only encoder count includes standalone fallback");
            CHECK(g_gp_n - enc0 == want_encoders + cases[ci].observer, "copy observer has its own encoder between clears");
            CHECK(g_stat_pass - pass0 == (uint64_t)want_passes, "render pass count excludes standalone fallback");
            if (cases[ci].fallback) CHECK(g_pass.tw == (cases[ci].fallback == 2 ? W / 4 : W / 2) && g_pass.th == g_pass.tw,
                "final pass has the smaller attachment size; standalone clear covers full depth view");
            uint64_t pixels = 0, stencil_pixels = 0;
            for (int k = 0; k < nr; k++) {
                int lv = cases[ci].boundary ? 0 : cases[ci].levels[k]; uint64_t area = (uint64_t)(W >> lv) * (W >> lv) * samples;
                pixels += area; if (cases[ci].stencil_mask || (cases[ci].mixed_masks && k >= 2)) stencil_pixels += area;
            }
            uint64_t want_load = pixels * (!cases[ci].depth_mask || cases[ci].fallback ? 4 : 0) + (pixels - stencil_pixels);
            uint64_t want_store = pixels * (cases[ci].depth_mask ? 4 : 0) + stencil_pixels + (cases[ci].fallback == 2 ? (W / 4) * (W / 4) * 4 : 0);
            // Existing ledger excludes the standalone encoder; after it, the final pass loads depth.
            CHECK(g_bw_load - load0 == want_load, "load ledger counts attached planes (final pass only for fallback)");
            CHECK(g_bw_store - store0 == want_store, "store ledger counts written planes (final pass only for fallback)");
            mtl_sub_commit(); mtl_drain();
            CHECK(!mtl_last_commit_error(), "production clear has no GPU error");
            id<MTLTexture> stencil = [base newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
            for (uint32_t lv = 0; lv < levels; lv++) {
                unsigned w = W >> lv;
                id<MTLBuffer> out = [g_dev newBufferWithLength:w * w * layer_count * samples * 8 options:MTLResourceStorageModeShared];
                cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                [e setComputePipelineState:cases[ci].msaa ? reader_ms : reader]; [e setTexture:stencil atIndex:0]; [e setTexture:base atIndex:1];
                [e setBuffer:out offset:0 atIndex:0]; if (!cases[ci].msaa) [e setBytes:&lv length:4 atIndex:1];
                [e dispatchThreads:MTLSizeMake(w,w,layer_count) threadsPerThreadgroup:MTLSizeMake(w,w,1)];
                [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
                CHECK(cb.status == MTLCommandBufferStatusCompleted, "both planes read back on GPU");
                const uint32_t *px = out.contents; int valid = 1;
                for (int s = 0; s < layer_count; s++) {
                    uint32_t want_d = OLD_D, want_s = 0xa0 + s;
                    if (cases[ci].boundary) {
                        if (!lv && s < LC_MAX + 2) want_d = QUARTER;
                    } else {
                        for (int k = 0; k < nr; k++) if (s >= cases[ci].layers[k] && s < cases[ci].layers[k] + (cases[ci].fallback == 1 ? 4 : 1) && cases[ci].levels[k] == (int)lv) {
                            if (cases[ci].depth_mask) want_d = cases[ci].depth[k];
                            if (cases[ci].stencil_mask || (cases[ci].mixed_masks && k >= 2)) want_s = 0x3c;
                        }
                        if (cases[ci].observer && !lv && s >= layer_count - 3 && s < layer_count - 1) {
                            want_d = QUARTER; want_s = 0xa1 + s - (layer_count - 3);
                        }
                    }
                    for (unsigned p = 0; p < w*w*samples; p++) if (px[2*(s*w*w*samples+p)] != want_d || px[2*(s*w*w*samples+p)+1] != want_s) {
                        valid = 0; fprintf(stderr, "case %u mip %u layer %d pixel %u: depth %#x stencil %#x, want %#x/%#x\n", ci,lv,s,p,px[2*(s*w*w*samples+p)],px[2*(s*w*w*samples+p)+1],want_d,want_s); break;
                    }
                }
                CHECK(valid, cases[ci].name);
            }
            mtl_tex_release(&t);
            if (cases[ci].fallback == 2) mtl_tex_release(&color);
        }
        }
    }
    printf("layered clear batching: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
