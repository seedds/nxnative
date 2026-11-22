// Residency by command buffer (nvn_mtl.m res_name, PR #2715): no residency set is attached to either queue, and every command buffer committed on g_q or g_q4 names g_res and the
// present snapshots' set (useResidencySet) after its last begin; the present's also its drawable's layer set. Real Metal: the queues' addResidencySet and commits and the command
// buffers' begin, useResidencySet and commit are wrapped, so the calls each command buffer gets are counted whichever path made it (calls, not what the driver keeps resident).
// Checks, each run with the encode thread off and on (DM_NVN_ENC_THREAD): no set on a queue; a submission with a cut and an upload batch (three command buffers); a present snapshot
// and its present through the display link; an offscreen present (present_offscreen_m4); a classic command buffer (q_cb, by mtl_drain); one each. That no other path ends a command
// buffer or makes a classic one is dm/tools/tests/test_residency_naming.py's. A wait on an indirect draw's count (mdi_wait) and the warm-up pass (warm_start) are not driven here
#include "../nvn/nvn_mtl.m"
#import <objc/runtime.h>
@interface FakeLinkUpdate : NSObject
@property id<CAMetalDrawable> drawable;
@property CFTimeInterval targetPresentationTimestamp;
@end
@implementation FakeLinkUpdate
@end
static CAMetalLayer *g_test_layer;
void *front_metal_layer(void) { return (__bridge void *)g_test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)

