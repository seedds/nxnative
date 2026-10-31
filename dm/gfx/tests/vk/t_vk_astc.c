// ASTC on the Vulkan backend (docs/design/vulkan-backend.md 5.2, decision D1; design 10 item 2): for each of the 28 ASTC formats (14 block sizes, linear and sRGB), a 2D array of
// two layers and two levels whose blocks are synthetic (dm/gfx/tex/tests/astc_synth.h: normal blocks of random partitions, dual planes, weight grids, ranges and LDR endpoint
// modes, void-extent blocks, illegal blocks and blocks with an HDR partition, one kind per block in turn) is uploaded three ways:
//   A held as ASTC, decoded by the device when sampled, where the device has textureCompressionASTC_LDR (a decoder independent of this repository's two; lavapipe 25.2.8 does
//     not have it, so A is skipped there and the independent comparison is dm/gfx/tex/tests/t_astc_mesa.c's, Mesa's decoder through OpenGL on llvmpipe);
//   B decoded at upload into RGBA8 by the compute shader (dm/gfx/hostvk/shaders/astc_decode.comp; DM_VK_ASTC=decode), some formats in bands of one block row;
//   D decoded at upload by the CPU path (dm/gfx/tex/astc_decode.c inside the backend, DM_VK_DESWIZZLE=cpu);
// and C, the test's own decode by dm/gfx/tex/astc_decode.c, uploaded as an RGBA8 (sRGB) texture. Every level and layer of B and D is read back and must equal C's texels bit for
// bit. Level 0, layer 0 of A, B and C is sampled with the same shader (texelFetch into RGBA32F): B must equal C there too, and A against C is reported texel by texel, with the
// differing texels by their blocks' kind: a difference with lavapipe is a finding about one of the decoders, explained in the step note, not a failure of this test.
// The texture memory counters must count the native and the decoded textures and the blocks the decoded ones stand for.
// Checks: per format the two bit-exact readbacks and the sampled B = C; the memory counters; the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/fetch_frag.h"
#include "tests/astc_synth.h"
#include "nvn_fmt_vk.h"
#include <math.h>

static const char k_fetch[] = "[{\"kind\":\"tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"},"
                              "{\"kind\":\"samp_tex\",\"reg\":0,\"name\":\"t\",\"class\":\"combined\",\"set\":2,\"binding\":2048,\"dim\":\"2d\",\"sampled\":\"float\"}]";
