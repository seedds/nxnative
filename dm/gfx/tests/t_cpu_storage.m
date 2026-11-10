// An uncompressed 2D texture the CPU wrote, over storage the GPU has not written (nvn_mtl.m tex_cpu_content): its storage is its content at first use, flushed or not, and a later CPU
// write is uploaded. Once the GPU writes its storage it becomes a GPU texture with the content it has (cpu_unc_drop), and CPU writes are no longer uploaded. Over storage the GPU already
// writes it stays a GPU texture, unless the CPU has rewritten that storage since. Mario Kart World's terrain patches: R16, RG8, RGBA8, R11G11B10F and RGBA16 in a CPU_CACHED pool, never
// flushed, were zeros and every patch a flat plane. Ocarina of Time's terrain height and blend maps lie in the pool its normal maps are drawn into: zeros, and holes in the ground.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no error", !mtl_last_commit_error()); }
enum { SIZE = 16, LARGE = 256, POOL = 1 << 20 };
static void fill(tex_t *t, void *mem, uint8_t v) {
    uint32_t w = t->d.width, h = t->d.height;
    uint8_t *linear = malloc((size_t)w * h * 4);
    for (uint32_t i = 0; i < w * h; i++) {
        linear[i * 4] = v;
        linear[i * 4 + 1] = v + 1;
        linear[i * 4 + 2] = v + 2;
        linear[i * 4 + 3] = 255;
    }

    CHECK("fixture swizzle succeeds", !nvn_swizzle(&t->d, 0, 0, linear, w * 4, mem));
    free(linear);
}
static uint8_t first_red(id<MTLTexture> image) {   // the red of texel (3, 5), read through a blit: the texture is private
    static id<MTLCommandQueue> queue; if (!queue) queue = [g_dev newCommandQueue];
    id<MTLBuffer> out = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (!image || !out) { fprintf(stderr, "nothing to read back\n"); return 0; }
    id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:image.parentTexture ?: image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(3, 5, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK("readback completes", cb.status == MTLCommandBufferStatusCompleted);
    return *(uint8_t *)out.contents;
}
static uint8_t stored_red(const tex_t *t) {   // the red of texel (3, 5) of level 0, as the storage holds it
    uint8_t *linear = malloc((size_t)t->d.width * t->d.height * 4);
    CHECK("fixture deswizzle succeeds", !nvn_deswizzle(&t->d, 0, 0, t->mem, linear, t->d.width * 4));
    uint8_t red = linear[(5 * t->d.width + 3) * 4];
    free(linear);
    return red;
}
static void gpu_clear(tex_t *t, float red) {   // a texture clear: recorded (the storage's span is stamped), replayed, completed
    mtl_tex_gpu_written(t);
    p_clear_tex c = {.tex = t, .r = {0, 0, 0, (int)t->d.width, (int)t->d.height, 1}, .mask = 15, .c.f = {red, 0, 0, 1}};
    mtl_sub_begin(); clear_tex(&c); finish();
}
static id<MTLTexture> resolve(tex_t *t) { mtl_sub_begin(); id<MTLTexture> image = tex_mtl(t); CHECK("texture resolves", image != nil); finish(); return image; }
typedef struct { void *mem; pool_t pool; tex_t t; } fixture;
static tex_t texture_at(fixture *f, uint64_t serial, uint64_t off, uint32_t size, int format, uint32_t tex_flags) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .pool = &f->pool, .off = off, .mem = (uint8_t *)f->mem + off};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = size, .height = size, .depth = 1, .levels = 1, .samples = 1, .flags = tex_flags};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static void setup(fixture *f, uint64_t serial, uint32_t pool_flags, int format, uint32_t tex_flags) {
    if (posix_memalign(&f->mem, getpagesize(), POOL)) {
        abort();
    }

    memset(f->mem, 0, POOL);
    f->pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = f->mem, .size = POOL, .flags = pool_flags};
    mtl_pool_add(f->mem, POOL);
    nvn_dirty_add_range(f->mem, POOL);
    f->t = texture_at(f, serial, 0, SIZE, format, tex_flags);
}
static void teardown(fixture *f) {
    mtl_tex_release(&f->t);
    mtl_alias_drop_pool(&f->pool);
    nvn_dirty_remove_range(f->mem);
    mtl_pool_del(f->mem);
    mtl_drain();
    free(f->mem);
}
static const uint32_t CACHED = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;

