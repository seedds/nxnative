// DM_NVN_DS_IDLE: a depth / stencil plane that no draw of its pass used in the pass's earlier occurrences is attached with load and store DontCare.
// Replays real op streams on an 8x8 RGBA8 + Depth32Float_Stencil8 target seeded with depth .75 / stencil 0x2d, reads all three planes back and checks:
//   0 a colour-only pass after one learning pass: both planes idle (320 bytes not loaded), planes unchanged, colour drawn
//   1 an idle opening draw, then a draw that tests and writes depth and stencil: the pass ends first (fallback), depth .5 / stencil 0x36 written, the key never goes idle again
//   2 an idle opening draw, then a scissored depth-stencil clear (clear_ds_tri): fallback, the left half cleared to .25 / 0x11, the right half unchanged
//   3 an opening draw that tests depth only: only the stencil plane is idle (64 bytes), depth still tested
//   4 switch off: no plane idle, no bookkeeping
// Every case is also replayed with the switch off on a fresh history and must match it byte for byte. Run under MTL_DEBUG_LAYER=1 (check_t_ds_idle).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t color = {.magic=MAGIC('T','E','X',0), .serial=99601};
static tex_t depth = {.magic=MAGIC('T','E','X',0), .serial=99602};
static prog_t program = {.serial=99603};
static void seed(void) {
    MTL4RenderPassDescriptor *p=[MTL4RenderPassDescriptor new];
    p.colorAttachments[0].texture=tex_mtl(&color); p.colorAttachments[0].loadAction=MTLLoadActionClear;
    p.colorAttachments[0].storeAction=MTLStoreActionStore; p.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    p.depthAttachment.texture=p.stencilAttachment.texture=tex_mtl(&depth);
    p.depthAttachment.loadAction=p.stencilAttachment.loadAction=MTLLoadActionClear;
    p.depthAttachment.storeAction=p.stencilAttachment.storeAction=MTLStoreActionStore;
    p.depthAttachment.clearDepth=.75; p.stencilAttachment.clearStencil=0x2d;
    enc_r_end(enc_r_begin(p));
}
static ds_st ds_off(void) { return (ds_st){.df=NVN_DEPTH_FUNC_ALWAYS,.sf_f=NVN_STENCIL_FUNC_ALWAYS,.sf_b=NVN_STENCIL_FUNC_ALWAYS,
    .fail_f=NVN_STENCIL_OP_KEEP,.zfail_f=NVN_STENCIL_OP_KEEP,.zpass_f=NVN_STENCIL_OP_KEEP,.fail_b=NVN_STENCIL_OP_KEEP,.zfail_b=NVN_STENCIL_OP_KEEP,.zpass_b=NVN_STENCIL_OP_KEEP}; }
typedef struct { uint8_t color[256]; float depth[64]; uint8_t stencil[64]; uint64_t idle_bytes, fallback; } result;
static result run(int sw, int kind) {
    static uint64_t frame; mtl_set_frame(++frame);
    g_dsi_m=sw;
    state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); seed();
    S.ds=ds_off(); if (kind==3) { S.ds.dt=1; S.ds.df=NVN_DEPTH_FUNC_LESS; }
    S.st_ref[0]=S.st_ref[1]=0x36; S.st_vmask[0]=S.st_vmask[1]=S.st_wmask[0]=S.st_wmask[1]=255; g_dss_dirty=1;
    uint8_t buf[512], *q=buf;
#define PUT(OP, T, ...) do { op_hdr h_={OP,sizeof(op_hdr)+sizeof(T)}; T v_=__VA_ARGS__; memcpy(q,&h_,sizeof h_); memcpy(q+sizeof h_,&v_,sizeof v_); q+=h_.size; } while (0)
    p_set_rt rt={.n=1,.depth=&depth}; rt.color[0]=&color;
    PUT(OP_SET_RT, p_set_rt, rt);
    PUT(OP_BIND_PROGRAM, p_bind_program, {&program,3});
    PUT(OP_DRAW, p_draw, {NVN_DRAW_PRIMITIVE_TRIANGLES,0,3});
    if (kind==1) { ds_st d=ds_off(); d.dt=1; d.dw=1; d.df=NVN_DEPTH_FUNC_LESS; d.st=1; d.zpass_f=d.zpass_b=NVN_STENCIL_OP_REPLACE;
        p_bind_state b={.kind=5}; memcpy(b.bytes,&d,sizeof d); PUT(OP_BIND_STATE, p_bind_state, b);
        PUT(OP_DRAW, p_draw, {NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}); }
    if (kind==2) { PUT(OP_SCISSOR, p_scissor, {0,0,4,8}); PUT(OP_CLEAR_DS, p_clear_ds, {.25f,1,0x11,0xff}); }
    { op_hdr e={OP_END,sizeof(op_hdr)}; memcpy(q,&e,sizeof e); }
