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
#define EXPECT(name, want, got)                                                                                        \
    do {                                                                                                               \
        long long w_ = (long long)(want), g_ = (long long)(got);                                                       \
        g_n++;                                                                                                         \
        if (w_ != g_) {                                                                                                \
            g_fail++;                                                                                                  \
            printf("FAIL %s: want %lld got %lld\n", name, w_, g_);                                                     \
        }                                                                                                              \
    } while (0)

// Every tile of level `level` decodes to the whole-level decode's texels at its rect (one layer of the storage filled with a pattern)
static void tiles_match(const char *name, const nvn_texdesc *desc, unsigned level, unsigned layer) {
    nvn_texlayout layout;
    nvn_tex_layout(desc, &layout);
    uint32_t tile_width, tile_height, tile_depth, cols, rows;
    int tiled = nvn_tex_sparse_tiles(desc, &tile_width, &tile_height, &tile_depth);
    EXPECT(name, 1, nvn_tex_tile_grid(desc, level, &cols, &rows) && (int)level < tiled);
    int block_width, block_height, bytes_per_block = nvn_fmt_bpb(desc->format, &block_width, &block_height);
    uint64_t level_start = nvn_tex_level_offset(desc, level),
             level_end = level + 1 < desc->levels ? nvn_tex_level_offset(desc, level + 1) : layout.layer_size;
    EXPECT("the level holds whole tiles", 0, (level_end - level_start) % 0x10000);
    EXPECT("the level is cols x rows tiles", (level_end - level_start) >> 16, (uint64_t)cols * rows);
    uint8_t *storage = malloc(layout.layer_size * (layer + 1));
    for (uint64_t i = 0; i < layout.layer_size * (layer + 1); i++) {
        storage[i] = (uint8_t)(i * 2654435761u >> 13);
    }
    uint32_t level_width = desc->width >> level, level_height = desc->height >> level,
             row_bytes = (level_width + block_width - 1) / block_width * bytes_per_block,
             block_rows = (level_height + block_height - 1) / block_height;
    uint8_t *linear = malloc((size_t)row_bytes * block_rows);
    nvn_deswizzle(desc, level, layer, storage, linear, row_bytes);
    uint8_t *tile = malloc((size_t)tile_width / block_width * bytes_per_block * (tile_height / block_height));
    int mismatch = 0;
    for (uint32_t ty = 0; ty < rows; ty++) {
        for (uint32_t tx = 0; tx < cols; tx++) {
            uint32_t x = tx * tile_width, y = ty * tile_height;
            if (x >= level_width || y >= level_height) {
                continue;
            }
            uint32_t w = x + tile_width > level_width ? level_width - x : tile_width,
                     h = y + tile_height > level_height ? level_height - y : tile_height,
                     tile_row_bytes = (w + block_width - 1) / block_width * bytes_per_block,
                     tile_rows = (h + block_height - 1) / block_height;
            const uint8_t *src =
                storage + layer * layout.layer_size + level_start + ((uint64_t)ty * cols + tx) * 0x10000;
            uint32_t decoded_w, decoded_h;
            EXPECT("tile decode", 0,
                   nvn_deswizzle_tile(desc, level, tx, ty, src, tile, tile_row_bytes, &decoded_w, &decoded_h));
            EXPECT("tile row bytes", tile_row_bytes, decoded_w);
            EXPECT("tile rows", tile_rows, decoded_h);
            for (uint32_t r = 0; r < tile_rows && !mismatch; r++) {
                mismatch =
                    memcmp(tile + (size_t)r * tile_row_bytes,
                           linear + (size_t)(y / block_height + r) * row_bytes + x / block_width * bytes_per_block,
                           tile_row_bytes) != 0;
            }
            if (mismatch) {
                printf("  %s: tile (%u, %u) of level %u differs\n", name, tx, ty, level);
                break;
            }
        }
    }
    EXPECT(name, 0, mismatch);
    free(storage);
    free(linear);
    free(tile);
}

