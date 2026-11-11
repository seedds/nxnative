// A colour clear writes only the channels the bound channel mask state lets through, as a draw does: cleared under a mask of G and B, a target keeps its R and A
// (Nintendo Switch Sports Resort's screen-space shadow target; docs/reference/rendering.md, A colour clear under a partial channel mask leaves the other channels). The mask comes from the state the replay holds or from a state
// bind earlier in the same records. With every channel of the target off the clear is whole (what a console does there is not known).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { W = 4, OLD_C = 0x11223344 };   // RGBA8 bytes R 0x44, G 0x33, B 0x22, A 0x11; the clear is 0.5 (0x80) in every channel

int main(void) { @autoreleasepool {
    mtl_init();
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    // state: the channel mask the replay holds when the records start; bind: a channel mask bound by a record before the clear (-1: none); clear: the clear's own mask
    struct { uint32_t state; int64_t bind; int clear; uint32_t want; const char *label; } cases[] = {
        {0xffffffffu, -1, 0xf, 0x80808080, "every channel let through: the clear replaces all four"},
        {0xfffffff6u, -1, 0xf, 0x11808044, "a channel mask of G and B: the clear keeps R and A"},
        {0xffffffffu, 0xfffffff6, 0xf, 0x11808044, "the same mask bound by a record before the clear"},
        {0xfffffff6u, -1, 0x3, 0x11228044, "the clear's own mask R and G under a state of G and B: only G"},
        {0xfffffff0u, -1, 0xf, 0x80808080, "every channel of the target off: the clear is whole, as without the state"},
        {0xfffffff6u, -1, 0x9, OLD_C, "the clear's own mask R and A under a state of G and B: nothing to write"},
        {0xffffff6fu, -1, 0xf, 0x80808080, "another target's mask does not hold this one back"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        tex_t t = {.magic = MAGIC('T','E','X',0), .serial = 99701 + i};
        t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = W, .height = W, .depth = 1, .levels = 1, .samples = 1};
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:tex_desc(&t, 0)];
        CHECK(base != nil, "synthetic target allocates");
        if (!base) continue;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *init = [MTLRenderPassDescriptor renderPassDescriptor];
        init.colorAttachments[0].texture = base; init.colorAttachments[0].loadAction = MTLLoadActionClear; init.colorAttachments[0].storeAction = MTLStoreActionStore;
        init.colorAttachments[0].clearColor = MTLClearColorMake(0x44 / 255., 0x33 / 255., 0x22 / 255., 0x11 / 255.);
        [[cb renderCommandEncoderWithDescriptor:init] endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "the old content reaches the GPU");
        res_add(base); mtl_tex_init(&t); serial_put(t.serial, base);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        S.chmask.mask = cases[i].state;
        R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &t;
        uint8_t ops[256] = {0}; size_t at = 0;
        if (cases[i].bind >= 0) {
            op_hdr h = {OP_BIND_STATE, (uint32_t)(sizeof(op_hdr) + offsetof(p_bind_state, bytes) + sizeof(chmask_st))}; memcpy(ops + at, &h, sizeof h);
            p_bind_state b = {.kind = 2}; chmask_st m = {(uint32_t)cases[i].bind}; memcpy(b.bytes, &m, sizeof m); memcpy(ops + at + sizeof h, &b, offsetof(p_bind_state, bytes) + sizeof m);
            at += h.size;
        }
        op_hdr h = {OP_CLEAR_COLOR, sizeof(op_hdr) + sizeof(p_clear_color)}; memcpy(ops + at, &h, sizeof h);
        p_clear_color c = {.index = 0, .mask = cases[i].clear, .rgba = {.5f, .5f, .5f, .5f}}; memcpy(ops + at + sizeof h, &c, sizeof c); at += h.size;
        op_hdr end = {OP_END, sizeof(op_hdr)}; memcpy(ops + at, &end, sizeof end);
        mtl_exec(ops);
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "the clear completes without a GPU error");
        id<MTLBuffer> result = [g_dev newBufferWithLength:W * W * 4 options:MTLResourceStorageModeShared];
        cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:base sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, W, 1) toBuffer:result destinationOffset:0
            destinationBytesPerRow:W * 4 destinationBytesPerImage:W * W * 4];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "the target is read back");
        const uint32_t *px = result.contents; int valid = 1;
        for (int k = 0; k < W * W; k++) if (px[k] != cases[i].want) { valid = 0; fprintf(stderr, "case %u texel %d: %#x, want %#x\n", i, k, px[k], cases[i].want); break; }
        CHECK(valid, cases[i].label);
        printf("case %u: %s %s\n", i, cases[i].label, valid ? "PASS" : "FAIL");
        mtl_tex_release(&t);
    }
    printf("clears under a channel mask: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
