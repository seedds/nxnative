// 见 nvn_bcdec.h。解码用 bcdec（dm/third_party/bcdec，单头文件，MIT / 公有领域）。
// See nvn_bcdec.h. Decoding uses bcdec (dm/third_party/bcdec, single header, MIT / public domain).
#include "nvn_bcdec.h"
#include <stdlib.h>
#include <string.h>

enum { R8Unorm = 10, R8Snorm = 12, RG8Unorm = 30, RG8Snorm = 32, RGBA8Unorm = 70, RGBA8Unorm_sRGB = 71, RGB9E5Float = 93, RGBA16Float = 115 };

// Path 1 (GPU Direct BC): No CPU unpacking or decoding
int nvn_bc_unpacked_gpu(uint32_t f, uint32_t *pf, int *bpp) {
    (void)f; (void)pf; (void)bpp;
    return 0;
}

int nvn_bc_decode_gpu(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch) {
    (void)f; (void)src; (void)pitch; (void)w; (void)h; (void)dst; (void)dpitch;
    return -1;
}

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE   // BC4 / BC5 带有符号版本的接口 | interface with signed variants of BC4 / BC5
#include "../../third_party/bcdec/bcdec.h"
#include <math.h>

#define RGB9E5_MANTISSA_BITS          9
#define RGB9E5_EXP_BIAS               15
#define RGB9E5_MAX_VALID_BIASED_EXP   31
#define MAX_RGB9E5_EXP                (RGB9E5_MAX_VALID_BIASED_EXP - RGB9E5_EXP_BIAS)
#define RGB9E5_MANTISSA_VALUES        (1 << RGB9E5_MANTISSA_BITS)
#define MAX_RGB9E5_MANTISSA           (RGB9E5_MANTISSA_VALUES - 1)
#define MAX_RGB9E5                    (((float)(MAX_RGB9E5_MANTISSA)) / RGB9E5_MANTISSA_VALUES * (1 << MAX_RGB9E5_EXP))

static inline uint32_t float3_to_rgb9e5(float r, float g, float b) {
    float rc = fminf(fmaxf(r, 0.0f), MAX_RGB9E5);
    float gc = fminf(fmaxf(g, 0.0f), MAX_RGB9E5);
    float bc = fminf(fmaxf(b, 0.0f), MAX_RGB9E5);
    float maxrgb = fmaxf(rc, fmaxf(gc, bc));
    int exp_shared;
    if (maxrgb < 1.0f / (1 << (RGB9E5_EXP_BIAS - 1))) {
        exp_shared = 0;
    } else {
        int exp;
        frexpf(maxrgb, &exp);
        exp_shared = exp + RGB9E5_EXP_BIAS;
        if (exp_shared > RGB9E5_MAX_VALID_BIASED_EXP) exp_shared = RGB9E5_MAX_VALID_BIASED_EXP;
    }
    float scale = ldexpf(1.0f, RGB9E5_EXP_BIAS + RGB9E5_MANTISSA_BITS - exp_shared);
    int maxm = (int)floorf(maxrgb * scale + 0.5f);
    if (maxm == RGB9E5_MANTISSA_VALUES) {
        scale *= 0.5f;
        exp_shared++;
        if (exp_shared > RGB9E5_MAX_VALID_BIASED_EXP) exp_shared = RGB9E5_MAX_VALID_BIASED_EXP;
    }
    int rm = (int)floorf(rc * scale + 0.5f);
    int gm = (int)floorf(gc * scale + 0.5f);
    int bm = (int)floorf(bc * scale + 0.5f);
    if (rm > MAX_RGB9E5_MANTISSA) rm = MAX_RGB9E5_MANTISSA;
    if (gm > MAX_RGB9E5_MANTISSA) gm = MAX_RGB9E5_MANTISSA;
    if (bm > MAX_RGB9E5_MANTISSA) bm = MAX_RGB9E5_MANTISSA;
    return (uint32_t)rm | ((uint32_t)gm << 9) | ((uint32_t)bm << 18) | ((uint32_t)exp_shared << 27);
}

// Path 2 (Standard CPU decode): BC1-7 standard, BC6H -> RGBA16Float (8 bpp)
int nvn_bc_unpacked_standard(uint32_t f, uint32_t *pf, int *bpp) {
    uint32_t dummy_pf; int dummy_bpp;
    if (!pf) pf = &dummy_pf;
    if (!bpp) bpp = &dummy_bpp;
    switch (f) {
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x4d: *pf = RGBA8Unorm; *bpp = 4; return 1;
    case 0x45: case 0x46: case 0x47: case 0x48: case 0x4e: *pf = RGBA8Unorm_sRGB; *bpp = 4; return 1;
    case 0x49: *pf = R8Unorm; *bpp = 1; return 1;
    case 0x4a: *pf = R8Snorm; *bpp = 1; return 1;
    case 0x4b: *pf = RG8Unorm; *bpp = 2; return 1;
    case 0x4c: *pf = RG8Snorm; *bpp = 2; return 1;
    case 0x4f: case 0x50: *pf = RGBA16Float; *bpp = 8; return 1;
    default: return 0;
    }
}