int main(void) {
    // Mario Kart World's terrain arrays: ASTC 8x8 (16 bytes per 8x8 block), 8192 wide: a 64 KB tile is 64x64 blocks = 512x512 texels, the level is 16x16 tiles
    nvn_texdesc mkw = {
        .target = 4, .format = 0x8e, .flags = 0x20, .width = 8192, .height = 8192, .depth = 68, .levels = 12};
    uint32_t tile_width, tile_height, tile_depth, cols, rows;
    EXPECT("MKW ASTC 8x8: tiled levels", 5,
           nvn_tex_sparse_tiles(&mkw, &tile_width, &tile_height, &tile_depth)); // 8192 .. 512 wide
    EXPECT("MKW: tile width", 512, tile_width);
    EXPECT("MKW: tile height", 512, tile_height);
    EXPECT("MKW: level 0 grid", 1, nvn_tex_tile_grid(&mkw, 0, &cols, &rows));
    EXPECT("MKW: level 0 cols", 16, cols);
    EXPECT("MKW: level 0 rows", 16, rows);
    EXPECT("MKW: level 4 grid", 1, nvn_tex_tile_grid(&mkw, 4, &cols, &rows));
    EXPECT("MKW: level 4 cols", 1, cols);
    EXPECT("MKW: level 4 rows", 1, rows);
    EXPECT("MKW: level 5 is mip tail", 0, nvn_tex_tile_grid(&mkw, 5, &cols, &rows));
    nvn_texlayout layout;
    nvn_tex_layout(&mkw, &layout);
    EXPECT("MKW: layer size is whole tiles", 0, layout.layer_size & 0xFFFF);
    EXPECT("MKW: level 1 at 256 tiles", 256ull << 16, nvn_tex_level_offset(&mkw, 1));
    // A smaller stand-in of the same shape class for the byte check (8192x8192 would decode 16 MB per level): 1024x1024 ASTC 8x8, 2x2 tiles at level 0, one at level 1
    nvn_texdesc small = {
        .target = 4, .format = 0x8e, .flags = 0x20, .width = 1024, .height = 1024, .depth = 2, .levels = 4};
    EXPECT("small: tiled levels", 2, nvn_tex_sparse_tiles(&small, &tile_width, &tile_height, &tile_depth));
    for (int switch2_order = 0; switch2_order < 2; switch2_order++) {
        nvn_gob_switch2 = switch2_order;
        tiles_match(switch2_order ? "small level 0 layer 1 (Switch 2 GOBs)" : "small level 0 layer 1", &small, 0, 1);
        tiles_match(switch2_order ? "small level 1 (Switch 2 GOBs)" : "small level 1", &small, 1, 0);
    }
    nvn_gob_switch2 = 0;
    // Monster Hunter Rise's streamed 1024x1024 ASTC 6x6 (no packaged layout): 48 GOBs wide = 3 tiles of 16 GOBs (64 blocks = 384 texels) across, 1024 / 6 = 171 block rows in 3 rows of 64; level 1 at 0x90000
    nvn_texdesc mhr = {
        .target = 1, .format = 0x7d, .flags = 0x20, .width = 1024, .height = 1024, .depth = 1, .levels = 11};
    nvn_tex_sparse_tiles(&mhr, &tile_width, &tile_height, &tile_depth);
    EXPECT("MHR: tile width", 384, tile_width);
    EXPECT("MHR: tile height", 384, tile_height);
    EXPECT("MHR: level 0 grid", 1, nvn_tex_tile_grid(&mhr, 0, &cols, &rows));
    EXPECT("MHR: cols", 3, cols);
    EXPECT("MHR: rows", 3, rows);
    EXPECT("MHR: level 1 offset", 0x90000, nvn_tex_level_offset(&mhr, 1));
    tiles_match("MHR level 0 (edge tiles clipped)", &mhr, 0, 0);
    // A 2D RGBA8 sparse texture (4 bytes per texel): tiles of 128x128 texels (block height 16 GOBs), a level 1 tile grid of 2x2 for 512x512
    nvn_texdesc rgba = {
        .target = 1, .format = 0x25, .flags = 0x20, .width = 512, .height = 512, .depth = 1, .levels = 3};
    nvn_tex_sparse_tiles(&rgba, &tile_width, &tile_height, &tile_depth);
    EXPECT("RGBA8: tile width", 128, tile_width);
    EXPECT("RGBA8: tile height", 128, tile_height);
    EXPECT("RGBA8: level 1 grid", 1, nvn_tex_tile_grid(&rgba, 1, &cols, &rows));
    EXPECT("RGBA8: level 1 cols", 2, cols);
    tiles_match("RGBA8 level 1", &rgba, 1, 0);
    // Not sparse: no grid
    nvn_texdesc plain = {.target = 1, .format = 0x25, .flags = 0, .width = 512, .height = 512, .depth = 1, .levels = 3};
    EXPECT("not sparse: no grid", 0, nvn_tex_tile_grid(&plain, 0, &cols, &rows));
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
