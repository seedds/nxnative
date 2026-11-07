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
    // A retired descriptor whose storage is taken over by another texture must not re-create a texture or force an un-noted pool rebuild.
    mtl_sub_begin();
    uint8_t mem_buf[65536];
    pool_t tpool = {.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem=mem_buf, .size=sizeof mem_buf};
    mtl_pool_add(mem_buf, sizeof mem_buf);
    tex_t r1 = a, r2 = a;
    r2.d.width = 2; r2.d.height = 2; nvn_tex_layout(&r2.d, &r2.L);
    r1.pool = r2.pool = &tpool; r1.off = r2.off = 0; r1.mem = r2.mem = mem_buf;
    r1.serial = 99601; r2.serial = 99602;
    mtl_tex_init(&r1); mtl_tex_init(&r2);
    id<MTLTexture> r1_tex = tex_mtl(&r1);
    CHECK(r1_tex != nil, "initial pool-backed texture created");
    mtl_reg_tex(951, &r1, NULL); mtl_reg_tex(952, &r2, NULL);
    mtl_tex_release(&r1); mtl_tex_release(&r2);
    os_unfair_lock_lock(&g_rmu);
    g_seen_done[951 >> 5] |= 1u << (951 & 31);
    CHECK(tex_by_id(951) == r1_tex && g_treg[951].view != NULL && !g_treg[952].view, "retired descriptor 951 holds a transient view while 952 has none");
    __unsafe_unretained id<MTLBuffer> pbuf = nil;
    uint64_t gpu0 = pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &pbuf);
    CHECK(gpu0 != 0 && pbuf != nil && !tex_get(r2.serial), "initial bindless pool table built while unread retired descriptor 952 stays un-ghosted");
    os_unfair_lock_unlock(&g_rmu);
    treg_finish_submission();
    CHECK(!g_treg[951].view, "retired descriptor drops its transient view at submission end");
    g_sub_serial++;
    g_fence += ALIAS_IDLE + 2;
    tex_t rep = r1; rep.serial = 99603; rep.d.width = 8; rep.d.height = 8; nvn_tex_layout(&rep.d, &rep.L);
    mtl_tex_init(&rep);
    uint64_t nopatch_before = g_stat_pool_nopatch[3];
    id<MTLTexture> rep_tex = tex_mtl(&rep);
    CHECK(rep_tex != nil && rep_tex != r1_tex && !alias_has(r1_tex) && alias_has(rep_tex), "new texture takes over storage from the retired one");
    os_unfair_lock_lock(&g_rmu);
    uint64_t gpu1 = pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &pbuf);
    CHECK(gpu1 != 0 && g_stat_pool_nopatch[3] == nopatch_before, "storage takeover patches the bindless pool without an un-noted rebuild");
    CHECK(tex_by_id(952) == nil && g_treg[952].gone && alias_has(rep_tex), "un-ghosted retired descriptor over taken storage marks gone and does not evict the replacement");
    os_unfair_lock_unlock(&g_rmu);
    mtl_tex_release(&rep); mtl_alias_drop_pool(&tpool); mtl_pool_del(mem_buf);
    // A dynamic-resolution COMPRESSIBLE family in a VIRTUAL pool keeps its placement heap across Finalize and resolves retired aliases after ghost eviction.
    mtl_pool_add(mem_buf, sizeof mem_buf);
    pool_t vpool = {.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=mem_buf, .size=sizeof mem_buf};
    tex_t v0 = a, v1 = a, v0_dup = a;
    v0.pool = v1.pool = v0_dup.pool = &vpool; v0.mem = v1.mem = v0_dup.mem = mem_buf;
    v0.d.flags = v1.d.flags = v0_dup.d.flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE;
    v0.d.width = v0_dup.d.width = 16; v0.d.height = v0_dup.d.height = 16; nvn_tex_layout(&v0.d, &v0.L); v0_dup.L = v0.L;
    v1.d.width = 32; v1.d.height = 32; nvn_tex_layout(&v1.d, &v1.L);
    v0.serial = 99701; v1.serial = 99702; v0_dup.serial = 99703;
    mtl_tex_init(&v0); mtl_tex_init(&v1); mtl_tex_init(&v0_dup);
    mtl_reg_tex(953, &v0_dup, NULL);
    os_unfair_lock_lock(&g_rmu); g_seen_done[953 >> 5] |= 1u << (953 & 31); os_unfair_lock_unlock(&g_rmu);
    mtl_tex_release(&v0); mtl_tex_release(&v1); mtl_tex_release(&v0_dup);
    id<MTLTexture> vt0 = tex_mtl(&v0), vt1 = tex_mtl(&v1);
    CHECK(vt0.heap != nil && vt0.heap == vt1.heap, "VIRTUAL-pool dynamic-resolution family shares one placement heap across Finalize");
    CHECK(!pool_unwritten_target(&v0_dup), "a compressible descriptor whose storage is in g_alias is not treated as an unwritten target");
    os_unfair_lock_lock(&g_rmu);
    CHECK(tex_by_id(953) == vt0, "retired descriptor resolves through g_alias on first submission");
    os_unfair_lock_unlock(&g_rmu);
    treg_finish_submission(); g_sub_serial++;
    os_unfair_lock_lock(&g_rmu);
    CHECK(tex_by_id(953) == vt0, "retired descriptor continues to resolve through g_alias on subsequent submissions");
    os_unfair_lock_unlock(&g_rmu);
    // In a large pool (g_ntreg > 2048), retired descriptors resolve in pool_gpu and keep their cached view across submissions without rebuilding the table whole or thrashing g_ghost.
    tex_t v_unseen = v0; v_unseen.serial = 99704; mtl_tex_init(&v_unseen);
    mtl_reg_tex(2100, &v_unseen, NULL); mtl_tex_release(&v_unseen);
    treg_finish_submission(); g_sub_serial++;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &pbuf);
    uint64_t builds_after_first = g_stat_pool_builds, ghost_puts_after_first = g_ghost_puts;
    CHECK(g_treg[2100].view != NULL && tex_by_id(2100) == vt0, "retired descriptor in a large pool resolves in pool_gpu");
    os_unfair_lock_unlock(&g_rmu);
    treg_finish_submission(); g_sub_serial++;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &pbuf);
    CHECK(g_treg[2100].view != NULL && g_stat_pool_builds == builds_after_first && g_ghost_puts == ghost_puts_after_first, "subsequent submission keeps the cached view and pool table without a full rebuild or ghost thrash");
    os_unfair_lock_unlock(&g_rmu);
    mtl_alias_drop_pool(&vpool); mtl_pool_del(mem_buf);
    mtl_sub_commit(); mtl_drain();
    printf("texture upgrade aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
