// Storage sizes of sparse (flag 0x20) 2D textures with the automatic block height, against the sizes of the pre-swizzled level data the games ship: Yoshi's Crafted World's
// streamed DXT and BGRA8 textures (one level each) and Monster Hunter Rise's 1024×1024 ASTC 6×6 (48 GOBs wide, level 1 at 0x90000). A non-sparse texture keeps the automatic block height
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/nvn_tex.h"
#include "../tex/nvn_deswizzle.h"

static int g_n, g_fail;

static void check_size(uint32_t w, uint32_t h, int format, int flags, uint64_t want) {
    nvn_texdesc d = { .target = 1, .format = format, .flags = flags, .width = w, .height = h, .depth = 1, .levels = 1 };
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    g_n++; if (L.size != want) g_fail++;
    printf("%4ux%-4u format %#x flags %#x: size %#llx (want %#llx) %s\n", w, h, format, flags, (unsigned long long)L.size, (unsigned long long)want, L.size == want ? "✓" : "✗");
}

int main(void) {
    static const struct { uint32_t w, h; int format; uint64_t size; } yoshi[] = {
        {384, 384, 0x48, 0x40000}, {384, 384, 0x45, 0x20000}, {960, 384, 0x48, 0x80000}, {480, 480, 0x48, 0x40000}, {900, 420, 0x48, 0x80000},
        {1920, 912, 0x48, 0x200000}, {1920, 864, 0x48, 0x200000}, {1440, 480, 0x48, 0xc0000}, {840, 448, 0x48, 0x80000}, {840, 840, 0x48, 0x100000},
        {1350, 270, 0x9c, 0x210000}, {240, 270, 0x9c, 0x60000}, {720, 270, 0x9c, 0x120000}, {1080, 174, 0x9c, 0x120000}, {2010, 390, 0x9c, 0x400000},
    };
    for (unsigned i = 0; i < sizeof yoshi / sizeof *yoshi; i++) check_size(yoshi[i].w, yoshi[i].h, yoshi[i].format, 0x20, yoshi[i].size);
    check_size(384, 384, 0x48, 0, 0x30000);
    nvn_texdesc mh = { .target = 1, .format = 0x7d, .flags = 0x20, .width = 1024, .height = 1024, .depth = 1, .levels = 11 };
    uint64_t l1 = nvn_tex_level_offset(&mh, 1); uint32_t g0 = nvn_tex_level_gobs(&mh, 0);
    g_n++; if (l1 != 0x90000 || g0 != 48) g_fail++;
    printf("1024x1024 ASTC 6x6 sparse: level 0 %u GOBs wide, level 1 at %#llx (want 48, 0x90000) %s\n", g0, (unsigned long long)l1, l1 == 0x90000 && g0 == 48 ? "✓" : "✗");
    // Tiles decoded one by one (nvn_deswizzle_tile, the placement sparse textures of nvn_mtl.m sparse_sync) read what the whole level reads: a sparse BC6H 2D array (16-byte blocks,
    // 256x256 texels a tile, Mario Kart World's terrain format) and a sparse RGBA8 texture (128x128), each with level 0 one tile short of square, both GOB orders; the mip tail decoded
    // from its own bytes (nvn_deswizzle_from) reads what the whole texture reads
    static const struct { int format; uint32_t w, h, layers, levels; } tc[] = {{0x50, 1024, 768, 2, 5}, {0x25, 512, 384, 1, 4}};
    for (int g2 = 0; g2 < 2; g2++) for (unsigned c = 0; c < 2; c++) {
        nvn_gob_switch2 = g2;
        nvn_texdesc d = { .target = tc[c].layers > 1 ? 4 : 1, .format = tc[c].format, .flags = 0x20, .width = tc[c].w, .height = tc[c].h, .depth = tc[c].layers, .levels = tc[c].levels };
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        uint32_t tw, th, td; int nt = nvn_tex_sparse_tiles(&d, &tw, &th, &td);
        uint8_t *st = malloc(L.size); for (uint64_t i = 0; i < L.size; i++) st[i] = (uint8_t)(i * 2654435761u >> 13);
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh), ok = nt > 0 && nt < (int)d.levels;
        for (uint32_t y = 0; ok && y < tc[c].layers; y++) for (int l = 0; ok && l < (int)d.levels; l++) {
            uint32_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1; size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            uint8_t *whole = malloc(pitch * rows), *part = malloc(pitch * rows);
            ok = !nvn_deswizzle(&d, (unsigned)l, y, st, whole, pitch);
            if (l < nt) {   // each tile from its own 64 KB, into its place
                uint32_t tx_n = (w + tw - 1) / tw, ty_n = (h + th - 1) / th; size_t tp = (size_t)(tw / bw) * bpb; uint8_t tile[0x10000], lin[0x10000];
                for (uint32_t ty = 0; ok && ty < ty_n; ty++) for (uint32_t tx = 0; ok && tx < tx_n; tx++) {
                    memcpy(tile, st + y * L.layer_size + nvn_tex_level_offset(&d, (unsigned)l) + ((uint64_t)ty * tx_n + tx) * 0x10000, 0x10000);
                    uint32_t wb, nr; ok = !nvn_deswizzle_tile(&d, (unsigned)l, tx, ty, tile, lin, tp, &wb, &nr) && wb && nr;
                    for (uint32_t r = 0; ok && r < nr; r++) memcpy(part + (size_t)(ty * (th / bh) + r) * pitch + (size_t)tx * tp, lin + (size_t)r * tp, wb);
                }
            } else {   // the tail from its own bytes
                uint64_t at = y * L.layer_size + nvn_tex_level_offset(&d, (unsigned)nt);
                ok = ok && !nvn_deswizzle_from(&d, (unsigned)l, y, st + at, at, part, pitch);
            }
            ok = ok && !memcmp(whole, part, pitch * rows);
            free(whole); free(part);
        }
        free(st);
        g_n++; if (!ok) g_fail++;
        printf("%ux%ux%u format %#x %s GOB order: %d tiled levels of %ux%u, tiles and tail decode as the whole %s\n", d.width, d.height, tc[c].layers, d.format, g2 ? "Switch 2" : "X1", nt, tw, th, ok ? "✓" : "✗");
    }
    nvn_gob_switch2 = 0;
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
