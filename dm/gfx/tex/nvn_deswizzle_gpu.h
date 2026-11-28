#pragma once
#include <stddef.h>
#include <stdint.h>
#import <Metal/Metal.h>
#include "nvn_deswizzle.h"

int nvn_deswz_gpu_init(id<MTLDevice> dev);
int nvn_deswz_gpu_sparse_supported(void);
// Small RGBA16F arrays: write raw component bits through an integer texture
// view, across all layers in one dispatch per mip (no per-face staging/copies).
int nvn_deswz_gpu_direct_supported(const nvn_texdesc *d);
int nvn_deswz_gpu_direct_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, id<MTLTexture> uint_view);
int nvn_deswz_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, uint64_t src_off,
                         const nvn_texdesc *d, unsigned level, unsigned layer,
                         id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch);
int nvn_deswz_gpu_encode_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off, int sparse,
                            const nvn_texdesc *d, unsigned level, unsigned layer,
                            id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch);

// Encode one depth-block-aligned region. va stays at the origin of the full
// page table; base includes the byte offset of the region in that storage.
void nvn_deswz_gpu_sparse_region_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
        id<MTLBuffer> params, size_t params_off, id<MTLBuffer> pages, uint64_t va, uint64_t base,
        id<MTLBuffer> dst, uint64_t dst_off, uint32_t row, uint32_t rows, uint32_t depth,
        unsigned th, unsigned td, unsigned columns, unsigned gob_order);
