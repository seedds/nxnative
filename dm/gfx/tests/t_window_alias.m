// Textures in GPU-only memory share bytes as on NVN: descriptions of other sizes and formats over overlapping storage of a CPU_NO_ACCESS pool are placed in one placement heap per
// 32 MB span of guest addresses, each at its own address's offset scaled by 9/8 (window_texture). Ocarina of Time: 400 Metal textures of 793 MB stood for 89 MB of NVN storage
// (docs/reference/memory.md). A span gets its heap only once the textures made apart over it pass the heap's size. The spans are on by themselves only where Metal's working set
// is under 8 GB (window_wants); the check target turns them on with DM_NVN_WINDOW_ALIAS=1. Run with DM_NVN_WINDOW_ALIAS=0 it shows the old behaviour, every description an
// allocation of its own, and fails
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static tex_t target(pool_t *pool, uint64_t serial, uint64_t off, unsigned w, unsigned h, int format) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .pool=pool, .mem=pool->mem+off, .off=(int64_t)off};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format, .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width=w, .height=h, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static id<MTLBuffer> read_image(id<MTLTexture> t) {
    NSUInteger row=t.width*4, bytes=row*t.height;
    id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(b);
    enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(t.width,t.height,1), b, 0, row, bytes);
    mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "image read completes without GPU error");
    res_remove(b); mtl_sub_begin(); return b;
}
static void fill(id<MTLTexture> t, uint32_t seed) {
    NSUInteger bytes=t.width*t.height*4;
    id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    uint32_t *p=b.contents; for (NSUInteger i=0;i<bytes/4;i++) p[i]=seed^(uint32_t)i;
    res_add(b); enc_b_copy_buffer_texture(blit(), b, 0, t.width*4, bytes, MTLSizeMake(t.width,t.height,1), t, 0, 0, MTLOriginMake(0,0,0));
    mtl_sub_commit(); mtl_drain(); res_remove(b); mtl_sub_begin();
}
static int holds(id<MTLTexture> t, uint32_t seed) {
    id<MTLBuffer> b=read_image(t); uint32_t *p=b.contents; int ok=1;
    for (NSUInteger i=0;i<b.length/4;i++) ok &= p[i]==(seed^(uint32_t)i);
    return ok;
}
static int zero(id<MTLTexture> t) { id<MTLBuffer> b=read_image(t); uint32_t *p=b.contents; int ok=1; for (NSUInteger i=0;i<b.length/4;i++) ok &= p[i]==0; return ok; }
int main(void) { @autoreleasepool {
    // The rule that turns the spans on by itself: the M1 iPad's working set (8 GB device, 5,461 MB) is in, a 16 GB Mac's (12,124 MB on an M4) is out, and an unknown size is out
    CHECK(window_wants(5461ull<<20) && window_wants(8191ull<<20), "a working set under 8 GB shares GPU-only memory in spans");
    CHECK(!window_wants(8192ull<<20) && !window_wants(12124ull<<20) && !window_wants(0), "8 GB of working set or more, or none known, keeps the separate heaps");
    mtl_init(); void *mem=NULL, *cmem=NULL; __weak id<MTLHeap> weak_heap;
    if (posix_memalign(&mem, WIN_SIZE, 2*WIN_SIZE) || posix_memalign(&cmem, 16384, 16<<20)) abort();
    memset(mem, 0, 2*WIN_SIZE); memset(cmem, 0, 16<<20);
    @autoreleasepool {
    mtl_sub_begin();
    pool_t gpu={.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=mem, .size=2*WIN_SIZE};
    pool_t cpu={.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=cmem, .size=16<<20};
    // 48 descriptions over the first 8 MB of the window, of other sizes and formats, as a game's passes and its dynamic resolution make them
    enum { N=48 }; tex_t a[N]; id<MTLTexture> m[N];
    uint64_t before=g_dev.currentAllocatedSize;
    for (int i=0;i<N;i++) { a[i]=target(&gpu, 970000+i, (uint64_t)(i%4)<<20, 1024-(i*7), 1024-(i*5), i%3==2 ? NVN_FORMAT_R32F : NVN_FORMAT_RGBA8); mtl_tex_init(&a[i]); m[i]=tex_mtl(&a[i]); }
    uint64_t delta=g_dev.currentAllocatedSize-before, sum=0; for (int i=0;i<N;i++) sum+=m[i].allocatedSize;
    printf("%d descriptions over 8 MB of GPU-only memory: %llu MB of textures, device allocation delta %llu MB\n", N, (unsigned long long)(sum>>20), (unsigned long long)(delta>>20));
    CHECK(!m[0].heap, "the first texture of a span keeps its own allocation: a heap would not save memory yet");
    int first=-1; for (int i=0;i<N && first<0;i++) { NvImageHeap *o=m[i].heap ? objc_getAssociatedObject(m[i], &g_image_heap_owner_key) : nil; if (o && o->window) first=i; }   // before it, same-offset families may share heaps of their own (image_texture)
    int one=first>0; for (int i=first>0?first:N;i<N;i++) one &= m[i].heap && m[i].heap==m[first].heap;
    CHECK(one && first<N/2, "once the textures made apart pass the heap's size, every later description shares one heap");
    int at=first>0; for (int i=first>0?first:N;i<N;i++) { MTLSizeAndAlign q=[g_dev heapTextureSizeAndAlignWithDescriptor:tex_desc(&a[i],0)]; at &= m[i].heapOffset==(WIN_SCALE((uint64_t)(i%4)<<20)+q.align-1)/q.align*q.align; }
    CHECK(at, "each sits at its own address's offset in the span, scaled by 9/8");
    // Bytes another description wrote are garbage to this one in Metal's layouts, where on NVN they meant the same: a lossless-compressed texture whose metadata another alias
    // overwrote is undefined to read, and on the M1 iPad Ocarina of Time's GPU hung on one (kIOGPUCommandBufferCallbackErrorHang). Placed textures carry no GPU-optimized contents
    int plain=first>0; for (int i=first>0?first:N;i<N;i++) plain &= !m[i].allowGPUOptimizedContents;
    CHECK(plain, "placed textures have no GPU-optimized (lossless-compressed) contents");
    CHECK(delta<2*WIN_HEAP+(16u<<20), "the GPU memory taken is the span's, not the sum of the descriptions");
    CHECK(first>0 && [g_res_objects containsObject:m[first].heap] && ![g_res_objects containsObject:m[N-1]], "the heap is resident, its textures through it");
    // Bytes are shared as on NVN: a texture over storage another wrote reads it; storage that does not overlap is untouched
    tex_t x=target(&gpu, 971000, 16u<<20, 256, 256, NVN_FORMAT_RGBA8), y=target(&gpu, 971001, 16u<<20, 256, 256, NVN_FORMAT_RGBA8UI), z=target(&gpu, 971002, 20u<<20, 256, 256, NVN_FORMAT_RGBA8);
    mtl_tex_init(&x); mtl_tex_init(&z); id<MTLTexture> tx=tex_mtl(&x), tz=tex_mtl(&z);
    CHECK(tx.heap && tx.heap==m[N-1].heap && tz.heap==tx.heap, "later textures of the span join its heap");
    CHECK(zero(tx) && zero(tz), "storage no texture covered before starts as zeros");
    fill(tx, 0x5a5a1234); fill(tz, 0x0f0f9876);
    mtl_tex_init(&y); id<MTLTexture> ty=tex_mtl(&y);
    CHECK(ty.heap==tx.heap && ty.heapOffset==tx.heapOffset && ty!=tx, "another format over the same storage is another texture over the same bytes");
    CHECK(holds(ty, 0x5a5a1234), "a description made over written storage inherits its bytes (no zero fill over a live alias)");
    CHECK(holds(tz, 0x0f0f9876) && holds(tx, 0x5a5a1234), "storage that does not overlap keeps its own bytes");
    // The second span has no heap for one texture; CPU-visible pools, linear textures and textures past their span's end keep allocations of their own
    tex_t w=target(&gpu, 971010, WIN_SIZE+(1u<<20), 512, 512, NVN_FORMAT_RGBA8); mtl_tex_init(&w);
    CHECK(!tex_mtl(&w).heap, "a lone texture in another span keeps its own allocation (no heap is made for it)");
    tex_t c=target(&cpu, 971020, 0, 512, 512, NVN_FORMAT_RGBA8); mtl_tex_init(&c);
    CHECK(!tex_mtl(&c).heap, "a CPU-visible pool's texture keeps its own allocation");
    tex_t l=target(&gpu, 971030, 24u<<20, 128, 64, NVN_FORMAT_RGBA8); l.d.flags|=NVN_TEXTURE_FLAGS_LINEAR; nvn_tex_layout(&l.d, &l.L); mtl_tex_init(&l);
    CHECK(!tex_mtl(&l).heap || tex_mtl(&l).heap!=tx.heap, "a linear texture stays out of the span's heap");
    // One across a span's end goes to the spans offset by half a span, made once they save memory too: two overlapping descriptions there share the second span's heap
    tex_t e=target(&gpu, 971040, WIN_SIZE-(64u<<10), 1024, 1024, NVN_FORMAT_RGBA8), e2=target(&gpu, 971041, WIN_SIZE-(64u<<10), 1000, 1000, NVN_FORMAT_R32F); mtl_tex_init(&e); mtl_tex_init(&e2);
    id<MTLTexture> te=tex_mtl(&e), te2=tex_mtl(&e2);
    CHECK(te.heap!=tx.heap && te2.heap && te2.heap!=tx.heap, "a texture across a span's end does not go to that span's heap");
    CHECK(te2.heap && (te.heap==te2.heap || !te.heap), "overlapping textures across the span's end share the offset span's heap");
    te=te2=nil;
    // The heap lives while one of its textures does
    weak_heap=tx.heap;
    for (int i=0;i<N;i++) { mtl_tex_release(&a[i]); m[i]=nil; }
    mtl_tex_release(&x); mtl_tex_release(&y); mtl_tex_release(&z); mtl_tex_release(&w); mtl_tex_release(&c); mtl_tex_release(&l); mtl_tex_release(&e); mtl_tex_release(&e2);
    tx=ty=tz=nil; mtl_alias_drop_pool(&gpu); mtl_alias_drop_pool(&cpu);
    mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
    dispatch_sync(g_feedback_q, ^{}); defer_drain();
    }
    CHECK(!weak_heap || ![g_res_objects containsObject:weak_heap], "the span's heap leaves the residency set once its textures have gone");
    CHECK(g_image_heap_n==0, "no heap owner is left");
    printf("t_window_alias: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
