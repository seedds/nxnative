// Full production renderer; no owner/cache function is stubbed. No Metal device or game files.
// Install beside ../nvn/nvn_mtl.m in a private mirrored tree. Define TEST_GHOST_LRU
// only when that included source declares g_ghost_use; it enables read-only invariants.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static unsigned held(nvn_hmap *m,id o) { uintptr_t k=(uintptr_t)(__bridge void *)o; return (unsigned)(uintptr_t)nvn_hm_get(m,&k); }
static int ghost(uint64_t k) { return nvn_hm_get(&g_ghost,&k)!=NULL; }
static void list_check(void) {
#ifdef TEST_GHOST_LRU
    unsigned n=0; ghost_use *prev=NULL;
    int ok=g_ghost_use.n==g_ghost.n;
    for(ghost_use *u=g_ghost_lru;u;prev=u,u=u->next) {
        if(++n>g_ghost_use.n) { ok=0; break; }
        ok &= u->prev==prev && nvn_hm_get(&g_ghost_use,&u->serial)==u && ghost(u->serial);
        if(prev) ok &= prev->frame<=u->frame && prev->sub<=u->sub;
    }
    CHECK(ok && n==g_ghost.n && prev==g_ghost_mru,"LRU map/list/ghost entries agree, links and timestamps ordered");
#endif
}
// Empty real submission. The queue and command buffer stay nil: Objective-C messages
// do nothing, while the production commit path and its ghost trim execute unchanged.
// No feedback can fire on a nil queue; publish the empty completion explicitly.
static uint64_t commit_empty(void) {
    g_sub_pool=objc_autoreleasePoolPush(); atomic_store(&g_sub_on,1);
    uint64_t fence=mtl_sub_commit();
    atomic_fetch_sub(&g_seg_inflight,1);
    post_complete(fence,0,0); defer_drain();
    return fence;
}
@interface GhostTestView : NSObject
@property(nonatomic,strong) id parentTexture;
@property(nonatomic) MTLPixelFormat pixelFormat;
@end
@implementation GhostTestView
- (BOOL)conformsToProtocol:(Protocol *)p { return p==@protocol(MTLTexture) || [super conformsToProtocol:p]; }
@end
int main(void) { @autoreleasepool {
    setenv("DM_NVN_ENC_THREAD","0",1); setenv("DM_AUDIO","0",1);
    enum { N=1024, FRAMES=32, SUBS=25 }; uint64_t base=100000;
    NSMutableArray *objects=[NSMutableArray array];
    atomic_store(&g_cur_frame,1);
    for(unsigned i=0;i<N;i++) { id o=[NSObject new]; [objects addObject:o]; tex_put(base+i,o); }
    uint64_t puts=g_ghost_puts, notes=atomic_load(&g_ser_gen), misses=0;
    for(unsigned f=0;f<FRAMES;f++) {
        atomic_store(&g_cur_frame,f+1);
        for(unsigned sub=0;sub<SUBS;sub++) {
            for(unsigned i=0;i<N;i++) misses += tex_get(base+i)!=objects[i];
            commit_empty();
        }
    }
    printf("working_set=%d frames=%d submissions_per_frame=%d misses=%llu puts_delta=%llu\n",N,FRAMES,SUBS,(unsigned long long)misses,(unsigned long long)(g_ghost_puts-puts));
    CHECK(!misses,"1024 finalized serials used each frame remain resolvable across 800 submissions");
    CHECK(g_ghost.n==N && g_ghost_puts==puts,"active working set exceeds 512 without re-put churn");
    CHECK(atomic_load(&g_ser_gen)==notes,"active lookups/commits produce no invalidation notes"); list_check();
    uint64_t k=base+N-1; id old=objects[N-1], replacement=[NSObject new];
    mtl_tex_pin(k); mtl_tex_pin(k); unsigned n=g_ghost.n; puts=g_ghost_puts;
    tex_put(k,replacement);
    CHECK(g_ghost.n==n && g_ghost_puts==puts,"replacement changes neither cardinality nor insertion count");
    CHECK(!held(&g_ghost_held,old) && held(&g_ghost_held,replacement)==1 && held(&g_by_serial_held,replacement)==1,"replacement transfers ghost and pin ownership counts");
    atomic_fetch_add(&g_cur_frame,15); n=g_ghost.n; commit_empty();
    CHECK(g_ghost.n==n,"15 idle frames do not evict before 16 or 1024 submissions");
    atomic_fetch_add(&g_cur_frame,1); n=g_ghost.n;
    tex_put(900000,[NSObject new]);
    CHECK(n+1-g_ghost.n==2,"a put trims exactly two eligible idle extras");
    n=g_ghost.n; commit_empty();
    CHECK(n-g_ghost.n==(n>512 ? MIN(32,n-512) : 0),"a commit trims at most 32 idle extras");
    for(int i=0;i<40;i++) commit_empty();
    CHECK(g_ghost.n==512,"idle extras drain to 512 without additional puts");
    CHECK(!ghost(base) && !held(&g_ghost_held,objects[0]),"oldest idle serial and its held count leave together");
    // Make the pinned entry older than enough fresh entries to force its eviction.
    atomic_fetch_add(&g_cur_frame,16);
    for(unsigned i=0;i<600;i++) tex_put(1000000+i,[NSObject new]);
    for(int i=0;i<40;i++) commit_empty();
    CHECK(!ghost(k) && tex_get(k)==replacement,"an evicted ghost still resolves through its pin");
    mtl_tex_unpin(k); CHECK(tex_get(k)==replacement,"one unpin preserves the other pin reference");
    mtl_tex_unpin(k); CHECK(tex_get(k)==nil && !held(&g_by_serial_held,replacement),"last unpin drops the pin holder"); list_check();
    // The frame stands still: submission threshold independently expires idle extras.
    for(unsigned i=0;i<600;i++) tex_put(2000000+i,[NSObject new]);
    for(uint32_t i=0;i<g_ghost.cap;i++) if(g_ghost.vals[i]) { uint64_t serial; memcpy(&serial,g_ghost.keys+(size_t)i*g_ghost.klen,8); (void)tex_get(serial); }
    uint64_t sub0=atomic_load(&g_fence)+1;
    atomic_store(&g_fence,sub0+1022); n=g_ghost.n; commit_empty();
    CHECK(g_ghost.n==n,"1023 idle submissions alone do not evict");
    n=g_ghost.n; commit_empty();
    CHECK(n-g_ghost.n==MIN(32,n-512),"1024 idle submissions expire extras with no frame advance"); list_check();
    // Exercise the actual stale-view path in tex_mtl_, using an NSObject view test
    // double. It never creates a device; after deletion the nil device cannot rebuild.
    uint64_t vk=3000000; GhostTestView *v=[GhostTestView new]; v.parentTexture=[NSObject new]; v.pixelFormat=MTLPixelFormatRGBA8Unorm;
    tex_put(vk,v); tex_t t={.magic=MAGIC('T','E','X',0),.serial=vk}; t.d.width=t.d.height=4; t.d.depth=t.d.levels=1; t.d.target=NVN_TEXTURE_TARGET_2D; t.d.format=NVN_FORMAT_RGBA8;
    (void)tex_mtl_(&t);
    CHECK(!ghost(vk) && !held(&g_ghost_held,v),"a stale finalized view drops its ghost entry and ownership count"); list_check();
    tex_put(vk,[NSObject new]); CHECK(ghost(vk),"a removed stale serial can be inserted again"); list_check();
    printf("ghost_lru: %d checks, %d failed\n",checks,failed);
    return failed ? 1 : 0;
} }