static void block_standard(uint32_t f, const uint8_t *b, uint8_t *o, int op) {
    switch (f) {
    case 0x41: case 0x42: case 0x45: case 0x46: bcdec_bc1(b, o, op); break;
    case 0x43: case 0x47: bcdec_bc2(b, o, op); break;
    case 0x44: case 0x48: bcdec_bc3(b, o, op); break;
    case 0x49: bcdec_bc4(b, o, op, 0); break;
    case 0x4a: bcdec_bc4(b, o, op, 1); break;
    case 0x4b: bcdec_bc5(b, o, op, 0); break;
    case 0x4c: bcdec_bc5(b, o, op, 1); break;
    case 0x4d: case 0x4e: bcdec_bc7(b, o, op); break;
    case 0x4f: case 0x50: {
        uint16_t rgb[16 * 3]; bcdec_bc6h_half(b, rgb, 4 * 3, f == 0x4f);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint16_t *d = (uint16_t *)(o + y * op) + x * 4; const uint16_t *s = rgb + (y * 4 + x) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0x3c00;
        }
        break; }
    }
}

int nvn_bc_decode_standard(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch) {
    uint32_t pf; int bpp; if (!nvn_bc_unpacked_standard(f, &pf, &bpp)) return -1;
    int bb = (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4a) ? 8 : 16;
    uint8_t tmp[4 * 4 * 8];
    for (uint32_t by = 0; by * 4 < h; by++) for (uint32_t bx = 0; bx * 4 < w; bx++) {
        const uint8_t *b = src + by * pitch + bx * bb;
        uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4, ch = h - by * 4 < 4 ? h - by * 4 : 4;
        uint8_t *o = dst + (size_t)by * 4 * dpitch + (size_t)bx * 4 * bpp;
        if (cw == 4 && ch == 4) { block_standard(f, b, o, (int)dpitch); continue; }
        block_standard(f, b, tmp, 4 * bpp);
        for (uint32_t y = 0; y < ch; y++) memcpy(o + y * dpitch, tmp + y * 4 * bpp, cw * bpp);
    }
    return 0;
}

// Path 3 (Reduced RAM CPU decode): BC1-7 standard, BC6H -> RGB9E5Float (4 bpp)
int nvn_bc_unpacked_rgb9e5(uint32_t f, uint32_t *pf, int *bpp) {
    uint32_t dummy_pf; int dummy_bpp;
    if (!pf) pf = &dummy_pf;
    if (!bpp) bpp = &dummy_bpp;
    switch (f) {
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x4d: *pf = RGBA8Unorm; *bpp = 4; return 1;
    case 0x45: case 0x46: case 0x47: case 0x48: case 0x4e: *pf = RGBA8Unorm_sRGB; *bpp = 4; return 1;
    case 0x49: *pf = R8Unorm; *bpp = 1; return 1;
    case 0x4a: *pf = R8Snorm; *bpp = 1; return 1;
    case 0x4b: *pf = RG8Unorm; *bpp = 2; return 1;
    case 0x4c: *pf = RG8Snorm; *bpp = 2; return 1;
    case 0x4f: case 0x50: *pf = RGB9E5Float; *bpp = 4; return 1;
    default: return 0;
    }
}

static void block_rgb9e5(uint32_t f, const uint8_t *b, uint8_t *o, int op) {
    switch (f) {
    case 0x41: case 0x42: case 0x45: case 0x46: bcdec_bc1(b, o, op); break;
    case 0x43: case 0x47: bcdec_bc2(b, o, op); break;
    case 0x44: case 0x48: bcdec_bc3(b, o, op); break;
    case 0x49: bcdec_bc4(b, o, op, 0); break;
    case 0x4a: bcdec_bc4(b, o, op, 1); break;
    case 0x4b: bcdec_bc5(b, o, op, 0); break;
    case 0x4c: bcdec_bc5(b, o, op, 1); break;
    case 0x4d: case 0x4e: bcdec_bc7(b, o, op); break;
    case 0x4f: case 0x50: {
        float rgb[16 * 3]; bcdec_bc6h_float(b, rgb, 4 * 3, f == 0x4f);
        for (int y = 0; y < 4; y++) {
            uint32_t *d = (uint32_t *)(o + y * op);
            for (int x = 0; x < 4; x++) {
                const float *s = rgb + (y * 4 + x) * 3;
                d[x] = float3_to_rgb9e5(s[0], s[1], s[2]);
            }
        }
        break; }
    }
}

int nvn_bc_decode_rgb9e5(uint32_t f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch) {
    uint32_t pf; int bpp; if (!nvn_bc_unpacked_rgb9e5(f, &pf, &bpp)) return -1;
    int bb = (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4a) ? 8 : 16;
    uint8_t tmp[4 * 4 * 8];
    for (uint32_t by = 0; by * 4 < h; by++) for (uint32_t bx = 0; bx * 4 < w; bx++) {
        const uint8_t *b = src + by * pitch + bx * bb;
        uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4, ch = h - by * 4 < 4 ? h - by * 4 : 4;
        uint8_t *o = dst + (size_t)by * 4 * dpitch + (size_t)bx * 4 * bpp;
        if (cw == 4 && ch == 4) { block_rgb9e5(f, b, o, (int)dpitch); continue; }
        block_rgb9e5(f, b, tmp, 4 * bpp);
        for (uint32_t y = 0; y < ch; y++) memcpy(o + y * dpitch, tmp + y * 4 * bpp, cw * bpp);
    }
    return 0;
}

// Initialized to standard CPU decode by default
nvn_bc_unpacked_fn g_nvn_bc_unpacked = nvn_bc_unpacked_standard;
nvn_bc_decode_fn g_nvn_bc_decode = nvn_bc_decode_standard;
