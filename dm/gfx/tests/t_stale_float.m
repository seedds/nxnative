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
static const uint32_t F32_ONE = 0x3f800000u, F32_INF = 0x7f800000u, F32_NINF = 0xff800000u;
static int rg11b10_nan3(uint32_t v) { return ((v >> 6) & 31) == 31 && ((v >> 17) & 31) == 31 && ((v >> 27) & 31) == 31; }
// Resolve a texture whose pool holds `storage` (t.L.size bytes) and read back every addressable level, layer, and slice into `out`.
static void readback_storage(const nvn_texdesc *desc, size_t bpp, int pool_flags, const void *storage, uint64_t serial, void *out) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .d = *desc};
    nvn_tex_layout(&t.d, &t.L);
    size_t psz = MAX((size_t)65536, ((size_t)t.L.size + 4095) & ~(size_t)4095);
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), psz)) abort(); memset(mem, 0, psz);
    memcpy(mem, storage, (size_t)t.L.size);
    int cpu_vis = !(pool_flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
    pool_t *pool = calloc(1, sizeof *pool); *pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = psz, .flags = pool_flags};
    mtl_pool_add(mem, psz);
    if (cpu_vis) nvn_dirty_add_range(mem, psz);
    t.pool = pool; t.mem = mem;
    mtl_tex_init(&t); mtl_sub_begin(); id<MTLTexture> image = tex_mtl(&t);
    CHECK("texture resolves", image != nil); finish();
    unsigned levels = image ? (unsigned)image.mipmapLevelCount : (t.d.levels ? t.d.levels : 1);
    unsigned layers = t.L.layers ? t.L.layers : 1;
    size_t total = 0;
    for (unsigned l = 0; l < levels; l++) {
        uint32_t w, h, z; tex_lsize(&t, l, &w, &h, &z);
        total += (size_t)w * h * z * layers * bpp;
    }
    memset(out, 0xab, total);
    if (image) {
        static id<MTLCommandQueue> queue; if (!queue) queue = [g_dev newCommandQueue];
        id<MTLBuffer> b = [g_dev newBufferWithLength:total options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        size_t off = 0;
        for (unsigned l = 0; l < levels; l++) {
            uint32_t w, h, z; tex_lsize(&t, l, &w, &h, &z);
            for (unsigned y = 0; y < layers; y++) {
                [e copyFromTexture:image sourceSlice:y sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, z)
                    toBuffer:b destinationOffset:off destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
                off += (size_t)w * h * z * bpp;
            }
        }
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK("readback completes", cb.status == MTLCommandBufferStatusCompleted);
        memcpy(out, b.contents, total);
    }
    mtl_tex_release(&t); mtl_drain(); mtl_alias_drop_pool(pool);
    if (cpu_vis) nvn_dirty_remove_range(mem);
    mtl_pool_del(mem); mtl_drain(); free(pool); free(mem);
}
// Each texel's bits after the texture is resolved from storage holding `linear` (W x H texels of bpp bytes).
static void readback(int format, size_t bpp, int pool_flags, int tex_flags, const void *linear, uint64_t serial, void *out) {
    nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D, .format = format, .flags = tex_flags, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1};
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint8_t *st = calloc(1, (size_t)L.size);
    CHECK("fixture swizzle succeeds", !nvn_swizzle(&d, 0, 0, linear, W * bpp, st));
    readback_storage(&d, bpp, pool_flags, st, serial, out);
    free(st);
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

    // 10. 1x1 R32F block-linear (512-byte GOB): +Inf in padding (offset 4 and all padding) preserves 1.0; +Inf in the real texel zeroes by default and is kept with DM_NVN_STALE_FLOAT=1
    {
        nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F, .width = 1, .height = 1, .depth = 1, .levels = 1, .samples = 1};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        CHECK("1x1 R32F block-linear occupies 512 bytes", L.size == 512);
        uint32_t st[128] = {0}, px = 0;
        st[0] = F32_ONE; st[1] = F32_INF;   // byte offset 4, immediately after texel (0,0)
        readback_storage(&d, 4, no_cpu, st, 9859, &px);
        CHECK("1x1 R32F +Inf at padding offset 4 preserves 1.0", px == F32_ONE);
        for (size_t i = 1; i < 128; i++) st[i] = F32_INF;
        readback_storage(&d, 4, no_cpu, st, 9860, &px);
        CHECK("1x1 R32F +Inf across all 508 padding bytes preserves 1.0", px == F32_ONE);
        memset(st, 0, sizeof st); st[0] = F32_INF;
        readback_storage(&d, 4, no_cpu, st, 9861, &px);
        CHECK("1x1 R32F +Inf in real texel zeroes by default (kept under DM_NVN_STALE_FLOAT=1)", keep ? px == F32_INF : px == 0);
    }

    // 11. Multi-level 2D R32F (16x16, 5 mips down to 1x1): padding-only +Inf across all mips preserves 1.0; real +Inf in the 1x1 bottom mip zeroes whole texture
    {
        nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F, .width = 16, .height = 16, .depth = 1, .levels = 5, .samples = 1};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        enum { N = 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1 };
        uint32_t *st = malloc((size_t)L.size), lin[16 * 16], got[N];
        for (size_t i = 0; i < L.size / 4; i++) st[i] = F32_INF;
        for (int i = 0; i < 16 * 16; i++) lin[i] = F32_ONE;
        for (unsigned l = 0; l < 5; l++) {
            uint32_t w = 16u >> l;
            CHECK("mip swizzle succeeds", !nvn_swizzle(&d, l, 0, lin, w * 4, st));
        }
        readback_storage(&d, 4, no_cpu, st, 9862, got);
        ok = 1; for (int i = 0; i < N; i++) ok &= got[i] == F32_ONE;
        CHECK("multi-level R32F with +Inf in all mip/GOB padding preserves 1.0", ok);
        memset(st, 0, (size_t)L.size);
        for (unsigned l = 0; l < 5; l++) {
            uint32_t w = 16u >> l;
            lin[0] = (l == 4) ? F32_INF : F32_ONE;
            CHECK("mip swizzle with real +Inf succeeds", !nvn_swizzle(&d, l, 0, lin, w * 4, st));
        }
        readback_storage(&d, 4, no_cpu, st, 9863, got);
        ok = 1; for (int i = 0; i < N; i++) ok &= keep ? (i == N - 1 ? got[i] == F32_INF : got[i] == F32_ONE) : got[i] == 0;
        CHECK("multi-level R32F real +Inf in level 4 zeroes whole texture (kept under DM_NVN_STALE_FLOAT=1)", ok);
        free(st);
    }

    // 12. 2D array RGBA16F (5x3 x 3 layers, 2 levels): NaN in row/GOB/layer padding preserves finite texels; real NaN in layer 2 level 1 zeroes whole texture
    {
        nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA16F, .width = 5, .height = 3, .depth = 3, .levels = 2, .samples = 1};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        enum { NCH = (5 * 3 + 2 * 1) * 3 * 4 };
        uint16_t *st = malloc((size_t)L.size), lin[5 * 3 * 4], got[NCH];
        for (size_t i = 0; i < L.size / 2; i++) st[i] = HALF_NAN;
        for (int i = 0; i < 5 * 3 * 4; i++) lin[i] = HALF_ONE;
        for (unsigned l = 0; l < 2; l++) for (unsigned y = 0; y < 3; y++) {
            uint32_t w = l ? 2u : 5u;
            CHECK("array swizzle succeeds", !nvn_swizzle(&d, l, y, lin, w * 8, st));
        }
        readback_storage(&d, 8, no_cpu, st, 9864, got);
        ok = 1; for (int i = 0; i < NCH; i++) ok &= got[i] == HALF_ONE;
        CHECK("2D array RGBA16F with NaN in GOB/layer padding preserves finite texels", ok);
        memset(st, 0, (size_t)L.size);
        for (unsigned l = 0; l < 2; l++) for (unsigned y = 0; y < 3; y++) {
            uint32_t w = l ? 2u : 5u;
            for (int i = 0; i < 5 * 3 * 4; i++) lin[i] = HALF_ONE;
            if (l == 1 && y == 2) lin[4 * 1 + 3] = HALF_NAN;   // texel (1,0) alpha in layer 2 level 1
            CHECK("array swizzle with real NaN succeeds", !nvn_swizzle(&d, l, y, lin, w * 8, st));
        }
        readback_storage(&d, 8, no_cpu, st, 9865, got);
        int bad_idx = 5 * 3 * 3 * 4 + 2 * (2 * 1 * 4) + 4 * 1 + 3;
        ok = 1; for (int i = 0; i < NCH; i++) ok &= keep ? (i == bad_idx ? (got[i] & 0x7c00) == 0x7c00 : got[i] == HALF_ONE) : got[i] == 0;
        CHECK("2D array RGBA16F real NaN in layer 2 level 1 zeroes whole texture (kept under DM_NVN_STALE_FLOAT=1)", ok);
        free(st);
    }

    // 13. 3D RG11B10F (5x3x3, 2 levels): NaN in slice/depth-block/GOB padding preserves finite voxels; real NaN in slice 2 zeroes whole volume
    {
        nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_R11G11B10F, .width = 5, .height = 3, .depth = 3, .levels = 2, .samples = 1};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        enum { N = 5 * 3 * 3 + 2 * 1 * 1 };
        uint32_t *st = malloc((size_t)L.size), lin[5 * 3 * 3], got[N];
        for (size_t i = 0; i < L.size / 4; i++) st[i] = RG11B10_NAN;
        for (int i = 0; i < 5 * 3 * 3; i++) lin[i] = RG11B10_ONE;
        CHECK("3D level 0 swizzle succeeds", !nvn_swizzle(&d, 0, 0, lin, 5 * 4, st));
        CHECK("3D level 1 swizzle succeeds", !nvn_swizzle(&d, 1, 0, lin, 2 * 4, st));
        readback_storage(&d, 4, no_cpu, st, 9866, got);
        ok = 1; for (int i = 0; i < N; i++) ok &= got[i] == RG11B10_ONE;
        CHECK("3D RG11B10F with NaN in slice/GOB padding preserves finite voxels", ok);
        memset(st, 0, (size_t)L.size);
        lin[2 * 15 + 1 * 5 + 3] = RG11B10_NAN;   // slice 2, (3, 1)
        CHECK("3D level 0 swizzle with real NaN succeeds", !nvn_swizzle(&d, 0, 0, lin, 5 * 4, st));
        lin[2 * 15 + 1 * 5 + 3] = RG11B10_ONE;
        CHECK("3D level 1 swizzle succeeds", !nvn_swizzle(&d, 1, 0, lin, 2 * 4, st));
        readback_storage(&d, 4, no_cpu, st, 9867, got);
        int bad_idx = 2 * 15 + 1 * 5 + 3;
        ok = 1; for (int i = 0; i < N; i++) ok &= keep ? (i == bad_idx ? rg11b10_nan3(got[i]) : got[i] == RG11B10_ONE) : got[i] == 0;
        CHECK("3D RG11B10F real NaN in slice 2 zeroes whole volume (kept under DM_NVN_STALE_FLOAT=1)", ok);
        free(st);
    }

    // 14. Switch 2 GOB order (gob_order = 1): 5x5 R32F with +Inf in padding preserves 1.0; +Inf at texel (4, 2) (where X1 and Switch 2 GOB offsets differ) zeroes whole texture
    {
        nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F, .width = 5, .height = 5, .depth = 1, .levels = 1, .samples = 1, .gob_order = 1};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        uint32_t *st = malloc((size_t)L.size), lin[25], got[25];
        for (size_t i = 0; i < L.size / 4; i++) st[i] = F32_INF;
        for (int i = 0; i < 25; i++) lin[i] = F32_ONE;
        CHECK("Switch 2 GOB swizzle succeeds", !nvn_swizzle(&d, 0, 0, lin, 5 * 4, st));
        readback_storage(&d, 4, no_cpu, st, 9868, got);
        ok = 1; for (int i = 0; i < 25; i++) ok &= got[i] == F32_ONE;
        CHECK("Switch 2 GOB order 5x5 R32F padding-only +Inf preserves 1.0", ok);
        memset(st, 0, (size_t)L.size);
        lin[2 * 5 + 4] = F32_INF;
        CHECK("Switch 2 GOB swizzle with real +Inf succeeds", !nvn_swizzle(&d, 0, 0, lin, 5 * 4, st));
        readback_storage(&d, 4, no_cpu, st, 9869, got);
        ok = 1; for (int i = 0; i < 25; i++) ok &= keep ? (i == 2 * 5 + 4 ? got[i] == F32_INF : got[i] == F32_ONE) : got[i] == 0;
        CHECK("Switch 2 GOB order 5x5 R32F real +Inf at (4,2) zeroes whole texture (kept under DM_NVN_STALE_FLOAT=1)", ok);
        free(st);
    }

    // 15. Linear render-target fallback R32F (1x2, LINEAR_RENDER_TARGET, stride 20 unaligned to Metal's linear pitch alignment so tex_init_fill runs): row padding +Inf preserves 1.0; real +Inf in row 1 zeroes texture
    {
        nvn_texdesc d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F, .flags = NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, .width = 1, .height = 2, .depth = 1, .levels = 1, .samples = 1, .stride = 20};
        uint32_t st[10], got[2];
        for (int i = 0; i < 10; i++) st[i] = F32_INF;
        st[0] = F32_ONE; st[5] = F32_ONE;   // row 0 byte 0, row 1 byte 20; bytes 4..19 and 24..39 are padding
        readback_storage(&d, 4, no_cpu, st, 9870, got);
        CHECK("linear fallback R32F row-padding +Inf preserves 1.0", got[0] == F32_ONE && got[1] == F32_ONE);
        memset(st, 0, sizeof st); st[0] = F32_ONE; st[5] = F32_INF;
        readback_storage(&d, 4, no_cpu, st, 9871, got);
        CHECK("linear fallback R32F real +Inf in row 1 zeroes texture (kept under DM_NVN_STALE_FLOAT=1)",
              keep ? (got[0] == F32_ONE && got[1] == F32_INF) : (got[0] == 0 && got[1] == 0));
    }

    printf("t_stale_float: %d checks, %d failed (DM_NVN_STALE_FLOAT=%d)\n", checks, failed, keep);
    return failed != 0;
} }
