// A compute shader Apple's compiler takes minutes (or forever) to build (Nintendo Switch Sports Resort), stood in for by an MTLDevice whose builds of the kernels named "hold*"
// wait until the test releases them. A pipeline neither made nor archived is built off the submit thread (cps_get in nvn_mtl.m): a build that returns within the wait runs in
// the same submission; a held one's first dispatch waits at most the wait and is skipped, a second held shader in the same burst does not wait (one budget for a burst), later
// work of the submission runs, later dispatches of a held shader are skipped without waiting or building again, and once the build returns its dispatches run. With no wait
// (DM_NVN_CPS_WAIT_MS=0) a new pipeline's first dispatch is skipped at once and the next one runs; a build that fails is not asked for again. A pipeline the archive holds is
// taken at once on the submit thread, with no build. The held builds are named in the log as slow, and their pipelines are captured each into a flush period of its own
// (arch_harvest_solo), so they hold back no other pipeline's file
#include "../nvn/nvn_mtl.m"
#include <objc/runtime.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

// The MTLDevice's compute pipeline build, counted by kernel name; held for "hold*" until released, failed for "bad_k"
typedef id (*cps_fn)(id, SEL, id, NSError **);
static cps_fn g_real_cps; static dispatch_semaphore_t g_entered;
static _Atomic int g_released, g_builds_hold, g_builds_hold2, g_builds_free, g_builds_arch, g_builds_bad, g_builds_on_submit;
static pthread_t g_submit;
static id fake_cps(id self, SEL cmd, id<MTLFunction> f, NSError **e) {
    if (pthread_equal(pthread_self(), g_submit)) g_builds_on_submit++;
    if ([f.name hasPrefix:@"hold"]) {
        ([f.name isEqualToString:@"hold_k"] ? g_builds_hold++ : g_builds_hold2++);
        dispatch_semaphore_signal(g_entered);
        while (!atomic_load(&g_released)) usleep(1000);
    } else if ([f.name isEqualToString:@"free_k"]) g_builds_free++;
    else if ([f.name isEqualToString:@"arch_k"]) g_builds_arch++;
    else if ([f.name isEqualToString:@"bad_k"]) { g_builds_bad++; if (e) *e = [NSError errorWithDomain:@"test" code:1 userInfo:@{NSLocalizedDescriptionKey: @"refused"}]; return nil; }
    return g_real_cps(self, cmd, f, e);
}
static int until(int (^cond)(void), double seconds) { uint64_t t0 = bp_now(); while (!cond()) { if (bp_now() - t0 > seconds * 1e9) return 0; usleep(10000); } return 1; }

// A program whose compute shader is kernel name of lib: it writes its value to out[thread]
static prog_t *program(uint64_t serial, id<MTLLibrary> lib, NSString *name) {
    prog_t *pg = calloc(1, sizeof *pg); pg->serial = serial;
    NvProg *p = [NvProg new]; NvShader *cs = [NvShader new];
    p->sh[5] = cs; cs->stage = 5; cs->fn = fn4_note([lib newFunctionWithName:name], lib, name, nil);
    strlcpy(cs->key, [name stringByAppendingString:@"_b"].UTF8String, sizeof cs->key);
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = cs->rf.tg[1] = cs->rf.tg[2] = 1;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=0, .aid=0};
    serial_put(serial, p);
    return pg;
}
static NvShader *shader_of(prog_t *pg) { NvProg *p = serial_get(pg->serial); return p->sh[5]; }
static int job_done(prog_t *pg) { CpsJob *job = shader_of(pg)->cps_job[1]; return !job || !dispatch_group_wait(job->done, DISPATCH_TIME_NOW); }
static uint32_t *g_mem;
enum { OUT_WORDS = 64, THREADS = 4, WAIT_MS = 400 };
// One submission: each program dispatched in turn, program i writing out[i * OUT_WORDS ...]; returns how long the replay took, without the GPU's time (ms)
static double submit(prog_t **pgs, int n) {
    memset(g_mem, 0, 4096);
    mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
    uint64_t t0 = bp_now();
    for (int i = 0; i < n; i++) {
        S.cprog = pgs[i]; S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)(g_mem + i * OUT_WORDS), .size=OUT_WORDS * 4};
        p_dispatch dp = {.x=THREADS, .y=1, .z=1}; dispatch(&dp);
    }
    double ms = (bp_now() - t0) / 1e6;
    uint64_t fence = mtl_sub_commit(); if (!mtl_wait(fence, 5000000000ull)) { fprintf(stderr, "FAIL: GPU timeout\n"); failed++; }
    mtl_drain();
    return ms;
}
static int wrote(int i, uint32_t v) { for (int t = 0; t < THREADS; t++) if (g_mem[i * OUT_WORDS + t] != v) return 0; return 1; }