#undef PUT
    uint64_t b0=g_dsi_bytes, f0=g_dsi_fallback;
    mtl_exec(buf); rp_end();
    id<MTLBuffer> b[3]; b[0]=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    b[1]=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; b[2]=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if(!b[0]||!b[1]||!b[2]) { fprintf(stderr,"readback buffers: out of memory\n"); abort(); }
    [blit() copyFromTexture:tex_mtl(&color) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b[0] destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b[1] destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256 options:MTLBlitOptionDepthFromDepthStencil];
    [blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b[2] destinationOffset:0 destinationBytesPerRow:8 destinationBytesPerImage:64 options:MTLBlitOptionStencilFromDepthStencil];
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU submission completes");
    CHECK(!mtl_last_commit_error(),"GPU submission has no error");
    result r={0}; memcpy(r.color,b[0].contents,256); memcpy(r.depth,b[1].contents,256); memcpy(r.stencil,b[2].contents,64);
    r.idle_bytes=g_dsi_bytes-b0; r.fallback=g_dsi_fallback-f0; return r;
}
static int same(const result *a, const result *b) { return !memcmp(a->color,b->color,256)&&!memcmp(a->depth,b->depth,256)&&!memcmp(a->stencil,b->stencil,64); }   // NOLINT(bugprone-suspicious-memory-comparison): planes read back from the GPU, compared bit for bit
static int pixels(const result *r, int kind) {
    int good=1; uint8_t red[4]={255,0,0,255};
    for (int i=0;i<64;i++) { int left=(i&7)<4;
        float d=kind==1?.5f:kind==2&&left?.25f:.75f; uint8_t s=kind==1?0x36:kind==2&&left?0x11:0x2d;
        good &= !memcmp(r->color+4*i,red,4) && r->depth[i]==d && r->stencil[i]==s; }
    return good;
}
int main(void) { @autoreleasepool {
    mtl_init();
    color.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=1,.levels=1,.samples=1};
    depth.d=color.d; depth.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],.5,1);}\nfragment float4 f(){return float4(1,0,0,1);}" options:nil error:&error];
    CHECK(lib!=nil,"literal test shaders compile"); if (!lib) return 1;
    NvProg *pr=[NvProg new]; pr->sh[0]=[NvShader new]; pr->sh[1]=[NvShader new];
    pr->sh[0]->fn=[lib newFunctionWithName:@"v"]; pr->sh[1]->fn=[lib newFunctionWithName:@"f"];
    pr->sh[1]->stage=1; pr->sh[1]->rf.color_mask=1; serial_put(program.serial,pr);
    static const char *name[]={"colour-only pass","later depth-stencil draw","later scissored depth-stencil clear","depth-tested opening draw","switch off"};
    for (int kind=0;kind<5;kind++) {
        memset(g_dsi_tab,0,sizeof g_dsi_tab); result off=run(0,kind==4?0:kind);
        memset(g_dsi_tab,0,sizeof g_dsi_tab); int sw=kind==4?0:1;
        result learn=run(sw,0), on=run(sw,kind==4?0:kind);
        CHECK(learn.idle_bytes==0&&learn.fallback==0,"an unseen pass loads its planes");
        CHECK(pixels(&off,kind==4?0:kind),"switch off: all 64 pixels match hand-computed colour / depth / stencil");
        CHECK(same(&off,&on),"idle planes match the loaded control byte for byte");
        uint64_t want_bytes=kind==0?320:kind==3?64:0; uint64_t want_fb=kind==1||kind==2;
        if (kind==1||kind==2) want_bytes=320;   // the opening draw went idle before the fallback
        CHECK(on.idle_bytes==want_bytes,"only planes no draw used are idle");
        CHECK(on.fallback==want_fb,"a later use of an idle plane ends the pass once");
        if (kind==1||kind==2) { result again=run(1,0); CHECK(again.idle_bytes==0&&again.fallback==0,"a key whose idle plane was needed loads it from then on"); CHECK(pixels(&again,0),"after a fallback the planes are loaded"); }
        printf("case %d (%s): idle %llu bytes, fallback %llu\n",kind,name[kind],(unsigned long long)on.idle_bytes,(unsigned long long)on.fallback);
    }
    printf("DS_IDLE %d checks, %d failed\n",checks,failed); return failed!=0;
} }

