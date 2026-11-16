// A texture the CPU fills, uploaded twice into one submission's prologue (vk_texture.c upload: the CPU wrote it again between two draws of the submission, as Mario vs. Donkey
// Kong's glyph texture, a linear R8 2048 x 2048 one, is written while its submission is replayed). Every upload of the prologue runs before the submission's draws, so the
// later upload must land after the earlier one: without a barrier between them the two transfer writes are unordered (the validation layer's SYNC-HAZARD-WRITE-AFTER-WRITE)
// and the image may keep the older content. Cases: a linear texture through the GPU path (a copy straight from the pool, the game's case) and a packaged one through the CPU
// path (the deswizzled texels staged at each upload), each written with one pattern, uploaded, written with another and uploaded again in the same submission
// (hvk_test_reupload stands for the CPU's write between two draws). No Metal counterpart. Checks: the image holds the second pattern, the second upload was counted as one in
// the same prologue, and the validation check (synchronization validation)
#include "t_vk_harness.h"
extern __typeof__(*(NVNT_nvnTextureBuilderSetStride)0) h_nvnTextureBuilderSetStride;
typedef struct hvk_tex hvk_tex;
hvk_tex *hvk_tex_get(const tex_t *t);
void hvk_sub_begin(void);
uint64_t hvk_sub_commit(void);
void hvk_finish(void);
void hvk_test_reupload(int on);
void hvk_test_tex_opts(int astc_decode, int deswizzle, int d24_d32f, int force_alt);

enum { W = 256, H = 64, STRIDE = 1024 };
static void pattern(uint8_t *row0, size_t stride, int k) {   // pattern k into W x H RGBA8 texels, rows stride bytes apart
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W * 4; x++) row0[y * stride + x] = (uint8_t)(x * 7 + y * 13 + k * 101);
}
int main(void) {
    hv_up("t_vk_reupload");
    hv_device(0);
    hv_pool p; if (!hv_pool_make(&p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    size_t off = 0;
    uint8_t *want = malloc((size_t)W * H * 4), *got = malloc((size_t)W * H * 4);
    if (!want || !got) return 2;
    for (int c = 0; c < 2; c++) {   // 0: linear, GPU path; 1: packaged (block height 1 GOB), CPU path
        NVNtexture tex; const tex_t *tt = (const tex_t *)(const void *)&tex;
        if (c == 0) {
            NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, &g_dev); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
            h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetSize2D(&tb, W, H); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR);
            h_nvnTextureBuilderSetStride(&tb, STRIDE);
            off = (off + 4095) & ~(size_t)4095;
            h_nvnTextureBuilderSetStorage(&tb, &p.pool, (ptrdiff_t)off); h_nvnTextureInitialize(&tex, &tb);
            off += (size_t)STRIDE * H;
            hvk_test_tex_opts(-1, 1, -1, -1);   // DM_VK_DESWIZZLE=gpu: the linear texture's copy from the pool
        } else {
            hv_texture(&tex, &p, &off, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, W, H, 1, 1, 0, 0, 0);
            hvk_test_tex_opts(-1, 2, -1, -1);   // DM_VK_DESWIZZLE=cpu
        }
        uint8_t lin[W * 4 * H];
        for (int k = 0; k < 2; k++) {   // the first pattern, then the second (the second write faults on the storage the first upload protected: the dirty tracker's hook)
            if (c == 0) pattern(tt->mem, STRIDE, k);
            else { pattern(lin, W * 4, k); if (!hv_tex_fill(&tex, 0, 0, lin, W * 4)) { t("swizzle", 0); return hv_finish(); } }
            if (k == 0) { hvk_sub_begin(); hvk_test_reupload(1); }
            if (!hvk_tex_get(tt)) { t("texture made", 0); return hv_finish(); }
        }
        hvk_test_reupload(0);
        hvk_sub_commit(); hvk_finish();
        pattern(want, W * 4, 1);
        int ok = hv_read_blocks(tt, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, W, H, 1, 1, 1, 4, got) && !memcmp(got, want, (size_t)W * H * 4);
        char name[200];
        snprintf(name, sizeof name, "%s: two uploads in one prologue, the image holds the second pattern", c == 0 ? "linear RGBA8, GPU path (a copy from the pool)" : "packaged RGBA8, CPU path");
        t(name, ok);
        hvk_test_tex_opts(-1, 0, -1, -1);
    }
    char res[1024]; be_res_stats(res, sizeof res);   // the backend's resource line: "uploads GPU <n> CPU <n> (<n> again in one prologue)"
    const char *q = strstr(res, " again in one prologue)"); unsigned long long again = 0;
    if (q) { while (q > res && q[-1] != '(') q--; again = strtoull(q, NULL, 10); }
    char name[200]; snprintf(name, sizeof name, "the second upload of each case counted as one in the same prologue (%llu)", again);
    t(name, again == 2);
    free(want); free(got);
    hv_pool_free(&p);
    return hv_finish();
}
