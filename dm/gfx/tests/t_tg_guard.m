// The threadgroup-size guard (nvn_mtl.m tg_check), on the GPU. A compute pipeline whose maxTotalThreadsPerThreadgroup is below the shader's declared threadgroup size:
//  - 512 threads on a pipeline built with a maximum of 256: the dispatch is rebuilt to fit and runs every thread;
//  - 2048 threads (over the device's 1024): the dispatch is skipped and writes nothing.
// Without the guard both dispatches go to Metal with more threads than the pipeline takes (the debug layer stops the test; without it the work is partly done).
// Every kernel has a fixed loop count (no unbounded loop on the GPU)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { THREADS = 16384, REGION = THREADS * 4 };
static uint32_t spin_ref(uint32_t i) { uint32_t x = i; for (uint32_t k = 0; k < 64u; ++k) x = x * 1664525u + 1013904223u; return x | 1u; }
static int submit(void) { uint64_t f = mtl_sub_commit(); int ok = mtl_wait(f, 5000000000ull); mtl_drain(); return ok && !mtl_last_commit_error(); }
static void run(prog_t *pg, uint64_t out, int groups) {
    mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
    S.cprog = pg; S.ssbo[5][0] = (p_bind_buf){.addr = out, .size = REGION};
    p_dispatch dp = {.x = groups, .y = 1, .z = 1}; dispatch(&dp);
}

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A1 { device uint *out [[id(0)]]; };\n"
        "kernel void spin(constant A1 &a [[buffer(0)]], uint i [[thread_position_in_grid]]) { uint x = i; for (uint k = 0; k < 64u; ++k) x = x * 1664525u + 1013904223u; a.out[i] = x | 1u; }\n"
        options:nil error:&error];
    CHECK(lib != nil, "the synthetic compute shader compiles"); if (!lib) return 1;
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 3 * REGION)) return 1;
    memset(mem, 0, 3 * REGION); mtl_pool_add(mem, 3 * REGION);
    uint64_t A = (uint64_t)mem, B = A + REGION, C = A + 2 * REGION;
    // One shader per case, each with its pipeline built at a maximum of 256 threads (as Metal does for a kernel that needs many registers), declaring threadgroups of tg threads
    const struct { int tg; uint64_t out; int groups; int runs; const char *what; } cases[] = {
        {512, A, THREADS / 512, 1, "512 threads on a pipeline of at most 256: rebuilt to fit, every thread runs"},
        {2048, B, 4, 0, "2048 threads (over the device's 1024): not dispatched, nothing written"},
        {64, C, THREADS / 64, 1, "64 threads, within the maximum: runs as before"},
    };
    for (int i = 0; i < 3; i++) {
        prog_t pg = {.serial = 970101 + (uint64_t)i}; NvProg *program = [NvProg new]; NvShader *cs = [NvShader new];
        program->sh[5] = cs; cs->stage = 5; cs->fn = [lib newFunctionWithName:@"spin"]; cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
        snprintf(cs->key, sizeof cs->key, "spin%d", i);
        cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 0};
        cs->rf.tg[0] = (uint16_t)cases[i].tg; cs->rf.tg[1] = cs->rf.tg[2] = 1;
        serial_put(pg.serial, program);
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = cs->fn; d.maxTotalThreadsPerThreadgroup = 256;
        id<MTLComputePipelineState> low = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
        CHECK(low && low.maxTotalThreadsPerThreadgroup == 256, "a pipeline built with a maximum of 256 threads"); if (!low) return 1;
        const void *k = (__bridge const void *)cs; hm_put_id(&g_cps, &k, low);
        run(&pg, cases[i].out, cases[i].groups); // NOLINT(clang-analyzer-osx.cocoa.RetainCount): hm_put_id keeps the pipeline in g_cps, the host's compute pipeline cache
        CHECK(submit(), "the submission completes with no GPU error");
        int ok = 1; for (uint32_t t = 0; t < THREADS; t++) ok &= ((const uint32_t *)cases[i].out)[t] == (cases[i].runs ? spin_ref(t) : 0u);
        CHECK(ok, cases[i].what);
        serial_del(pg.serial);
    }
    printf("t_tg_guard: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
