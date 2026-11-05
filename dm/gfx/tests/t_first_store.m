// Dispatch-first resets must follow preceding GPU writes and precede every append.
// Exercise the production Metal 4 path with four words and GPU-written indirect counts.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init(); state_reset(); memset(&R, 0, sizeof R);
    NSError *error = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device atomic_uint *words [[id(0)]]; };\n"
        "kernel void append(constant Args &a [[buffer(0)]]) { "
        "uint n=atomic_fetch_add_explicit(a.words,1u,memory_order_relaxed); "
        "atomic_fetch_max_explicit(a.words+1,(n+64u)/64u,memory_order_relaxed); }";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "append fixture compiles"); if (!lib) return 1;
    NvShader *cs = [NvShader new]; cs->stage = 5; cs->fn = [lib newFunctionWithName:@"append"];
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = 64; cs->rf.tg[1] = cs->rf.tg[2] = 1;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=2, .aid=0};
    cs->rf.nfirst = 4;
    const uint16_t word[] = {0, 2, 3, 1}; const uint32_t value[] = {0, 1, 1, 0};
    for (int i=0; i<4; i++) { cs->rf.first[i].reg=2; cs->rf.first[i].word=word[i]; cs->rf.first[i].value=value[i]; }
    NvProg *pr = [NvProg new]; pr->sh[5] = cs; prog_t guest = {.serial=UINT64_MAX-8};
    serial_put(guest.serial, pr); S.cprog = &guest; // NOLINT(clang-analyzer-osx.cocoa.RetainCount): serial_del releases the table's retain.
    void *mem = NULL; size_t len = (size_t)getpagesize(); if (posix_memalign(&mem, len, len)) abort();
    memset(mem, 0, len); mtl_pool_add(mem, len); uint64_t addr=(uint64_t)(uintptr_t)mem;
    S.ssbo[5][2] = (p_bind_buf){.addr=addr, .size=64};
    for (int run=0; run<20; run++) {
        mtl_sub_begin();
        mtl_sub_fill(addr, 20, 0x12345678);
        mtl_sub_fill(addr+256, 4, 128); mtl_sub_fill(addr+260, 8, 1);
        p_dispatch dp = {.x=128, .y=1, .z=1}; if (run & 1) dp.ind=addr+256;
        dispatch(&dp);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "append dispatch completes");
        CHECK(!mtl_last_commit_error(), "append has no GPU error");
        const uint32_t *out=mem;
        CHECK(out[0]==8192 && out[1]==128, "all workgroups append after the reset");
        CHECK(out[2]==1 && out[3]==1, "both nonzero indirect words survive");
        CHECK(out[4]==0x12345678, "adjacent storage remains intact");
    }
    mtl_drain(); serial_del(guest.serial); S.cprog=NULL; mtl_pool_del(mem); free(mem);
    printf("t_first_store: %d checks, %d failed\n", checks, failed); return failed!=0;
} }
