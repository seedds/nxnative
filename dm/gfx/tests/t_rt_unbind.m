// Empty target bindings are state, not work. Exercise real replay/pass stores and
// pixel readback, plus the conservative attachment proof for uniform handles.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
typedef struct { uint8_t p[8192]; size_t n; } chain;
static void emit(chain *c, unsigned op, const void *a, size_t n) {
    size_t size = (8 + n + 7) & ~7ull; if (c->n + size > sizeof c->p) abort();
    op_hdr h = {op, (uint32_t)size}; memcpy(c->p + c->n, &h, 8);
    if (n) memcpy(c->p + c->n + 8, a, n); c->n += size;
}
static tex_t texture(uint64_t serial) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .off=(int64_t)serial<<20};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=8, .height=8, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static void triangle(chain *c) { p_draw d = {.prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .count=3, .instances=1}; emit(c, OP_DRAW, &d, sizeof d); }
static void run(chain *c) {
    emit(c, OP_END, NULL, 0); state_reset(); memset(&R, 0, sizeof R);
    mtl_sub_begin(); mtl_exec(c->p); mtl_sub_commit(); mtl_drain();
    CHECK(!mtl_last_commit_error(), "submission has no GPU error");
    CHECK(!g_unb, "submission leaves no deferred unbind");
}
static void pixels(tex_t *t, int red, int green, const char *label) {
    id<MTLTexture> mt = tex_mtl(t);
    id<MTLBuffer> b = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared];
    CHECK(mt && b, "readback resources allocate"); if (!mt || !b) return;
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> en = [cb blitCommandEncoder];
    [en copyFromTexture:mt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:2048];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int ok = cb.status == MTLCommandBufferStatusCompleted;
    for (int y=0; y<8; y++) for (int x=0; x<8; x++) { const uint8_t *p=(const uint8_t *)b.contents+y*256+x*4; ok &= p[0]==red && p[1]==green && p[2]==0 && p[3]==255; }
    CHECK(ok, label);
}
static void identity(tex_t *t) {
    p_set_rt a = {.n=1}, b; a.color[0]=t; b=a;
    b.n=8; b.has_cview[0]=1; b.cview[0]=(view_t){.base_level=7, .min_layer=9, .format=123, ._rsv=0xffff};
    CHECK(rt_same(&a,&b), "inactive view fields and trailing empty slots do not change the binding");
    b.cview[0].set_levels=1; b.cview[0].base_level=1;
    CHECK(!rt_same(&a,&b), "a different selected mip ends the pass");
    b=a; b.has_cview[0]=a.has_cview[0]=1;
    a.cview[0]=(view_t){.set_layers=1, .min_layer=0, .nlayers=1}; b.cview[0]=a.cview[0]; b.cview[0].min_layer=1;
    CHECK(!rt_same(&a,&b), "a different selected layer ends the pass");
    b=a; b.cview[0].set_format=1; b.cview[0].format=NVN_FORMAT_RGBA8_SRGB;
    CHECK(!rt_same(&a,&b), "a different selected format ends the pass");
}
static void replay(prog_t *pg, prog_t *green_pg, tex_t *t, tex_t *copy) {
    void *mem=NULL; if (posix_memalign(&mem,getpagesize(),getpagesize())) abort(); memset(mem,0,getpagesize()); mtl_pool_add(mem,getpagesize());
    p_set_rt rt={.n=1}, empty={0}; rt.color[0]=t;
    p_bind_program bind={pg,3}; p_clear_color green={0,0xf,{0,1,0,1}};
    // Warm the demanded pipeline before asserting the number of actual passes.
    chain warm={0}; emit(&warm,OP_SET_RT,&rt,sizeof rt); emit(&warm,OP_BIND_PROGRAM,&bind,sizeof bind); triangle(&warm); run(&warm);
    for (int kind=0; kind<8; kind++) {
        chain c={0}; emit(&c,OP_SET_RT,&rt,sizeof rt); emit(&c,OP_BIND_PROGRAM,&bind,sizeof bind); triangle(&c);
        emit(&c,OP_SET_RT,&empty,sizeof empty);
        // Repeated empty binds and sub-list returns may carry state to the rebind.
        if (kind==0) { chain sub={0}; emit(&sub,OP_SET_RT,&empty,sizeof empty); emit(&sub,OP_END,NULL,0); uint64_t ptr=(uint64_t)(uintptr_t)sub.p;
            emit(&c,OP_CALL,&ptr,8); emit(&c,OP_SET_RT,&rt,sizeof rt); triangle(&c);
            uint64_t before=g_stat_pass; run(&c);
            CHECK(g_stat_pass-before==(rtc_on() && unb_on() ? 1 : 2), "state-only unbind/rebind continues exactly one pass");
            pixels(t,255,0,"continued pass stores all red pixels"); continue; }
        if (kind==1) { uint32_t bits=~0u; emit(&c,OP_BARRIER,&bits,4); }
        if (kind==2) { p_copy_t2t cp={.src=t,.dst=copy,.sr={0,0,0,8,8,1},.dr={0,0,0,8,8,1}}; emit(&c,OP_COPY_T2T,&cp,sizeof cp); }
        if (kind==3) { run(&c); CHECK(!R.rt.n && !R.rt.depth,"end of submission applies empty binding"); pixels(t,255,0,"unbind at end stores old attachment"); continue; }
        if (kind==4) { p_set_rt other={.n=1}; other.color[0]=copy; emit(&c,OP_SET_RT,&other,sizeof other); emit(&c,OP_CLEAR_COLOR,&green,sizeof green); run(&c);
            pixels(t,255,0,"different target preserves the old pass"); pixels(copy,0,255,"different target receives its own clear"); continue; }
        if (kind==5) { p_bind_program gb={green_pg,3}; emit(&c,OP_BIND_PROGRAM,&gb,sizeof gb); triangle(&c); run(&c);
            CHECK(!R.rt.n && !R.rt.depth,"draw itself applies the empty binding"); pixels(t,255,0,"unbound green draw cannot change the previous red target"); continue; }
        if (kind==7) { uint64_t update[3]={(uint64_t)(uintptr_t)mem,8,73}; emit(&c,OP_UPDATE_UB,update,sizeof update); }
        emit(&c,OP_SET_RT,&rt,sizeof rt);
        if (kind==6) emit(&c,OP_CLEAR_COLOR,&green,sizeof green); else triangle(&c);
        uint64_t before=g_stat_pass; run(&c);
        CHECK(g_stat_pass-before>=2,"barrier/copy/clear or unbound draw prevents accidental continuation");
        pixels(t,kind==6?0:255,kind==6?255:0,"ordered clear/draw has the expected pixels");
        if (kind==2) pixels(copy,255,0,"copy after an empty binding sees the stored render target");
        if (kind==7) CHECK(*(uint64_t *)mem==73,"inline update after empty binding reaches memory in order");
    }
    mtl_pool_del(mem); free(mem);
}
static void replay_handles(prog_t *red_pg, prog_t *green_pg, id<MTLLibrary> lib, NvShader *vs, tex_t *t, tex_t *source) {
    void *mem=NULL; if (posix_memalign(&mem,getpagesize(),getpagesize())) abort(); memset(mem,0,getpagesize()); mtl_pool_add(mem,getpagesize());
    *(uint32_t *)mem=602; mtl_reg_tex(602,source,NULL);
    prog_t pg={.serial=882006}; NvProg *pr=[NvProg new]; pr->sh[0]=vs; pr->sh[1]=[NvShader new]; NvShader *fs=pr->sh[1];
    fs->stage=1; fs->fn=[lib newFunctionWithName:@"read_tex"]; strcpy(fs->key,"rt-unbind-handles");
    fs->ae=[fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len=fs->ae.encodedLength;
    fs->rf.bindless=1; fs->rf.nb=1; fs->rf.color_known=1; fs->rf.color_mask=1;
    fs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEXU,.reg=2,.aid=0,.dtt=MTLTextureType2D}; serial_put(pg.serial,pr);
    p_set_rt rt={.n=1}; rt.color[0]=source; p_bind_program bind={green_pg,3};
    chain init={0}; emit(&init,OP_SET_RT,&rt,sizeof rt); emit(&init,OP_BIND_PROGRAM,&bind,sizeof bind); triangle(&init); run(&init);
    rt.color[0]=t; bind.prog=&pg;
    p_bind_buf ub={.stage=1,.index=2,.addr=(uint64_t)(uintptr_t)mem,.size=16};
    chain warm={0}; emit(&warm,OP_SET_RT,&rt,sizeof rt); emit(&warm,OP_BIND_PROGRAM,&bind,sizeof bind); emit(&warm,OP_BIND_UBO,&ub,sizeof ub); triangle(&warm); run(&warm);
    // No empty bind: disabling unbind deferral must leave this handle proof and same-target continuation enabled.
    chain c={0}; bind.prog=red_pg; emit(&c,OP_SET_RT,&rt,sizeof rt); emit(&c,OP_BIND_PROGRAM,&bind,sizeof bind); triangle(&c);
    bind.prog=&pg; emit(&c,OP_SET_RT,&rt,sizeof rt); emit(&c,OP_BIND_PROGRAM,&bind,sizeof bind); emit(&c,OP_BIND_UBO,&ub,sizeof ub); triangle(&c);
    uint64_t before=g_stat_pass; run(&c);
    CHECK(g_stat_pass-before==(rtc_on() ? 1 : 2),"nonattachment uniform handle continues the same-target pass independently of unbind deferral");
    pixels(t,0,255,"continued draw reads the green nonattachment texture through its uniform handle");
    mtl_pool_del(mem); free(mem);
}
static void handles(tex_t *t) {
    void *mem=NULL; if (posix_memalign(&mem,getpagesize(),getpagesize())) abort(); memset(mem,0,getpagesize()); mtl_pool_add(mem,getpagesize());
    state_reset(); memset(&R,0,sizeof R); R.rt.n=1; R.rt.color[0]=t;
    NvProg *pr=[NvProg new]; pr->sh[1]=[NvShader new]; NvShader *fs=pr->sh[1]; fs->stage=1; fs->rf.bindless=1; fs->rf.nb=1;
    fs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEXU,.reg=2};
    S.ubo[1][2]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)mem,.size=16};
    tex_t independent=texture(882002), alias=texture(882003); alias.mem=(uint8_t *)mem+128; alias.L.size=128; t->mem=mem; t->L.size=256;
    mtl_reg_tex(601,t,NULL); mtl_reg_tex(602,&independent,NULL); mtl_reg_tex(603,&alias,NULL);
    g_samples_fb_left=1;
    *(uint32_t *)mem=602;
    CHECK(!draw_samples_rt(pr),"resolved nonattachment fragment handle permits continuation");
    *(uint32_t *)mem=601;
    CHECK(draw_samples_rt(pr),"uniform handle naming colour attachment splits");
    *(uint32_t *)mem=603;
    CHECK(draw_samples_rt(pr),"uniform handle to another serial with overlapping storage splits");
    *(uint32_t *)mem=699;
    CHECK(draw_samples_rt(pr),"unregistered uniform texture handle splits");
    int unknown[]={NVN_RB_TEXA,NVN_RB_TEXH,NVN_RB_TEXP,NVN_RB_IMGU,NVN_RB_TBUFU,NVN_RB_TEXD};
    for (unsigned i=0;i<sizeof unknown/sizeof *unknown;i++) { fs->rf.b[0].kind=unknown[i]; CHECK(draw_samples_rt(pr),"unsupported handle/table binding stays conservative"); }
    fs->rf.b[0].kind=NVN_RB_TEXU; fs->rf.pool=1; CHECK(draw_samples_rt(pr),"pool reflection stays conservative"); fs->rf.pool=0;
    fs->rf.dyn=1; CHECK(draw_samples_rt(pr),"dynamic reflection stays conservative"); fs->rf.dyn=0;
    *(uint32_t *)mem=602; fs->rf.nb=2;
    int buffers[]={NVN_RB_GMEM,NVN_RB_SBUFP,NVN_RB_SBUFU,NVN_RB_SBUFQ};
    for (unsigned i=0;i<sizeof buffers/sizeof *buffers;i++) { fs->rf.b[1]=(nvn_rbind){.kind=buffers[i]}; CHECK(draw_samples_rt(pr),"mixed bindless/raw buffer reads keep the pass-end storage mirror boundary"); }
    fs->rf.nb=1;
    R.rt.depth=&alias; R.rt.n=0; *(uint32_t *)mem=601;
    CHECK(draw_samples_rt(pr),"uniform handle to overlapping depth storage splits");
    g_samples_fb_left=0; CHECK(draw_samples_rt(pr),"slice merge retains the conservative handle guard");
    t->mem=NULL; mtl_pool_del(mem); free(mem);
}
int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout,NULL,_IONBF,0); mtl_init();
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 v(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\nfragment float4 red() { return float4(1,0,0,1); }\nfragment float4 green() { return float4(0,1,0,1); }\nstruct A { texture2d<float,access::read> source [[id(0)]]; };\nfragment float4 read_tex(float4 p [[position]], constant A &a [[buffer(0)]]) { return a.source.read(uint2(p.xy)); }\n" options:nil error:&error];
    if (!lib) { fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 2; }
    prog_t pg={.serial=882000}; NvProg *pr=[NvProg new]; pr->sh[0]=[NvShader new]; pr->sh[1]=[NvShader new];
    pr->sh[0]->fn=[lib newFunctionWithName:@"v"]; pr->sh[0]->stage=0;
    pr->sh[1]->fn=[lib newFunctionWithName:@"red"]; pr->sh[1]->stage=1; pr->sh[1]->rf.color_known=1; pr->sh[1]->rf.color_mask=1; serial_put(pg.serial,pr);
    prog_t green_pg={.serial=882005}; NvProg *gp=[NvProg new]; gp->sh[0]=pr->sh[0]; gp->sh[1]=[NvShader new];
    gp->sh[1]->fn=[lib newFunctionWithName:@"green"]; gp->sh[1]->stage=1; gp->sh[1]->rf.color_known=1; gp->sh[1]->rf.color_mask=1; serial_put(green_pg.serial,gp);
    tex_t t=texture(882001), cp=texture(882004); identity(&t); replay(&pg,&green_pg,&t,&cp);
    replay_handles(&pg,&green_pg,lib,pr->sh[0],&t,&cp); handles(&t);
    mtl_tex_release(&t); mtl_tex_release(&cp);
    printf("render target unbind: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
