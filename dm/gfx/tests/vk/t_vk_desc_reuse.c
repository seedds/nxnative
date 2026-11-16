// Descriptor sets kept across frames (vk_pipeline.c set_take, hvk_pipeline_slot_reset; docs/reference/rendering.md, "Descriptor sets kept across frames"): a submission slot's
// sets go back to free lists per layout when the slot is reset and are rewritten when taken again, instead of a pool reset and new sets every frame. Three 4 x 4 RGBA8 textures
// of different texels are fetched in turn, each fetch a submission of its own, 24 times: three times round the backend's 8 slots, so every slot's sets are taken again by draws
// that bind another texture than the one they were written for. Checks: every fetch reads its own texture's texels (a set taken again holds what it was rewritten with, not
// what it held before), the backend took sets again (its counters: sets allocated from a pool stop growing after the first round), and the validation check
#include "t_vk_harness.h"
#include <math.h>
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/fetch_frag.h"
void hvk_desc_set_stats(uint64_t *made, uint64_t *reused);

static const char k_fetch[] = "[{\"kind\":\"tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"},"
                              "{\"kind\":\"samp_tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"}]";
enum { N = 4, ROUNDS = 24 };
int main(void) {
    hv_up("t_vk_desc_reuse");
    hv_device(0);
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    hv_pool p; if (!hv_pool_make(&p, 4u << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds c = {p.mem + 0x40000, 0, 0x10000};
    NVNprogram fetch;
    const hv_shader sf[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch", 1, HV_SPV(fetch_frag_spv), k_fetch, NULL}};
    hv_program(&fetch, &p, 0, sf, 2);
    NVNtexturePool tp; NVNsamplerPool sp;
    h_nvnTexturePoolInitialize(&tp, &p.pool, 0x10000, 256); h_nvnSamplerPoolInitialize(&sp, &p.pool, 0x30000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&sp, 1, &samp);
    size_t off = 0x50000;
    NVNtexture tex[3]; uint8_t texels[3][N * N * 4];
    for (int i = 0; i < 3; i++) {
        hv_texture(&tex[i], &p, &off, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, N, N, 1, 1, 0, 0, 0);
        off += 0x10000;
        for (int k = 0; k < N * N * 4; k++) texels[i][k] = (uint8_t)(i == 0 ? k * 5 + 7 : i == 1 ? 255 - k * 3 : k * 11 + 100);
        if (!hv_tex_fill(&tex[i], 0, 0, texels[i], N * 4)) { t("texture storage filled", 0); return hv_finish(); }
        h_nvnTexturePoolRegisterTexture(&tp, 1 + i, &tex[i], NULL);
    }
    uint64_t made0 = 0, again0 = 0, made8 = 0, again8 = 0, made = 0, again = 0;
    int bad = 0;
    for (int r = 0; r < ROUNDS; r++) {
        if (r == 8) hvk_desc_set_stats(&made8, &again8);   // after the first round of the 8 slots
        if (r == 0) hvk_desc_set_stats(&made0, &again0);
        const int i = r % 3;
        float out[N * N * 4];
        if (!hv_fetch(&q, &c, &fetch, &tp, &sp, 1 + i, NVN_FORMAT_RGBA32F, N, N, out)) { bad++; continue; }
        for (int k = 0; k < N * N * 4; k++) if (fabsf(out[k] - texels[i][k] / 255.0f) > 0.5f / 255.0f) { bad++; break; }
    }
    hvk_desc_set_stats(&made, &again);
    char name[200];
    snprintf(name, sizeof name, "each of %d fetches in turn from three textures reads its own texture's texels (%d wrong)", ROUNDS, bad);
    t(name, !bad);
    snprintf(name, sizeof name, "sets taken again from the slots' free lists: %llu (allocated from a pool: %llu in the first round, %llu after it)",
             (unsigned long long)(again - again0), (unsigned long long)(made8 - made0), (unsigned long long)(made - made8));
    t(name, again > again0 && made == made8);
    hv_pool_free(&p);
    return hv_finish();
}
