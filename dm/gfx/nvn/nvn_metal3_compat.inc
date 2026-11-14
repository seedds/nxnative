// Experimental adapter derived from Llllll07/nxnative-ios18-check,
// bayonetta-metal3-performance, 44e7da0 (MIT; upstream LICENSE applies).
// Metal 3 implementation of the replay's queue / table / encoder abstraction.
// No Metal 4 objects are allocated. Classic Metal owns command-buffer allocation,
// tracks resource hazards, and encodes copies on blit encoders. GPU-address binds
// are resolved back to their registered MTLBuffer and offset before every draw.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

static NSRecursiveLock *nx3_registry_lock;
static NSHashTable *nx3_resources;
static NSMapTable *nx3_ids;
@interface NX3BufferRange : NSObject
@property(weak) id<MTLBuffer> buffer;
@property uint64_t lo, hi;
@end
@implementation NX3BufferRange @end
@interface NX3ResourceSnapshot : NSObject
@property(strong) NSArray *objects;
@property uint64_t generation;
@end
@implementation NX3ResourceSnapshot @end
static NSMutableArray<NX3BufferRange *> *nx3_buffer_ranges;
static _Atomic uint64_t nx3_resource_generation;
static _Atomic uint64_t nx3_lookups, nx3_binding_sets, nx3_binding_skips, nx3_declarations;
static void nx3_register(id object) {
    if (!object) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ nx3_registry_lock = [NSRecursiveLock new]; nx3_resources = [NSHashTable weakObjectsHashTable]; nx3_ids = [NSMapTable strongToWeakObjectsMapTable]; nx3_buffer_ranges = [NSMutableArray array]; });
    [nx3_registry_lock lock];
    // A view has its own descriptor ID but shares residency/hazards with its
    // underlying allocation (the same parent/buffer walk as res_check).
    id allocation = object;
    if ([object conformsToProtocol:@protocol(MTLTexture)]) {
        id<MTLTexture> texture = object;
        while (texture.parentTexture) texture = texture.parentTexture;
        allocation = texture.buffer ?: (id)texture;
    }
    if ([allocation conformsToProtocol:@protocol(MTLResource)] && ![nx3_resources containsObject:allocation]) {
        [nx3_resources addObject:allocation];
        if ([allocation conformsToProtocol:@protocol(MTLBuffer)]) {
            id<MTLBuffer> buffer = allocation;
            // Only allocation changes the address index. Draws binary-search it
            // instead of enumerating every texture and buffer in the registry.
            if (nx3_buffer_ranges.count > 4096) {
                NSIndexSet *dead = [nx3_buffer_ranges indexesOfObjectsPassingTest:^BOOL(NX3BufferRange *r, NSUInteger i, BOOL *stop) { return r.buffer == nil; }];
                [nx3_buffer_ranges removeObjectsAtIndexes:dead];
            }
            NX3BufferRange *r = [NX3BufferRange new]; r.buffer = buffer; r.lo = buffer.gpuAddress; r.hi = r.lo + buffer.length;
            NSUInteger lo = 0, hi = nx3_buffer_ranges.count;
            while (lo < hi) { NSUInteger m = (lo + hi) / 2; if (nx3_buffer_ranges[m].lo <= r.lo) lo = m + 1; else hi = m; }
            [nx3_buffer_ranges insertObject:r atIndex:lo];
        }
        atomic_fetch_add_explicit(&nx3_resource_generation, 1, memory_order_release);
    }
    if ([object respondsToSelector:@selector(gpuResourceID)]) {
        MTLResourceID rid = [(id<MTLTexture>)object gpuResourceID];
        if (rid._impl) [nx3_ids setObject:object forKey:@(rid._impl)];
    }
    [nx3_registry_lock unlock];
}
static void nx3_remove(id object) {
    [nx3_registry_lock lock];
    // Retiring a view must not retire its still-live parent allocation.
    if ([nx3_resources containsObject:object]) {
        [nx3_resources removeObject:object];
        atomic_fetch_add_explicit(&nx3_resource_generation, 1, memory_order_release);
    }
    [nx3_registry_lock unlock];
}
static NSArray *nx3_snapshot(NX3ResourceSnapshot *snapshot) {
    uint64_t generation = atomic_load_explicit(&nx3_resource_generation, memory_order_acquire);
    if (!snapshot.objects || snapshot.generation != generation) {
        [nx3_registry_lock lock]; snapshot.objects = nx3_resources.allObjects;
        snapshot.generation = atomic_load_explicit(&nx3_resource_generation, memory_order_relaxed); [nx3_registry_lock unlock];
    }
    return snapshot.objects;
}
// Views and buffer-backed textures are made on MTLTexture/MTLBuffer, bypassing
// NX3Device. Register every returned texture before a table consumes its ID.
static id<MTLTexture> nx3_track_texture(id<MTLTexture> texture) { nx3_register(texture); return texture; }
static id nx3_id(uint64_t value) {
    [nx3_registry_lock lock]; id object = [nx3_ids objectForKey:@(value)]; [nx3_registry_lock unlock];
    if (value && !object) { fprintf(stderr, "[RED] Metal3 unresolved resource ID 0x%llx\n", (unsigned long long)value); abort(); }
    return object;
}
static id<MTLBuffer> nx3_buffer(uint64_t address, NSUInteger *offset) {
    if (!address) { *offset = 0; return nil; }
    atomic_fetch_add_explicit(&nx3_lookups, 1, memory_order_relaxed);
    [nx3_registry_lock lock];
    id<MTLBuffer> selected = nil;
    NSUInteger lo = 0, hi = nx3_buffer_ranges.count;
    while (lo < hi) { NSUInteger m = (lo + hi) / 2; if (nx3_buffer_ranges[m].lo <= address) lo = m + 1; else hi = m; }
    while (lo) {
        NX3BufferRange *r = nx3_buffer_ranges[--lo];
        if (address < r.hi && (selected = r.buffer)) { *offset = address - r.lo; break; }
    }
    [nx3_registry_lock unlock];
    if (!selected) { fprintf(stderr, "[RED] Metal3 unresolved GPU buffer address 0x%llx\n", (unsigned long long)address); abort(); }
    return selected;
}
static void nx3_stats_line(char *out, size_t size) {
    snprintf(out, size, "buffer lookups %llu, native binding writes %llu, unchanged bindings reused %llu, resource declaration batches %llu",
        (unsigned long long)atomic_exchange(&nx3_lookups, 0), (unsigned long long)atomic_exchange(&nx3_binding_sets, 0),
        (unsigned long long)atomic_exchange(&nx3_binding_skips, 0), (unsigned long long)atomic_exchange(&nx3_declarations, 0));
}

