// Exercise the production push binding and dump gate with separate graphics and compute banks.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static void run_case(NvShader *sh, NSString *dir, int stage, int pass, int draw, int exists, int bank) {
    g_stat_pass = pass; g_stat_draw = draw;
    CHECK(encode_args(nil, sh, stage, stage == 0), "production encode_args succeeds");
    NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"F7_p%d_d%d_st%d_c24.bin", pass, draw, stage]];
    NSData *data = [NSData dataWithContentsOfFile:path];
    CHECK((data != nil) == exists, "capture presence and filename");
    // Read the pointer from the actual encoded argument buffer and resolve it into the arena.
    uint64_t ptr = 0; memcpy(&ptr, (uint8_t *)sh->ab_buf.contents + sh->ab_off, sizeof ptr);
    CHECK(ptr >= g_cur_arena->gpu && ptr + DMSH_PUSH_SIZE <= g_cur_arena->gpu + g_cur_arena->len, "argument points into current arena");
    if (ptr >= g_cur_arena->gpu && ptr + DMSH_PUSH_SIZE <= g_cur_arena->gpu + g_cur_arena->len) {
        const uint8_t *bound = g_cur_arena->cpu + (ptr - g_cur_arena->gpu);
        CHECK(!memcmp(bound, S.push[bank], DMSH_PUSH_SIZE), "bound arena selects correct stage bank");
        if (data) { CHECK(data.length == DMSH_PUSH_SIZE, "capture is exactly 256 bytes");
            CHECK(data.length == DMSH_PUSH_SIZE && !memcmp(data.bytes, bound, DMSH_PUSH_SIZE), "all captured bytes match bound arena"); }
    }
}
int main(void) { @autoreleasepool {
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&err]) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 2; }
    setenv("DM_NVN_PASSDUMP", "7", 1); unsetenv("DM_NVN_PASSDUMP_ALL"); setenv("DM_NVN_CBDUMP", dir.fileSystemRepresentation, 1); setenv("DM_NVN_CBDUMP_KEY", "matching", 1);
    mtl_init(); arena_begin(); atomic_store(&g_cur_frame, 7);
    for (int i = 0; i < DMSH_PUSH_SIZE; i++) { S.push[0][i] = (uint8_t)(i * 37 + 11); S.push[1][i] = (uint8_t)(i * 19 + 83); }
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; struct A { constant uchar *p [[id(0)]]; }; kernel void k(constant A &a [[buffer(0)]], device uchar *out [[buffer(1)]]) { out[0] = a.p[0]; }" options:nil error:&err];
    if (!lib) { fprintf(stderr, "Metal shader: %s\n", err.localizedDescription.UTF8String); return 2; }
    NvShader *sh = [NvShader new]; sh->fn = [lib newFunctionWithName:@"k"]; sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len = sh->ae.encodedLength;
    sh->rf.nb = 1; sh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_PUSH, .aid = 0}; snprintf(sh->key, sizeof sh->key, "matching");
    run_case(sh, dir, 0, 10, 1, 1, 0);
    run_case(sh, dir, 0, 10, 2, 0, 0); // default gate records only the first encode per pass/stage
    run_case(sh, dir, 5, 10, 3, 1, 1);
    snprintf(sh->key, sizeof sh->key, "matching-extra"); run_case(sh, dir, 0, 11, 4, 0, 0);
    snprintf(sh->key, sizeof sh->key, "matching"); unsetenv("DM_NVN_CBDUMP"); run_case(sh, dir, 0, 12, 5, 0, 0);
    setenv("DM_NVN_CBDUMP", dir.fileSystemRepresentation, 1); g_pd_a0 = -1; run_case(sh, dir, 0, 13, 6, 0, 0);
    [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    printf("== %d checks, %d failed ==\n", checks, failed); return failed != 0;
} }
