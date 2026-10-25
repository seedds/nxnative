// Traditional Metal binary archives. No Metal 4 APIs or game-state serialization.
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t hits, misses, bypassed, added, saved;
    int loaded, write_failed, disabled;
} nvn_cache_status;
void nvn_cache_snapshot(nvn_cache_status *out);
void nvn_cache_flush(void); // Worker-thread only; atomic replacement preserves the last good file.
void nvn_cache_flush_async(void);
#ifdef __OBJC__
#import <Metal/Metal.h>
id<MTLRenderPipelineState> nvn_cached_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **error);
id<MTLRenderPipelineState> nvn_cached_render_specialized(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, int specialized, NSError **error);
id<MTLComputePipelineState> nvn_cached_compute(id<MTLDevice> device, id<MTLFunction> function, NSError **error);
#endif
