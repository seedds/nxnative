// CPU texel writes into block-linear storage (hle_nvn.c write_texels and write_region_bl, nvnTextureWriteTexels), on the port: a region written must leave the storage byte for
// byte as "decode the whole level, patch the region, encode again" leaves it (t_nvn.c 6c2, the Metal build's form of these cases, which runs on the Mac only). The region writer
// addresses each block row once (nvn_bl_row_of) and each 16-byte run by shifts (nvn_bl_row_at) since a game's per-frame glyph writes spent more in nvn_bl_offset than in their
// copies (Mario vs. Donkey Kong's title). Cases: RGBA8 100 x 37 (a row not a whole number of GOBs), BC1 64 x 64 (4 x 4 blocks of 8 bytes), RGBA16F 33 x 17 as a two-layer array
// (the last layer through a view), R8 300 x 70 with a block height of 4 GOBs (a run that starts inside 16 bytes and one that ends inside them), and each with five regions: a
// texel, a corner, one crossing GOBs, a whole row and the whole level, over random old content. Needs no GPU work: the storage is read back on the CPU
#include "t_vk_harness.h"
extern __typeof__(*(NVNT_nvnTextureViewSetDefaults)0) h_nvnTextureViewSetDefaults;
extern __typeof__(*(NVNT_nvnTextureViewSetLayers)0) h_nvnTextureViewSetLayers;

int main(void) {
    hv_up("t_vk_write_texels");
    hv_device(0);
    hv_pool p; if (!hv_pool_make(&p, 1 << 20)) { t("memory pool allocated", 0); return hv_finish(); }
    struct { int fmt, w, h, layers, bh; int rg[5][4]; } cs[] = {
        {NVN_FORMAT_RGBA8, 100, 37, 1, 0, {{0, 0, 1, 1}, {99, 36, 1, 1}, {13, 5, 40, 9}, {0, 20, 100, 1}, {0, 0, 100, 37}}},
        {NVN_FORMAT_RGBA_DXT1, 64, 64, 1, 1, {{0, 0, 4, 4}, {60, 60, 4, 4}, {16, 8, 32, 20}, {0, 32, 64, 4}, {0, 0, 64, 64}}},
        {NVN_FORMAT_RGBA16F, 33, 17, 2, 1, {{32, 16, 1, 1}, {1, 2, 30, 13}, {0, 0, 33, 1}, {5, 9, 17, 8}, {0, 0, 33, 17}}},
        {NVN_FORMAT_R8, 300, 70, 1, 2, {{7, 3, 1, 1}, {5, 33, 23, 30}, {61, 0, 70, 70}, {0, 69, 300, 1}, {0, 0, 300, 70}}},
    };
    static uint8_t want[1 << 18], lin[1 << 17], src[1 << 17];
    int bad = 0, n = 0; uint32_t seed = 12345; size_t off = 0;
    for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
        NVNtexture tex; const tex_t *tt = (const tex_t *)(const void *)&tex;
        hv_texture(&tex, &p, &off, cs[c].layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D, cs[c].fmt, (uint32_t)cs[c].w, (uint32_t)cs[c].h,
                   (uint32_t)cs[c].layers, 1, 0, cs[c].bh, 0);
        off += (size_t)tt->L.size;
        int bw, bh, bpb = nvn_fmt_bpb(cs[c].fmt, &bw, &bh);
        size_t lp = (size_t)((cs[c].w + bw - 1) / bw) * (size_t)bpb;
        if (tt->L.size > sizeof want) { t("texture storage fits the test's buffers", 0); return hv_finish(); }
        for (int k = 0; k < 5; k++) {
            dm_guest_write_prepare(tt->mem, tt->L.size);   // the dirty tracker's protection lifted before the CPU writes the old content
            for (size_t i = 0; i < tt->L.size; i++) tt->mem[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 3);
            const int x = cs[c].rg[k][0], y = cs[c].rg[k][1], w = cs[c].rg[k][2], h = cs[c].rg[k][3], z = cs[c].layers - 1;
            const size_t wbb = (size_t)((w + bw - 1) / bw) * (size_t)bpb, hbb = (size_t)((h + bh - 1) / bh);
            for (size_t i = 0; i < wbb * hbb; i++) src[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 5);
            memcpy(want, tt->mem, tt->L.size);   // expected: the whole level decoded, the region patched, encoded again (the last layer)
            nvn_deswizzle(&tt->d, 0, (unsigned)z, want, lin, lp);
            for (size_t r = 0; r < hbb; r++) memcpy(lin + ((size_t)(y / bh) + r) * lp + (size_t)(x / bw) * (size_t)bpb, src + r * wbb, wbb);
            nvn_swizzle(&tt->d, 0, (unsigned)z, lin, lp, want);
            NVNtextureView vw; h_nvnTextureViewSetDefaults(&vw); h_nvnTextureViewSetLayers(&vw, z, 1);
            NVNcopyRegion rg = {x, y, 0, w, h, 1};
            h_nvnTextureWriteTexels(&tex, cs[c].layers > 1 ? &vw : NULL, &rg, src);
            n++;
            if (memcmp(want, tt->mem, tt->L.size)) { bad++; printf("  case %u region %d (%d,%d %dx%d) differs\n", c, k, x, y, w, h); }
        }
        h_nvnTextureFinalize(&tex);
    }
    char name[160];
    snprintf(name, sizeof name, "WriteTexels regions byte-identical to a whole-level round trip (%d cases, %d differ)", n, bad);
    t(name, n == 20 && !bad);
    hv_pool_free(&p);
    return hv_finish();
}
