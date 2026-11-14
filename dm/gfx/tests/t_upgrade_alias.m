// A usage upgrade replaces one shared image, including every serial naming it. Buffer textures share one view by storage, made with every usage they can take, so neither an image
// write nor a format view rebuilds it, and texture objects made again every frame over the same range add nothing to the residency set (GitHub issues #1061 and #2423).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    setenv("DM_TBUF_ALIGN", "16", 1);   // every Mac viewed at the M4 Pro's alignment (tbuf_align reads it once): the buffer texture below is 2 R32 elements past a boundary
    mtl_init(); mtl_sub_begin();
    tex_t a = {.magic=MAGIC('T','E','X',0), .serial=99301};
    a.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&a.d, &a.L); tex_t b = a, ghost = a;
    b.serial++; ghost.serial += 2;
    mtl_tex_init(&a); mtl_tex_init(&b);
    id<MTLTexture> old = tex_mtl(&a);
    CHECK(tex_mtl(&b) == old && tex_mtl(&ghost) == old, "live and finalized serials initially share storage");
    tex_t image = a; image.serial += 3; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; mtl_tex_init(&image);
    CHECK(tex_mtl(&image) == old, "an IMAGE sampling descriptor shares storage without a usage change");
    mtl_reg_tex(950, &image, NULL); S.img[5][0] = 950;
    NvShader *writer = [NvShader new]; writer->rf.nb = 1; writer->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .rw=1};
    NvProg *program = [NvProg new]; program->sh[5] = writer; image_prepare(program, 1u << 5);
    old = tex_mtl(&image);
    CHECK(old.usage & MTLTextureUsageShaderWrite, "IMAGE descriptor adds shader-write usage");
    CHECK(tex_mtl(&a) == old && tex_mtl(&b) == old && tex_mtl(&ghost) == old, "IMAGE usage does not split storage across existing descriptors");
    // Exercise replacement at the hash table's growth threshold: reinserting while iterating would resize and reshuffle it.
    uint64_t extra = 99400;
    pthread_mutex_lock(&g_smu); own_stats st = own_stats_locked(); pthread_mutex_unlock(&g_smu);
    while ((st.live + 1) * 4 <= st.live_cap * 3) { tex_put((1ull << 62) + extra, old); extra++; pthread_mutex_lock(&g_smu); st = own_stats_locked(); pthread_mutex_unlock(&g_smu); }   // live entries (external serials), counted as the owner counts them
    uint32_t capacity = st.live_cap, count = st.live;
    id<MTLTexture> upgraded = tex_upgrade_pfv(&a, old, MTLPixelFormatR32Uint);
    CHECK(upgraded && upgraded != old, "usage upgrade creates a replacement");
    pthread_mutex_lock(&g_smu); st = own_stats_locked(); pthread_mutex_unlock(&g_smu);
    CHECK(st.live_cap == capacity && st.live == count, "replacement does not grow or reshape the serial table");
    int all = 1; for (uint64_t serial = 99400; serial < extra; serial++) all &= tex_get((1ull << 62) + serial) == upgraded;
    CHECK(all, "every shared serial at the growth threshold sees the replacement");
    CHECK(tex_get(b.serial) == upgraded, "other live serial sees the replacement");
    CHECK(tex_get(ghost.serial) == upgraded, "finalized serial sees the replacement");
    pthread_mutex_lock(&g_smu); int moved = !own_shared_locked(old) && own_shared_locked(upgraded); pthread_mutex_unlock(&g_smu);
    CHECK(moved, "sharing membership follows the replacement");
    uint32_t pixels[16]; for (int i = 0; i < 16; i++) pixels[i] = 0x44332211u;
    id<MTLBuffer> src = [g_dev newBufferWithBytes:pixels length:sizeof pixels options:MTLResourceStorageModeShared]; res_add(src);
    id<MTL4ComputeCommandEncoder> encoder = blit();
    enc_b_copy_buffer_texture(encoder, src, 0, 16, sizeof pixels, MTLSizeMake(4,4,1), upgraded, 0, 0, MTLOriginMake(0,0,0));
    for (int j = 0; j < 2; j++) {
        id<MTLTexture> texture = tex_mtl(j ? &ghost : &b);
        id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof pixels options:MTLResourceStorageModeShared]; res_add(dst);
        encoder = blit(); enc_b_copy_texture_buffer(encoder, texture, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), dst, 0, 16, sizeof pixels);
        mtl_sub_commit(); mtl_drain();
        CHECK(!memcmp(dst.contents, pixels, sizeof pixels), "other serial reads every GPU-written pixel after upgrade");
        if (!j) mtl_sub_begin();
    }
    CHECK(![g_res_objects containsObject:old], "retired base is removed from the residency set after GPU completion");
    {   // Buffer textures (TARGET_BUFFER), 16 R32I elements at pool offset 0x108: 8 bytes past a 16-byte boundary, so the view starts 2 elements before them (tbuf_lead)
        enum { POOL = 65536, OFF = 0x108, W = 16 };
        uint8_t *mem; if (posix_memalign((void **)&mem, (size_t)getpagesize(), POOL)) abort();
        memset(mem, 0xEE, POOL); mtl_pool_add(mem, POOL);
        pool_t pool = {.magic = MAGIC('P','O','O','L'), .mem = mem, .size = POOL, .flags = 0x24};
        mtl_sub_begin();
        tex_t ta = {.magic = MAGIC('T','E','X',0), .serial = 99601, .pool = &pool, .off = OFF, .mem = mem + OFF};
        ta.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32I, .width = W, .height = 1, .depth = 1, .levels = 1, .samples = 1};
        nvn_tex_layout(&ta.d, &ta.L);
        tex_t tb = ta, tghost = ta, timage = ta; tb.serial = 99602; tghost.serial = 99603; timage.serial = 99604; timage.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
        mtl_tex_init(&ta); mtl_tex_init(&tb); mtl_tex_init(&timage);
        id<MTLTexture> view = tex_mtl(&ta);
        CHECK(view && view.textureType == MTLTextureTypeTextureBuffer && view.bufferOffset == OFF - 8 && view.width == W + 2, "a buffer texture's view starts at the boundary below it, 2 elements wider");
        CHECK((view.usage & (MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView)) == (MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView) && tex_atomic_capable(view),
              "one made without IMAGE takes image writes, atomics and format views");
        CHECK(tex_mtl(&tb) == view && tex_mtl(&tghost) == view && tex_mtl(&timage) == view, "live, finalized and IMAGE objects over the same range share its view");
        mtl_reg_tex(961, &ta, NULL); mtl_reg_image(962, &timage, NULL);
        void *rv = NULL; tex_fast_rid(961, &rv);
        os_unfair_lock_lock(&g_rmu); uint32_t l1 = tbuf_lead(961, view), l2 = tbuf_lead(962, view); os_unfair_lock_unlock(&g_rmu);
        CHECK((__bridge id)rv == view && l1 == 2 && l2 == 2, "each registered ID of the shared view moves its coordinates by the same 2 elements");
        uint64_t ups = g_pfv_upgrades; S.img[5][0] = 962;
        writer->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureTypeTextureBuffer, .dpf = NVN_DPF_SINT, .rw = 1, .atom = 1};
        program->c_img = 0; image_prepare(program, 1u << 5);
        CHECK(g_pfv_upgrades == ups && tex_mtl(&timage) == view && tex_mtl(&ta) == view, "an atomic image write through the IMAGE object rebuilds nothing");
        id<MTLTexture> uview = [view newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
        CHECK(uview && uview.textureType == MTLTextureTypeTextureBuffer, "an R32UI view of it needs no upgrade");
        // Unreal's transient buffer textures: a new object over the same range every frame, released after use
        NSUInteger resident = g_res_objects.allObjects.count; int same = 1;
        for (uint64_t i = 0; i < 64; i++) { tex_t tn = ta; tn.serial = 99700 + i; mtl_tex_init(&tn); same &= tex_mtl(&tn) == view; mtl_tex_release(&tn); }
        CHECK(same && g_res_objects.allObjects.count == resident, "64 objects made again over the same range share the view and add nothing to the residency set");
        tex_t wide = ta; wide.serial = 99605; wide.d.width = W * 2; nvn_tex_layout(&wide.d, &wide.L); mtl_tex_init(&wide);
        id<MTLTexture> wv = tex_mtl(&wide);
        CHECK(wv && wv != view && wv.width == 2 * W + 2 && tex_mtl(&ta) == view, "another width over the same range is a view of its own, and the first stays while live objects hold it");
        tex_t bgra = ta; bgra.serial = 99606; bgra.off = 0x400; bgra.mem = mem + 0x400; bgra.d.format = NVN_FORMAT_BGRA8; nvn_tex_layout(&bgra.d, &bgra.L); mtl_tex_init(&bgra);
        id<MTLTexture> bv = tex_mtl(&bgra);
        CHECK(bv && !(bv.usage & MTLTextureUsageShaderWrite), "a BGRA8 view is made without the write usage Metal refuses it");
        mtl_sub_commit(); mtl_drain();
        // A GPU write through the R32UI view, each element at its coordinate moved by the lead, lands in the pool; another object's view reads it back
        NSError *error = nil;
        const char *msl = "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void put(texture_buffer<uint, access::write> t [[texture(0)]], uint i [[thread_position_in_grid]]) { t.write(uint4(0x11223300u + i, 0, 0, 0), i + 2); }\n"
            "kernel void get(texture_buffer<int> t [[texture(0)]], device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = uint(t.read(i + 2).x); }\n";
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(msl) options:nil error:&error];
        id<MTLComputePipelineState> pput = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"put"] error:&error];
        id<MTLComputePipelineState> pget = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"get"] error:&error];
        CHECK(pput && pget, "buffer-texture kernels compiled");
        id<MTLCommandQueue> q = [g_dev newCommandQueue];
        id<MTLBuffer> out = [g_dev newBufferWithLength:W * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cmd = [q commandBuffer]; id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pput]; [enc setTexture:uview atIndex:0]; [enc dispatchThreads:MTLSizeMake(W, 1, 1) threadsPerThreadgroup:MTLSizeMake(W, 1, 1)];
        [enc setComputePipelineState:pget]; [enc setTexture:tex_mtl(&tghost) atIndex:0]; [enc setBuffer:out offset:0 atIndex:0]; [enc dispatchThreads:MTLSizeMake(W, 1, 1) threadsPerThreadgroup:MTLSizeMake(W, 1, 1)];
        [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
        uint32_t want[W]; for (uint32_t i = 0; i < W; i++) want[i] = 0x11223300u + i;
        CHECK(cmd.status == MTLCommandBufferStatusCompleted && !memcmp(mem + OFF, want, sizeof want), "a write through the shared view lands on the texture's own elements in the pool");
        CHECK(!memcmp(out.contents, want, sizeof want), "a finalized object's view reads them back");
        CHECK(mem[OFF - 1] == 0xEE && mem[OFF + W * 4] == 0xEE, "nothing around them is written");
        mtl_tex_release(&ta); mtl_tex_release(&tb); mtl_tex_release(&timage); mtl_tex_release(&wide); mtl_tex_release(&bgra);
        mtl_alias_drop_pool(&pool); mtl_pool_del(mem);
    }
    printf("texture upgrade aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
