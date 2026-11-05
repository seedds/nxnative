// The tile mapping rule of a sparse texture in a sparse virtual pool (nvn_mtl.m sp_sync): guest 64 KB tile (tx, ty) of a tiled level is at (ty * cols + tx) * 64 KB from the
// level's start (nvn_tex_tile_grid), covers the texels (tx * tw, ty * th) of the tile size nvn_tex_sparse_tiles reports, and its bytes decode with nvn_deswizzle_tile to what
// the whole-level decode (nvn_deswizzle) gives for those texels. Mario Kart World's 8192x8192x68 ASTC 8x8 array (format 0x8e, 12 levels, flag 0x20) and Monster Hunter Rise's
// 1024x1024 ASTC 6x6 (the mip tail at a 4 KB offset) are the shapes; the Switch 2's GOB order is checked as well
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/nvn_tex.h"
#include "../tex/nvn_deswizzle.h"

static int g_n, g_fail;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); g_n++; \
    if (w_ != g_) { g_fail++; printf("FAIL %s: want %lld got %lld\n", name, w_, g_); } } while (0)

// Every tile of level `level` decodes to the whole-level decode's texels at its rect (one layer of the storage filled with a pattern)
static void tiles_match(const char *name, const nvn_texdesc *d, unsigned level, unsigned layer) {
    nvn_texlayout L; nvn_tex_layout(d, &L);
    uint32_t tw, th, td, cols, rows; int tiled = nvn_tex_sparse_tiles(d, &tw, &th, &td);
    EXPECT(name, 1, nvn_tex_tile_grid(d, level, &cols, &rows) && (int)level < tiled);
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint64_t lo = nvn_tex_level_offset(d, level), hi = level + 1 < d->levels ? nvn_tex_level_offset(d, level + 1) : L.layer_size;
    EXPECT("the level holds whole tiles", 0, (hi - lo) % 0x10000);
    EXPECT("the level is cols x rows tiles", (hi - lo) >> 16, (uint64_t)cols * rows);
    uint8_t *storage = malloc(L.layer_size * (layer + 1));
    for (uint64_t i = 0; i < L.layer_size * (layer + 1); i++) storage[i] = (uint8_t)(i * 2654435761u >> 13);
    uint32_t lw = d->width >> level, lh = d->height >> level, wb = (lw + bw - 1) / bw * bpb, lr = (lh + bh - 1) / bh;
    uint8_t *lin = malloc((size_t)wb * lr); nvn_deswizzle(d, level, layer, storage, lin, wb);
    unsigned lth, ltd; nvn_tex_level_tiles(d, level, &lth, &ltd);
    uint32_t gobs = tw / bw * bpb / 64; uint8_t *tile = malloc((size_t)tw / bw * bpb * (th / bh)); int bad = 0;
    for (uint32_t ty = 0; ty < rows; ty++) for (uint32_t tx = 0; tx < cols; tx++) {
        uint32_t x = tx * tw, y = ty * th; if (x >= lw || y >= lh) continue;
        uint32_t w = x + tw > lw ? lw - x : tw, h = y + th > lh ? lh - y : th, twb = (w + bw - 1) / bw * bpb, trows = (h + bh - 1) / bh;
        const uint8_t *src = storage + layer * L.layer_size + lo + ((uint64_t)ty * cols + tx) * 0x10000;
        nvn_deswizzle_tile(lth, gobs, twb, trows, src, tile, twb);
        for (uint32_t r = 0; r < trows && !bad; r++) bad = memcmp(tile + (size_t)r * twb, lin + (size_t)(y / bh + r) * wb + x / bw * bpb, twb) != 0;
        if (bad) { printf("  %s: tile (%u, %u) of level %u differs\n", name, tx, ty, level); break; }
    }
    EXPECT(name, 0, bad);
    free(storage); free(lin); free(tile);
}

