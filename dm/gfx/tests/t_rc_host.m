// Experimental GPU arithmetic test; no game replay or installed app changes.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
typedef struct { uint64_t xs, ys, rec, out, xc, yc; uint32_t nx, ny, ne, pad; } job;
_Static_assert(sizeof(job)==64,"Metal job ABI");
int main(int argc, char **argv) { @autoreleasepool {
    assert(!rc_mask_direct(NULL,NULL));
    assert(argc==2); setenv("DM_NVN_GPU_PREDICATE","1",1); mtl_init(); NSError *error=nil;
    NSString *source=[NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error]; assert(source);
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:source options:nil error:&error];
    if(!lib){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 1;}
    id<MTLFunction> function=[lib newFunctionWithName:@"rc_mesh_mask"];assert(function);
    id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:function error:&error];assert(ps);
    id<MTLFunction> draw_function=[lib newFunctionWithName:@"rc_draw_mask"];assert(draw_function);
    id<MTLComputePipelineState> psdraw=[g_dev newComputePipelineStateWithFunction:draw_function error:&error];assert(psdraw);
    enum { N=64 };
    id<MTLBuffer> seed=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> gpu=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> pointers=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLBuffer> jobs=[g_dev newBufferWithLength:N*sizeof(job) options:MTLResourceStorageModeShared];
    id<MTLBuffer> out=[g_dev newBufferWithLength:N*32 options:MTLResourceStorageModeShared];
    assert(seed && gpu && pointers && jobs && out);
    fprintf(stderr,"GPU operand storage mode: %lu (private=%lu)\n",(unsigned long)gpu.storageMode,(unsigned long)MTLStorageModePrivate);
    assert(gpu.storageMode==MTLStorageModePrivate); // Never read contents; GPU copy supplies every operand.
    for(id b in @[seed,gpu,pointers,jobs,out])res_add(b);
    MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new];ad.maxBufferBindCount=1;
    id<MTL4ArgumentTable> at=rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);assert(at);
    [at setAddress:jobs.gpuAddress atIndex:0];
    uint64_t *ptr=pointers.contents;for(int k=0;k<8;k++)ptr[k]=gpu.gpuAddress+8*k;
    int checks=0;
    for(int iteration=0;iteration<8;iteration++) {
        uint64_t vals[8]={0,1,2,UINT32_MAX,UINT64_C(0x100000000),UINT64_MAX,17,3};
        vals[0]=(iteration&1)?9:0; vals[1]+=iteration;
        memcpy(seed.contents,vals,sizeof vals); memset(out.contents,0xa5,out.length);
        job *j=jobs.contents;uint32_t expected[N][5];
        for(int i=0;i<N;i++) {
            uint32_t nx=i%3,ny=(i/3)%3,ix=(i/9)%6,iy=(i/7)%6;
            uint64_t xc=i%4==0?UINT64_MAX:0,yc=i%5==0?UINT64_C(0x100000000):0;
            // Explicit high-bit, overflow and equal cases supplement the mixed cases.
            if(i==0){nx=ny=0;xc=yc=0;}
            if(i==1){nx=ny=0;xc=UINT64_C(0x100000000);yc=0;}
            if(i==2){nx=ny=1;ix=iy=5;xc=yc=0;}
            if(i==3){nx=1;ix=5;xc=1;ny=0;yc=0;}
            uint64_t x=xc,y=yc;for(uint32_t k=0;k<nx;k++)x+=vals[ix+k];for(uint32_t k=0;k<ny;k++)y+=vals[iy+k];
            uint32_t ne=(i+iteration)&1, count=i%11==0?0:3u+i,first=17u+i*101u;
            uint32_t record[2]={count,first};memcpy((uint8_t*)seed.contents+128+i*8,record,8);
            j[i]=(job){pointers.gpuAddress+ix*8,pointers.gpuAddress+iy*8,gpu.gpuAddress+128+i*8,out.gpuAddress+i*32,xc,yc,nx,ny,ne,0};
            expected[i][0]=(ne?x!=y:x==y)?count:0;expected[i][1]=expected[i][2]=1;expected[i][3]=0;expected[i][4]=first;
        }
        mtl_sub_begin();enc_b_copy_buffer(blit(),seed,0,gpu,0,4096);if(g_blit){enc_b_end(g_blit);g_blit=nil;}
        id<MTL4ComputeCommandEncoder> ce=enc_c_begin(nil);[ce setArgumentTable:at];enc_c_pso(ce,ps);
        enc_c_dispatch(ce,MTLSizeMake(N,1,1),MTLSizeMake(1,1,1));enc_c_end(ce);mtl_sub_commit();mtl_drain();assert(!mtl_last_commit_error());
        for(int i=0;i<N;i++) {
            uint32_t *actual=(uint32_t*)((uint8_t*)out.contents+i*32);
            assert(!memcmp(actual,expected[i],sizeof expected[i]));checks++;
            assert(actual[5]==0xa5a5a5a5 && actual[6]==0xa5a5a5a5 && actual[7]==0xa5a5a5a5);checks++;
        }
    }

    // Real raster visibility writes feed the compute predicate without CPU readback.
    NSString *raster=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[position]];};\n"
        "vertex V visible(uint i [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return V{float4(p[i],0,1)};}\n"
        "vertex V hidden(uint i [[vertex_id]]){return V{float4(4+float(i),4,0,1)};}\n"
        "fragment float4 colour(){return float4(1,0,0,1);}\n";
    id<MTLLibrary> rl=[g_dev newLibraryWithSource:raster options:nil error:&error];assert(rl);
    id<MTLRenderPipelineState> rp[2];
    for(int k=0;k<2;k++) { MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
        d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        d.vertexFunction=[rl newFunctionWithName:k?@"visible":@"hidden"];d.fragmentFunction=[rl newFunctionWithName:@"colour"];
        rp[k]=[g_dev newRenderPipelineStateWithDescriptor:d error:&error];assert(rp[k]); }
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=995001,.off=(int64_t)995001<<20};
    target.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=16,.height=16,.depth=1,.levels=1,.samples=1};
    tex_t consumer=target;consumer.serial++;consumer.off+=1<<20;
    id<MTLBuffer> indices=[g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
    id<MTLBuffer> pixels=[g_dev newBufferWithLength:16*16*4 options:MTLResourceStorageModeShared];
    res_add(indices);res_add(pixels);for(int i=0;i<32;i++)((uint32_t*)indices.contents)[i]=(uint32_t)i;
    void *guest=NULL;assert(!posix_memalign(&guest,getpagesize(),65536));memset(guest,0,65536);mtl_pool_add(guest,65536);
    for(int format=0;format<2;format++)for(int iteration=0;iteration<32;iteration++)for(int mode=0;mode<2;mode++) {
        int visible=iteration&1;uint32_t record[5]={format?3u:13u,format?1u:29u,7,0xfffffff9u,29};memcpy(seed.contents,record,20);
        memset(out.contents,0xa5,out.length);mtl_sub_begin();
        enc_b_copy_buffer(blit(),seed,0,gpu,0,20);if(g_blit){enc_b_end(g_blit);g_blit=nil;}
        state_reset();memset(&R,0,sizeof R);R.has_rt=R.has_sc=1;R.sc=(p_scissor){0,0,16,16};
        R.rt.n=1;R.rt.color[0]=&target;R.clear_c[0]=1;R.cc[0]=MTLClearColorMake(0,0,0,1);g_vis_on=1;
        id<MTL4RenderCommandEncoder> re=rp_begin_ex(0);assert(re);
        enc_r_viewport(re,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(re,(MTLScissorRect){0,0,16,16});enc_r_cull(re,MTLCullModeNone);
        enc_r_pso(re,rp[visible]);enc_r_visibility(re,MTLVisibilityResultModeCounting,0);enc_r_draw(re,MTLPrimitiveTypeTriangle,0,3);
        uint64_t *ptr=pointers.contents;ptr[0]=g_vis.gpuAddress;
        rp_end();g_vis_on=0;
        assert(!g_rc_gpu.count); // Every new submission retires the previous map.
        uint64_t report[4]={0};uint64_t address=(uint64_t)report;
        struct {int n;vis_slot s[1];} snapshot={1,{{(__bridge void*)g_vis,0,g_sub_serial}}};
        g_vis_n=1; // This fixture recorded visibility slot zero directly.
        snapshot.s[0].submission=g_sub_serial-1;rc_gpu_samples(address,(const vis_list*)&snapshot);assert(!npm_find(&g_rc_gpu,address)->kind);
        snapshot.s[0].submission=g_sub_serial;
        rc_gpu_samples(address,(const vis_list*)&snapshot);
        nvn_predicate_operand operand=*npm_find(&g_rc_gpu,address);assert(operand.kind==2&&operand.count==1);
        pend_add(address+16,4,2);assert(npm_find(&g_rc_gpu,address)->kind==2);
        pend_add(address+7,1,2);assert(!npm_find(&g_rc_gpu,address)->kind);
        rc_gpu_samples(address,(const vis_list*)&snapshot);assert(npm_find(&g_rc_gpu,address)->version!=operand.version);
        sbw_note(address+7,1,SBW_GPU);assert(!npm_find(&g_rc_gpu,address)->kind);
        rc_gpu_samples(address,(const vis_list*)&snapshot);
        rc_gpu_write(0,0);assert(!npm_find(&g_rc_gpu,address)->kind);
        rc_gpu_samples(address,(const vis_list*)&snapshot);
        snapshot.s[0].buf=(__bridge void*)seed;rc_gpu_samples(address,(const vis_list*)&snapshot);assert(!npm_find(&g_rc_gpu,address)->kind);
        snapshot.s[0].buf=(__bridge void*)g_vis;rc_gpu_samples(address,(const vis_list*)&snapshot);
        npm_record(&g_rc_gpu,address+8,1,0,0,7);
        mtl_sub_counter(address,0x10);assert(npm_find(&g_rc_gpu,address)->kind==1&&npm_find(&g_rc_gpu,address)->constant==0);
        assert(!npm_find(&g_rc_gpu,address+8)->kind);
        npm_record(&g_rc_gpu,address+8,1,0,0,7);
        rc_gpu_samples(address,(const vis_list*)&snapshot);assert(!npm_find(&g_rc_gpu,address+8)->kind);checks+=2;
        rc_gpu_samples(address,(const vis_list*)&snapshot);operand=*npm_find(&g_rc_gpu,address);assert(operand.kind==2);
        NvProg *writer=[NvProg new];NvShader *shader=[NvShader new];writer->sh[1]=shader;
        shader->rf.nb=1;shader->rf.b[0].kind=NVN_RB_SBUFP;shader->rf.b[0].reg=0;shader->rf.b[0].rw=1;
        S.ssbo[1][0].addr=address+64;S.ssbo[1][0].size=8;g_pass_swrite=1;swrite_note(writer);assert(npm_find(&g_rc_gpu,address)->kind==2);
        S.ssbo[1][0].addr=address;shader->sb_ro=1;swrite_note(writer);assert(npm_find(&g_rc_gpu,address)->kind==2);
        shader->sb_ro=0;swrite_note(writer);assert(!npm_find(&g_rc_gpu,address)->kind);
        rc_gpu_samples(address,(const vis_list*)&snapshot);shader->rf.b[0].kind=NVN_RB_GMEM;swrite_note(writer);assert(!npm_find(&g_rc_gpu,address)->kind);
        rc_gpu_samples(address,(const vis_list*)&snapshot);operand=*npm_find(&g_rc_gpu,address);checks+=4;
        if(iteration&2){uint32_t previous=g_seg;g_cut_copy=1;sub_cut();assert(g_seg==previous+1);assert(g_vis_ring[g_vis_ring_i].fence==UINT64_MAX);}
        checks+=12;
        // The comparison word is GPU-written after its CPU value was five.
        // Resolving it must preserve the GPU read, not bake in that CPU value.
        *(uint64_t*)guest=5;*(uint64_t*)((uint8_t*)seed.contents+64)=0;
        void *guest_buffer;uint64_t guest_offset,guest_gpu;
        assert(mtl_gpu_addr((uint64_t)guest,8,&guest_buffer,&guest_offset,&guest_gpu));
        enc_b_copy_buffer(blit(),seed,64,(__bridge id<MTLBuffer>)guest_buffer,guest_offset,8);
        if(g_blit){enc_b_end(g_blit);g_blit=nil;}
        nvn_predicate_operand ordinary;
        assert(rc_gpu_operand((uint64_t)guest,&ordinary));assert(ordinary.kind==2&&ordinary.count==1);
        assert(!rc_gpu_operand((uint64_t)guest+1,&ordinary));
        assert(!rc_gpu_operand(address+8,&ordinary)); // Report timestamp field.
        rc_gpu_cpu_write((uint64_t)guest+31,4);
        assert(!rc_gpu_operand((uint64_t)guest+24,&ordinary));
        assert(!rc_gpu_operand((uint64_t)guest+32,&ordinary));
        rc_gpu_cpu_write((uint64_t)guest+1025,128);
        assert(!rc_gpu_operand((uint64_t)guest+1024,&ordinary));
        assert(!rc_gpu_operand((uint64_t)guest+1152,&ordinary));
        assert(rc_gpu_operand((uint64_t)guest+1160,&ordinary));
        assert(rc_gpu_operand((uint64_t)guest,&ordinary));checks+=10;
        *(job*)jobs.contents=(job){operand.slots,ordinary.slots,gpu.gpuAddress,out.gpuAddress,0,0,operand.count,ordinary.count,(uint32_t)mode,5};
        int integrated=format&&(iteration&4);
        if(integrated){
            uint64_t condition=(uint64_t)guest+8192;rc_gpu_samples(condition,(const vis_list*)&snapshot);
            *(uint64_t*)((uint8_t*)guest+8208)=0;
            NvProg *readonly=[NvProg new];g_rc_address=condition;g_rc_mode=(uint32_t)mode;
            g_exec_cur=(const uint8_t*)guest+16384;g_exec_next=NULL;
            p_draw_idx direct={.prim=4,.count=3,.instances=1,.itype=2,.ibuf=(uint64_t)guest+4096,.base_vertex=-7,.base_instance=29};
            static prog_t fixture_program={.serial=996001};serial_put(fixture_program.serial,readonly);
            S.prog=bind_stages(&fixture_program,3);
            uint8_t *commands=(uint8_t*)guest+16384;size_t used=0;
            #define RC_EMIT(OP,X) do{size_t n=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)n};memcpy(commands+used,&h,8);memcpy(commands+used+8,&(X),sizeof(X));used+=n;}while(0)
            RC_EMIT(OP_DRAW_IDX,direct);g_exec_next=commands+used;
            p_render_cond inverse={.addr=condition,.mode=(uint32_t)!mode};RC_EMIT(OP_RENDER_COND,inverse);
            uint64_t unrelated_report[2]={NVN_COUNTER_TYPE_SAMPLES_PASSED,condition+64};RC_EMIT(OP_REPORT_COUNTER,unrelated_report);
            const uint8_t *second=commands+used;RC_EMIT(OP_DRAW_IDX,direct);
            uint64_t stop[2]={NVN_COUNTER_TYPE_SAMPLES_PASSED,condition};RC_EMIT(OP_REPORT_COUNTER,stop);
            const uint8_t *third=commands+used;RC_EMIT(OP_DRAW_IDX,direct);
            op_hdr end={OP_END,8};memcpy(commands+used,&end,8);
            #undef RC_EMIT
            uint64_t masked=rc_mask_prepare(readonly,NULL,&direct);assert(masked);
            assert(g_rc_masks_n==2);assert(rc_mask_find(second,condition,(uint32_t)!mode));assert(!rc_mask_find(third,condition,(uint32_t)!mode));
            assert(S.prog==&fixture_program);g_exec_next=NULL;checks+=4;
            assert(rc_mask_find(g_exec_cur,condition,(uint32_t)mode)==masked);
            assert(!rc_mask_find(g_exec_cur,condition,(uint32_t)!mode));
            assert(masked>=g_cur_arena->gpu&&masked+20<=g_cur_arena->gpu+g_cur_arena->len);
            enc_b_copy_buffer(blit(),g_cur_arena->buf,masked-g_cur_arena->gpu,out,0,20);
            uint64_t inverted=rc_mask_find(second,condition,(uint32_t)!mode);
            assert(inverted>=g_cur_arena->gpu&&inverted+20<=g_cur_arena->gpu+g_cur_arena->len);
            enc_b_copy_buffer(blit(),g_cur_arena->buf,inverted-g_cur_arena->gpu,out,32,20);
            if(g_blit){enc_b_end(g_blit);g_blit=nil;}
            rc_gpu_write((uint64_t)guest+5000,4);assert(rc_mask_find(g_exec_cur,condition,(uint32_t)mode)==masked);
            rc_gpu_write(condition+23,1);assert(!rc_mask_find(g_exec_cur,condition,(uint32_t)mode));checks++;
            assert(rc_mask_prepare(readonly,NULL,&direct));
            rc_gpu_shader_write(writer);assert(!rc_mask_find(g_exec_cur,condition,(uint32_t)mode));checks+=2;
            g_exec_cur=NULL;g_rc_mode=2;checks+=5;
        }else{
            id<MTL4ComputeCommandEncoder> ce=enc_c_begin(nil);[ce setArgumentTable:at];enc_c_pso(ce,format?psdraw:ps);
            enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(ce);
        }
        if(format){
            state_reset();memset(&R,0,sizeof R);R.has_rt=R.has_sc=1;R.sc=(p_scissor){0,0,16,16};
            R.rt.n=1;R.rt.color[0]=&consumer;R.clear_c[0]=1;R.cc[0]=MTLClearColorMake(0,0,0,1);
            id<MTL4RenderCommandEncoder> draw=rp_begin_ex(0);assert(draw);
            enc_r_viewport(draw,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(draw,(MTLScissorRect){0,0,16,16});enc_r_cull(draw,MTLCullModeNone);
            enc_r_pso(draw,rp[1]);enc_r_draw_indexed_indirect(draw,MTLPrimitiveTypeTriangle,MTLIndexTypeUInt32,indices.gpuAddress+(integrated?28:0),indices.length-(integrated?28:0),out.gpuAddress);rp_end();
            enc_b_copy_texture_buffer(blit(),tex_mtl(&consumer),0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),pixels,0,64,1024);
        }
        mtl_sub_commit();mtl_drain();assert(!mtl_last_commit_error());
        uint32_t *actual=out.contents;assert(actual[0]==((mode?visible:!visible)?(format?3:13):0));assert(actual[4]==29);checks+=2;
        if(integrated){assert(actual[8]==((mode?visible:!visible)?0:3));assert(actual[9]==1&&actual[10]==0&&actual[11]==0xfffffff9u&&actual[12]==29);checks+=2;}
        if(format){assert(actual[1]==1 && actual[2]==(integrated?0:7) && actual[3]==0xfffffff9u);checks++;
            uint32_t expected=(mode?visible:!visible)?0xff0000ffu:0xff000000u;
            for(int i=0;i<256;i++){assert(((uint32_t*)pixels.contents)[i]==expected);checks++;}}
    }
    mtl_pool_del(guest);mtl_drain();free(guest);
    mtl_tex_release(&target);mtl_tex_release(&consumer);res_remove(indices);res_remove(pixels);
    for(id b in @[seed,gpu,pointers,jobs,out])res_remove(b);
    printf("GPU conditional mesh primitive: %d checks PASS; GPU-private operands, 64-bit sum/compare, both modes, zero grid and first task\n",checks);
}return 0;}
