// Deferred inline uniform updates (ubd_on) in TBDR mode (DM_NVN_TBDR=1): production mtl_exec on hand-written chains whose fragment shader returns a uniform buffer's
// first float4, closed-form expectations. Every draw must read the data NVN gives it in command order, whatever the TBDR reorderers do around the update:
// in-pass (draw, update, draw), same-target continuation (draw, update, SET_RT of the same targets, draw), a pass deferred into a later one (defer_try) with an update
// before it, and an update between the deferred pass and its target pass (the deferral must refuse it). The counters show what the host did: updates deferred, passes, deferrals.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w=(want), g=(got); count++; printf("%s expected %llu → got %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w==g?"✓":"✗"); fail += w != g; } while (0)
typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(void) { chain c={0}; if (posix_memalign((void **)&c.p,getpagesize(),65536)) abort(); memset(c.p,0,65536); mtl_pool_add(c.p,65536); return c; }
static void emit(chain *c, unsigned op, const void *a, size_t n) { size_t z=(8+n+7)&~7ull; if (c->n+z>65536) abort(); op_hdr h={op,(uint32_t)z}; memcpy(c->p+c->n,&h,8); if(n) memcpy(c->p+c->n+8,a,n); c->n+=z; }
static tex_t tex(unsigned w) { static uint64_t s=9500; tex_t t={.magic=MAGIC('T', 'E', 'X', 0), .serial=s, .off=(int64_t)(s-9500)<<20}; s++;   // own storage offset (g_alias)
    t.d.width=t.d.height=w; t.d.depth=t.d.samples=1; t.d.levels=2; t.d.target=NVN_TEXTURE_TARGET_2D; t.d.format=NVN_FORMAT_RGBA8; return t; }
static int view(const tex_t *t, int lv) { static int id=1500; view_t v={.set_levels=1,.base_level=lv,.nlevels=1}; mtl_reg_tex(id,t,&v); return id++; }
static void scissor(chain *c, int x, int w) { p_scissor sc={x,0,w,8}; emit(c,OP_SCISSOR,&sc,sizeof sc); }
static void set_rt(chain *c, const tex_t *a, const tex_t *b) { p_set_rt r={.n=b?2:1}; r.color[0]=a; r.color[1]=b;
    for (int i=0;i<r.n;i++) { r.has_cview[i]=1; r.cview[i].set_levels=1; r.cview[i].base_level=0; r.cview[i].nlevels=1; }
    emit(c,OP_SET_RT,&r,sizeof r); p_viewports v={.n=1}; v.v[0][2]=v.v[0][3]=8; emit(c,OP_VIEWPORTS,&v,sizeof v); scissor(c,0,8); }
static void clear_color(chain *c, int i) { p_clear_color x={.index=i,.mask=15,.rgba={0,0,0,1}}; emit(c,OP_CLEAR_COLOR,&x,sizeof x); }
static void bind_prog(chain *c, prog_t *p) { p_bind_program b={p,3}; emit(c,OP_BIND_PROGRAM,&b,sizeof b); }
static void bind_tex(chain *c, int id) { p_bind_handles b={.stage=1,.first=0,.n=1}; b.h[0]=id; emit(c,OP_BIND_TEX,&b,sizeof b); }
static void bind_ubo(chain *c, void *mem) { p_bind_buf ub={.stage=1,.index=0,.addr=(uint64_t)mem,.size=16}; emit(c,OP_BIND_UBO,&ub,sizeof ub); }
static void update(chain *c, void *mem, const float rgba[4]) { struct { uint64_t dst,n; float v[4]; } u={(uint64_t)mem,16,{0}}; memcpy(u.v,rgba,16); emit(c,OP_UPDATE_UB,&u,sizeof u); }
static void draw_cmd(chain *c) { p_draw d={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}; emit(c,OP_DRAW,&d,sizeof d); }
static void blend(chain *c) { color_st col={1,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS}; p_bind_state x={.kind=1}; memcpy(x.bytes,&col,sizeof col); emit(c,OP_BIND_STATE,&x,sizeof x);
    blend_st b={0,NVN_BLEND_FUNC_SRC_ALPHA,NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD}; x.kind=0; memcpy(x.bytes,&b,sizeof b); emit(c,OP_BIND_STATE,&x,sizeof x); }
