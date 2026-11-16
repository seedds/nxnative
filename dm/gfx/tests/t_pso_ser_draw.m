// A draw's own compile during an archive write (nvn_mtl.m, ser_compile_begin with tl_ser_draw): it waits at most DM_PSO_SER_DRAW_WAIT_MS (50 ms) for the write, once per
// write, then declines (returns 0, tl_ser_declined) and compiles nothing; any other compile waits for the write as before (the write and a compile must not overlap: #2513).
// The write is played by another thread holding ser_begin / ser_end; no Metal compile is made
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)

static void write_for(int ms) {   // a write on another thread, held for ms
    dispatch_semaphore_t in = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ ser_begin(); dispatch_semaphore_signal(in); usleep((useconds_t)ms * 1000); ser_end(); });
    dispatch_semaphore_wait(in, DISPATCH_TIME_FOREVER);
}
static double ms_since(uint64_t t0) { return (bp_now() - t0) / 1e6; }

int main(void) {
    @autoreleasepool {
        tl_ser_draw = 1;
        CHECK(ser_compile_begin() == 1, "no write: a draw's compile begins"); ser_compile_end();

        write_for(600);
        uint64_t t0 = bp_now(); int r = ser_compile_begin(); double w = ms_since(t0);
        CHECK(r == 0 && tl_ser_declined, "during a write: a draw's compile declines");
        CHECK(w >= 40 && w < 250, "it waited about DM_PSO_SER_DRAW_WAIT_MS (50 ms) first");
        CHECK(g_ser_draw_skips == 1, "the skip is counted");
        tl_ser_declined = 0; t0 = bp_now(); r = ser_compile_begin(); w = ms_since(t0);
        CHECK(r == 0 && w < 10, "a second draw in the same write declines at once");

        tl_ser_draw = 0; t0 = bp_now(); r = ser_compile_begin(); w = ms_since(t0);
        CHECK(r == 1 && w > 300, "any other compile waits for the write to end"); ser_compile_end();

        tl_ser_draw = 1; tl_ser_declined = 0;
        write_for(400);
        t0 = bp_now(); r = ser_compile_begin(); w = ms_since(t0);
        CHECK(r == 0 && w >= 40, "the next write: a draw waits once again before it declines");
        tl_ser_draw = 0; r = ser_compile_begin(); CHECK(r == 1, "a non-draw compile after it begins"); ser_compile_end();

        tl_ser_draw = 1; tl_ser_declined = 0;
        write_for(30);
        t0 = bp_now(); r = ser_compile_begin(); w = ms_since(t0);
        CHECK(r == 1 && !tl_ser_declined, "a write shorter than the wait: the draw's compile waits it out and begins"); if (r) ser_compile_end();
        CHECK(g_ser_compiles == 0, "every begun compile ended");
    }
    printf("t_pso_ser_draw: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
