// A buffer image below Metal's 16-byte view alignment (nvn_mtl.m tbias_view): bound as a view from the aligned boundary below its offset, wider by the elements in between,
// and the shader moves each coordinate by that count from its offset table (the translator's TranslationFlags.MetalBufferImageBias), so writes land on the image's own bytes.
// Assassin's Creed II's occlusion culling wrote its visibility marks two R32 elements early into buffers at pool offsets 8 mod 16 (GitHub PR #1399). The kernel below does
// what a translated shader does (Instructions.cs BiasBufferImageCoordinate: x = c + bias, or c itself when that sum wraps), and the pool's bytes are checked around the image
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { POOL = 65536, W = 16, OFF = 0x108, ALIGNED = 0x200, FILL = 0xEE };
static const char *kSource =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void k(texture_buffer<uint, access::read_write> img [[texture(0)]], constant uint4 *table [[buffer(0)]], constant int *coords [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
    "    uint b = table[0].x, c = uint(coords[i]), m = c + b;\n"
    "    img.write(uint4(100u + i), m < b ? c : m);\n"
    "}\n";
static tex_t buffer_image(uint8_t *mem, uint64_t off, uint64_t serial) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .mem = mem + off, .off = (int64_t)off};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32UI, .flags = NVN_TEXTURE_FLAGS_IMAGE, .width = W, .height = 1, .depth = 1, .levels = 1, .samples = 1};
    mtl_tex_init(&t);
    return t;
}
int main(void) { @autoreleasepool {
    mtl_init();
    uint8_t *mem; if (posix_memalign((void **)&mem, (size_t)getpagesize(), POOL)) abort();
    memset(mem, FILL, POOL); mtl_pool_add(mem, POOL);
    tex_t img = buffer_image(mem, OFF, 99701), aligned = buffer_image(mem, ALIGNED, 99702);
    mtl_reg_image(701, &img, NULL); mtl_reg_image(702, &aligned, NULL);
    nvn_rbind b = {.kind = NVN_RB_IMG, .dtt = MTLTextureTypeTextureBuffer, .dpf = NVN_DPF_UINT, .rw = 1, .tbx = 1};
    os_unfair_lock_lock(&g_rmu);
    uint64_t rid = 0; int real = 0; uint32_t bias = 99;
    id<MTLTexture> t = img_binding(701, &b, &rid, &real);
    CHECK(real && t && t.textureType == MTLTextureTypeTextureBuffer && t.bufferOffset == OFF, "the image's own view sits at its pool offset");
    id<MTLTexture> v = tbias_view(701, t, &rid, &bias);
    CHECK(bias == 2, "8 bytes below the boundary: the shader moves R32 coordinates by 2");
    CHECK(v && v != t && v.buffer == t.buffer && v.bufferOffset == OFF - 8 && v.width == W + 2 && v.pixelFormat == t.pixelFormat, "the bound view starts at the boundary below, 2 elements wider");
    CHECK(rid == v.gpuResourceID._impl, "the slot gets the view's resource ID");
    uint64_t rid2 = 0; uint32_t bias2 = 0;
    CHECK(tbias_view(701, t, &rid2, &bias2) == v && bias2 == 2 && rid2 == rid, "the view is kept with the texture and found again");
    uint64_t arid = 0; int areal = 0; uint32_t abias = 99;
    id<MTLTexture> at = img_binding(702, &b, &arid, &areal), av = tbias_view(702, at, &arid, &abias);
    CHECK(av == at && abias == 0, "an image at an aligned offset is bound as it is, its coordinates unmoved");
    os_unfair_lock_unlock(&g_rmu);

    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(kSource) options:nil error:&error];
    id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
    id<MTLComputePipelineState> pso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
    CHECK(pso != nil, "test kernel compiled"); if (!pso) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    // Coordinates: the first and last element, two in between, and three outside the image: -1 and -2 (whose sum with the offset wraps into the 8 bytes before it
    // without the guard) and W (one past its end)
    const int32_t coords[] = {0, 1, 7, W - 1, -1, -2, W};
    enum { N = sizeof coords / sizeof *coords };
    uint32_t table[16] = {bias};
    id<MTLBuffer> tb = [g_dev newBufferWithBytes:table length:sizeof table options:MTLResourceStorageModeShared], cb = [g_dev newBufferWithBytes:coords length:sizeof coords options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cmd = [q commandBuffer]; id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso]; [enc setTexture:v atIndex:0]; [enc setBuffer:tb offset:0 atIndex:0]; [enc setBuffer:cb offset:0 atIndex:1];
    [enc dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
    CHECK(cmd.status == MTLCommandBufferStatusCompleted, "kernel completed");
    uint32_t want[W]; memset(want, FILL, sizeof want);
    for (int i = 0; i < N; i++) if (coords[i] >= 0 && coords[i] < W) want[coords[i]] = 100u + (uint32_t)i;
    CHECK(!memcmp(mem + OFF, want, sizeof want), "every write lands on its own element of the image");
    int before = 1, after = 1;
    for (int i = 0; i < OFF; i++) before &= mem[i] == FILL;
    for (int i = OFF + W * 4; i < POOL; i++) after &= mem[i] == FILL;
    CHECK(before, "nothing before the image is written: a coordinate before it stays outside the view");
    CHECK(after, "nothing after the image is written");
    for (int i = 0; i < W; i++) if (((uint32_t *)(void *)(mem + OFF))[i] != want[i]) fprintf(stderr, "  element %d: %#x, expected %#x\n", i, ((uint32_t *)(void *)(mem + OFF))[i], want[i]);
    // For the record, not a check (it is the GPU's behaviour, which Metal does not define): the image's own view, its coordinates unmoved, as before the offset table
    memset(mem, FILL, POOL); table[0] = 0; memcpy(tb.contents, table, sizeof table);
    cmd = [q commandBuffer]; enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso]; [enc setTexture:t atIndex:0]; [enc setBuffer:tb offset:0 atIndex:0]; [enc setBuffer:cb offset:0 atIndex:1];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];   // element 0 only
    [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
    for (int o = 0; o < POOL; o += 4) if (*(uint32_t *)(void *)(mem + o) == 100u) { printf("the view made at pool offset %#x itself writes element 0 at pool offset %#x\n", OFF, o); break; }
    mtl_pool_del(mem);
    printf("t_buffer_image_bias: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
