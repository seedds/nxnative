// Busy-queue cut admission through production submission code and real GPU copies.
// A synthetic outstanding count makes the admission decision deterministic; actual
// segment/final commits must balance their counts and preserve copy ordering.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
static void scenario(int outstanding, int copy_cut, int enabled, int segment) {
    id<MTLBuffer> io=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> mid=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    CHECK(io && mid,"GPU buffers allocated"); if (!io || !mid) abort();
    res_add(io); res_add(mid); memset(io.contents,0,4096); memset(io.contents,0x6d,256);
    CHECK(atomic_load(&g_seg_inflight)==0,"previous real submissions completed");
    if (outstanding) atomic_fetch_add(&g_seg_inflight,1);
    mtl_sub_begin();
    enc_b_copy_buffer(blit_buf(0x10000,0x10100,0x20000,0x20100),io,0,mid,0,256);
    g_stat_draw+=64; g_cut_copy=copy_cut;
    uint64_t cuts=g_cuts; sub_cut();
    int skip=enabled && outstanding && !copy_cut && !segment;
    CHECK(g_cuts-cuts==(uint64_t)!skip,"only non-copy busy submission mode defers the cut");
    CHECK(!skip || g_cut_draw0==g_stat_draw,"deferred cut advances its draw-count boundary");
    enc_b_copy_buffer(blit_buf(0x20000,0x20100,0x10200,0x10300),mid,0,io,512,256);
    uint64_t fence=mtl_sub_commit();
    CHECK(mtl_wait(fence,5000000000ull),"final fence completes"); mtl_drain();
    CHECK(!mtl_last_commit_error(),"GPU submission has no error");
    CHECK(!memcmp((uint8_t*)io.contents+512,io.contents,256),"copy ordering survives both deferred and committed boundaries");
    // Feedback can follow the GPU event; wait for the callback, not another frame.
    for(int i=0;i<2000 && atomic_load(&g_seg_inflight)!=outstanding;i++) usleep(1000);
    CHECK(atomic_load(&g_seg_inflight)==outstanding,"real segment/final feedback balances its outstanding count");
    if(outstanding) atomic_fetch_sub(&g_seg_inflight,1);
    res_remove(io); res_remove(mid);
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_CUT","64",1); mtl_init();
    int enabled=getenv("DM_NVN_CUT_BUSY") && atoi(getenv("DM_NVN_CUT_BUSY"));
    int segment=seg_ts();
    scenario(0,0,enabled,segment); scenario(1,0,enabled,segment); scenario(1,1,enabled,segment);
    printf("t_cut_busy busy=%d segment=%d: %d checks, %d failed\n",enabled,segment,checks,failed);
    return failed!=0;
} }
