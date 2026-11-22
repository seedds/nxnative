//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   The Switch 2 (NVN2) orders a GOB's 16-byte pieces otherwise: (x%64/32)·256 + (y%8/4)·128 + (x%32/16)·64 + (y%4/2)·32 + (y%2)·16 + x%16 (nvn_gob_order)
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y, int switch2) {
    if (switch2) return (x % 64 / 32) * 256 + (y % 8 / 4) * 128 + (x % 32 / 16) * 64 + (y % 4 / 2) * 32 + (y % 2) * 16 + x % 16;
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

// Byte offsets of a 16-byte segment in a GOB, split into its column and row.
typedef struct { uint16_t x[4], y[8]; } gob_segments;
static const gob_segments g_segments[2] = {
    {{0, 32, 256, 288}, {0, 16, 64, 80, 128, 144, 192, 208}},
    {{0, 64, 256, 320}, {0, 16, 32, 48, 128, 144, 160, 176}},
};

// Copy one row using constant-sized operations, leaving a short segment only at a clipped edge.
static inline void move_bl_row(uint8_t *bl, uint8_t *lin, uint32_t bytes, uint64_t block, const uint16_t x[4], int to_linear) {
    uint32_t nfull = bytes >> 4, tail = bytes & 15;
    if (to_linear) {
        for (uint32_t j = 0; j < nfull; j++) memcpy(lin + (j << 4), bl + (j >> 2) * block + x[j & 3], 16);
        if (tail) memcpy(lin + (nfull << 4), bl + (nfull >> 2) * block + x[nfull & 3], tail);
    } else {
        for (uint32_t j = 0; j < nfull; j++) memcpy(bl + (j >> 2) * block + x[j & 3], lin + (j << 4), 16);
        if (tail) memcpy(bl + (nfull >> 2) * block + x[nfull & 3], lin + (nfull << 4), tail);
    }
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td, int switch2) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // which block row
         + (x / 64) * block                                            // which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // which GOB within the block
         + gob_off(x, y, switch2);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || (d->flags & 0x50) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR / LINEAR_RENDER_TARGET
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint64_t bl_at, uint8_t *lin, size_t pitch, int to_linear) {   // bl: the storage from byte bl_at on
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes || base < bl_at) return -1;
    bl += base - bl_at; uint32_t lb = nvn_tex_level_gobs(d, level) * 64;   // the row as laid out (padded in the tiled levels of a sparse texture or a packaged layout with width alignment)
    if (lb < row_bytes) lb = row_bytes;
    // Slice and row offsets are shared by every segment in the row.
    const gob_segments *gob = &g_segments[nvn_gob_order(d) != 0];
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t block = 512ull * bh * bd, gobs_x = (uint64_t)(lb + 63) >> 6, blocks_y = ((uint64_t)rows + 8 * bh - 1) >> (3 + th);
    for (uint32_t z = 0; z < slices; z++) {
        uint64_t zb = (uint64_t)(z >> td) * gobs_x * blocks_y * block + (uint64_t)(z & (bd - 1)) * bh * 512;
        for (uint32_t y = 0; y < rows; y++) {
            uint64_t p = (uint64_t)(y >> (3 + th)) * gobs_x * block + (uint64_t)((y & (8 * bh - 1)) >> 3) * 512 + gob->y[y & 7];
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            move_bl_row(bl + zb + p, row, row_bytes, block, gob->x, to_linear);
        }
    }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, 0, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, 0, (uint8_t *)src, src_pitch, 0);
}

// Every step-th block of every step-th block row of a level and layer (2D, one slice), from half a step in: the block in the middle of each step x step cell. nx x ny blocks, rows of
// nx blocks in dst. A block lies in one 16-byte piece of the storage, so only block sizes of 1, 2, 4, 8 and 16 bytes (-1 for others, and what nvn_deswizzle refuses)
int nvn_deswizzle_pick(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint32_t step, void *dst, uint32_t nx, uint32_t ny) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (!step || nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || slices != 1 || bpb > 16 || (bpb & (bpb - 1))) return -1;
    uint32_t lb = nvn_tex_level_gobs(d, level) * 64, bx = row_bytes / bpb;   // lb: the row as laid out (move)
    if (lb < row_bytes) lb = row_bytes;
    const uint8_t *bl = (const uint8_t *)src + base; uint8_t *o = dst;
    for (uint32_t j = 0; j < ny; j++) {
        uint32_t y = j * step + step / 2; if (y >= rows) y = rows - 1;
        for (uint32_t i = 0; i < nx; i++, o += bpb) { uint32_t x = i * step + step / 2; if (x >= bx) x = bx - 1; memcpy(o, bl + nvn_bl_offset(x * bpb, y, 0, lb, rows, th, td, nvn_gob_order(d)), bpb); }
    }
    return 0;
}

// One 64 KB tile of a sparse texture's tiled level (2D, one slice). Such a level is laid out with its width padded to whole tiles (nvn_tex.c level_walk), and a tile is one block row of
// 128 / block height GOB columns: 64 KB of its own, at the level's start + (ty × tiles across + tx) × 64 KB. So it decodes as a block-linear surface of that size; the part inside the level
// goes to dst (block rows of pitch bytes). *wb and *rows: the bytes and block rows written (0 for a tile past the level)
int nvn_deswizzle_tile(const nvn_texdesc *d, unsigned level, uint32_t tx, uint32_t ty, const void *tile, void *dst, size_t pitch, uint32_t *wb, uint32_t *nrows) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    *wb = *nrows = 0;
    if (nvn_deswizzle_geom(d, level, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || td || slices != 1 || th > 7) return -1;
    uint32_t trb = (128u >> th) * 64, trows = 8u << th, x0 = tx * trb, y0 = ty * trows;
    if (x0 >= row_bytes || y0 >= rows) return 0;
    uint32_t w = row_bytes - x0 < trb ? row_bytes - x0 : trb, h = rows - y0 < trows ? rows - y0 : trows;
    if (pitch < w) return -1;
    const gob_segments *gob = &g_segments[nvn_gob_order(d) != 0];
    uint64_t block = 512ull << th;
    // The tile is one block row. Only the position within its GOB column varies with y.
    for (uint32_t y = 0; y < h; y++) {
        uint64_t offset = (uint64_t)(y >> 3) * 512 + gob->y[y & 7];
        move_bl_row((uint8_t *)tile + offset, (uint8_t *)dst + (size_t)y * pitch, w, block, gob->x, 1);
    }
    *wb = w; *nrows = h;
    return 0;
}
// nvn_deswizzle with the storage given from byte src_at on (a sparse texture's mip tail gathered on its own): the level must lie at or after it
int nvn_deswizzle_from(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint64_t src_at, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, src_at, (uint8_t *)dst, dst_pitch, 1);
}
