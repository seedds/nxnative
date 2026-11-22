// Inject a compute pipeline error at the Metal device, then drive the actual NVN dispatch path.
#import <objc/runtime.h>
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed, compile_attempts;
static NSString *compile_error;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id fail_compute_pipeline(id self, SEL sel, id<MTLFunction> f, NSError **error) __attribute__((ns_returns_retained));
static id fail_compute_pipeline(id self, SEL sel, id<MTLFunction> f, NSError **error) {
    compile_attempts++;
    if (error) *error = [NSError errorWithDomain:@"TestMetalCompiler" code:2 userInfo:@{NSLocalizedDescriptionKey: compile_error}];
    return nil;
}

static NvShader *shader(id<MTLFunction> function, const char *key) {
    NvShader *cs = [NvShader new];
    snprintf(cs->key, sizeof cs->key, "%s", key);
    cs->stage = 5;
    cs->fn = function;
    cs->ae = [function newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = cs->rf.tg[1] = cs->rf.tg[2] = 1;
    cs->rf.nb = 1;
    cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=0, .aid=0};
    return cs;
}

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *out [[id(0)]]; };\n"
        "kernel void write_value(constant Args &a [[buffer(0)]]) { a.out[0] = 42; }\n" options:nil error:&error];
    id<MTLFunction> function = [lib newFunctionWithName:@"write_value"];
    Method method = class_getInstanceMethod([(id)g_dev class], @selector(newComputePipelineStateWithFunction:error:));
    if (!function || !method) { fprintf(stderr, "Cannot set up compute test: %s\n", error.localizedDescription.UTF8String); return 1; }

    NvProg *program = [NvProg new];
    prog_t pg = {.serial=960011};
    serial_put(pg.serial, program);
    size_t page = (size_t)getpagesize();
    void *mem = NULL;
    if (posix_memalign(&mem, page, page)) return 1;
    memset(mem, 0, page);
    mtl_pool_add(mem, page);
    mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
    S.cprog = &pg;
    S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=page};
    p_dispatch one = {.x=1, .y=1, .z=1};

    NvShader *retry = shader(function, "1111111111111111");
    program->sh[5] = retry;
    compile_error = @"Compilation failed: XPC_ERROR_CONNECTION_INTERRUPTED";
    IMP original = method_setImplementation(method, (IMP)fail_compute_pipeline);
    dispatch(&one);
    method_setImplementation(method, original);
    CHECK(compile_attempts == 1 && !retry->cps_failed, "a compiler-service interruption leaves the shader retryable");
    dispatch(&one);
    uint64_t fence = mtl_sub_commit();
    CHECK(mtl_wait(fence, 5000000000ull), "retry submission completes");
    mtl_drain();
    CHECK(!mtl_last_commit_error() && *(uint32_t *)mem == 42, "the next dispatch builds the pipeline and writes the result");

    mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
    S.cprog = &pg;
    S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=page};
    NvShader *refused = shader(function, "2222222222222222");
    program->sh[5] = refused;
    compile_attempts = 0;
    compile_error = @"Compute pipeline refused";
    original = method_setImplementation(method, (IMP)fail_compute_pipeline);
    dispatch(&one);
    dispatch(&one);
    method_setImplementation(method, original);
    CHECK(compile_attempts == 1 && refused->cps_failed, "a permanent refusal is cached");

    // The reported failure occurs after the first three network dispatches have been consumed.
    CHECK(ndve_offsets(), "model weight offsets are valid");
    g_nd.state = 2;
    g_nd.pos = 4;
    g_nd.w0 = 0x100000;
    program->sh[5] = shader(function, k_ndve_keys[4]);
    S.ssbo[5][2] = (p_bind_buf){.addr=g_nd.w0 + g_ndve_w[4], .size=1};
    S.ssbo[5][3] = (p_bind_buf){.addr=g_nd.w0 + g_ndve_b[4], .size=1};
    compile_attempts = 0;
    compile_error = @"Compilation failed: XPC_ERROR_CONNECTION_INTERRUPTED";
    original = method_setImplementation(method, (IMP)fail_compute_pipeline);
    dispatch(&one);
    method_setImplementation(method, original);
    CHECK(compile_attempts == 0 && g_nd.pos == 5, "NDVE consumes the layer without asking Metal for its fallback pipeline");

    program->sh[5] = shader(function, k_ndve_keys[5]);
    S.ssbo[5][2].addr = g_nd.w0 + g_ndve_w[5];
    S.ssbo[5][3].addr = g_nd.w0 + g_ndve_b[5];
    dispatch(&one);
    CHECK(g_nd.pos == 6 && !g_nd.off, "the following layer continues the network in order");
    fence = mtl_sub_commit();
    CHECK(mtl_wait(fence, 5000000000ull), "NDVE test submission completes");
    mtl_drain();
    mtl_pool_del(mem);
    mtl_drain();
    free(mem);
    printf("t_dispatch_pipeline: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
