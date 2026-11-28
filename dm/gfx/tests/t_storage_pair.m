// Storage pairs (nvn_mtl.m pair_texture): a 4-sample colour target of W x H and the single-sample image of 2W x 2H in its format over one pool offset are one storage on NVN, sample s
// of pixel (x, y) being texel (2x + (s & 1), 2y + (s >> 1)) (Metro 2033 Redux's effects target over its scene target). Checked here on the host's own textures: whichever of the
// pair is made first, the second is placed over it in its heap and reads and writes the same samples, compressed (recycled memory) and uncompressed (a resolution family); it holds
// the heap, so the first one's release does not hand the heap to another texture while it lives, and a first one made again joins it with the content kept; other sample counts,
// sizes, formats, depth and CPU-visible pools stay apart. Runs with DM_NVN_RT_RECYCLE=0 (a partner outside a heap stays apart) and DM_NVN_STORAGE_ALIAS=0 (nothing is paired)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static pool_t g_pool; static uint64_t g_serial = 97000;
static tex_t tex(uint64_t off, unsigned w, unsigned h, unsigned samples, int format, unsigned flags) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = ++g_serial, .pool = &g_pool, .mem = g_pool.mem + off, .off = (int64_t)off};
    t.d = (nvn_texdesc){.target = samples > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D, .format = format, .flags = flags, .width = w, .height = h, .depth = 1, .levels = 1, .samples = samples};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
// The host holds an autorelease pool from mtl_sub_begin to mtl_sub_commit: a pool of the test's own encloses whole submissions
#define POOLED(...) do { mtl_sub_commit(); mtl_drain(); @autoreleasepool { mtl_sub_begin(); __VA_ARGS__; mtl_sub_commit(); mtl_drain(); } mtl_sub_begin(); } while (0)
static void settle(void) {   // the released textures' fences pass and the deferred releases run (as in t_image_alias)
    mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
    dispatch_sync(g_feedback_q, ^{}); defer_drain(); mtl_sub_begin();
}
static int waits(id<MTLHeap> h) { int w = 0; os_unfair_lock_lock(&g_rh_lk); for (unsigned i = 0; i < g_rh_n; i++) w |= (__bridge id)g_rh[i].heap == h; os_unfair_lock_unlock(&g_rh_lk); return w; }
static void drop_entry(const tex_t *t) { alias_key k = tex_alias_key(t); pthread_mutex_lock(&g_smu); own_alias_del_locked(&k); pthread_mutex_unlock(&g_smu); }

