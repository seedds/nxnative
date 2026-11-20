// A whole clear waiting as a lazy clear (lc_push) lands before a later scissored clear of the same target, never after it: the pass a triangle clear opens takes it as its load
// action (clear_color_tri, clear_ds_tri), and a scissored zero clear's copy lands it first (clear_rect_zero). FINAL FANTASY IX clears the window whole, then its 4:3 field view
// with a scissor, and draws the field in that pass; the whole clear landed in the next pass and wiped the field.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static uint8_t ops[1024]; static size_t nops;
static void *op(uint32_t code, size_t size) { op_hdr h = {code, (uint32_t)(sizeof h + size)}; memcpy(ops + nops, &h, sizeof h); void *a = ops + nops + sizeof h; memset(a, 0, size); nops += h.size; return a; }

int main(void) { @autoreleasepool {
    mtl_init();
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    // kind 0: a colour clear drawn as a triangle; 1: a zero colour clear copied into the rectangle; 2: a depth clear drawn as a triangle
    struct { int kind, side; p_scissor scissor; float whole, part; uint64_t taken, landed; const char *label; } cases[] = {
        {0, 8, {2, 1, 3, 2}, 1.f, .5f, 1, 0, "waiting whole colour clear, then a scissored colour clear drawn as a triangle: the triangle's pass takes it as its load action"},
        {1, 8, {2, 1, 3, 2}, 1.f, 0.f, 0, 1, "waiting whole colour clear, then a scissored zero clear copied into its rectangle: it lands before the copy"},
        {2, 8, {2, 1, 3, 2}, .75f, .25f, 1, 0, "waiting whole depth clear, then a scissored depth clear drawn as a triangle: the triangle's pass takes it as its load action"},
        {1, 1536, {2, 1, 1532, 1533}, 1.f, 0.f, 0, 1, "a scissored zero clear taller than one copy changes only its rectangle"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        int depth = cases[i].kind == 2, side = cases[i].side;
        p_scissor sc = cases[i].scissor;
        tex_t t = {.magic = MAGIC('T','E','X',0), .serial = 99801 + i};
        t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = depth ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8, .width = side, .height = side, .depth = 1, .levels = 1, .samples = 1};
        MTLTextureDescriptor *d = tex_desc(&t, 0); d.usage |= MTLTextureUsageRenderTarget;
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
        CHECK(base != nil, "target allocates"); if (!base) continue;
        MTLRenderPassDescriptor *init = [MTLRenderPassDescriptor renderPassDescriptor];   // old content, distinct from both clears
        if (depth) { init.depthAttachment.texture = base; init.depthAttachment.loadAction = MTLLoadActionClear; init.depthAttachment.storeAction = MTLStoreActionStore; init.depthAttachment.clearDepth = .5; }
        else { init.colorAttachments[0].texture = base; init.colorAttachments[0].loadAction = MTLLoadActionClear; init.colorAttachments[0].storeAction = MTLStoreActionStore; init.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1); }
        id<MTLCommandBuffer> cb = [queue commandBuffer]; [[cb renderCommandEncoderWithDescriptor:init] endEncoding]; [cb commit]; [cb waitUntilCompleted];
        res_add(base); mtl_tex_init(&t); serial_put(t.serial, base);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        uint64_t taken0, landed0; mtl_lc_stats(&taken0, &landed0);
        p_set_rt rt = {0}; if (depth) rt.depth = &t; else { rt.n = 1; rt.color[0] = &t; }
        nops = 0;
        *(p_set_rt *)op(OP_SET_RT, sizeof rt) = rt;
        float w = cases[i].whole, p = cases[i].part;
        if (depth) *(p_clear_ds *)op(OP_CLEAR_DS, sizeof(p_clear_ds)) = (p_clear_ds){.depth = w, .depth_mask = 1};
        else *(p_clear_color *)op(OP_CLEAR_COLOR, sizeof(p_clear_color)) = (p_clear_color){0, 0xf, {w, 0, 0, 1}};
        *(p_set_rt *)op(OP_SET_RT, sizeof rt) = rt;   // the same targets again with no draw: the whole clear waits (lc_push)
        *(p_scissor *)op(OP_SCISSOR, sizeof sc) = sc;
        if (depth) *(p_clear_ds *)op(OP_CLEAR_DS, sizeof(p_clear_ds)) = (p_clear_ds){.depth = p, .depth_mask = 1};
        else *(p_clear_color *)op(OP_CLEAR_COLOR, sizeof(p_clear_color)) = (p_clear_color){0, 0xf, {p, p, p, p}};
        op(OP_END, 0);
        mtl_exec(ops);
        uint64_t taken1, landed1; mtl_lc_stats(&taken1, &landed1);
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "replay completes without GPU error");
        size_t row = (size_t)side * 4, image = row * side;
        id<MTLBuffer> out = [g_dev newBufferWithLength:image options:MTLResourceStorageModeShared];
        CHECK(out != nil, "readback buffer allocates"); if (!out) { mtl_tex_release(&t); continue; }
        cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:base sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(side, side, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:image];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        int valid = 1;
        for (int y = 0; y < side; y++) for (int x = 0; x < side; x++) {
            int inside = x >= sc.x && x < sc.x + sc.w && y >= sc.y && y < sc.y + sc.h;
            float v = inside ? p : w;
            const uint8_t *px = (const uint8_t *)out.contents + y * row + x * 4;
            if (depth) { float got; memcpy(&got, px, 4); valid &= got == v; }
            else { uint8_t r = (uint8_t)lrintf(v * 255), a = inside ? r : 255, g = inside ? r : 0;
                valid &= abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - g) <= 1 && abs(px[3] - a) <= 1; }
        }
        CHECK(valid, cases[i].label);
        CHECK(taken1 - taken0 == cases[i].taken && landed1 - landed0 == cases[i].landed, "waiting clears taken / landed as expected");
        printf("case %u: %s %s (taken %llu, landed %llu)\n", i, cases[i].label, valid ? "PASS" : "FAIL", (unsigned long long)(taken1 - taken0), (unsigned long long)(landed1 - landed0));
        mtl_tex_release(&t);
    }
    CHECK(zero_buf(1).length == (4u << 20), "the large rectangle keeps the zero buffer at 4 MiB");
    printf("lazy clear order: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
