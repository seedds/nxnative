#pragma once
#include <stddef.h>
#include <stdint.h>
#import <Metal/Metal.h>
#include "nvn_deswizzle.h"

int nvn_deswz_gpu_init(id<MTLDevice> dev);
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
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, unsigned layer,
                            id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch);
void nvn_deswz_ring_dump(void);   // the last deswizzle jobs and the bounds refusals, at the first upload command buffer error
int nvn_deswz_check(const nvn_texdesc *d, unsigned level, unsigned layer, uint64_t src_gpu, uint64_t src_off, uint64_t src_len, uint64_t dst_off, uint64_t dst_len, size_t dst_pitch);   // 0 fine, -2 refused (out of bounds)
