// Diagnostic only. Capture the actual resources bound to selected compute shaders.
// GPU reads are ordered before/after the dispatch, with no CPU read of guest bytes.
static const char *g_ncap_path,*g_ncap_arm,*g_ncap_keys;
static unsigned g_ncap_frames=2,g_ncap_dispatch_id;
static uint64_t g_ncap_first=UINT64_MAX,g_ncap_bytes;
static int g_ncap_on,g_ncap_done,g_ncap_failed,g_ncap_truncated;
static NvShader *g_ncap_shader;
static NSMutableDictionary *g_ncap_textures,*g_ncap_samplers;
static NSMutableArray *g_ncap_records,*g_ncap_errors;
// Logical dependency audit only: captures bindings, not pixel values. Clear/copy and
// implicit attachment loads are not inferred. Missing edges remain explicitly open.
static int g_ncap_flow_on,g_ncap_flow_truncated;
static NSMutableArray *g_ncap_flow;
static int ncap_flow_active(void) {
    uint64_t frame=atomic_load(&g_cur_frame);
    return g_ncap_on&&g_ncap_flow_on&&!g_ncap_done&&g_ncap_first!=UINT64_MAX&&frame>=g_ncap_first&&frame<g_ncap_first+g_ncap_frames;
}
static NSDictionary *ncap_flow_texture(const tex_t *texture) {
    return @{@"serial":@(texture->serial),@"memory":@((uint64_t)(uintptr_t)texture->mem),@"storage_size":@(texture->L.size),@"width":@(texture->d.width),@"height":@(texture->d.height),@"format":@(texture->d.format)};
}
static void ncap_flow_bindings(NvShader *shader,int stage,NSMutableArray *reads,NSMutableArray *writes) {
    if(!shader)return;
    for(int i=0;i<shader->rf.nb;i++) {
        const nvn_rbind *binding=&shader->rf.b[i];int textureID=0;
        if(binding->kind==NVN_RB_TEX)textureID=(int)(tex_slot_h(S.tex[stage],S.img[stage],binding->reg)&0xfffff);
        else if((binding->kind==NVN_RB_IMG||binding->kind==NVN_RB_IMGU)&&binding->reg<32)textureID=(int)(S.img[stage][binding->reg]&0xfffff);
        else continue;
        if(textureID<=0||textureID>=g_ntreg||!g_treg[textureID].ok)continue;
        NSMutableDictionary *resource=[ncap_flow_texture(&g_treg[textureID].t) mutableCopy];
        resource[@"stage"]=@(stage);resource[@"argument"]=@(binding->aid);resource[@"reg"]=@(binding->reg);resource[@"kind"]=@(binding->kind);
        if(binding->rw||binding->atom)[writes addObject:resource];
        if(binding->kind==NVN_RB_TEX||!binding->rw||binding->atom)[reads addObject:resource];
    }
}
static void ncap_flow_event(NvShader *compute) {
    if(!ncap_flow_active())return;
    if(g_ncap_flow.count>=8192){g_ncap_flow_truncated=1;return;}
    NSMutableArray *reads=[NSMutableArray array],*writes=[NSMutableArray array],*shaders=[NSMutableArray array];
    NSDictionary *depth=@{};
    os_unfair_lock_lock(&g_rmu);
    if(compute){[shaders addObject:@{@"stage":@5,@"key":@(compute->key)}];ncap_flow_bindings(compute,5,reads,writes);}
    else {
        NvProg *program=S.prog?prog_get(S.prog):nil;
        for(int stage=0;stage<5;stage++)if(program&&program->sh[stage]){
            NvShader *shader=program->sh[stage];[shaders addObject:@{@"stage":@(stage),@"key":@(shader->key)}];ncap_flow_bindings(shader,stage,reads,writes);
        }
        for(int i=0;i<R.rt.n;i++)if(R.rt.color[i]){NSMutableDictionary *r=[ncap_flow_texture(R.rt.color[i]) mutableCopy];r[@"attachment"]=@(i);[writes addObject:r];}
        if(R.rt.depth)depth=ncap_flow_texture(R.rt.depth);
    }
    os_unfair_lock_unlock(&g_rmu);
    [g_ncap_flow addObject:@{@"order":@(g_ncap_flow.count),@"frame":@(atomic_load(&g_cur_frame)),@"kind":compute?@"compute":@"draw",@"shaders":shaders,@"reads":reads,@"writes":writes,@"depth_attachment":depth}];
}

