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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

// Registering unrelated memory must not rebuild a texture descriptor table.
int main(void) { @autoreleasepool {
    mtl_init(); size_t bytes=65536;
    uint8_t *mem=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    uint8_t *other=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if (mem==MAP_FAILED || other==MAP_FAILED) abort();
    pool_builder_t pb={.mem=mem,.size=bytes,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    NVNmemoryPool pool={0}, unrelated={0};
    CHECK(h_nvnMemoryPoolInitialize(&pool,(NVNmemoryPoolBuilder *)&pb), "texture pool initialized");
    NVNtextureBuilder tb; NVNtexture tex={0};
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb,64);h_nvnTextureBuilderSetHeight(&tb,64);
    h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb,&pool,0);
    CHECK(h_nvnTextureInitialize(&tex,&tb), "texture initialized");
    mtl_sub_begin(); CHECK(tex_mtl((tex_t *)&tex)!=nil,"texture backing created");
    mtl_reg_tex(256,(tex_t *)&tex,NULL);
    os_unfair_lock_lock(&g_rmu);
    __unsafe_unretained id<MTLBuffer> table=nil;
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    uint64_t id0=((uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD))[256];
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"initial submission completed");
    pb.mem=other;CHECK(h_nvnMemoryPoolInitialize(&unrelated,(NVNmemoryPoolBuilder *)&pb),"unrelated pool initialized");
    for (int stage=0;stage<2;stage++) {
        if (stage) h_nvnMemoryPoolFinalize(&unrelated);
        mtl_sub_begin();uint64_t builds=g_stat_pool_builds;
        os_unfair_lock_lock(&g_rmu);
        pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
        CHECK(((uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD))[256]==id0,"unrelated pool does not alter texture binding");
        CHECK(g_stat_pool_builds==builds,"unrelated pool change does not rebuild bindless table");
        os_unfair_lock_unlock(&g_rmu);
        CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"submission completed");
    }
    // Finalizing a different pool with actual shared texture storage must only
    // retire its own descriptor. The original texture binding stays usable.
    CHECK(h_nvnMemoryPoolInitialize(&unrelated,(NVNmemoryPoolBuilder *)&pb),"textured unrelated pool initialized");
    NVNtexture other_tex={0}; h_nvnTextureBuilderSetStorage(&tb,&unrelated,0);
    CHECK(h_nvnTextureInitialize(&other_tex,&tb),"other texture initialized");
    mtl_sub_begin(); CHECK(tex_mtl((tex_t *)&other_tex)!=nil,"other backing created");
    mtl_reg_tex(257,(tex_t *)&other_tex,NULL);
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    uint64_t *prior=(uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD);
    CHECK(prior[257]!=prior[*(uint32_t *)table.contents],"other descriptor initially real");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"other creation completed");
    h_nvnTextureFinalize(&other_tex); h_nvnMemoryPoolFinalize(&unrelated);
    mtl_sub_begin(); uint64_t drop_builds=g_stat_pool_builds;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    uint64_t *after=(uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD);
    CHECK(after[256]==id0,"pool eviction preserves unrelated binding");
    CHECK(after[257]==after[*(uint32_t *)table.contents],"pool eviction replaces removed binding with dummy");
    CHECK(g_stat_pool_builds==drop_builds,"pool eviction patches without full-table rebuild");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"pool eviction completed");
    mtl_reg_tex(257,NULL,NULL);
    // A nonzero sparse generation means physical/virtual aliasing may exist;
    // a physical-range note alone is then insufficient to select descriptors.
    uint64_t sparse_before=atomic_fetch_add(&g_vp_gen,1);
    CHECK(h_nvnMemoryPoolInitialize(&unrelated,(NVNmemoryPoolBuilder *)&pb),"pool initialized with sparse mappings possible");
    mtl_sub_begin();uint64_t full_before=g_stat_pool_builds;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    CHECK(g_stat_pool_builds>full_before,"sparse mappings retain conservative full-table fallback");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"sparse fallback submission completed");
    h_nvnMemoryPoolFinalize(&unrelated);atomic_store(&g_vp_gen,sparse_before);
    h_nvnTextureFinalize(&tex);h_nvnMemoryPoolFinalize(&pool);
    mtl_sub_begin();os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    uint64_t *entries=(uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD);
    CHECK(entries[256]==entries[*(uint32_t *)table.contents],"removed texture storage becomes dummy");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"removal submission completed");
    CHECK(!mtl_last_commit_error(),"no GPU error");
    mtl_reg_tex(256,NULL,NULL);defer_drain();munmap(mem,bytes);munmap(other,bytes);
    printf("t_pool_registration_delta: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
