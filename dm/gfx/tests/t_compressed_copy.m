// Copies through distinct Metal buffers and sparse guest aliases must see earlier GPU writes.
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

static void finish(void) {
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
}
static void bytes(const void *got, const void *want, size_t n, const char *label) {
    CHECK(!memcmp(got, want, n), label);
}
// A compressed IMAGE texture still takes CPU uploads. A later pool chunk must
// preserve a GPU copy into it, including the bytes mirrored into guest storage.
static void compressed_copy_chunk_case(int image_flag) {
    size_t len=1ull<<30, n=65536;
    uint8_t *mem=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    uint8_t *src=mmap(NULL,131072,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    CHECK(mem!=MAP_FAILED && src!=MAP_FAILED,"fixture allocation");
    pool_t pool; pool_builder_t pb={.mem=mem,.size=len,.flags=0x22};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool*)&pool,(NVNmemoryPoolBuilder*)&pb),"pool initialized");
    mtl_pool_add(src,131072);
    tex_t t={.magic=MAGIC('T','E','X',0),.pool=&pool,.mem=mem+(64<<20),.off=64<<20,.serial=99000+image_flag};
    t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=0x44,.width=256,.height=256,.depth=1,.levels=1,.samples=1,.flags=image_flag?NVN_TEXTURE_FLAGS_IMAGE:0};
    nvn_tex_layout(&t.d,&t.L); memset(t.mem,0x33,t.L.size);
    for(size_t i=0;i<n;i++) src[i]=(uint8_t)(i*13+7);
    mtl_tex_init(&t); mtl_sub_begin(); tex_mtl(&t); finish();
    p_copy_b2t cp={.src=(uint64_t)src,.tex=&t,.r={.width=256,.height=256,.depth=1}};
    mtl_sub_begin(); copy_b2t(&cp,COPY_STRIDES_OFF); finish();
    mtl_sub_begin(); id<MTLTexture> mt=tex_mtl(&t); finish();
    id<MTLBuffer> out=[g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];res_add(out);
    mtl_sub_begin();enc_b_copy_texture_buffer(blit(),mt,0,0,MTLOriginMake(0,0,0),MTLSizeMake(256,256,1),out,0,1024,n);finish();
    CHECK(!memcmp(out.contents,src,n),"GPU copy correct before new chunk");
    uint8_t *linear=malloc(n);
    CHECK(!nvn_deswizzle(&t.d,0,0,t.mem,linear,1024),"guest storage deswizzles");
    CHECK(!memcmp(linear,src,n),"compressed GPU copy is mirrored into guest storage"); free(linear);
    void *buf;uint64_t off,gpu; CHECK(mtl_gpu_addr((uint64_t)t.mem,4096,&buf,&off,&gpu),"chunk created across protected texture");
    mtl_sub_begin();mt=tex_mtl(&t);finish();
    mtl_sub_begin();enc_b_copy_texture_buffer(blit(),mt,0,0,MTLOriginMake(0,0,0),MTLSizeMake(256,256,1),out,0,1024,n);finish();
    CHECK(!memcmp(out.contents,src,n),"GPU copy preserved after new chunk");
    mtl_tex_release(&t); res_remove(out); mtl_pool_del(src);
    h_nvnMemoryPoolFinalize((NVNmemoryPool*)&pool); mtl_drain();
    munmap(src,131072); munmap(mem,len);
}

int main(void) { @autoreleasepool {
    mtl_init(); compressed_copy_chunk_case(0); compressed_copy_chunk_case(1);
    printf("t_compressed_copy: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
