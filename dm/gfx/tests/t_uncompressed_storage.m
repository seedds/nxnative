// An uncompressed texture the CPU wrote, with no flush, in a CPU_CACHED pool (tex_cpu_content): sampled at first use, it reads the CPU's bytes and a later CPU write is
// re-uploaded while no render target, shader image or texture clear has been recorded in its pool. In a pool the GPU writes it keeps the earlier rule (zeros with no flush), and a
// texture taken from its storage before its pool's first GPU write becomes a GPU texture: it keeps its content and is no longer re-uploaded.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
static void finish(void) { CHECK("submission completes",mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK("GPU has no error",!mtl_last_commit_error()); }
static void fill(tex_t *t,uint32_t rgba){   // every texel of level 0 = rgba
    uint32_t linear[16*16]; for(int i=0;i<16*16;i++) linear[i]=rgba;
    CHECK("fixture swizzle succeeds",!nvn_swizzle(&t->d,0,0,linear,16*4,t->mem));
}
static uint32_t texel(id<MTLTexture> image){   // the centre texel, as RGBA8
    NSError *error=nil; static id<MTLComputePipelineState> ps; static id<MTLCommandQueue> queue;
    if(!ps){id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void rd(texture2d<float> t[[texture(0)]],device uint *o[[buffer(0)]],uint i[[thread_position_in_grid]]){o[i]=pack_float_to_unorm4x8(t.read(uint2(8,8)));}" options:nil error:&error];
        ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&error];queue=[g_dev newCommandQueue];}
    CHECK("probe compiles",ps!=nil); if(!ps||!image) return 0xdeadbeef;
    id<MTLBuffer> r=[g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (!r) return 0xdeadbeef;
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
    [ce setComputePipelineState:ps];[ce setTexture:image atIndex:0];[ce setBuffer:r offset:0 atIndex:0];[ce dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[ce endEncoding];[cb commit];[cb waitUntilCompleted];
    return *(uint32_t *)r.contents;
}
typedef struct { void *mem; pool_t pool; tex_t t; } fx;
static void fx_make(fx *f,uint64_t serial){
    if(posix_memalign(&f->mem,getpagesize(),65536))abort(); memset(f->mem,0,65536);
    f->pool=(pool_t){.magic=MAGIC('P','O','O','L'),.mem=f->mem,.size=65536,.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(f->mem,65536);
    f->t=(tex_t){.magic=MAGIC('T','E','X',0),.serial=serial,.pool=&f->pool,.mem=f->mem};
    f->t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=16,.height=16,.depth=1,.levels=1,.samples=1};
    nvn_tex_layout(&f->t.d,&f->t.L);
}
static void fx_free(fx *f){ mtl_tex_release(&f->t); mtl_alias_drop_pool(&f->pool); mtl_pool_del(f->mem); mtl_drain(); free(f->mem); }
static id<MTLTexture> resolve(tex_t *t){ mtl_sub_begin(); id<MTLTexture> m=tex_mtl(t); finish(); return m; }
int main(void){@autoreleasepool{mtl_init();
    // A pool the GPU never writes: the CPU's bytes at first use, and a later CPU write
    fx a; fx_make(&a,9850); fill(&a.t,0xff3366ccu); mtl_tex_init(&a.t);
    CHECK("no GPU write: first use reads the CPU's bytes",texel(resolve(&a.t))==0xff3366ccu);
    CHECK("no GPU write: the texture is CPU-written",tex_cpu_written(9850));
    dm_guest_write_prepare(a.mem,a.t.L.size); fill(&a.t,0xff00ff00u);
    CHECK("no GPU write: a later CPU write is re-uploaded",texel(resolve(&a.t))==0xff00ff00u);
    // The pool's first GPU write: the texture keeps its content as a GPU texture and is no longer re-uploaded
    mtl_pool_gpu_written(&a.pool);
    id<MTLTexture> kept=resolve(&a.t);
    CHECK("after a GPU write into the pool: not CPU-written",!tex_cpu_written(9850));
    CHECK("after a GPU write into the pool: the content is kept",texel(kept)==0xff00ff00u);
    dm_guest_write_prepare(a.mem,a.t.L.size); fill(&a.t,0xffff0000u);
    CHECK("after a GPU write into the pool: a CPU write is not re-uploaded",texel(resolve(&a.t))==0xff00ff00u);
    fx_free(&a);
    // A pool with a render target recorded in it: the earlier rule, a GPU texture of zeros with no flush
    fx b; fx_make(&b,9851); mtl_pool_gpu_written(&b.pool); fill(&b.t,0xff3366ccu); mtl_tex_init(&b.t);
    CHECK("pool with a render target: zeros, as before",texel(resolve(&b.t))==0);
    CHECK("pool with a render target: not CPU-written",!tex_cpu_written(9851));
    fx_free(&b);
    printf("t_uncompressed_storage: %d checks, %d failed\n",checks,failed);return failed!=0;}}
