// DM_NVN_SHADER_ALT: a shader key with a library in the variant folder draws with it in the switch's DM_AB phases, as a program twin (prog_alt) with the variant's shader object, whose
// pipelines and archive entries are its own (pso_key holds the shader objects; lib_name tags the library alt/<file>); a variant whose side libraries are not the game's set, or whose
// entry points differ from the game's reflection, is refused and its key stays the game's; a key with no variant stays the game's; phase 0 is always the game's.
// make GAME=<id> check_t_shader_alt (fixtures made with the SDK's Metal compiler; without it the test says so and passes)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static int sh(NSString *cmd) { int r = system(cmd.UTF8String); return WIFEXITED(r) ? WEXITSTATUS(r) : -1; }
static void put(NSString *path, NSString *text) { [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int lib(NSString *dir, NSString *name, NSString *src) {   // <dir>/<name>.metallib from MSL
    put([NSString stringWithFormat:@"%@/%@.metal", dir, name], src);
    return sh([NSString stringWithFormat:@"cd '%@' && xcrun -sdk macosx metal -c %@.metal -o %@.air 2>/dev/null && xcrun -sdk macosx metallib %@.air -o %@.metallib && rm %@.metal %@.air", dir, name, name, name, name, name, name]);
}
static NvProg *prog_of(prog_t *program, uint64_t serial, NvShader *fs) {
    NvProg *pr = [NvProg new]; pr->sh[1] = fs; program->magic = MAGIC('P', 'R', 'O', 'G'); program->serial = serial; serial_put(serial, pr); return pr;
}
int main(void) { @autoreleasepool {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s/t_shader_alt.XXXXXX", getenv("TMPDIR") ?: "/tmp"); if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *game = [root stringByAppendingPathComponent:@"game"], *alt = [root stringByAppendingPathComponent:@"alt"];
    NSString *f = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(%d, %d, 0, 1); }\n";
    NSString *kc = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(uint i [[thread_position_in_grid]]) { threadgroup_barrier(mem_flags::mem_none); if (i > %du) threadgroup_barrier(mem_flags::mem_none); }\n";
    NSString *km = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n[[mesh]] void main0(metal::mesh<V, void, 3, 1, topology::triangle> m, uint i [[thread_index_in_threadgroup]]) { if (i == 0) m.set_primitive_count(%du); }\n";
    NSString *fu = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(1); }\nfragment uint4 main0_u() { return uint4(1); }\n";
    if (lib(game, @"f1_b", [NSString stringWithFormat:f, 1, 0]) || lib(alt, @"f1_b", [NSString stringWithFormat:f, 0, 1]) ||   // f1: a variant
        lib(game, @"f2_b", [NSString stringWithFormat:f, 1, 1]) ||                                                             // f2: none
        lib(game, @"fx_b", [NSString stringWithFormat:f, 1, 0]) || lib(game, @"fx_bx", [NSString stringWithFormat:f, 1, 0]) ||   // fx: the variant lacks the game's side library
        lib(alt, @"fx_b", [NSString stringWithFormat:f, 0, 1]) ||
        lib(game, @"fu_b", fu) || lib(alt, @"fu_b", [NSString stringWithFormat:f, 0, 1]) ||                                     // fu: the variant lacks an entry point
        lib(game, @"c1_b", [NSString stringWithFormat:kc, 1]) || lib(alt, @"c1_b", [NSString stringWithFormat:kc, 2]) ||          // c1: a compute kernel with a variant
        lib(game, @"m1_b", [NSString stringWithFormat:km, 1]) || lib(alt, @"m1_b", [NSString stringWithFormat:km, 0])) {           // m1: a mesh shader with a variant
        printf("t_shader_alt: no Metal compiler (xcrun metal), not run\n"); return 0; }
    for (NSString *k in @[@"f1", @"f2", @"fx"]) put([game stringByAppendingFormat:@"/%@_b.json", k], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"entry\": \"main0\", \"bindings\": [], \"vertex_inputs\": []}", DMSH_CHAIN]);
    put([game stringByAppendingPathComponent:@"fu_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"entry\": \"main0\", \"entry_u\": \"main0_u\", \"bindings\": [], \"vertex_inputs\": []}", DMSH_CHAIN]);
    put([game stringByAppendingPathComponent:@"c1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"stage\": \"cs\", \"entry\": \"main0\", \"threadgroup_size\": [1, 1, 1], \"bindings\": [], \"vertex_inputs\": []}", DMSH_CHAIN]);
    put([game stringByAppendingPathComponent:@"m1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"stage\": \"ms\", \"entry\": \"main0\", \"mesh_stage\": {\"kind\": \"ms\", \"threads\": 32, \"payload\": 0, \"task\": false}, \"bindings\": [], \"vertex_inputs\": []}", DMSH_CHAIN]);
    setenv("DM_MSL_DIR", game.UTF8String, 1); setenv("DM_AB", "10", 1); setenv("DM_NVN_SHADER_ALT", alt.UTF8String, 1); setenv("DM_NVN_SHADER_ALT_AB", "ab", 1);
    mtl_init(); g_msl_dir = game;   // the fixture is the library directory itself (no b-<chain> folder)
    NvShader *s1 = shader_load(@"f1_b", 1, 0), *s2 = shader_load(@"f2_b", 1, 0), *sx = shader_load(@"fx_b", 1, 0), *su = shader_load(@"fu_b", 1, 0);
    CHECK(s1 && s2 && sx && su);
    if (!(s1 && s2 && sx && su)) { printf("t_shader_alt: %d checks, %d failed (the fixtures did not load)\n", checks, failed); return 1; }
    static prog_t p1, p2, px, pu; NvProg *b1 = prog_of(&p1, 98001, s1), *b2 = prog_of(&p2, 98002, s2), *bx = prog_of(&px, 98003, sx), *bu = prog_of(&pu, 98004, su);
    atomic_store(&g_ab_phase, 0);
    CHECK(prog_get(&p1) == b1);   // phase 0: the game's
    b1->sh[0] = s2; long rc0 = CFGetRetainCount((__bridge CFTypeRef)s2);   // a stage without a variant: the twin shares the game's shader object
    NvProg *t1 = nil;
    @autoreleasepool {   // the getters' results are autoreleased: the pool ends before the twin is let go
    atomic_store(&g_ab_phase, 1);
    t1 = prog_get(&p1);
    CHECK(t1->sh[0] == s2 && CFGetRetainCount((__bridge CFTypeRef)s2) == rc0 + 1);   // and holds its own reference to it (an unretained copy would be released twice)
    CHECK(t1 != b1 && t1->sh[1] != s1 && t1->sh[1] == s1->alt && t1->sh[1]->altdir != nil);   // phase 1: the twin with the variant
    CHECK(t1->sh[1]->fn != nil && t1->sh[1]->fn != s1->fn);
    CHECK([lib_name([alt stringByAppendingPathComponent:@"f1_b.metallib"]) isEqualToString:@"alt/f1_b.metallib"]);   // the variant's archive key is its own
    CHECK([lib_name([game stringByAppendingPathComponent:@"f1_b.metallib"]) isEqualToString:@"f1_b.metallib"]);
    CHECK(prog_get(&p1) == t1);   // the twin is made once
    CHECK(sh_alt(t1->sh[1]) && !sh_alt(s1));   // its pipelines are not recorded for the prewarm
    pso_key ka, kb; uint32_t cfmt[8] = {MTLPixelFormatRGBA8Unorm};
    pso_key_fill(&ka, s2, s1, cfmt, 0, 0, 1, 0); pso_key_fill(&kb, s2, t1->sh[1], cfmt, 0, 0, 1, 0);
    CHECK(memcmp(&ka, &kb, sizeof ka) != 0);   // its pipelines are other entries
    CHECK(prog_get(&p2) == b2 && !s2->alt);   // no variant
    CHECK(prog_get(&px) == bx && !sx->alt);   // side libraries differ: refused
    CHECK(prog_get(&pu) == bu && !su->alt);   // entry points differ: refused
    {   // a compute kernel: the dispatch takes prog_get(cprog)->sh[5], so its twin carries the variant, and the pipeline cache (g_cps, by shader object) gives it a pipeline of its own
        NvShader *c1 = shader_load(@"c1_b", 5, 0); CHECK(c1 && c1->fn);
        static prog_t pc; NvProg *bc = c1 ? prog_of(&pc, 98005, nil) : nil; if (bc) bc->sh[5] = c1;
        NvProg *tc = bc ? prog_get(&pc) : nil;
        CHECK(tc && tc != bc && tc->sh[5] == c1->alt && sh_alt(tc->sh[5]) && tc->sh[5]->fn && tc->sh[5]->fn != c1->fn);
        CHECK([lib_name([alt stringByAppendingPathComponent:@"c1_b.metallib"]) isEqualToString:@"alt/c1_b.metallib"]);
        NSError *ce = nil; id<MTLComputePipelineState> pg = c1 ? cps_new(c1->fn, &ce) : nil, pv = tc ? cps_new(tc->sh[5]->fn, &ce) : nil;
        CHECK(pg && pv && pg != pv);   // two pipelines, the variant's from its own function
        atomic_store(&g_ab_phase, 0); CHECK(!bc || prog_get(&pc) == bc); atomic_store(&g_ab_phase, 1);   // phase 0: the game's kernel
    }
    {   // a mesh shader: the mesh draw takes prog_get(prog)->sh[4] (nvn_geometry.inc), and mtk_pso keys its pipeline by the shader objects
        NvShader *m1 = shader_load(@"m1_b", 4, 0); CHECK(m1 && m1->fn && m1->mesh_kind == 2);
        static prog_t pm; NvProg *bm = m1 ? prog_of(&pm, 98006, nil) : nil; if (bm) bm->sh[4] = m1;
        NvProg *tm = bm ? prog_get(&pm) : nil;
        CHECK(tm && tm != bm && tm->sh[4] == m1->alt && sh_alt(tm->sh[4]) && tm->sh[4]->mesh_kind == 2 && tm->sh[4]->mesh_notask && tm->sh[4]->fn != m1->fn);
    }
    atomic_store(&g_ab_phase, 2);
    CHECK(prog_get(&p1) == b1);   // back to the game's
    }
    b1->alt = nil; t1 = nil;
    CHECK(CFGetRetainCount((__bridge CFTypeRef)s2) == rc0);   // the twin gone, the shader's references are as before
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_shader_alt: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