static int ncap_selected(NvShader *shader) {
    if(!g_ncap_on||g_ncap_done||g_ncap_first==UINT64_MAX||!shader)return 0;
    uint64_t frame=atomic_load(&g_cur_frame);if(frame<g_ncap_first||frame>=g_ncap_first+g_ncap_frames)return 0;
    for(const char *p=g_ncap_keys;*p;){const char *end=strchr(p,',');size_t n=end?(size_t)(end-p):strlen(p);if(n==16&&!strncmp(p,shader->key,16))return 1;if(!end)break;p=end+1;}
    return 0;
}
static void ncap_error(NSString *message) { g_ncap_failed=1;[g_ncap_errors addObject:message]; }
static void ncap_frame(uint64_t frame) {
    static int configured;
    if(!configured){configured=1;g_ncap_path=getenv("DM_NVN_NATIVE_CAPTURE_OUT");g_ncap_arm=getenv("DM_NVN_NATIVE_CAPTURE_ARM");g_ncap_keys=getenv("DM_NVN_NATIVE_CAPTURE_KEYS");g_ncap_flow_on=getenv("DM_NVN_NATIVE_CAPTURE_FLOW")&&atoi(getenv("DM_NVN_NATIVE_CAPTURE_FLOW"));g_ncap_on=g_ncap_path&&*g_ncap_path&&g_ncap_arm&&*g_ncap_arm&&g_ncap_keys&&*g_ncap_keys;const char *n=getenv("DM_NVN_NATIVE_CAPTURE_FRAMES");if(n)g_ncap_frames=(unsigned)MAX(1,MIN(3,atoi(n)));}
    if(!g_ncap_on||g_ncap_done)return;
    if(g_ncap_first==UINT64_MAX){FILE *f=fopen(g_ncap_arm,"r");if(!f)return;fclose(f);g_ncap_first=frame;g_ncap_records=[NSMutableArray array];g_ncap_flow=g_ncap_flow_on?[NSMutableArray array]:nil;g_ncap_errors=[NSMutableArray array];MLOG("NATIVE_CAPTURE_ARMED frame %llu limit %u",(unsigned long long)frame,g_ncap_frames);return;}
    if(frame<g_ncap_first+g_ncap_frames)return;
    g_ncap_done=1;uint64_t fence=g_cb?mtl_sub_commit():mtl_last_fence();int complete=mtl_wait(fence,2000000000ull);
    NSError *error=nil;[[NSFileManager defaultManager] createDirectoryAtPath:@(g_ncap_path) withIntermediateDirectories:YES attributes:nil error:&error];int io_ok=error==nil;
    NSMutableArray *records=[NSMutableArray array];
    for(NSDictionary *original in g_ncap_records){NSMutableDictionary *record=[original mutableCopy];NSMutableArray *resources=[NSMutableArray array];
        for(NSDictionary *originalResource in original[@"resources"]){NSMutableDictionary *r=[originalResource mutableCopy];id<MTLBuffer> buffer=r[@"_buffer"];[r removeObjectForKey:@"_buffer"];
            if(complete&&buffer){NSUInteger row=[r[@"row_bytes"] unsignedIntegerValue],gpuRow=[r[@"gpu_row_bytes"] unsignedIntegerValue],height=[r[@"height"] unsignedIntegerValue];NSData *bytes;
                if(row&&gpuRow&&height){NSMutableData *packed=[NSMutableData dataWithLength:row*height];for(NSUInteger y=0;y<height;y++)memcpy((uint8_t *)packed.mutableBytes+row*y,(const uint8_t *)buffer.contents+gpuRow*y,row);bytes=packed;}
                else bytes=[NSData dataWithBytes:buffer.contents length:[r[@"size"] unsignedIntegerValue]];
                if(![bytes writeToFile:[@(g_ncap_path) stringByAppendingPathComponent:r[@"file"]] options:NSDataWritingAtomic error:&error])io_ok=0;}
            if(buffer){if(complete)res_remove(buffer);else defer_obj_release(buffer);}[resources addObject:r];}
        record[@"resources"]=resources;[records addObject:record];}
    NSDictionary *receipt=@{@"first_frame":@(g_ncap_first),@"frame_limit":@(g_ncap_frames),@"complete":@(complete),@"failed":@(g_ncap_failed),@"truncated":@(g_ncap_truncated),@"io_ok":@(io_ok),@"readback_bytes":@(g_ncap_bytes),@"records":records,@"logical_flow":g_ncap_flow?:@[],@"flow_truncated":@(g_ncap_flow_truncated),@"flow_scope":@"logical texture bindings and raster attachments; no clear/copy edges, implicit loads, buffer dependencies or original task/mesh aliases",@"errors":g_ncap_errors?:@[],@"scope":@"actual level-0/slice-0 texture views and effective buffer pointers; captures perturb timing"};
    NSData *json=[NSJSONSerialization dataWithJSONObject:receipt options:NSJSONWritingPrettyPrinted error:&error];if(!json||![json writeToFile:[@(g_ncap_path) stringByAppendingPathComponent:@"receipt.json"] options:NSDataWritingAtomic error:&error])io_ok=0;
    MLOG("%sNATIVE_CAPTURE_DONE records %lu bytes %llu complete=%d failed=%d truncated=%d io_ok=%d",complete&&!g_ncap_failed&&!g_ncap_truncated&&io_ok?"":"[RED] ",(unsigned long)records.count,(unsigned long long)g_ncap_bytes,complete,g_ncap_failed,g_ncap_truncated,io_ok);
    g_ncap_records=nil;g_ncap_flow=nil;g_ncap_textures=nil;g_ncap_samplers=nil;g_ncap_shader=nil;
}
static void ncap_begin_args(NvShader *shader,int stage) {
    if(stage!=5)return;
    g_ncap_shader=ncap_selected(shader)?shader:nil;
    g_ncap_textures=g_ncap_shader?[NSMutableDictionary dictionary]:nil;g_ncap_samplers=g_ncap_shader?[NSMutableDictionary dictionary]:nil;
}
// Called at the final binding, after snapshots, float-fit and atomic views resolve.
static void ncap_texture(NvShader *shader,int stage,unsigned argument,int textureID,id<MTLTexture> texture) {
    if(stage!=5||shader!=g_ncap_shader||!texture)return;
    NSMutableDictionary *info=[@{@"_texture":texture,@"argument":@(argument),@"pixel_format":@(texture.pixelFormat),@"width":@(texture.width),@"height":@(texture.height),@"texture_type":@(texture.textureType),@"resource_id":@(texture.gpuResourceID._impl),@"parent_resource_id":@(texture.parentTexture?texture.parentTexture.gpuResourceID._impl:0),@"parent_level":@(texture.parentRelativeLevel),@"parent_slice":@(texture.parentRelativeSlice),@"swizzle":@[@(texture.swizzle.red),@(texture.swizzle.green),@(texture.swizzle.blue),@(texture.swizzle.alpha)]} mutableCopy];
    if(textureID>0&&textureID<g_ntreg&&g_treg[textureID].ok){const tex_t *t=&g_treg[textureID].t;info[@"nvn_serial"]=@(t->serial);info[@"nvn_memory"]=@((uint64_t)(uintptr_t)t->mem);info[@"nvn_format"]=@(t->d.format);info[@"nvn_flags"]=@(t->d.flags);info[@"nvn_storage_size"]=@(t->L.size);}
    g_ncap_textures[@(argument)]=info;
}
static void ncap_sampler(NvShader *shader,int stage,const nvn_rbind *binding,int samplerID,uint64_t resourceID) {
    if(stage!=5||shader!=g_ncap_shader)return;
    NSMutableDictionary *info=[@{@"argument":@(binding->aid),@"reg":@(binding->reg),@"resource_id":@(resourceID),@"border_bit":@(binding->bw)} mutableCopy];
    if(samplerID>0&&samplerID<g_nsreg&&g_sreg_ok[samplerID]){samp_par p=g_sreg[samplerID];info[@"nvn"]=@{@"mag":@(p.magf),@"min":@(p.minf),@"wrap":@[@(p.ws),@(p.wt),@(p.wr)],@"border":@[@(p.border[0]),@(p.border[1]),@(p.border[2]),@(p.border[3])],@"lod_min":@(p.lod_min),@"lod_max":@(p.lod_max),@"lod_bias":@(p.lod_bias),@"anisotropy":@(p.aniso),@"compare_mode":@(p.cmp_mode),@"compare_function":@(p.cmp_func)};}
    g_ncap_samplers[@(binding->aid)]=info;
}
static NSUInteger ncap_buffer_size(NvShader *shader,const nvn_rbind *b) {
    const p_bind_buf *u=b->reg<16?(b->kind==NVN_RB_SBUFP?&S.ssbo[5][b->reg]:&S.ubo[5][b->reg]):NULL;
    switch(b->kind){
        case NVN_RB_CBUF:return u?MAX(u->size,4ull*b->fixw):0;
        case NVN_RB_SBUFP:case NVN_RB_UBOP:return u?u->size:0;
        case NVN_RB_PUSH:return DMSH_PUSH_SIZE;
        case NVN_RB_SUPPORT:return 65536;
        case NVN_RB_CBSIZE:return 16*((DMSH_CBSIZE_SLOTS+3)/4);
        case NVN_RB_DRV0:case NVN_RB_DRV2:return DMSH_DRV_SIZE;
        case NVN_RB_C1:return shader->rf.c1_len?:16;
        case NVN_RB_BORDER:{unsigned n=0;for(int i=0;i<shader->rf.nb;i++)n=MAX(n,shader->rf.b[i].bw);return DMSH_BORDER_K+16*n;}
        default:return 0;
    }
}
static id<MTLBuffer> ncap_readback(NSUInteger size) {
    if(!size||size>(32ull<<20)||g_ncap_bytes+size>(192ull<<20)){g_ncap_truncated=1;return nil;}
    id<MTLBuffer> b=[g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];if(!b){ncap_error(@"readback allocation failed");return nil;}res_add(b);g_ncap_bytes+=size;return b;
}
// Same compute encoder: barrier -> copies -> barrier -> restore the native table/PSO.
static void ncap_dispatch(NvShader *shader,const p_dispatch *dispatch,uint64_t indirectGPU,id<MTL4ComputeCommandEncoder> encoder,id<MTLComputePipelineState> nativePipeline,int after) {
    if(!after)ncap_flow_event(shader);
    if(!ncap_selected(shader))return;
    if(g_ncap_shader!=shader){ncap_error(@"resolved texture bindings missing");return;}
    if(!after)g_ncap_dispatch_id++;
    NSString *phase=after?@"after":@"before";uint64_t frame=atomic_load(&g_cur_frame);
    NSMutableArray *resources=[NSMutableArray array],*jobs=[NSMutableArray array],*first=[NSMutableArray array];
    for(int i=0;i<shader->rf.nfirst;i++)[first addObject:@{@"reg":@(shader->rf.first[i].reg),@"word":@(shader->rf.first[i].word),@"value":@(shader->rf.first[i].value)}];
    [encoder barrierAfterEncoderStages:(MTLStageDispatch|MTLStageBlit) beforeEncoderStages:(MTLStageDispatch|MTLStageBlit) visibilityOptions:resource_vis()];
    NSUInteger argumentLength=shader->arg_len?:shader->ae.encodedLength;
    const uint64_t *arguments=shader->ab_buf&&shader->ab_off+argumentLength<=shader->ab_buf.length?(const void *)((const uint8_t *)shader->ab_buf.contents+shader->ab_off):NULL;
    for(int i=0;i<shader->rf.nb;i++){const nvn_rbind *b=&shader->rf.b[i];if(after&&!b->rw&&!b->atom)continue;
        NSString *file=[NSString stringWithFormat:@"f%llu-d%u-%@-a%u.bin",(unsigned long long)frame,g_ncap_dispatch_id,phase,b->aid];
        NSDictionary *bound=g_ncap_textures[@(b->aid)];id<MTLTexture> texture=bound[@"_texture"];
        if(texture){NSUInteger bytes=mtl_pf_bytes(texture.pixelFormat);if(texture.textureType!=MTLTextureType2D||texture.sampleCount!=1||!bytes){ncap_error([NSString stringWithFormat:@"unsupported texture shape/format at argument %u",b->aid]);continue;}
            NSUInteger row=texture.width*bytes,pitch=(row+255)&~255ul;id<MTLBuffer> readback=ncap_readback(pitch*texture.height);if(!readback)continue;
            [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*texture.height];
            NSMutableDictionary *r=[bound mutableCopy];[r removeObjectForKey:@"_texture"];r[@"_buffer"]=readback;r[@"file"]=file;r[@"row_bytes"]=@(row);r[@"gpu_row_bytes"]=@(pitch);r[@"size"]=@(row*texture.height);r[@"kind"]=@(b->kind);r[@"reg"]=@(b->reg);r[@"write"]=@(b->rw);r[@"atomic"]=@(b->atom);[resources addObject:r];continue;}
        if(g_ncap_samplers[@(b->aid)])continue;
        NSUInteger size=ncap_buffer_size(shader,b);
        if(!size||!arguments||(b->aid+1)*sizeof(uint64_t)>argumentLength||!arguments[b->aid]){ncap_error([NSString stringWithFormat:@"unknown/unresolved argument %u kind %d",b->aid,b->kind]);continue;}
        id<MTLBuffer> readback=ncap_readback(size);if(!readback)continue;
        NSMutableDictionary *r=[@{@"_buffer":readback,@"file":file,@"argument":@(b->aid),@"kind":@(b->kind),@"reg":@(b->reg),@"write":@(b->rw),@"atomic":@(b->atom),@"effective_gpu":@(arguments[b->aid]),@"size":@(size)} mutableCopy];
        if(b->reg<16){const p_bind_buf *u=b->kind==NVN_RB_SBUFP?&S.ssbo[5][b->reg]:&S.ubo[5][b->reg];r[@"guest_address"]=@(u->addr);r[@"bound_size"]=@(u->size);}
        [resources addObject:r];[jobs addObject:r];}
    if(!after&&dispatch->ind){id<MTLBuffer> data=ncap_readback(12);if(data){NSMutableDictionary *r=[@{@"_buffer":data,@"file":[NSString stringWithFormat:@"f%llu-d%u-indirect.bin",(unsigned long long)frame,g_ncap_dispatch_id],@"argument":@(-1),@"kind":@"indirect",@"effective_gpu":@(indirectGPU),@"guest_address":@(dispatch->ind),@"size":@12} mutableCopy];[resources addObject:r];[jobs addObject:r];}}
    if(jobs.count){static id<MTLComputePipelineState> copyPipeline;static id<MTL4ArgumentTable> table;
        if(!copyPipeline){NSError *error=nil;id<MTLLibrary> lib=host_lib("pose_probe",&error);id<MTLFunction> fn=[lib newFunctionWithName:@"pose_copy"];copyPipeline=fn?[g_dev newComputePipelineStateWithFunction:fn error:&error]:nil;MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=1;table=copyPipeline?rec_table([g_dev newArgumentTableWithDescriptor:d error:&error]):nil;if(!table)ncap_error(@"byte-copy pipeline unavailable");}
        if(table){typedef struct{uint64_t source,destination;uint32_t size,pad;}job;id<MTLBuffer> parameters;size_t offset=arena_alloc(jobs.count*sizeof(job),&parameters);job *p=(void *)((uint8_t *)parameters.contents+offset);[encoder setArgumentTable:table];[encoder setComputePipelineState:copyPipeline];
            for(NSUInteger i=0;i<jobs.count;i++){NSDictionary *r=jobs[i];id<MTLBuffer> dst=r[@"_buffer"];uint32_t size=[r[@"size"] unsignedIntValue];p[i]=(job){[r[@"effective_gpu"] unsignedLongLongValue],dst.gpuAddress,size,0};[table setAddress:parameters.gpuAddress+offset+i*sizeof(job) atIndex:0];[encoder dispatchThreadgroups:MTLSizeMake((size+63)/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];}}
    }
    [encoder barrierAfterEncoderStages:(MTLStageDispatch|MTLStageBlit) beforeEncoderStages:(MTLStageDispatch|MTLStageBlit) visibilityOptions:resource_vis()];[encoder setArgumentTable:g_ct];[encoder setComputePipelineState:nativePipeline];
    [g_ncap_records addObject:@{@"frame":@(frame),@"dispatch":@(g_ncap_dispatch_id),@"phase":phase,@"shader":@(shader->key),@"grid":@[@(dispatch->x),@(dispatch->y),@(dispatch->z)],@"threadgroup":@[@(shader->rf.tg[0]),@(shader->rf.tg[1]),@(shader->rf.tg[2])],@"indirect_guest":@(dispatch->ind),@"indirect_gpu":@(indirectGPU),@"first_stores":first,@"samplers":g_ncap_samplers.allValues?:@[],@"resources":resources}];
}
