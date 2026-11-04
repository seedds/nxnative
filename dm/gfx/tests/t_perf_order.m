// Default performance paths: real replay and Metal pixels, plus a gated CPU
// completion owner. No game shaders/files or frame timing is used as an oracle.
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

// Redirect only this translation unit, and only while the CPU budget cases run.
static int test_budget_active;
static _Atomic uint64_t *test_done;
static struct {
    uint64_t now, event_elapsed, event_ms, event_fence;
    int event_calls, event_success, cond_calls, cond_complete;
    int64_t cond_remaining;
} test_budget;
static uint64_t test_clock_gettime_nsec_np(clockid_t clock) {
    return test_budget_active ? test_budget.now : clock_gettime_nsec_np(clock);
}
static int test_clock_gettime(clockid_t clock, struct timespec *t) {
    if (!test_budget_active) return clock_gettime(clock, t);
    uint64_t ns = test_budget.now + (clock == CLOCK_REALTIME ? 1000000000000ull : 0);
    *t = (struct timespec){.tv_sec = ns / 1000000000ull, .tv_nsec = ns % 1000000000ull};
    return 0;
}
static int test_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (!test_budget_active) return pthread_cond_timedwait(cond, mutex, deadline);
    test_budget.cond_calls++;
    test_budget.cond_remaining = (int64_t)deadline->tv_sec * 1000000000ll + deadline->tv_nsec
        - (int64_t)(test_budget.now + 1000000000000ull);
    if (test_budget.cond_complete && test_budget.cond_remaining > 0) { *test_done = 1; return 0; }
    return ETIMEDOUT;
}
static pthread_mutex_t *test_trace_lock;
static uint64_t test_trace_delay;
static int test_pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (test_budget_active && mutex == test_trace_lock) test_budget.now += test_trace_delay;
    return pthread_mutex_lock(mutex);
}
@interface WaitBudgetEvent : NSObject
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds;
@end
@implementation WaitBudgetEvent
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds {
    test_budget.event_calls++;
    test_budget.event_fence = value;
    test_budget.event_ms = milliseconds;
    test_budget.now += test_budget.event_elapsed;
    return test_budget.event_success;
}
@end
#define clock_gettime_nsec_np test_clock_gettime_nsec_np
#define clock_gettime test_clock_gettime
#define pthread_mutex_lock test_pthread_mutex_lock
#define pthread_cond_timedwait test_pthread_cond_timedwait
#include "../nvn/nvn_mtl.m"
#undef pthread_mutex_lock
#undef pthread_cond_timedwait
#undef clock_gettime
#undef clock_gettime_nsec_np
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(s,c) do { int ok=!!(c); checks++; failed+=!ok; printf("%s: %s\n",s,ok?"PASS":"FAIL"); } while(0)
typedef struct { uint8_t p[16384]; size_t n; } chain;
static void emit(chain *c, unsigned op, const void *a, size_t n) {
    size_t sz=(8+n+7)&~7ull; if(c->n+sz>sizeof c->p) abort();
    op_hdr h={op,(uint32_t)sz}; memcpy(c->p+c->n,&h,8);
    if(n) memcpy(c->p+c->n+8,a,n); c->n+=sz;
}
static tex_t texture(int target, int format, int layers) {
    static uint64_t serial=800000;
    serial++;
    tex_t t={.magic=MAGIC('T','E','X',0),.serial=serial,.off=(int64_t)serial<<20};
    t.d=(nvn_texdesc){.target=target,.format=format,.width=8,.height=8,.depth=layers,.levels=1,.samples=1};
    nvn_tex_layout(&t.d,&t.L); return t;
}
static void rt(chain *c, const tex_t *t, int layer, const tex_t *ds) {
    p_set_rt r={.n=1,.depth=ds}; r.color[0]=t;
    if(layer>=0) { r.has_cview[0]=1; r.cview[0]=(view_t){.set_layers=1,.min_layer=layer,.nlayers=1}; }
    emit(c,OP_SET_RT,&r,sizeof r);
}
static void bind(chain *c, const prog_t *p, int stages) {
    p_bind_program b={p,stages}; emit(c,OP_BIND_PROGRAM,&b,sizeof b);
}
static void triangle(chain *c) {
    p_draw d={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1}; emit(c,OP_DRAW,&d,sizeof d);
}
static void reset(void) {
    state_reset(); memset(&R,0,sizeof R); g_csh_n=0;
}
static uint64_t execute(chain *c) {
    emit(c,OP_END,NULL,0); reset(); mtl_sub_begin(); mtl_exec(c->p);
    uint64_t f=mtl_sub_commit(); CHECK("submission completes",mtl_wait(f,2000000000ull));
    mtl_drain(); CHECK("no GPU error",!mtl_last_commit_error()); return f;
}
static NvProg *make_program(id<MTLLibrary> lib, prog_t *pg, const char *fragment) {
    static uint64_t serial=900000; pg->serial=++serial;
    NvProg *p=[NvProg new]; p->sh[0]=[NvShader new]; p->sh[1]=[NvShader new];
    p->sh[0]->fn=[lib newFunctionWithName:@"v"]; p->sh[0]->stage=0;
    p->sh[1]->fn=[lib newFunctionWithName:@(fragment)]; p->sh[1]->stage=1;
    p->sh[1]->rf.color_known=1; p->sh[1]->rf.color_mask=1;
    serial_put(pg->serial,p); return p;
}
static void slices(id<MTLLibrary> lib) {
    tex_t t=texture(NVN_TEXTURE_TARGET_2D_ARRAY,NVN_FORMAT_RGBA8,6);
    prog_t pg={0}; NvProg *p=make_program(lib,&pg,"red");
    chain c={0}; bind(&c,&pg,3);
    for(int sl=0;sl<6;sl++) { rt(&c,&t,sl,NULL); triangle(&c); }
    // Warm the demanded amplified pipeline before checking the actual merge.
    execute(&c);
    for(int i=0;i<g_sm_amp_n;i++) {
        for(int k=0;k<2000;k++) { os_unfair_lock_lock(&g_sm_amp_lock); int done=g_sm_amp[i].done; os_unfair_lock_unlock(&g_sm_amp_lock); if(done) break; usleep(1000); }
    }
    c.n-=8; uint64_t before=g_sm_ncont; execute(&c);
    CHECK("six independent slices take five continuations",!sm_on() || g_sm_ncont-before==5);
    id<MTLTexture> mt=tex_mtl(&t);
    id<MTLBuffer> out=[g_dev newBufferWithLength:6*256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[g_q commandBuffer]; id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
    for(int sl=0;sl<6;sl++) [e copyFromTexture:mt sourceSlice:sl sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:out destinationOffset:sl*256 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int bad=0; const uint8_t *bytes=out.contents;
    for(int i=0;i<6*64;i++) bad+=memcmp(bytes+4*i,(uint8_t[]){255,0,0,255},4)!=0;
    CHECK("every pixel of all six layers is red",!bad);

    reset(); R.has_rt=1; R.rt.n=1; R.rt.color[0]=&t;
    R.rt.has_cview[0]=1; R.rt.cview[0]=(view_t){.set_layers=1,.nlayers=1};
    mtl_reg_tex(501,&t,NULL); S.tex[1][0]=501;
    p->sh[1]->rf.nb=1; p->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0};
    chain next={0}; rt(&next,&t,1,NULL); triangle(&next); emit(&next,OP_END,NULL,0); g_exec_next=next.p;
    sm_plan(p); CHECK("first draw sampling attachment cannot start slice merge",!g_sm_plan.on);
    int kinds[]={NVN_RB_TEXU,NVN_RB_TEXD,NVN_RB_TEXA};
    for(unsigned i=0;i<sizeof kinds/sizeof *kinds;i++) {
        p->sh[1]->rf.b[0].kind=kinds[i];
        CHECK("unknown/bindless attachment reads split merged pass",draw_samples_rt(p));
    }
    p->sh[1]->rf.nb=0; p->sh[1]->rf.bindless=1;
    CHECK("bindless reflection splits merged pass",draw_samples_rt(p));
    p->sh[1]->rf.bindless=0; p->sh[1]->rf.dyn=1;
    CHECK("dynamic reflection splits merged pass",draw_samples_rt(p));
    g_exec_next=NULL; g_sm_plan.on=0;
}
static void hoists(id<MTLLibrary> lib) {
    prog_t pg={0}, cp={.serial=910000}; NvProg *drawpr=make_program(lib,&pg,"red");
    NvProg *cs=[NvProg new]; cs->sh[5]=[NvShader new]; cs->sh[5]->stage=5;
    cs->sh[5]->rf.tg[0]=cs->sh[5]->rf.tg[1]=cs->sh[5]->rf.tg[2]=1;
    cs->sh[5]->fn=[lib newFunctionWithName:@"compute_one"];
    cs->sh[5]->ae=[cs->sh[5]->fn newArgumentEncoderWithBufferIndex:0];
    cs->sh[5]->rf.nb=1; cs->sh[5]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};
    serial_put(cp.serial,cs);
    void *mem=NULL; if(posix_memalign(&mem,getpagesize(),getpagesize())) abort(); memset(mem,0,getpagesize()); mtl_pool_add(mem,getpagesize());
    tex_t t=texture(NVN_TEXTURE_TARGET_2D,NVN_FORMAT_RGBA8,1);
    for(int hazard=0;hazard<2;hazard++) {
        chain c={0}; rt(&c,&t,-1,NULL); bind(&c,&pg,3); bind(&c,&cp,32);
        p_bind_buf b={.stage=5,.index=0,.addr=(uint64_t)mem,.size=4}; emit(&c,OP_BIND_SSBO,&b,sizeof b);

        triangle(&c);
        if(hazard) { uint32_t bits=~0u; emit(&c,OP_BARRIER,&bits,sizeof bits); }
        p_dispatch d={.x=1,.y=1,.z=1}; emit(&c,OP_DISPATCH,&d,sizeof d); triangle(&c);
        uint64_t before=g_csh_moved; execute(&c);
        CHECK(hazard?"explicit game barrier prevents hoist":"independent dispatch hoisted once",g_csh_moved-before==(cshoist_on() && !hazard));
        CHECK("compute writes expected shared memory",*(uint32_t *)mem==73);
    }
    // Drive the production scanner with stencil writes and a stencil-only read.
    tex_t ds=texture(NVN_TEXTURE_TARGET_2D,NVN_FORMAT_DEPTH32F_STENCIL8,1);
    view_t view={.set_dsmode=1,.dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL}; mtl_reg_tex(502,&ds,&view);
    cs->sh[5]->rf.nb=2; cs->sh[5]->rf.b[1]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0,.aid=1};
    reset(); R.has_rt=1; R.rt.n=1; R.rt.color[0]=&t; R.rt.depth=&ds;
    S.cprog=&cp; S.tex[5][0]=502; S.ssbo[5][0]=(typeof(S.ssbo[5][0])){.addr=(uint64_t)mem,.size=4};
    S.ds.dw=0; S.ds.st=1; S.ds.zpass_f=NVN_STENCIL_OP_REPLACE; S.st_wmask[0]=255;
    chain next={0}; p_dispatch d={.x=1,.y=1,.z=1}; emit(&next,OP_DISPATCH,&d,sizeof d); emit(&next,OP_END,NULL,0);
    p_draw dw={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3};
    uint64_t before=g_csh_moved; mtl_sub_begin(); g_exec_next=next.p; cs_hoist_try(prog_get(&pg),&dw,NULL); mtl_sub_commit(); mtl_drain();
    CHECK("stencil read stays after stencil-writing draw",g_csh_moved==before);
    g_exec_next=NULL;
    reset(); csh_rng ranges[CSH_RN]; int nr=0;
    p_draw_idx indirect={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.ibuf=(uint64_t)mem,.ind=(uint64_t)mem+512};
    CHECK("unknown indirect index range prevents hoist",!csh_draw_reads(prog_get(&pg),NULL,&indirect,ranges,&nr));
    tex_t linear=t; linear.mem=mem; linear.d.flags=NVN_TEXTURE_FLAGS_LINEAR; linear.d.target=NVN_TEXTURE_TARGET_2D; linear.L.size=256;
    mtl_reg_tex(503,&linear,NULL); S.tex[1][0]=503;
    drawpr->sh[1]->rf.nb=1; drawpr->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0};
    nr=0; int known=csh_draw_reads(drawpr,&dw,NULL,ranges,&nr);
    csh_rng write={(uint64_t)mem+128,(uint64_t)mem+132};
    CHECK("sampled linear storage conflicts with compute write",!known || csh_hit(ranges,nr,&write,1));
    drawpr->sh[1]->rf.b[0].kind=NVN_RB_TEXU; nr=0;
    CHECK("unresolved draw texture storage prevents hoist",!csh_draw_reads(drawpr,&dw,NULL,ranges,&nr));
    R.has_rt=1; R.rt.n=1; R.rt.color[0]=&linear;
    cs->sh[5]->rf.nb=1; cs->sh[5]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_UBOP,.reg=0};
    S.ubo[5][0]=(p_bind_buf){.addr=(uint64_t)mem,.size=4}; nr=0; int depth=0;
    CHECK("compute uniform read cannot precede aliased attachment write",!csh_cs(cs->sh[5],&d,ranges,&nr,&depth));
    cs->sh[5]->rf.b[0].kind=NVN_RB_SBUFP;
    S.ssbo[5][0]=S.ubo[5][0]; nr=0;
    CHECK("compute storage access cannot precede aliased attachment write",!csh_cs(cs->sh[5],&d,ranges,&nr,&depth));
    tex_t overlapping=linear; overlapping.serial++; overlapping.mem=(uint8_t *)mem+64; overlapping.L.size=128;
    mtl_reg_tex(504,&overlapping,NULL); S.tex[5][0]=504;
    cs->sh[5]->rf.b[0].kind=NVN_RB_TEX; nr=0;
    CHECK("offset compute texture read cannot precede overlapping target",!csh_cs(cs->sh[5],&d,ranges,&nr,&depth));
    S.tex[1][0]=504; drawpr->sh[1]->rf.b[0].kind=NVN_RB_TEX;
    CHECK("offset sampled storage splits merged pass",draw_samples_rt(drawpr));
    mtl_pool_del(mem); free(mem);
    mtl_prog_release(&cp); mtl_prog_release(&pg);
}
static void wait_budgets(void) {
    id<MTLSharedEvent> saved_event = g_ev;
    uint64_t saved_done = g_done, saved_fence = g_fence;
    post_rec saved_post = g_postr[1];
    uint64_t saved_n[WS_N], saved_blk[WS_N], saved_ns[WS_N];
    for (int i = 0; i < WS_N; i++) { saved_n[i] = g_ws_n[i]; saved_blk[i] = g_ws_blk[i]; saved_ns[i] = g_ws_ns[i]; }
    unsigned char saved_fend[sizeof g_fend]; memcpy(saved_fend, g_fend, sizeof g_fend);
    uint64_t saved_wake_n = g_ws_wake_n, saved_wake_ns = g_ws_wake_ns, saved_fb_ns = g_ws_fb_ns;
    int fast = fast_wake();
    g_ev = (id<MTLSharedEvent>)[WaitBudgetEvent new]; g_done = 0; g_fence = 1;
    test_budget_active = 1;
    const uint64_t hour = 3600000000000ull;
    const struct {
        const char *name;
        uint64_t timeout, elapsed, event_ms, remaining;
        int fallback;
    } cases[] = {
        {"20 ms exhausted", 20000000, 20000000, 20, 0, 0},
        {"20.5 ms retains 500 us", 20500000, 20000000, 20, 500000, 1},
        {"early event failure retains full budget", 20000000, 0, 20, 20000000, 1},
        {"event overshoots deadline", 20000000, 21000000, 20, 0, 0},
        {"zero timeout", 0, 0, 0, 0, 0},
        {"sub-ms timeout", 999999, 0, 0, 0, 0},
        {"one ms exhausted", 1000000, 1000000, 1, 0, 0},
        {"infinite sentinel keeps one hour per stage", UINT64_MAX, hour, 3600000, hour, 1},
        {"finite hour plus 20 ms retains 20 ms", hour + 20000000, hour, 3600000, 20000000, 1},
        {"finite two hours keeps fallback hour clamp", 2 * hour + 20000000, hour, 3600000, hour, 1},
    };
    int sites[] = {WS_SYNC, WS_SYNC_NEWEST, WS_ACQ};
    for (unsigned si = 0; si < sizeof sites / sizeof *sites; si++) {
        for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
            const typeof(*cases) *c = &cases[i];
            test_budget = (typeof(test_budget)){.now = 1999750000, .event_elapsed = c->elapsed};
            printf("budget: %s, site=%d, fast=%d, ledger=%d\n", c->name, sites[si], fast, ws_on());
            uint64_t trace = mtl_sync_trace_begin(1, c->timeout, sites[si] == WS_ACQ ? 2 : 1);
            CHECK("incomplete fence returns false", !mtl_wait_at(1, c->timeout, sites[si]));
            mtl_sync_trace_end(trace, 1, NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
            CHECK("event receives truncated and clamped milliseconds", test_budget.event_calls == fast
                && (!fast || (test_budget.event_ms == c->event_ms && test_budget.event_fence == 1)));
            CHECK("only the virtual event advances time", test_budget.now == 1999750000 + (fast ? c->elapsed : 0));
            int fallback = fast ? c->fallback : 1;
            uint64_t remaining = fast ? c->remaining : (c->timeout > hour ? hour : c->timeout);
            CHECK("feedback wait count", test_budget.cond_calls == fallback);
            CHECK("feedback absolute deadline has only remaining budget", !fallback
                || test_budget.cond_remaining == (int64_t)remaining);
        }
    }
    test_budget = (typeof(test_budget)){.now = 1999750000}; g_done = 1;
    CHECK("completed fence returns immediately", mtl_wait_at(1, 20000000, WS_ACQ));
    CHECK("completed fence skips both waits", !test_budget.event_calls && !test_budget.cond_calls
        && test_budget.now == 1999750000);
    g_done = 0;
    CHECK("public zero-time poll returns false", !mtl_wait(1, 0));
    CHECK("public zero-time poll skips event and has zero deadline", !test_budget.event_calls
        && test_budget.cond_calls == 1 && test_budget.cond_remaining == 0);
    if (fast) {
        volatile uint32_t event = 0;
        g_postr[1] = (post_rec){.fence = 1, .n = 1, .ops = calloc(1, sizeof(post_op))};
        g_postr[1].ops[0] = (post_op){.kind = 1, .val = &event, .v = 47};
        test_budget = (typeof(test_budget)){.now = 1999750000, .event_elapsed = 7000000, .event_success = 1};
        CHECK("signaled event succeeds through real post_early", mtl_wait_at(1, 20000000, WS_ACQ));
        CHECK("CPU event is visible before successful return", event == 47 && g_done == 1
            && g_postr[1].claimed && !g_postr[1].ops);
        CHECK("successful early wait skips feedback wait", test_budget.event_calls == 1 && !test_budget.cond_calls);
    }
    test_budget_active = 0; memset(&test_budget, 0, sizeof test_budget);
    g_ev = saved_event; g_done = saved_done; g_fence = saved_fence; g_postr[1] = saved_post;
    for (int i = 0; i < WS_N; i++) { g_ws_n[i] = saved_n[i]; g_ws_blk[i] = saved_blk[i]; g_ws_ns[i] = saved_ns[i]; }
    memcpy(g_fend, saved_fend, sizeof g_fend);
    g_ws_wake_n = saved_wake_n; g_ws_wake_ns = saved_wake_ns; g_ws_fb_ns = saved_fb_ns;
}
static dispatch_semaphore_t waiter_started, waiter_done;
static void *early_waiter(void *arg) {
    dispatch_semaphore_signal(waiter_started); post_early((uintptr_t)arg); dispatch_semaphore_signal(waiter_done); return NULL;
}
static void posts(void) {
    // Pause the real completion owner between claim and its first event write.
    uint64_t f=g_fence+1; post_rec *r=&g_postr[f%POST_RING]; volatile uint32_t event=0;
    r->ops=calloc(1,sizeof *r->ops); r->ops[0]=(post_op){.kind=1,.val=&event,.v=47}; r->n=1; r->fence=f; r->claimed=0;
    CHECK("completion owner claimed",post_claim(r,f));
    waiter_started=dispatch_semaphore_create(0); waiter_done=dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th,NULL,early_waiter,(void *)(uintptr_t)f);
    dispatch_semaphore_wait(waiter_started,DISPATCH_TIME_FOREVER);
    int returned=dispatch_semaphore_wait(waiter_done,dispatch_time(DISPATCH_TIME_NOW,50000000))==0;
    CHECK("competing waiter waits for CPU effects",!returned);
    CHECK("fence remains incomplete while owner is paused",g_done<f);
    post_run(r,0,0); if(!returned) dispatch_semaphore_wait(waiter_done,DISPATCH_TIME_FOREVER); pthread_join(th,NULL);
    CHECK("event visible before successful wait",event==47 && g_done>=f);
    // A late callback must not touch the new generation of its old ring slot.
    r->fence=f+POST_RING; r->claimed=0; r->n=1;
    r->ops=calloc(1,sizeof *r->ops); r->ops[0]=(post_op){.kind=1,.val=&event,.mode=1,.v=3};
    int stale=post_claim(r,f);
    CHECK("old fence cannot claim recycled slot",!stale);
    if(stale) post_run(r,0,0);
    post_complete(f,0,0);
    CHECK("old callback leaves future event untouched",event==47);
    free(r->ops); r->ops=NULL; r->n=0; r->claimed=1;

    post_rec *a=&g_postr[(f+1)%POST_RING], *b=&g_postr[(f+2)%POST_RING];
    *a=(post_rec){.fence=f+1,.n=1,.ops=calloc(1,sizeof(post_op))};
    *b=(post_rec){.fence=f+2,.n=1,.ops=calloc(1,sizeof(post_op))};
    a->ops[0]=(post_op){.kind=1,.val=&event,.v=10};
    b->ops[0]=(post_op){.kind=1,.val=&event,.mode=1,.v=5};
    post_complete(f+2,0,0);
    CHECK("later completion applies earlier events in order",event==15 && g_done==f+2);
    post_complete(f+1,0,0); post_complete(f+2,0,0);
    CHECK("callbacks never repeat an event",event==15);
}
static dispatch_semaphore_t boundary_start, boundary_recorded, boundary_finish;
static uint64_t boundary_identity;
static void *trace_boundary_writer(void *unused) {
    (void)unused;
    dispatch_semaphore_wait(boundary_start, DISPATCH_TIME_FOREVER);
    boundary_identity = mtl_sync_trace_begin(777, 10000000, 1);
    ft_emit(FT_SUBMIT_RECORD, 0, 777, 0, 0, 0, 0);
    dispatch_semaphore_signal(boundary_recorded);
    dispatch_semaphore_wait(boundary_finish, DISPATCH_TIME_FOREVER);
    ft_emit(FT_FEEDBACK, 0, 777, 0, 0, 1, 2);
    mtl_sync_trace_end(boundary_identity, 777, NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    return NULL;
}
static void trace_boundary_test(void) {
    char directory[] = "/tmp/nxnative-fence-boundary-XXXXXX";
    if (!mkdtemp(directory)) { CHECK("temporary capture directory", 0); return; }
    char setting[1200]; snprintf(setting, sizeof setting, "10-20:%s/events.tsv", directory);
    setenv("DM_NVN_FENCE_TRACE", setting, 1);
    mtl_set_frame(9);
    CHECK("preceding frame is not selected", !ft_selected());
    boundary_start = dispatch_semaphore_create(0); boundary_recorded = dispatch_semaphore_create(0); boundary_finish = dispatch_semaphore_create(0);
    pthread_t thread; if (pthread_create(&thread, NULL, trace_boundary_writer, NULL)) { CHECK("boundary producer starts", 0); return; }
    mtl_set_frame(10); dispatch_semaphore_signal(boundary_start);
    dispatch_semaphore_wait(boundary_recorded, DISPATCH_TIME_FOREVER);
    CHECK("producer selects first published frame", boundary_identity != 0);
    mtl_set_frame(21); size_t count = g_fence_trace_buffer.used;
    dispatch_semaphore_signal(boundary_finish); pthread_join(thread, NULL);
    CHECK("delayed completion cannot append after cutoff", g_fence_trace_buffer.used == count && !ft_selected());
    unsigned begins = 0, ends = 0, submitted = 0, feedback = 0; int frames = 1;
    for (size_t i = 0; i < count; i++) {
        ft_event e = g_fence_trace_buffer.events[i]; frames &= e.frame == 10;
        begins += e.kind == FT_SYNC_BEGIN; ends += e.kind == FT_SYNC_END;
        submitted += e.kind == FT_SUBMIT_RECORD; feedback += e.kind == FT_FEEDBACK;
    }
    CHECK("cutoff retains missing joins instead of fabricating completion", begins == 1 && submitted == 1 && !ends && !feedback && frames);
    NSData *bytes = nil;
    for (int i = 0; i < 5000 && !bytes; i++) { bytes = [NSData dataWithContentsOfFile:@(g_ft_path)]; if (!bytes) usleep(1000); }
    NSString *text = bytes ? [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] : @"";
    CHECK("incomplete trace exports intact rows and explicit cutoff", bytes && [text containsString:@"missing joins remain unknown"]
        && [text componentsSeparatedByString:@"\n"].count == count + 3);
    printf("boundary fixture export: %s\n", g_ft_path);
}
static void trace_gpu_export(void) {
    // Wait for real feedback independently of the shared-event early wake path.
    int complete = 0;
    for (int attempt = 0; attempt < 5000; attempt++) {
        unsigned submitted = 0, feedback = 0;
        pthread_mutex_lock(&g_fence_trace_buffer.lock);
        for (size_t i = 0; i < g_fence_trace_buffer.used; i++) {
            submitted += g_fence_trace_buffer.events[i].kind == FT_SUBMIT_RECORD;
            feedback += g_fence_trace_buffer.events[i].kind == FT_FEEDBACK;
        }
        pthread_mutex_unlock(&g_fence_trace_buffer.lock);
        if (submitted >= 2 && submitted == feedback) { complete = 1; break; }
        usleep(1000);
    }
    CHECK("multiple real submissions receive feedback", complete);
    mtl_set_frame(3);
    CHECK("frame published before capture closes", g_cur_frame == 3 && g_ft_state == 2);
    unsigned submissions = 0; int joined = 1;
    for (size_t i = 0; i < g_fence_trace_buffer.used; i++) {
        ft_event e = g_fence_trace_buffer.events[i];
        if (e.kind != FT_SUBMIT_RECORD) continue;
        submissions++; unsigned begin = 0, end = 0, feedback = 0;
        uint64_t begin_time = 0, end_time = 0;
        for (size_t j = 0; j < g_fence_trace_buffer.used; j++) {
            ft_event q = g_fence_trace_buffer.events[j];
            if (q.fence != e.fence) continue;
            if (q.kind == FT_COMMIT_BEGIN) { begin++; begin_time = q.cpu_ns; }
            if (q.kind == FT_COMMIT_END) { end++; end_time = q.cpu_ns; }
            feedback += q.kind == FT_FEEDBACK;
        }
        joined &= begin == 1 && end == 1 && feedback == 1 && begin_time >= e.cpu_ns && end_time >= begin_time;
    }
    CHECK("submission fences join actual commit and feedback", submissions >= 2 && joined);
    NSString *path = @(g_ft_path); NSData *bytes = nil;
    for (int attempt = 0; attempt < 5000 && !bytes; attempt++) { bytes = [NSData dataWithContentsOfFile:path]; if (!bytes) usleep(1000); }
    CHECK("atomic TSV export exists", bytes != nil);
    NSString *text = bytes ? [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] : @"";
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    CHECK("export identifies cutoff and complete row count", [text containsString:@"cutoff is NOT completion"]
        && lines.count == g_fence_trace_buffer.used + 3);
    size_t count = g_fence_trace_buffer.used;
    ft_emit(FT_FEEDBACK, 0, UINT64_MAX, 0, 0, 0, 0);
    CHECK("post-cutoff callback cannot change exported records", g_fence_trace_buffer.used == count
        && [[NSData dataWithContentsOfFile:path] isEqualToData:bytes]);
    printf("fence trace fixture export: %s\n", g_ft_path);
}
static void trace_guest_wrapper(void) {
    // Use the actual linked HLE implementation registered in the executable.
    struct entry { const char *name; void *fn; };
    unsigned long size = 0;
    const struct entry *entries = (const void *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &size);
    typedef NVNsyncWaitResult (*guest_wait_fn)(const NVNsync *, uint64_t);
    guest_wait_fn wait = NULL;
    for (size_t i = 0; entries && i < size / sizeof *entries; i++)
        if (!strcmp(entries[i].name, "nvnSyncWait")) wait = (guest_wait_fn)entries[i].fn;
    CHECK("real guest SyncWait implementation found", wait != NULL);
    if (!wait) return;
    id<MTLSharedEvent> saved_event = g_ev;
    uint64_t saved_done = g_done, saved_fence = g_fence;
    g_ev = (id<MTLSharedEvent>)[WaitBudgetEvent new]; g_done = 0; g_fence = 1;
    test_budget_active = 1; test_trace_lock = &g_fence_trace_buffer.lock; test_trace_delay = 2000000;
    NVNsync sync = {0}; sync_t *obj = (sync_t *)&sync;
    test_budget = (typeof(test_budget)){.now = 1999750000};
    CHECK("unfenced guest sync times out without waiting", wait(&sync, 20000000) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED
        && !test_budget.event_calls && !test_budget.cond_calls && !g_ft_sync);
    obj->fenced = 1; obj->fence = 1; g_done = 1;
    CHECK("completed guest sync reports already signaled", wait(&sync, 20000000) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED && !g_ft_sync);
    g_done = 0; test_budget = (typeof(test_budget)){.now = 1999750000};
    CHECK("incomplete guest sync preserves timeout result", wait(&sync, 20000000) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED && !g_ft_sync);
    CHECK("guest poll trace is charged before timed event", !fast_wake() || test_budget.event_ms == 10);
    CHECK("guest fallback excludes all prior trace delays", test_budget.cond_remaining == (fast_wake() ? 6000000 : 8000000));
    test_budget = (typeof(test_budget)){.now = 1999750000};
    CHECK("zero guest timeout remains a poll", wait(&sync, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED
        && !test_budget.event_calls && test_budget.cond_remaining <= 0);
    test_budget = (typeof(test_budget)){.now = 1999750000};
    CHECK("infinite guest timeout retains sentinel", wait(&sync, UINT64_MAX) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED
        && (!fast_wake() || test_budget.event_ms == 3600000) && test_budget.cond_remaining == 3600000000000ll - 2000000);
    test_done = &g_done;
    test_budget = (typeof(test_budget)){.now = 1999750000, .cond_complete = 1};
    CHECK("guest timed completion reports condition satisfied", wait(&sync, 20000000) == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED && !g_ft_sync);
    test_done = NULL;
    test_trace_delay = 0; test_trace_lock = NULL; test_budget_active = 0;
    g_ev = saved_event; g_done = saved_done; g_fence = saved_fence;
}
static void trace_delayed_budget(void) {
    id<MTLSharedEvent> saved_event = g_ev;
    uint64_t saved_done = g_done, saved_fence = g_fence;
    g_ev = (id<MTLSharedEvent>)[WaitBudgetEvent new]; g_done = 0; g_fence = 1;
    test_budget_active = 1;
    test_budget = (typeof(test_budget)){.now = 1999750000, .event_elapsed = 5000000};
    test_trace_lock = &g_fence_trace_buffer.lock; test_trace_delay = 2000000;
    uint64_t trace = mtl_sync_trace_begin(1, 20000000, 1);
    CHECK("delayed trace retains incomplete result", !mtl_wait_at(1, 20000000, WS_SYNC));
    CHECK("trace delay is charged to event budget", !fast_wake() || test_budget.event_ms == 18);
    CHECK("fallback deadline excludes trace delay", test_budget.cond_calls == 1
        && test_budget.cond_remaining == (fast_wake() ? 9000000 : 16000000));
    mtl_sync_trace_end(trace, 1, NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    test_trace_delay = 0; test_trace_lock = NULL; test_budget_active = 0;
    g_ev = saved_event; g_done = saved_done; g_fence = saved_fence;
}
static void trace_poll_pair(void) {
    uint64_t saved_done = g_done, saved_fence = g_fence;
    g_done = 0; g_fence = 1; test_budget_active = 1;
    test_budget = (typeof(test_budget)){.now = 1999750000};
    size_t start = g_fence_trace_buffer.used;
    uint64_t trace = mtl_sync_trace_begin(1, 20000000, 1);
    CHECK("selected guest wait has an identity", trace != 0);
    CHECK("initial poll remains incomplete", !mtl_wait(1, 0));
    // Cross a presentation boundary between the poll and timed call.
    mtl_set_frame(11); g_done = 1;
    CHECK("timed call observes completed fence", mtl_wait(1, 20000000));
    mtl_sync_trace_end(trace, 1, NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    unsigned begins = 0, ends = 0, waits = 0, returns = 0;
    int identity = 1, frame10 = 0, frame11 = 0;
    for (size_t i = start; i < g_fence_trace_buffer.used; i++) {
        ft_event e = g_fence_trace_buffer.events[i];
        if (e.kind == FT_CLOCK_PAIR) continue;
        identity &= e.sync == trace && e.fence == 1;
        begins += e.kind == FT_SYNC_BEGIN; ends += e.kind == FT_SYNC_END;
        waits += e.kind == FT_WAIT_BEGIN; returns += e.kind == FT_WAIT_END;
        frame10 |= e.frame == 10; frame11 |= e.frame == 11;
    }
    CHECK("poll and timed call share one guest identity", identity && begins == 1 && ends == 1 && waits == 2 && returns == 2);
    CHECK("cross-frame wait preserves observed frames", frame10 && frame11);
    CHECK("guest scope clears after return", g_ft_sync == 0);
    test_budget_active = 0; g_done = saved_done; g_fence = saved_fence;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 2 && !strcmp(argv[1], "--trace-boundary")) {
        alarm(10); trace_boundary_test(); printf("boundary: %d checks, %d failed\n", checks, failed); return failed != 0;
    }
    if(argc==2 && (!strcmp(argv[1],"--cpu") || !strcmp(argv[1],"--trace-cpu"))) {
        int traced = !strcmp(argv[1],"--trace-cpu");
        if (traced) { setenv("DM_NVN_FENCE_TRACE", "10-20:/tmp/nxnative-fence-test.tsv", 1); mtl_set_frame(10); trace_poll_pair(); trace_delayed_budget(); trace_guest_wrapper(); }
        alarm(10); setbuf(stdout,NULL); wait_budgets(); posts();
        printf("CPU completion: %d checks, %d failed\n",checks,failed); return failed!=0;
    }
    int trace_gpu = argc == 2 && !strcmp(argv[1], "--trace-gpu");
    if (trace_gpu) {
        char directory[] = "/tmp/nxnative-fence-XXXXXX";
        if (!mkdtemp(directory)) return 2;
        char setting[1200]; snprintf(setting, sizeof setting, "1-2:%s/events.tsv", directory);
        setenv("DM_NVN_FENCE_TRACE", setting, 1); mtl_set_frame(1);
    }
    alarm(30); setbuf(stdout,NULL); mtl_init();
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
        "fragment float4 red() { return float4(1,0,0,1); }\n"
        "struct Args { device uint *p [[id(0)]]; };\n"
        "kernel void compute_one(constant Args &a [[buffer(0)]]) { *a.p=73; }\n" options:nil error:&error];
    if(!lib) { fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 2; }
    slices(lib); hoists(lib); posts();
    if (trace_gpu) trace_gpu_export();
    printf("performance ordering: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
