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
    }
    printf("t_cpu_storage: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