// What the wrappers saw, under g_t_mu: command buffers that named g_res and those that named g_snap_res since their last begin (pointers; a commit takes its buffer out); since mark(),
// commits on each queue ([0] g_q, [1] g_q4), those whose buffer had not named both, and namings of g_t_layer_set (the test's drawable layer); and sets attached to a queue
static pthread_mutex_t g_t_mu = PTHREAD_MUTEX_INITIALIZER;
static CFMutableSetRef g_t_named, g_t_named_snap;
static uint64_t g_t_commits[2], g_t_unnamed[2], g_t_layer_named; static int g_t_attached;
static id g_t_layer_set;
static void t_committed(int q4, void *cb) {
    pthread_mutex_lock(&g_t_mu); g_t_commits[q4]++; if (!CFSetContainsValue(g_t_named, cb) || !CFSetContainsValue(g_t_named_snap, cb)) g_t_unnamed[q4]++;
    CFSetRemoveValue(g_t_named, cb); CFSetRemoveValue(g_t_named_snap, cb); pthread_mutex_unlock(&g_t_mu);
}
static IMP o_use_cl, o_use_4, o_begin_4, o_commit_cl, o_commit_4, o_commit_4o, o_add_cl, o_add_4;
static void h_use(id self, SEL s, id set, IMP o) {
    pthread_mutex_lock(&g_t_mu);
    if (set == g_res) CFSetAddValue(g_t_named, (__bridge void *)self);
    if (set == g_snap_res) CFSetAddValue(g_t_named_snap, (__bridge void *)self);
    if (set && set == g_t_layer_set) g_t_layer_named++;
    pthread_mutex_unlock(&g_t_mu);
    ((void (*)(id, SEL, id))o)(self, s, set);
}
static void h_use_cl(id self, SEL s, id set) { h_use(self, s, set, o_use_cl); }
static void h_use_4(id self, SEL s, id set) { h_use(self, s, set, o_use_4); }
static void h_begin_4(id self, SEL s, id al) {   // a begin drops the buffer's sets
    pthread_mutex_lock(&g_t_mu); CFSetRemoveValue(g_t_named, (__bridge void *)self); CFSetRemoveValue(g_t_named_snap, (__bridge void *)self); pthread_mutex_unlock(&g_t_mu);
    ((void (*)(id, SEL, id))o_begin_4)(self, s, al);
}
static void h_commit_cl(id self, SEL s) {
    if ([(id<MTLCommandBuffer>)self commandQueue] == g_q) t_committed(0, (__bridge void *)self);
    ((void (*)(id, SEL))o_commit_cl)(self, s);
}
static void h_commit_4(id self, SEL s, __unsafe_unretained id const *b, NSUInteger n) {
    if (self == g_q4) for (NSUInteger i = 0; i < n; i++) t_committed(1, (__bridge void *)b[i]);
    ((void (*)(id, SEL, __unsafe_unretained id const *, NSUInteger))o_commit_4)(self, s, b, n);
}
static void h_commit_4o(id self, SEL s, __unsafe_unretained id const *b, NSUInteger n, id o) {
    if (self == g_q4) for (NSUInteger i = 0; i < n; i++) t_committed(1, (__bridge void *)b[i]);
    ((void (*)(id, SEL, __unsafe_unretained id const *, NSUInteger, id))o_commit_4o)(self, s, b, n, o);
}
static void h_add(id self, SEL s, id set, IMP o) {
    pthread_mutex_lock(&g_t_mu); g_t_attached++; pthread_mutex_unlock(&g_t_mu);
    ((void (*)(id, SEL, id))o)(self, s, set);
}
static void h_add_cl(id self, SEL s, id set) { h_add(self, s, set, o_add_cl); }
static void h_add_4(id self, SEL s, id set) { h_add(self, s, set, o_add_4); }
static Method g_hooked[8]; static int g_nhooked;
static void hook(Class c, SEL s, IMP h, IMP *o) {   // a method two classes share (a common superclass) is wrapped once
    Method m = class_getInstanceMethod(c, s);
    if (!m) { checks++; failed++; printf("%s has %s: FAIL\n", class_getName(c), sel_getName(s)); return; }
    for (int i = 0; i < g_nhooked; i++) if (g_hooked[i] == m) return;
    g_hooked[g_nhooked++] = m; *o = method_setImplementation(m, h);
}
static void hooks(void) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue]; id<MTL4CommandQueue> q4 = [d newMTL4CommandQueue];
    Class ccl = [[q commandBuffer] class], c4 = [[d newCommandBuffer] class];
    hook(ccl, @selector(useResidencySet:), (IMP)h_use_cl, &o_use_cl);
    hook(c4, @selector(useResidencySet:), (IMP)h_use_4, &o_use_4);
    hook(c4, @selector(beginCommandBufferWithAllocator:), (IMP)h_begin_4, &o_begin_4);
    hook(ccl, @selector(commit), (IMP)h_commit_cl, &o_commit_cl);
    hook([q4 class], @selector(commit:count:), (IMP)h_commit_4, &o_commit_4);
    hook([q4 class], @selector(commit:count:options:), (IMP)h_commit_4o, &o_commit_4o);
    hook([q class], @selector(addResidencySet:), (IMP)h_add_cl, &o_add_cl);
    hook([q4 class], @selector(addResidencySet:), (IMP)h_add_4, &o_add_4);
} }

static void mark(void) { pthread_mutex_lock(&g_t_mu); memset(g_t_commits, 0, sizeof g_t_commits); memset(g_t_unnamed, 0, sizeof g_t_unnamed); g_t_layer_named = 0; pthread_mutex_unlock(&g_t_mu); }
static void expect(const char *what, int q4, uint64_t want) {   // the command buffers committed on the queue since mark(): as many as the path commits, and each named both sets
    pthread_mutex_lock(&g_t_mu); uint64_t c = g_t_commits[q4], u = g_t_unnamed[q4]; pthread_mutex_unlock(&g_t_mu);
    char name[200]; printf("%s: %llu command buffers committed on %s, %llu without the set named\n", what, (unsigned long long)c, q4 ? "g_q4" : "g_q", (unsigned long long)u);
    snprintf(name, sizeof name, "%s: commits %llu command buffer%s on %s", what, (unsigned long long)want, want == 1 ? "" : "s", q4 ? "g_q4" : "g_q"); CHECK(name, c == want);
    snprintf(name, sizeof name, "%s: each one names the residency sets", what); CHECK(name, u == 0);
}
static CAMetalLayer *offscreen_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(8, 8); return l;
}

