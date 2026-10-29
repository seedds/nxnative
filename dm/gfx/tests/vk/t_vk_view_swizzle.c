// Channel mapping of registered textures as a shader samples them: the Vulkan port of dm/gfx/tests/t_view_swizzle.m. Each texture is registered in a texture pool (with a view
// or none), bound to fragment slot 0 with a sampler of the sampler pool, and fetched texel by texel into an RGBA32F target: the builder's swizzle composed with the view's and with
// the format's own mapping (RGBX's alpha one, the RGB565 family's channel order, dm/gfx/tex/nvn_fmt_vk.c), for textures the CPU filled (packaged storage, uploaded) and for
// RGB565-family textures the GPU cleared (they read as cleared). An image texture: its sampled view keeps alpha one, its storage view (a compute shader's imageStore) has no
// mapping and stores what was written. The Metal test's case of bits in "a texture Metal made" (Metal's channel order) and its view-cache identity checks are Metal's.
// Checks: 8 components per case, the image case's 8 stored bytes, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/fetch_frag.h"
#include "shaders/gen/store_comp.h"
#include <math.h>
static NVNqueue g_q;
static hv_cmds g_c;
static hv_pool g_p;
static NVNtexturePool g_tp; static NVNsamplerPool g_sp;
static NVNprogram g_fetch, g_store;
static int g_next = 1;   // texture IDs
static uint32_t g_off = 0x20000;   // texture storage
static int g_fail_case;
static const char k_fetch[] = "[{\"kind\":\"tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"},"
                              "{\"kind\":\"samp_tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"}]";
static const char k_store[] = "[{\"kind\":\"img\",\"reg\":0,\"name\":\"img\",\"class\":\"storage_image\",\"set\":3,\"binding\":0,\"dim\":\"2d\",\"sampled\":\"float\",\"rw\":true}]";

// A 2×1 texture of format, its storage at the next free offset (packaged: the CPU's bytes are its content), the builder's swizzle when given
static void make(NVNtexture *t, int format, const void *pixels, unsigned bytes, const int *builder, int flags) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, &g_dev); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&tb, format); h_nvnTextureBuilderSetSize2D(&tb, 2, 1); h_nvnTextureBuilderSetFlags(&tb, flags);
    if (builder) h_nvnTextureBuilderSetSwizzle(&tb, builder[0], builder[1], builder[2], builder[3]);
    if (pixels) { NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay); memcpy(g_p.mem + g_off, pixels, bytes); }
    h_nvnTextureBuilderSetStorage(&tb, &g_p.pool, g_off); g_off += 0x1000;
    h_nvnTextureInitialize(t, &tb);
}
// The two texels as the fragment shader reads them through texture ID id
static void read_check(int tc, int id, const float expected[8], float tol) {
    tex_t out = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA32F, 2, 1, 1, 1, 0);
    const tex_t *rt[1] = {&out};
    hv_rt(&g_c, 1, rt, NULL);
    p_set_pool tp = {(const dpool_t *)&g_tp}, sp = {(const dpool_t *)&g_sp}; hv_emit(&g_c, OP_SET_TEX_POOL, &tp, sizeof tp); hv_emit(&g_c, OP_SET_SAMP_POOL, &sp, sizeof sp);
    hv_bind(&g_c, &g_fetch, 3); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    p_bind_tex1 b = {1, 0, (uint64_t)(uint32_t)id | (uint64_t)1 << 20, nvn_dpool_base(&g_tp, 1), nvn_dpool_base(&g_sp, 0)}; hv_emit(&g_c, OP_BIND_TEX1, &b, sizeof b);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&g_q, &g_c);
    float actual[8] = {-9, -9, -9, -9, -9, -9, -9, -9};
    int read = hv_read(&out, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 2, 1, 16, actual);
    for (int i = 0; i < 8; i++) {
        int ok = read && fabsf(actual[i] - expected[i]) <= tol;
        if (!ok) { fprintf(stderr, "case %d component %d: got %g expected %g\n", tc, i, actual[i], expected[i]); g_fail_case = tc; }
        t("sampled channel matches NVN format and mapping", ok);
    }
}
static void swizzle_case(int tc, int format, int view_format, const void *pixels, unsigned bytes, const int *builder, const int *mapping, const float expected[8]) {
    NVNtexture t; make(&t, format, pixels, bytes, builder, 0);
    NVNtextureView nv; memset(&nv, 0, sizeof nv); view_t *v = (view_t *)(void *)&nv;
    if (mapping) { v->set_swizzle = 1; for (int c = 0; c < 4; c++) v->swz[c] = (uint8_t)mapping[c]; }
    if (view_format) { v->set_format = 1; v->format = (uint16_t)view_format; }
    int id = g_next++;
    h_nvnTexturePoolRegisterTexture(&g_tp, id, &t, mapping || view_format ? &nv : NULL);
    read_check(tc, id, expected, 0.001f);
}
static void cleared_case(int tc, int format) {   // what the GPU wrote into an RGB565-family texture reads as written: a clear to red reads red
    NVNtexture t; make(&t, format, NULL, 0, NULL, 0);
    p_clear_tex c; memset(&c, 0, sizeof c); c.tex = (const tex_t *)&t; c.mask = 15; c.r = (NVNcopyRegion){0, 0, 0, 2, 1, 1}; c.c.f[0] = 1; c.c.f[3] = 1;
    hv_emit(&g_c, OP_CLEAR_TEX, &c, sizeof c); hv_submit(&g_q, &g_c);
    int id = g_next++; h_nvnTexturePoolRegisterTexture(&g_tp, id, &t, NULL);
    read_check(tc, id, (const float[8]){1, 0, 0, 1, 1, 0, 0, 1}, 0.001f);
}
static void image_case(int tc) {   // an RGBX8 image: sampled with alpha one, stored through a view without the mapping
    NVNtexture tx; make(&tx, NVN_FORMAT_RGBX8, NULL, 0, NULL, NVN_TEXTURE_FLAGS_IMAGE);
    int tid = g_next++, iid = g_next++;
    h_nvnTexturePoolRegisterTexture(&g_tp, tid, &tx, NULL); h_nvnTexturePoolRegisterImage(&g_tp, iid, &tx, NULL);
    p_set_pool tp = {(const dpool_t *)&g_tp}; hv_emit(&g_c, OP_SET_TEX_POOL, &tp, sizeof tp);
    hv_bind(&g_c, &g_store, NVN_SHADER_STAGE_COMPUTE_BIT);
    p_bind_handles bh; memset(&bh, 0, sizeof bh); bh.stage = 5; bh.first = 0; bh.n = 1; bh.h[0] = (uint64_t)(iid + nvn_dpool_base(&g_tp, 1));
    hv_emit(&g_c, OP_BIND_IMG, &bh, offsetof(p_bind_handles, h) + 8);
    p_dispatch d = {1, 1, 1, 0, 0}; hv_emit(&g_c, OP_DISPATCH, &d, sizeof d);
    hv_submit(&g_q, &g_c);
    uint8_t px[8]; memset(px, 0, sizeof px);
    t("the stored texels are read back", hv_read((const tex_t *)&tx, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 2, 1, 4, px));
    static const int want[4] = {64, 128, 191, 32};
    for (int i = 0; i < 8; i++) t("the image write is stored as written (no channel mapping on the storage view)", abs((int)px[i] - want[i % 4]) <= 1);
    read_check(tc, tid, (const float[8]){px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, 1, px[4] / 255.f, px[5] / 255.f, px[6] / 255.f, 1}, 0.001f);
}

