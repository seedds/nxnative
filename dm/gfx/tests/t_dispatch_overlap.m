// The game's dispatches in the copy encoder (dispatch_enc, DM_NVN_DISPATCH_OVERLAP): two with nothing between them and no barrier of the game's run side by side; a game barrier,
// a copy between them or a host dispatch keeps the barrier (counted by the copy encoder's barriers, g_blit_barriers)
#import <Foundation/Foundation.h>
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

int main(void) { @autoreleasepool {
    setenv("DM_NVN_DISPATCH_OVERLAP", "1", 1);
    mtl_init();
    mtl_sub_begin();
    uint64_t b0 = g_blit_barriers;
    dispatch_enc(1);
    CHECK(g_blit_barriers == b0, "the first dispatch in the encoder needs no barrier");
    dispatch_enc(1); dispatch_enc(1);
    CHECK(g_blit_barriers == b0 && g_disp_beside == 2, "dispatches with nothing between them run side by side");
    g_cs_gbar = 1; dispatch_enc(1);
    CHECK(g_blit_barriers == b0 + 1 && !g_cs_gbar, "a game barrier between them keeps the barrier");
    blit_buf(0, 0, 0x1000, 0x2000); dispatch_enc(1);
    CHECK(g_blit_barriers == b0 + 3, "a copy after a dispatch waits for it, and the dispatch after the copy waits for the copy");
    uint64_t b1 = g_blit_barriers; dispatch_enc(0);
    CHECK(g_blit_barriers == b1 + 1, "a host dispatch is ordered as a copy is");
    uint64_t b2 = g_blit_barriers; dispatch_enc(1);
    CHECK(g_blit_barriers == b2 + 1, "a game dispatch after a host dispatch waits for it");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
    printf("t_dispatch_overlap: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