static void ds_off(chain *c) { ds_st d={0,0,NVN_DEPTH_FUNC_ALWAYS,0,NVN_STENCIL_FUNC_ALWAYS,NVN_STENCIL_FUNC_ALWAYS,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP};
    p_bind_state x={.kind=5}; memcpy(x.bytes,&d,sizeof d); emit(c,OP_BIND_STATE,&x,sizeof x); }
static void readback(const tex_t *t, uint8_t out[256]) { id<MTLBuffer> b=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    uint64_t f=mtl_sub_commit(); EXPECT("  submit completed",1,mtl_wait(f,5000000000ull)); EXPECT("  no error in commit feedback",0,mtl_last_commit_error()); memcpy(out,b.contents,256); }
static NvProg *t_keep[8]; static prog_t t_pg[2];   // static: the pipeline cache is keyed by the prog_t address
static void setup(void) { NSError *e=nil; id<MTLLibrary> l=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
      "vertex float4 v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],0,1);}\n"
      "struct U{constant float4 *c [[id(0)]];}; fragment float4 ub(constant U&a [[buffer(0)]]){return float4(a.c->rgb,1);}\n"
      "struct A{texture2d<float> t [[id(0)]];}; fragment float4 sample(constant A&a [[buffer(0)]]){return a.t.read(uint2(0));}\n" options:nil error:&e];
    if (!l) { fprintf(stderr,"%s\n",e.localizedDescription.UTF8String); abort(); }
    for (int i=0;i<2;i++) { NvProg *p=[NvProg new]; p->sh[0]=[NvShader new]; p->sh[0]->fn=[l newFunctionWithName:@"v"]; NvShader *f=p->sh[1]=[NvShader new]; f->stage=1;
        f->fn=[l newFunctionWithName:i?@"sample":@"ub"]; f->rf.nb=1; f->rf.b[0]=(nvn_rbind){.kind=i?NVN_RB_TEX:NVN_RB_CBUF,.reg=0,.dtt=2}; f->ae=[f->fn newArgumentEncoderWithBufferIndex:0];
        t_keep[i]=p; t_pg[i].serial=9100+i; serial_put(t_pg[i].serial,p); } }
static const float RED[4]={1,0,0,1}, GREEN[4]={0,1,0,1}, BLUE[4]={0,0,1,1};
static void *ub_mem(const float init[4]) { void *m=NULL; if (posix_memalign(&m,getpagesize(),getpagesize())) abort(); memset(m,0,getpagesize()); memcpy(m,init,16); mtl_pool_add(m,getpagesize()); return m; }
typedef struct { uint64_t passes, deferred_ub, deferrals; } stats;
static stats run(chain *c, const tex_t *out, uint8_t px[256]) { emit(c,OP_END,NULL,0); state_reset(); memset(&R,0,sizeof R); vis_clear(); g_vis_on=g_vis_counting=0;
    stats s0={g_stat_pass,g_ubd_deferred,g_defer_n}; mtl_sub_begin(); uint64_t hh=(uint64_t)c->p; mtl_sub_index(&hh,1); mtl_exec(c->p); readback(out,px);
    return (stats){g_stat_pass-s0.passes,g_ubd_deferred-s0.deferred_ub,g_defer_n-s0.deferrals}; }
// Pixels whose column is below `split` must be `l`, the others `r` (RGBA8, alpha `al`)
static void halves(const char *name, const uint8_t px[256], int split, const float l[4], const float r[4], uint8_t al) { unsigned bad=0;
    for (int y=0;y<8;y++) for (int x=0;x<8;x++) { const float *w=x<split?l:r; const uint8_t *p=px+4*(8*y+x);
        bad += p[0]!=(uint8_t)(w[0]*255) || p[1]!=(uint8_t)(w[1]*255) || p[2]!=(uint8_t)(w[2]*255) || p[3]!=al; }
    EXPECT(name,0,bad); }
