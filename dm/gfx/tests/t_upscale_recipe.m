// Procedural color and conservative fallback tests; no captured game assets.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c,s) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",s); } } while(0)
static id<MTLTexture> texture(MTLPixelFormat f,NSUInteger w,NSUInteger h,MTLTextureUsage u) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModeShared; d.usage=u;
    id<MTLTexture> t=[g_dev newTextureWithDescriptor:d]; if (!t) abort();res_add(t);return t;
}
static void finish(void) { CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU completion");CHECK(!mtl_last_commit_error(),"no GPU error"); }
static uint32_t first(id<MTLTexture> t) { uint32_t x=0; [t getBytes:&x bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];return x; }
static void setall(id<MTLTexture> t,uint32_t x) { size_t n=t.width*t.height;uint32_t *p=malloc(n*4);for(size_t i=0;i<n;i++)p[i]=x;[t replaceRegion:MTLRegionMake2D(0,0,t.width,t.height) mipmapLevel:0 withBytes:p bytesPerRow:t.width*4];free(p); }
// Exercise the actual replacement branch and its post-dispatch scene observer together.
// The backing installation follows t_feedback/t_image_attachment_store: real NVN
// descriptors and mapped guest pools, with a procedural Metal texture as their storage.
static void composed_dispatch(const up_recipe *profile) {
    const uint64_t frame=912345, source_serial=9912341, output_serial=9912342;
    size_t bytes=32u<<20; void *source_mem=NULL,*output_mem=NULL;
    CHECK(!posix_memalign(&source_mem,16384,bytes) && !posix_memalign(&output_mem,16384,bytes),"composed mapped pool allocations");
    if(!source_mem || !output_mem)abort();
    pool_t pools[2]={
        {.magic=MAGIC('P','O','O','L'),.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED,.mem=source_mem,.size=bytes},
        {.magic=MAGIC('P','O','O','L'),.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED,.mem=output_mem,.size=bytes}
    };
    mtl_pool_add(source_mem,bytes);mtl_pool_add(output_mem,bytes);
    tex_t src={.magic=MAGIC('T','E','X',0),.serial=source_serial,.pool=&pools[0],.mem=source_mem};
    tex_t dst={.magic=MAGIC('T','E','X',0),.serial=output_serial,.pool=&pools[1],.mem=output_mem};
    src.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGB9E5F,.width=profile->iw,.height=profile->ih,.depth=1,.levels=1,.samples=1};
    dst.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R11G11B10F,.flags=NVN_TEXTURE_FLAGS_IMAGE,.width=profile->ow,.height=profile->oh,.depth=1,.levels=1,.samples=1};
    nvn_tex_layout(&src.d,&src.L);nvn_tex_layout(&dst.d,&dst.L);
    CHECK(src.L.size && dst.L.size,"composed NVN layouts");
    CHECK(src.L.size<bytes && dst.L.size<bytes,"composed pool extents contain complete textures");
    id<MTLTexture> input=texture(MTLPixelFormatRGB9E5Float,profile->iw,profile->ih,MTLTextureUsageShaderRead);
    id<MTLTexture> output=texture(MTLPixelFormatRG11B10Float,profile->ow,profile->oh,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite);
    setall(input,(15u<<27)|256u|(128u<<9)|(384u<<18));setall(output,0);
    mtl_tex_init(&src);mtl_tex_init(&dst);serial_put(src.serial,input);serial_put(dst.serial,output);
    mtl_reg_tex(811,&src,NULL);mtl_reg_image(812,&dst,NULL);
    NvShader *shader=[NvShader new];strcpy(shader->key,profile->key);
    shader->rf.tg[0]=128;shader->rf.tg[1]=shader->rf.tg[2]=1;shader->rf.nb=2;
    shader->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.aid=profile->ti,.reg=0,.dtt=MTLTextureType2D,.dpf=NVN_DPF_FLOAT};
    shader->rf.b[1]=(nvn_rbind){.kind=NVN_RB_IMG,.aid=profile->oi,.reg=1,.rw=1,.dtt=MTLTextureType2D,.dpf=NVN_DPF_FLOAT};
    NvProg *program=[NvProg new];program->sh[5]=shader;prog_t native_program={.serial=9912343};serial_put(native_program.serial,program);
    S.cprog=&native_program;S.tex[5][0]=811;S.img[5][1]=812;
    uint32_t rect[8]={0,0,profile->iw-1,profile->ih-1,0,0,profile->ow-1,profile->oh-1};memcpy(S.push[1],rect,sizeof rect);
    atomic_store(&g_cur_frame,frame);g_temporal_depth_frame=frame;g_temporal_full_frame=UINT64_MAX;g_temporal_disabled=0;
    g_temporal_game_depth=temporal_texture(MTLPixelFormatDepth32Float,profile->iw,profile->ih,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    // State produced by the preceding native AA dependency chain. The final
    // dispatch must extend it rather than erase the intermediate scene serials.
    const uint64_t upstream[3]={9912300,9912301,source_serial};memcpy(g_temporal_flow,upstream,sizeof upstream);g_temporal_flow_n=3;
    uint64_t before=g_up_mfx_n,captured=g_temporal_coverage.captured;
    p_dispatch dp={.x=profile->gx,.y=profile->gy,.z=profile->gz};
    mtl_sub_begin();dispatch(&dp);finish();
    CHECK(g_up_mfx_n==before+1,"real dispatch intercepted exactly once");
    CHECK(g_temporal_full_frame==frame,"replacement dispatch notifies final-scene observer");
    CHECK(g_temporal_coverage.captured==captured+1,"observer captured exactly one pre-UI scene");
    CHECK(g_temporal_ui_serial==output_serial,"observer records the replacement output serial");
    CHECK(g_temporal_game_ui==output,"observer keeps the real replacement image backing");
    CHECK(g_temporal_flow_n==4 && !memcmp(g_temporal_flow,upstream,sizeof upstream) && g_temporal_flow[3]==output_serial,"native AA flow survives and includes final output");
    CHECK(g_temporal_full_scene && g_temporal_full_scene.width==profile->ow && g_temporal_full_scene.height==profile->oh,"full-size scene snapshot exists");
    if(g_temporal_full_frame==frame && g_temporal_full_scene) {
        id<MTLBuffer> rb=[g_dev newBufferWithLength:profile->ow*profile->oh*4 options:MTLResourceStorageModeShared];res_add(rb);
        mtl_sub_begin();enc_b_copy_texture_buffer(blit(),g_temporal_full_scene,0,0,MTLOriginMake(0,0,0),MTLSizeMake(profile->ow,profile->oh,1),rb,0,profile->ow*4,profile->ow*profile->oh*4);finish();
        uint32_t first=((uint32_t*)rb.contents)[0];
        CHECK(abs((int)(first&2047u)-(13<<6))<=1,"observer snapshot contains actual scaled procedural pixels");
    }
    S.cprog=NULL;mtl_reg_tex(811,NULL,NULL);mtl_reg_tex(812,NULL,NULL);mtl_tex_release(&src);mtl_tex_release(&dst);serial_del(native_program.serial);
    // The process owns these mapped pools until exit, as do the Metal no-copy
    // records; do not free their bytes while a retirement fence may still hold them.
}
int main(void) { @autoreleasepool {
    setenv("DM_OOT_INTERPOLATION_PREVIEW","3",1);
    mtl_init();const up_recipe *profile=up_recipe_profile();CHECK(profile!=NULL,"generated profile recipe");
    NvShader *cs=[NvShader new];strcpy(cs->key,profile->key);
    const char *opt=getenv("DM_UPSCALER_EXPERIMENTAL");int enabled=opt&&!strcmp(opt,"1");
    CHECK(up_recipe_named(cs)==enabled,"explicit experimental opt-in gate");
    CHECK(up_mfx_wanted(cs)==enabled,"MetalFX and experimental selection");
    strcpy(cs->key,"0000000000000000_b");CHECK(!up_recipe_named(cs),"other native AA shaders retained");
    if(!enabled) { printf("upscale recipe gate: %d checks, %d failed\n",checks,failed);return failed!=0; }
    up_recipe r={.iw=32,.ih=24,.ow=64,.oh=48};
    id<MTLTexture> in=texture(MTLPixelFormatRGB9E5Float,r.iw,r.ih,MTLTextureUsageShaderRead);
    // Exact RGB=[.5,.25,.75], shared exponent=15 -> linear=[.25,.0625,.5625].
    setall(in,(15u<<27)|256u|(128u<<9)|(384u<<18));
    id<MTLTexture> out=texture(MTLPixelFormatRG11B10Float,r.ow,r.oh,MTLTextureUsageShaderWrite);
    CHECK(!(out.usage&MTLTextureUsageRenderTarget),"output is image-only, no render-target upgrade");
    mtl_sub_begin();CHECK(up_recipe_draw(&r,in,out),"recipe encoded");finish();
    up_mfx_scaler *cached=up_mfx_scaler_for(r.iw,r.ih,r.ow,r.oh,2);CHECK(cached!=NULL,"scaler cached");
    CHECK(cached->sc.colorProcessingMode==MTLFXSpatialScalerColorProcessingModeLinear,"Linear mode");
    id<MTLBuffer> rb=[g_dev newBufferWithLength:r.iw*r.ih*8 options:MTLResourceStorageModeShared];res_add(rb);
    mtl_sub_begin();enc_b_copy_texture_buffer(blit(),cached->in,0,0,MTLOriginMake(0,0,0),MTLSizeMake(r.iw,r.ih,1),rb,0,r.iw*8,r.iw*r.ih*8);finish();
    __fp16 *v=rb.contents;int bad=0;for(unsigned i=0;i<r.iw*r.ih;i++)bad+=v[4*i]!=.25f||v[4*i+1]!=.0625f||v[4*i+2]!=.5625f||v[4*i+3]!=1.f;
    CHECK(!bad,"GPU square decode matches exact procedural RGB and alpha");
    uint32_t packed=first(out);CHECK(out.pixelFormat==MTLPixelFormatRG11B10Float,"original image format preserved");
    unsigned want[3]={13u<<6,11u<<6,(14u<<5)|4u},got[3]={packed&2047u,(packed>>11)&2047u,packed>>22};
    for(int k=0;k<3;k++)CHECK(abs((int)got[k]-(int)want[k])<=1,"constant color survives native format conversion");
    const uint32_t sentinel=0x12345678;id<MTLTexture> wrong=texture(MTLPixelFormatRG11B10Float,r.iw,r.ih,MTLTextureUsageShaderRead);
    for(int test=0;test<3;test++) {
        setall(out,sentinel);up_recipe shape=r; if(test==1)shape.ow++;
        mtl_sub_begin();int did=up_recipe_draw(&shape,test==0?wrong:test==2?out:in,out);CHECK(!did,"unsupported format, shape or alias refused");finish();CHECK(first(out)==sentinel,"refusal leaves output pixels unchanged");
    }
    // The real binding adapter must reject absent mapped image descriptors.
    strcpy(cs->key,profile->key);cs->rf.nb=2;cs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.aid=profile->ti,.reg=0};cs->rf.b[1]=(nvn_rbind){.kind=NVN_RB_IMG,.aid=profile->oi,.reg=1,.rw=1,.dtt=MTLTextureType2D};
    uint32_t rect[8]={0,0,profile->iw-1,profile->ih-1,0,0,profile->ow-1,profile->oh-1};memcpy(S.push[1],rect,sizeof rect);
    p_dispatch dp={.x=profile->gx,.y=profile->gy,.z=profile->gz};
    mtl_sub_begin();CHECK(!up_recipe_encode(cs,&dp),"unmapped image uses native fallback");finish();CHECK(first(out)==sentinel,"unmapped image fallback preserves existing output");
    dp.x--; mtl_sub_begin();CHECK(!up_recipe_encode(cs,&dp),"partial dispatch grid retained");finish();CHECK(first(out)==sentinel,"partial grid fallback leaves output unchanged");
    // Force eviction before deferred encoding drains: old scratch and scaler remain resident/alive through the fence.
    mtl_sub_begin();CHECK(up_recipe_draw(&r,in,out),"encode before cache eviction");
    for(int i=0;i<5;i++)CHECK(up_mfx_scaler_for(16+i*2,16,40+i*2,40,2)!=NULL,"cache shape churn");finish();
    packed=first(out);CHECK(abs((int)(packed&2047u)-(int)want[0])<=1,"recorded work survives cache eviction");
    composed_dispatch(profile);
    printf("upscale recipe GPU: %d checks, %d failed; deferred=%s\n",checks,failed,getenv("DM_NVN_ENC_THREAD")?:"default");return failed!=0;
} }
