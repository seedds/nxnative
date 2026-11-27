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
static id<MTLBuffer> read_pixels(tex_t *t, MTLBlitOption option) {
    id<MTLTexture> mt = tex_mtl(t);
    id<MTLBuffer> b = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared];
    CHECK(mt && b, "readback resources allocate"); if (!mt || !b) return nil;
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> en = [cb blitCommandEncoder];
    [en copyFromTexture:mt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:2048 options:option];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "GPU readback completes");
    return cb.status == MTLCommandBufferStatusCompleted ? b : nil;
}
static void pixels(tex_t *t, int red, int green, const char *label) {
    id<MTLBuffer> b = read_pixels(t, MTLBlitOptionNone); if (!b) return;
    int ok = 1;
    for (int y=0; y<8; y++) for (int x=0; x<8; x++) { const uint8_t *p=(const uint8_t *)b.contents+y*256+x*4; ok &= p[0]==red && p[1]==green && p[2]==0 && p[3]==255; }
    CHECK(ok, label);
}
static void plane_pixels(tex_t *t, MTLBlitOption option, float left, float right, const char *label) {
    id<MTLBuffer> b = read_pixels(t, option); if (!b) return;
    int ok = 1;
    for (int y = 0; y < 8; y++) {
        const uint8_t *row = (const uint8_t *)b.contents + y * 256;
        for (int x = 0; x < 8; x++) {
            float value = option == MTLBlitOptionStencilFromDepthStencil ? row[x] : ((const float *)row)[x];
            ok &= value == (x < 4 ? left : right);
        }
    }
    CHECK(ok, label);
}
static void program(chain *c, const prog_t *pg, unsigned stages) {
    p_bind_program bind = {.prog=pg, .stages=stages};
    emit(c, OP_BIND_PROGRAM, &bind, sizeof bind);
}
static void depth_stencil(chain *c, int depth, int stencil) {
    ds_st ds = {.dt=depth, .dw=depth, .df=NVN_DEPTH_FUNC_ALWAYS,
        .st=stencil, .sf_f=NVN_STENCIL_FUNC_ALWAYS, .sf_b=NVN_STENCIL_FUNC_ALWAYS,
        .fail_f=NVN_STENCIL_OP_KEEP, .zfail_f=NVN_STENCIL_OP_KEEP, .zpass_f=NVN_STENCIL_OP_REPLACE,
        .fail_b=NVN_STENCIL_OP_KEEP, .zfail_b=NVN_STENCIL_OP_KEEP, .zpass_b=NVN_STENCIL_OP_REPLACE};
    p_bind_state bind = {.kind=5};
    memcpy(bind.bytes, &ds, sizeof ds); emit(c, OP_BIND_STATE, &bind, sizeof bind);
    if (stencil) for (int k = 0; k < 3; k++) {
        p_stencil state = {.face=NVN_FACE_FRONT_AND_BACK, .which=k, .value_mask=255, .mask=255, .ref=23};
        emit(c, OP_STENCIL, &state, sizeof state);
    }
}
static void clear_depth_stencil(chain *c) {
    p_clear_ds clear = {.depth=.75f, .depth_mask=1, .stencil=7, .stencil_mask=255};
    emit(c, OP_CLEAR_DS, &clear, sizeof clear);
}
static void depth_only(const prog_t *pg, tex_t *depth, tex_t *other) {
    for (int changed = 0; changed < 2; changed++) {
        chain seed = {0}; p_set_rt rt = {.depth=depth};
        emit(&seed, OP_SET_RT, &rt, sizeof rt); clear_depth_stencil(&seed);
        rt.depth = other; emit(&seed, OP_SET_RT, &rt, sizeof rt); clear_depth_stencil(&seed);
        chain c = {0}; rt.depth = depth; emit(&c, OP_SET_RT, &rt, sizeof rt);
        program(&c, pg, 1); depth_stencil(&c, 1, 0);
        p_scissor left = {.w=4, .h=8}, right = {.x=4, .w=4, .h=8};
        emit(&c, OP_SCISSOR, &left, sizeof left); triangle(&c);
        if (changed) rt.depth = other;
        emit(&c, OP_SET_RT, &rt, sizeof rt);
        emit(&c, OP_SCISSOR, &right, sizeof right); triangle(&c);
        chain warm = c; run(&warm); run(&seed);
        uint64_t before = g_stat_pass; run(&c);
        CHECK(g_stat_pass - before == (changed || !rtc_on() ? 2 : 1),
              changed ? "changing the depth-only attachment splits the pass" : "identical depth-only bindings continue one pass");
        plane_pixels(depth, MTLBlitOptionDepthFromDepthStencil, 0, changed ? .75f : 0, "depth-only draws write the intended halves of the first target");
        plane_pixels(other, MTLBlitOptionDepthFromDepthStencil, .75f, changed ? 0 : .75f, "depth-only draws leave the other target alone until it is bound");
    }
}
static NvShader *read_shader(id<MTLLibrary> lib, NSString *entry, int stage, int kind, int dpf) {
    NvShader *sh = [NvShader new];
    sh->stage = stage; sh->fn = [lib newFunctionWithName:entry];
    snprintf(sh->key, sizeof sh->key, "rt-%s", entry.UTF8String);
    sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len = sh->ae.encodedLength;
    sh->rf.bindless = kind == NVN_RB_TEXU; sh->rf.nb = 1;
    sh->rf.color_known = 1; sh->rf.color_mask = 1;
    sh->rf.b[0] = (nvn_rbind){.kind=kind, .reg=2, .dtt=MTLTextureType2D, .dpf=dpf};
    return sh;
}
static void feedback_draw(chain *c, const prog_t *pg, int depth, int stencil, int reads, const p_bind_buf *ub) {
    program(c, pg, 3); depth_stencil(c, depth, stencil);
    chmask_st mask = {.mask=reads ? 15 : 0}; p_bind_state bind = {.kind=2};
    memcpy(bind.bytes, &mask, sizeof mask); emit(c, OP_BIND_STATE, &bind, sizeof bind);
    if (ub) emit(c, OP_BIND_UBO, ub, sizeof *ub);
    triangle(c);
}
static uint64_t seeded_replay(const chain *seed, chain *draws) {
    chain init = *seed, warm = *draws;
    run(&init); run(&warm); init = *seed; run(&init);
    uint64_t before = g_stat_pass; run(draws);
    return g_stat_pass - before;
}
static void replay_planes(const prog_t *writer, id<MTLLibrary> lib, NvShader *vs, tex_t *color, tex_t *depth) {
    prog_t readers[] = {{.serial=882012}, {.serial=882013}};
    mtl_reg_tex(605, depth, NULL);
    view_t stencil = {.set_dsmode=1, .dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL};
    mtl_reg_tex(606, depth, &stencil);
    void *mem = NULL;
    if (posix_memalign(&mem, getpagesize(), getpagesize())) abort();
    memset(mem, 0, getpagesize()); mtl_pool_add(mem, getpagesize());
    for (int read_stencil = 0; read_stencil < 2; read_stencil++) {
        NvProg *pr = [NvProg new]; pr->sh[0] = vs;
        pr->sh[1] = read_shader(lib, read_stencil ? @"read_stencil" : @"read_depth", 1,
                               read_stencil ? NVN_RB_TEX : NVN_RB_TEXU, read_stencil ? NVN_DPF_UINT : NVN_DPF_DEPTH);
        serial_put(readers[read_stencil].serial, pr);
        *(uint32_t *)mem = 605;
        p_bind_buf ub = {.stage=1, .index=2, .addr=(uint64_t)(uintptr_t)mem, .size=16};
        p_bind_tex1 tex = {.stage=1, .slot=2, .h=606};
        p_set_rt rt = {.n=1, .depth=depth}; rt.color[0] = color;
        chain seed = {0}; emit(&seed, OP_SET_RT, &rt, sizeof rt); clear_depth_stencil(&seed);
        p_clear_color black = {.mask=15, .rgba={0,0,0,1}};
        emit(&seed, OP_CLEAR_COLOR, &black, sizeof black);
        const p_bind_buf *handle = read_stencil ? NULL : &ub;
        enum { READ_AFTER, READ_BEFORE, READ_WHILE_WRITING };
        for (int write_stencil = 0; write_stencil < 2; write_stencil++) for (int order = READ_AFTER; order <= READ_WHILE_WRITING; order++) {
            int overlap = read_stencil == write_stencil;
            if (overlap && order == READ_WHILE_WRITING) continue;   // existing depth feedback tests cover overlapping snapshots
            chain c = {0}; emit(&c, OP_SET_RT, &rt, sizeof rt);
            if (read_stencil) emit(&c, OP_BIND_TEX1, &tex, sizeof tex);
            if (order != READ_BEFORE) feedback_draw(&c, writer, !write_stencil, write_stencil, 0, NULL);
            feedback_draw(&c, &readers[read_stencil], order == READ_WHILE_WRITING && !write_stencil,
                          order == READ_WHILE_WRITING && write_stencil, 1, handle);
            if (order == READ_BEFORE) feedback_draw(&c, writer, !write_stencil, write_stencil, 0, NULL);
            CHECK(seeded_replay(&seed, &c) == (overlap ? 2 : 1),
                  overlap ? "sampling and writing the same plane splits in either order" : "sampling and writing different planes stays in one pass");
            int reads_after_writer = order != READ_BEFORE && overlap;
            int expected = read_stencil ? (reads_after_writer ? 23 : 7) : (reads_after_writer ? 0 : 191);
            pixels(color, expected, 255, "fragment reads the expected depth or stencil value");
            plane_pixels(depth, MTLBlitOptionDepthFromDepthStencil, write_stencil ? .75f : 0, write_stencil ? .75f : 0,
                         "depth plane contains only the intended writes");
            plane_pixels(depth, MTLBlitOptionStencilFromDepthStencil, write_stencil ? 23 : 7, write_stencil ? 23 : 7,
                         "stencil plane contains only the intended writes");
        }
        // Cover a waiting load-action clear, one already applied when a draw opened
        // the pass, and a masked stencil clear emitted as a triangle.
        for (int open = 0; open <= !read_stencil; open++) {
            chain c = {0}; emit(&c, OP_SET_RT, &rt, sizeof rt);
            p_clear_ds clear = {.depth=.5f, .depth_mask=!read_stencil, .stencil=11, .stencil_mask=read_stencil ? 15 : 0};
            emit(&c, OP_CLEAR_DS, &clear, sizeof clear);
            if (open) feedback_draw(&c, writer, 0, 1, 0, NULL);
            if (read_stencil) emit(&c, OP_BIND_TEX1, &tex, sizeof tex);
            feedback_draw(&c, &readers[read_stencil], 0, 0, 1, handle);
            CHECK(seeded_replay(&seed, &c) == 2, "sampling a cleared plane waits for its store");
            pixels(color, read_stencil ? 11 : 128, 255, "readback sees the new clear on the sampled plane");
        }
    }
    mtl_pool_del(mem); free(mem);
}
static void replay_vertex_handles(const prog_t *writer, id<MTLLibrary> lib, tex_t *color, tex_t *depth, tex_t *other) {
    prog_t reader = {.serial=882014};
    NvProg *pr = [NvProg new];
    pr->sh[0] = read_shader(lib, @"vertex_depth", 0, NVN_RB_TEXU, NVN_DPF_DEPTH);
    pr->sh[1] = [NvShader new]; pr->sh[1]->stage = 1;
    pr->sh[1]->fn = [lib newFunctionWithName:@"vertex_value"];
    serial_put(reader.serial, pr);
    mtl_reg_tex(605, depth, NULL); mtl_reg_tex(607, other, NULL);
    void *mem = NULL;
    if (posix_memalign(&mem, getpagesize(), getpagesize())) abort();
    memset(mem, 0, getpagesize()); mtl_pool_add(mem, getpagesize());
    uint32_t *words = mem;
    for (int attached = 0; attached < 2; attached++) {
        // The fragment slot contains the opposite handle, so resolving the
        // right slot on the wrong stage gives the opposite pass decision.
        words[0] = attached ? 605 : 607; words[4] = attached ? 607 : 605;
        p_bind_buf ub = {.stage=0, .index=2, .addr=(uint64_t)(uintptr_t)mem, .size=16};
        p_bind_buf fragment = {.stage=1, .index=2, .addr=ub.addr+16, .size=16};
        chain seed = {0}; p_set_rt rt = {.depth=other};
        emit(&seed, OP_SET_RT, &rt, sizeof rt); clear_depth_stencil(&seed); run(&seed);
        chain c = {0}; rt.depth = depth; rt.n = 1; rt.color[0] = color;
        emit(&c, OP_SET_RT, &rt, sizeof rt); clear_depth_stencil(&c);
        program(&c, writer, 3); depth_stencil(&c, 1, 0); triangle(&c);
        emit(&c, OP_SET_RT, &rt, sizeof rt); emit(&c, OP_BIND_UBO, &fragment, sizeof fragment);
        feedback_draw(&c, &reader, 0, 0, 1, &ub);
        chain warm = c; run(&warm);
        uint64_t before = g_stat_pass; run(&c);
        CHECK(g_stat_pass - before == (attached || !rtc_on() ? 2 : 1),
              attached ? "vertex handle naming the depth attachment still splits" : "disjoint vertex uniform handle continues the pass");
        pixels(color, attached ? 0 : 191, 255, "vertex shader reads the intended uniform texture");
    }
    mtl_pool_del(mem); free(mem);
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
static void leading_subset(prog_t *red_pg, prog_t *green_pg, id<MTLLibrary> lib, NvShader *vs) {
    // Leading-target continuation (DM_NVN_RT_SUBSET): 2 targets -> leading 1 target -> back to 2 targets.
    prog_t two_pg={.serial=882020}, read_pg={.serial=882021};
    NvProg *two=[NvProg new]; two->sh[0]=vs; two->sh[1]=[NvShader new];
    two->sh[1]->stage=1; two->sh[1]->fn=[lib newFunctionWithName:@"two_rg"]; strcpy(two->sh[1]->key,"rt-subset-two");
    two->sh[1]->rf.color_known=1; two->sh[1]->rf.color_mask=3; serial_put(two_pg.serial,two);
    NvProg *rd=[NvProg new]; rd->sh[0]=vs; rd->sh[1]=[NvShader new];
    rd->sh[1]->stage=1; rd->sh[1]->fn=[lib newFunctionWithName:@"read_tex"]; strcpy(rd->sh[1]->key,"rt-subset-read");
    rd->sh[1]->ae=[rd->sh[1]->fn newArgumentEncoderWithBufferIndex:0]; rd->sh[1]->arg_len=rd->sh[1]->ae.encodedLength;
    rd->sh[1]->rf.nb=1; rd->sh[1]->rf.color_known=1; rd->sh[1]->rf.color_mask=1;
    rd->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0,.aid=0,.dtt=MTLTextureType2D}; serial_put(read_pg.serial,rd);

    tex_t c0=texture(882022), c1=texture(882023), small=texture(882024);
    small.d.width=4; small.d.height=4; nvn_tex_layout(&small.d,&small.L);
    mtl_reg_tex(610,&c1,NULL);
    p_set_rt full={.n=2,.color={&c0,&c1}}, sub={.n=1,.color={&c0}}, shrunk={.n=2,.color={&c0,&small}};
    int cont = rtc_on() && rtsub_on();

    // 1. 2 targets (red, green) -> 1 target (green into c0 only) -> back to 2 targets (red into c0 only): 1 pass, c1 untouched (green)
    {
        chain c={0};
        emit(&c,OP_SET_RT,&full,sizeof full); program(&c,&two_pg,3); triangle(&c);
        emit(&c,OP_SET_RT,&sub,sizeof sub); program(&c,green_pg,3); triangle(&c);
        emit(&c,OP_SET_RT,&full,sizeof full); program(&c,red_pg,3); triangle(&c);
        chain warm=c; run(&warm);
        uint64_t before=g_stat_pass; run(&c);
        CHECK(g_stat_pass-before==(cont?1u:3u),"leading-target subset and return to full targets continue one pass");
        pixels(&c0,255,0,"leading-target continuation writes c0 cleanly");
        pixels(&c1,0,255,"leading-target continuation leaves trailing target c1 untouched");
    }
    // 2. Subset draw whose fragment shader writes past the subset (color_mask=3) must split so c1 keeps its earlier red value
    {
        chain c={0};
        emit(&c,OP_SET_RT,&full,sizeof full); program(&c,green_pg,3);
        p_clear_color clr={.index=1,.rgba={1,0,0,1},.mask=15}; emit(&c,OP_CLEAR_COLOR,&clr,sizeof clr); triangle(&c);
        emit(&c,OP_SET_RT,&sub,sizeof sub); program(&c,&two_pg,3); triangle(&c);
        chain warm=c; run(&warm);
        uint64_t before=g_stat_pass; run(&c);
        CHECK(g_stat_pass-before==2u,"subset draw writing past the game's targets splits the pass");
        pixels(&c0,255,0,"split subset draw still writes c0");
        pixels(&c1,255,0,"split subset draw does not overwrite trailing target c1");
    }
    // 3. Subset draw that reads trailing target c1 must split the pass and read c1's stored pixels
    {
        p_bind_handles th={.stage=1,.first=0,.n=1,.h={610}};
        chain c={0};
        emit(&c,OP_SET_RT,&full,sizeof full); program(&c,&two_pg,3); triangle(&c);   // c0=red, c1=green
        emit(&c,OP_SET_RT,&sub,sizeof sub); program(&c,&read_pg,3); emit(&c,OP_BIND_TEX,&th,sizeof th); triangle(&c);
        chain warm=c; run(&warm);
        uint64_t before=g_stat_pass; run(&c);
        CHECK(g_stat_pass-before==2u,"subset draw reading a trailing target splits the pass");
        pixels(&c0,0,255,"subset draw reads stored trailing target c1 cleanly");
    }
    // 4. Trailing target smaller than leading target shrinks pass bounds: rt_prefix must refuse so c0 is not clipped to 4x4
    {
        chain c={0};
        p_clear_color clr0={.index=0,.rgba={0,0,0,1},.mask=15};
        emit(&c,OP_SET_RT,&sub,sizeof sub); emit(&c,OP_CLEAR_COLOR,&clr0,sizeof clr0);
        emit(&c,OP_SET_RT,&shrunk,sizeof shrunk); program(&c,&two_pg,3); triangle(&c);
        emit(&c,OP_SET_RT,&sub,sizeof sub); program(&c,green_pg,3); triangle(&c);
        chain warm=c; run(&warm);
        uint64_t before=g_stat_pass; run(&c);
        CHECK(g_stat_pass-before==2u,"smaller trailing target refuses leading-subset continuation");
        pixels(&c0,0,255,"non-continued full-size subset draw covers all 8x8 pixels");
    }
    // 5. Clear arriving while in a leading-subset continuation and returning to full targets applies the clear and keeps c1
    {
        chain c={0};
        p_clear_color clr0={.index=0,.rgba={0,1,0,1},.mask=15};
        emit(&c,OP_SET_RT,&full,sizeof full); program(&c,&two_pg,3); triangle(&c);   // c0=red, c1=green
        emit(&c,OP_SET_RT,&sub,sizeof sub); emit(&c,OP_CLEAR_COLOR,&clr0,sizeof clr0);
        emit(&c,OP_SET_RT,&full,sizeof full);
        chain warm=c; run(&warm); run(&c);
        pixels(&c0,0,255,"clear during leading-subset continuation lands on c0");
        pixels(&c1,0,255,"clear during leading-subset continuation leaves trailing target c1 intact");
    }
    mtl_tex_release(&c0); mtl_tex_release(&c1); mtl_tex_release(&small);
}
int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout,NULL,_IONBF,0); mtl_init();
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "float4 position(uint i) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
        "vertex float4 v(uint i [[vertex_id]]) { return position(i); }\n"
        "fragment float4 red() { return float4(1,0,0,1); }\n"
        "fragment float4 green() { return float4(0,1,0,1); }\n"
        "struct Two { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\n"
        "fragment Two two_rg() { return {float4(1,0,0,1), float4(0,1,0,1)}; }\n"
        "struct A { texture2d<float,access::read> source [[id(0)]]; };\n"
        "fragment float4 read_tex(float4 p [[position]], constant A &a [[buffer(0)]]) { return a.source.read(uint2(p.xy)); }\n"
        "struct D { depth2d<float,access::read> source [[id(0)]]; };\n"
        "struct T { texture2d<uint,access::read> source [[id(0)]]; };\n"
        "fragment float4 read_depth(float4 p [[position]], constant D &a [[buffer(0)]]) { return float4(a.source.read(uint2(p.xy)),1,0,1); }\n"
        "fragment float4 read_stencil(float4 p [[position]], constant T &a [[buffer(0)]]) { return float4(float(a.source.read(uint2(p.xy)).r)/255,1,0,1); }\n"
        "struct V { float4 p [[position]]; float value; };\n"
        "vertex V vertex_depth(uint i [[vertex_id]], constant D &a [[buffer(0)]]) { return {position(i), a.source.read(uint2(0))}; }\n"
        "fragment float4 vertex_value(V in [[stage_in]]) { return float4(in.value,1,0,1); }\n" options:nil error:&error];
    if (!lib) { fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 2; }
    prog_t pg={.serial=882000}; NvProg *pr=[NvProg new]; pr->sh[0]=[NvShader new]; pr->sh[1]=[NvShader new];
    pr->sh[0]->fn=[lib newFunctionWithName:@"v"]; pr->sh[0]->stage=0;
    pr->sh[1]->fn=[lib newFunctionWithName:@"red"]; pr->sh[1]->stage=1; pr->sh[1]->rf.color_known=1; pr->sh[1]->rf.color_mask=1; serial_put(pg.serial,pr);
    prog_t green_pg={.serial=882005}; NvProg *gp=[NvProg new]; gp->sh[0]=pr->sh[0]; gp->sh[1]=[NvShader new];
    gp->sh[1]->fn=[lib newFunctionWithName:@"green"]; gp->sh[1]->stage=1; gp->sh[1]->rf.color_known=1; gp->sh[1]->rf.color_mask=1; serial_put(green_pg.serial,gp);
    tex_t t=texture(882001), cp=texture(882004); identity(&t); replay(&pg,&green_pg,&t,&cp);
    tex_t depth=texture(882010), other=texture(882011);
    depth.d.format=other.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
    nvn_tex_layout(&depth.d, &depth.L); nvn_tex_layout(&other.d, &other.L);
    depth_only(&pg, &depth, &other);
    replay_planes(&pg, lib, pr->sh[0], &t, &depth);
    replay_vertex_handles(&pg, lib, &t, &depth, &other);
    replay_handles(&pg,&green_pg,lib,pr->sh[0],&t,&cp); handles(&t);
    leading_subset(&pg,&green_pg,lib,pr->sh[0]);
    mtl_tex_release(&t); mtl_tex_release(&cp);
    mtl_tex_release(&depth); mtl_tex_release(&other);
    printf("render target unbind: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
