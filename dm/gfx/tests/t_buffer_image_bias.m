// A buffer texture below Metal's view alignment (nvn_mtl.m tex_mtl, tbuf_lead): its view starts at the aligned boundary below its pool offset, wider by the elements in between, and
// the shader moves each coordinate by that count from its offset table (the translator's TranslationFlags.MetalBufferImageBias), so accesses land on the texture's own bytes.
// Assassin's Creed II's occlusion culling wrote its visibility marks two R32 elements early into buffers at pool offsets 8 mod 16 (GitHub PR #1399). The kernels below do what a
// translated shader does (Instructions.cs BiasBufferImageCoordinate: x = c + bias, or c itself when that sum wraps; a size query gives the width less the bias), and the pool's
// bytes are checked around the texture. A binding with no table entry reads an aligned copy of the texture's own bytes (tbuf_shadow). DM_TBUF_ALIGN=16 gives every Mac the
// M4 Pro's alignment, and every view is one Metal accepts, so the test runs under Metal validation
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
    "}\n"
    "kernel void r(texture_buffer<uint> tex [[texture(0)]], constant uint4 *table [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
    "    uint b = table[0].x, m = i + b;\n"
    "    out[i] = i < 16 ? tex.read(m < b ? i : m).x : tex.get_width() - b;\n"
    "}\n"
    // an array of buffer textures, as a translated shader reads it: element e's entry at the array's first plus e, picked by the same index as the texture
    "kernel void a(array<texture_buffer<uint>, 2> tex [[texture(0)]], constant uint4 *table [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
    "    uint e = i / 16, x = i % 16, n = 3u + e, b = table[n >> 2][n & 3], m = x + b;\n"
    "    out[i] = tex[e].read(m < b ? x : m).x;\n"
    "}\n";
