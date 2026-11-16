// The prebuild after the first frame (nvn_mtl.m, DM_PSO_PREBUILD_WHEN=after): the tier order (pairs whose shaders the prewarm lists saw first, pre_order with g_pre_seen),
// the workers' pause while the game needs the compiler (pre_pause_wait, g_pre_hot_ns), and the switch. No Metal compile is made
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)

int main(void) {
    @autoreleasepool {
        setenv("DM_PSO_PREBUILD_WHEN", "after", 1);
        CHECK(pre_when_after() == 1, "DM_PSO_PREBUILD_WHEN=after turns it on");

        // pairs: v1 has three pairs (the most: first without tiers), v2 one, v3 two
        NSArray<NSArray<NSString *> *> *pairs = @[@[@"v1", @"f1"], @[@"v1", @"f2"], @[@"v1", @"f3"], @[@"v2", @"f4"], @[@"v3", @"f5"], @[@"v3", @"f6"]];
        g_pre_seen = nil;
        NSArray<NSNumber *> *o = pre_order(pairs);
        CHECK(o[0].intValue == 0 && o[1].intValue == 1 && o[2].intValue == 2, "no tiers: the vertex shader with the most pairs first, as before");
        g_pre_seen = [NSSet setWithArray:@[@"v2", @"f4", @"v3", @"f6"]];
        o = pre_order(pairs);
        CHECK(o[0].intValue == 3 || o[0].intValue == 5, "tiers: a pair with both shaders seen comes first");
        CHECK((o[0].intValue == 3 && o[1].intValue == 5) || (o[0].intValue == 5 && o[1].intValue == 3), "both tier-1 pairs before any other");
        CHECK(o[2].intValue == 4, "then the pair with one shader seen");
        CHECK(o[3].intValue == 0 && o[4].intValue == 1 && o[5].intValue == 2, "then the rest, in the old order");
        g_pre_seen = nil;

        uint64_t t0 = bp_now(); pre_pause_wait();
        CHECK((bp_now() - t0) / 1e6 < 50, "nothing hot: no pause");
        atomic_store(&g_pre_hot_ns, bp_now()); t0 = bp_now(); pre_pause_wait(); double ms = (bp_now() - t0) / 1e6;
        CHECK(ms >= 1700 && ms < 3000, "a draw waited just now: the workers pause about 2 s");
        CHECK(g_pre_pauses == 1 && g_pre_paused_ns > 1500000000ull, "the pause is counted");
        atomic_store(&g_pre_hot_ns, bp_now()); atomic_store(&g_warm_stop, 1); t0 = bp_now(); pre_pause_wait();
        CHECK((bp_now() - t0) / 1e6 < 50, "the process ending: no pause");
    }
    printf("t_pso_prebuild_bg: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
