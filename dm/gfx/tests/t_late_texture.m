// A sampled uncompressed tile is filled after its first GPU use, without an NVN flush.
// A real render target sharing the same dirty host page must retain its GPU contents.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static void finish(void) { CHECK("submission completes",mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK("GPU has no error",!mtl_last_commit_error()); }
static uint16_t pixel(id<MTLTexture> t) {
    id<MTLBuffer> out=[g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
    CHECK("readback buffer allocated",out!=nil); if (!out) return 0;
    id<MTLCommandBuffer> cb=q_cb(); id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
             toBuffer:out destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:128];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK("readback completes",cb.status==MTLCommandBufferStatusCompleted);
    return *(uint16_t *)out.contents;
}
int main(void) { @autoreleasepool {
    if (!DM_GAME_TEX_DIRTY_UPLOAD) { puts("t_late_texture: requires a tex_dirty_upload profile; skipped"); return 0; }
    void crash_install(void); crash_install(); mtl_init();
    void *mem=NULL; if (posix_memalign(&mem,getpagesize(),65536)) abort(); memset(mem,0,65536);
    pool_builder_t pb={.mem=mem,.size=65536,.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED}; NVNmemoryPool pool;
    h_nvnMemoryPoolInitialize(&pool,(const NVNmemoryPoolBuilder *)&pb);
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetSize2D(&tb,8,8); h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_R16);
    NVNtexture sampled={0}, target={0}, alias={0};
    h_nvnTextureBuilderSetStorage(&tb,&pool,0); h_nvnTextureInitialize(&sampled,&tb);
    h_nvnTextureBuilderSetStorage(&tb,&pool,4096); h_nvnTextureInitialize(&target,&tb); h_nvnTextureInitialize(&alias,&tb);
    tex_t *s=(tex_t *)&sampled, *rt=(tex_t *)&target, *a=(tex_t *)&alias;
    mtl_sub_begin(); id<MTLTexture> first=tex_mtl(s), rendered=tex_mtl(rt);
    CHECK("uncompressed sampled texture has conservative render-target usage",first.usage & MTLTextureUsageRenderTarget);
    CHECK("alias shares render target storage",tex_mtl(a)==rendered);
    p_clear_tex clear={.tex=rt,.r={0,0,0,8,8,1},.mask=15}; clear.c.f[0]=0.5f; clear_tex(&clear); finish();
    CHECK("clear reached the GPU",pixel(rendered)==32768);
    CHECK("a possible render target is not GPU-owned",!mtl_tex_gpu_owned(s->serial));
    CHECK("a cleared target is GPU-owned",mtl_tex_gpu_owned(rt->serial));
    CHECK("its alias shares GPU ownership",mtl_tex_gpu_owned(a->serial));
    // The CPU's tile fill also dirties the target's page; its guest bytes still contain zero.
    uint16_t data[64]; for (int i=0;i<64;i++) data[i]=12345;
    CHECK("late tile fill swizzles",!nvn_swizzle(&s->d,0,0,data,16,s->mem));
    mtl_sub_begin(); nvn_ft_poll(); id<MTLTexture> uploaded=tex_mtl(s); id<MTLTexture> kept=tex_mtl(rt); finish();
    CHECK("late CPU fill is uploaded",pixel(uploaded)==12345);
    CHECK("rendered target is not overwritten by its stale guest storage",pixel(kept)==32768);
    CHECK("rendered alias is not overwritten",pixel(tex_mtl(a))==32768);
    mtl_sub_begin(); id<MTLTexture> upgraded=tex_upgrade_pfv(rt,kept,MTLPixelFormatR16Uint); finish();
    CHECK("format-view upgrade retains GPU ownership",mtl_tex_gpu_owned(rt->serial) && mtl_tex_gpu_owned(a->serial));
    CHECK("format-view upgrade preserves pixels",pixel(upgraded)==32768);
    // Streaming can fill the tile before TextureInitialize. No dirty generation after registration exists then.
    tex_t seed=*s; seed.mem=(uint8_t *)mem+16384;
    dm_guest_write_prepare(seed.mem,seed.L.size); CHECK("initial tile fill swizzles",!nvn_swizzle(&seed.d,0,0,data,16,seed.mem));
    NVNtexture initial={0}; h_nvnTextureBuilderSetStorage(&tb,&pool,16384); h_nvnTextureInitialize(&initial,&tb);
    mtl_sub_begin(); id<MTLTexture> initial_image=tex_mtl((tex_t *)&initial); finish();
    CHECK("CPU fill before texture initialization is uploaded",pixel(initial_image)==12345);
    // Streaming reuses this storage after Finalize, fills it, then registers a new descriptor.
    // Its registration stamp is newer than the fill; an old CPU-seeded alias must not win.
    h_nvnTextureFinalize(&initial);
    for (int i=0;i<64;i++) data[i]=23456;
    dm_guest_write_prepare(seed.mem,seed.L.size); CHECK("replacement tile fill swizzles",!nvn_swizzle(&seed.d,0,0,data,16,seed.mem));
    h_nvnTextureInitialize(&initial,&tb);
    mtl_sub_begin(); id<MTLTexture> replacement_image=tex_mtl((tex_t *)&initial); finish();
    CHECK("new descriptor reads replacement CPU tile",pixel(replacement_image)==23456);
    CHECK("previous in-flight tile keeps its own pixels",pixel(initial_image)==12345);
    // A CPU-seeded backing can subsequently become a real render target. Shared-page dirtiness must not restore its old CPU pixels.
    seed.mem=(uint8_t *)mem+32768;
    dm_guest_write_prepare(seed.mem,seed.L.size); CHECK("rendered seed fill swizzles",!nvn_swizzle(&seed.d,0,0,data,16,seed.mem));
    NVNtexture rendered_seed={0}, rendered_seed_alias={0}; h_nvnTextureBuilderSetStorage(&tb,&pool,32768); h_nvnTextureInitialize(&rendered_seed,&tb);
    mtl_sub_begin(); id<MTLTexture> seeded_target=tex_mtl((tex_t *)&rendered_seed);
    clear.tex=(tex_t *)&rendered_seed; clear_tex(&clear); finish();
    CHECK("CPU-seeded backing can be rendered",pixel(seeded_target)==32768);
    dm_guest_write_prepare((uint8_t *)mem+36864,1); *((uint8_t *)mem+36864)=1;
    h_nvnTextureInitialize(&rendered_seed_alias,&tb);
    mtl_sub_begin(); id<MTLTexture> seeded_alias=tex_mtl((tex_t *)&rendered_seed_alias); finish();
    CHECK("rendered seed wins over stale CPU storage",pixel(seeded_alias)==32768);
    // A new descriptor over an existing render target must not seed it again from stale CPU data.
    NVNtexture new_alias={0}; h_nvnTextureBuilderSetStorage(&tb,&pool,4096); h_nvnTextureInitialize(&new_alias,&tb);
    mtl_sub_begin(); id<MTLTexture> aliased_image=tex_mtl((tex_t *)&new_alias); finish();
    CHECK("new descriptor preserves rendered alias contents",pixel(aliased_image)==32768);
    h_nvnTextureFinalize(&rendered_seed_alias); h_nvnTextureFinalize(&rendered_seed);
    h_nvnTextureFinalize(&new_alias); h_nvnTextureFinalize(&initial);
    h_nvnTextureFinalize(&alias); h_nvnTextureFinalize(&target); h_nvnTextureFinalize(&sampled);
    mtl_drain(); h_nvnMemoryPoolFinalize(&pool); free(mem);
    printf("t_late_texture: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
