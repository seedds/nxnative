// Invalid completion timestamps must not poison frame or upload timing accumulators.
// Exercise the production helpers directly: no GPU fault or game is needed.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    double now = CACurrentMediaTime(), start = now - 2, end = now - 1;
    uint64_t frame = 1000, t0 = 0, t1 = 0;
    CHECK(gpu_time_valid(start, end) == 1, "elapsed ordered timestamps are valid");
    const double bad[][2] = {
        {0, end}, {-1, end}, {end, start}, {end, end},
        {NAN, end}, {start, NAN}, {INFINITY, INFINITY}, {start, INFINITY},
        {start, now + 3600}, {start, start + 6148483371.4372},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++, frame++) {
        CHECK(!gpu_time_valid(bad[i][0], bad[i][1]), "invalid endpoints rejected by shared predicate");
        gpu_tag_begin(frame);
        t1 = 123;
        CHECK(mtl_gpu_ms(frame) == -1, "pending completion stays unavailable");
        CHECK(mtl_gpu_done(frame, &t1) == 0, "pending completion keeps mtl_gpu_done running");
        gpu_tag_done(frame, bad[i][0], bad[i][1]);
        CHECK(mtl_gpu_ms(frame) == 0, "invalid timestamps contribute no duration");
        CHECK(!mtl_gpu_span(frame, &t0, &t1), "invalid timestamps contribute no span");
        t1 = 123;
        CHECK(mtl_gpu_done(frame, &t1) == 1 && t1 == 0, "invalid timestamps leave mtl_gpu_done end at 0");
        CHECK(atomic_load(&g_gpu[frame % GRING].pending) == 0, "invalid completion still retires");
    }
    gpu_tag_begin(frame); gpu_tag_begin(frame); gpu_tag_begin(frame);
    gpu_tag_done(frame, start, end);
    gpu_tag_done(frame, start, now + 3600);
    t1 = 123;
    CHECK(mtl_gpu_ms(frame) == -1, "invalid completion does not retire other work");
    CHECK(mtl_gpu_done(frame, &t1) == 0, "remaining work keeps mtl_gpu_done running");
    gpu_tag_done(frame, start - 1, start);
    CHECK(fabs(mtl_gpu_ms(frame) - 2000) < 0.001, "mixed frame retains only valid work");
    CHECK(mtl_gpu_span(frame, &t0, &t1), "valid work retains its span");
    CHECK(t0 == (uint64_t)((start - 1) * 1e9) && t1 == (uint64_t)(end * 1e9), "span excludes invalid endpoint");
    t1 = 0;
    CHECK(mtl_gpu_done(frame, &t1) == 1 && t1 == (uint64_t)(end * 1e9), "mtl_gpu_done retains latest valid end");
    frame++;
    gpu_tag_begin(frame); gpu_tag_begin(frame + GRING);
    gpu_tag_done(frame, start, end);
    CHECK(mtl_gpu_ms(frame + GRING) == -1, "late completion cannot retire a reused ring slot");
    gpu_tag_done(frame + GRING, start, end);
    CHECK(fabs(mtl_gpu_ms(frame + GRING) - 1000) < 0.001, "new ring slot retains its own duration");
    printf("t_gpu_timing: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