int main(void) {
    hv_up("t_vk_view_swizzle");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    g_c = (hv_cmds){g_p.mem + 0x8000, 0, 0x8000};
    const hv_shader sf[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch", 1, HV_SPV(fetch_frag_spv), k_fetch, NULL}};
    const hv_shader sc[1] = {{"store", 5, HV_SPV(store_comp_spv), k_store, NULL}};
    hv_program(&g_fetch, &g_p, 0, sf, 2); hv_program(&g_store, &g_p, 1024, sc, 1);
    h_nvnTexturePoolInitialize(&g_tp, &g_p.pool, 0x10000, 64); h_nvnSamplerPoolInitialize(&g_sp, &g_p.pool, 0x18000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&g_sp, 1, &samp);
    const uint16_t pixels[2] = {0x001f, 0xf800};
    const int rgb1[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_ONE};
    const int map[4] = {NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_ONE, NVN_TEXTURE_SWIZZLE_ZERO};
    const int aaaa[4] = {NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_A};
    const float rgb[8] = {1, 0, 0, 1, 0, 0, 1, 1}, bgr[8] = {0, 0, 1, 1, 1, 0, 0, 1}, composed[8] = {0, 1, 1, 0, 1, 0, 1, 0};
    swizzle_case(400, NVN_FORMAT_RGB565, 0, pixels, 4, NULL, NULL, rgb);
    swizzle_case(401, NVN_FORMAT_RGB565, 0, pixels, 4, rgb1, NULL, rgb);
    swizzle_case(402, NVN_FORMAT_RGB565, 0, pixels, 4, rgb1, map, composed);
    swizzle_case(403, NVN_FORMAT_BGR565, 0, pixels, 4, NULL, NULL, bgr);
    swizzle_case(404, NVN_FORMAT_RG8, 0, pixels, 4, NULL, NULL, (const float[8]){31 / 255.f, 0, 0, 1, 0, 248 / 255.f, 0, 1});
    const uint8_t rgba[8] = {255, 0, 0, 0, 0, 0, 255, 64};
    swizzle_case(405, NVN_FORMAT_RGBX8, 0, rgba, 8, NULL, NULL, rgb);
    swizzle_case(406, NVN_FORMAT_RGBX8, 0, rgba, 8, NULL, aaaa, (const float[8]){1, 1, 1, 1, 1, 1, 1, 1});
    swizzle_case(407, NVN_FORMAT_RGBA8, NVN_FORMAT_RGBX8, rgba, 8, NULL, NULL, rgb);
    swizzle_case(408, NVN_FORMAT_RGBA8, 0, rgba, 8, NULL, NULL, (const float[8]){1, 0, 0, 0, 0, 0, 1, 64 / 255.f});
    swizzle_case(409, NVN_FORMAT_RGBX8, NVN_FORMAT_RGBA8, rgba, 8, NULL, NULL, (const float[8]){1, 0, 0, 0, 0, 0, 1, 64 / 255.f});
    cleared_case(413, NVN_FORMAT_RGB565);
    cleared_case(414, NVN_FORMAT_BGR565);
    image_case(415);
    printf("texture view swizzle: %s\n", g_fail_case ? "a case failed" : "every case matches");
    hv_pool_free(&g_p);
    return hv_finish();
}
