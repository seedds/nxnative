// Synthetic GPU test; no game executable, replay or game shader compilation.
// Exercise the production stencil view, equal-size depth/stencil copy, float-bit
// object IDs, 252-byte fragment counter writes and a snapshot preceding a reset.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks,failed;
#define CHECK(c,label) do { checks++; if(!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
static void run(unsigned w,unsigned h,int format,unsigned number,prog_t *pg,prog_t *readonly) {
    mtl_set_frame(number+1); mtl_sub_begin(); state_reset(); memset(&R,0,sizeof R);
    tex_t src={.magic=MAGIC('T','E','X',0),.serial=989000+number*3,.off=(uint64_t)(number*3+1)<<26};
    src.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=format,.width=w,.height=h,.depth=1,.levels=1,.samples=1};
    nvn_tex_layout(&src.d,&src.L); mtl_tex_init(&src);
    tex_t dst=src,color=src; dst.serial++; dst.off+=1ull<<26; color.serial+=2; color.off+=2ull<<26;
    color.d.format=NVN_FORMAT_RGBA8; nvn_tex_layout(&color.d,&color.L); mtl_tex_init(&dst); mtl_tex_init(&color);
    id<MTLTexture> source=tex_mtl(&src),destination=tex_mtl(&dst),output=tex_mtl(&color);
    CHECK(source && destination && output,"fixture textures allocate");if(!source || !destination || !output)exit(1);
    R.has_rt=1;R.rt.depth=&src;R.clear_d=R.clear_s=1;R.cd=.75;R.cs=42;rp_flush();
    // Every possible eight-bit ID, including bit 7 and the out-of-range ID 63.
    NSUInteger row=(w+255)&~255ul;
    id<MTLBuffer> pattern=[g_dev newBufferWithLength:row*h options:MTLResourceStorageModeShared];
    CHECK(pattern!=nil,"fixture stencil pattern allocates");if(!pattern)exit(1);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)((uint8_t *)pattern.contents)[y*row+x]=(uint8_t)x;
    res_add(pattern);
    enc_b_copy_buffer_texture_opt(blit(),pattern,0,row,row*h,MTLSizeMake(w,h,1),source,0,0,MTLOriginMake(0,0,0),MTLBlitOptionStencilFromDepthStencil);
    // Replay the same loaded-but-unwritten depth/stencil store-elision pattern
    // seen immediately before the live scan copy, at the actual large sizes.
    state_reset();memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&color;R.rt.depth=&src;
    S.prog=readonly;S.ds.dt=1;S.ds.dw=S.ds.st=0;S.ds.df=NVN_DEPTH_FUNC_LESS;
    uint64_t saved=g_udc_saved_bytes;
    p_draw read={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1};draw(&read,NULL);rp_end();
    CHECK(g_udc_saved_bytes-saved==(uint64_t)w*h*5,"production read-only depth/stencil pass elides both stores");
    p_copy_t2t copy={.src=&src,.dst=&dst,.sr={.width=(int)w,.height=(int)h,.depth=1},.dr={.width=(int)w,.height=(int)h,.depth=1}};
    CHECK(copy_t2t_enc(&copy)!=nil,"production equal-size depth/stencil copy encodes");
    view_t v={.set_dsmode=1,.dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL,.set_swizzle=1};
    for(int i=0;i<4;i++)v.swz[i]=NVN_TEXTURE_SWIZZLE_R;
    mtl_reg_tex(975,&dst,&v);
    void *mem=NULL; if(posix_memalign(&mem,getpagesize(),getpagesize()))abort();
    memset(mem,0xa5,getpagesize()); mtl_pool_add(mem,getpagesize());
    uint32_t *counter=(uint32_t *)((uint8_t *)mem+64);memset(counter,0,252);
    state_reset();memset(&R,0,sizeof R); R.has_rt=1;R.rt.n=1;R.rt.color[0]=&color;
    S.prog=pg;S.tex[1][2]=975;S.ssbo[1][0]=(p_bind_buf){.addr=(uint64_t)counter,.size=252};
    S.ds.dt=S.ds.dw=S.ds.st=0;
    p_draw d={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1};draw(&d,NULL);rp_end();
    void *mb;uint64_t off;int mapped=mtl_addr((uint64_t)counter,252,&mb,&off);CHECK(mapped,"counter uses the guest memory pool");if(!mapped)exit(1);
    id<MTLBuffer> snapshot=kd_gpu_buffer((__bridge id<MTLBuffer>)mb,off,252);
    NSUInteger sr=0,dr=0;
    id<MTLBuffer> stencil=kd_gpu_plane(destination,0,0,1,&sr),depth=kd_gpu_plane(destination,0,0,0,&dr);
    CHECK(snapshot && stencil && depth,"bounded GPU snapshots allocate");if(!snapshot || !stencil || !depth)exit(1);
    // A reset later in the same submission must not erase the diagnostic snapshot.
    mtl_sub_fill((uint64_t)counter,252,0);
    ((uint32_t *)mem)[0]=0x12345678; // independent CPU data sharing the mapped page
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"fence waits for fragment writes and readback");mtl_drain();
    CHECK(!mtl_last_commit_error(),"no GPU submission error");
    uint32_t expected[63]={0};uint64_t pixels=0;int planes_ok=1;
    for(unsigned y=0;y<h;y++) {
        const uint8_t *s=(const uint8_t *)stencil.contents+y*sr;
        const float *z=(const float *)((const uint8_t *)depth.contents+y*dr);
        for(unsigned x=0;x<w;x++) { unsigned id=(uint8_t)x,index=((id&126)>>1)-1u;
            planes_ok &= s[x]==id && z[x]==.75f;
            if(!(id&128) && index<63) { expected[index]++;pixels++; }
        }
    }
    CHECK(planes_ok,"every copied stencil and depth texel matches the independent pattern");
    CHECK(!memcmp(snapshot.contents,expected,252),"all 63 float-bit ID counters equal independent pixel totals");
    int zero=1;for(int i=0;i<63;i++)zero &= counter[i]==0;
    CHECK(zero,"later GPU reset reaches the live shared counters");
    int guard=((uint32_t *)mem)[0]==0x12345678;
    for(unsigned i=4;i<(unsigned)getpagesize();i++)if(i<64||i>=316)guard &= ((uint8_t *)mem)[i]==0xa5;
    CHECK(guard,"GPU counter writes preserve unrelated live CPU memory on the same page");
    CHECK(!kd_gpu_buffer((__bridge id<MTLBuffer>)mb,off,4097),"oversized storage snapshot rejected");
    CHECK(!kd_gpu_buffer((__bridge id<MTLBuffer>)mb,off,0),"empty storage snapshot rejected");
    CHECK(!kd_gpu_buffer((__bridge id<MTLBuffer>)mb,off+1,4),"unaligned storage snapshot offset rejected");
    CHECK(!kd_gpu_buffer((__bridge id<MTLBuffer>)mb,off,3),"unaligned storage snapshot length rejected");
    CHECK(!kd_gpu_buffer((__bridge id<MTLBuffer>)mb,((__bridge id<MTLBuffer>)mb).length-4,8),"storage snapshot beyond backing allocation rejected");
    CHECK(!kd_gpu_plane(output,0,0,1,&sr),"a color texture is not mistaken for stencil storage");
    printf("KEY_READBACK %ux%u NVN=%#x counted=%llu: %s\n",w,h,format,(unsigned long long)pixels,planes_ok?"planes PASS":"planes FAIL");
    res_remove(pattern);mtl_pool_del(mem);free(mem);
    mtl_tex_release(&src);mtl_tex_release(&dst);mtl_tex_release(&color);
}
int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE","off",1);setenv("DM_PSO_MANIFEST","off",1);setenv("DM_PSO_USER","/dev/null",1);
    setenv("DM_PSO_PREBUILD","0",1);setenv("DM_PSO_HYBRID","0",1);setenv("DM_NVN_PREDICT","0",1);
    setenv("DM_NVN_UNWRITTEN_DC","1",1);setenv("DM_NVN_KEYDUMP","test-counter:6:0",1);if(!getenv("DM_NVN_KEYREADBACK"))setenv("DM_NVN_KEYREADBACK","1",1);
    mtl_init();NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A{device atomic_uint* count[[id(0)]];texture2d<float,access::read> stencil[[id(1)]];};\n"
        "vertex float4 v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],0,1); }\n"
        "fragment float4 plain(){return float4(0,1,0,1); }\n"
        "fragment float4 f(constant A&a[[buffer(0)]],float4 p[[position]]){uint id=as_type<uint>(a.stencil.read(uint2(p.xy)).y);uint i=((id&126)>>1)-1u;if(!(id&128)&&i<63&&!simd_is_helper_thread())atomic_fetch_add_explicit(a.count+i,1u,memory_order_relaxed);return float4(1,0,0,1); }\n" options:nil error:&error];
    CHECK(lib!=nil,"independent test shaders compile");if(!lib){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 1;}
    prog_t pg={.serial=988999};NvProg *pr=[NvProg new];pr->sh[0]=[NvShader new];pr->sh[1]=[NvShader new];
    pr->sh[0]->fn=[lib newFunctionWithName:@"v"];pr->sh[1]->fn=[lib newFunctionWithName:@"f"];pr->sh[1]->stage=1;
    strcpy(pr->sh[1]->key,"test-counter");pr->sh[1]->ae=[pr->sh[1]->fn newArgumentEncoderWithBufferIndex:0];
    pr->sh[1]->rf.nb=2;pr->sh[1]->rf.color_mask=1;
    pr->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.aid=0,.rw=1};
    pr->sh[1]->rf.b[1]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=2,.aid=1,.dtt=MTLTextureType2D,.dpf=NVN_DPF_FLOAT};
    serial_put(pg.serial,pr);
    prog_t ro={.serial=988998};NvProg *plain=[NvProg new];plain->sh[0]=pr->sh[0];plain->sh[1]=[NvShader new];
    plain->sh[1]->fn=[lib newFunctionWithName:@"plain"];plain->sh[1]->stage=1;plain->sh[1]->rf.color_mask=1;
    serial_put(ro.serial,plain);
    unsigned widths[]={1280,2560,3200},heights[]={720,1440,1800};
    for(unsigned f=0;f<2;f++)for(unsigned i=0;i<3;i++)run(widths[i],heights[i],f?NVN_FORMAT_DEPTH32F_STENCIL8:NVN_FORMAT_DEPTH24_STENCIL8,f*3+i,&pg,&ro);
    printf("KEY_READBACK %d checks, %d failed\n",checks,failed);return failed!=0;
} }
