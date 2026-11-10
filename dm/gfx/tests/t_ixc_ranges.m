// Range proof for preserving indirect conversions after native mesh writes.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init(); size_t bytes = (IXC_CAP * 4 + 32767) & ~(size_t)16383;
    void *mem = NULL; CHECK(!posix_memalign(&mem, 16384, bytes)); if (!mem) return 1;
    memset(mem, 0, bytes); mtl_pool_add(mem, bytes); uint64_t base = (uintptr_t)mem;
    NvShader *sh = [NvShader new]; NvProg *pr = [NvProg new]; pr->sh[4] = sh;
    sh->rf.nb = 1;
#define RESET() do { g_ixc_n=1; g_ixc[0]=(ixc_ent){.ind=base,.prim=7,.itype=-1}; g_ixc_epoch=100; g_wr_epoch=101; g_ixc_sub=g_sub_serial; S.ssbo[4][0]=(p_bind_buf){.addr=base+64,.size=16}; sh->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0}; sh->sb_ro=0; } while (0)
    RESET(); ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==101); CHECK(g_wr_epoch==101);
    RESET(); S.ssbo[4][0].addr=base+15; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); S.ssbo[4][0].addr=base+16; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==101);
    RESET(); S.ssbo[4][0].size=0; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); S.ssbo[4][0].addr=UINT64_MAX-3; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); sh->rf.b[0].kind=NVN_RB_GMEM; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); sh->rf.b[0].kind=NVN_RB_IMG; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); sh->rf.b[0].reg=16; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); g_ixc_epoch=99; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==99);
    RESET(); g_ixc_sub=g_sub_serial+1; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); g_wr_epoch=102; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); g_ixc_n=2; g_ixc[1]=g_ixc[0]; g_ixc[1].ind=base+64; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); S.ssbo[4][0].addr=base; sh->sb_ro=1; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==101);
    RESET(); S.ssbo[4][0].addr=0; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==101);
    RESET(); g_ixc[0].itype=0; g_ixc[0].ibuf=base+64;
    S.ssbo[4][0]=(p_bind_buf){.addr=base+bytes-8,.size=4}; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); g_ixc[0].itype=0; S.ssbo[4][0].addr=base+19; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); g_ixc[0].itype=0; S.ssbo[4][0].addr=base+20; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==101);
    RESET(); g_ixc[0].ind=UINT64_MAX-3; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); g_ixc[0].ibuf=0x1234; ixc_mesh_written(pr,100); CHECK(g_ixc_epoch==100);
    RESET(); ixc_mesh_written(nil,100); CHECK(g_ixc_epoch==100);
    g_ixc_n=0; mtl_pool_del(mem); mtl_drain(); free(mem);
    printf("indirect cache ranges: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
