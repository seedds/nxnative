// A commands node that only sets its queue's descriptor pools (nvnCommandBufferSetTexturePool / SetSamplerPool, then nothing) is applied to that queue's state with no Metal
// submission (nvn_mtl.m mtl_exec_state_only, nvn_queue.inc nq_worker): the queue's fence does not move, and its pools are the ones the next node of the queue uses. A node with
// anything else in it is a submission as before. With DM_NVN_EMPTY_SUB=0 (the Makefile's second run) the pool-only node is a submission too, with the same state
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#define P(name) ((NVNT_##name)nvn_lookup(#name))
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, label) do { checks++; if (!(c)) { fails++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static uint8_t commands[4096] __attribute__((aligned(8))); static size_t used;
static void emit(unsigned op, const void *p, size_t n) {
    size_t size = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)size};
    memcpy(commands + used, &h, 8); if (n) memcpy(commands + used + 8, p, n); used += size;
}
static void submit(NVNqueue *q) {
    static uint64_t list; list = ctl_e(CTL_CHUNK,commands,used) | CTL_LAST; NVNcommandHandle handle = (NVNcommandHandle)&list;
    P(nvnQueueSubmitCommands)(q,1,&handle); P(nvnQueueFinish)(q);
}
int main(void) { @autoreleasepool {
    const char *e = getenv("DM_NVN_EMPTY_SUB"); int on = !(e && !strcmp(e,"0"));
    mtl_init();
    static NVNdevice device; static NVNqueue queue; NVNdeviceBuilder db = {0};
    P(nvnDeviceBuilderSetDefaults)(&db); CHECK(P(nvnDeviceInitialize)(&device,&db),"device initialized");
    NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb,&device);
    CHECK(P(nvnQueueInitialize)(&queue,&qb),"queue initialized");
    uint64_t serial = ((queue_t *)&queue)->serial;
    static NVNmemoryPool memory; static NVNtexturePool tp[2]; static NVNsamplerPool sp[2];
    for (int i=0;i<2;i++) { P(nvnTexturePoolInitialize)(&tp[i],&memory,0,64); P(nvnSamplerPoolInitialize)(&sp[i],&memory,0,64); }
    int t0 = nvn_dpool_base(&tp[0],1), t1 = nvn_dpool_base(&tp[1],1), s0 = nvn_dpool_base(&sp[0],0), s1 = nvn_dpool_base(&sp[1],0);
    CHECK(t0 != t1 && s0 != s1,"two pools of each kind number their IDs apart");
    // A node of pool settings only
    used = 0; p_set_pool a = {(const dpool_t *)&tp[1]}, b = {(const dpool_t *)&sp[1]};
    emit(OP_SET_TEX_POOL,&a,sizeof a); emit(OP_SET_SAMP_POOL,&b,sizeof b);
    uint64_t f0 = atomic_load(&g_fence), e0 = mtl_empty_subs();
    submit(&queue);
    mtl_queue_activate(serial);
    CHECK(S.tex_base == t1 && S.samp_base == s1,"the node's pools become the queue's");
    if (on) CHECK(atomic_load(&g_fence) == f0 && mtl_empty_subs() == e0 + 1,"a node of pool settings only makes no Metal submission: the queue's fence stays");
    else CHECK(atomic_load(&g_fence) > f0 && mtl_empty_subs() == e0,"with DM_NVN_EMPTY_SUB=0 it is a submission of its own");
    // Pools and a clear: a submission, as before
    tex_t target = {.magic=MAGIC('T','E','X',0),.serial=930001};
    target.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=1,.levels=1,.samples=1};
    used = 0; a.pool = (const dpool_t *)&tp[0]; emit(OP_SET_TEX_POOL,&a,sizeof a);
    p_set_rt rt = {.n=1}; rt.color[0]=&target; emit(OP_SET_RT,&rt,sizeof rt);
    p_clear_color clear = {.rgba={0,1,0,1},.mask=15}; emit(OP_CLEAR_COLOR,&clear,sizeof clear);
    uint64_t f1 = atomic_load(&g_fence), e1 = mtl_empty_subs();
    submit(&queue);
    mtl_queue_activate(serial);
    CHECK(atomic_load(&g_fence) > f1 && mtl_empty_subs() == e1,"a node with work besides its pool settings is a submission");
    CHECK(S.tex_base == t0 && S.samp_base == s1,"its pool setting holds, and the sampler pool the earlier node set stays");
    printf("t_empty_sub (DM_NVN_EMPTY_SUB=%s): %d checks, %d failed\n",on?"1":"0",checks,fails); return fails != 0;
} }
