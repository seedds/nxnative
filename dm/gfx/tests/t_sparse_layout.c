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
    // Sparse 3D textures take the standard 3D tile (sparse_tile_h3, sparse_tile_d3). Metroid Prime Remastered's lighting volumes map fixed tile indices of each 16-slice slab, BC1 and BC6H alike: those tiles
    // must be one and the same box of texels in both formats, and exactly that box, in exactly that slab
    static const struct { int format; uint64_t size; uint32_t tw, per_slab, ntiles, tiles[12]; } vol[] = {
        {0x42, 0x1700000, 128, 16, 6, {5, 6, 7, 9, 10, 11}},   // BC1: 4x4 tiles a slab
        {0x50, 0x2e00000, 64, 32, 12, {10, 11, 12, 13, 14, 15, 18, 19, 20, 21, 22, 23}},   // BC6H: 8x4 tiles a slab
    };
    for (unsigned c = 0; c < 2; c++) {
        nvn_texdesc d = { .target = 2, .format = vol[c].format, .flags = 0x20, .width = 512, .height = 256, .depth = 368, .levels = 1 };
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        uint32_t tw, th, td; int nt = nvn_tex_sparse_tiles(&d, &tw, &th, &td);
        int bw, bh, bpb = nvn_fmt_bpb(d.format, &bw, &bh), ok = L.size == vol[c].size && L.tileH == 1 && L.tileD == 4 && nt == 1 && tw == vol[c].tw && th == 64 && td == 16;
        uint32_t slab = 15; uint8_t *st = calloc(1, L.size);
        for (uint32_t i = 0; i < vol[c].ntiles; i++) memset(st + ((uint64_t)slab * vol[c].per_slab + vol[c].tiles[i]) * 0x10000, 0xff, 0x10000);
        size_t pitch = (size_t)(512 / bw) * bpb; uint32_t rows = 256 / bh; uint8_t *lin = calloc(1, pitch * rows * 368);
        ok = !nvn_deswizzle(&d, 0, 0, st, lin, pitch) && ok;
        uint64_t wrong = 0;
        for (uint32_t z = 0; z < 368; z++) for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < 512u / bw; x++) {
            int want = z >= slab * 16 && z < (slab + 1) * 16 && y * bh >= 64 && y * bh < 192 && x * bw >= 128;
            wrong += lin[(size_t)z * pitch * rows + (size_t)y * pitch + (size_t)x * bpb] != (want ? 0xff : 0);
        }
        ok = ok && !wrong; free(st); free(lin);
        g_n++; if (!ok) g_fail++;
        printf("512x256x368 format %#x sparse: size %#llx, block %u/%u, tile %ux%ux%u, mapped tiles the box 128..512 x 64..192 (%llu blocks elsewhere) %s\n", d.format, (unsigned long long)L.size, L.tileH, L.tileD, tw, th, td, (unsigned long long)wrong, ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97");
    }
    static const struct { int format; uint32_t tw, th, td; unsigned h, dp; } shape[] = {{0x01, 64, 32, 32, 2, 5}, {0x0d, 32, 32, 32, 2, 5}, {0x25, 32, 32, 16, 2, 4}, {0x29, 32, 16, 16, 1, 4}};
    for (unsigned c = 0; c < 4; c++) {
        nvn_texdesc d = { .target = 2, .format = shape[c].format, .flags = 0x20, .width = 256, .height = 128, .depth = 64, .levels = 1 };
        nvn_texlayout L; nvn_tex_layout(&d, &L); uint32_t tw, th, td; nvn_tex_sparse_tiles(&d, &tw, &th, &td);
        int ok = tw == shape[c].tw && th == shape[c].th && td == shape[c].td && L.tileH == shape[c].h && L.tileD == shape[c].dp;
        g_n++; if (!ok) g_fail++;
        printf("256x128x64 format %#x sparse: tile %ux%ux%u, block %u/%u %s\n", d.format, tw, th, td, L.tileH, L.tileD, ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97");
    }
    {   // smaller than a tile: all mip tail, the automatic layout
        nvn_texdesc d = { .target = 2, .format = 0x25, .flags = 0x20, .width = 16, .height = 16, .depth = 16, .levels = 1 }, n = d; n.flags = 0;
        nvn_texlayout L, N; nvn_tex_layout(&d, &L); nvn_tex_layout(&n, &N);
        int ok = L.tileH == N.tileH && L.tileD == N.tileD;
        g_n++; if (!ok) g_fail++;
        printf("16x16x16 RGBA8 sparse keeps the automatic block %u/%u %s\n", L.tileH, L.tileD, ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97");
    }
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