int main(void) {
    @autoreleasepool {
        mtl_init();
        fixture f;
        setup(&f, 7001, CACHED, NVN_FORMAT_RGBA8, 0);
        fill(&f.t, f.mem, 40);
        mtl_tex_init(&f.t);
        CHECK("at first use the storage is the content", first_red(resolve(&f.t)) == 40);
        dm_guest_write_prepare(f.mem, f.t.L.size); fill(&f.t, f.mem, 80);
        CHECK("a later CPU write is uploaded", first_red(resolve(&f.t)) == 80);
        mtl_tex_gpu_written(&f.t);
        CHECK("once the GPU writes its storage it keeps the content it has", first_red(resolve(&f.t)) == 80);
        CHECK("and is no longer CPU-written", !tex_cpu_written(f.t.serial));
        dm_guest_write_prepare(f.mem, f.t.L.size); fill(&f.t, f.mem, 120);
        CHECK("and a CPU write is no longer uploaded", first_red(resolve(&f.t)) == 80);
        teardown(&f);

        setup(&f, 7002, CACHED, NVN_FORMAT_RGBA8, 0);
        mtl_tex_gpu_written(&f.t);
        fill(&f.t, f.mem, 40);
        CHECK("over storage the GPU writes it is not taken from storage", !tex_cpu_content(&f.t));
        teardown(&f);

        setup(&f, 7004, CACHED, NVN_FORMAT_RGBA8, 0);
        tex_t target = texture_at(&f, 7005, 0x40000, SIZE, NVN_FORMAT_RGBA8, 0);
        mtl_tex_gpu_written(&target);
        fill(&f.t, f.mem, 40);
        CHECK("beside storage the GPU writes, in the same pool, it is taken from storage", tex_cpu_content(&f.t));
        teardown(&f);

        setup(&f, 7006, CACHED, NVN_FORMAT_RGBA8, 0);
        tex_t large = texture_at(&f, 7007, 0x40000, LARGE, NVN_FORMAT_RGBA8, 0);
        mtl_tex_gpu_written(&large);
        dm_guest_write_prepare(large.mem, large.L.size); fill(&large, large.mem, 40);
        CHECK("over storage the GPU wrote, then the CPU rewrote, it is taken from storage", tex_cpu_content(&large));
        mtl_tex_gpu_written(&large);
        mtl_sub_begin(); cpu_unc_drop(); finish();
        CHECK("and the GPU's next write there makes it a GPU texture", !tex_cpu_written(large.serial));
        CHECK("which then is not taken from storage again", !tex_cpu_content(&large));
        mtl_tex_release(&large);
        teardown(&f);

        setup(&f, 7008, CACHED, NVN_FORMAT_RGBA8, 0);
        large = texture_at(&f, 7009, 0x40000, LARGE, NVN_FORMAT_RGBA8, 0);
        tex_t upper = texture_at(&f, 7020, 0x40000 + large.L.size / 2, LARGE / 2, NVN_FORMAT_RGBA8, 0);
        tex_t lower = texture_at(&f, 7021, 0x40000, LARGE / 2, NVN_FORMAT_RGBA8, 0);
        mtl_tex_gpu_written(&large);
        dm_guest_write_prepare(upper.mem, upper.L.size); fill(&upper, upper.mem, 40);
        mtl_tex_gpu_written(&lower);
        CHECK("a GPU write over another part of the storage leaves the CPU's rewrite seen", tex_cpu_content(&upper));
        CHECK("and the part it wrote is the GPU's", gws_owned((uintptr_t)lower.mem, (uintptr_t)lower.mem + lower.L.size));
        mtl_tex_release(&upper);
        teardown(&f);

        // A span's sliver inside a texture (gws_sliver): less than a page of it, not covering it. Ocarina of Time's terrain tiles, GitHub issue #1566
        setup(&f, 7040, CACHED, NVN_FORMAT_RGBA8, 0);
        tex_t drawn = texture_at(&f, 7041, 0x40000, 68, NVN_FORMAT_RGBA8, 0);   // 40,960 bytes: it ends 8 KB into its third page
        uintptr_t pg = (uintptr_t)getpagesize(), drawn_end = (uintptr_t)drawn.mem + drawn.L.size, shared = drawn_end & ~(pg - 1);
        CHECK("fixture: the drawn texture ends inside a page", drawn_end != shared && drawn_end - shared < pg);
        tex_t filled = texture_at(&f, 7042, shared - (uintptr_t)f.mem, 68, NVN_FORMAT_RGBA8, 0);   // begins in that page: the drawn texture's span reaches 8 KB into it
        mtl_tex_gpu_written(&drawn);
        dm_guest_write_prepare(filled.mem, filled.L.size); fill(&filled, filled.mem, 40);
        CHECK("a neighbour's span reaching less than a page into a texture does not make it the GPU's", !gws_owned((uintptr_t)filled.mem, (uintptr_t)filled.mem + filled.L.size));
        CHECK("and the texture is taken from its storage", tex_cpu_content(&filled));
        CHECK("while the drawn texture stays the GPU's", gws_owned((uintptr_t)drawn.mem, drawn_end));
        mtl_tex_release(&filled); mtl_tex_release(&drawn);
        teardown(&f);

        setup(&f, 7043, CACHED, NVN_FORMAT_RGBA8, 0);   // a remnant of an old span between two newer ones (gws_set keeps the parts outside a new span), inside a texture the CPU then filled
        large = texture_at(&f, 7044, 0x40000, LARGE, NVN_FORMAT_RGBA8, 0);
        uintptr_t lo = (uintptr_t)large.mem, hi = lo + large.L.size, cut = lo + large.L.size / 2;
        mtl_tex_gpu_written(&large);
        pthread_mutex_lock(&g_gws_mu); gws_set(lo, cut); gws_set(cut + 2560, hi); pthread_mutex_unlock(&g_gws_mu);
        CHECK("fixture: three spans, the middle one 2,560 bytes", g_ngws >= 3 && g_gws[gws_find(cut)].lo == cut && g_gws[gws_find(cut)].hi == cut + 2560);
        dm_guest_write_prepare(large.mem, large.L.size); fill(&large, large.mem, 40);
        CHECK("a sub-page remnant of an old span inside a texture the CPU rewrote does not keep it the GPU's", !gws_owned(lo, hi));
        CHECK("and the texture is taken from its storage", tex_cpu_content(&large));
        mtl_tex_release(&large);
        teardown(&f);

        setup(&f, 7045, CACHED, NVN_FORMAT_RGBA8, 0);   // a texture smaller than a page over storage the GPU wrote stays the GPU's: the span covers it, and a neighbour's CPU write in its page is not its
        tex_t beside = texture_at(&f, 7046, f.t.L.size, SIZE, NVN_FORMAT_RGBA8, 0);
        mtl_tex_gpu_written(&f.t);
        dm_guest_write_prepare(beside.mem, beside.L.size); fill(&beside, beside.mem, 40);
        CHECK("a texture smaller than a page over storage the GPU wrote stays the GPU's", gws_owned((uintptr_t)f.t.mem, (uintptr_t)f.t.mem + f.t.L.size));
        CHECK("though a neighbour in its page is CPU-written", !tex_cpu_content(&f.t) && tex_cpu_content(&beside));
        mtl_tex_release(&beside);
        teardown(&f);

        // nvn_ft_poll's question of a registered texture the CPU wrote after its first use (mtl_tex_cpu_rewritten)
        setup(&f, 7030, CACHED, NVN_FORMAT_RGBA8, 0);
        large = texture_at(&f, 7031, 0x40000, LARGE, NVN_FORMAT_RGBA8, 0);
        dm_guest_write_prepare(large.mem, large.L.size); fill(&large, large.mem, 40);
        CHECK("a rewrite of storage the GPU never wrote is uploaded", mtl_tex_cpu_rewritten(&large) == MTL_REWRITE_UPLOADED && tex_cpu_written(large.serial));
        mtl_tex_gpu_written(&large);
        mtl_sub_begin(); cpu_unc_drop(); finish();
        CHECK("and the GPU's next write there makes it a GPU texture again", !tex_cpu_written(large.serial));
        dm_guest_write_prepare(large.mem, large.L.size); fill(&large, large.mem, 80);
        mtl_tex_gpu_written(&large);
        CHECK("a rewrite the GPU wrote over afterwards is the GPU's until the CPU rewrites it", mtl_tex_cpu_rewritten(&large) == MTL_REWRITE_GPU_NOW && !tex_cpu_written(large.serial));
        dm_guest_write_prepare(large.mem, large.L.size); fill(&large, large.mem, 120);
        CHECK("and the CPU's next rewrite is uploaded", mtl_tex_cpu_rewritten(&large) == MTL_REWRITE_UPLOADED && tex_cpu_written(large.serial));
        mtl_tex_release(&large);
        teardown(&f);

        setup(&f, 7032, CACHED, NVN_FORMAT_RGBA8, 0);   // a block-compressed texture: a GPU copy into it while registered lands in its storage too (tex_storage_dst)
        tex_t bc = texture_at(&f, 7033, 0x40000, LARGE, NVN_FORMAT_RGBA_DXT1, 0);
        dm_guest_write_prepare(bc.mem, bc.L.size); memset(bc.mem, 0x5a, bc.L.size);
        mtl_tex_gpu_written(&bc);
        CHECK("a block-compressed rewrite is uploaded though a GPU copy into it followed", mtl_tex_cpu_rewritten(&bc) == MTL_REWRITE_UPLOADED && tex_cpu_written(bc.serial));
        mtl_tex_release(&bc);
        teardown(&f);

        setup(&f, 7034, CACHED, NVN_FORMAT_RGBA8, 0);   // a second texture object over storage the GPU wrote through the first (the spans are by address)
        large = texture_at(&f, 7035, 0x40000, LARGE, NVN_FORMAT_RGBA8, 0);
        tex_t again = texture_at(&f, 7036, 0x40000, LARGE, NVN_FORMAT_RGBA8, 0);
        dm_guest_write_prepare(large.mem, large.L.size); fill(&large, large.mem, 40);
        mtl_tex_gpu_written(&large);
        CHECK("another texture over storage the GPU wrote after the CPU is the GPU's too", mtl_tex_cpu_rewritten(&again) == MTL_REWRITE_GPU_NOW);
        mtl_tex_release(&again); mtl_tex_release(&large);
        teardown(&f);

        setup(&f, 7022, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, NVN_FORMAT_RGBA8, 0);
        size_t spans = g_ngws;
        mtl_tex_gpu_written(&f.t);
        CHECK("storage the CPU cannot write gets no span", g_ngws == spans);
        teardown(&f);

        setup(&f, 7003, CACHED, NVN_FORMAT_RGBA8, 0);
        CHECK("storage of zeros is not taken", !tex_cpu_content(&f.t));
        teardown(&f);

        static const struct { const char *label; int format; uint32_t flags; } left_out[] = {
            {"a display texture stays a GPU texture", NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_DISPLAY},
            {"a video-decode texture stays a GPU texture", NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_VIDEO_DECODE},
            {"a shader image stays a GPU texture", NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_IMAGE},
            {"a compressible texture stays a GPU texture", NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_COMPRESSIBLE},
            {"a depth texture stays a GPU texture", NVN_FORMAT_DEPTH32F, 0},
        };
        for (unsigned i = 0; i < sizeof left_out / sizeof *left_out; i++) {
            setup(&f, 7010 + i, CACHED, left_out[i].format, left_out[i].flags);
            memset(f.mem, 0x55, f.t.L.size);
            CHECK(left_out[i].label, !tex_cpu_content(&f.t));
            teardown(&f);
        }

        // What the GPU writes into a GPU texture in storage the CPU can read reaches that storage, written there on the GPU in the same submission (gsb_store): Ocarina of Time moves
        // the terrain normal maps the GPU drew with memcpy and re-creates them at the new address
        atomic_store(&g_cur_frame, 100);
        setup(&f, 7060, CACHED, NVN_FORMAT_RGBA8, 0);
        large = texture_at(&f, 7061, 0x40000, 128, NVN_FORMAT_RGBA8, 0);
        uintptr_t llo = (uintptr_t)large.mem, lhi = llo + large.L.size;
        uint64_t stores = g_gsb_stores;
        gpu_clear(&large, 0.2f);
        CHECK("a clear of a GPU texture reaches its storage", stored_red(&large) == 51 && g_gsb_stores == stores + 1);
        CHECK("which stays the GPU's: the GPU's write is not the CPU's", gws_owned(llo, lhi) && !tex_cpu_written(large.serial) && mtl_tex_cpu_rewritten(&large) == MTL_REWRITE_GPU_NOW);
        tex_t moved = texture_at(&f, 7062, 0x60000, 128, NVN_FORMAT_RGBA8, 0);
        dm_guest_write_prepare(moved.mem, moved.L.size); memcpy(moved.mem, large.mem, large.L.size);
        CHECK("its storage moved with memcpy: a texture made there holds the clear", first_red(resolve(&moved)) == 51 && tex_cpu_written(moved.serial));
        // written in consecutive frames: every write is in the storage once its submission completes, and a copy right after the second finds the second (no quiet period)
        atomic_store(&g_cur_frame, 101); gpu_clear(&large, 0.4f);
        CHECK("a write the frame after another is in the storage at once", stored_red(&large) == 102);
        atomic_store(&g_cur_frame, 102); gpu_clear(&large, 0.6f);
        CHECK("and so is the next one", stored_red(&large) == 153 && g_gsb_stores == stores + 3 && gws_owned(llo, lhi));
        tex_t moved2 = texture_at(&f, 7068, 0x80000, 128, NVN_FORMAT_RGBA8, 0);
        dm_guest_write_prepare(moved2.mem, moved2.L.size); memcpy(moved2.mem, large.mem, large.L.size);
        CHECK("a memcpy right after the second write gives a texture with the second write", first_red(resolve(&moved2)) == 153);
        // a region: only its texels are written
        p_clear_tex part = {.tex = &large, .r = {8, 8, 0, 40, 24, 1}, .mask = 15, .c.f = {0.8f, 0, 0, 1}};
        mtl_tex_gpu_written(&large); mtl_sub_begin(); clear_tex(&part); finish();
        { uint8_t *lin = malloc(128 * 128 * 4); int bad = nvn_deswizzle(&large.d, 0, 0, large.mem, lin, 128 * 4);
          for (int y = 0; y < 128 && !bad; y++) for (int x = 0; x < 128; x++) bad += lin[(y * 128 + x) * 4] != ((x >= 8 && x < 48 && y >= 8 && y < 32) ? 204 : 153);
          CHECK("a cleared region reaches the storage, and only its texels", !bad); free(lin); }
        // another writer over the storage after the GPU texture's write: nothing of the GPU texture lands later over it, however many frames pass
        tex_t first = texture_at(&f, 7064, 0xa0000, 128, NVN_FORMAT_RGBA8, 0), image = texture_at(&f, 7065, 0xa0000, 128, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_IMAGE);
        atomic_store(&g_cur_frame, 110); gpu_clear(&first, 0.2f);
        atomic_store(&g_cur_frame, 111); gpu_clear(&first, 0.4f);
        CHECK("a GPU texture drawn in two frames in a row holds the second in its storage", stored_red(&first) == 102);
        mtl_tex_release(&first);
        dm_guest_write_prepare(first.mem, first.L.size); fill(&first, first.mem, 77);   // the game reuses the storage
        for (int k = 112; k < 120; k++) { atomic_store(&g_cur_frame, (uint64_t)k); mtl_sub_begin(); finish(); }
        tex_t after = texture_at(&f, 7066, 0xa0000, 128, NVN_FORMAT_RGBA8, 0);
        CHECK("nothing of the older texture lands over the CPU's bytes, however many frames pass: a texture made there holds them", stored_red(&first) == 77 && first_red(resolve(&after)) == 77);
        mtl_tex_release(&after);
        uint64_t before = g_gsb_stores;
        gpu_clear(&image, 0.6f);   // a texture the rule leaves out (an image) drawn over it: its draw stays in its host texture
        CHECK("an image's clear is not stored", g_gsb_stores == before && stored_red(&first) == 77);
        mtl_tex_release(&image);
        // a texture smaller than a page
        mtl_tex_init(&f.t);
        gpu_clear(&f.t, 0.2f);
        CHECK("a texture smaller than a page is written into its storage", stored_red(&f.t) == 51);
        // Ocarina of Time's normal tiles: 260x260 RG8, 9 levels, each drawn on its own; its rows (520 bytes) are no multiple of 16, its small levels have lower block heights
        fixture g; setup(&g, 7069, CACHED, NVN_FORMAT_RGBA8, 0);
        tex_t tile = texture_at(&g, 7067, 0x40000, 260, NVN_FORMAT_RG8, 0); tile.d.levels = 9; nvn_tex_layout(&tile.d, &tile.L);
        CHECK("the tile fits in the pool", tile.off + tile.L.size <= POOL);
        mtl_tex_gpu_written(&tile); mtl_sub_begin();
        for (unsigned lv = 0; lv < 9; lv++) {
            p_clear_tex tc = {.tex = &tile, .has_view = 1, .r = {0, 0, 0, (int)MAX(260u >> lv, 1u), (int)MAX(260u >> lv, 1u), 1}, .mask = 15, .c.f = {(float)(lv + 1) / 51.f, 0.2f, 0, 1}};
            tc.view.set_levels = 1; tc.view.base_level = lv; clear_tex(&tc);
        }
        finish();
        { uint8_t *lin = malloc(260 * 260 * 2); int bad = 0;
          for (unsigned lv = 0; lv < 9; lv++) { unsigned w = MAX(260u >> lv, 1u);
              memset(lin, 0xEE, 260 * 260 * 2); bad += nvn_deswizzle(&tile.d, lv, 0, tile.mem, lin, w * 2) ? 1000 : 0;
              for (unsigned i = 0; i < w * w; i++) bad += lin[i * 2] != (lv + 1) * 5 || lin[i * 2 + 1] != 51; }
          CHECK("every level of a 260x260 RG8 tile reaches its storage, every texel", !bad); free(lin); }
        CHECK("none of it through the tables of textures uploaded from their storage", !g_nabs && !g_nabsp && !g_mrb_n && !g_mrb_untracked);
        mtl_tex_release(&tile); teardown(&g); mtl_tex_release(&moved2); mtl_tex_release(&moved); mtl_tex_release(&large);
        teardown(&f);
    }
    printf("t_cpu_storage: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
