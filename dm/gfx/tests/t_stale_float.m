// Stale floating-point storage at first use (storage_has_nonfinite / tex_init_fill in nvn_mtl.m).
// A non-COMPRESSIBLE float texture placed in a CPU_NO_ACCESS pool over reused memory starts as those bytes on the Switch too; NVIDIA's float blender discards stale NaNs under a zero
// destination weight whereas Metal keeps NaN x 0 = NaN. Where first-use storage in a CPU_NO_ACCESS pool holds any NaN or infinite float element, tex_init_fill skips the fill so the
// texture starts from zeros instead. Finite storage keeps its content, and CPU-visible pools are untouched. DM_NVN_STALE_FLOAT=1 keeps all storage as-is.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no error", !mtl_last_commit_error()); }
enum { W = 16, H = 16 };
static const uint32_t RG11B10_ONE = (15u << 6) | (15u << 17) | (15u << 27), RG11B10_NAN = 0xffffffffu, RG11B10_RINF = 31u << 6;
static const uint16_t HALF_ONE = 0x3c00, HALF_NAN = 0xffff, HALF_INF = 0x7c00;
static const uint32_t F32_ONE = 0x3f800000u, F32_NINF = 0xff800000u;
static int rg11b10_nan3(uint32_t v) { return ((v >> 6) & 31) == 31 && ((v >> 17) & 31) == 31 && ((v >> 27) & 31) == 31; }
// Each texel's bits after the texture is resolved from storage holding `linear` (W x H texels of bpp bytes).
static void readback(int format, size_t bpp, int pool_flags, int tex_flags, const void *linear, uint64_t serial, void *out) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 65536)) abort(); memset(mem, 0, 65536);
    int cpu_vis = !(pool_flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
    pool_t *pool = calloc(1, sizeof *pool); *pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = 65536, .flags = pool_flags};
    mtl_pool_add(mem, 65536);
    if (cpu_vis) nvn_dirty_add_range(mem, 65536);
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .pool = pool, .mem = mem};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .flags = tex_flags, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1};
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
    mtl_tex_release(&t); mtl_drain(); mtl_alias_drop_pool(pool);
    if (cpu_vis) nvn_dirty_remove_range(mem);
    mtl_pool_del(mem); mtl_drain(); free(pool); free(mem);
}
int main(void) { @autoreleasepool {
    mtl_init();
    const char *e = getenv("DM_NVN_STALE_FLOAT"); int keep = e && atoi(e) == 1;
    const int no_cpu = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    const int cpu_unc = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    uint32_t in[W * H], out[W * H]; uint16_t hin[W * H * 4], hout[W * H * 4];

    // 1. RG11B10F half NaN in CPU_NO_ACCESS -> zeros by default, preserved with DM_NVN_STALE_FLOAT=1
    for (int i = 0; i < W * H; i++) in[i] = i & 1 ? RG11B10_NAN : RG11B10_ONE;
    readback(NVN_FORMAT_R11G11B10F, 4, no_cpu, 0, in, 9850, out);
    int ok = 1; for (int i = 0; i < W * H; i++) ok &= keep ? (i & 1 ? rg11b10_nan3(out[i]) : out[i] == RG11B10_ONE) : out[i] == 0;
    CHECK("RG11B10F half NaN starts from zeros (kept under DM_NVN_STALE_FLOAT=1)", ok);

    // 2. RG11B10F all finite in CPU_NO_ACCESS -> content in both modes
    for (int i = 0; i < W * H; i++) in[i] = RG11B10_ONE;
    readback(NVN_FORMAT_R11G11B10F, 4, no_cpu, 0, in, 9851, out);
    ok = 1; for (int i = 0; i < W * H; i++) ok &= out[i] == RG11B10_ONE;
    CHECK("finite RG11B10F storage keeps its content", ok);

    // 3. RG11B10F single NaN texel (1 of 256) in CPU_NO_ACCESS -> whole texture starts from zeros
    in[37] = RG11B10_NAN;
    readback(NVN_FORMAT_R11G11B10F, 4, no_cpu, 0, in, 9852, out);
    ok = 1; for (int i = 0; i < W * H; i++) ok &= keep ? (i == 37 ? rg11b10_nan3(out[i]) : out[i] == RG11B10_ONE) : out[i] == 0;
    CHECK("RG11B10F single NaN texel starts whole texture from zeros", ok);

    // 4. RG11B10F single +Inf channel in CPU_NO_ACCESS -> whole texture starts from zeros
    in[37] = RG11B10_ONE; in[99] = RG11B10_RINF;
    readback(NVN_FORMAT_R11G11B10F, 4, no_cpu, 0, in, 9853, out);
    ok = 1; for (int i = 0; i < W * H; i++) ok &= keep ? (i == 99 ? ((out[i] >> 6) & 31) == 31 : out[i] == RG11B10_ONE) : out[i] == 0;
    CHECK("RG11B10F single Inf channel starts whole texture from zeros", ok);

    // 5. RGBA16F single NaN channel (1 of 1024) in CPU_NO_ACCESS -> whole texture starts from zeros
    for (int i = 0; i < W * H * 4; i++) hin[i] = HALF_ONE;
    hin[4 * 9 + 2] = HALF_NAN;
    readback(NVN_FORMAT_RGBA16F, 8, no_cpu, 0, hin, 9854, hout);
    ok = 1; for (int i = 0; i < W * H * 4; i++) ok &= keep ? (i == 4 * 9 + 2 ? (hout[i] & 0x7c00) == 0x7c00 : hout[i] == HALF_ONE) : hout[i] == 0;
    CHECK("RGBA16F single NaN channel starts whole texture from zeros", ok);

    // 6. RGBA16F all finite in CPU_NO_ACCESS -> content in both modes
    for (int i = 0; i < W * H * 4; i++) hin[i] = HALF_ONE;
    readback(NVN_FORMAT_RGBA16F, 8, no_cpu, 0, hin, 9855, hout);
    ok = 1; for (int i = 0; i < W * H * 4; i++) ok &= hout[i] == HALF_ONE;
    CHECK("finite RGBA16F storage keeps its content", ok);

    // 7. R32F single -Inf in CPU_NO_ACCESS -> whole texture starts from zeros
    for (int i = 0; i < W * H; i++) in[i] = F32_ONE;
    in[200] = F32_NINF;
    readback(NVN_FORMAT_R32F, 4, no_cpu, 0, in, 9856, out);
    ok = 1; for (int i = 0; i < W * H; i++) ok &= keep ? (i == 200 ? out[i] == F32_NINF : out[i] == F32_ONE) : out[i] == 0;
    CHECK("R32F single -Inf starts whole texture from zeros", ok);

    // 8. R32F all finite in CPU_NO_ACCESS -> content in both modes
    for (int i = 0; i < W * H; i++) in[i] = F32_ONE;
    readback(NVN_FORMAT_R32F, 4, no_cpu, 0, in, 9857, out);
    ok = 1; for (int i = 0; i < W * H; i++) ok &= out[i] == F32_ONE;
    CHECK("finite R32F storage keeps its content", ok);

    // 9. CPU_UNCACHED RGBA16F (IMAGE keeps it on tex_init_fill rather than tex_cpu_content) -> NaN and Inf preserved in both modes
    for (int i = 0; i < W * H * 4; i++) hin[i] = HALF_ONE;
    hin[4 * 3 + 0] = HALF_NAN; hin[4 * 3 + 1] = HALF_INF;
    readback(NVN_FORMAT_RGBA16F, 8, cpu_unc, NVN_TEXTURE_FLAGS_IMAGE, hin, 9858, hout);
    ok = 1; for (int i = 0; i < W * H * 4; i++) ok &= i == 4 * 3 + 0 ? (hout[i] & 0x7c00) == 0x7c00 : i == 4 * 3 + 1 ? hout[i] == HALF_INF : hout[i] == HALF_ONE;
    CHECK("CPU_UNCACHED RGBA16F keeps NaN and Inf content in both modes", ok);

    printf("t_stale_float: %d checks, %d failed (DM_NVN_STALE_FLOAT=%d)\n", checks, failed, keep);
    return failed != 0;
} }
