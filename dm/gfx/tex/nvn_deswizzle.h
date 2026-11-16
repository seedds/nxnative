// Tegra block-linear ↔ linear. Level / layer offsets and per-level block height and block depth always come from nvn/nvn_tex.c (nvn_tex_level_offset / nvn_tex_layout / nvn_tex_level_tiles).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../nvn/nvn_tex.h"

// Decode level number level, layer number layer (layer of an array / cube; 0 for non-layered textures) from the whole-texture storage src_storage into linear dst_linear.
// dst is laid out by block rows: dst_pitch bytes per row (≥ width in blocks of this level × bytes per block); for compressed formats one row = one row of blocks.
// 3D textures: all depth slices of this level are decoded at once; slice k is at dst_linear + k × dst_pitch × block row count of this level.
// Not supported: multisampling, the LINEAR flag (already linear), BUFFER; returns -1 on these, 0 on success.
int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch);
// Reverse (for unit test round trips): linear src_linear → written into the corresponding level / layer of the whole-texture storage dst_storage
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch, void *dst_storage);
// Single-point address: within this level and layer, linear (x byte, y block row, z slice) → block-linear byte offset relative to the start of this level and layer (unit tests verify against hand calculation).
// row_bytes: the row as laid out, which a sparse texture's tiled levels pad to whole tiles (nvn_tex_level_gobs × 64)
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned tileH, unsigned tileD);
// The same address for the bytes of one block row of slice 0, with the row's terms computed once (nvn_bl_row_of) and each byte's by shifts and a table
// (nvn_bl_row_at): nvn_bl_row_at(&r, x) == nvn_bl_offset(x, y, 0, row_bytes, rows, tileH, tileD) for r = nvn_bl_row_of(y, row_bytes, tileH, tileD). For a writer that
// addresses a region row by row (WriteTexels), where nvn_bl_offset's divisions by the block height for every 16-byte run cost more than the copy
typedef struct { uint64_t row, block; } nvn_bl_row;
nvn_bl_row nvn_bl_row_of(uint32_t y, uint32_t row_bytes, unsigned tileH, unsigned tileD);
static inline uint64_t nvn_bl_row_at(const nvn_bl_row *r, uint32_t x) {
    static const uint16_t kox[4] = {0, 32, 256, 288};   // (x%64/32)*256 + (x%32/16)*32 by x%64/16
    return r->row + (uint64_t)(x >> 6) * r->block + kox[(x >> 4) & 3] + (x & 15);
}

int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices,
                       uint64_t *base, unsigned *tileH, unsigned *tileD, uint32_t *bpb);
