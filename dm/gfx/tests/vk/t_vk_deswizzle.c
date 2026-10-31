// The GPU deswizzle of block-linear textures against the CPU one (docs/design/vulkan-backend.md 4.3, 5.3; dm/gfx/hostvk/shaders/deswizzle.comp against dm/gfx/tex/nvn_deswizzle.c):
// for each case (a format of 1 to 16 bytes a texel or block, compressed ones included, a size, a block height and depth, levels, layers or slices), random texels are swizzled
// into two packaged textures' storage, one uploaded through the GPU path (DM_VK_DESWIZZLE=gpu) and one through the CPU path (cpu); every level and layer of both images is read
// back and must equal the texels written, bit for bit. One case runs its GPU upload in bands of 64 KiB (the scratch reused band after band), one is a linear texture (the GPU
// path's direct copy from the pool at the texture's stride). Then a 2048 x 2048 RGBA8 texture (16 MiB) is uploaded both ways and timed: lavapipe runs the compute pass on the
// CPU, so the numbers are a first data point of the two paths' cost on this machine, not a GPU measurement.
// Checks: two per case (the GPU and the CPU image equal the texels), the GPU path taken and the CPU path taken, the two timings' readbacks, and the validation check
#include "t_vk_harness.h"
extern __typeof__(*(NVNT_nvnTextureBuilderSetStride)0) h_nvnTextureBuilderSetStride;

