// NVN2 AS storage is opaque. Its guest buffer address names a Metal AS; triangle
// and instance data remain ordinary GPU buffers and are consumed in command order.
// Included after dispatch_enc: all work stays on the existing Metal4 queue.
// Metal's ordinary query returns IDs/transforms, but not the original instance
// SBT contribution. Retain only that GPU-consumed word, not a second AS model.
@interface NvAsSbt : NSObject
@property id<MTLBuffer> buffer;
@property uint64_t fence;
@property uint32_t count;
@end
@implementation NvAsSbt
- (void)dealloc { if(_buffer)defer_obj_at(_buffer,0,_fence); }
@end
@interface NvAccel : NSObject
@property id<MTLAccelerationStructure> object;
@property NSArray<NvAccel *> *children;
@property NvAsSbt *sbt;
@property id<MTLBuffer> compactSize;
@property uint64_t compactFence;
@property const void *pool;
@property uint32_t flags;
@property BOOL instances;
@end
@implementation NvAccel
- (void)dealloc {
    // A TLAS retains its children. Removing a guest address must not retire a
    // BLAS that a live TLAS still references, including after compaction.
    uint64_t open=atomic_load(&g_sub_open),fence=open?open:atomic_load(&g_fence);
    if(_object)defer_obj_at(_object,0,fence);
    if(_compactSize)defer_obj_at(_compactSize,0,fence);
}
@end
// A buffer is a view of pool bytes, so its Initialize/Finalize cannot prove the
// opaque contents dead. Keep address identities until an overlapping AS write or
// pool retirement. This conservatively retains freed suballocations in a pool
// whose allocator gives no content-lifetime notification.
static NSMutableDictionary<NSNumber *, NvAccel *> *g_as;
static pthread_mutex_t g_as_mu = PTHREAD_MUTEX_INITIALIZER;

