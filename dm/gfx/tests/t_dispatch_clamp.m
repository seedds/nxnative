// The indirect dispatch clamp (dpc_clamp_enc, ixc.metal dpc_clamp) on the GPU: a GPU-written record of at most 2^22 threadgroups in all passes unchanged, a 1D one of more
// than 65535 groups too (it was cut to 65535, a partial dispatch); one of more dispatches nothing, also when its product does not fit 64 bits
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    setenv("DM_NVN_ENC_THREAD", "0", 1);
    mtl_init();
    id<MTLBuffer> rb = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(rb);
    static const uint32_t cases[][4] = {   // x, y, z, passes unchanged
        {64, 64, 1, 1}, {100000, 1, 1, 1}, {1u << 22, 1, 1, 1}, {2048, 2048, 1, 1}, {1, 1, 70000, 1},
        {(1u << 22) + 1, 1, 1, 0}, {4096, 4096, 1, 0}, {65536, 65536, 2, 0}, {0xffffffffu, 0xffffffffu, 0xffffffffu, 0}, {3, 0x80000000u, 0x80000000u, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        memcpy(rb.contents, cases[i], 12);
        mtl_sub_begin();
        id ce = dispatch_enc(); __strong id<MTLBuffer> ob = nil; size_t oo = 0; uint64_t og = 0;
        int ok = dpc_clamp_enc(ce, rb.gpuAddress, &ob, &oo, &og);
        CHECK(ok, "the clamp kernel is encoded");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
        if (!ok || !ob) continue;
        const uint32_t *out = (const uint32_t *)((const uint8_t *)ob.contents + oo);
        char label[128]; snprintf(label, sizeof label, "%u x %u x %u: %s", cases[i][0], cases[i][1], cases[i][2], cases[i][3] ? "unchanged" : "nothing dispatched");
        CHECK(cases[i][3] ? !memcmp(out, cases[i], 12) : (!out[0] && !out[1] && !out[2]), label);
    }
    printf("t_dispatch_clamp: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