int main(void) {
    // Mario Kart World's terrain arrays: ASTC 8x8 (16 bytes per 8x8 block), 8192 wide: a 64 KB tile is 64x64 blocks = 512x512 texels, the level is 16x16 tiles
    nvn_texdesc mkw = {.target = 4, .format = 0x8e, .flags = 0x20, .width = 8192, .height = 8192, .depth = 68, .levels = 12};
    uint32_t tw, th, td, cols, rows;
    EXPECT("MKW ASTC 8x8: tiled levels", 5, nvn_tex_sparse_tiles(&mkw, &tw, &th, &td));   // 8192 .. 512 wide
    EXPECT("MKW: tile width", 512, tw); EXPECT("MKW: tile height", 512, th);
    EXPECT("MKW: level 0 grid", 1, nvn_tex_tile_grid(&mkw, 0, &cols, &rows)); EXPECT("MKW: level 0 cols", 16, cols); EXPECT("MKW: level 0 rows", 16, rows);
    EXPECT("MKW: level 4 grid", 1, nvn_tex_tile_grid(&mkw, 4, &cols, &rows)); EXPECT("MKW: level 4 cols", 1, cols); EXPECT("MKW: level 4 rows", 1, rows);
    EXPECT("MKW: level 5 is mip tail", 0, nvn_tex_tile_grid(&mkw, 5, &cols, &rows));
    nvn_texlayout L; nvn_tex_layout(&mkw, &L);
    EXPECT("MKW: layer size is whole tiles", 0, L.layer_size & 0xFFFF);
    EXPECT("MKW: level 1 at 256 tiles", 256ull << 16, nvn_tex_level_offset(&mkw, 1));
    // A smaller stand-in of the same shape class for the byte check (8192x8192 would decode 16 MB per level): 1024x1024 ASTC 8x8, 2x2 tiles at level 0, one at level 1
    nvn_texdesc small = {.target = 4, .format = 0x8e, .flags = 0x20, .width = 1024, .height = 1024, .depth = 2, .levels = 4};
    EXPECT("small: tiled levels", 2, nvn_tex_sparse_tiles(&small, &tw, &th, &td));
    for (int sw2 = 0; sw2 < 2; sw2++) {
        nvn_gob_switch2 = sw2;
        tiles_match(sw2 ? "small level 0 layer 1 (Switch 2 GOBs)" : "small level 0 layer 1", &small, 0, 1);
        tiles_match(sw2 ? "small level 1 (Switch 2 GOBs)" : "small level 1", &small, 1, 0);
    }
    nvn_gob_switch2 = 0;
    // Monster Hunter Rise's streamed 1024x1024 ASTC 6x6 (no packaged layout): 48 GOBs wide = 3 tiles of 16 GOBs (64 blocks = 384 texels) across, 1024 / 6 = 171 block rows in 3 rows of 64; level 1 at 0x90000
    nvn_texdesc mhr = {.target = 1, .format = 0x7d, .flags = 0x20, .width = 1024, .height = 1024, .depth = 1, .levels = 11};
    EXPECT("MHR: tile width", 384, (nvn_tex_sparse_tiles(&mhr, &tw, &th, &td), tw)); EXPECT("MHR: tile height", 384, th);
    EXPECT("MHR: level 0 grid", 1, nvn_tex_tile_grid(&mhr, 0, &cols, &rows)); EXPECT("MHR: cols", 3, cols); EXPECT("MHR: rows", 3, rows);
    EXPECT("MHR: level 1 offset", 0x90000, nvn_tex_level_offset(&mhr, 1));
    tiles_match("MHR level 0 (edge tiles clipped)", &mhr, 0, 0);
    // A 2D RGBA8 sparse texture (4 bytes per texel): tiles of 128x128 texels (block height 16 GOBs), a level 1 tile grid of 2x2 for 512x512
    nvn_texdesc rgba = {.target = 1, .format = 0x25, .flags = 0x20, .width = 512, .height = 512, .depth = 1, .levels = 3};
    EXPECT("RGBA8: tile width", 128, (nvn_tex_sparse_tiles(&rgba, &tw, &th, &td), tw)); EXPECT("RGBA8: tile height", 128, th);
    EXPECT("RGBA8: level 1 grid", 1, nvn_tex_tile_grid(&rgba, 1, &cols, &rows)); EXPECT("RGBA8: level 1 cols", 2, cols);
    tiles_match("RGBA8 level 1", &rgba, 1, 0);
    // Not sparse: no grid
    nvn_texdesc plain = {.target = 1, .format = 0x25, .flags = 0, .width = 512, .height = 512, .depth = 1, .levels = 3};
    EXPECT("not sparse: no grid", 0, nvn_tex_tile_grid(&plain, 0, &cols, &rows));
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
