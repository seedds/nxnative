// Interleaved completed/pending fences used to shift a large array once per
// retired object. Check the linear partition, retention and release ordering.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static unsigned destroyed;
static uint32_t last_id = UINT32_MAX;
@interface RetiredProbe : NSObject { @public uint32_t number; } @end
@implementation RetiredProbe
- (void)dealloc { assert(number < last_id); last_id = number; destroyed++; }
@end

int main(void) { @autoreleasepool {
    enum { N = 100000 };
    for (uint32_t i = 0; i < N; i++) {
        RetiredProbe *p = [RetiredProbe new]; p->number = i;
        g_fence = i % 2 ? 9 : 0;
        defer_obj_unref(p);
    }
    assert(g_deferred.count == N && destroyed == 0);
    g_done = 1; uint64_t start = bp_now(); defer_drain();
    assert(g_deferred.count == N / 2 && destroyed == N / 2);
    for (NSUInteger i = 0; i < g_deferred.count; i++) {
        assert(g_deferred[i]->fence == 10);
        assert(((RetiredProbe *)g_deferred[i]->object)->number == 2 * i + 1);
    }
    printf("interleaved deferred release: %u entries, %.1f ms\n", N, (bp_now() - start) / 1e6);
    last_id = UINT32_MAX; g_done = 10; defer_drain();
    assert(g_deferred.count == 0 && destroyed == N);
    defer_drain(); assert(destroyed == N);
    puts("deferred fence partition, retention and ordering PASS");
} return 0; }