static NvAccel *as_find(uint64_t address) {
    pthread_mutex_lock(&g_as_mu); NvAccel *a = g_as[@(address)]; pthread_mutex_unlock(&g_as_mu);
    return a;
}
uint64_t mtl_as_resource_id(uint64_t address) {
    NvAccel *a = as_find(address);
    if (!a) MDIE("[RED] acceleration structure %#llx has not been built", (unsigned long long)address);
    return a.object.gpuResourceID._impl;
}
uint64_t mtl_as_bound_address(unsigned stage, unsigned slot) { return stage < 9 && slot < 32 ? S.acceleration[stage][slot] : 0; }
int mtl_as_acquire(uint64_t address,nvn_as_binding *binding) {
    if(!binding)MDIE("[RED] AS dispatch binding output is null");
    memset(binding,0,sizeof *binding);if(!address)return 0;
    uint64_t fence=atomic_load(&g_sub_open);
    if(!fence)MDIE("[RED] AS dispatch bindings require an open GPU submission");
    NvAccel *a=as_find(address);if(!a)MDIE("[RED] acceleration structure %#llx has not been built",(unsigned long long)address);
    NvAsSbt *sbt=a.sbt;if(sbt)sbt.fence=MAX(sbt.fence,fence);
    *binding=(nvn_as_binding){a.object.gpuResourceID._impl,sbt.buffer.gpuAddress,sbt.count,
                              a.instances?NVN_AS_BINDING_TLAS:NVN_AS_BINDING_BLAS};
    // NvAccel's destructor already fence-retires the AS/children. Pin its owner
    // explicitly for the dispatch, and the selected SBT owner separately: an
    // in-place rebuild can replace a.sbt before deferred encoding/GPU execution.
    defer_obj_at(sbt?@[a,sbt]:@[a],1,fence);return 1;
}
static void as_drop_pool(const void *pool) {   // g_mu held: cannot race a new AS registry entry
    NSMutableArray *gone = [NSMutableArray array];
    pthread_mutex_lock(&g_as_mu);
    for (NSNumber *key in g_as.allKeys) if (g_as[key].pool == pool) { [gone addObject:g_as[key]]; [g_as removeObjectForKey:key]; }
    pthread_mutex_unlock(&g_as_mu);
}
static MTL4BufferRange as_range(uint64_t address, uint64_t length) {
    void *buffer; uint64_t offset, gpu;
    if (!address || !length || length > UINT64_MAX - address || !mtl_gpu_addr(address, length, &buffer, &offset, &gpu))
        MDIE("[RED] acceleration-structure buffer %#llx + %#llx is outside a memory pool", (unsigned long long)address, (unsigned long long)length);
    return (MTL4BufferRange){gpu, length};
}
static void as_storage(uint64_t address, uint64_t length) {
    if (!address || !length || length > UINT64_MAX-address) MDIE("[RED] invalid acceleration-structure storage range");
    if(address&255)MDIE("[RED] acceleration-structure storage %#llx is not 256-byte aligned",(unsigned long long)address);
    if (nvn_sparse_alias(address, address + length)) MDIE("[RED] sparse acceleration-structure storage is not implemented");
    (void)as_range(address, length);
}
static void as_validate(const nvn_as_build *b) {
    if (!b || (b->count && !b->geometries) || b->operation > 1) MDIE("[RED] invalid NVN2 acceleration-structure build descriptor");
    if (b->flags & ~15u) MDIE("[RED] acceleration-structure build flags %#x are not implemented", b->flags);
}
static MTLAttributeFormat as_format(unsigned f, unsigned *bytes) {
    switch (f) {
        case NVN_FORMAT_RG32F: *bytes=8; return MTLAttributeFormatFloat2;
        case NVN_FORMAT_RGB32F: *bytes=12; return MTLAttributeFormatFloat3;
        case NVN_FORMAT_RGBX32F: *bytes=16; return MTLAttributeFormatFloat3;
        case NVN_FORMAT_RG16F: *bytes=4; return MTLAttributeFormatHalf2;
        case NVN_FORMAT_RGB16F: *bytes=6; return MTLAttributeFormatHalf3;
        default: MDIE("[RED] acceleration-structure vertex format %#x is not implemented", f);
    }
}
static id<MTLComputePipelineState> as_kernel(int instances) {
    static id<MTLComputePipelineState> ps[2];
    int k=!!instances;
    if (!ps[k]) { NSError *e=nil; id<MTLLibrary> lib=host_lib_from(g_comp4,"accel",&e); id<MTLFunction> f=[lib newFunctionWithName:k?@"instances":@"indices8"];
        ps[k]=f?dev_cps(f, &e):nil;
        if(!ps[k]) MDIE("[RED] acceleration-structure conversion pipeline: %s", e.localizedDescription.UTF8String);
    }
    return ps[k];
}
static void as_barrier(void) {
    // AS work has its own Metal stage. A dispatch/blit barrier alone neither
    // orders a BLAS before its TLAS nor makes the compacted-size write visible.
    MTLStages stages=MTLStageDispatch|MTLStageBlit|MTLStageAccelerationStructure;
    [g_blit barrierAfterEncoderStages:stages beforeEncoderStages:stages visibilityOptions:blit_vis()];
    g_bw_n=g_br_n=g_dw_n=g_dr_n=g_blit_ops=0;g_blit_barriers++;
}
static MTL4AccelerationStructureDescriptor *as_descriptor(const nvn_as_build *b, int execute, NSArray<NvAccel *> **children, NvAsSbt **sbt) {
    as_validate(b);
    MTLAccelerationStructureUsage usage = b->flags & NVN_AS_ALLOW_UPDATE ? MTLAccelerationStructureUsageRefit : 0;
    if (b->count && b->geometries[0].type == 3) {
        if (b->count != 1) MDIE("[RED] multiple instance geometries in an acceleration structure are not implemented");
        const nvn_as_geometry *g=b->geometries;
        MTL4InstanceAccelerationStructureDescriptor *d=[MTL4InstanceAccelerationStructureDescriptor new];
        d.usage=usage; d.instanceCount=g->instances.count; d.instanceDescriptorStride=sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
        d.instanceTransformationMatrixLayout=MTLMatrixLayoutRowMajor;
        d.instanceDescriptorBuffer=(MTL4BufferRange){0,d.instanceCount*d.instanceDescriptorStride};
        if(execute && d.instanceCount) {
            // Instance addresses can be GPU-written. Retain the complete lookup
            // set without a CPU readback; retiring a registry entry must not free
            // a child a built TLAS might reference. This can overretain children.
            NSMutableArray<NvAccel *> *keep=[NSMutableArray array]; NSMutableArray<NSNumber *> *keys=[NSMutableArray array];
            pthread_mutex_lock(&g_as_mu);
            for(NSNumber *key in g_as) if(!g_as[key].instances) [keys addObject:key];
            [keys sortUsingSelector:@selector(compare:)];
            id<MTLBuffer> map; size_t mo=arena_alloc(MAX((size_t)16,keys.count*16),&map); uint64_t *pairs=(uint64_t *)((uint8_t *)map.contents+mo);
            for(NSUInteger i=0;i<keys.count;i++) { NvAccel *a=g_as[keys[i]]; pairs[2*i]=keys[i].unsignedLongLongValue; pairs[2*i+1]=a.object.gpuResourceID._impl; [keep addObject:a]; }
            pthread_mutex_unlock(&g_as_mu);
            if(!keys.count)MDIE("[RED] nonempty AS instance input has no built child acceleration structures");
            if(children)*children=keep;
            NvAsSbt *snapshot=[NvAsSbt new];snapshot.count=(uint32_t)d.instanceCount;snapshot.fence=atomic_load(&g_sub_open);
            snapshot.buffer=[g_dev newBufferWithLength:d.instanceCount*sizeof(uint32_t) options:MTLResourceStorageModeShared];
            if(!snapshot.buffer)MDIE("[RED] AS SBT snapshot allocation failed");
            res_add(snapshot.buffer);*sbt=snapshot;
            id<MTLBuffer> out,params,status; size_t oo=arena_alloc(d.instanceCount*d.instanceDescriptorStride,&out),po=arena_alloc(8,&params),so=arena_alloc(16,&status);
            memset((uint8_t *)status.contents+so,0,16);
            [g_commit_blocks addObject:^{
                const uint32_t *v=(const uint32_t *)((const uint8_t *)status.contents+so);
                if(v[0]==1)MDIE("[RED] AS instance %u references an unbuilt child address %#llx",v[1],(unsigned long long)((uint64_t)v[2]|(uint64_t)v[3]<<32));
                if(v[0]==2)MDIE("[RED] AS instance %u has unsupported option bits %#x",v[1],v[2]);
            }];
            uint32_t n[2]={(uint32_t)d.instanceCount,(uint32_t)keys.count};memcpy((uint8_t *)params.contents+po,n,8);
            id<MTL4ComputeCommandEncoder> e=dispatch_enc();
            MTL4BufferRange src=as_range(g->instances.address,d.instanceCount*64);
            enc_c_pso(e,as_kernel(1));enc_c_args(e,src.bufferAddress,0);enc_c_args(e,out.gpuAddress+oo,1);enc_c_args(e,map.gpuAddress+mo,2);enc_c_args(e,params.gpuAddress+po,3);enc_c_args(e,status.gpuAddress+so,4);enc_c_args(e,snapshot.buffer.gpuAddress,5);
            enc_c_dispatch(e,MTLSizeMake((d.instanceCount+63)/64,1,1),MTLSizeMake(64,1,1));
            d.instanceDescriptorBuffer=(MTL4BufferRange){out.gpuAddress+oo,d.instanceCount*d.instanceDescriptorStride};
            blit_barrier();
        }
        return d;
    }
    NSMutableArray *geometries=[NSMutableArray array];
    for(unsigned i=0;i<b->count;i++) {
        const nvn_as_geometry *g=&b->geometries[i];
        if(g->type==1) {
            const typeof(g->triangles) *t=&g->triangles;
            if(t->primitive_offset || t->transform) MDIE("[RED] nonzero NVN2 triangle extra fields are not implemented (offset %#x, address %#llx)",t->primitive_offset,(unsigned long long)t->transform);
            if(t->flags & ~3u) MDIE("[RED] triangle flags %#x are not implemented",t->flags);
            unsigned bytes; MTL4AccelerationStructureTriangleGeometryDescriptor *d=[MTL4AccelerationStructureTriangleGeometryDescriptor new];
            d.vertexFormat=as_format(t->vertex_format,&bytes);d.vertexStride=t->vertex_stride?t->vertex_stride:bytes;
            if(d.vertexStride<bytes)MDIE("[RED] AS vertex stride %lu is smaller than its %u-byte format",(unsigned long)d.vertexStride,bytes);
            if((t->index_count?t->index_count:t->vertex_count)%3)MDIE("[RED] AS triangle input count is not a multiple of three");
            if(t->index_count && !t->vertex_count)MDIE("[RED] indexed AS geometry has no vertices");
            d.triangleCount=(t->index_count?t->index_count:t->vertex_count)/3;d.opaque=!!(t->flags&1);d.allowDuplicateIntersectionFunctionInvocation=!(t->flags&2);
            uint64_t vbytes=(uint64_t)t->vertex_count*d.vertexStride;
            d.vertexBuffer=(MTL4BufferRange){0,vbytes};if(execute && vbytes)d.vertexBuffer=as_range(t->vertices,vbytes);
            if(t->index_count) {
                if(t->index_type>2) MDIE("[RED] invalid AS index type %u",t->index_type);
                d.indexType=t->index_type==2?MTLIndexTypeUInt32:MTLIndexTypeUInt16;
                d.indexBuffer=(MTL4BufferRange){0,(uint64_t)t->index_count*(t->index_type==2?4:2)};
                if(execute) {
                    MTL4BufferRange input=as_range(t->indices,(uint64_t)t->index_count*(1u<<t->index_type));
                    if(t->index_type) d.indexBuffer=input;
                    else { id<MTLBuffer> out,params;size_t oo=arena_alloc((size_t)t->index_count*2,&out),po=arena_alloc(4,&params);memcpy((uint8_t *)params.contents+po,&t->index_count,4);
                        id<MTL4ComputeCommandEncoder> e=dispatch_enc();enc_c_pso(e,as_kernel(0));enc_c_args(e,input.bufferAddress,0);enc_c_args(e,out.gpuAddress+oo,1);enc_c_args(e,params.gpuAddress+po,2);
                        enc_c_dispatch(e,MTLSizeMake((t->index_count+63)/64,1,1),MTLSizeMake(64,1,1));blit_barrier();d.indexBuffer=(MTL4BufferRange){out.gpuAddress+oo,(uint64_t)t->index_count*2}; }
                }
            }
            [geometries addObject:d];
        } else if(g->type==2) {
            if(g->aabbs.flags&~3u)MDIE("[RED] bounding-box flags %#x are not implemented",g->aabbs.flags);
            MTL4AccelerationStructureBoundingBoxGeometryDescriptor *d=[MTL4AccelerationStructureBoundingBoxGeometryDescriptor new];d.boundingBoxCount=g->aabbs.count;d.boundingBoxStride=g->aabbs.stride?g->aabbs.stride:24;d.opaque=!!(g->aabbs.flags&1);d.allowDuplicateIntersectionFunctionInvocation=!(g->aabbs.flags&2);
            if(d.boundingBoxStride<24 || d.boundingBoxStride%4)MDIE("[RED] invalid AS bounding-box stride %lu",(unsigned long)d.boundingBoxStride);
            d.boundingBoxBuffer=(MTL4BufferRange){0,d.boundingBoxCount*d.boundingBoxStride};
            if(execute && d.boundingBoxCount)d.boundingBoxBuffer=as_range(g->aabbs.boxes,d.boundingBoxCount*d.boundingBoxStride);
            [geometries addObject:d];
        } else MDIE("[RED] NVN2 acceleration geometry kind %u is not implemented",g->type);
    }
    MTL4PrimitiveAccelerationStructureDescriptor *d=[MTL4PrimitiveAccelerationStructureDescriptor new];d.usage=usage;d.geometryDescriptors=geometries;return d;
}
void mtl_as_sizes(const nvn_as_build *b,uint64_t *storage,uint64_t *scratch,uint64_t *update) { @autoreleasepool {
    mtl_init(); if(!g_dev.supportsRaytracing)MDIE("[RED] this Metal device does not support acceleration structures");
    MTLAccelerationStructureSizes s=[g_dev accelerationStructureSizesWithDescriptor:as_descriptor(b,0,NULL,NULL)];
    if(storage)*storage=s.accelerationStructureSize;if(scratch)*scratch=s.buildScratchBufferSize;if(update)*update=s.refitScratchBufferSize;
} }
static NvAccel *as_new(uint64_t address,uint64_t size) {
    (void)as_storage(address,size);
    NvAccel *a=[NvAccel new];NSMutableArray *gone=[NSMutableArray array];
    // The owner lookup, allocation and registry reservation form one transaction
    // with pool_take_out. There is no later republish that can resurrect a pool
    // finalized (or replaced at the same address) while this build is encoded.
    pthread_mutex_lock(&g_mu);int i=pool_find(address,size);
    if(i<0)MDIE("[RED] acceleration-structure storage was retired before allocation");
    a.pool=g_pools[i].key;a.object=[g_dev newAccelerationStructureWithSize:size];
    if(!a.object)MDIE("[RED] Metal AS allocation of %llu bytes failed",(unsigned long long)size);
    res_add(a.object);
    pthread_mutex_lock(&g_as_mu);if(!g_as)g_as=[NSMutableDictionary dictionary];
    for(NSNumber *key in g_as.allKeys) {
        uint64_t old=key.unsignedLongLongValue;NvAccel *v=g_as[key];
        if(old<address+a.object.size && address<old+v.object.size) { [gone addObject:v];[g_as removeObjectForKey:key]; }
    }
    g_as[@(address)]=a;pthread_mutex_unlock(&g_as_mu);pthread_mutex_unlock(&g_mu);return a;
}
static void as_measure(NvAccel *a,id<MTL4ComputeCommandEncoder> e) {
    if(!(a.flags&NVN_AS_ALLOW_COMPACTION))return;
    if(!a.compactSize) {a.compactSize=[g_dev newBufferWithLength:8 options:MTLResourceStorageModeShared];res_add(a.compactSize);}
    [e writeCompactedAccelerationStructureSize:a.object toBuffer:(MTL4BufferRange){a.compactSize.gpuAddress,8}];a.compactFence=g_fence+1;as_barrier();
}
static void as_exec_build(const p_as_build *p) {
    for(uint64_t i=0;i<p->count;i++) {
        const nvn_as_build *b=&p->builds[i];as_validate(b);
        blit_raw(); // land inline updates before input conversion or AS build
        NSArray<NvAccel *> *children=nil;NvAsSbt *sbt=nil;MTL4AccelerationStructureDescriptor *d=as_descriptor(b,1,&children,&sbt);
        MTLAccelerationStructureSizes s=[g_dev accelerationStructureSizesWithDescriptor:d];
        uint64_t need=b->operation?s.refitScratchBufferSize:s.buildScratchBufferSize;
        if(b->scratch_size<need)MDIE("[RED] AS scratch has %llu bytes, needs %llu",(unsigned long long)b->scratch_size,(unsigned long long)need);
        NvAccel *source=b->operation?as_find(b->source?b->source:b->destination):nil;
        if(b->operation && (!source || !(source.flags&NVN_AS_ALLOW_UPDATE)))MDIE("[RED] AS refit source was not built for updates");
        // A compacted AS remains refittable. Metal requires its actual compacted
        // capacity for the refit destination, not the original build-size query.
        uint64_t capacity=b->operation?source.object.size:s.accelerationStructureSize;
        if(b->destination_size<capacity)MDIE("[RED] AS destination has %llu bytes, needs %llu",(unsigned long long)b->destination_size,(unsigned long long)capacity);
        (void)as_storage(b->destination,b->destination_size);
        NvAccel *previous=as_find(b->destination);BOOL instances=[d isKindOfClass:MTL4InstanceAccelerationStructureDescriptor.class];
        if(source && source.instances!=instances)MDIE("[RED] AS refit changes the acceleration-structure type");
        // Rebuilding storage in place must preserve its Metal resource ID: a
        // previously built TLAS may still reference this child address.
        NvAccel *a=source && (!b->source || b->source==b->destination)?source:
            previous && previous.instances==instances && previous.object.size>=capacity && previous.object.size<=b->destination_size?previous:as_new(b->destination,b->destination_size);
        a.flags=b->flags;a.instances=instances;a.children=children;a.sbt=sbt;
        id<MTL4ComputeCommandEncoder> e=dispatch_enc();MTL4BufferRange scratch=need?as_range(b->scratch,need):(MTL4BufferRange){0,0};
        as_barrier();
        if(b->operation)[e refitAccelerationStructure:source.object descriptor:d destination:a.object scratchBuffer:scratch];
        else [e buildAccelerationStructure:a.object descriptor:d scratchBuffer:scratch];
        as_barrier();
        as_measure(a,e);
        for(uint64_t j=0;j<p->post_count;j++) {
            if(p->post[j].type!=0 || !a.compactSize)MDIE("[RED] AS postbuild report type %u or missing compaction permission is not implemented",p->post[j].type);
            if(p->post[j].address>UINT64_MAX-i*8)MDIE("[RED] AS postbuild destination address overflows");
            MTL4BufferRange out=as_range(p->post[j].address+i*8,8);void *buffer;uint64_t off;if(!mtl_addr(p->post[j].address+i*8,8,&buffer,&off))MDIE("[RED] AS postbuild destination is unavailable");
            [e copyFromBuffer:a.compactSize sourceOffset:0 toBuffer:(__bridge id<MTLBuffer>)buffer destinationOffset:off size:8];(void)out;
            sbw_note(p->post[j].address+i*8,8,SBW_GPU);as_barrier();
        }
    }
}
static void as_exec_copy(const p_as_copy *p) {
    NvAccel *source=as_find(p->source);if(!source)MDIE("[RED] AS copy source %#llx was not built",(unsigned long long)p->source);
    uint64_t size=source.object.size;
    if(p->mode) {
        if(p->mode!=1 || !source.compactSize)MDIE("[RED] AS compaction source has no measured compacted size");
        // Metal AS allocation is CPU-only. An immediate compact must complete the
        // real GPU size query first; a later-submit compact usually has it already.
        if(atomic_load(&g_done)<source.compactFence) {mtl_sub_commit();mtl_finish();mtl_sub_begin();}
        memcpy(&size,source.compactSize.contents,8);if(!size || size>source.object.size)MDIE("[RED] Metal returned invalid compacted AS size %llu",(unsigned long long)size);
    }
    (void)as_storage(p->destination,size);
    if(p->destination<p->source+source.object.size && p->source<p->destination+size)MDIE("[RED] AS copy source and destination storage overlap");
    NvAccel *previous=as_find(p->destination);
    NvAccel *a=previous && previous.instances==source.instances && previous.object.size>=size?previous:as_new(p->destination,size);
    a.flags=source.flags;a.instances=source.instances;a.children=source.children;a.sbt=source.sbt;
    id<MTL4ComputeCommandEncoder> e=blit();
    as_barrier();
    if(p->mode)[e copyAndCompactAccelerationStructure:source.object toAccelerationStructure:a.object];else[e copyAccelerationStructure:source.object toAccelerationStructure:a.object];
    as_barrier();as_measure(a,e);
}
static void as_exec_bind(const p_bind_as *p) {
    if(p->stage>=9 || p->index>=32)MDIE("[RED] invalid acceleration-structure binding stage %u slot %u",p->stage,p->index);
    S.acceleration[p->stage][p->index]=p->address;
}