int main(void) { @autoreleasepool {
    alarm(30); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_SNAP", "2", 1); setenv("DM_NVN_CUT_BUSY", "0", 1);   // a snapshot ring; every cut commits, whatever the GPU is still running
    g_t_named = CFSetCreateMutable(NULL, 0, NULL); g_t_named_snap = CFSetCreateMutable(NULL, 0, NULL);
    hooks();
    mtl_init();
    printf("encode thread %s\n", rec_on() ? "on" : "off");
    CHECK("both host residency sets made", g_res && g_snap_res);
    { const char *e = getenv("DM_NVN_ENC_THREAD"); CHECK("the encode thread as DM_NVN_ENC_THREAD asks", e && rec_on() == (atoi(e) > 0)); }

    // A submission: a segment cut off at a pass boundary, an upload batch, then its last command buffer
    state_reset(); memset(&R, 0, sizeof R); mark();
    mtl_sub_begin();
    uint64_t cuts = g_cuts; g_stat_draw += (uint64_t)cut_draws(); sub_cut();
    CHECK("the cut committed a segment", g_cuts == cuts + 1);
    up_begin(); up_flush();
    CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    expect("a submission with a cut and an upload batch", 1, 3);

    // A present through the display link, with present snapshots: the snapshot's copy, then the present of the snapshot to a drawable
    g_test_layer = offscreen_layer(); mtl_present_layer(8, 8);
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1;   // display-link mode without a link thread (t_present_alloc)
    NvMetalDisplayLinkDelegate *link = [NvMetalDisplayLinkDelegate new];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> am = [g_dev newTextureWithDescriptor:d];   // the game's texture: what it holds is not looked at
    tex_t a = {.magic=MAGIC('T','E','X',0), .serial=93001};
    a.d.width = a.d.height = 8; a.d.depth = a.d.levels = a.d.samples = 1;
    a.d.target = NVN_TEXTURE_TARGET_2D; a.d.format = NVN_FORMAT_RGBA8;
    serial_put(a.serial, am); res_add(am); res_commit(); mark();
    uint64_t copies = g_snap_v; present_enqueue(&a, 1. / 60, NULL, 0, NULL); snap_commit_wait(g_snap_v);
    CHECK("the present was copied into a snapshot", g_snap_v == copies + 1 && [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    expect("a present snapshot", 1, 1);
    CAMetalLayer *l = offscreen_layer(); g_t_layer_set = l.residencySet;
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = [l nextDrawable]; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016;
    uint64_t done = g_pq_done; mark();
    [link metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int presented = !g_pq_inflight && g_pq_done > done; pthread_mutex_unlock(&g_pq_mu);
    CHECK("the present completed", presented);
    expect("a present through the display link", 1, 1);
    CHECK("the present names its drawable's layer set", g_t_layer_named == 1);

    // An offscreen present (a whole-window screenshot's pass)
    d.pixelFormat = MTLPixelFormatBGRA8Unorm; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> out = [g_dev newTextureWithDescriptor:d]; res_add(out); res_commit();
    static const int whole[4] = {0, 0, 8, 8}; mark();
    CHECK("the offscreen present completed", present_offscreen_wait(am, whole, out) != PRESENT_FAILED);
    expect("an offscreen present", 1, 1);

    // A classic command buffer (q_cb): mtl_drain commits one and waits for it
    mark(); mtl_drain();
    expect("a classic command buffer", 0, 1);

    CHECK("no residency set is attached to a queue", g_t_attached == 0);
    printf("t_res_cb (encode thread %s): %d checks, %d failed\n", rec_on() ? "on" : "off", checks, failed);
    return failed != 0;
} }
