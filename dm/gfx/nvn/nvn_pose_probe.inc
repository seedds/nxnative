// Bounded diagnostic of effective vertex uniforms. No normal-frame history policy.
static uint64_t g_pose_first=UINT64_MAX,g_pose_bytes;
static unsigned g_pose_frames=8,g_pose_per_frame,g_pose_palette_bytes;
static int g_pose_on,g_pose_done,g_pose_truncated,g_pose_copy_failed,g_pose_sync,g_pose_native,g_pose_native_incomplete;
static uint64_t g_pose_stop=UINT64_MAX,g_pose_trigger=UINT64_MAX;
static unsigned g_pose_target_width,g_pose_target_height;
static const char *g_pose_arm,*g_pose_out,*g_pose_key;
static NSMutableArray *g_pose_rows,*g_pose_jobs;
static int pose_probe_active(void) { if(!g_pose_on||g_pose_done)return 0;uint64_t f=atomic_load(&g_cur_frame);return f>=g_pose_first&&f-g_pose_first<g_pose_frames; }
static void pose_probe_frame(uint64_t frame) {
    static int configured;
    if(!configured){configured=1;g_pose_arm=getenv("DM_NVN_POSE_PROBE_ARM");g_pose_out=getenv("DM_NVN_POSE_PROBE_OUT");g_pose_key=getenv("DM_NVN_POSE_PROBE_KEY");g_pose_on=g_pose_arm&&*g_pose_arm&&g_pose_out&&*g_pose_out&&g_pose_key&&*g_pose_key;
        const char *native=getenv("DM_NVN_POSE_PROBE_NATIVE");g_pose_native=native&&!strcmp(native,"1");
        const char *n=getenv("DM_NVN_POSE_PROBE_FRAMES");if(n)g_pose_frames=(unsigned)MAX(1,MIN(8,atoi(n)));const char *sync=getenv("DM_NVN_POSE_PROBE_SYNC_TEMPORAL");g_pose_sync=sync&&atoi(sync);if(g_pose_sync)g_pose_frames=32;const char *palette=getenv("DM_NVN_POSE_PROBE_PALETTE_BYTES");if(palette)g_pose_palette_bytes=(unsigned)MIN(65536,MAX(0,atoi(palette)));}
    if(!g_pose_on||g_pose_done)return;
    g_pose_per_frame=0;
    if(g_pose_first==UINT64_MAX){if(g_pose_sync)return;FILE *f=fopen(g_pose_arm,"r");if(!f)return;fclose(f);g_pose_first=frame;g_pose_rows=[NSMutableArray array];g_pose_jobs=[NSMutableArray array];MLOG("POSE_PROBE_ARMED frame %llu frames %u",(unsigned long long)frame,g_pose_frames);return;}
    if(frame<g_pose_first+g_pose_frames&&frame<g_pose_stop)return;
    g_pose_done=1;uint64_t fence=g_cb?mtl_sub_commit():mtl_last_fence();int complete=mtl_wait(fence,2000000000ull);
    NSMutableArray *rows=[NSMutableArray array];NSError *error=nil;
    [[NSFileManager defaultManager] createDirectoryAtPath:@(g_pose_out) withIntermediateDirectories:YES attributes:nil error:&error];
    int io_ok=error==nil;
    for(NSDictionary *record in g_pose_rows){NSMutableDictionary *row=[record mutableCopy];
        for(NSString *category in @[@"bindings",@"geometry",@"native_bindings"]){NSMutableArray *saved=[NSMutableArray array];
        for(NSDictionary *binding in record[category]){NSMutableDictionary *b=[binding mutableCopy];id<MTLBuffer> data=b[@"buffer"];[b removeObjectForKey:@"buffer"];
            if(data&&([category isEqual:@"native_bindings"]||(complete&&!g_pose_copy_failed))){NSString *kind=b[@"kind"]?:@"cb";NSNumber *slot=b[@"slot"]?:b[@"reg"];NSString *name=[NSString stringWithFormat:@"f%@-d%@-%@%@.bin",record[@"frame"],record[@"ordinal"],kind,slot];NSData *bytes=[NSData dataWithBytes:data.contents length:[b[@"size"] unsignedIntegerValue]];
                if(![bytes writeToFile:[@(g_pose_out) stringByAppendingPathComponent:name] options:NSDataWritingAtomic error:&error])io_ok=0;b[@"file"]=name;}
            if(data){if(complete||[category isEqual:@"native_bindings"])res_remove(data);else defer_obj_release(data);}
            [saved addObject:b];}
        row[category]=saved;}[rows addObject:row];}
    NSDictionary *receipt=@{@"first_frame":@(g_pose_first),@"frames":@(g_pose_frames),@"stop_frame":@(g_pose_stop),@"temporal_trigger":@(g_pose_trigger),@"synchronized":@(g_pose_sync),@"synchronization_complete":@(!g_pose_sync||(g_pose_stop!=UINT64_MAX&&frame>=g_pose_stop)),@"complete":@(complete),@"copy_failed":@(g_pose_copy_failed),@"native_enabled":@(g_pose_native),@"native_incomplete":@(g_pose_native_incomplete),@"truncated":@(g_pose_truncated),@"io_ok":@(io_ok),@"bytes":@(g_pose_bytes),@"rows":rows};
    NSData *json=[NSJSONSerialization dataWithJSONObject:receipt options:NSJSONWritingPrettyPrinted error:&error];
    if(!json||![json writeToFile:[@(g_pose_out) stringByAppendingPathComponent:@"receipt.json"] options:NSDataWritingAtomic error:&error])io_ok=0;
    MLOG("%sPOSE_PROBE_DONE rows %lu bytes %llu complete=%d copy_failed=%d truncated=%d io_ok=%d",complete&&!g_pose_copy_failed&&io_ok?"":"[RED] ",(unsigned long)rows.count,(unsigned long long)g_pose_bytes,complete,g_pose_copy_failed,g_pose_truncated,io_ok);
    g_pose_rows=nil;g_pose_jobs=nil;
}
// Triggered only after a real temporal input capture has been encoded.
// Its drawing is already complete, so poses begin with the next replay frame.
static void pose_probe_temporal_capture(uint64_t snapshot_frame,unsigned width,unsigned height,int last) {
    if(!g_pose_on||!g_pose_sync||g_pose_done)return;
    uint64_t frame=atomic_load(&g_cur_frame);
    if(g_pose_first==UINT64_MAX){g_pose_first=frame+1;g_pose_trigger=snapshot_frame;g_pose_target_width=width;g_pose_target_height=height;g_pose_rows=[NSMutableArray array];g_pose_jobs=[NSMutableArray array];
        MLOG("POSE_PROBE_SYNC snapshot %llu replay %llu first %llu",(unsigned long long)snapshot_frame,(unsigned long long)frame,(unsigned long long)g_pose_first);}
    if(last)g_pose_stop=frame+1;
}
// Raw geometry snapshots are diagnostic inputs. Converted/pulled fetch metadata
// is retained separately so an offline replay must reproduce the fetch contract.
static NSDictionary *pose_probe_geometry(uint64_t guest,uint64_t size,NSString *kind,unsigned slot) {
    if(!guest||!size||size>(2u<<20)||g_pose_bytes+size>64ull*1024*1024){g_pose_truncated=1;return nil;}
    void *buffer;uint64_t offset,source;
    if(!mtl_gpu_addr(guest,size,&buffer,&offset,&source)){g_pose_copy_failed=1;return nil;}
    id<MTLBuffer> data=[g_dev newBufferWithLength:(NSUInteger)size options:MTLResourceStorageModeShared];
    if(!data){g_pose_copy_failed=1;return nil;}res_add(data);g_pose_bytes+=size;
    NSDictionary *copy=@{@"kind":kind,@"slot":@(slot),@"guest_address":@(guest),@"effective_gpu":@(source),@"size":@(size),@"buffer":data};[g_pose_jobs addObject:copy];return copy;
}
// Native ranges never use the argument-buffer address: that can be a short prefix shadow.
// Returned CPU-owned bytes are immutable at this draw and never enter g_pose_jobs.
static NSDictionary *pose_probe_native_binding(NvShader *sh,unsigned reg) {
    const p_bind_buf *u=&S.ubo[0][reg];int used=0;
    for(int i=0;i<sh->rf.nb;i++)if(sh->rf.b[i].kind==NVN_RB_CBUF&&sh->rf.b[i].reg==reg){used=1;break;}
    uint64_t n=u->size,prefix=used?MIN(cb_len(sh,reg,n),65536ull):0;
    NSMutableDictionary *b=[@{@"kind":@"native_cb",@"stage":@0,@"reg":@(reg),@"native_address":@(u->addr),@"native_size":@(n),@"shader_prefix_size":@(prefix),
        @"shader_used":@(used),@"binding_semantics":used?@"shader-used":@"unused-or-stale-binding",@"read_size":@0,@"size":@0,@"status":@"unbound"} mutableCopy];
    if(!u->addr||!n){if(u->addr||n){b[@"status"]=@"incomplete-empty-binding";g_pose_native_incomplete=1;}return b;}
    if(n>UBP_MAX_BYTES){b[@"status"]=@"incomplete-oversize";g_pose_truncated=g_pose_native_incomplete=1;return b;}
    if(u->addr>UINT64_MAX-n){b[@"status"]=@"incomplete-range-overflow";g_pose_native_incomplete=1;return b;}
    const uint64_t limit=64ull*1024*1024;
    if(g_pose_bytes>limit||n>limit-g_pose_bytes){b[@"status"]=@"incomplete-byte-budget";g_pose_truncated=g_pose_native_incomplete=1;return b;}
    id<MTLBuffer> data=[g_dev newBufferWithLength:(NSUInteger)n options:MTLResourceStorageModeShared];
    if(!data){b[@"status"]=@"incomplete-allocation";g_pose_native_incomplete=1;return b;}
    uint64_t hi=u->addr+n,gpu=0;const uint8_t *cpu=NULL;
    if(g_nubd&&ubd_covers(u->addr,hi)) {
        // Full immutable inline coverage supersedes earlier writes, including GPU-only ones.
        ubd_read(u->addr,(size_t)n,data.contents);b[@"resolution"]=@"full-inline-updates";
    } else {
        uint64_t why[5];memcpy(why,g_ubp_why,sizeof why);
        if(!ubp_get(u->addr,hi,&gpu,&cpu)) {
            static NSString *const reasons[]={@"shadow-disabled",@"unknown-gpu-write",@"earlier-inflight-gpu-write",@"queued-copy-fill-or-feedback",@"unreadable-or-write-overflow"};
            b[@"status"]=@"incomplete-shadow-unavailable";b[@"reason"]=@"unclassified";
            for(unsigned i=0;i<5;i++)if(g_ubp_why[i]!=why[i]){b[@"reason"]=reasons[i];break;}
            g_pose_native_incomplete=1;return b;
        }
        ubc_wait(g_ubp_ready);memcpy(data.contents,cpu,(size_t)n);b[@"resolution"]=@"full-range-shadow";b[@"resolved_gpu"]=@(gpu);
    }
    res_add(data);g_pose_bytes+=n;b[@"buffer"]=data;b[@"read_size"]=@(n);b[@"size"]=@(n);b[@"status"]=@"captured";return b;
}
static void pose_probe_note(NvProg *program,const p_draw *dw,const p_draw_idx *di,int pulled) {
    if(!pose_probe_active()||!program->sh[0]||!R.rt.depth||!g_pass.hd||!S.ds.dw)return;
    int seed=!strcmp(program->sh[0]->key,g_pose_key),candidate=0;
    if(!seed&&g_pose_palette_bytes&&S.ubo[0][3].size==g_pose_palette_bytes)
        for(int i=0;i<program->sh[0]->rf.nb;i++)if(program->sh[0]->rf.b[i].kind==NVN_RB_CBUF&&program->sh[0]->rf.b[i].reg==3){candidate=1;break;}
    if(!seed&&!candidate)return;
    if(g_pose_per_frame>=64){g_pose_truncated=1;return;}
    NvShader *sh=program->sh[0];size_t arg_len=sh->arg_len?sh->arg_len:sh->ae.encodedLength;if(!sh->ab_buf||sh->ab_off+arg_len>sh->ab_buf.length){g_pose_copy_failed=1;return;}
    NSMutableArray *bindings=[NSMutableArray array];const uint64_t *slots=(const void *)((const uint8_t *)sh->ab_buf.contents+sh->ab_off);
    for(int i=0;i<sh->rf.nb;i++){const nvn_rbind *b=&sh->rf.b[i];if(b->kind!=NVN_RB_CBUF||b->reg>=16)continue;
        const p_bind_buf *u=&S.ubo[0][b->reg];uint64_t n=MIN(cb_len(sh,b->reg,u->size),65536ull);if(!u->addr||!n)continue;
        if((b->aid+1)*sizeof(uint64_t)>arg_len||g_pose_bytes+n>64ull*1024*1024){g_pose_truncated=1;continue;}
        uint64_t source=slots[b->aid];if(!source){g_pose_copy_failed=1;continue;}
        id<MTLBuffer> data=[g_dev newBufferWithLength:(NSUInteger)n options:MTLResourceStorageModeShared];if(!data){g_pose_copy_failed=1;continue;}res_add(data);g_pose_bytes+=n;
        NSDictionary *binding=@{@"reg":@(b->reg),@"guest_address":@(u->addr),@"effective_gpu":@(source),@"size":@(n),@"buffer":data};[bindings addObject:binding];[g_pose_jobs addObject:binding];}
    NSMutableArray *native_bindings=nil;
    if(g_pose_native&&seed){native_bindings=[NSMutableArray array];for(unsigned reg=0;reg<4;reg++)[native_bindings addObject:pose_probe_native_binding(sh,reg)];}
    NSMutableArray *vertices=[NSMutableArray array],*attributes=[NSMutableArray array],*geometry=[NSMutableArray array];
    static int capture_geometry=-1;if(capture_geometry<0){const char *v=getenv("DM_NVN_POSE_PROBE_GEOMETRY");capture_geometry=v&&atoi(v);}
    NSString *geometry_status=@"disabled";
    if(capture_geometry&&g_pose_target_width&&(g_pass.tw!=g_pose_target_width||g_pass.th!=g_pose_target_height))geometry_status=@"outside-synchronized-target";
    else if(capture_geometry){
        geometry_status=@"raw";
        if((dw&&dw->ind)||(di&&(di->ind||di->itype<0||di->itype>2))||g_geo_program||g_mtk.on||g_geo_mesh.on)geometry_status=@"unsupported-generated-or-indirect";
        else {
            unsigned used=vs_streams(&sh->rf);
            for(unsigned st=0;st<16;st++)if(used>>st&1){
                if((S.vbuf[st].address&3)||g_vbuf_need[st]){geometry_status=@"unsupported-unaligned-or-padded";continue;}
                NSDictionary *copy=pose_probe_geometry(S.vbuf[st].address,S.vbuf[st].size,@"vertex",st);if(copy)[geometry addObject:copy];}
            if(di&&di->count>0){NSDictionary *copy=pose_probe_geometry(di->ibuf,(uint64_t)di->count*(1u<<di->itype),@"index",0);if(copy)[geometry addObject:copy];}
        }
    }
    for(int i=0;i<16;i++)if(S.vbuf[i].address)[vertices addObject:@{@"stream":@(i),@"address":@(S.vbuf[i].address),@"size":@(S.vbuf[i].size),@"stride":@(S.stream[i].stride),@"divisor":@(S.stream[i].divisor)}];
    for(int i=0;i<S.nattr&&i<32;i++)if(S.attr[i].fmt){int converted=0,used=0;
        for(int v=0;v<sh->rf.nv;v++)if(sh->rf.v[v].nvn==i){used=1;converted=vconv_wanted(&sh->rf,v,NULL,vin_on(sh,NULL,S.rast_discard));}
        [attributes addObject:@{@"location":@(i),@"format":@(S.attr[i].fmt),@"offset":@(S.attr[i].offset),@"stream":@(S.attr[i].stream),@"used":@(used),@"converted":@(converted)}];}
    NSData *draw=[NSData dataWithBytes:dw?(const void *)dw:(const void *)di length:dw?sizeof(*dw):sizeof(*di)];
    [g_pose_rows addObject:@{@"frame":@(atomic_load(&g_cur_frame)),@"ordinal":@(g_pose_rows.count),@"pass":@(g_stat_pass),@"draw":@(g_stat_draw+1),@"depth_serial":@(R.rt.depth->serial),@"width":@(g_pass.tw),@"height":@(g_pass.th),@"vertex":@(sh->key),@"seed":@(seed),@"indexed":@(di!=NULL),@"draw_struct_base64":[draw base64EncodedStringWithOptions:0],@"draw_struct_size":@(draw.length),@"vertices":vertices,@"attributes":attributes,@"bindings":bindings,@"native_bindings":native_bindings?:@[],@"native_enabled":@(g_pose_native&&seed),@"geometry":geometry,@"geometry_status":geometry_status,@"pulled":@(pulled),
        @"viewport":@[@(g_ed.vp.originX),@(g_ed.vp.originY),@(g_ed.vp.width),@(g_ed.vp.height),@(g_ed.vp.znear),@(g_ed.vp.zfar)],
        @"scissor":@[@(g_ed.sc.x),@(g_ed.sc.y),@(g_ed.sc.width),@(g_ed.sc.height)],@"cull":@(g_ed.cull),@"winding":@(g_ed.wind)}];g_pose_per_frame++;
}
// Called after the render encoder ends, BEFORE deferred inline updates land.
// blit_raw/enc_c_begin would land those updates first and copy the wrong pose.
static void pose_probe_flush(void) {
    if(!g_pose_jobs.count)return;
    NSArray *jobs=g_pose_jobs;g_pose_jobs=[NSMutableArray array];
    static id<MTLComputePipelineState> pipeline;static id<MTL4ArgumentTable> table;
    if(!pipeline){NSError *error=nil;id<MTLLibrary> lib=host_lib("pose_probe",&error);id<MTLFunction> fn=[lib newFunctionWithName:@"pose_copy"];
        pipeline=fn?[g_dev newComputePipelineStateWithFunction:fn error:&error]:nil;MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=1;table=pipeline?rec_table([g_dev newArgumentTableWithDescriptor:d error:&error]):nil;
        if(!table){g_pose_copy_failed=1;MLOG("[RED] pose probe copy pipeline: %s",error.localizedDescription.UTF8String);return;}}
    typedef struct {uint64_t source,destination;uint32_t size,pad;} job;
    id<MTLBuffer> params;size_t offset=arena_alloc(jobs.count*sizeof(job),&params);job *p=(void *)((uint8_t *)params.contents+offset);
    id<MTL4ComputeCommandEncoder> e=blit_open();[e setArgumentTable:table];[e setComputePipelineState:pipeline];
    for(NSUInteger i=0;i<jobs.count;i++){NSDictionary *j=jobs[i];id<MTLBuffer> data=j[@"buffer"];uint32_t n=[j[@"size"] unsignedIntValue];p[i]=(job){[j[@"effective_gpu"] unsignedLongLongValue],data.gpuAddress,n,0};
        [table setAddress:params.gpuAddress+offset+i*sizeof(job) atIndex:0];[e dispatchThreadgroups:MTLSizeMake((n+63)/64,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];}
    enc_b_end(g_blit);g_blit=nil;g_blit_ct=0;args_dirty_all();
}