static void in_pass(void) { printf("CASE in_pass: draw (left), update, draw (right) in one pass\n");
    void *m=ub_mem(RED); tex_t a=tex(8); chain c=chain_new(); uint8_t px[256];
    set_rt(&c,&a,NULL); clear_color(&c,0); ds_off(&c); bind_prog(&c,&t_pg[0]); bind_ubo(&c,m); scissor(&c,0,4); draw_cmd(&c); update(&c,m,GREEN); scissor(&c,4,4); draw_cmd(&c);
    stats s=run(&c,&a,px); halves("  left red (before the update), right green (after it)",px,4,RED,GREEN,255);
    EXPECT("  the update is deferred, not ending the pass",1,s.deferred_ub); EXPECT("  one render pass",1,s.passes); mtl_pool_del(m); free(m); }
static void continuation(void) { printf("CASE continuation: draw, update, SET_RT of the same target, draw\n");
    void *m=ub_mem(RED); tex_t a=tex(8); chain c=chain_new(); uint8_t px[256];
    set_rt(&c,&a,NULL); clear_color(&c,0); ds_off(&c); bind_prog(&c,&t_pg[0]); bind_ubo(&c,m); scissor(&c,0,4); draw_cmd(&c); update(&c,m,GREEN);
    set_rt(&c,&a,NULL); scissor(&c,4,4); draw_cmd(&c); update(&c,m,BLUE);
    stats s=run(&c,&a,px); halves("  left red, right green (the later blue update reaches no draw)",px,4,RED,GREEN,255);
    EXPECT("  both updates deferred",2,s.deferred_ub); EXPECT("  the continuation stays one render pass",1,s.passes);
    EXPECT("  memory holds the last update once the submission is done",1,!memcmp(m,BLUE,16)); mtl_pool_del(m); free(m); }
// P draws A; a pass on another target; Q = (A, Q2) reads only A's level 1, so P is deferred into Q (t_defer_chain's positive case). `between`: an update between P and Q.
static void deferred(int between) { printf("CASE %s\n", between?"update between P and Q: the deferral must refuse it":"deferred pass: an update before P, P deferred into Q");
    void *m=ub_mem(RED); tex_t a=tex(8),b=tex(8),q2=tex(8); int a1=view(&a,1); chain c=chain_new(); uint8_t px[256];
    update(&c,m,GREEN);
    set_rt(&c,&a,NULL); clear_color(&c,0); ds_off(&c); bind_prog(&c,&t_pg[0]); bind_ubo(&c,m); blend(&c); draw_cmd(&c);
    set_rt(&c,&b,NULL); clear_color(&c,0); ds_off(&c); draw_cmd(&c);
    if (between) update(&c,m,BLUE);
    set_rt(&c,&a,&q2); bind_prog(&c,&t_pg[1]); bind_tex(&c,a1); draw_cmd(&c);
    setenv("DM_NVN_DEFER","1",1); stats s=run(&c,&a,px);
    halves("  A green: P reads the update made before it (alpha 0: Q's draw writes its black texel's alpha, blend ONE / ZERO)",px,8,GREEN,GREEN,0);
    EXPECT(between?"  not deferred":"  P deferred into Q",between?0:1,s.deferrals);
    EXPECT("  updates deferred",between?2:1,s.deferred_ub); mtl_pool_del(m); free(m); }
int main(void) { @autoreleasepool { setvbuf(stdout,NULL,_IONBF,0); setenv("DM_NVN_TBDR","1",1); mtl_init(); setup();
    in_pass(); continuation(); deferred(0); deferred(1);
    printf("== %d checks, %d failed ==\n",count,fail); return fail!=0; } }
