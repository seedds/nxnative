// Full command replay: visibility query -> conditional direct/ indexed draws -> pixels.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void *p){(void)p;return NULL;}
module_t *g_mod;int g_nmod;
void svc_thread_dump(void){}
uint64_t now_ns(void){return bp_now();}
int main(void){@autoreleasepool{
    setenv("DM_NVN_GPU_PREDICATE","1",1);setenv("DM_NVN_TBDR","0",1);setenv("DM_PSO_ARCHIVE","off",1);setenv("DM_PSO_PREBUILD","0",1);
    mtl_init();NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[position]];};\n"
      "vertex V visible(uint i [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return V{float4(p[i],0,1)};}\n"
      "vertex V hidden(uint i [[vertex_id]]){return V{float4(4+float(i),4,0,1)};}\n"
      "fragment float4 colour(){return float4(1,0,0,1);}\n";
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:source options:nil error:&error];assert(lib);
    prog_t program[2]={{.serial=997001},{.serial=997002}};
    NSMutableArray *keep=[NSMutableArray array];
    for(int i=0;i<2;i++){
        NvProg *pr=[NvProg new];pr->sh[0]=[NvShader new];pr->sh[1]=[NvShader new];
        pr->sh[0]->stage=0;pr->sh[1]->stage=1;
        pr->sh[0]->fn=fn4_note([lib newFunctionWithName:i?@"visible":@"hidden"],lib,i?@"visible":@"hidden",nil);
        pr->sh[1]->fn=fn4_note([lib newFunctionWithName:@"colour"],lib,@"colour",nil);
        snprintf(pr->sh[0]->key,sizeof pr->sh[0]->key,"%016x_b",i+201);
        snprintf(pr->sh[1]->key,sizeof pr->sh[1]->key,"%016x_b",203);
        serial_put(program[i].serial,pr);[keep addObject:pr];
    }
    uint8_t *memory=NULL;assert(!posix_memalign((void**)&memory,getpagesize(),262144));memset(memory,0,262144);mtl_pool_add(memory,262144);
    uint64_t query=(uint64_t)memory+220000;uint32_t *index=(uint32_t*)(memory+240000);index[0]=7;index[1]=8;index[2]=9;
    tex_t producer={.magic=MAGIC('T','E','X',0),.serial=997101,.off=(int64_t)997101<<20};
    producer.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=16,.height=16,.depth=1,.levels=1,.samples=1};
    tex_t consumer=producer;consumer.serial++;consumer.off+=1<<20;
    id<MTLBuffer> pixels=[g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];res_add(pixels);int checks=0;
    for(int format=0;format<2;format++)for(int visible=0;visible<2;visible++)for(int iteration=0;iteration<8;iteration++){
        memset((void*)query,0,256);size_t used=0;
        #define EMIT(OP,X) do{size_t n=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)n};memcpy(memory+used,&h,8);memcpy(memory+used+8,&(X),sizeof(X));used+=n;}while(0)
        p_set_rt rt={.n=1};rt.color[0]=&producer;EMIT(OP_SET_RT,rt);
        p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=16;EMIT(OP_VIEWPORTS,vp);
        p_scissor sc={0,0,16,16};EMIT(OP_SCISSOR,sc);
        p_clear_color clear={.index=0,.mask=15,.rgba={0,0,1,1}};EMIT(OP_CLEAR_COLOR,clear);
        p_bind_program bind={&program[visible],3};EMIT(OP_BIND_PROGRAM,bind);
        uint64_t type=NVN_COUNTER_TYPE_SAMPLES_PASSED;EMIT(OP_RESET_COUNTER,type);
        p_draw draw={.prim=4,.count=3,.instances=1};EMIT(OP_DRAW,draw);
        uint64_t original[2]={NVN_COUNTER_TYPE_SAMPLES_PASSED,query+128};EMIT(OP_REPORT_COUNTER,original);
        uint64_t report[2]={NVN_COUNTER_TYPE_SAMPLES_PASSED,query};EMIT(OP_REPORT_COUNTER,report);
        rt.color[0]=&consumer;EMIT(OP_SET_RT,rt);EMIT(OP_CLEAR_COLOR,clear);
        bind.prog=&program[1];EMIT(OP_BIND_PROGRAM,bind);
        p_draw_idx indexed={.prim=4,.count=3,.instances=1,.itype=2,.ibuf=(uint64_t)index,.base_vertex=-7,.base_instance=29};
        for(int mode=0;mode<2;mode++){
            p_render_cond condition={.addr=query,.mode=(uint32_t)mode};EMIT(OP_RENDER_COND,condition);
            sc=(p_scissor){0,mode*8,16,8};EMIT(OP_SCISSOR,sc);
            // Cross the mask batch boundary with unique commands and repeated calls.
            for(int repeat=0;repeat<((iteration&4)?RC_MASK_MAX+17:1);repeat++){
            if(iteration&2){
                uint8_t *sub=memory+200000;size_t n=(8+(format?sizeof indexed:sizeof draw)+7)&~7ull;
                op_hdr h={format?OP_DRAW_IDX:OP_DRAW,(uint32_t)n},end_sub={OP_END,8};memcpy(sub,&h,8);
                if(format)memcpy(sub+8,&indexed,sizeof indexed);else memcpy(sub+8,&draw,sizeof draw);
                memcpy(sub+n,&end_sub,8);uint64_t call=(uint64_t)sub;EMIT(OP_CALL,call);
            }else if(format){EMIT(OP_DRAW_IDX,indexed);}else{EMIT(OP_DRAW,draw);}
            }
            assert(used<200000);
            if(!mode){uint64_t overwrite[2]={0x10,query+((iteration&1)?0:64)};EMIT(OP_REPORT_COUNTER,overwrite);}
        }
        p_render_cond enabled={.mode=2};EMIT(OP_RENDER_COND,enabled);
        op_hdr end={OP_END,8};memcpy(memory+used,&end,8);
        #undef EMIT
        state_reset();memset(&R,0,sizeof R);mtl_sub_begin();uint64_t handle=(uint64_t)memory;mtl_sub_index(&handle,1);
        uint64_t batches=g_rc_mask_runs;mtl_exec(memory);rp_flush();
        enc_b_copy_texture_buffer(blit(),tex_mtl(&consumer),0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),pixels,0,64,1024);
        assert(mtl_wait(mtl_sub_commit(),5000000000ull));assert(!mtl_last_commit_error());
        assert(g_rc_mask_runs>batches);assert((*(uint64_t*)(query+128)!=0)==visible);assert((*(uint64_t*)query!=0)==((iteration&1)?0:visible));checks+=3;
        for(int y=0;y<16;y++)for(int x=0;x<16;x++){
            int red=y<8?!visible:((iteration&1)?0:visible);uint32_t wanted=red?0xff0000ffu:0xffff0000u;
            uint32_t actual=((uint32_t*)pixels.contents)[y*16+x];
            if(actual!=wanted)fprintf(stderr,"format%d visible%d iteration%d x%d y%d got%08x expected%08x\n",format,visible,iteration,x,y,actual,wanted);
            assert(actual==wanted);checks++;
        }
    }
    mtl_tex_release(&producer);mtl_tex_release(&consumer);res_remove(pixels);mtl_pool_del(memory);mtl_drain();free(memory);
    printf("GPU conditional full replay: %d checks PASS\n",checks);
}return 0;}