static id<MTLLibrary> g_lib;
static void classic(void (^f)(id<MTLCommandBuffer>)) {   // on the classic queue, after the submissions so far (their residency changes committed)
    mtl_sub_commit(); mtl_drain();
    id<MTLCommandBuffer> cb = q_cb(); f(cb); [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "the test's command buffer completes"); mtl_sub_begin();
}
// A ramp drawn into t, every sample its own value when t is multisampled
static void ramp(id<MTLTexture> t, uint32_t seed) {
    NSError *e = nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [g_lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [g_lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = t.pixelFormat; pd.rasterSampleCount = t.sampleCount;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e]; CHECK(ps != nil, "the ramp pipeline builds"); if (!ps) return;
    classic(^(id<MTLCommandBuffer> cb) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; uint32_t s = seed;
        [re setRenderPipelineState:ps]; [re setFragmentBytes:&s length:4 atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
    });
}
// Every texel of the single-sample image and every sample of the 4-sample target, decoded the same way: → how many texels hold the sample NVN puts there, of how many
static size_t agree(id<MTLTexture> one, id<MTLTexture> four, size_t *distinct) {
    *distinct = 0;
    NSError *e = nil; size_t W = one.width, H = one.height;
    id<MTLFunction> f1 = [g_lib newFunctionWithName:@"rd1"], f4 = [g_lib newFunctionWithName:@"rd4"];
    CHECK(f1 && f4, "the readback functions exist"); if (!f1 || !f4) return 0;
    id<MTLComputePipelineState> r1 = [g_dev newComputePipelineStateWithFunction:f1 error:&e], r4 = [g_dev newComputePipelineStateWithFunction:f4 error:&e];
    CHECK(r1 && r4, "the readback pipelines build"); if (!r1 || !r4) return 0;
    id<MTLBuffer> o1 = [g_dev newBufferWithLength:W * H * 16 options:MTLResourceStorageModeShared], o4 = [g_dev newBufferWithLength:W * H * 16 options:MTLResourceStorageModeShared];
    CHECK(o1 && o4, "the readback buffers allocate"); if (!o1 || !o4) return 0;
    classic(^(id<MTLCommandBuffer> cb) {
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:r1]; [ce setTexture:one atIndex:0]; [ce setBuffer:o1 offset:0 atIndex:0]; [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce setComputePipelineState:r4]; [ce setTexture:four atIndex:0]; [ce setBuffer:o4 offset:0 atIndex:0]; [ce dispatchThreads:MTLSizeMake(W / 2, H / 2, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce endEncoding];
    });
    const float *a = o1.contents, *b = o4.contents; size_t ok = 0, nz = 0;
    for (size_t y = 0; y < H; y++) for (size_t x = 0; x < W; x++) {
        const float *ta = a + (y * W + x) * 4, *tb = b + (((y / 2) * (W / 2) + x / 2) * 4 + ((x & 1) | ((y & 1) << 1))) * 4;
        ok += ta[0] == tb[0] && ta[1] == tb[1] && ta[2] == tb[2] && ta[3] == tb[3]; nz += ta[0] != a[0] || ta[1] != a[1] || ta[3] != a[3];
    }
    *distinct = nz; return ok;
}
static void same_samples(id<MTLTexture> one, id<MTLTexture> four, const char *what) {
    size_t n = one.width * one.height, distinct; char label[200];
    ramp(four, 1); size_t a = agree(one, four, &distinct);
    printf("%s: a ramp drawn per sample into the 4-sample target, %zu of %zu texels of the image hold their sample (%zu not the first one's)\n", what, a, n, distinct);
    snprintf(label, sizeof label, "%s: every sample drawn through the 4-sample target is its texel of the image", what); CHECK(a == n && distinct > n / 2, label);
    ramp(one, 2); a = agree(one, four, &distinct);
    printf("%s: a ramp drawn into the image, %zu of %zu samples of the 4-sample target hold their texel\n", what, a, n);
    snprintf(label, sizeof label, "%s: every texel drawn through the image is its sample of the 4-sample target", what); CHECK(a == n && distinct > n / 2, label);
}

// The order of making, the layout and the heap's lifetime, with the scene target in recycled memory (compressed, rh_place)
static void pair_in_recycled_memory(int ms_first) {
    char what[80]; snprintf(what, sizeof what, "recycled memory, %s made first", ms_first ? "4-sample" : "single-sample");
    uint64_t off = ms_first ? 0 : 8 << 20;
    tex_t scene = tex(off, 1280, 720, 1, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE), fx = tex(off, 640, 360, 4, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    tex_t *firstt = ms_first ? &fx : &scene, *secondt = ms_first ? &scene : &fx;
    CHECK(scene.L.size == fx.L.size, "NVN lays the pair out in the same bytes");
    unsigned before = g_pair_n; __weak id<MTLHeap> heap; __weak id<MTLTexture> gone, kept;
    POOLED({
        id<MTLTexture> first = tex_mtl(firstt), second = tex_mtl(secondt), one = tex_mtl(&scene), four = tex_mtl(&fx);
        CHECK(four.sampleCount == 4 && one.sampleCount == 1 && one.width == 1280 && four.width == 640, "each description keeps its own Metal texture");
        if (!sw_on(rh_mode())) {
            CHECK(!first.heap && !second.heap && one != four && g_pair_n == before, "with recycled memory off the first is its own allocation and the pair stays apart (a red line)");
            first = second = one = four = nil; mtl_tex_release(&scene); mtl_tex_release(&fx); drop_entry(&scene); drop_entry(&fx);
        } else {
            heap = first.heap; gone = first; kept = second;
            CHECK(heap && second.heap == heap && !first.heapOffset && !second.heapOffset, "the second of the pair is placed at the start of the first one's heap");
            CHECK(second.allowGPUOptimizedContents == first.allowGPUOptimizedContents, "with the first one's compression");
            CHECK(g_pair_n == before + 1 && (resource_vis() & MTL4VisibilityOptionResourceAlias), "while a pair lives, barriers flush aliased memory");
            same_samples(one, four, what);
            first = second = one = four = nil; mtl_tex_release(firstt); drop_entry(firstt);   // the first one goes while the second lives
        }
    });
    settle();
    if (!sw_on(rh_mode())) return;
    CHECK(!gone && kept, "the first one's texture is released, the second one's kept by the sharing table");
    CHECK(heap && !waits(heap), "the first one's release leaves the heap with the second, not waiting for another texture");
    tex_t again = ms_first ? tex(off, 640, 360, 4, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE) : tex(off, 1280, 720, 1, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    POOLED({   // a first one made again over the same bytes joins the second's heap, the content kept
        id<MTLTexture> made = tex_mtl(&again), second = tex_mtl(secondt);
        CHECK(made.heap == heap && second == kept, "a texture made again over the bytes joins the live other's heap");
        size_t distinct, n = 1280 * 720, a = agree(ms_first ? second : made, ms_first ? made : second, &distinct);
        CHECK(a == n && distinct > n / 2, "and reads what was drawn before: nothing filled it");
        made = second = nil; mtl_tex_release(&again); drop_entry(&again); mtl_tex_release(secondt); drop_entry(secondt);
    });
    settle();
    CHECK(!kept && g_pair_n == before, "the pair's alias visibility goes with its second texture");
    CHECK(heap && waits(heap), "the heap waits for its next texture once both are gone");
}
// A single-sample image of a resolution family (image_texture): the 4-sample target joins the family's heap with matching compression
static void pair_in_family(void) { POOLED({
    uint64_t off = 16 << 20;
    tex_t scene = tex(off, 1280, 720, 1, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_COMPRESSIBLE), small = tex(off, 1152, 648, 1, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    tex_t fx = tex(off, 640, 360, 4, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    id<MTLTexture> one = tex_mtl(&scene), four = tex_mtl(&fx);
    CHECK(one.heap && objc_getAssociatedObject(one, &g_image_heap_owner_key) && one.allowGPUOptimizedContents == !fam_nocomp(), "the family's image is in the family's heap with the family's compression");
    CHECK(four.heap == one.heap && four.allowGPUOptimizedContents == one.allowGPUOptimizedContents && objc_getAssociatedObject(four, &g_image_heap_owner_key), "the 4-sample target joins it with matching compression, holding the family's heap");
    same_samples(one, four, "resolution family");
    CHECK(tex_mtl(&small).heap == one.heap, "the family's other size still shares the heap");
    one = four = nil; mtl_tex_release(&scene); mtl_tex_release(&small); mtl_tex_release(&fx); drop_entry(&scene); drop_entry(&small); drop_entry(&fx);
}); }
// What is not one storage stays apart
static void apart(void) { POOLED({
    struct { unsigned w, h, s; int fmt; unsigned flags; const char *what; } c[] = {
        {640, 720, 2, NVN_FORMAT_RGBA8, 0, "2 samples (Metal lays them out otherwise)"},
        {640, 360, 4, NVN_FORMAT_RGBA16F, 0, "another format"},
        {320, 360, 4, NVN_FORMAT_RGBA8, 0, "another size"},
        {640, 360, 4, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR, "a linear target"},
    };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        uint64_t off = (24 + 8 * (uint64_t)i) << 20; unsigned before = g_pair_n; char label[160];
        tex_t scene = tex(off, 1280, 720, 1, NVN_FORMAT_RGBA8, 0), other = tex(off, c[i].w, c[i].h, c[i].s, c[i].fmt, c[i].flags);
        id<MTLTexture> one = tex_mtl(&scene), two = tex_mtl(&other);
        snprintf(label, sizeof label, "%s stays apart", c[i].what);
        CHECK(one && (!two || !one.heap || two.heap != one.heap) && g_pair_n == before, label);
        mtl_tex_release(&scene); mtl_tex_release(&other); drop_entry(&scene); drop_entry(&other);
    }
    {   // a depth target
        uint64_t off = 60 << 20; unsigned before = g_pair_n;
        tex_t z = tex(off, 1280, 720, 1, NVN_FORMAT_DEPTH32F, 0), zm = tex(off, 640, 360, 4, NVN_FORMAT_DEPTH32F, 0);
        id<MTLTexture> a = tex_mtl(&z), b = tex_mtl(&zm);
        CHECK(a && (!b || !a.heap || b.heap != a.heap) && g_pair_n == before, "depth targets stay apart");
        mtl_tex_release(&z); mtl_tex_release(&zm); drop_entry(&z); drop_entry(&zm);
    }
    {   // a CPU-visible pool
        pool_t saved = g_pool; g_pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED; unsigned before = g_pair_n;
        tex_t s = tex(56 << 20, 1280, 720, 1, NVN_FORMAT_RGBA8, 0), m = tex(56 << 20, 640, 360, 4, NVN_FORMAT_RGBA8, 0);
        id<MTLTexture> a = tex_mtl(&s), b = tex_mtl(&m);
        CHECK(a && (!b || !a.heap || b.heap != a.heap) && g_pair_n == before, "a CPU-visible pool's pair stays apart");
        mtl_tex_release(&s); mtl_tex_release(&m); drop_entry(&s); drop_entry(&m); g_pool = saved;
    }
}); }
// The layout check (pair_layout_ok): it ran for the kinds paired above, once each, and finds a pair Metal lays out otherwise (one compressed, one not)
static void layout_check(void) { POOLED({
    MTLTextureDescriptor *one = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:1280 height:720 mipmapped:NO];
    one.storageMode = MTLStorageModePrivate; one.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    MTLTextureDescriptor *four = [one copy]; four.textureType = MTLTextureType2DMultisample; four.sampleCount = 4; four.width = 640; four.height = 360;
    uint32_t bad = 1; int ok = pair_layout_ok(four, one, &bad); unsigned before = g_pair_probes;
    CHECK(ok && bad == 0, "the scene target's kind lays out as NVN does");
    CHECK(pair_layout_ok(one, four, &bad) && bad == 0 && g_pair_probes == before, "a kind already checked is not checked again");
    one.allowGPUOptimizedContents = YES; four.allowGPUOptimizedContents = NO;
    ok = pair_layout_ok(four, one, &bad);
    printf("layout check, a compressed image over an uncompressed 4-sample target: %u of %u samples elsewhere\n", bad, 1280u * 720u);
    CHECK(!ok && bad && bad != UINT32_MAX && g_pair_probes == before + 1, "a pair laid out otherwise fails the check");
    one.pixelFormat = four.pixelFormat = MTLPixelFormatRG16Uint; one.allowGPUOptimizedContents = four.allowGPUOptimizedContents = YES;
    CHECK(pair_layout_ok(one, four, &bad) && !bad, "an integer format checks through its own kernel");
}); }
int main(void) { @autoreleasepool {
    mtl_init(); void *mem = NULL; posix_memalign(&mem, 16384, 64 << 20); memset(mem, 0, 64 << 20);
    g_pool = (pool_t){.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem = mem, .size = 64 << 20};
    NSError *e = nil;
    g_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vs(uint v [[vertex_id]]) { float2 p = float2((v << 1) & 2, v & 2); return float4(p * 2 - 1, 0, 1); }\n"
        "fragment float4 fs(float4 pos [[position]], uint s [[sample_id]], constant uint &seed [[buffer(0)]]) { uint x = uint(pos.x), y = uint(pos.y);"
        " return float4(float((x * seed) & 255) / 255, float(y & 255) / 255, float((x * 7 + y * 3 + seed) & 255) / 255, float(s * 60 + seed) / 255); }\n"
        "kernel void rd1(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
        " if (p.x < t.get_width() && p.y < t.get_height()) o[p.y * t.get_width() + p.x] = t.read(p); }\n"
        "kernel void rd4(texture2d_ms<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {"
        " if (p.x < t.get_width() && p.y < t.get_height()) for (uint s = 0; s < 4; s++) o[(p.y * t.get_width() + p.x) * 4 + s] = t.read(p, s); }\n" options:nil error:&e];
    if (!g_lib) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    mtl_sub_begin();
    if (!pair_on()) {   // DM_NVN_STORAGE_ALIAS=0
        tex_t s = tex(0, 1280, 720, 1, NVN_FORMAT_RGBA8_SRGB, 0), m = tex(0, 640, 360, 4, NVN_FORMAT_RGBA8_SRGB, 0);
        id<MTLTexture> a = tex_mtl(&s), b = tex_mtl(&m);
        CHECK(a && b && (!a.heap || b.heap != a.heap) && g_pair_n == 0, "with the switch off the pair stays apart");
        mtl_tex_release(&s); mtl_tex_release(&m); mtl_alias_drop_pool(&g_pool); settle();
        printf("storage pairs (off): %d checks, %d failed\n", checks, failed); return failed != 0;
    }
    pair_in_recycled_memory(0);
    pair_in_recycled_memory(1);
    pair_in_family();
    CHECK(g_pair_probes == (sw_on(rh_mode()) ? 2u : 1u), "the layout was checked once for each kind paired (compressed in recycled memory, uncompressed in a family)");
    apart();
    layout_check();
    mtl_alias_drop_pool(&g_pool); settle(); settle();
    CHECK(g_pair_n == 0, "no pair outlives its textures");
    printf("storage pairs: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
