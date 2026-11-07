// Incremental pool index: insertion, overlapping ranges, swap-removal and replacement.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; if (failed < 12) fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static uint32_t rng = 1234567;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void add(uintptr_t lo, uintptr_t size) { g_pools[g_npool++] = (pool_ent){.lo=lo,.hi=lo+size}; g_plast=-1; pidx_append(); }
static void remove_at(int i) { pidx_remove(i); g_pools[i]=g_pools[--g_npool]; g_plast=-1; }
static void verify(void) {
    pidx_sort(); unsigned char seen[4096]={0}; int valid=1;
    for(int j=0;j<g_npool;j++) { int i=g_pidx[j]; if(i<0||i>=g_npool||seen[i]++) {valid=0;break;} if(j && g_pools[g_pidx[j-1]].lo>g_pools[i].lo) valid=0; }
    CHECK(valid,"index is a sorted permutation");
    for(int q=0;q<24;q++) {
        uint64_t a=next()%32768,n=1+next()%192; int exists=0;
        for(int i=0;i<g_npool;i++) exists |= a>=g_pools[i].lo && a+n<=g_pools[i].hi;
        int hit=pool_find(a,n); CHECK((hit>=0)==exists,"lookup agrees with linear coverage oracle");
        if(hit>=0) CHECK(a>=g_pools[hit].lo && a+n<=g_pools[hit].hi,"chosen pool covers the whole query");
    }
}
int main(void) { @autoreleasepool {
    g_cap=4096; g_pools=calloc(g_cap,sizeof *g_pools); pidx_sort();
    for(int i=0;i<1200;i++) add(next()%32768,1+next()%512);
    verify(); uint64_t sorts=g_pidx_sorts;
    for(int step=0;step<1000;step++) {
        int i=next()%g_npool;
        // Mirror a large-pool chunk growing: append replacement, swap-remove old.
        if(step%3==0) { uintptr_t lo=g_pools[i].lo; add(lo,1024); remove_at(i); }
        else { remove_at(i); add(next()%32768,1+next()%512); }
        verify();
    }
    CHECK(g_pidx_sorts==sorts,"pool churn never re-sorts unchanged entries");
    while(g_npool) remove_at(next()%g_npool);
    verify(); add(17,33); verify(); remove_at(0); verify();
    printf("pool index: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
