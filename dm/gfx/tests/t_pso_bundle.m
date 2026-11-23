// The pipeline archive made offline and bundled with the app (DM_PSO_BUNDLE, pso_bundle_open / pso_bundle_covers in nvn_mtl.m), made here as build_ipa.sh makes it: tiny shader
// libraries and their reflection, a pair list, then dm/gfx/tools/pso_offline.py (metal-tt) for this Mac's GPU. Checks: the archive for this GPU opens and comes first among the files of
// bases without a vertex layout; with a matching stamp the prebuild builds only the pairs left to the device (a function with constants) and takes nothing from the compiler otherwise,
// and a draw of a covered pair takes its base from the bundle and captures nothing for the device's own archive;
// a stamp of another depth mode or other pairs runs the device's prebuild, which still takes the bundled bases it finds; an archive whose bases the probe does not find is not trusted;
// a folder without this GPU's file and a file that is no archive leave no archive; the compute shaders of the shader map have their pipelines in the archive (cps_new: nothing
// built), and a shader with function constants or not in the map is built as before; a pulling and a shadow-input library whose entry points the reflection names otherwise than
// the shader's own are loaded by those names (shader_load, pull_load, vin_fn), and the archive holds the base pre_desc makes of them.
// Without metal-tt (no Metal toolchain) it says so and passes
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, ...) do { uint64_t w = (want), g = (__VA_ARGS__); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static int sh(NSString *cmd) { int r = system(cmd.UTF8String); return WIFEXITED(r) ? WEXITSTATUS(r) : -1; }
static void put(NSString *path, NSString *text) { [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int lib(NSString *dir, NSString *name, NSString *src) {   // <dir>/<name>.metallib from MSL, with the SDK's compiler
    put([NSString stringWithFormat:@"%@/%@.metal", dir, name], src);
    return sh([NSString stringWithFormat:@"cd %@ && xcrun -sdk macosx metal -c %@.metal -o %@.air && xcrun -sdk macosx metallib %@.air -o %@.metallib && rm %@.metal %@.air", dir, name, name, name, name, name, name]);
}
static void reset_counts(void) { g_pre_done = g_pre_built = g_pre_archived = g_pre_skipped = g_pre_failed = 0; }

int main(void) { @autoreleasepool {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s/t_pso_bundle.XXXXXX", getenv("TMPDIR") ?: "/tmp"); if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *msl = [root stringByAppendingFormat:@"/msl/b-%s", DMSH_CHAIN], *tables = [root stringByAppendingPathComponent:@"tables"];
    NSString *bundle = [root stringByAppendingPathComponent:@"bundle"], *arch = [root stringByAppendingPathComponent:@"archive"];
    // A magic number alone cannot establish an archive: these are the known crash inputs, checked without calling Metal.
    NSString *bad = [root stringByAppendingPathComponent:@"bad.mtl4a"];
    const uint8_t magics[][4] = {{0xcf, 0xfa, 0xed, 0xfe}, {0xcb, 0xfe, 0xba, 0xbe}, {0xca, 0xfe, 0xba, 0xbe}, {0xca, 0xfe, 0xba, 0xbf}};
    for (unsigned i = 0; i < sizeof magics / sizeof magics[0]; i++) {
        [[NSData dataWithBytes:magics[i] length:4] writeToFile:bad atomically:YES];
        EXPECT("archive guard: bare magic refused", 0, arch_file_ok(bad));
        NSMutableData *header = [NSMutableData dataWithLength:32]; memcpy(header.mutableBytes, magics[i], 4); [header writeToFile:bad atomically:YES];
        EXPECT("archive guard: bare header refused", 0, arch_file_ok(bad));
    }
    NSString *v = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
        "vertex V main0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n"
        "vertex V main0_z(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0.5, 1); return o; }\n";
    NSString *f = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(%d, %d, 0, 1); }\n";
    NSString *fc = @"#include <metal_stdlib>\nusing namespace metal;\nconstant bool k [[function_constant(0)]];\nfragment float4 main0() { return is_function_constant_defined(k) ? float4(1) : float4(0); }\n";
    NSString *k = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * %d; }\n";
    NSString *kc = @"#include <metal_stdlib>\nusing namespace metal;\nconstant uint m [[function_constant(0)]];\n"
        "kernel void main0(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = is_function_constant_defined(m) ? m : i; }\n";
    [NSFileManager.defaultManager createDirectoryAtPath:msl withIntermediateDirectories:YES attributes:nil error:nil];
    if (lib(msl, @"v1_bp", v) || lib(msl, @"f1_b", [NSString stringWithFormat:f, 1, 0]) || lib(msl, @"f2_b", [NSString stringWithFormat:f, 0, 1]) || lib(msl, @"fc_b", fc) ||
        lib(msl, @"c1_b", [NSString stringWithFormat:k, 2]) || lib(msl, @"c2_b", [NSString stringWithFormat:k, 4]) ||
        lib(msl, @"c3_b", [NSString stringWithFormat:k, 5]) || lib(msl, @"cc_b", kc)) {
        printf("t_pso_bundle: no Metal compiler (xcrun metal), GPU checks not run; %d file checks failed\n", failed); return failed != 0; }
    put([msl stringByAppendingPathComponent:@"v1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"entry_z\": \"main0_z\", \"pull\": {}, \"vertex_inputs\": []}", DMSH_CHAIN]);
    for (NSString *k in @[@"f1", @"f2", @"fc"]) put([msl stringByAppendingFormat:@"/%@_b.json", k], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\"}", DMSH_CHAIN]);
    for (NSString *c in @[@"c1", @"c2", @"c3", @"cc"]) put([msl stringByAppendingFormat:@"/%@_b.json", c], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"stage\": \"cs\", \"entry\": \"main0\"%@}",
                                                DMSH_CHAIN, @""]);
    put([tables stringByAppendingPathComponent:@"shader_map.tsv"], @"package\tindex\tstage\tdm_ctl_sha16\tsource\ncs.bin\t0\tcompute\tc1\tx\ncs.bin\t1\tcompute\tc2\tx\ncs.bin\t2\tcompute\tcc\tx\n");
    put([tables stringByAppendingPathComponent:@"shader_pairs.tsv"], @"# pairs\nv1\tf1\nv1\tf2\nv1\tfc\n");
    setenv("DM_NVN_SHMAP", [tables stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1);
    setenv("DM_MSL_DIR", [root stringByAppendingPathComponent:@"msl"].UTF8String, 1);
    setenv("DM_PSO_ARCHIVE", arch.UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init();
    NSString *gpu = g_dev.architecture.name;
    // the archive as build_ipa.sh makes it (the tool's own folder: this file is dm/gfx/tests/t_pso_bundle.m)
    NSString *tool = [[@(__FILE__) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"../tools/pso_offline.py"];
    if (sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/shader_pairs.tsv --shader-map %@/shader_map.tsv --out %@ --work %@/work > %@/pso_offline.log 2>&1",
            tool, gpu, msl, tables, tables, bundle, root, root])) {
        NSString *log = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_offline.log"] encoding:NSUTF8StringEncoding error:nil];
        if ([log containsString:@"metal-tt is not available"]) { printf("t_pso_bundle: no metal-tt (xcodebuild -downloadComponent MetalToolchain), GPU checks not run; %d file checks failed\n", failed); return failed != 0; }
        fprintf(stderr, "pso_offline.py failed:\n%s\n", log.UTF8String); return 1;
    }
    EXPECT("bundle: this GPU's archive", 1, [NSFileManager.defaultManager fileExistsAtPath:[bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]]);
    NSData *archiveBytes = [NSData dataWithContentsOfFile:[bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]];
    EXPECT("archive guard: real offline archive accepted", 1, arch_file_ok([bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]));
    // what the open reads first (arch_open): the records outside __TEXT (__AIR_DATA) and the libraries (__TEXT,__metallib), not the GPU code
    @autoreleasepool {
        NSString *ap = [bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]; arch_reads rd = {0};
        EXPECT("archive reads: offline archive checked", 1, arch_file_check(ap, &rd));
        EXPECT("archive reads: offline archive, records and libraries", 2, rd.n);
        uint64_t tot = 0; for (int i = 0; i < rd.n; i++) tot += rd.len[i];
        EXPECT("archive reads: offline archive, less than the file", 1, tot > 0 && tot < archiveBytes.length);
        NSError *e = nil; EXPECT("archive reads: offline archive opens", 1, arch_open(ap, &e) != nil);
        EXPECT("archive reads: not an archive opens nothing", 1, arch_open(bad, &e) == nil);
    }
    for (NSUInteger len = 32; len < archiveBytes.length; len = MIN(len * 2, archiveBytes.length)) {
        [[archiveBytes subdataWithRange:NSMakeRange(0, len)] writeToFile:bad atomically:YES];
        EXPECT("archive guard: truncated offline archive refused", 0, arch_file_ok(bad));
    }
    // Both standard universal wrappers must retain their slice bounds, including 64-bit offsets that cannot fit in the file.
    for (int wide = 0; wide < 2; wide++) {
        NSMutableData *wrapped = [NSMutableData dataWithLength:64];
        struct fat_header *fh = wrapped.mutableBytes; fh->magic = OSSwapHostToBigInt32(wide ? FAT_MAGIC_64 : FAT_MAGIC); fh->nfat_arch = OSSwapHostToBigInt32(1);
        if (wide) {
            struct fat_arch_64 *a = (void *)((uint8_t *)wrapped.mutableBytes + sizeof *fh);
            a->offset = OSSwapHostToBigInt64(64); a->size = OSSwapHostToBigInt64(archiveBytes.length);
        } else {
            struct fat_arch *a = (void *)((uint8_t *)wrapped.mutableBytes + sizeof *fh);
            a->offset = OSSwapHostToBigInt32(64); a->size = OSSwapHostToBigInt32((uint32_t)archiveBytes.length);
        }
        [wrapped appendData:archiveBytes]; [wrapped writeToFile:bad atomically:YES];
        EXPECT("archive guard: universal GPU image accepted", 1, arch_file_ok(bad));
        if (wide) ((struct fat_arch_64 *)((uint8_t *)wrapped.mutableBytes + sizeof *fh))->offset = UINT64_MAX;
        else ((struct fat_arch *)((uint8_t *)wrapped.mutableBytes + sizeof *fh))->size = UINT32_MAX;
        [wrapped writeToFile:bad atomically:YES];
        EXPECT("archive guard: universal slice outside file refused", 0, arch_file_ok(bad));
    }
    NSMutableData *broken = [archiveBytes mutableCopy];
    struct mach_header_64 *mh = broken.mutableBytes;
    EXPECT("archive fixture: one GPU image", MH_MAGIC_64, mh->magic);
    ((struct load_command *)((uint8_t *)broken.mutableBytes + sizeof *mh))->cmdsize = 0;
    [broken writeToFile:bad atomically:YES];
    EXPECT("archive guard: invalid load command refused", 0, arch_file_ok(bad));
    NSString *rest = [NSString stringWithContentsOfFile:[bundle stringByAppendingPathComponent:@"rest_pairs.tsv"] encoding:NSUTF8StringEncoding error:nil];
    EXPECT("bundle: one pair left to the device", 1, [rest isEqualToString:@"v1\tfc\n"]);
    setenv("DM_PSO_BUNDLE", bundle.UTF8String, 1);
    shmap_load(); pso4_init();
    EXPECT("bundle: opened", 1, g_arch_bundle != nil);
    EXPECT("bundle: first among the files without a vertex layout", 1, g_arch4p.count && g_arch4p[0] == g_arch_bundle);
    // compute pipelines: the shader map's compute shaders come from the bundle; a shader with function constants and one the map does not have are built
    @autoreleasepool {
        NvShader *c1 = shader_load(@"c1_b", 5, 0), *c2 = shader_load(@"c2_b", 5, 0), *c3 = shader_load(@"c3_b", 5, 0), *cc = shader_load(@"cc_b", 5, 0); NSError *err = nil;
        EXPECT("compute: shaders loaded", 1, c1 && c2 && c3 && cc);
        uint64_t a0 = g_cps_arch, b0 = g_cps_built; int n0 = g_arch_new;
        EXPECT("compute: a pipeline", 1, c1 && cps_new(c1->fn, &err) != nil);
        EXPECT("compute: a second pipeline", 1, c2 && cps_new(c2->fn, &err) != nil);
        EXPECT("compute: both from the bundle", a0 + 2, g_cps_arch);
        EXPECT("compute: nothing built", b0, g_cps_built);
        EXPECT("compute with constants: a pipeline", 1, cc && cps_new(cc->fn, &err) != nil);
        EXPECT("compute not in the map: a pipeline", 1, c3 && cps_new(c3->fn, &err) != nil);
        EXPECT("compute with constants, not in the map: built", b0 + 2, g_cps_built);
        EXPECT("compute with constants, not in the map: not from the bundle", a0 + 2, g_cps_arch);
        dispatch_sync(arch_harvest_q(), ^{});
        EXPECT("compute not in the map: captured into this device's archive, the one with constants not", n0 + 1, g_arch_new);
    }
    // A separate output forces one base per compiler invocation, then metal-pack merges both for this GPU.
    NSString *batched = [root stringByAppendingPathComponent:@"batched"];
    if (sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/shader_pairs.tsv --out %@ --work %@/batch-work --batch-size 1 > %@/pso_batched.log 2>&1",
            tool, gpu, msl, tables, batched, root, root])) {
        NSString *log = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_batched.log"] encoding:NSUTF8StringEncoding error:nil];
        fprintf(stderr, "batched pso_offline.py failed:\n%s\n", log.UTF8String); return 1;
    }
    NSString *batchPath = [batched stringByAppendingFormat:@"/%@.mtl4a", gpu];
    EXPECT("batched archive: container accepted", 1, arch_file_ok(batchPath));
    id<MTL4Archive> batchArchive = arch_file_ok(batchPath) ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:batchPath] error:nil] : nil;
    EXPECT("batched archive: opened", 1, batchArchive != nil);
    EXPECT("batched archive: first group's base", 1, [batchArchive newRenderPipelineStateWithDescriptor:pre_desc(@"v1", @"f1", 0) error:nil] != nil);
    EXPECT("batched archive: second group's base", 1, [batchArchive newRenderPipelineStateWithDescriptor:pre_desc(@"v1", @"f2", 0) error:nil] != nil);
    EXPECT("batched archive: absent depth entry stays absent", 0, [batchArchive newRenderPipelineStateWithDescriptor:pre_desc(@"v1", @"f1", 1) error:nil] != nil);
    // Whether a vertex shader writes the layer is its reflection's layer_out (dmsh_entry.py layer_out), as shader_load takes it, not the layer's name in its library: one with layer
    // entry points of its own (entry_layer, vs_layer_entry) names it there, and its main0 pairs get a base. One whose main0 writes it (layer_out) gets none: its pipelines take an input
    // primitive class
    NSString *vl = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\nstruct L { float4 p [[position]]; uint l [[render_target_array_index]]; };\n"
        "vertex V main0(uint id [[vertex_id]]) { V o; o.p = float4(float(id & 1), float(id >> 1), 0, 1); return o; }\n"
        "vertex L main0_l(uint id [[vertex_id]]) { L o; o.p = float4(float(id & 1), float(id >> 1), 0, 1); o.l = 1; return o; }\n";
    if (!lib(msl, @"vl_b", vl) && !lib(msl, @"vw_b", vl)) {
        put([msl stringByAppendingPathComponent:@"vl_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"entry_layer\": \"main0_l\", \"vertex_inputs\": []}", DMSH_CHAIN]);
        put([msl stringByAppendingPathComponent:@"vw_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"layer_out\": true, \"vertex_inputs\": []}", DMSH_CHAIN]);
        EXPECT("prebuild: a library with layer entry points of its own gets main0's base", 1, pre_desc(@"vl", @"f1", 0) != nil);
        EXPECT("prebuild: a vertex shader whose main0 writes the layer (layer_out) gets none", 0, pre_desc(@"vw", @"f1", 0) != nil);
    }
    // The border corrections' constant in a pulling library's main0_z, its second function, at the near = 0 depth mode: metal-tt specializes only a library's first function
    // ("cannot find private metadata at offset '8'"), so pso_offline.py specializes it from a library of its own (needs_cut), and the base serves pre_desc through the whole library
    // (Nintendo Switch Sports Resort: 2,401 bases of 124 such libraries had left the archive refused)
    NSString *vb = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nconstant bool dm_border_off [[function_constant(%d)]];\n"
        "constant bool dm_border_live = !is_function_constant_defined(dm_border_off) || !dm_border_off;\nstruct V { float4 p [[position]]; };\n"
        "vertex V main0(uint id [[vertex_id]]) { V o; o.p = float4(float(id & 1), float(id >> 1), dm_border_live ? 0.25 : 0, 1); return o; }\n"
        "vertex V main0_z(uint id [[vertex_id]]) { V o; o.p = float4(float(id & 1), float(id >> 1), dm_border_live ? 0.75 : 0.5, 1); return o; }\n", DMSH_BORDER_FC];
    if (!lib(msl, @"vb_bp", vb)) {
        put([msl stringByAppendingPathComponent:@"vb_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"entry_z\": \"main0_z\", \"pull\": {}, \"vertex_inputs\": []}", DMSH_CHAIN]);
        put([tables stringByAppendingPathComponent:@"border.tsv"], @"vb\tf1\n");
        NSString *zero = [root stringByAppendingPathComponent:@"zero"];
        int rc = sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/border.tsv --out %@ --work %@/zero-work --depth zero --no-compute > %@/pso_zero.log 2>&1",
                     tool, gpu, msl, tables, zero, root, root]);
        EXPECT("border constant in main0_z: pso_offline.py made the archive", 0, rc);
        if (rc) fprintf(stderr, "%s\n", [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_zero.log"] encoding:NSUTF8StringEncoding error:nil].UTF8String);
        NSString *zeroPath = [zero stringByAppendingFormat:@"/%@.mtl4a", gpu];
        id<MTL4Archive> zeroArchive = arch_file_ok(zeroPath) ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:zeroPath] error:nil] : nil;
        EXPECT("border constant in main0_z: archive opened", 1, zeroArchive != nil);
        EXPECT("border constant in main0_z: the host's specialized base found", 1, [zeroArchive newRenderPipelineStateWithDescriptor:pre_desc(@"vb", @"f1", 1) error:nil] != nil);
        EXPECT("border constant in main0_z: no main0 base", 0, [zeroArchive newRenderPipelineStateWithDescriptor:pre_desc(@"vb", @"f1", 0) error:nil] != nil);
    }
    // Pixel shaders with dm_blend_off (alone and beside dm_border_off): pso_offline.py specializes dm_blend_off = false (and dm_border_off = true),
    // so both pre_desc and hybrid_get (FN_OFF_BORDER, shader_load's dm_blend_off = false) find the base in the archive, while FN_OFF_BLEND does not
    NSString *fb = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nconstant bool dm_border_off [[function_constant(%d)]];\n"
        "constant bool dm_blend_off [[function_constant(%d)]];\n"
        "fragment float4 main0() { return float4(is_function_constant_defined(dm_border_off) && dm_border_off ? 1 : 0, is_function_constant_defined(dm_blend_off) && dm_blend_off ? 1 : 0, 0, 1); }\n",
        DMSH_BORDER_FC, DMSH_BLEND_OFF_FC];
    NSString *fb0 = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nconstant bool dm_blend_off [[function_constant(%d)]];\n"
        "fragment float4 main0() { return float4(0, is_function_constant_defined(dm_blend_off) && dm_blend_off ? 1 : 0, 0, 1); }\n", DMSH_BLEND_OFF_FC];
    if (!lib(msl, @"fb_b", fb) && !lib(msl, @"fb0_b", fb0)) {
        put([msl stringByAppendingPathComponent:@"fb_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"blend_zero\": true}", DMSH_CHAIN]);
        put([msl stringByAppendingPathComponent:@"fb0_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"blend_zero\": true}", DMSH_CHAIN]);
        put([tables stringByAppendingPathComponent:@"blend.tsv"], @"v1\tfb\nv1\tfb0\n");
        NSString *bo = [root stringByAppendingPathComponent:@"blend"];
        int rc = sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/blend.tsv --out %@ --work %@/blend-work --depth gl --no-compute > %@/pso_blend.log 2>&1",
                     tool, gpu, msl, tables, bo, root, root]);
        EXPECT("blend constant: pso_offline.py made the archive", 0, rc);
        if (rc) fprintf(stderr, "%s\n", [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_blend.log"] encoding:NSUTF8StringEncoding error:nil].UTF8String);
        NSString *boPath = [bo stringByAppendingFormat:@"/%@.mtl4a", gpu];
        id<MTL4Archive> boArchive = arch_file_ok(boPath) ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:boPath] error:nil] : nil;
        EXPECT("blend constant: archive opened", 1, boArchive != nil);
        MTL4RenderPipelineDescriptor *db = pre_desc(@"v1", @"fb", 0), *db0 = pre_desc(@"v1", @"fb0", 0);
        EXPECT("blend+border: pre_desc base found", 1, db && [boArchive newRenderPipelineStateWithDescriptor:db error:nil] != nil);
        EXPECT("blend-only: pre_desc base found", 1, db0 && [boArchive newRenderPipelineStateWithDescriptor:db0 error:nil] != nil);
        id<MTLLibrary> vl = lib_from_file([msl stringByAppendingPathComponent:@"v1_bp.metallib"], NULL);
        id<MTLFunction> vf = fn4_note([vl newFunctionWithName:@"main0"], vl, @"main0", nil);
        for (NSString *fk in @[@"fb_b", @"fb0_b"]) {
            id<MTLLibrary> fl = lib_from_file([msl stringByAppendingFormat:@"/%@.metallib", fk], NULL);
            MTLFunctionConstantValues *fcv = [MTLFunctionConstantValues new]; BOOL no = NO; [fcv setConstantValue:&no type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC];
            id<MTLFunction> ff = fn_spec(fl, @"main0", fcv, nil);
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vf; d.fragmentFunction = ff; d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            MTL4RenderPipelineDescriptor *p0 = pso_desc4(d, 1, FN_OFF_BORDER), *p1 = pso_desc4(d, 1, FN_OFF_BORDER | FN_OFF_BLEND);
            EXPECT("hybrid stand-in (FN_OFF_BORDER) matches pair archive", 1, p0 && [boArchive newRenderPipelineStateWithDescriptor:p0 error:nil] != nil);
            EXPECT("steady-state blend-off (FN_OFF_BORDER|FN_OFF_BLEND) is distinct", 0, p1 && [boArchive newRenderPipelineStateWithDescriptor:p1 error:nil] != nil);
        }
    }
    EXPECT("batched archive: full coverage stamp preserved", 1, [[NSString stringWithContentsOfFile:[batched stringByAppendingPathComponent:@"prebuilt.tsv"] encoding:NSUTF8StringEncoding error:nil]
            isEqualToString:[NSString stringWithContentsOfFile:[bundle stringByAppendingPathComponent:@"prebuilt.tsv"] encoding:NSUTF8StringEncoding error:nil]]);
    // its stamp matches: only the pair with function constants is built here
    reset_counts();
    EXPECT("covered: one pair built on the device", 1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("covered: its base compiled", 1, g_pre_built);
    EXPECT("covered: nothing else", 0, g_pre_archived + g_pre_skipped + g_pre_failed);
    EXPECT("covered: again, nothing to do", -1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    // a draw of a covered pair (its pulling pipeline, as pso_desc_pull makes it) takes the base from the bundle: nothing compiled, nothing captured for the device's own archive
    @autoreleasepool {
        id<MTLLibrary> vl = lib_from_file([msl stringByAppendingPathComponent:@"v1_bp.metallib"], NULL), fl = lib_from_file([msl stringByAppendingPathComponent:@"f1_b.metallib"], NULL);
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn4_note([vl newFunctionWithName:@"main0"], vl, @"main0", nil); d.fragmentFunction = fn4_note([fl newFunctionWithName:@"main0"], fl, @"main0", nil);
        d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        uint64_t built0 = g_pb_built, arch0 = g_pb_archived; int new0 = g_arch_new; NSError *err = nil;
        EXPECT("covered draw: a pipeline", 1, pso_build(d, 0, &err) != nil);
        EXPECT("covered draw: its base from the bundle", arch0 + 1, g_pb_archived);
        EXPECT("covered draw: none compiled", built0, g_pb_built);
        EXPECT("covered draw: nothing captured for the device's archive", new0, g_arch_new);
    }
    // the near = 0 depth mode: not what the archive was made for, so the device's prebuild runs, and its main0_z bases are not in the archive
    [NSFileManager.defaultManager removeItemAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"] error:nil];
    reset_counts(); atomic_store(&g_depth_zero, 1);
    EXPECT("other depth mode: every pair", 3, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("other depth mode: every base compiled", 3, g_pre_built);
    atomic_store(&g_depth_zero, 0);
    // other pairs (a second list): the device's prebuild runs and takes the bundled bases it finds
    [NSFileManager.defaultManager removeItemAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"] error:nil];
    put([tables stringByAppendingPathComponent:@"more.tsv"], @"v1\tf2\nv1\tfc\n"); setenv("DM_PSO_PAIRS", [tables stringByAppendingPathComponent:@"more.tsv"].UTF8String, 1);
    reset_counts();
    EXPECT("other pairs: every pair", 3, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("other pairs: two bases from the bundle", 2, g_pre_archived);
    EXPECT("other pairs: one compiled", 1, g_pre_built);
    unsetenv("DM_PSO_PAIRS");
    // a matching stamp but an archive without the bases (here: one of the device's own files): the probe finds none, so it is not trusted
    @autoreleasepool {
        NSString *txt = [NSString stringWithContentsOfFile:[tables stringByAppendingPathComponent:@"shader_pairs.tsv"] encoding:NSUTF8StringEncoding error:nil];
        NSString *own = [arch stringByAppendingPathComponent:[[[NSFileManager.defaultManager contentsOfDirectoryAtPath:arch error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH 'p' AND self ENDSWITH '.mtl4a'"]] firstObject]];
        id<MTL4Archive> real = g_arch_bundle; NSError *e = nil;
        g_arch_bundle = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:own] error:&e];
        EXPECT("archive guard: real serialized archive accepted", 1, arch_file_ok(own));
        arch_reads rd = {0}; EXPECT("archive reads: serialized archive checked", 1, arch_file_check(own, &rd));
        EXPECT("archive reads: serialized archive, records and libraries", 1, rd.n >= 2);
        EXPECT("archive reads: serialized archive opens", 1, arch_open(own, &e) != nil);
        EXPECT("probe: another archive opened", 1, g_arch_bundle != nil);
        EXPECT("probe: its bases are not this run's", 0, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 0), 0, rest));
        g_arch_bundle = real;
        EXPECT("probe: the bundled archive holds them", 1, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 0), 0, rest));
        EXPECT("probe: not for the other depth mode", 0, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 1), 1, rest));
        // Raw-pair sampling can hit only ineligible pairs: 0 of 0 lookups must not suppress the device prebuild.
        NSMutableString *sparse = [NSMutableString string];
        for (int i = 0; i < PRE_PROBE; i++) [sparse appendFormat:@"missing%d\tf1\n", i];
        [sparse appendString:@"v1\tf1\n"];
        put([bundle stringByAppendingPathComponent:@"prebuilt.tsv"], pre_stamp(sparse, 0));
        EXPECT("probe: no eligible sample cannot establish coverage", 0, pso_bundle_covers(pre_parse(sparse), pre_stamp(sparse, 0), 0, nil));
        put([bundle stringByAppendingPathComponent:@"prebuilt.tsv"], pre_stamp(txt, 0));
    }
    // a folder without this GPU's archive, and a file that is no archive (Metal crashed on one, and on a bare Mach-O header too): no archive, no crash
    @autoreleasepool {
        id<MTL4Archive> real = g_arch_bundle; NSString *other = [root stringByAppendingPathComponent:@"other"];
        put([other stringByAppendingPathComponent:@"applegpu_g99x.mtl4a"], @"x");
        NSMutableArray *list = [NSMutableArray array]; g_arch_bundle = nil;
        setenv("DM_PSO_BUNDLE", other.UTF8String, 1); pso_bundle_open(list, arch_plan(nil));
        EXPECT("no file for this GPU: none opened", 0, list.count + (g_arch_bundle != nil));
        put([other stringByAppendingFormat:@"/%@.mtl4a", gpu], @"not an archive");   // Metal's own loadFromURL crashed on it: never handed to Metal
        pso_bundle_open(list, arch_plan(nil));
        EXPECT("not an archive: none opened", 0, list.count + (g_arch_bundle != nil));
        EXPECT("not an archive: refused before Metal", 0, arch_file_ok([other stringByAppendingFormat:@"/%@.mtl4a", gpu]));
        setenv("DM_PSO_BUNDLE", "off", 1); pso_bundle_open(list, arch_plan(nil));
        EXPECT("off: none opened", 0, list.count);
        g_arch_bundle = real;
    }
    // The entry points of a vertex shader's pulling and shadow-input libraries are the ones its reflection names under `pull` and `vin` (side_entry), which need not be the shader's
    // own: one compile unit that holds a shader's entry points and its pulling ones cannot name both main0. shader_load takes the names for pull_load and vin_fn, pre_desc for the
    // base, and pso_offline.py (vertex_entry) puts that base into the archive. A reflection that names none there means the shader's own names, as the libraries have them today
    @autoreleasepool {
        NSString *(^vsrc)(NSArray<NSString *> *) = ^(NSArray<NSString *> *names) {
            NSMutableString *s = [@"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n" mutableCopy]; int i = 0;
            for (NSString *n in names) [s appendFormat:@"vertex V %@(uint id [[vertex_id]]) { V o; o.p = float4(float(id & 1), float(id >> 1), %d * 0.125, 1); return o; }\n", n, i++];
            return s; };
        NSArray *ownNames = @[@"main0", @"main0_z"], *ownPull = @[@"main0", @"main0_z", @"main0_wl", @"main0_z_wl"];
        if (!lib(msl, @"vn_b", vsrc(ownNames)) && !lib(msl, @"vn_bp", vsrc(@[@"main0_p", @"main0_zp", @"main0_wl", @"main0_z_wl"])) && !lib(msl, @"vn_bv", vsrc(@[@"main0_v", @"main0_zv"])) &&
            !lib(msl, @"vo_b", vsrc(ownNames)) && !lib(msl, @"vo_bp", vsrc(ownPull)) && !lib(msl, @"vo_bv", vsrc(ownNames))) {
            NSString *refl = @"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"entry\": \"main0\", \"entry_z\": \"main0_z\", \"vertex_inputs\": [{\"location\": 0, \"msl_type\": \"uint4\"}], "
                "\"pull\": {%@\"slot\": 16, \"inputs\": [0]}, \"vin\": {%@\"shadow\": [{\"location\": 0, \"float\": 16, \"sint\": 17}]}}";
            put([msl stringByAppendingPathComponent:@"vn_b.json"], [NSString stringWithFormat:refl, DMSH_CHAIN,
                @"\"entry\": \"main0_p\", \"entry_z\": \"main0_zp\", \"entry_wl\": \"main0_wl\", \"entry_z_wl\": \"main0_z_wl\", ", @"\"entry\": \"main0_v\", \"entry_z\": \"main0_zv\", "]);
            put([msl stringByAppendingPathComponent:@"vo_b.json"], [NSString stringWithFormat:refl, DMSH_CHAIN, @"", @""]);   // names none
            for (int z = 0; z < 2; z++) {
                atomic_store(&g_depth_zero, z);
                NvShader *vn = shader_load(@"vn_b", 0, 0), *vo = shader_load(@"vo_b", 0, 0);
                EXPECT("entry names: shaders loaded", 1, vn && vn->fn && vo && vo->fn);
                if (!vn || !vo) continue;
                EXPECT("named: the shader's own entry", 1, [vn->fn.name isEqualToString:z ? @"main0_z" : @"main0"]);
                EXPECT("named: the pulling entry", 1, [pull_fn(vn).name isEqualToString:z ? @"main0_zp" : @"main0_p"]);
                EXPECT("named: the wide-line entry", 1, [vn->wlfn.name isEqualToString:z ? @"main0_z_wl" : @"main0_wl"]);
                EXPECT("named: the shadow-input entry", 1, [vin_fn(vn).name isEqualToString:z ? @"main0_zv" : @"main0_v"]);
                EXPECT("unnamed: the pulling entry has the shader's own name", 1, [pull_fn(vo).name isEqualToString:z ? @"main0_z" : @"main0"]);
                EXPECT("unnamed: the wide-line entry", 1, [vo->wlfn.name isEqualToString:z ? @"main0_z_wl" : @"main0_wl"]);
                EXPECT("unnamed: the shadow-input entry", 1, [vin_fn(vo).name isEqualToString:z ? @"main0_z" : @"main0"]);
            }
            atomic_store(&g_depth_zero, 0);
            put([tables stringByAppendingPathComponent:@"names.tsv"], @"vn\tf1\n");
            NSString *named = [root stringByAppendingPathComponent:@"named"];
            int rc = sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/names.tsv --out %@ --work %@/named-work --no-compute > %@/pso_named.log 2>&1",
                         tool, gpu, msl, tables, named, root, root]);
            EXPECT("named: pso_offline.py made the archive", 0, rc);
            if (rc) fprintf(stderr, "%s\n", [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_named.log"] encoding:NSUTF8StringEncoding error:nil].UTF8String);
            NSString *namedPath = [named stringByAppendingFormat:@"/%@.mtl4a", gpu];
            id<MTL4Archive> namedArchive = arch_file_ok(namedPath) ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:namedPath] error:nil] : nil;
            MTL4RenderPipelineDescriptor *d0 = pre_desc(@"vn", @"f1", 0), *d1 = pre_desc(@"vn", @"f1", 1);
            EXPECT("named: archive opened", 1, namedArchive != nil);
            EXPECT("named: the prebuild's bases name the pulling library's entry points", 1, d0 != nil && d1 != nil);
            EXPECT("named: the host's base found in the archive", 1, d0 && [namedArchive newRenderPipelineStateWithDescriptor:d0 error:nil] != nil);
            EXPECT("named: no base of the near = 0 entry", 0, d1 && [namedArchive newRenderPipelineStateWithDescriptor:d1 error:nil] != nil);
        }
    }
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_bundle: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