int main(void) { @autoreleasepool {
    char tmp[] = "/tmp/t_cps_async.XXXXXX"; if (!mkdtemp(tmp)) abort();
    NSString *arch = [@(tmp) stringByAppendingPathComponent:@"archive"];
    setenv("DM_PSO_ARCHIVE", arch.UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init(); pso4_init();
    if (!g_comp4 || !g_arch_cap) { printf("t_cps_async: no Metal 4 compiler, not run\n"); return 0; }
    g_submit = pthread_self(); g_entered = dispatch_semaphore_create(0);
    Method m = class_getInstanceMethod(object_getClass((id)g_dev), @selector(newComputePipelineStateWithFunction:error:));
    if (!m) abort();
    g_real_cps = (cps_fn)method_setImplementation(m, (IMP)fake_cps);
    g_cps_wait_ms = WAIT_MS; g_slow_ms = 200;   // the bound on a first dispatch's wait, and when a compile is named as slow (5 s each in a run)

    NSError *error = nil;
    NSMutableString *src = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\nstruct Args { device uint *out [[id(0)]]; };\n"];
    NSDictionary<NSString *, NSNumber *> *kernels = @{@"hold_k": @7, @"hold2_k": @8, @"free_k": @9, @"arch_k": @5, @"zero_k": @6, @"bad_k": @4};
    for (NSString *k in kernels) [src appendFormat:@"kernel void %@(constant Args &a [[buffer(0)]], uint3 p [[thread_position_in_grid]]) { a.out[p.x] = %@; }\n", k, kernels[k]];
    [src appendFormat:@"// %d\n", getpid()];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "synthetic compute shaders compile"); if (!lib) return 1;
    if (posix_memalign((void **)&g_mem, getpagesize(), getpagesize())) return 1;
    mtl_pool_add(g_mem, getpagesize());
    prog_t *hold = program(970001, lib, @"hold_k"), *hold2 = program(970002, lib, @"hold2_k"), *fre = program(970003, lib, @"free_k");

    // a build that returns within the wait: built off the submit thread, its first dispatch runs
    uint64_t async0 = g_cps_async;
    submit((prog_t *[]){fre}, 1);
    CHECK(wrote(0, 9), "in time: its first dispatch runs");
    CHECK(g_cps_async == async0 + 1 && g_builds_free == 1, "in time: built once, in the background");

    // held, two at once: the first one's dispatch waits the bound and is skipped; the second held shader's, in the same burst, does not wait; the made pipeline's dispatch
    // after them runs; a second dispatch of a held shader neither waits nor asks for another build
    uint64_t skipped0 = g_cps_skipped, told0 = g_slow_told;
    double ms = submit((prog_t *[]){hold, hold2, fre, hold}, 4);
    CHECK(!dispatch_semaphore_wait(g_entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), "held: a build started");
    CHECK(!dispatch_semaphore_wait(g_entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), "held: the other build started");
    uint64_t entered2 = bp_now();   // both builds are held from here on: released only once each has run past g_slow_ms (so each is captured alone)
    CHECK(ms >= WAIT_MS / 2, "held: the first dispatch waited for the bound");
    CHECK(ms < WAIT_MS * 3 / 2, "held: one wait for the burst, not one each, and none for the build itself");
    CHECK(wrote(0, 0) && wrote(1, 0) && wrote(3, 0), "held: their dispatches skipped");
    CHECK(wrote(2, 9), "held: a later dispatch in the submission ran");
    CHECK(g_builds_hold == 1 && g_builds_hold2 == 1, "held: each built once");
    CHECK(g_builds_on_submit == 0, "held: no build on the submit thread");
    CHECK(g_cps_skipped - skipped0 == 3, "held: three dispatches counted as skipped");
    ms = submit((prog_t *[]){fre, hold, hold2}, 3);
    CHECK(ms < WAIT_MS / 2, "held, next submission: no wait for the held builds");
    CHECK(wrote(0, 9) && wrote(1, 0) && wrote(2, 0), "held, next submission: the other runs, the held ones are skipped");
    CHECK(g_builds_hold == 1 && g_builds_hold2 == 1 && g_builds_free == 1, "held, next submission: nothing built again");
    CHECK(until(^{ return (int)(g_slow_told >= told0 + 2); }, 5), "held: named in the log as still compiling");
    while (bp_now() - entered2 < 3ull * g_slow_ms * 1000000ull) usleep(10000);

    // released: the next dispatches take the pipelines, each built once; each build is captured into a period of its own and written, the current period untouched
    dispatch_sync(arch_harvest_q(), ^{});   // the other pipeline's capture, into the current period
    uint64_t solo0 = g_arch_solo; int cur_n = atomic_load(&g_ep->n);
    atomic_store(&g_released, 1);
    CHECK(until(^{ return (int)(g_arch_solo >= solo0 + 2); }, 30), "released: each pipeline captured alone and written");
    CHECK(atomic_load(&g_ep->n) == cur_n, "released: the current flush period holds nothing of them");
    submit((prog_t *[]){hold, hold2}, 2);
    CHECK(wrote(0, 7) && wrote(1, 8), "released: their dispatches run");
    CHECK(g_builds_hold == 1 && g_builds_hold2 == 1, "released: not built again");
    CHECK(g_builds_on_submit == 0, "released: no build on the submit thread");

    // no wait (DM_NVN_CPS_WAIT_MS=0): the first dispatch of a new pipeline is skipped at once, the next one after its build runs
    g_cps_wait_ms = 0; g_cps_quiet_until = 0;
    prog_t *zero = program(970005, lib, @"zero_k");
    skipped0 = g_cps_skipped;
    ms = submit((prog_t *[]){zero}, 1);
    CHECK(ms < WAIT_MS / 2, "no wait: the dispatch did not wait");
    CHECK(wrote(0, 0) && g_cps_skipped == skipped0 + 1, "no wait: skipped");
    CHECK(until(^{ return job_done(zero); }, 10), "no wait: built in the background");
    submit((prog_t *[]){zero}, 1);
    CHECK(wrote(0, 6), "no wait: its next dispatch runs");
    g_cps_wait_ms = WAIT_MS;

    // a build that fails: within the wait, the dispatch is skipped and the shader marked; never asked again
    prog_t *bad = program(970006, lib, @"bad_k");
    submit((prog_t *[]){bad, fre}, 2);
    CHECK(wrote(0, 0) && wrote(1, 9), "failed: skipped, the next dispatch runs");
    CHECK(shader_of(bad)->cps_failed == 1 && !shader_of(bad)->cps_job[0] && !shader_of(bad)->cps_job[1], "failed: marked, no build left");
    submit((prog_t *[]){bad}, 1);
    CHECK(g_builds_bad == 1, "failed: not asked again");

    // the cached path: a pipeline in the archive is taken on the submit thread, at once, with no build
    id<MTLFunction> af = fn4_note([lib newFunctionWithName:@"arch_k"], lib, @"arch_k", nil);
    CHECK(cps_new(af, &error) != nil, "archived: built once for the archive");
    dispatch_sync(arch_harvest_q(), ^{});
    arch_flush_wait();
    NSMutableArray<id<MTL4Archive>> *open = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:arch error:nil]) if ([n hasSuffix:@".mtl4a"]) {
        id<MTL4Archive> a = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:[arch stringByAppendingPathComponent:n]] error:nil];
        if (a) [open addObject:a];
    }
    CHECK(open.count >= 3, "archived: the files written");
    g_arch4 = open;   // as a new start with those files (the index is empty: every file is asked)
    int builds = g_builds_arch; uint64_t run0 = g_cps_run, async1 = g_cps_async;
    prog_t *ap = program(970004, lib, @"arch_k");
    submit((prog_t *[]){ap}, 1);
    CHECK(wrote(0, 5), "archived: its first dispatch runs");
    CHECK(g_builds_arch == builds, "archived: not built");
    CHECK(g_cps_run == run0 + 1, "archived: taken from the archive");
    CHECK(g_cps_async == async1, "archived: nothing built in the background");
    // the held shaders' pipelines are in their own files too: new shaders with their functions take them from there
    int hb = g_builds_hold, hb2 = g_builds_hold2; prog_t *hp = program(970007, lib, @"hold_k"), *hp2 = program(970008, lib, @"hold2_k");
    submit((prog_t *[]){hp, hp2}, 2);
    CHECK(wrote(0, 7) && wrote(1, 8) && g_builds_hold == hb && g_builds_hold2 == hb2, "archived: the held shaders' pipelines taken from their own files");

    method_setImplementation(m, (IMP)g_real_cps);
    [NSFileManager.defaultManager removeItemAtPath:@(tmp) error:nil];
    printf("t_cps_async: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