static hv_pool g_p; static size_t g_off = 0;
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)g_rng; }
static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
// Level / layer (or 3D slices) of a texture's image into out: rows of blocks, tight, slice after slice
static int read_sub(const tex_t *t, unsigned level, unsigned layer, uint32_t w, uint32_t h, uint32_t d, int bw, int bh, int bpb, uint8_t *out) {
    VkFormat f; VkImageAspectFlags a; VkImage img = hvk_test_image(t, &f, &a);
    if (!img) return 0;
    size_t bytes = (size_t)((w + bw - 1) / bw) * (size_t)bpb * ((h + bh - 1) / bh) * d;
    VkBuffer b; VkDeviceMemory m; void *map;
    if (hvk_buffer_make((VkDeviceSize)bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &b, &m, &map)) return 0;
    VkCommandBuffer cb; int ok = 0;
    if (hvk_oneoff_begin(&cb)) {
        hvk_full_barrier(cb);
        VkBufferImageCopy r = {0, 0, 0, {a, level, layer, 1}, {0, 0, 0}, {w, h, d}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
        hvk_full_barrier(cb);
        if ((ok = hvk_oneoff_end(cb))) memcpy(out, map, bytes);
    }
    hvk_buffer_free(b, m);
    return ok;
}
typedef struct { int target, fmt; uint32_t w, h, depth, levels; int lbh, lbd; const char *what; } dcase;
// Both textures of a case made, filled, uploaded (GPU path, CPU path) and read back against the texels written; returns 1 when both match
static int run_case(const dcase *c, int gpu_band, double *ms_gpu, double *ms_cpu) {
    int bw, bh, bpb = nvn_fmt_bpb(c->fmt, &bw, &bh), is3d = c->target == NVN_TEXTURE_TARGET_3D;
    uint32_t layers = is3d ? 1 : c->depth;
    NVNtexture tex[2];
    for (int k = 0; k < 2; k++) hv_texture(&tex[k], &g_p, &g_off, c->target, c->fmt, c->w, c->h, c->depth, c->levels, 0, c->lbh, c->lbd);
    size_t total = 0;
    for (unsigned l = 0; l < c->levels; l++) total += (size_t)((mipd(c->w, l) + bw - 1) / bw) * bpb * ((mipd(c->h, l) + bh - 1) / bh) * (is3d ? mipd(c->depth, l) : 1) * layers;
    uint8_t *src = malloc(total), *got = malloc(total);
    if (!src || !got) exit(2);
    for (size_t i = 0; i < total; i++) src[i] = (uint8_t)rnd();
    size_t o = 0;
    for (unsigned l = 0; l < c->levels; l++)
        for (unsigned z = 0; z < layers; z++) {
            uint32_t w = mipd(c->w, l), h = mipd(c->h, l), d = is3d ? mipd(c->depth, l) : 1;
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, n = pitch * ((h + bh - 1) / bh) * d;
            for (int k = 0; k < 2; k++) if (!hv_tex_fill(&tex[k], l, z, src + o, pitch)) { fprintf(stderr, "%s: swizzle failed\n", c->what); exit(2); }
            o += n;
        }
    int ok = 1;
    for (int k = 0; k < 2; k++) {   // k 0: GPU, 1: CPU
        hvk_test_tex_opts(-1, k == 0 ? 1 : 2, -1, -1);
        if (k == 0 && gpu_band) hvk_test_band_bytes((uint32_t)gpu_band);
        uint64_t t0 = plat_now_ns();
        VkFormat f; VkImageAspectFlags a;
        if (!hvk_test_image((const tex_t *)(const void *)&tex[k], &f, &a)) { ok = 0; continue; }
        double ms = (double)(plat_now_ns() - t0) / 1e6;
        if (k == 0 && ms_gpu) *ms_gpu = ms;
        if (k == 1 && ms_cpu) *ms_cpu = ms;
        hvk_test_band_bytes(0);
        o = 0;
        int same = 1;
        for (unsigned l = 0; l < c->levels; l++)
            for (unsigned z = 0; z < layers; z++) {
                uint32_t w = mipd(c->w, l), h = mipd(c->h, l), d = is3d ? mipd(c->depth, l) : 1;
                size_t n = (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * d;
                if (!read_sub((const tex_t *)(const void *)&tex[k], l, z, w, h, d, bw, bh, bpb, got + o)) same = 0;
                else if (memcmp(got + o, src + o, n)) {
                    size_t i = 0; while (i < n && got[o + i] == src[o + i]) i++;
                    if (same) fprintf(stderr, "%s, %s path: level %u layer %u differs at byte %zu of %zu (%02x, written %02x)\n", c->what, k ? "CPU" : "GPU", l, z, i, n, got[o + i], src[o + i]);
                    same = 0;
                }
                o += n;
            }
        char name[256]; snprintf(name, sizeof name, "%s: the %s upload holds the texels written, bit for bit (%zu bytes)", c->what, k ? "CPU" : "GPU", total);
        t(name, same);
        ok &= same;
    }
    hvk_test_tex_opts(-1, 0, -1, -1);
    free(src); free(got);
    return ok;
}

int main(void) {
    const hostvk_dev *dev = hv_up("t_vk_deswizzle");
    hv_device(0);
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    if (!hv_pool_make(&g_p, 64u << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    dcase cases[] = {
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R8, 1, 1, 1, 1, 0, 0, "R8 1x1"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R8, 37, 23, 1, 3, 1, 0, "R8 37x23, 3 levels, block height 2"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RG8, 64, 64, 1, 4, 2, 0, "RG8 64x64, 4 levels, block height 4"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGB565, 50, 50, 1, 2, 3, 0, "RGB565 50x50, 2 levels, block height 8"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 300, 200, 1, 5, 4, 0, "RGBA8 300x200, 5 levels, block height 16"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA16F, 129, 65, 1, 2, 3, 0, "RGBA16F 129x65, 2 levels, block height 8"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA32F, 80, 48, 1, 1, 5, 0, "RGBA32F 80x48, block height 32"},
        {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 70, 40, 3, 2, 2, 0, "RGBA8 70x40, 3 layers, 2 levels"},
        {NVN_TEXTURE_TARGET_CUBEMAP, NVN_FORMAT_R32F, 32, 32, 6, 3, 2, 0, "R32F cube 32x32, 3 levels"},
        {NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, 33, 17, 9, 2, 1, 1, "RGBA8 3D 33x17x9, 2 levels, block depth 2"},
        {NVN_TEXTURE_TARGET_3D, NVN_FORMAT_R16, 64, 32, 16, 1, 2, 2, "R16 3D 64x32x16, block depth 4"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGB_DXT1, 256, 128, 1, 3, 3, 0, "BC1 256x128, 3 levels"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_DXT5, 100, 60, 1, 2, 2, 0, "BC3 100x60 (partial blocks), 2 levels"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BPTC_UNORM, 132, 68, 1, 2, 4, 0, "BC7 132x68, 2 levels"},
        {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA_ASTC_6x6, 96, 60, 2, 2, 2, 0, "ASTC 6x6 held compressed 96x60, 2 layers, 2 levels"},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_ASTC_10x8, 250, 130, 1, 3, 3, 0, "ASTC 10x8 held compressed 250x130, 3 levels"},
    };
    int n = (int)(sizeof cases / sizeof *cases), astc_native = dev->astc_ldr;
    uint64_t up0[3], up1[3]; hvk_test_tex_mem(NULL, up0);
    for (int i = 0; i < n; i++) {
        int f = cases[i].fmt;
        if (f >= NVN_FORMAT_RGBA_ASTC_4x4 && f <= NVN_FORMAT_RGBA_ASTC_12x12_SRGB && !astc_native) { printf("note: %s skipped: the device has no ASTC\n", cases[i].what); continue; }
        hvk_test_tex_opts(0, -1, -1, -1);   // ASTC held compressed: its blocks are what the deswizzle moves
        run_case(&cases[i], 0, NULL, NULL);
    }
    dcase banded = {NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 200, 150, 2, 3, 4, 0, "RGBA8 200x150, 2 layers, 3 levels, GPU bands of 64 KiB"};
    run_case(&banded, 64 << 10, NULL, NULL);
    dcase banded3 = {NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA16F, 64, 64, 12, 1, 2, 1, "RGBA16F 3D 64x64x12, GPU bands of 64 KiB (rows of one slice)"};
    run_case(&banded3, 64 << 10, NULL, NULL);
    hvk_test_tex_mem(NULL, up1);
    char name[200];
    snprintf(name, sizeof name, "the GPU path uploaded subresources (%llu) and so did the CPU path (%llu)", (unsigned long long)(up1[0] - up0[0]), (unsigned long long)(up1[1] - up0[1]));
    t(name, up1[0] > up0[0] && up1[1] > up0[1]);
    // A linear texture: the GPU path is a copy from the pool at the texture's stride
    {
        NVNtexture lt[2]; uint32_t w = 100, h = 30, stride = 512;
        for (int k = 0; k < 2; k++) {
            NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, &g_dev); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
            h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetSize2D(&tb, (int)w, (int)h); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR);
            h_nvnTextureBuilderSetStride(&tb, stride);
            g_off = (g_off + 4095) & ~(size_t)4095;
            h_nvnTextureBuilderSetStorage(&tb, &g_p.pool, (ptrdiff_t)g_off); h_nvnTextureInitialize(&lt[k], &tb);
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w * 4; x++) g_p.mem[g_off + y * stride + x] = (uint8_t)(x * 7 + y * 13 + k * 0);
            g_off += (size_t)stride * h;
        }
        int ok = 1; uint8_t *got = malloc(w * h * 4);
        for (int k = 0; k < 2; k++) {
            hvk_test_tex_opts(-1, k == 0 ? 1 : 2, -1, -1);
            const tex_t *tt = (const tex_t *)(const void *)&lt[k];
            int same = read_sub(tt, 0, 0, w, h, 1, 1, 1, 4, got);
            for (uint32_t y = 0; same && y < h; y++) same = !memcmp(got + (size_t)y * w * 4, tt->mem + (size_t)y * stride, w * 4);
            snprintf(name, sizeof name, "linear RGBA8 100x30 at a 512-byte stride: the %s upload holds its rows", k ? "CPU" : "GPU"); t(name, same);
            ok &= same;
        }
        free(got); (void)ok;
        hvk_test_tex_opts(-1, 0, -1, -1);
    }
    // The timing: 2048 x 2048 RGBA8, each path from a fresh texture to its upload completed (hvk_test_image: one submission, waited for)
    double ms_gpu = 0, ms_cpu = 0;
    dcase big = {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 2048, 2048, 1, 1, 4, 0, "RGBA8 2048x2048 (16 MiB), timed"};
    run_case(&big, 0, &ms_gpu, &ms_cpu);
    printf("deswizzle of 16 MiB on %s: GPU path %.1f ms (%.0f MiB/s), CPU path %.1f ms (%.0f MiB/s); lavapipe runs the compute pass on the CPU: not a GPU measurement\n",
           dev->props.deviceName, ms_gpu, 16.0 / (ms_gpu / 1000), ms_cpu, 16.0 / (ms_cpu / 1000));
    hv_pool_free(&g_p);
    (void)q;
    return hv_finish();
}
