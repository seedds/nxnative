// GPUs without BC support: BC1 / BC2 / BC3 (NVN 0x41–0x48) are stored as ASTC 4x4 (8 bits per texel) by transcoding every BC block into one ASTC block, which is 4× smaller than RGBA8.
// The algorithm is bc2astc_core.h, which the GPU kernel (nvn_mtl.m) compiles from the same text; this file is the CPU side (sparse pools, copy_b2t, tests) and the reference for the kernel.
#pragma once
#include <stddef.h>
#include <stdint.h>

// NVN format is BC1/2/3 → returns 1, *mtl_pf = MTLPixelFormatASTC_4x4_LDR (204) or ASTC_4x4_sRGB (186), *kind = 1/2/3 (BC1/BC2/BC3); returns 0 for every other format
int nvn_bc_astc_fmt(uint32_t nvn_fmt, uint32_t *mtl_pf, int *kind);
// One block: bc has 8 bytes (kind 1) or 16 bytes (kind 2, 3), astc receives 16 bytes. Returns the layout id of bc2astc_core.h (| BCA_SWAPPED)
uint32_t nvn_bc2astc_block(int kind, const uint8_t *bc, uint8_t *astc);
// A w×h texel region: src has pitch bytes per block row, dst dpitch bytes per block row (16 per ASTC block). Returns 0 on success, -1 if the format is not BC1/2/3
int nvn_bc2astc(uint32_t nvn_fmt, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch);
// The block that ASTC textures are filled with where BC textures are zero-filled: a constant (0,0,0,0) block (an all-zero ASTC block is invalid)
void nvn_astc_zero_block(uint8_t out[16]);
