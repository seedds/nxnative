// Waiting clears of consecutive layers of one 2D array level land in one layered pass (lc_land, lc_layer_run; DM_NVN_LC_LAYERS): every layer of the run holds its clear, the
// layers outside it keep their content, and a run stops at a gap, another colour or a layer out of order. Mario Kart World clears the five layers of a 128x128 array one at a time
// every frame, and each landed in a pass of its own. Run with DM_NVN_LC_LAYERS=0 too: the same pixels, one pass a clear
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static uint8_t ops[4096]; static size_t nops;
static void *op(uint32_t code, size_t size) { op_hdr h = {code, (uint32_t)(sizeof h + size)}; memcpy(ops + nops, &h, sizeof h); void *a = ops + nops + sizeof h; memset(a, 0, size); nops += h.size; return a; }
enum { W_ = 8, LAYERS = 6 };

int main(void) { @autoreleasepool {
    const char *e = getenv("DM_NVN_LC_LAYERS"); int on = !e || strcmp(e, "0") != 0;
    mtl_init();
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    // Each case clears these layers in order, with these colours (0 red, 1 green); merged: the passes they land in with the switch on
    struct { int n, layer[5], colour[5], merged; const char *label; } cases[] = {
        {5, {0, 1, 2, 3, 4}, {0, 0, 0, 0, 0}, 1, "layers 0-4, one colour: one layered pass"},
        {3, {0, 1, 3}, {0, 0, 0}, 2, "layers 0, 1 and 3: the gap ends the run"},
        {3, {1, 2, 3}, {0, 1, 1}, 2, "layer 1 red, 2 and 3 green: the colour ends the run"},
        {2, {2, 1}, {0, 0}, 2, "layer 2 then 1: a run goes up"},
    };
    for (unsigned ci = 0; ci < sizeof cases / sizeof *cases; ci++) {
        tex_t t = {.magic = MAGIC('T','E','X',0), .serial = 99901 + ci};
        t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .width = W_, .height = W_, .depth = LAYERS, .levels = 1, .samples = 1};
        MTLTextureDescriptor *d = tex_desc(&t, 0); d.usage |= MTLTextureUsageRenderTarget;
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
        CHECK(base != nil && base.textureType == MTLTextureType2DArray && base.arrayLength == LAYERS, "a 2D array target of six layers"); if (!base) continue;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int l = 0; l < LAYERS; l++) {   // old content: blue in every layer
            MTLRenderPassDescriptor *init = [MTLRenderPassDescriptor renderPassDescriptor]; init.colorAttachments[0].texture = base; init.colorAttachments[0].slice = (NSUInteger)l;
            init.colorAttachments[0].loadAction = MTLLoadActionClear; init.colorAttachments[0].storeAction = MTLStoreActionStore; init.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
            [[cb renderCommandEncoderWithDescriptor:init] endEncoding]; }
        [cb commit]; [cb waitUntilCompleted];
        res_add(base); mtl_tex_init(&t); serial_put(t.serial, base);
        tex_t other = {.magic = MAGIC('T','E','X',0), .serial = 99951 + ci};   // a plain target the last SetRenderTargets names, so the last clear waits too
        other.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = W_, .height = W_, .depth = 1, .levels = 1, .samples = 1};
        MTLTextureDescriptor *od = tex_desc(&other, 0); od.usage |= MTLTextureUsageRenderTarget; id<MTLTexture> ob = [g_dev newTextureWithDescriptor:od];
        res_add(ob); mtl_tex_init(&other); serial_put(other.serial, ob);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        uint64_t taken0, landed0, merged0 = mtl_lc_merged(); mtl_lc_stats(&taken0, &landed0);
        nops = 0;
        for (int i = 0; i < cases[ci].n; i++) {
            p_set_rt rt = {0}; rt.n = 1; rt.color[0] = &t; rt.has_cview[0] = 1; rt.cview[0].set_layers = 1; rt.cview[0].min_layer = (uint16_t)cases[ci].layer[i]; rt.cview[0].nlayers = 1;
            *(p_set_rt *)op(OP_SET_RT, sizeof rt) = rt;
            float g = (float)cases[ci].colour[i];
            *(p_clear_color *)op(OP_CLEAR_COLOR, sizeof(p_clear_color)) = (p_clear_color){0, 0xf, {1 - g, g, 0, 1}};
        }
        p_set_rt rt = {0}; rt.n = 1; rt.color[0] = &other; *(p_set_rt *)op(OP_SET_RT, sizeof rt) = rt;
        op(OP_END, 0);
        mtl_exec(ops);
        uint64_t taken1, landed1, merged1 = mtl_lc_merged(); mtl_lc_stats(&taken1, &landed1);
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "replay completes without GPU error");
        id<MTLBuffer> out = [g_dev newBufferWithLength:LAYERS * W_ * W_ * 4 options:MTLResourceStorageModeShared];
        cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        for (int l = 0; l < LAYERS; l++)
            [b copyFromTexture:base sourceSlice:(NSUInteger)l sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W_, W_, 1) toBuffer:out destinationOffset:(NSUInteger)(l * W_ * W_ * 4) destinationBytesPerRow:W_ * 4 destinationBytesPerImage:W_ * W_ * 4];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        int valid = 1;
        for (int l = 0; l < LAYERS; l++) {
            int want = -1; for (int i = 0; i < cases[ci].n; i++) if (cases[ci].layer[i] == l) want = cases[ci].colour[i];   // -1: not cleared, still blue
            uint8_t r = want < 0 ? 0 : want ? 0 : 255, g = want == 1 ? 255 : 0, bl = want < 0 ? 255 : 0;
            for (int p = 0; p < W_ * W_; p++) { const uint8_t *px = (const uint8_t *)out.contents + (l * W_ * W_ + p) * 4; valid &= px[0] == r && px[1] == g && px[2] == bl && px[3] == 255; }
        }
        CHECK(valid, cases[ci].label);
        uint64_t want_passes = on ? (uint64_t)cases[ci].merged : (uint64_t)cases[ci].n;
        CHECK(landed1 - landed0 == want_passes && taken1 == taken0, "the clears landed in as many passes as expected");
        CHECK(merged1 - merged0 == (uint64_t)cases[ci].n - want_passes, "and the merged count says how many layers shared a pass");
        printf("case %u: %s %s (%llu passes)\n", ci, cases[ci].label, valid ? "PASS" : "FAIL", (unsigned long long)(landed1 - landed0));
        mtl_tex_release(&t); mtl_tex_release(&other);
    }
    printf("lazy clear layers (DM_NVN_LC_LAYERS %s): %d checks, %d failed\n", on ? "on" : "off", checks, failed);
    return failed != 0;
} }

