// Floating-point textures over stale storage (storage_nonfinite / tex_init_fill in nvn_mtl.m). A render target placed in a CPU_NO_ACCESS pool whose storage still holds other
// data starts as that data; where it decodes as NaN or infinity in more than 1 % of its texels it is not a picture the game loaded, and the texture starts from zeros instead
// (Splatoon 3's lobby probe array: Metal blends NaN x 0 = NaN where NVIDIA's float blender gives 0). Finite data is still the content; where 1 % or fewer of the texels are
// non-finite, those alone start from 0 (another run of the same game: 14 NaN texels in the probe array spread as black and red blocks). DM_NVN_STALE_FLOAT=1 keeps all of it.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
static void finish(void) { CHECK("submission completes",mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK("GPU has no error",!mtl_last_commit_error()); }
enum { W = 16, H = 16 };
static const uint32_t RG11B10_ONE = (15u << 6) | (15u << 17) | (15u << 27), RG11B10_NAN = 0xffffffffu;
static const uint16_t HALF_ONE = 0x3c00, HALF_NAN = 0xffff;
static int rg11b10_nan3(uint32_t v) { return ((v >> 6) & 31) == 31 && ((v >> 17) & 31) == 31 && ((v >> 27) & 31) == 31; }
// Each texel's bits after the texture is made from storage holding `linear` (W x H texels of bpp bytes)
static void readback(int format, size_t bpp, const void *linear, uint64_t serial, void *out) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 65536)) abort(); memset(mem, 0, 65536);
    // A pool object and storage of its own per run (never reused): the storage-sharing table keys a texture by its pool and offset
    pool_t *pool = calloc(1, sizeof *pool); *pool = (pool_t){.magic = MAGIC('P','O','O','L'), .mem = mem, .size = 65536, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, 65536);
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .pool = pool, .mem = mem};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    CHECK("fixture swizzle succeeds", !nvn_swizzle(&t.d, 0, 0, linear, W * bpp, mem));
    mtl_tex_init(&t); mtl_sub_begin(); id<MTLTexture> image = tex_mtl(&t);
    CHECK("texture resolves", image != nil); finish();
    memset(out, 0xab, W * H * bpp);
    if (image) {
        static id<MTLCommandQueue> queue; if (!queue) queue = [g_dev newCommandQueue];
        id<MTLBuffer> b = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:b destinationOffset:0
            destinationBytesPerRow:W * bpp destinationBytesPerImage:W * H * bpp];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK("readback completes", cb.status == MTLCommandBufferStatusCompleted);
        memcpy(out, b.contents, W * H * bpp);
    }
    mtl_tex_release(&t); mtl_drain();
}
int main(void) { @autoreleasepool { mtl_init(); const char *e = getenv("DM_NVN_STALE_FLOAT"); int keep = e && atoi(e) == 1;
    uint32_t in[W * H], out[W * H]; uint16_t hin[W * H * 4], hout[W * H * 4];
    // Stale storage: half the texels decode as NaN; the texture starts from zeros
    for (int i = 0; i < W * H; i++) in[i] = i & 1 ? RG11B10_NAN : RG11B10_ONE;
    readback(NVN_FORMAT_R11G11B10F, 4, in, 9850, out);
    int zero = 1; for (int i = 0; i < W * H; i++) zero &= out[i] == 0;
    CHECK("RG11B10 storage half NaN starts from zeros", zero);
    // Finite data is the content
    for (int i = 0; i < W * H; i++) in[i] = RG11B10_ONE;
    readback(NVN_FORMAT_R11G11B10F, 4, in, 9851, out);
    int same = 1; for (int i = 0; i < W * H; i++) same &= out[i] == RG11B10_ONE;
    CHECK("finite RG11B10 storage is the content", same);
    // One non-finite texel in 256 (under 1 %): that texel starts from 0, the rest is the content
    in[37] = RG11B10_NAN;
    readback(NVN_FORMAT_R11G11B10F, 4, in, 9852, out);
    // (with DM_NVN_STALE_FLOAT=1 the NaN stays; Metal's upload makes it canonical, each channel's exponent all ones)
    same = 1; for (int i = 0; i < W * H; i++) same &= i == 37 ? (keep ? rg11b10_nan3(out[i]) : out[i] == 0) : out[i] == RG11B10_ONE;
    CHECK("a rare non-finite texel starts from 0, the rest is the content", same);
    // RGBA16F: one NaN channel in 1024; only that channel starts from 0
    for (int i = 0; i < W * H * 4; i++) hin[i] = HALF_ONE;
    hin[4 * 9 + 2] = HALF_NAN;
    readback(NVN_FORMAT_RGBA16F, 8, hin, 9855, hout);
    same = 1; for (int i = 0; i < W * H * 4; i++) same &= i == 4 * 9 + 2 ? (keep ? (hout[i] & 0x7c00) == 0x7c00 : hout[i] == 0) : hout[i] == HALF_ONE;
    CHECK("a rare NaN channel starts from 0, its texel's other channels are the content", same);
    // RGBA16F: a quarter of the halves are NaN
    for (int i = 0; i < W * H * 4; i++) hin[i] = i % 4 == 3 ? HALF_NAN : HALF_ONE;
    readback(NVN_FORMAT_RGBA16F, 8, hin, 9853, hout);
    zero = 1; for (int i = 0; i < W * H * 4; i++) zero &= hout[i] == 0;
    CHECK("RGBA16F storage a quarter NaN starts from zeros", zero);
    for (int i = 0; i < W * H * 4; i++) hin[i] = HALF_ONE;
    readback(NVN_FORMAT_RGBA16F, 8, hin, 9854, hout);
    same = 1; for (int i = 0; i < W * H * 4; i++) same &= hout[i] == HALF_ONE;
    CHECK("finite RGBA16F storage is the content", same);
    printf("t_stale_float: %d checks, %d failed\n", checks, failed); return failed != 0; } }