@interface NX3AllocatorDescriptor : NSObject
@property(copy) NSString *label;
@end
@implementation NX3AllocatorDescriptor @end
@protocol NX3Allocator <NSObject>
@property(readonly) NSUInteger allocatedSize;
- (void)reset;
@end
@interface NX3Allocator : NSObject<NX3Allocator> @end
@implementation NX3Allocator
- (NSUInteger)allocatedSize { return 0; } // Classic command buffers own their allocations.
- (void)reset {} // Their lifetime is the real MTLCommandBuffer's completion.
@end
@interface NX3TableDescriptor : NSObject
@property NSUInteger maxBufferBindCount, maxTextureBindCount, maxSamplerStateBindCount;
@end
@implementation NX3TableDescriptor @end
@protocol NX3Table <NSObject>
- (void)setAddress:(uint64_t)value atIndex:(NSUInteger)index;
- (void)setTexture:(MTLResourceID)value atIndex:(NSUInteger)index;
- (void)setSamplerState:(MTLResourceID)value atIndex:(NSUInteger)index;
@end
@interface NX3Table : NSObject<NX3Table> {
@public uint64_t addresses[31], textures[128], samplers[128];
    uint32_t addressMask; uint64_t textureMask[2], samplerMask[2];
}
@end
@implementation NX3Table
- (void)setAddress:(uint64_t)v atIndex:(NSUInteger)i { if (i >= 31) abort(); addresses[i] = v; addressMask |= 1u << i; }
- (void)setTexture:(MTLResourceID)v atIndex:(NSUInteger)i { if (i >= 128) abort(); textures[i] = v._impl; textureMask[i / 64] |= 1ull << (i % 64); }
- (void)setSamplerState:(MTLResourceID)v atIndex:(NSUInteger)i { if (i >= 128) abort(); samplers[i] = v._impl; samplerMask[i / 64] |= 1ull << (i % 64); }
@end
@protocol NX3Encoder <NSObject>
@property(copy) NSString *label;
- (void)endEncoding;
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b visibilityOptions:(NSUInteger)v;
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b visibilityOptions:(NSUInteger)v;
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b visibilityOptions:(NSUInteger)v;
@end
@protocol NX3RenderEncoder <NX3Encoder, MTLRenderCommandEncoder>
- (void)drawMeshThreadgroupsWithIndirectBuffer:(uint64_t)a threadsPerObjectThreadgroup:(MTLSize)o threadsPerMeshThreadgroup:(MTLSize)m;
- (void)setArgumentTable:(id<NX3Table>)table atStages:(MTLRenderStages)stages;
- (void)writeTimestampWithGranularity:(NSUInteger)g afterStage:(MTLRenderStages)s intoHeap:(id)h atIndex:(NSUInteger)i;
- (void)drawPrimitives:(MTLPrimitiveType)type indirectBuffer:(uint64_t)address;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t indexBuffer:(uint64_t)a indexBufferLength:(NSUInteger)len instanceCount:(NSUInteger)i baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(uint64_t)a indexBufferLength:(NSUInteger)len indirectBuffer:(uint64_t)ind;
@end
@protocol NX3ComputeEncoder <NX3Encoder, MTLComputeCommandEncoder, MTLBlitCommandEncoder>
- (void)setArgumentTable:(id<NX3Table>)table;
- (void)writeTimestampWithGranularity:(NSUInteger)g intoHeap:(id)h atIndex:(NSUInteger)i;
- (void)dispatchThreadgroupsWithIndirectBuffer:(uint64_t)a threadsPerThreadgroup:(MTLSize)t;
@end
// Argument tables outlive passes and allocations. Metal 4 reads only the
// current pipeline's bindings; resolving every historical slot on Metal 3 can
// dereference retired resources during a scene change. Cache compiler reflection
// once per PSO, including stage-in vertex streams, rather than retaining old
// allocations forever or ignoring unresolved addresses used by the shader.
@interface NX3PipelineBindings : NSObject {
@public uint32_t buffers[5]; uint64_t textures[5][2], samplers[5][2];
}
@end
@implementation NX3PipelineBindings @end
static char nx3_pipeline_bindings_key;
static void nx3_pipeline_stage(NX3PipelineBindings *mask, NSArray<id<MTLBinding>> *bindings, int stage) {
    for (id<MTLBinding> b in bindings) {
        if (!b.used) continue;
        NSUInteger i = b.index;
        if (b.type == MTLBindingTypeBuffer && i < 31) mask->buffers[stage] |= 1u << i;
        else if (b.type == MTLBindingTypeTexture && i < 128) {
            NSUInteger n = [(id<MTLTextureBinding>)b arrayLength];
            for (NSUInteger j = i; j < i + MAX(n, 1u) && j < 128; j++) mask->textures[stage][j / 64] |= 1ull << (j % 64);
        } else if (b.type == MTLBindingTypeSampler && i < 128) mask->samplers[stage][i / 64] |= 1ull << (i % 64);
    }
}
static void nx3_pipeline_save(id pipeline, NX3PipelineBindings *bindings) {
    if (pipeline) objc_setAssociatedObject(pipeline, &nx3_pipeline_bindings_key, bindings, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
@interface NX3Encoder : NSObject {
@public id<MTLCommandBuffer> cb; id native; int kind; // 1 render, 2 compute, 3 blit
    NX3Table *vt, *ft, *ct, *ot, *mt;
    id<MTLComputePipelineState> computePipeline;
    NX3PipelineBindings *pipelineBindings;
    NSUInteger threadgroupLengths[32]; uint32_t threadgroupMask;
    uint64_t boundAddresses[5][31], boundTextures[5][128], boundSamplers[5][128];
    uint32_t validAddresses[5]; uint64_t validTextures[5][2], validSamplers[5][2];
    uint64_t declaredGeneration;
    NX3ResourceSnapshot *resourceSnapshot;
    BOOL reflectedResources;
    __unsafe_unretained id reflectedObjects[256]; MTLResourceUsage reflectedUsage[256]; NSUInteger reflectedCount;
}
- (id)compute;
- (id)blit;
- (void)apply;
@end
@implementation NX3Encoder
- (void)resetBindings { memset(validAddresses, 0, sizeof validAddresses); memset(validTextures, 0, sizeof validTextures); memset(validSamplers, 0, sizeof validSamplers); declaredGeneration = 0; reflectedResources = NO; reflectedCount = 0; }
- (id)compute { if (kind == 3) { [native endEncoding]; native = nil; } if (!native) { native = [cb computeCommandEncoder]; [self resetBindings];
        if (computePipeline) [native setComputePipelineState:computePipeline];
        for (NSUInteger i=0;i<32;i++) if (threadgroupMask & (1u<<i)) [native setThreadgroupMemoryLength:threadgroupLengths[i] atIndex:i]; } kind = 2; return native; }
- (id)blit { if (kind == 2) { [native endEncoding]; native = nil; } if (!native) { native = [cb blitCommandEncoder]; [self resetBindings]; } kind = 3; return native; }
- (BOOL)conformsToProtocol:(Protocol *)p { return p == @protocol(NX3Encoder) || p == @protocol(NX3RenderEncoder) || p == @protocol(NX3ComputeEncoder) || [super conformsToProtocol:p]; }
- (void)applyTable:(NX3Table *)table stage:(int)stage {
    if (!table) return;
    id enc = stage == 2 ? [self compute] : native;
    for (NSUInteger i = 0; i < 31; i++) if ((table->addressMask & (1u << i)) && (!pipelineBindings || (pipelineBindings->buffers[stage] & (1u << i)))) {
        if ((validAddresses[stage] & (1u << i)) && boundAddresses[stage][i] == table->addresses[i]) { atomic_fetch_add_explicit(&nx3_binding_skips, 1, memory_order_relaxed); continue; }
        NSUInteger offset = 0; id<MTLBuffer> buffer = nx3_buffer(table->addresses[i], &offset);
        if (stage == 0) [enc setVertexBuffer:buffer offset:offset atIndex:i];
        else if (stage == 1) [enc setFragmentBuffer:buffer offset:offset atIndex:i];
        else if (stage == 3) [enc setObjectBuffer:buffer offset:offset atIndex:i];
        else if (stage == 4) [enc setMeshBuffer:buffer offset:offset atIndex:i];
        else [enc setBuffer:buffer offset:offset atIndex:i];
        validAddresses[stage] |= 1u << i; boundAddresses[stage][i] = table->addresses[i]; atomic_fetch_add_explicit(&nx3_binding_sets, 1, memory_order_relaxed);
    }
    for (NSUInteger i = 0; i < 128; i++) {
        if ((table->textureMask[i / 64] & (1ull << (i % 64))) && (!pipelineBindings || (pipelineBindings->textures[stage][i / 64] & (1ull << (i % 64))))) {
            if ((validTextures[stage][i / 64] & (1ull << (i % 64))) && boundTextures[stage][i] == table->textures[i]) { atomic_fetch_add_explicit(&nx3_binding_skips, 1, memory_order_relaxed); }
            else {
            id texture = nx3_id(table->textures[i]);
            if (stage == 0) [enc setVertexTexture:texture atIndex:i]; else if (stage == 1) [enc setFragmentTexture:texture atIndex:i]; else if (stage == 3) [enc setObjectTexture:texture atIndex:i]; else if (stage == 4) [enc setMeshTexture:texture atIndex:i]; else [(id<MTLComputeCommandEncoder>)enc setTexture:texture atIndex:i];
            validTextures[stage][i / 64] |= 1ull << (i % 64); boundTextures[stage][i] = table->textures[i]; atomic_fetch_add_explicit(&nx3_binding_sets, 1, memory_order_relaxed);
            }
        }
        if ((table->samplerMask[i / 64] & (1ull << (i % 64))) && (!pipelineBindings || (pipelineBindings->samplers[stage][i / 64] & (1ull << (i % 64))))) {
            if ((validSamplers[stage][i / 64] & (1ull << (i % 64))) && boundSamplers[stage][i] == table->samplers[i]) { atomic_fetch_add_explicit(&nx3_binding_skips, 1, memory_order_relaxed); }
            else {
            id sampler = nx3_id(table->samplers[i]);
            if (stage == 0) [enc setVertexSamplerState:sampler atIndex:i]; else if (stage == 1) [enc setFragmentSamplerState:sampler atIndex:i]; else if (stage == 3) [enc setObjectSamplerState:sampler atIndex:i]; else if (stage == 4) [enc setMeshSamplerState:sampler atIndex:i]; else [(id<MTLComputeCommandEncoder>)enc setSamplerState:sampler atIndex:i];
            validSamplers[stage][i / 64] |= 1ull << (i % 64); boundSamplers[stage][i] = table->samplers[i]; atomic_fetch_add_explicit(&nx3_binding_sets, 1, memory_order_relaxed);
            }
        }
    }
}
- (void)apply {
    if (kind == 1) { [self applyTable:vt stage:0]; [self applyTable:ft stage:1]; [self applyTable:ot stage:3]; [self applyTable:mt stage:4]; }
    else [self applyTable:ct stage:2];
    if (reflectedResources) return; // encode_args declared the actual pointees with their read/write usage.
    // useResources is an encoder declaration, not a per-draw operation. Repeat
    // only when an allocation/view adds a possible argument-buffer pointee.
    uint64_t generation = atomic_load_explicit(&nx3_resource_generation, memory_order_acquire);
    if (declaredGeneration == generation) return;
    @autoreleasepool {
        if (!resourceSnapshot) resourceSnapshot = [NX3ResourceSnapshot new];
        NSArray *resources = nx3_snapshot(resourceSnapshot); generation = resourceSnapshot.generation;
        NSUInteger count = resources.count;
        if (count) {
            __unsafe_unretained id<MTLResource> objects[count]; [resources getObjects:objects range:NSMakeRange(0, count)];
            if (kind == 1) [(id<MTLRenderCommandEncoder>)native useResources:objects count:count usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex | MTLRenderStageFragment | (ot ? MTLRenderStageObject : 0) | (mt ? MTLRenderStageMesh : 0)];
            else [(id<MTLComputeCommandEncoder>)[self compute] useResources:objects count:count usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        }
        declaredGeneration = generation; atomic_fetch_add_explicit(&nx3_declarations, 1, memory_order_relaxed);
    }
}
- (void)beginReflectedResources { if (kind != 1) [self compute]; reflectedResources = YES; }
- (void)useReflectedResource:(id)r usage:(MTLResourceUsage)usage {
    if (!r || ![r conformsToProtocol:@protocol(MTLResource)]) return; // Samplers have no memory allocation.
    NSUInteger i = 0; for (; i < reflectedCount && reflectedObjects[i] != r; i++) {}
    if (i < reflectedCount) { if ((reflectedUsage[i] & usage) == usage) return; usage |= reflectedUsage[i]; reflectedUsage[i] = usage; }
    else if (reflectedCount < 256) { reflectedObjects[reflectedCount] = r; reflectedUsage[reflectedCount++] = usage; }
    if (kind == 1) [(id<MTLRenderCommandEncoder>)native useResource:r usage:usage stages:MTLRenderStageVertex | MTLRenderStageFragment | (ot ? MTLRenderStageObject : 0) | (mt ? MTLRenderStageMesh : 0)];
    else [(id<MTLComputeCommandEncoder>)[self compute] useResource:r usage:usage];
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p { pipelineBindings = objc_getAssociatedObject(p, &nx3_pipeline_bindings_key); [(id<MTLRenderCommandEncoder>)native setRenderPipelineState:p]; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s { [(id<MTLRenderCommandEncoder>)native setDepthStencilState:s]; }
- (void)setViewport:(MTLViewport)v { [(id<MTLRenderCommandEncoder>)native setViewport:v]; }
- (void)setScissorRect:(MTLScissorRect)r { [(id<MTLRenderCommandEncoder>)native setScissorRect:r]; }
- (void)setCullMode:(MTLCullMode)m { [(id<MTLRenderCommandEncoder>)native setCullMode:m]; }
- (void)setFrontFacingWinding:(MTLWinding)w { [(id<MTLRenderCommandEncoder>)native setFrontFacingWinding:w]; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { [(id<MTLRenderCommandEncoder>)native setTriangleFillMode:m]; }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { [(id<MTLRenderCommandEncoder>)native setDepthBias:b slopeScale:s clamp:c]; }
- (void)setDepthClipMode:(MTLDepthClipMode)m { [(id<MTLRenderCommandEncoder>)native setDepthClipMode:m]; }
- (void)setStencilReferenceValue:(uint32_t)v { [(id<MTLRenderCommandEncoder>)native setStencilReferenceValue:v]; }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { [(id<MTLRenderCommandEncoder>)native setBlendColorRed:r green:g blue:b alpha:a]; }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { [(id<MTLRenderCommandEncoder>)native setVisibilityResultMode:m offset:o]; }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p { computePipeline=p; pipelineBindings = objc_getAssociatedObject(p, &nx3_pipeline_bindings_key); [(id<MTLComputeCommandEncoder>)[self compute] setComputePipelineState:p]; }
- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i { if(i>=32) abort(); threadgroupLengths[i]=n; threadgroupMask |= 1u<<i; [(id<MTLComputeCommandEncoder>)[self compute] setThreadgroupMemoryLength:n atIndex:i]; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n { [self apply]; [(id<MTLRenderCommandEncoder>)native drawPrimitives:p vertexStart:s vertexCount:n]; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b { [self apply]; [(id<MTLRenderCommandEncoder>)native drawPrimitives:p vertexStart:s vertexCount:n instanceCount:i baseInstance:b]; }
- (void)dispatchThreadgroups:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self compute]; [self apply]; [(id<MTLComputeCommandEncoder>)native dispatchThreadgroups:g threadsPerThreadgroup:t]; }
- (void)dispatchThreads:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self compute]; [self apply]; [(id<MTLComputeCommandEncoder>)native dispatchThreads:g threadsPerThreadgroup:t]; }
- (id)forwardingTargetForSelector:(SEL)s {
    NSString *name = NSStringFromSelector(s);
    if ([name hasPrefix:@"draw"]) { [self apply]; return native; }
    if ([name hasPrefix:@"dispatch"]) { [self compute]; [self apply]; return native; }
    if ([name hasPrefix:@"copy"] || [name hasPrefix:@"fill"] || [name hasPrefix:@"synchronize"]) return [self blit];
    if ([name hasPrefix:@"setCompute"] || [name hasPrefix:@"setThreadgroup"]) return [self compute];
    // A direct classic setter can overwrite state last applied from a table.
    if ([name hasPrefix:@"setVertexBuffer"] || [name hasPrefix:@"setFragmentBuffer"] || [name hasPrefix:@"setVertexBytes"] || [name hasPrefix:@"setFragmentBytes"] || [name hasPrefix:@"setBuffer"] || [name hasPrefix:@"setBytes"] || [name containsString:@"Texture"] || [name containsString:@"SamplerState"]) [self resetBindings];
    if (!native && kind != 1) [self compute];
    return native;
}
- (void)setArgumentTable:(NX3Table *)table atStages:(MTLRenderStages)s {
    if ((s & MTLRenderStageVertex && vt != table) || (s & MTLRenderStageFragment && ft != table) || (s & MTLRenderStageObject && ot != table) || (s & MTLRenderStageMesh && mt != table)) [self resetBindings];
    if (s & MTLRenderStageVertex) vt=table; if (s & MTLRenderStageFragment) ft=table;
    if (s & MTLRenderStageObject) ot=table; if (s & MTLRenderStageMesh) mt=table;
}
- (void)setArgumentTable:(NX3Table *)table { if (ct != table) [self resetBindings]; ct = table; }
- (void)endEncoding { [native endEncoding]; native = nil; }
// Queue-boundary visibility is supplied by classic tracked resource hazards.
// These calls occur at encoder boundaries; an intra-render memory barrier is
// neither equivalent nor legal with an after-fragment stage on Apple GPUs.
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b visibilityOptions:(NSUInteger)v { (void)a; (void)b; (void)v; }
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b visibilityOptions:(NSUInteger)v { (void)a; (void)b; (void)v; }
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b visibilityOptions:(NSUInteger)v {
    (void)v;
    if (kind == 1 && native) {
        // Current replay's intra-render barrier is vertex-write -> vertex/fragment.
        // Do not silently weaken future fragment/object/mesh dependencies.
        if (a != MTLStageVertex || (b & ~(MTLStageVertex | MTLStageFragment))) {
            fprintf(stderr, "[RED] Metal3 unsupported intra-render barrier stages %llu -> %llu\n", (unsigned long long)a, (unsigned long long)b); abort();
        }
        MTLRenderStages before = (b & MTLStageVertex ? MTLRenderStageVertex : 0) | (b & MTLStageFragment ? MTLRenderStageFragment : 0);
        [(id<MTLRenderCommandEncoder>)native memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures afterStages:MTLRenderStageVertex beforeStages:before];
    } else if (kind == 2 && native) [(id<MTLComputeCommandEncoder>)native memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    // Blits are ordered; compute/blit transitions end the old encoder and use tracked resources.
}
- (void)drawMeshThreadgroupsWithIndirectBuffer:(uint64_t)a threadsPerObjectThreadgroup:(MTLSize)o threadsPerMeshThreadgroup:(MTLSize)m { NSUInteger off; id<MTLBuffer>b=nx3_buffer(a,&off); [self apply]; [(id<MTLRenderCommandEncoder>)native drawMeshThreadgroupsWithIndirectBuffer:b indirectBufferOffset:off threadsPerObjectThreadgroup:o threadsPerMeshThreadgroup:m]; }
- (void)drawPrimitives:(MTLPrimitiveType)p indirectBuffer:(uint64_t)a { NSUInteger o; id<MTLBuffer>b = nx3_buffer(a, &o); [self apply]; [(id<MTLRenderCommandEncoder>)native drawPrimitives:p indirectBuffer:b indirectBufferOffset:o]; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t indexBuffer:(uint64_t)a indexBufferLength:(NSUInteger)len instanceCount:(NSUInteger)i baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    NSUInteger o; id<MTLBuffer>b = nx3_buffer(a, &o); if (len > b.length - o) abort(); [self apply];
    [(id<MTLRenderCommandEncoder>)native drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:b indexBufferOffset:o instanceCount:i baseVertex:bv baseInstance:bi];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(uint64_t)a indexBufferLength:(NSUInteger)len indirectBuffer:(uint64_t)ind {
    NSUInteger o, io; id<MTLBuffer>b = nx3_buffer(a, &o); id<MTLBuffer>ib = nx3_buffer(ind, &io); if (len > b.length - o) abort(); [self apply];
    [(id<MTLRenderCommandEncoder>)native drawIndexedPrimitives:p indexType:t indexBuffer:b indexBufferOffset:o indirectBuffer:ib indirectBufferOffset:io];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(uint64_t)a threadsPerThreadgroup:(MTLSize)t { NSUInteger o; id<MTLBuffer>b = nx3_buffer(a, &o); [self compute]; [self apply]; [(id<MTLComputeCommandEncoder>)native dispatchThreadgroupsWithIndirectBuffer:b indirectBufferOffset:o threadsPerThreadgroup:t]; }
@end

// BEGIN CLASSIC PRESENT READ SET
static void nx3_present_read_resources(NX3Encoder *encoder, id<MTLTexture> source, id<MTLBuffer> constants) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ fprintf(stderr, "[dm] [nvn-mtl] Metal3 present read set: source and constants only; unrelated allocations excluded\n"); });
    [encoder beginReflectedResources];
    [encoder useReflectedResource:source usage:MTLResourceUsageRead];
    [encoder useReflectedResource:constants usage:MTLResourceUsageRead];
}
// END CLASSIC PRESENT READ SET

@protocol NX3Feedback <NSObject>
@property(readonly) double GPUStartTime, GPUEndTime;
@property(readonly) NSError *error;
@end
@interface NX3Feedback : NSObject<NX3Feedback>
@property(strong) id<MTLCommandBuffer> buffer;
@end
@implementation NX3Feedback
- (double)GPUStartTime { return self.buffer.GPUStartTime; }
- (double)GPUEndTime { return self.buffer.GPUEndTime; }
- (NSError *)error { return self.buffer.error; }
@end
@interface NX3CommitOptions : NSObject
@property(strong) NSMutableArray *handlers;
- (void)addFeedbackHandler:(void (^)(id<NX3Feedback>))handler;
@end
@implementation NX3CommitOptions
- (instancetype)init { if ((self = [super init])) _handlers = [NSMutableArray array]; return self; }
- (void)addFeedbackHandler:(void (^)(id<NX3Feedback>))handler { [self.handlers addObject:[handler copy]]; }
@end
@protocol NX3CommandBuffer <NSObject>
- (void)beginCommandBufferWithAllocator:(id)allocator;
- (void)endCommandBuffer;
- (id<NX3RenderEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
- (id<NX3ComputeEncoder>)computeCommandEncoder;
- (id<NX3ComputeEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d;
@property(copy) NSString *label;
@end
@interface NX3CommandBuffer : NSObject<NX3CommandBuffer>
@property(strong) id<MTLCommandQueue> queue;
@property(strong) id<MTLCommandBuffer> native;
@property(strong) NX3ResourceSnapshot *resources;
@end
@implementation NX3CommandBuffer
- (void)beginCommandBufferWithAllocator:(id)a { (void)a; self.native = [self.queue commandBuffer]; self.resources = [NX3ResourceSnapshot new]; }
- (void)endCommandBuffer {}
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d { NX3Encoder *e = [NX3Encoder new]; e->cb = self.native; e->resourceSnapshot = self.resources; e->native = [self.native renderCommandEncoderWithDescriptor:d]; e->kind = 1; return e; }
- (id)computeCommandEncoder { NX3Encoder *e = [NX3Encoder new]; e->cb = self.native; e->resourceSnapshot = self.resources; e->kind = 2; return e; }
- (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d { NX3Encoder *e = [NX3Encoder new]; e->cb = self.native; e->resourceSnapshot = self.resources; e->native = [self.native computeCommandEncoderWithDescriptor:d]; e->kind = 2; return e; }
- (NSString *)label { return self.native.label; }
- (void)setLabel:(NSString *)s { self.native.label = s; }
@end
@interface NX3QueueDescriptor : NSObject
@property(strong) dispatch_queue_t feedbackQueue;
@end
@implementation NX3QueueDescriptor @end
@protocol NX3Queue <NSObject>
- (void)commit:(id<NX3CommandBuffer> const *)buffers count:(NSUInteger)count;
- (void)commit:(id<NX3CommandBuffer> const *)buffers count:(NSUInteger)count options:(NX3CommitOptions *)o;
- (void)signalEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)waitForEvent:(id<MTLEvent>)event value:(uint64_t)value;
- (void)addResidencySet:(id)set;
- (void)waitForDrawable:(id)drawable;
- (void)signalDrawable:(id)drawable;
@end
@interface NX3Queue : NSObject<NX3Queue>
@property(strong) id<MTLCommandQueue> native;
@property(strong) id<MTLCommandBuffer> last;
@property(strong) dispatch_queue_t feedbackQueue;
@end
@implementation NX3Queue
- (void)commit:(id<NX3CommandBuffer> const *)buffers count:(NSUInteger)n { [self commit:buffers count:n options:nil]; }
- (void)commit:(id<NX3CommandBuffer> const *)buffers count:(NSUInteger)n options:(NX3CommitOptions *)o {
    for (NSUInteger i = 0; i < n; i++) { NX3CommandBuffer *b = (NX3CommandBuffer *)buffers[i]; id<MTLCommandBuffer> cb = b.native; NSArray *handlers = [o.handlers copy]; dispatch_queue_t q = self.feedbackQueue;
        if (handlers.count) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { NX3Feedback *f = [NX3Feedback new]; f.buffer = done; dispatch_block_t invoke = ^{ for (void (^block)(id<NX3Feedback>) in handlers) block(f); }; if (q) dispatch_async(q, invoke); else invoke(); }];
        [cb commit]; self.last = cb;
    }
}
- (void)signalEvent:(id<MTLEvent>)event value:(uint64_t)v { id<MTLCommandBuffer> cb = [self.native commandBuffer]; [cb encodeSignalEvent:event value:v]; [cb commit]; self.last = cb; }
- (void)waitForEvent:(id<MTLEvent>)event value:(uint64_t)v { id<MTLCommandBuffer> cb = [self.native commandBuffer]; [cb encodeWaitForEvent:event value:v]; [cb commit]; self.last = cb; }
- (void)addResidencySet:(id)set { (void)set; } // Every indirectly bound resource is declared by NX3Encoder.apply.
- (void)waitForDrawable:(id)d { (void)d; } // Classic CAMetalDrawable acquisition owns availability.
- (void)signalDrawable:(id)d {
    (void)d;
    // The caller presents the drawable after this method. Classic Metal has no
    // drawable signal operation: wait for the actual producer to finish before
    // handing it to the display server. This serializes presentation but keeps
    // the Metal3 path correct until presentation is moved onto its command buffer.
    [self.last waitUntilCompleted];
}
@end

@interface NX3Device : NSObject
@property(strong) id<MTLDevice> native;
@property(strong) id<MTLCommandQueue> queue;
@end
@implementation NX3Device
- (id)forwardingTargetForSelector:(SEL)s { return self.native; }
// The replay queue is fixed at initialization. Diagnostics and other graphics
// APIs can request independent classic queues without moving replay submissions.
- (id)newCommandQueue { id<MTLCommandQueue> q = [self.native newCommandQueue]; if (!self.queue) self.queue = q; return q; }
- (id)newMTL4CommandQueueWithDescriptor:(NX3QueueDescriptor *)d error:(NSError **)error { (void)error; NX3Queue *q = [NX3Queue new]; if (!self.queue) self.queue = [self.native newCommandQueue]; q.native = self.queue; q.feedbackQueue = d.feedbackQueue; return q; }
- (id)newCommandAllocatorWithDescriptor:(id)d error:(NSError **)error { (void)d; (void)error; return [NX3Allocator new]; }
- (id)newCommandAllocator { return [NX3Allocator new]; }
- (id)newCommandBuffer { NX3CommandBuffer *cb = [NX3CommandBuffer new]; cb.queue = self.queue; return cb; }
- (id)newArgumentTableWithDescriptor:(id)d error:(NSError **)error { (void)d; (void)error; return [NX3Table new]; }
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    MTLRenderPipelineReflection *reflection = nil;
    id pipeline = [self.native newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionBindingInfo reflection:&reflection error:error];
    if (pipeline && reflection) {
        NX3PipelineBindings *mask = [NX3PipelineBindings new];
        nx3_pipeline_stage(mask, reflection.vertexBindings, 0); nx3_pipeline_stage(mask, reflection.fragmentBindings, 1);
        for (MTLVertexAttribute *a in d.vertexFunction.stageInputAttributes) {
            if (!a.active || a.attributeIndex >= 31) continue;
            MTLVertexAttributeDescriptor *v = d.vertexDescriptor.attributes[a.attributeIndex];
            if (v && v.format != MTLVertexFormatInvalid && v.bufferIndex < 31) mask->buffers[0] |= 1u << v.bufferIndex;
        }
        nx3_pipeline_save(pipeline, mask);
    }
    return pipeline;
}
- (id)newComputePipelineStateWithFunction:(id<MTLFunction>)f error:(NSError **)error {
    MTLComputePipelineReflection *reflection = nil;
    id pipeline = [self.native newComputePipelineStateWithFunction:f options:MTLPipelineOptionBindingInfo reflection:&reflection error:error];
    if (pipeline && reflection) { NX3PipelineBindings *mask = [NX3PipelineBindings new]; nx3_pipeline_stage(mask, reflection.bindings, 2); nx3_pipeline_save(pipeline, mask); }
    return pipeline;
}
- (id)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)d options:(MTLPipelineOption)options reflection:(MTLRenderPipelineReflection **)outReflection error:(NSError **)error {
    MTLRenderPipelineReflection *reflection = nil;
    id pipeline = [self.native newRenderPipelineStateWithMeshDescriptor:d options:options | MTLPipelineOptionBindingInfo reflection:&reflection error:error];
    if (outReflection) *outReflection = reflection;
    if (pipeline && reflection) {
        NX3PipelineBindings *mask = [NX3PipelineBindings new];
        nx3_pipeline_stage(mask, reflection.objectBindings, 3); nx3_pipeline_stage(mask, reflection.meshBindings, 4); nx3_pipeline_stage(mask, reflection.fragmentBindings, 1);
        nx3_pipeline_save(pipeline, mask);
    }
    return pipeline;
}
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o { id r = [self.native newBufferWithLength:n options:(o & ~MTLResourceHazardTrackingModeMask) | MTLResourceHazardTrackingModeTracked]; nx3_register(r); return r; }
- (id)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o { id r = [self.native newBufferWithBytes:p length:n options:(o & ~MTLResourceHazardTrackingModeMask) | MTLResourceHazardTrackingModeTracked]; nx3_register(r); return r; }
- (id)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)n options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))d { id r = [self.native newBufferWithBytesNoCopy:p length:n options:(o & ~MTLResourceHazardTrackingModeMask) | MTLResourceHazardTrackingModeTracked deallocator:d]; nx3_register(r); return r; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d { d.hazardTrackingMode = MTLHazardTrackingModeTracked; id r = [self.native newTextureWithDescriptor:d]; nx3_register(r); return r; }
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)d { MTLHeapDescriptor *copy=[d copy]; copy.hazardTrackingMode=MTLHazardTrackingModeTracked; return [self.native newHeapWithDescriptor:copy]; }
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d { id r = [self.native newSamplerStateWithDescriptor:d]; nx3_register(r); return r; }
@end
static id<MTLDevice> nx3_create_device(void) { NX3Device *d = [NX3Device new]; d.native = MTLCreateSystemDefaultDevice(); return (id<MTLDevice>)d; }