static tex_t buffer_texture(uint8_t *mem, uint64_t off, uint64_t serial, uint32_t flags) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .mem = mem + off, .off = (int64_t)off};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32UI, .flags = flags, .width = W, .height = 1, .depth = 1, .levels = 1, .samples = 1};
    mtl_tex_init(&t);
    return t;
}
static id<MTLComputePipelineState> kernel_named(id<MTLLibrary> lib, NSString *name) {
    NSError *error = nil; id<MTLFunction> fn = [lib newFunctionWithName:name];
    id<MTLComputePipelineState> pso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
    if (!pso) fprintf(stderr, "%s: %s\n", name.UTF8String, error.localizedDescription.UTF8String);
    return pso;
}
int main(void) { @autoreleasepool {
    setenv("DM_TBUF_ALIGN", "16", 1);
    mtl_init();
    uint8_t *mem; if (posix_memalign((void **)&mem, (size_t)getpagesize(), POOL)) abort();
    memset(mem, FILL, POOL); mtl_pool_add(mem, POOL);
    tex_t img = buffer_texture(mem, OFF, 99701, NVN_TEXTURE_FLAGS_IMAGE), aligned = buffer_texture(mem, ALIGNED, 99702, NVN_TEXTURE_FLAGS_IMAGE), tex = buffer_texture(mem, OFF, 99703, 0);
    mtl_reg_image(701, &img, NULL); mtl_reg_image(702, &aligned, NULL); mtl_reg_tex(703, &tex, NULL);
    nvn_rbind b = {.kind = NVN_RB_IMG, .dtt = MTLTextureTypeTextureBuffer, .dpf = NVN_DPF_UINT, .rw = 1, .tbx = 1};
    os_unfair_lock_lock(&g_rmu);
    uint64_t rid = 0; int real = 0;
    id<MTLTexture> t = img_binding(701, &b, &rid, &real);
    CHECK(real && t && t.textureType == MTLTextureTypeTextureBuffer && t.bufferOffset == OFF - 8 && t.width == W + 2, "the image's view starts at the 16-byte boundary below it, 2 elements wider");
    uint32_t bias = tbuf_lead(701, t);
    CHECK(bias == 2, "8 bytes below the boundary: the shader moves R32 coordinates by 2");
    uint64_t arid = 0; int areal = 0;
    id<MTLTexture> at = img_binding(702, &b, &arid, &areal);
    CHECK(areal && at.bufferOffset == ALIGNED && at.width == W && tbuf_lead(702, at) == 0, "an image at an aligned offset is viewed as it is, its coordinates unmoved");
    void *tv = NULL; uint64_t trid = tex_fast_rid(703, &tv); id<MTLTexture> tt = (__bridge id<MTLTexture>)tv;
    CHECK(tt && tt.bufferOffset == OFF - 8 && tbuf_lead(703, tt) == 2, "a buffer texture in a texture slot is viewed the same way");
    uint64_t srid = trid; id<MTLTexture> copy = tbuf_shadow(703, tt, &srid);
    CHECK(copy && copy != tt && copy.width == W && copy.bufferOffset % 16 == 0 && srid == copy.gpuResourceID._impl, "a binding with no table entry reads an aligned copy of the texture's own elements");
    uint64_t srid2 = 0;
    CHECK(tbuf_shadow(703, tt, &srid2) == copy && srid2 == srid, "one copy per texture per submission");
    uint64_t arid2 = arid;
    CHECK(tbuf_shadow(702, at, &arid2) == at && arid2 == arid, "an aligned texture is read as it is");
    CHECK(tbuf_lead(703, copy) == 0 && tbuf_lead(703, dummy_tbuf(NVN_DPF_UINT)) == 0, "a copy or a dummy bound for the texture has no offset of its own");
    os_unfair_lock_unlock(&g_rmu);

    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(kSource) options:nil error:&error];
    id<MTLComputePipelineState> kw = lib ? kernel_named(lib, @"k") : nil, kr = lib ? kernel_named(lib, @"r") : nil;
    CHECK(kw && kr, "test kernels compiled"); if (!kw || !kr) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    // Writes through the image: coordinates the first and last element, two in between, and three outside it: -1 and -2 (whose sum with the offset wraps into the 8 bytes before it
    // without the guard) and W (one past its end)
    const int32_t coords[] = {0, 1, 7, W - 1, -1, -2, W};
    enum { N = sizeof coords / sizeof *coords };
    uint32_t table[16] = {bias};
    id<MTLBuffer> tb = [g_dev newBufferWithBytes:table length:sizeof table options:MTLResourceStorageModeShared], cb = [g_dev newBufferWithBytes:coords length:sizeof coords options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = [q commandBuffer]; id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:kw]; [enc setTexture:t atIndex:0]; [enc setBuffer:tb offset:0 atIndex:0]; [enc setBuffer:cb offset:0 atIndex:1];
    [enc dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
    CHECK(cmd.status == MTLCommandBufferStatusCompleted, "write kernel completed");
    uint32_t want[W]; memset(want, FILL, sizeof want);
    for (int i = 0; i < N; i++) if (coords[i] >= 0 && coords[i] < W) want[coords[i]] = 100u + (uint32_t)i;
    CHECK(!memcmp(mem + OFF, want, sizeof want), "every write lands on its own element of the image");
    int before = 1, after = 1;
    for (int i = 0; i < OFF; i++) before &= mem[i] == FILL;
    for (int i = OFF + W * 4; i < POOL; i++) after &= mem[i] == FILL;
    CHECK(before, "nothing before the image is written: a coordinate before it stays outside the view");
    CHECK(after, "nothing after the image is written");
    for (int i = 0; i < W; i++) if (((uint32_t *)(void *)(mem + OFF))[i] != want[i]) fprintf(stderr, "  element %d: %#x, expected %#x\n", i, ((uint32_t *)(void *)(mem + OFF))[i], want[i]);
    // Reads through the texture slot, in place with its table entry, and through the aligned copy with none; then the size each reports
    for (int i = 0; i < W; i++) ((uint32_t *)(void *)(mem + OFF))[i] = 500u + (uint32_t)i;
    ((uint32_t *)(void *)(mem + OFF))[-1] = ((uint32_t *)(void *)(mem + OFF))[-2] = 0xBAD;
    memset(g_tbs_cache, 0, sizeof g_tbs_cache);   // as a new submission: the copy taken above holds what the pool held then
    os_unfair_lock_lock(&g_rmu); uint64_t crid = 0; copy = tbuf_shadow(703, tt, &crid); os_unfair_lock_unlock(&g_rmu);
    struct { id<MTLTexture> tex; uint32_t bias; const char *what; } reads[] = {{tt, bias, "in place, its coordinates moved"}, {copy, 0, "through the aligned copy"}};
    for (int k = 0; k < 2; k++) {
        uint32_t rt[16] = {reads[k].bias}; memcpy(tb.contents, rt, sizeof rt);
        id<MTLBuffer> out = [g_dev newBufferWithLength:(W + 1) * 4 options:MTLResourceStorageModeShared];
        cmd = [q commandBuffer]; enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:kr]; [enc setTexture:reads[k].tex atIndex:0]; [enc setBuffer:tb offset:0 atIndex:0]; [enc setBuffer:out offset:0 atIndex:1];
        [enc dispatchThreads:MTLSizeMake(W + 1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
        const uint32_t *o = out.contents; int ok = cmd.status == MTLCommandBufferStatusCompleted;
        for (int i = 0; i < W; i++) ok &= o[i] == 500u + (uint32_t)i;
        char label[128]; snprintf(label, sizeof label, "a texture slot reads the texture's own elements %s", reads[k].what); CHECK(ok, label);
        snprintf(label, sizeof label, "and its size is the texture's width (%s)", reads[k].what); CHECK(o[W] == W, label);
    }
    {   // an array of the misaligned texture and the aligned one, its entries 3 and 4 (after a single binding's): each element reads its own elements in place
        for (int i = 0; i < W; i++) ((uint32_t *)(void *)(mem + ALIGNED))[i] = 700u + (uint32_t)i;
        id<MTLComputePipelineState> ka = kernel_named(lib, @"a");
        uint32_t at_[16] = {0}; at_[3] = bias; at_[4] = 0; memcpy(tb.contents, at_, sizeof at_);
        id<MTLBuffer> out = [g_dev newBufferWithLength:2 * W * 4 options:MTLResourceStorageModeShared];
        cmd = [q commandBuffer]; enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ka]; [enc setTexture:tt atIndex:0]; [enc setTexture:at atIndex:1]; [enc setBuffer:tb offset:0 atIndex:0]; [enc setBuffer:out offset:0 atIndex:1];
        [enc dispatchThreads:MTLSizeMake(2 * W, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
        const uint32_t *o = out.contents; int ok = ka && cmd.status == MTLCommandBufferStatusCompleted;
        for (int i = 0; i < W; i++) ok &= o[i] == 500u + (uint32_t)i && o[W + i] == 700u + (uint32_t)i;
        CHECK(ok, "an array of buffer textures reads each element's own elements, each by its own entry");
    }
    mtl_pool_del(mem);
    printf("t_buffer_image_bias: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
