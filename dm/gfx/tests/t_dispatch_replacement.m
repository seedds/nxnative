// A selected host replacement must not depend on the unused translated pipeline.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks,failed;
#define CHECK(c,label) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}} while(0)
int main(void) { @autoreleasepool {
    setenv("DM_NDVE_ENGINE","1",1);mtl_init();
    NvShader *shader=[NvShader new];
    snprintf(shader->key,sizeof shader->key,"%s_b",k_ndve_keys[2]);
    shader->rf.tg[0]=shader->rf.tg[1]=shader->rf.tg[2]=1;
    // A missing/refused translated function cannot stop the selected native kernel.
    shader->cps_failed=1;
    NvProg *program=[NvProg new];program->sh[5]=shader;
    prog_t handle={0};handle.serial=997701;serial_put(handle.serial,program);
    memset(&S,0,sizeof S);S.cprog=&handle;
    g_nd.state=2;g_nd.pos=2;g_nd.w0=0x100000;
    S.ssbo[5][2]=(p_bind_buf){.addr=g_nd.w0+g_ndve_w[2],.size=16};
    S.ssbo[5][3]=(p_bind_buf){.addr=g_nd.w0+g_ndve_b[2],.size=16};
    p_dispatch d={.x=1,.y=1,.z=1};
    uint64_t built=g_bp_pso_n,skipped=g_nd.skipped;
    mtl_sub_begin();dispatch(&d);
    CHECK(g_nd.pos==3,"NDVE consumes the recognized layer despite a refused unused translated pipeline");
    CHECK(g_nd.skipped==skipped+1,"replacement records the skipped original layer exactly once");
    CHECK(g_bp_pso_n==built,"replacement does not attempt to compile the unused original pipeline");
    const void *key=(__bridge const void*)shader;
    CHECK(hm_get_id(&g_cps,&key)==nil,"no fake original pipeline is inserted into the cache");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"replacement submission completes");
    // If the replacement is unavailable, retain the ordinary failed-pipeline guard.
    g_nd.off=1;int pos=g_nd.pos;
    mtl_sub_begin();dispatch(&d);
    CHECK(g_nd.pos==pos && g_nd.skipped==skipped+1,"declined replacement does not consume a layer");
    CHECK(g_bp_pso_n==built,"a refused fallback is not recompiled on every dispatch");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"declined replacement submission completes");
    fprintf(stderr,"t_dispatch_replacement: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}}
