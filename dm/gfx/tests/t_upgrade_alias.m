// A usage upgrade replaces one shared image, including every serial naming it.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
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
    while ((g_by_serial.n + 1) * 4 <= g_by_serial.cap * 3) { hm_put_id(&g_by_serial, &extra, old); extra++; }
    uint32_t capacity = g_by_serial.cap, count = g_by_serial.n;
    id<MTLTexture> upgraded = tex_upgrade_pfv(&a, old, MTLPixelFormatR32Uint);
    CHECK(upgraded && upgraded != old, "usage upgrade creates a replacement");
    CHECK(g_by_serial.cap == capacity && g_by_serial.n == count, "replacement does not grow or reshape the serial table");
    int all = 1; for (uint64_t serial = 99400; serial < extra; serial++) all &= tex_get(serial) == upgraded;
    CHECK(all, "every shared serial at the growth threshold sees the replacement");
    CHECK(tex_get(b.serial) == upgraded, "other live serial sees the replacement");
    CHECK(tex_get(ghost.serial) == upgraded, "finalized serial sees the replacement");
    CHECK(!alias_has(old) && alias_has(upgraded), "sharing membership follows the replacement");
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

    // Buffer texture (NVN_TEXTURE_TARGET_BUFFER): shared by storage across live and finalized descriptors, IMAGE usage upgrade, format-view upgrade, and GPU/CPU visibility.
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 65536)) abort(); memset(mem, 0, 65536);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=65536, .flags=0x24}; mtl_pool_add(mem, 65536);
    mtl_sub_begin();
    tex_t ta = {.magic=MAGIC('T','E','X',0), .serial=99601, .pool=&pool, .off=256, .mem=(uint8_t *)mem + 256};
    ta.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_BUFFER, .format=NVN_FORMAT_R32I, .width=16, .height=1, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&ta.d, &ta.L); tex_t tb = ta, tghost = ta;
    tb.serial++; tghost.serial += 2;
    mtl_tex_init(&ta); mtl_tex_init(&tb);
    id<MTLTexture> told = tex_mtl(&ta);
    CHECK(told && told.textureType == MTLTextureTypeTextureBuffer && told.buffer != nil && told.bufferOffset == 256, "buffer texture resolves on its pool buffer");
    CHECK(!(told.usage & MTLTextureUsageShaderWrite), "buffer texture without IMAGE starts read-only");
    CHECK(tex_mtl(&tb) == told && tex_mtl(&tghost) == told, "live and finalized buffer-texture serials share storage");
    tex_t timage = ta; timage.serial += 3; timage.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; mtl_tex_init(&timage);
    CHECK(tex_mtl(&timage) == told, "an IMAGE buffer-texture descriptor shares storage without a usage change");
    mtl_reg_tex(951, &timage, NULL); S.img[5][0] = 951;
    writer->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .dtt=MTLTextureTypeTextureBuffer, .dpf=NVN_DPF_SINT, .rw=1, .atom=1};
    program->c_img = 0; image_prepare(program, 1u << 5);
    id<MTLTexture> twritable = tex_mtl(&timage);
    CHECK(twritable && twritable != told && (twritable.usage & MTLTextureUsageShaderWrite), "IMAGE buffer-texture descriptor adds shader-write usage");
    CHECK(tex_atomic_capable(twritable), "R32I buffer-texture upgrade enables shader atomics");
    CHECK(twritable.buffer == told.buffer && twritable.bufferOffset == told.bufferOffset, "upgraded buffer texture keeps its pool buffer and offset");
    CHECK(tex_mtl(&ta) == twritable && tex_mtl(&tb) == twritable && tex_mtl(&tghost) == twritable, "IMAGE usage does not split storage across buffer-texture descriptors");
    CHECK(!alias_has(told) && alias_has(twritable), "buffer-texture sharing membership follows the IMAGE upgrade");
    id<MTLTexture> tupgraded = tex_upgrade_pfv(&ta, twritable, MTLPixelFormatR32Uint);
    CHECK(tupgraded && tupgraded != twritable && (tupgraded.usage & (MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView)) == (MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView), "buffer-texture format-view upgrade preserves shader-write usage");
    CHECK(tex_atomic_capable(tupgraded), "buffer-texture format-view upgrade preserves atomic capability");
    CHECK(tupgraded.buffer == told.buffer && tupgraded.bufferOffset == told.bufferOffset, "format-view upgraded buffer texture keeps its pool buffer and offset");
    CHECK(tex_get(tb.serial) == tupgraded && tex_get(tghost.serial) == tupgraded, "live and finalized buffer-texture serials see the format-view replacement");
    CHECK(!alias_has(twritable) && alias_has(tupgraded), "buffer-texture sharing membership follows the format-view replacement");
    id<MTLTexture> uview = [tupgraded newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
    CHECK(uview != nil && uview.textureType == MTLTextureTypeTextureBuffer, "upgraded buffer texture supports a different-format view");
    mtl_sub_commit(); mtl_drain();
    CHECK(![g_res_objects containsObject:told] && ![g_res_objects containsObject:twritable], "retired buffer-texture bases are removed from the residency set after GPU completion");
    NSError *err = nil;
    const char *msl = "#include <metal_stdlib>\nusing namespace metal;\n"
                      "kernel void put(texture_buffer<uint, access::write> t [[texture(0)]], uint i [[thread_position_in_grid]]) { t.write(uint4(0x11223300u + i, 0, 0, 0), i); }\n"
                      "kernel void get(texture_buffer<uint> t [[texture(0)]], device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = t.read(i).x; }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(msl) options:nil error:&err];
    id<MTLComputePipelineState> pput = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"put"] error:&err];
    id<MTLComputePipelineState> pget = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"get"] error:&err];
    CHECK(pput != nil && pget != nil, "buffer-texture compute kernels compiled");
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pput]; [ce setTexture:uview atIndex:0];
    [ce dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "buffer-texture image write completed");
    uint32_t want[16]; for (uint32_t i = 0; i < 16; i++) want[i] = 0x11223300u + i;
    CHECK(!memcmp((const uint8_t *)mem + 256, want, sizeof want), "buffer-texture GPU write is visible in the guest pool map");
    for (int j = 0; j < 2; j++) {
        id<MTLTexture> rtex = [tex_mtl(j ? &tghost : &tb) newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
        id<MTLBuffer> out = [g_dev newBufferWithLength:sizeof want options:MTLResourceStorageModeShared];
        cb = q_cb(); ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pget]; [ce setTexture:rtex atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !memcmp(out.contents, want, sizeof want), "shared buffer-texture serial reads every GPU-written element after upgrade");
    }
    if (!getenv("MTL_DEBUG_LAYER")) {   // Metal API validation rejects creating the unaligned base view that tbuf_shadow wraps at bind time
        uint32_t *words = (uint32_t *)((uint8_t *)mem + 516);
        for (uint32_t i = 0; i < 16; i++) words[i] = 0x55667700u + i;
        mtl_sub_begin();
        tex_t ua = {.magic=MAGIC('T','E','X',0), .serial=99701, .pool=&pool, .off=516, .mem=(uint8_t *)words};
        ua.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_BUFFER, .format=NVN_FORMAT_R32UI, .width=16, .height=1, .depth=1, .levels=1, .samples=1};
        nvn_tex_layout(&ua.d, &ua.L); tex_t ub = ua, uc = ua; ub.serial++; uc.serial += 2;
        mtl_tex_init(&ua); mtl_tex_init(&ub);
        id<MTLTexture> ubase = tex_mtl(&ua);
        CHECK(ubase && tex_mtl(&ub) == ubase, "unaligned buffer textures share their base view");
        uint64_t copies0 = g_tbs_copies, rid1 = 0, rid2 = 0;
        id<MTLTexture> s1 = tbuf_shadow(ubase, &rid1), s2 = tbuf_shadow(tex_mtl(&ub), &rid2);
        CHECK(s1 && s1 != ubase && s1 == s2 && rid1 == rid2 && g_tbs_copies == copies0 + 1, "shared unaligned buffer textures reuse one tbuf_shadow copy per submission");
        id<MTLBuffer> out = [g_dev newBufferWithLength:sizeof want options:MTLResourceStorageModeShared];
        cb = q_cb(); ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pget]; [ce setTexture:s1 atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !memcmp(out.contents, words, sizeof want), "tbuf_shadow reads the unaligned elements in the first submission");
        mtl_sub_commit(); mtl_drain();
        for (uint32_t i = 0; i < 16; i++) words[i] = 0x99aabb00u + i;
        mtl_sub_begin();
        mtl_tex_init(&uc);
        CHECK(tex_mtl(&uc) == ubase, "new unaligned descriptor in the next submission shares the base view");
        uint64_t rid3 = 0;
        id<MTLTexture> s3 = tbuf_shadow(tex_mtl(&uc), &rid3);
        CHECK(s3 && s3 != s1 && g_tbs_copies == copies0 + 2, "tbuf_shadow refreshes the shared buffer texture in the next submission");
        cb = q_cb(); ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pget]; [ce setTexture:s3 atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !memcmp(out.contents, words, sizeof want), "tbuf_shadow reads the updated CPU bytes in the next submission");
        mtl_sub_commit(); mtl_drain();
    }
    mtl_pool_del(mem); mtl_alias_drop_pool(&pool); free(mem);
    printf("texture upgrade aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
