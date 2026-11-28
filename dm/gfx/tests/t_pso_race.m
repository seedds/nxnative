// Stress of the archive's capture path (arch_capture, which waits for a function under way as the draws and prewarm lists do; the prebuild puts such a pair back instead): many
// workers capture pipelines that share stage functions at the same time, through the
// current flush period's compiler, with real Metal and one metallib made from MSL (no game shaders), bases as the prebuild makes them (pre_desc; --specialized: one BGRA8 target).
// It measures what fn_claim and the prebuild's order cost: the time, and how many captures waited. It has not reproduced the crash (rendering.md, Two captures of one shader
// function crash in Metal's binary cache): 0 of 30 unguarded runs on an M4 Pro across these options, with shaders this small. One run is one process, so a crash would be its exit status.
//   t_pso_race [-n pairs] [--unguarded] [--order list|grouped] [--pairs shader_pairs.tsv] [--run pairs] [--work statements] [--specialized]
// --unguarded: no fn_claim, as before it existed. Without --pairs, every pair has the same vertex function and its own pixel function; with it, the list's pairs, each key
// a function of its own. --run: a new vertex function each that many pairs; --work: statements in each vertex function, so it compiles longer. Prints one line: pairs, built, failed, seconds, captures that waited (g_fn_waits), of them built without the archive after the wait (g_fn_plain)
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(int argc, char **argv) { @autoreleasepool {
    int n = 2000, grouped = 0, run = 0, work = 0, unspec = 1; const char *list = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--specialized")) unspec = 0;
        else if (!strcmp(argv[i], "--unguarded")) g_fn_guard = 0;
        else if (!strcmp(argv[i], "--order") && i + 1 < argc) grouped = !strcmp(argv[++i], "grouped");
        else if (!strcmp(argv[i], "--pairs") && i + 1 < argc) list = argv[++i];
        else if (!strcmp(argv[i], "--run") && i + 1 < argc) run = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) work = atoi(argv[++i]);
        else { fprintf(stderr, "usage: t_pso_race [-n pairs] [--unguarded] [--order list|grouped] [--pairs shader_pairs.tsv] [--run pairs] [--work statements]\n"); return 2; }
    }
    // the pairs, as function indices
    NSMutableArray<NSNumber *> *pv = [NSMutableArray array], *pf = [NSMutableArray array]; int nv = 1, nf = 0;
    if (list) {
        NSMutableDictionary<NSString *, NSNumber *> *vi = [NSMutableDictionary dictionary], *fi = [NSMutableDictionary dictionary];
        NSString *txt = [NSString stringWithContentsOfFile:@(list) encoding:NSUTF8StringEncoding error:nil];
        if (!txt) { fprintf(stderr, "t_pso_race: cannot read %s\n", list); return 2; }
        for (NSArray<NSString *> *p in pre_parse(txt)) {
            if ((int)pv.count >= n) break;
            if (!vi[p[0]]) vi[p[0]] = @(vi.count);
            if (!fi[p[1]]) fi[p[1]] = @(fi.count);
            [pv addObject:vi[p[0]]]; [pf addObject:fi[p[1]]];
        }
        nv = (int)vi.count; nf = (int)fi.count; n = (int)pv.count;
    } else { for (int i = 0; i < n; i++) { [pv addObject:@(run > 0 ? i / run : 0)]; [pf addObject:@(i)]; } nf = n; nv = run > 0 ? (n + run - 1) / run : 1; }

    char tmp[] = "/tmp/t_pso_race.XXXXXX"; if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *tables = [root stringByAppendingPathComponent:@"tables"];
    [NSFileManager.defaultManager createDirectoryAtPath:tables withIntermediateDirectories:YES attributes:nil error:nil];
    [@"# empty\n" writeToFile:[tables stringByAppendingPathComponent:@"shader_map.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    setenv("DM_NVN_SHMAP", [tables stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1);
    setenv("DM_MSL_DIR", [root stringByAppendingPathComponent:@"msl"].UTF8String, 1);
    setenv("DM_PSO_ARCHIVE", [root stringByAppendingPathComponent:@"archive"].UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init(); pso4_init();
    if (!g_comp4 || !g_arch_cap) { printf("t_pso_race: no Metal 4 compiler, not run\n"); return 0; }

    // one library, every function salted by the process so the system's shader cache holds none of them
    unsigned salt = (unsigned)getpid() * 2654435761u;
    NSMutableString *src = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; float4 c0 [[user(c0)]]; float4 c1 [[user(c1)]]; float4 c2 [[user(c2)]]; float4 c3 [[user(c3)]]; };\n"];
    for (int i = 0; i < nv; i++) {   // work: statements that make each vertex function take longer to compile, as a game's do
        [src appendFormat:@"vertex V v%d(uint id [[vertex_id]], constant float4 *u [[buffer(0)]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); float4 a = float4(uv, %u.0 / 4294967296.0, %d);\n", i, salt, i];
        for (int w = 0; w < work; w++) [src appendFormat:@" a = fma(a, u[%d], sin(a.yzwx * %d.0)) + (a.x > u[%d].y ? cos(a) : a * u[%d]);\n", w % 64, w + i, (w * 7) % 64, (w * 13) % 64];
        [src appendString:@" o.p = float4(uv * 2 - 1, 0, 1); o.c0 = a; o.c1 = a.yzwx; o.c2 = a.zwxy; o.c3 = a.wxyz; return o; }\n"];
    }
    // each pixel function reads its own set of the vertex outputs, so the pairs of one vertex function make different vertex programs, as a game's pairs do
    for (int i = 0; i < nf; i++) {
        int m = i % 15 + 1; NSMutableString *in = [NSMutableString string], *sum = [NSMutableString stringWithString:@"float4(0)"];
        for (int c = 0; c < 4; c++) if (m >> c & 1) { [in appendFormat:@" float4 c%d [[user(c%d)]];", c, c]; [sum appendFormat:@" + i.c%d", c]; }
        [src appendFormat:@"struct F%d { float4 p [[position]];%@ };\nfragment float4 f%d(F%d i [[stage_in]]) { float4 s = %@; return s * float4(%d, 1, %u.0 / 4294967296.0, 1) + sin(s.x * %d.0); }\n",
            i, in, i, i, sum, i, salt, i + 1];
    }
    // a metallib from the SDK's compiler, loaded from its bytes as the host loads a game's (lib_from_file); the run-time compiler when there is no SDK compiler
    NSError *le = nil; id<MTLLibrary> lib = nil; NSString *ms = [root stringByAppendingPathComponent:@"race.metal"];
    [src writeToFile:ms atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
    task.arguments = @[@"-sdk", @"macosx", @"metal", ms, @"-o", [root stringByAppendingPathComponent:@"race.metallib"]];
    int sdk = [task launchAndReturnError:nil]; if (sdk) { [task waitUntilExit]; sdk = task.terminationStatus == 0; }
    if (sdk) {
        NSData *d = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"race.metallib"]];
        lib = lib_tag([g_dev newLibraryWithData:dispatch_data_create(d.bytes, d.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&le], @"race");
    } else lib = [g_dev newLibraryWithSource:src options:nil error:&le];
    if (!lib) { fprintf(stderr, "t_pso_race: library: %s\n", le.localizedDescription.UTF8String); return 1; }

    int workers = compile_tasks(); NSMutableArray<NSNumber *> *ord = [NSMutableArray array];   // grouped: the prebuild's order (pre_order)
    if (grouped) { NSMutableArray *pp = [NSMutableArray array]; for (int i = 0; i < n; i++) [pp addObject:@[[NSString stringWithFormat:@"v%05d", pv[i].intValue], @""]]; [ord setArray:pre_order(pp)]; }
    else for (int i = 0; i < n; i++) [ord addObject:@(i)];
    __block _Atomic int next = 0, built = 0, failed = 0; uint64_t t0 = bp_now();
    dispatch_group_t g = dispatch_group_create();
    for (int t = 0; t < workers; t++) dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (int i; (i = atomic_fetch_add(&next, 1)) < n;) @autoreleasepool {
            NSUInteger p = ord[(NSUInteger)i].unsignedIntegerValue;
            MTL4RenderPipelineDescriptor *d = [MTL4RenderPipelineDescriptor new];
            MTL4LibraryFunctionDescriptor *vf = [MTL4LibraryFunctionDescriptor new], *ff = [MTL4LibraryFunctionDescriptor new];
            vf.library = lib; vf.name = [NSString stringWithFormat:@"v%d", pv[p].intValue]; ff.library = lib; ff.name = [NSString stringWithFormat:@"f%d", pf[p].intValue];
            d.vertexFunctionDescriptor = vf; d.fragmentFunctionDescriptor = ff;
            if (unspec) {   // a base as the prebuild makes it (pre_desc): no vertex layout, every colour attachment left to the specialization
                d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; d.rasterSampleCount = 1;
                for (int c = 0; c < 8; c++) { MTL4RenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[c];
                    a.pixelFormat = MTLPixelFormatUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized; a.blendingState = MTL4BlendStateUnspecialized;
                    a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
                    a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized; }
            } else d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            NSError *e = nil;
            if (arch_capture(d, &e, 0, NULL)) built++; else if (failed++ < 2) fprintf(stderr, "t_pso_race: pair %lu failed: %s\n", (unsigned long)p, e.localizedDescription.UTF8String);
        }
    });
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    double s = (bp_now() - t0) / 1e9;
    arch_flush(); arch_flush_wait();
    printf("t_pso_race: %s order, %s, %d workers, %d pairs (%d vertex, %d pixel functions): %d built, %d failed, %.1f s, %d captures waited, %d built without the archive\n", grouped ? "grouped" : "list",
           g_fn_guard ? "guarded" : "unguarded", workers, n, nv, nf, (int)built, (int)failed, s, (int)atomic_load(&g_fn_waits), (int)atomic_load(&g_fn_plain));
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    return built == n ? 0 : 1;
} }