#define MTL4CommandQueue NX3Queue
#define MTL4CommandQueueDescriptor NX3QueueDescriptor
#define MTL4CommandAllocator NX3Allocator
#define MTL4CommandAllocatorDescriptor NX3AllocatorDescriptor
#define MTL4CommandBuffer NX3CommandBuffer
#define MTL4ArgumentTable NX3Table
#define MTL4ArgumentTableDescriptor NX3TableDescriptor
#define MTL4CommandEncoder NX3Encoder
#define MTL4RenderCommandEncoder NX3RenderEncoder
#define MTL4ComputeCommandEncoder NX3ComputeEncoder
#define MTL4RenderPassDescriptor MTLRenderPassDescriptor
#define MTL4CommitFeedback NX3Feedback
#define MTL4CommitOptions NX3CommitOptions

@interface NX3FunctionDescriptor : NSObject @end
@implementation NX3FunctionDescriptor @end
@interface NX3LibraryFunctionDescriptor : NX3FunctionDescriptor
@property(strong) id<MTLLibrary> library;
@property(copy) NSString *name;
@end
@implementation NX3LibraryFunctionDescriptor @end
@interface NX3SpecializedFunctionDescriptor : NX3FunctionDescriptor
@property(strong) NX3FunctionDescriptor *functionDescriptor;
@property(strong) MTLFunctionConstantValues *constantValues;
@property(copy) NSString *specializedName;
@end
@implementation NX3SpecializedFunctionDescriptor @end
#define MTL4FunctionDescriptor NX3FunctionDescriptor
#define MTL4LibraryFunctionDescriptor NX3LibraryFunctionDescriptor
#define MTL4SpecializedFunctionDescriptor NX3SpecializedFunctionDescriptor
