// Copy ordering and binding provenance, using the production pass-end hook.
#define NVN_PIXEL_PROBE_NO_MAIN 1
#include "t_pixel_probe.m"
int main(void){@autoreleasepool {
    char arm[256],out[256];snprintf(arm,sizeof arm,"/tmp/pose-probe-%d.arm",getpid());snprintf(out,sizeof out,"/tmp/pose-probe-%d",getpid());unlink(arm);
    setenv("DM_NVN_POSE_PROBE_ARM",arm,1);setenv("DM_NVN_POSE_PROBE_OUT",out,1);setenv("DM_NVN_POSE_PROBE_KEY","pose-fixture",1);setenv("DM_NVN_POSE_PROBE_FRAMES","1",1);setenv("DM_NVN_UB_LAZY","1",1);setenv("DM_NVN_POSE_PROBE_GEOMETRY","1",1);
    int native=getenv("DM_TEST_POSE_NATIVE")!=NULL;
    if(native){setenv("DM_NVN_POSE_PROBE_NATIVE","1",1);setenv("DM_NVN_UB_PARTIAL","1",1);}else unsetenv("DM_NVN_POSE_PROBE_NATIVE");
    int broad=getenv("DM_TEST_POSE_BROAD")!=NULL;if(broad)setenv("DM_NVN_POSE_PROBE_PALETTE_BYTES",native?"1024":"64",1);
    if(getenv("DM_TEST_POSE_SYNC"))setenv("DM_NVN_POSE_PROBE_SYNC_TEMPORAL","1",1);
    if(pixel_probe_test())return 1;
    CHECK(g_pose_first==UINT64_MAX,"pose probe stays off before its ARM file exists");
    void *mem=NULL;CHECK(!posix_memalign(&mem,getpagesize(),65536),"source pool allocated");memset(mem,0xee,65536);mtl_pool_add(mem,65536);mtl_pool_addressed(mem,65536);
    void *mb;uint64_t mo,gpu;CHECK(mtl_gpu_addr((uint64_t)mem,64,&mb,&mo,&gpu),"guest uniform resolves to GPU storage");
    id<MTLBuffer> shadow=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];memset(shadow.contents,0x31,64);res_add(shadow);
    id<MTLBuffer> args=[g_dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
    NvProg *probe=[NvProg new];NvShader *sh=[NvShader new];probe->sh[0]=sh;strcpy(sh->key,"pose-fixture");sh->ab_buf=args;sh->arg_len=8;sh->rf.nb=1;sh->rf.b[0]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=3,.aid=0,.fixw=16};
    FILE *f=fopen(arm,"w");fclose(f);if(g_pose_sync)pose_probe_temporal_capture(11,8,8,0);mtl_set_frame(12);CHECK(pose_probe_active(),"one frame capture arms");
    state_reset();memset(&R,0,sizeof R);mtl_sub_begin();seed();S.ds=(ds_st){.dt=1,.dw=1,.df=NVN_DEPTH_FUNC_LESS};g_dss_dirty=1;px_draw(0);
    S.ubo[0][3]=(p_bind_buf){.addr=(uint64_t)mem,.size=64};p_draw draw={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3};
    if(native){
        // Keep the original 64-byte effective-shadow fixture. Only native size grows.
        sh->cb_ext[3]=64;S.ubo[0][3].size=1024;S.ubo[0][2]=(p_bind_buf){.addr=(uint64_t)mem+4096,.size=48};
        uint8_t tail[32],material[8];memset(tail,0x52,sizeof tail);memset(material,0x73,sizeof material);
        mtl_sub_update(tail,(uint64_t)mem+768,sizeof tail);mtl_sub_update(material,(uint64_t)mem+4096,sizeof material);
        CHECK(g_pose_native,"native bytes require the explicit diagnostic opt-in");
        uint64_t bytes=g_pose_bytes;int truncated=g_pose_truncated,incomplete=g_pose_native_incomplete;NSUInteger jobs=g_pose_jobs.count;
        uint8_t half[48];memset(half,0x41,sizeof half);uint64_t full=(uint64_t)mem+8192;
        sbw_note(full,96,SBW_GPU);mtl_sub_update(half,full,48);memset(half,0x42,sizeof half);mtl_sub_update(half,full+48,48);
        S.ubo[0][0]=(p_bind_buf){.addr=full,.size=96};NSDictionary *binding=pose_probe_native_binding(sh,0);id<MTLBuffer> owned=binding[@"buffer"];
        CHECK([binding[@"status"] isEqual:@"captured"]&&[binding[@"resolution"] isEqual:@"full-inline-updates"],"full immutable inline coverage works despite earlier unknown GPU data");
        int covered=owned.length==96;for(NSUInteger j=0;j<owned.length;j++)covered&=((const uint8_t *)owned.contents)[j]==(j<48?0x41:0x42);CHECK(covered,"full inline native bytes preserve both deferred updates");
        CHECK(g_pose_jobs.count==jobs,"CPU-owned native bytes are not appended to GPU copy jobs");if(owned)res_remove(owned);g_pose_bytes=bytes;
        uint64_t gpu_only=(uint64_t)mem+12288;mtl_sub_update(half,gpu_only,16);sbw_note(gpu_only,96,SBW_GPU);S.ubo[0][0]=(p_bind_buf){.addr=gpu_only,.size=96};binding=pose_probe_native_binding(sh,0);
        CHECK([binding[@"status"] isEqual:@"incomplete-shadow-unavailable"]&&[binding[@"reason"] isEqual:@"unknown-gpu-write"]&&!binding[@"buffer"]&&[binding[@"read_size"] unsignedIntValue]==0,"partly updated GPU-written native range refuses stale raw CPU fallback");
        uint64_t copied=(uint64_t)mem+16384;CHECK(pend_add(copied,96,2)>=0,"unknown COPY fixture enters the ordered pending log");mtl_sub_update(half,copied,16);S.ubo[0][0]=(p_bind_buf){.addr=copied,.size=96};binding=pose_probe_native_binding(sh,0);
        CHECK([binding[@"reason"] isEqual:@"queued-copy-fill-or-feedback"]&&!binding[@"buffer"],"unresolved COPY tail is explicitly incomplete, never read from raw CPU");
        S.ubo[0][0]=(p_bind_buf){.addr=(uint64_t)mem,.size=UBP_MAX_BYTES+1};binding=pose_probe_native_binding(sh,0);
        CHECK([binding[@"status"] isEqual:@"incomplete-oversize"]&&[binding[@"native_size"] unsignedLongLongValue]==UBP_MAX_BYTES+1&&!binding[@"buffer"],"oversize records original native size without a clipped read");
        S.ubo[0][0]=(p_bind_buf){.addr=UINT64_MAX-8,.size=16};binding=pose_probe_native_binding(sh,0);CHECK([binding[@"status"] isEqual:@"incomplete-range-overflow"]&&!binding[@"buffer"],"native end-address overflow is refused before resolution");
        S.ubo[0][0]=(p_bind_buf){.addr=full,.size=96};g_pose_bytes=64ull*1024*1024-32;binding=pose_probe_native_binding(sh,0);CHECK([binding[@"status"] isEqual:@"incomplete-byte-budget"]&&!binding[@"buffer"],"total capture byte cap refuses the entire native range");
        S.ubo[0][0]=(p_bind_buf){0};binding=pose_probe_native_binding(sh,0);CHECK([binding[@"status"] isEqual:@"unbound"]&&!binding[@"buffer"],"unbound native register preserves an explicit empty status");
        S.ubo[0][0]=(p_bind_buf){.addr=gpu_only,.size=96}; // Keep one refused register in the serialized receipt.
        g_pose_bytes=bytes;g_pose_truncated=truncated;g_pose_native_incomplete=incomplete;
    }
    *(uint64_t *)args.contents=shadow.gpuAddress;sh->rf.nv=1;sh->rf.v[0].nvn=0;sh->rf.v[0].ct=NVN_CT_FLOAT;S.nattr=1;S.attr[0].fmt=NVN_FORMAT_RGB32F;S.attr[0].stream=0;S.stream[0].stride=12;S.vbuf[0].address=(uint64_t)mem+64;S.vbuf[0].size=256;pose_probe_note(probe,&draw,NULL,0);
    if(broad)strcpy(sh->key,"pose-candidate");
    *(uint64_t *)args.contents=gpu;S.vbuf[0].address=(uint64_t)mem+128;pose_probe_note(probe,&draw,NULL,0);
    p_draw_idx indexed={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=1,.count=3,.ibuf=(uint64_t)mem+512};pose_probe_note(probe,NULL,&indexed,0);
    if(broad){S.ubo[0][3].size=32;pose_probe_note(probe,&draw,NULL,0);S.ubo[0][3].size=native?1024:64;}
    CHECK(!pose_probe_geometry((uint64_t)mem,(2u<<20)+1,@"vertex",0),"oversized geometry snapshot is rejected");g_pose_truncated=0;
    CHECK(g_pose_rows.count==3,"shared shader records distinct geometry bindings");
    if(native)memset((uint8_t *)mem+896,0x99,32); // Future raw CPU tail change must not alter an owned native snapshot.
    uint8_t later[64];memset(later,0x99,sizeof later);mtl_sub_update(later,(uint64_t)mem,64);
    CHECK(g_nubd>0,"source rewrite is deferred while render pass is open");rp_end();CHECK(!g_pose_jobs.count,"pass end encodes pending snapshots");
    id<MTLBuffer> overwrite=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];memset(overwrite.contents,0x66,64);res_add(overwrite);
    enc_b_copy_buffer(blit(),overwrite,0,shadow,0,64);
    enc_b_copy_buffer(blit(),overwrite,0,(__bridge id<MTLBuffer>)mb,mo+64,64);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"snapshot and later writes complete");CHECK(!mtl_last_commit_error(),"snapshot GPU work reports no error");
    if(g_pose_sync)pose_probe_temporal_capture(12,8,8,1);mtl_set_frame(13);CHECK(g_pose_done,"capture stops at configured frame bound");
    NSData *receipt=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:@"receipt.json"]];NSDictionary *result=[NSJSONSerialization JSONObjectWithData:receipt options:0 error:nil];
    CHECK([result[@"complete"] boolValue]&&![result[@"copy_failed"] boolValue]&&[result[@"io_ok"] boolValue],"complete receipt has no copy or IO failure");NSArray *rows=result[@"rows"];CHECK(rows.count==3,"receipt preserves the captured draw identities");
    CHECK([rows[0][@"seed"] boolValue]&&[rows[1][@"seed"] boolValue]==!broad,"receipt distinguishes exact shader from palette-size candidates");
    CHECK(![rows[0][@"vertices"] isEqual:rows[1][@"vertices"]],"same shader does not erase distinct vertex identities");
    uint8_t expected[2]={0x31,0xee};for(int i=0;i<2;i++){NSString *name=rows[i][@"bindings"][0][@"file"];NSData *data=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:name]];int equal=data.length==64;for(NSUInteger j=0;j<data.length;j++)equal&=((const uint8_t *)data.bytes)[j]==expected[i];CHECK(equal,i?"snapshot precedes deferred guest overwrite":"effective shadow captured instead of stale guest bytes");}
    if(native){
        CHECK([result[@"native_enabled"] boolValue]&&[result[@"native_incomplete"] boolValue],"deliberate unavailable native slot is recorded independently of successful prefix GPU copies");
        for(NSDictionary *row in rows){NSArray *native_rows=row[@"native_bindings"];
            if(![row[@"seed"] boolValue]){CHECK(!native_rows.count&&![row[@"native_enabled"] boolValue],"palette-size candidates do not broaden the exact-key native diagnostic");continue;}
            CHECK(native_rows.count==4,"native receipt preserves all four stage-zero register statuses");
            CHECK([native_rows[0][@"status"] isEqual:@"incomplete-shadow-unavailable"]&&[native_rows[0][@"reason"] isEqual:@"unknown-gpu-write"]&&[native_rows[1][@"status"] isEqual:@"unbound"]&&!native_rows[0][@"file"]&&!native_rows[1][@"file"],"refused and unbound native registers retain separate statuses without attempted buffer files");
            NSDictionary *material=native_rows[2],*palette=native_rows[3];
            CHECK(![material[@"shader_used"] boolValue]&&[material[@"binding_semantics"] isEqual:@"unused-or-stale-binding"]&&[material[@"shader_prefix_size"] unsignedIntValue]==0,"unread native CB2 is retained without claiming shader use or actor identity");
            NSData *mat=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:material[@"file"]]];int material_ok=mat.length==48;for(NSUInteger j=0;j<mat.length;j++)material_ok&=((const uint8_t *)mat.bytes)[j]==(j<8?0x73:0xee);CHECK(material_ok,"unreflected CB2 keeps its full native tail and partial inline update");
            CHECK([palette[@"native_address"] unsignedLongLongValue]==(uint64_t)mem&&[palette[@"native_size"] unsignedIntValue]==1024&&[palette[@"shader_prefix_size"] unsignedIntValue]==64&&[palette[@"read_size"] unsignedIntValue]==1024,"original native address/size, shader prefix and actual copied length are distinct");
            NSData *data=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:palette[@"file"]]];int native_ok=data.length==1024;for(NSUInteger j=0;j<data.length;j++)native_ok&=((const uint8_t *)data.bytes)[j]==(j>=768&&j<800?0x52:0xee);CHECK(native_ok,"full native bytes use safe range shadows, retain uncaptured tails, and survive future CPU overwrite");
            CHECK([row[@"bindings"][0][@"size"] unsignedIntValue]==64,"the original effective argument shadow is still read for only 64 bytes");
        }
    }else {CHECK(![result[@"native_enabled"] boolValue],"native diagnostic remains off unless explicitly requested");for(NSDictionary *row in rows)CHECK(![row[@"native_bindings"] count],"normal probe mode performs no full native capture");}
    CHECK([rows[0][@"geometry"] count]==1&&[rows[1][@"geometry"] count]==1,"only shader-used vertex streams are copied");
    for(int i=0;i<2;i++){NSString *name=rows[i][@"geometry"][0][@"file"];NSData *data=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:name]];int equal=data.length==256;for(NSUInteger j=0;j<data.length;j++)equal&=((const uint8_t *)data.bytes)[j]==0xee;CHECK(equal,"raw vertex snapshot survives later source overwrite");}
    CHECK([rows[2][@"geometry"] count]==2,"indexed draw captures its vertex and index sources");
    NSDictionary *index=rows[2][@"geometry"][1];NSData *ix=[NSData dataWithContentsOfFile:[@(out) stringByAppendingPathComponent:index[@"file"]]];
    CHECK([index[@"kind"] isEqual:@"index"]&&ix.length==6,"UInt16 capture length follows the draw index count");
    int index_ok=1;for(NSUInteger j=0;j<ix.length;j++)index_ok&=((const uint8_t *)ix.bytes)[j]==0xee;CHECK(index_ok,"index snapshot contains literal source bytes");
    CHECK(((uint8_t *)shadow.contents)[0]==0x66,"effective shadow was later overwritten on GPU");CHECK(((uint8_t *)[(__bridge id<MTLBuffer>)mb contents])[mo]==0x99,"deferred guest overwrite landed after snapshot");
    CHECK(!pose_probe_active(),"capture cannot rearm itself");unlink(arm);printf("POSE_PROBE %d checks, %d failed; %s\n",checks,failed,out);return failed!=0;
}}
