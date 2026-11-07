// A sampled uncompressed tile is filled after its first GPU use, without an NVN flush.
// A real render target sharing the same dirty host page must retain its GPU contents.
// Exercise dirty uploads independently of the selected game's default.
#include "game_profile.h"
#undef DM_GAME_TEX_FLUSH_UPLOAD
#define DM_GAME_TEX_FLUSH_UPLOAD 1
#undef DM_GAME_TEX_DIRTY_UPLOAD
#define DM_GAME_TEX_DIRTY_UPLOAD 1
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) {
    return NULL;
}
module_t *module_of(const void *p) {
    (void)p;
    return NULL;
}
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {
}
uint64_t now_ns(void) {
    return bp_now();
}
static int checks, failed;
#define CHECK(label, c)                                                                                                \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failed++;                                                                                                  \
            fprintf(stderr, "FAIL: %s\n", label);                                                                      \
        }                                                                                                              \
    } while (0)
static void finish(void) {
    CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("GPU has no error", !mtl_last_commit_error());
}
static id<MTLTexture> registered(int tid) {
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> texture = (__bridge id<MTLTexture>)tex_fast(tid);
    os_unfair_lock_unlock(&g_rmu);
    return texture;
}
static uint16_t pixel(id<MTLTexture> texture) {
    id<MTLBuffer> readback = [g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
    CHECK("readback buffer allocated", readback != nil);
    if (!readback) {
        return 0;
    }
    id<MTLCommandBuffer> command_buffer = q_cb();
    id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
    [encoder copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(8, 8, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:16
        destinationBytesPerImage:128];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    CHECK("readback completes", command_buffer.status == MTLCommandBufferStatusCompleted);
    return *(uint16_t *)readback.contents;
}
int main(void) {
    @autoreleasepool {
        if (!DM_GAME_TEX_DIRTY_UPLOAD) {
            puts("t_late_texture: requires a tex_dirty_upload profile; skipped");
            return 0;
        }
        void crash_install(void);
        crash_install();
        mtl_init();
        void *storage = NULL;
        if (posix_memalign(&storage, getpagesize(), 65536)) {
            abort();
        }
        memset(storage, 0, 65536);
        pool_builder_t pool_builder = {.mem = storage,
                                       .size = 65536,
                                       .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
        NVNmemoryPool pool;
        h_nvnMemoryPoolInitialize(&pool, (const NVNmemoryPoolBuilder *)&pool_builder);
        NVNtextureBuilder texture_builder;
        h_nvnTextureBuilderSetDefaults(&texture_builder);
        h_nvnTextureBuilderSetSize2D(&texture_builder, 8, 8);
        h_nvnTextureBuilderSetFormat(&texture_builder, NVN_FORMAT_R16);
        NVNtexture sampled = {0}, target = {0}, alias = {0};
        h_nvnTextureBuilderSetStorage(&texture_builder, &pool, 0);
        h_nvnTextureInitialize(&sampled, &texture_builder);
        h_nvnTextureBuilderSetStorage(&texture_builder, &pool, 4096);
        h_nvnTextureInitialize(&target, &texture_builder);
        h_nvnTextureInitialize(&alias, &texture_builder);
        tex_t *sampled_desc = (tex_t *)&sampled, *target_desc = (tex_t *)&target, *alias_desc = (tex_t *)&alias;
        mtl_sub_begin();
        id<MTLTexture> first = tex_mtl(sampled_desc), rendered = tex_mtl(target_desc);
        CHECK("uncompressed sampled texture has conservative render-target usage",
              first.usage & MTLTextureUsageRenderTarget);
        CHECK("alias shares render target storage", tex_mtl(alias_desc) == rendered);
        p_clear_tex clear = {.tex = target_desc, .r = {0, 0, 0, 8, 8, 1}, .mask = 15};
        clear.c.f[0] = 0.5f;
        mtl_tex_gpu_written(clear.tex); // the recording API normally publishes this storage write
        clear_tex(&clear);
        finish();
        CHECK("clear reached the GPU", pixel(rendered) == 32768);
        CHECK("a possible render target is not GPU-owned", !mtl_tex_gpu_owned(sampled_desc->serial));
        CHECK("a cleared target is GPU-owned", mtl_tex_gpu_owned(target_desc->serial));
        CHECK("its alias shares GPU ownership", mtl_tex_gpu_owned(alias_desc->serial));
        // The CPU's tile fill also dirties the target's page; its guest bytes still contain zero.
        uint16_t data[64];
        for (int i = 0; i < 64; i++) {
            data[i] = 12345;
        }
        CHECK("late tile fill swizzles", !nvn_swizzle(&sampled_desc->d, 0, 0, data, 16, sampled_desc->mem));
        mtl_sub_begin();
        nvn_ft_poll();
        id<MTLTexture> uploaded = tex_mtl(sampled_desc);
        id<MTLTexture> kept = tex_mtl(target_desc);
        finish();
        CHECK("late CPU fill is uploaded", pixel(uploaded) == 12345);
        CHECK("rendered target is not overwritten by its stale guest storage", pixel(kept) == 32768);
        CHECK("rendered alias is not overwritten", pixel(tex_mtl(alias_desc)) == 32768);
        mtl_sub_begin();
        id<MTLTexture> upgraded = tex_upgrade_pfv(target_desc, kept, MTLPixelFormatR16Uint);
        finish();
        CHECK("format-view upgrade retains GPU ownership",
              mtl_tex_gpu_owned(target_desc->serial) && mtl_tex_gpu_owned(alias_desc->serial));
        CHECK("format-view upgrade preserves pixels", pixel(upgraded) == 32768);
        // Streaming can fill the tile before TextureInitialize. No dirty generation after registration exists then.
        tex_t seed = *sampled_desc;
        seed.mem = (uint8_t *)storage + 16384;
        dm_guest_write_prepare(seed.mem, seed.L.size);
        CHECK("initial tile fill swizzles", !nvn_swizzle(&seed.d, 0, 0, data, 16, seed.mem));
        NVNtexture initial = {0};
        h_nvnTextureBuilderSetStorage(&texture_builder, &pool, 16384);
        h_nvnTextureInitialize(&initial, &texture_builder);
        view_t mip = {.set_levels = 1, .base_level = 0, .nlevels = 1};
        mtl_reg_tex(701, (tex_t *)&initial, NULL);
        mtl_reg_tex(702, (tex_t *)&initial, &mip);
        mtl_sub_begin();
        id<MTLTexture> initial_image = registered(701), initial_view = registered(702);
        finish();
        CHECK("CPU fill before texture initialization is uploaded", pixel(initial_image) == 12345);
        CHECK("registered mip view samples the initial seed", pixel(initial_view) == 12345);
        // Do not poll: a cached descriptor must observe each CPU write at its next use, even when the bounded scan has not reached this tile.
        for (int pass = 0; pass < 2; pass++) {
            // Recording a write to another texture must not revoke this seed's CPU ownership across the whole pool.
            if (pass == 1) {
                mtl_tex_gpu_written(target_desc);
            }
            uint16_t value = (uint16_t)(13579 + pass);
            for (int i = 0; i < 64; i++) {
                data[i] = value;
            }
            dm_guest_write_prepare(seed.mem, seed.L.size);
            CHECK("live seed refill swizzles", !nvn_swizzle(&seed.d, 0, 0, data, 16, seed.mem));
            mtl_sub_begin();
            id<MTLTexture> current = registered(701), current_view = registered(702);
            finish();
            CHECK("cached texture observes CPU rewrite without poll", pixel(current) == value);
            CHECK("cached mip view observes CPU rewrite without poll", pixel(current_view) == value);
            // finish() completed the earlier GPU use: upstream may recycle its spare now.
            CHECK("current upload has completed", !mtl_last_commit_error());
        }
        // Streaming reuses this storage after Finalize, fills it, then registers a new descriptor.
        // Its registration stamp is newer than the fill; an old CPU-seeded alias must not win.
        h_nvnTextureFinalize(&initial);
        for (int i = 0; i < 64; i++) {
            data[i] = 23456;
        }
        dm_guest_write_prepare(seed.mem, seed.L.size);
        CHECK("replacement tile fill swizzles", !nvn_swizzle(&seed.d, 0, 0, data, 16, seed.mem));
        h_nvnTextureInitialize(&initial, &texture_builder);
        mtl_sub_begin();
        id<MTLTexture> replacement_image = tex_mtl((tex_t *)&initial);
        finish();
        CHECK("new descriptor reads replacement CPU tile", pixel(replacement_image) == 23456);
        // The earlier submissions have completed; their spare texture may be reused.
        // A CPU-seeded backing can subsequently become a real render target. Shared-page dirtiness must not restore its old CPU pixels.
        seed.mem = (uint8_t *)storage + 32768;
        dm_guest_write_prepare(seed.mem, seed.L.size);
        CHECK("rendered seed fill swizzles", !nvn_swizzle(&seed.d, 0, 0, data, 16, seed.mem));
        NVNtexture rendered_seed = {0}, rendered_seed_alias = {0};
        h_nvnTextureBuilderSetStorage(&texture_builder, &pool, 32768);
        h_nvnTextureInitialize(&rendered_seed, &texture_builder);
        mtl_reg_tex(703, (tex_t *)&rendered_seed, NULL);
        mtl_sub_begin();
        id<MTLTexture> seeded_target = registered(703);
        clear.tex = (tex_t *)&rendered_seed;
        mtl_tex_gpu_written(clear.tex); // the recording API normally publishes this storage write
        clear_tex(&clear);
        finish();
        CHECK("CPU-seeded backing can be rendered", pixel(tex_mtl((tex_t *)&rendered_seed)) == 32768);
        dm_guest_write_prepare((uint8_t *)storage + 36864, 1);
        *((uint8_t *)storage + 36864) = 1;
        h_nvnTextureInitialize(&rendered_seed_alias, &texture_builder);
        mtl_sub_begin();
        id<MTLTexture> seeded_alias = tex_mtl((tex_t *)&rendered_seed_alias);
        finish();
        CHECK("rendered seed wins over stale CPU storage", pixel(seeded_alias) == 32768);
        mtl_sub_begin();
        id<MTLTexture> cached_target = registered(703);
        finish();
        CHECK("cached rendered seed preserves GPU pixels after shared-page write", pixel(cached_target) == 32768);
        CHECK("GPU-written seed no longer checks CPU dirtiness on fast lookup", !g_treg[703].view_pkg);
        // A new descriptor over an existing render target must not seed it again from stale CPU data.
        NVNtexture new_alias = {0};
        h_nvnTextureBuilderSetStorage(&texture_builder, &pool, 4096);
        h_nvnTextureInitialize(&new_alias, &texture_builder);
        mtl_sub_begin();
        id<MTLTexture> aliased_image = tex_mtl((tex_t *)&new_alias);
        finish();
        CHECK("new descriptor preserves rendered alias contents", pixel(aliased_image) == 32768);
        mtl_reg_tex(701, NULL, NULL);
        mtl_reg_tex(702, NULL, NULL);
        mtl_reg_tex(703, NULL, NULL);
        h_nvnTextureFinalize(&rendered_seed_alias);
        h_nvnTextureFinalize(&rendered_seed);
        h_nvnTextureFinalize(&new_alias);
        h_nvnTextureFinalize(&initial);
        h_nvnTextureFinalize(&alias);
        h_nvnTextureFinalize(&target);
        h_nvnTextureFinalize(&sampled);
        mtl_drain();
        h_nvnMemoryPoolFinalize(&pool);
        free(storage);
        printf("t_late_texture: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
}
