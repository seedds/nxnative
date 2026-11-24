// A direct grid with any zero dimension does no work, even with no program bound.
// It must leave pending clears, write epochs and encoders alone. An indirect
// argument address is not a direct zero grid: its dimensions belong to the GPU.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int main(void) { @autoreleasepool {
    mtl_init();
    uint64_t writes = g_wr_epoch, indices = g_mdx_epoch;
    const uint32_t dims[][3] = {{0, 8, 1}, {8, 0, 1}, {8, 8, 0}, {0, 0, 1}};
    for (unsigned i = 0; i < sizeof dims / sizeof *dims; i++) {
        p_dispatch d = {0}; d.x = dims[i][0]; d.y = dims[i][1]; d.z = dims[i][2];
        dispatch(&d);
        if (g_wr_epoch != writes || g_mdx_epoch != indices || g_cb || g_enc || g_blit) {
            fprintf(stderr, "FAIL: empty grid changed replay state\n"); return 1;
        }
    }
    puts("t_dispatch_zero: four zero-dimensional grids leave replay state untouched");
    return 0;
} }
