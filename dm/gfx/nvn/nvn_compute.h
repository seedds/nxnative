// Translated kernels have a fixed NVN group size. Give Metal that size before
// register allocation: its unconstrained choice can support fewer threads than
// the kernel dispatches (768 versus 1024 on M1 for Trails' culling shader).
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>

static inline id<MTLComputePipelineState> nvn_compute_pipeline(id<MTLDevice> device, id<MTLFunction> function, const uint16_t tg[3], NSError **error) {
    if (!tg || !tg[0] || !tg[1] || !tg[2]) return [device newComputePipelineStateWithFunction:function error:error];
    MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = function;
    descriptor.maxTotalThreadsPerThreadgroup = (NSUInteger)tg[0] * tg[1] * tg[2];
    return [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:error];
}
