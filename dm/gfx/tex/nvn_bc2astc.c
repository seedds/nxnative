// See nvn_bc2astc.h. The transcoder itself is bc2astc_core.h.
#include "nvn_bc2astc.h"
#include "bc2astc_core.h"
#include <string.h>

int nvn_bc_astc_fmt(uint32_t f, uint32_t *pf, int *kind) {
    // NVN: 0x41 RGB_DXT1, 0x42 RGBA_DXT1, 0x43 DXT3, 0x44 DXT5, 0x45–0x48 the same in sRGB
    if (f < 0x41 || f > 0x48) return 0;
    *pf = f >= 0x45 ? 186 : 204;
    *kind = (f & 3) == 1 || (f & 3) == 2 ? 1 : ((f & 3) == 3 ? 2 : 3);
    return 1;
}

uint32_t nvn_bc2astc_block(int kind, const uint8_t *bc, uint8_t *astc) {
    uint32_t w[4] = {0, 0, 0, 0}, o[4];
    memcpy(w, bc, kind == 1 ? 8 : 16);
    uint32_t r = bca_transcode(w, (u32)kind, o);
    memcpy(astc, o, 16);
    return r;
}

int nvn_bc2astc(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch) {
    uint32_t pf; int kind; if (!nvn_bc_astc_fmt(f, &pf, &kind)) return -1;
    size_t bb = kind == 1 ? 8 : 16;
    for (uint32_t by = 0; by * 4 < h; by++) for (uint32_t bx = 0; bx * 4 < w; bx++)
        nvn_bc2astc_block(kind, src + by * pitch + bx * bb, dst + by * dpitch + (size_t)bx * 16);
    return 0;
}

void nvn_astc_zero_block(uint8_t out[16]) {
    static const uint8_t z[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // void-extent block, colour 0
    memcpy(out, z, 16);
}
