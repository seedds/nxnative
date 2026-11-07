// Runtime material-handle proof: pending writes and storage aliases must refuse.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while(0)
int main(void) { @autoreleasepool {
    mtl_init();
    void *mem = NULL; CHECK(!posix_memalign(&mem, getpagesize(), 65536));
    memset(mem, 0, 65536); mtl_pool_add(mem, 65536); mtl_sub_begin();
    uint64_t addr = (uint64_t)mem; *(uint32_t *)mem = 7;
    S.ubo[1][1] = (p_bind_buf){.addr=addr, .size=16};
    S.ssbo[1][2] = (p_bind_buf){.addr=addr, .size=16};
    uint32_t value = 0;
    CHECK(fb_handle_word(NULL, 1, 1, 0, &value) && value == 7);
    CHECK(fb_handle_word(NULL, 0, 2, 0, &value) && value == 7);
    CHECK(!fb_handle_word(NULL, 0, 2, 13, &value));
    CHECK(!fb_handle_word(NULL, 1, 16, 0, &value));
    CHECK(!fb_handle_word(NULL, 1, 0, 0, &value));
    CHECK(!fb_handle_word(NULL, 1, 1, UINT64_MAX, &value));
    S.ubo[1][3]=(p_bind_buf){.addr=UINT64_MAX-2,.size=16};
    CHECK(!fb_handle_word(NULL,1,3,0,&value));
    sbw_note(addr, 4, SBW_GPU);
    CHECK(!fb_handle_word(NULL, 1, 1, 0, &value)); // even an already cached word
    pend_reset(); pend_add(addr, 4, 2);
    CHECK(!fb_handle_word(NULL, 1, 1, 0, &value));
    pend_reset(); pend_add(addr, 4, 3);
    CHECK(!fb_handle_word(NULL, 1, 1, 0, &value));
    pend_reset(); gw_put(addr, addr+4, g_done+1000, g_done);
    CHECK(!fb_handle_word(NULL, 1, 1, 0, &value));
    memset(g_gwx,0,sizeof g_gwx); memset(g_gw,0,sizeof g_gw);
    pend_reset(); value=123; mtl_sub_update(&value,addr,4);
    CHECK(fb_handle_word(NULL,1,1,0,&value) && value==123);
    uint16_t last=456; mtl_sub_update(&last,addr,2);
    CHECK(fb_handle_word(NULL,1,1,0,&value) && value==456);
    CHECK(*(uint32_t *)mem==7); // proof read the queued bytes, not stale CPU memory
    sbw_note(addr,4,SBW_GPU); CHECK(!fb_handle_word(NULL,1,1,0,&value));
    // Finish real deferred writes before exercising texture classification.
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK(!mtl_last_commit_error());
    mtl_sub_begin();
    tex_t depth={.serial=8801,.mem=(char *)mem+16384}; depth.L.size=4096;
    tex_t other={.serial=8802,.mem=(char *)mem+32768}; other.L.size=4096;
    R.rt.depth=&depth; mtl_reg_tex(1,&depth,NULL); mtl_reg_tex(2,&other,NULL);
    NvProg *pr=[NvProg new]; pr->sh[1]=[NvShader new];
    CHECK(fb_pool_reads_depth(pr)); // absent proof
    nvn_hp_prog *p=pr->sh[1]->rf.handles=calloc(1,sizeof *p);
    *p=(nvn_hp_prog){.n=1,.nroots=1,.roots={0},.nodes={{NVN_HP_CONST,2}}};
    CHECK(!fb_pool_reads_depth(pr));
    p->nodes[0].a=1; CHECK(fb_pool_reads_depth(pr));
    p->nodes[0].a=0; CHECK(!fb_pool_reads_depth(pr));
    p->nodes[0].a=1048575; CHECK(!fb_pool_reads_depth(pr)); // clamped dummy
    p->nodes[0].a=1; S.tex_base=1; CHECK(!fb_pool_reads_depth(pr)); S.tex_base=0;
    S.tex_base=-1; CHECK(fb_pool_reads_depth(pr)); S.tex_base=0;
    p->nodes[0].a=2;
    other.mem=depth.mem; mtl_reg_tex(2,&other,NULL); CHECK(fb_pool_reads_depth(pr));
    other.mem=(char *)mem+32768; other.L.size=0; mtl_reg_tex(2,&other,NULL); CHECK(fb_pool_reads_depth(pr));
    other.L.size=4096; other.mem=NULL; mtl_reg_tex(2,&other,NULL); CHECK(fb_pool_reads_depth(pr));
    other.mem=(char *)mem+32768; mtl_reg_tex(2,&other,NULL);
    S.tf_on=1; CHECK(fb_pool_reads_depth(pr)); S.tf_on=0;
    for(int st=0;st<5;st++) {
        if(!pr->sh[st])pr->sh[st]=[NvShader new];
        pr->sh[st]->rf.nb=1; pr->sh[st]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1};
        CHECK(fb_pool_reads_depth(pr));
        pr->sh[st]->rf.b[0].kind=NVN_RB_SBUFU; CHECK(fb_pool_reads_depth(pr));
        pr->sh[st]->rf.b[0].kind=NVN_RB_GMEM; CHECK(fb_pool_reads_depth(pr));
        pr->sh[st]->rf.b[0].kind=NVN_RB_IMG; CHECK(fb_pool_reads_depth(pr));
        pr->sh[st]->rf.b[0].kind=NVN_RB_GMEML; CHECK(!fb_pool_reads_depth(pr));
        pr->sh[st]->rf.nb=0;
    }
    *p=(nvn_hp_prog){.n=2,.nroots=1,.roots={1},.nodes={{NVN_HP_CONST,0},{NVN_HP_CB,1,0,0}}};
    S.ubo[1][1].size=0; CHECK(fb_pool_reads_depth(pr));
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK(!mtl_last_commit_error());
    printf("t_handle_pool: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
