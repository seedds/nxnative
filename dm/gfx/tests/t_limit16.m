// The float16 output limit: which pipelines take the library (lim_key: a limited output on a float16 target, none on a 32-bit float target, the merged pass's map followed),
// and the host loads <key>_bs and finds the entry point that matches each of the shader's own.
// make GAME=<id> check_t_limit16 (the fixture comes from test_dmsh_limit16.py --fixture <dir>; ./t_limit16 <dir>)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    setenv("DM_MSL_DIR", argv[1], 1);
    mtl_init(); g_msl_dir = @(argv[1]);
    NSError *error = nil; NSString *source = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingString:@"/base.metal"] encoding:NSUTF8StringEncoding error:&error];
    if (!source) { fprintf(stderr, "no base.metal in %s\n", argv[1]); return 1; }
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvShader *ps = [NvShader new]; ps->stage = 1; ps->limit_colors = 1 << 2; memset(ps->key, '0', 16); memcpy(ps->key + 16, "_b", 3);
    ps->fn = fn4_note([lib newFunctionWithName:@"main0"], lib, @"main0", nil);
    ps->fn_u = fn4_note([lib newFunctionWithName:@"main0_u"], lib, @"main0_u", nil);
    uint32_t cf[8] = {MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatR16Float};
    CHECK(lim_key(ps, cf, NULL) == 2);
    cf[2] = MTLPixelFormatRGBA16Float; CHECK(lim_key(ps, cf, NULL) == 2);
    cf[2] = MTLPixelFormatRG11B10Float; CHECK(lim_key(ps, cf, NULL) == 0);
    cf[2] = MTLPixelFormatR32Float; CHECK(lim_key(ps, cf, NULL) == 0);
    cf[0] = MTLPixelFormatR16Float; CHECK(lim_key(ps, cf, NULL) == 0);   // colour 0 is not limited
    uint8_t lmap[8] = {2, 0, 1}; CHECK(lim_key(ps, cf, lmap) == 2);   // a merged pass: colour 2 is physical target 0
    cf[0] = MTLPixelFormatR16Float; cf[1] = MTLPixelFormatR32Float; lmap[1] = 2; CHECK(lim_key(ps, cf, lmap) == 0);   // one limited output on each kind of target
    CHECK(lim_key(nil, cf, NULL) == 0); ps->limit_colors = 0; CHECK(lim_key(ps, cf, NULL) == 0); CHECK(lim_fn(ps, ps->fn) == nil); ps->limit_colors = 1 << 2;
    id<MTLFunction> f = lim_fn(ps, ps->fn), u = lim_fn(ps, ps->fn_u);
    CHECK(ps->sfn_state == 1); CHECK([f.name isEqualToString:@"main0"]); CHECK([u.name isEqualToString:@"main0_u"]);
    CHECK(f != ps->fn); CHECK(lim_fn(ps, lib ? nil : ps->fn) == nil);
    printf("float16 limit: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
