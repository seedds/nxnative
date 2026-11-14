// A cached compute-pipeline failure must leave an unrelated render pass open.
// The companion t_dispatch_replacement covers a refused original whose NDVE
// replacement is eligible; this test covers the ordinary no-replacement path.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c,label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init();
    NvShader *shader=[NvShader new];shader->cps_failed=1;
    snprintf(shader->key,sizeof shader->key,"refused_no_host_replacement");
    shader->rf.tg[0]=shader->rf.tg[1]=shader->rf.tg[2]=1;
    NvProg *program=[NvProg new];program->sh[5]=shader;
    prog_t handle={.serial=997711};serial_put(handle.serial,program);
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=997712,.off=997712ll<<20};
    target.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=4,.height=4,.depth=1,.levels=1,.samples=1};
    nvn_tex_layout(&target.d,&target.L);
    mtl_sub_begin();state_reset();memset(&R,0,sizeof R);S.cprog=&handle;
    R.has_rt=1;R.rt.n=1;R.rt.color[0]=&target;
    R.clear_c[0]=1;R.cc[0]=(MTLClearColor){0,1,0,1};
    id<MTL4RenderCommandEncoder> opened=rp_begin_ex(1);
    CHECK(opened!=nil,"a real render pass is open before the refused dispatches");
    uint64_t passes=g_stat_pass,built=g_bp_pso_n;
    p_dispatch d={.x=1,.y=1,.z=1};
    for(int i=0;i<4;i++)dispatch(&d);
    CHECK(g_enc==opened,"known-refused dispatches preserve the same open encoder");
    CHECK(g_stat_pass==passes,"known-refused dispatches add no render passes");
    CHECK(g_bp_pso_n==built,"known-refused dispatches never rebuild their pipeline");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"the preserved render pass completes without a GPU error");
    id<MTLTexture> image=tex_mtl(&target);
    id<MTLBuffer> pixels=[g_dev newBufferWithLength:4*256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:pixels.length];
    [blit endEncoding];[cb commit];[cb waitUntilCompleted];
    CHECK(cb.error==nil,"the preserved pass reads back without a GPU error");
    int green=1;for(int y=0;y<4;y++)for(int x=0;x<4;x++){
        const uint8_t *p=(const uint8_t *)pixels.contents+y*256+x*4;
        green &= p[0]==0 && p[1]==255 && p[2]==0 && p[3]==255;
    }
    CHECK(green,"the pass's clear remains visible in every output pixel");
    fprintf(stderr,"t_dispatch_refused: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}}
