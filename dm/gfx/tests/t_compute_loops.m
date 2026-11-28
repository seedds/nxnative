// Host half of test_dmsh_compute_loops.py: actual dispatch, submission state and completion diagnostics, with bounded rewritten kernels supplied by that test.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static NvShader *kernel(const char *path, const char *key, uint32_t sites) {
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&e];
    if (!lib) { fprintf(stderr, "%s\n", e.description.UTF8String); return nil; }
    NvShader *cs = [NvShader new]; cs->stage = 5; snprintf(cs->key, sizeof cs->key, "%s", key);
    cs->fn = [lib newFunctionWithName:@"main0"]; cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = 32; cs->rf.tg[1] = cs->rf.tg[2] = 1; cs->rf.nb = 1; cs->rf.loop_sites = sites;
    cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=0, .aid=0};
    return cs;
}
static void begin(uint8_t *mem) { mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R); S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=256}; }
static void finish(void) { uint64_t f = mtl_sub_commit(); CHECK(mtl_wait(f, 5000000000ull), "bounded submission completes"); mtl_drain(); CHECK(!mtl_last_commit_error(), "no GPU error"); }
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 4) return 2;
    alarm(60); mtl_init();
    NvShader *spin = kernel(argv[1], "1111111111111111", 1), *healthy = kernel(argv[2], "2222222222222222", 2);
    NvShader *barrier = kernel(argv[3], "3333333333333333", 2);
    if (!spin || !healthy || !barrier) return 1;
    prog_t pa = {.serial=970101}, pb = {.serial=970102}, pc = {.serial=970103}; NvProg *a = [NvProg new], *b = [NvProg new], *c = [NvProg new]; a->sh[5] = spin; b->sh[5] = healthy; c->sh[5] = barrier;
    serial_put(pa.serial, a); serial_put(pb.serial, b); serial_put(pc.serial, c);
    size_t page = (size_t)getpagesize(); uint8_t *mem = NULL;
    if (posix_memalign((void **)&mem, page, page)) return 1;
    memset(mem, 0, page); mtl_pool_add(mem, page); uint32_t *q = (uint32_t *)mem;
    p_dispatch many = {.x=128, .y=1, .z=1}, one = {.x=1, .y=1, .z=1};
    q[3] = UINT32_MAX; begin(mem); S.cprog = &pa;
    for (int i = 0; i < 8; i++) dispatch(&many);
    S.cprog = &pb; dispatch(&one); S.cprog = &pc; dispatch(&one); finish();
    printf("spin: %u threads, most %u, fewest %u; canceled nested %u\n", q[1], q[2], q[3], q[4]);
    CHECK(q[1] == 8u * 128u * 32u && q[2] == (1u << 14) && q[3] < (1u << 14), "helper cap ends every thread and later dispatches see abort");
    CHECK(atomic_load(&spin->loop_reported) && !atomic_load(&healthy->loop_reported), "only the cap hitter is reported, not a canceled kernel");
    CHECK(q[4] < 256u * 256u, "submission abort shortens later bounded work");
    int arrived = 1; for (unsigned i = 0; i < 32; i++) arrived &= q[8 + i] == 2047;
    CHECK(arrived && atomic_load(&barrier->loop_reported), "barrier loops keep a uniform fresh budget despite unequal prior work and the abort");
    begin(mem); S.cprog = &pb; dispatch(&one); finish();
    CHECK(q[4] == 256u * 256u, "next submission gets clean state and legitimate nested loops run fully");
    CHECK(!atomic_load(&healthy->loop_reported), "valid cumulative budget produces no cap hit");
    q[0] = 2; begin(mem); S.cprog = &pb; dispatch(&one); finish();
    printf("exhausted nested: %u\n", q[4]);
    CHECK(q[4] == 261632u && atomic_load(&healthy->loop_reported), "a legitimate exhausted function budget is reported");
    q[0] = 0; q[3] = UINT32_MAX; begin(mem); S.cprog = &pa; dispatch(&one); finish();
    CHECK(atomic_load(&spin->loop_reported), "repeat cap hit retains once-per-kernel report state");
    mtl_pool_del(mem); mtl_drain(); free(mem);
    printf("t_compute_loops: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