static const int BS[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
enum { BX = 12, BY = 8, LAYERS = 2, LEVELS = 2 };
static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

int main(void) {
    const hostvk_dev *dev = hv_up("t_vk_astc");
    hv_device(0);
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    hv_pool p; if (!hv_pool_make(&p, 16u << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds c = {p.mem + 0x40000, 0, 0x10000};
    NVNprogram fetch;
    const hv_shader sf[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"fetch", 1, HV_SPV(fetch_frag_spv), k_fetch, NULL}};
    hv_program(&fetch, &p, 0, sf, 2);
    NVNtexturePool tp; NVNsamplerPool sp;
    h_nvnTexturePoolInitialize(&tp, &p.pool, 0x10000, 256); h_nvnSamplerPoolInitialize(&sp, &p.pool, 0x30000, 16);
    NVNsamplerBuilder sb; NVNsampler samp; h_nvnSamplerBuilderSetDefaults(&sb); h_nvnSamplerInitialize(&samp, &sb); h_nvnSamplerPoolRegisterSampler(&sp, 1, &samp);
    size_t off = 0x50000; int next_id = 1, native = dev->astc_ldr;
    printf("device: textureCompressionASTC_LDR %s\n", native ? "present (way A: lavapipe's own decode)" : "absent (way A skipped)");
    uint64_t rng = 0xA57C;
    long blocks_total = 0, texels_total = 0, kinds_total[4] = {0}, nat_diff = 0, nat_texels = 0, nat_diff_kind[4] = {0}, nat_max = 0, nat_over1 = 0;
    int64_t want_blocks = 0;
    char name[256];
    static float fa[144 * 96 * 4], fb[144 * 96 * 4], fc[144 * 96 * 4];
    for (int k = 0; k < 14; k++)
        for (int srgb = 0; srgb < 2; srgb++) {
            int bw = BS[k][0], bh = BS[k][1], nf = NVN_FORMAT_RGBA_ASTC_4x4 + k + 14 * srgb;
            uint32_t W = BX * (uint32_t)bw, H = BY * (uint32_t)bh;
            uint8_t *blocks[LEVELS][LAYERS], *ref[LEVELS][LAYERS]; uint8_t *kind0 = NULL;
            long texels = 0, nblocks = 0;
            for (unsigned l = 0; l < LEVELS; l++)
                for (unsigned z = 0; z < LAYERS; z++) {
                    uint32_t wl = mipd(W, l), hl = mipd(H, l), nbx = (wl + (uint32_t)bw - 1) / (uint32_t)bw, nby = (hl + (uint32_t)bh - 1) / (uint32_t)bh;
                    blocks[l][z] = malloc((size_t)nbx * nby * 16); ref[l][z] = malloc((size_t)wl * hl * 4);
                    if (l == 0 && z == 0) kind0 = malloc((size_t)nbx * nby);
                    for (uint32_t i = 0; i < nbx * nby; i++) {
                        int kind = astc_synth_random(&rng, bw, bh, (int)(i % 16), blocks[l][z] + (size_t)i * 16, NULL);
                        if (kind < 0) { t("a synthetic block of every kind can be made", 0); return hv_finish(); }
                        kinds_total[kind]++;
                        if (kind0 && l == 0 && z == 0) kind0[i] = (uint8_t)kind;
                    }
                    astc_decode_image(blocks[l][z], (size_t)nbx * 16, wl, hl, bw, bh, srgb, ref[l][z], (size_t)wl * 4);
                    texels += (long)wl * hl; nblocks += (long)nbx * nby;
                    want_blocks += 2 * (int64_t)nbx * nby * 16;   // ways B and D hold decodes of these blocks
                }
            NVNtexture tex[4];   // A native, B GPU decode, D CPU decode, C the test's decode as RGBA8
            for (int w = 0; w < 4; w++) {
                if (w == 0 && !native) continue;
                hv_texture(&tex[w], &p, &off, NVN_TEXTURE_TARGET_2D_ARRAY, w == 3 ? (srgb ? NVN_FORMAT_RGBA8_SRGB : NVN_FORMAT_RGBA8) : nf, W, H, LAYERS, LEVELS, 0, 2, 0);
                for (unsigned l = 0; l < LEVELS; l++)
                    for (unsigned z = 0; z < LAYERS; z++) {
                        uint32_t wl = mipd(W, l), nbx = (wl + (uint32_t)bw - 1) / (uint32_t)bw;
                        if (!hv_tex_fill(&tex[w], l, z, w == 3 ? ref[l][z] : blocks[l][z], w == 3 ? (size_t)wl * 4 : (size_t)nbx * 16)) { t("texture storage filled", 0); return hv_finish(); }
                    }
                // made and uploaded now, under the switches of its way
                if (w == 0) hvk_test_tex_opts(0, 1, -1, -1);
                else if (w == 1) { hvk_test_tex_opts(1, 1, -1, -1); if (k % 3 == 0) hvk_test_band_bytes(1); }   // one block row a band
                else if (w == 2) hvk_test_tex_opts(1, 2, -1, -1);
                else hvk_test_tex_opts(0, 0, -1, -1);
                VkFormat f; VkImageAspectFlags a;
                int made = hvk_test_image((const tex_t *)(const void *)&tex[w], &f, &a) != VK_NULL_HANDLE;
                hvk_test_band_bytes(0);
                if (w == 1 || w == 2) {
                    snprintf(name, sizeof name, "%s %s: way %s made as VkFormat %u (RGBA8%s)", nvn_fmt_name(nf), srgb ? "sRGB" : "linear", w == 1 ? "B" : "D", (unsigned)f, srgb ? " sRGB" : "");
                    t(name, made && f == (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM));
                }
            }
            hvk_test_tex_opts(0, 0, -1, -1);
            // B and D read back, every level and layer, against C (the test's decode)
            for (int w = 1; w <= 2; w++) {
                long bad = 0; int read = 1;
                for (unsigned l = 0; l < LEVELS; l++)
                    for (unsigned z = 0; z < LAYERS; z++) {
                        uint32_t wl = mipd(W, l), hl = mipd(H, l);
                        uint8_t *got = malloc((size_t)wl * hl * 4);
                        if (!hv_read_n((const tex_t *)(const void *)&tex[w], VK_IMAGE_ASPECT_COLOR_BIT, l, z, 1, 0, 0, wl, hl, 4, got)) read = 0;
                        else for (uint32_t i = 0; i < wl * hl; i++) if (memcmp(got + 4 * i, ref[l][z] + 4 * i, 4)) {
                            if (!bad) fprintf(stderr, "%s way %s level %u layer %u texel %u,%u: %02x%02x%02x%02x, the C decoder %02x%02x%02x%02x\n", nvn_fmt_name(nf), w == 1 ? "B" : "D", l, z,
                                              i % wl, i / wl, got[4 * i], got[4 * i + 1], got[4 * i + 2], got[4 * i + 3], ref[l][z][4 * i], ref[l][z][4 * i + 1], ref[l][z][4 * i + 2], ref[l][z][4 * i + 3]);
                            bad++;
                        }
                        free(got);
                    }
                snprintf(name, sizeof name, "%s: %s decode = the C decoder, bit for bit (%ld blocks, %ld texels, 2 levels x 2 layers)", nvn_fmt_name(nf), w == 1 ? "the compute" : "the backend's CPU",
                         nblocks, texels);
                t(name, read && bad == 0);
            }
            // Level 0, layer 0 sampled through one shader: B against C exactly, A against C reported
            int ida = 0, idb = next_id++, idc = next_id++;
            h_nvnTexturePoolRegisterTexture(&tp, idb, &tex[1], NULL); h_nvnTexturePoolRegisterTexture(&tp, idc, &tex[3], NULL);
            int rb = hv_fetch(&q, &c, &fetch, &tp, &sp, idb, NVN_FORMAT_RGBA32F, W, H, fb), rc = hv_fetch(&q, &c, &fetch, &tp, &sp, idc, NVN_FORMAT_RGBA32F, W, H, fc);
            snprintf(name, sizeof name, "%s: the compute decode and the C decoder's RGBA8 sample the same (%u texels)", nvn_fmt_name(nf), W * H);
            t(name, rb && rc && !memcmp(fb, fc, (size_t)W * H * 16));
            if (native) {
                ida = next_id++; h_nvnTexturePoolRegisterTexture(&tp, ida, &tex[0], NULL);
                int ra = hv_fetch(&q, &c, &fetch, &tp, &sp, ida, NVN_FORMAT_RGBA32F, W, H, fa);
                snprintf(name, sizeof name, "%s: the texture held as ASTC is sampled", nvn_fmt_name(nf)); t(name, ra);
                long diff = 0; int mx = 0;
                uint32_t nbx = (W + (uint32_t)bw - 1) / (uint32_t)bw;
                for (uint32_t i = 0; ra && i < W * H; i++) {
                    int d = 0;
                    for (int ch = 0; ch < 4; ch++) { int u = (int)lrintf(fabsf(fa[4 * i + ch] - fc[4 * i + ch]) * 255.0f * 16); if (u > d) d = u; }   // in 1/16 of an 8-bit unit
                    if (fa[4 * i] != fc[4 * i] || fa[4 * i + 1] != fc[4 * i + 1] || fa[4 * i + 2] != fc[4 * i + 2] || fa[4 * i + 3] != fc[4 * i + 3]) {
                        uint32_t x = i % W, y = i / W; int kind = kind0[(y / (uint32_t)bh) * nbx + x / (uint32_t)bw];
                        if (!diff && !getenv("T_VK_ASTC_QUIET"))
                            printf("  %s: first texel differing from lavapipe: %u,%u (block kind %d): lavapipe %.6f %.6f %.6f %.6f, C %.6f %.6f %.6f %.6f\n", nvn_fmt_name(nf), x, y, kind,
                                   fa[4 * i], fa[4 * i + 1], fa[4 * i + 2], fa[4 * i + 3], fc[4 * i], fc[4 * i + 1], fc[4 * i + 2], fc[4 * i + 3]);
                        diff++; nat_diff_kind[kind]++;
                        if (d > 16) nat_over1++;
                    }
                    if (d > mx) mx = d;
                }
                nat_diff += diff; nat_texels += (long)W * H; if (mx > nat_max) nat_max = mx;
                printf("%-22s %-6s: %ld blocks, %ld texels; compute = C, CPU = C; lavapipe: %ld of %u texels of level 0 layer 0 differ (largest %.2f units of 1/255)\n", nvn_fmt_name(nf),
                       srgb ? "sRGB" : "linear", nblocks, texels, diff, W * H, mx / 16.0);
            } else printf("%-22s %-6s: %ld blocks, %ld texels; compute = C, CPU = C\n", nvn_fmt_name(nf), srgb ? "sRGB" : "linear", nblocks, texels);
            blocks_total += nblocks; texels_total += texels;
            for (unsigned l = 0; l < LEVELS; l++) for (unsigned z = 0; z < LAYERS; z++) { free(blocks[l][z]); free(ref[l][z]); }
            free(kind0);
        }
    printf("ASTC: %ld blocks (%ld normal, %ld void-extent, %ld illegal, %ld with an HDR partition), %ld texels over 28 formats, each decoded by the compute shader and the backend's "
           "CPU path and compared with the C decoder\n", blocks_total, kinds_total[0], kinds_total[1], kinds_total[2], kinds_total[3], texels_total);
    if (native)
        printf("lavapipe's own decode against the C decoder: %ld of %ld sampled texels differ (%ld by more than one 8-bit unit; largest %.2f units); differing texels by block kind: "
               "normal %ld, void-extent %ld, illegal %ld, HDR partition %ld\n", nat_diff, nat_texels, nat_over1, nat_max / 16.0, nat_diff_kind[0], nat_diff_kind[1], nat_diff_kind[2], nat_diff_kind[3]);
    int64_t mem[4]; uint64_t up[3]; hvk_test_tex_mem(mem, up);
    snprintf(name, sizeof name, "texture memory: ASTC held compressed %.2f MiB, decoded %.2f MiB standing for %.2f MiB of blocks (%lld expected), %llu ASTC decodes",
             mem[1] / 1048576.0, mem[2] / 1048576.0, mem[3] / 1048576.0, (long long)want_blocks, (unsigned long long)up[2]);
    t(name, (!native || mem[1] > 0) && mem[2] > 0 && mem[3] == want_blocks && up[2] > 0);
    char line[1024]; be_res_stats(line, sizeof line); printf("statistics line: %s\n", line);
    hv_pool_free(&p);
    return hv_finish();
}
