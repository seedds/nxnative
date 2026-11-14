// The prebuild tool's prewarm lists (DM_PSO_PREBUILD=tool, mtl_pso_prebuild_tool, pre_warm_lists) and the files its bundle form (DM_PSO_PREBUILD_OUT) makes for the app (<GPU>-prewarm-<NN>.mtl4a,
// pso_bundle_open_prewarm, and prewarm-<NN>.mtl4-json, each period as a pipelines script), on synthetic shaders with a vertex layout. Checks: a pso_key written as a prewarm row (pso_skey) decodes to the same Metal descriptor the draw's own key gives (vertex attribute format, offset, stream and stride),
// so the base the tool captures is the base a run asks for; the tool writes one file per flush period (g_arch_period_max lowered to 2: 5 bases, 3 files) and a stamp with the depth
// mode, its device form writes the lists' bases into the device's archive with no scripts, and DM_PSO_PREBUILD_WARM=0 leaves the lists out; and the host opens those files with the bundle and searches them first among the files of bases with a vertex layout, before the device's own archive, skips a file that is
// no archive, takes the keys of a file's index (<file>.keys, which the tool puts beside it) into the archive's index and a file without one into the files of bases with a vertex
// layout, finds the tool's bases without compiling, finds the device's own bases after them, and compiles what neither holds.
// The tool and the open run in processes of their own (the archive opens once per process): this binary runs itself with --tool and --open.
// Without the Metal compiler (xcrun metal) it says so and passes
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w_ = (uint64_t)(want), g_ = (uint64_t)(got); checks++; \
    if (w_ != g_) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); failed++; } } while (0)

static void put(NSString *path, NSString *text) { [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int fixtures(NSString *msl, NSString *tables) {
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VIn { float4 p [[attribute(0)]]; };\n"
        "vertex float4 vs(VIn in [[stage_in]]) { return in.p; }\n"
        "fragment float4 fs() { return float4(0.5, 0.25, 1, 0.5); }\n"
        "kernel void cs(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i; }\n";
    put([msl stringByAppendingPathComponent:@"warm-vs.metal"], src);
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"]; t.currentDirectoryURL = [NSURL fileURLWithPath:msl];
    t.arguments = @[@"-sdk", @"macosx", @"metal", @"warm-vs.metal", @"-o", @"warm-vs.metallib"]; t.standardOutput = NSFileHandle.fileHandleWithNullDevice; t.standardError = NSFileHandle.fileHandleWithNullDevice;
    if (![t launchAndReturnError:nil]) return 0;
    [t waitUntilExit]; if (t.terminationStatus) return 0;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm copyItemAtPath:[msl stringByAppendingPathComponent:@"warm-vs.metallib"] toPath:[msl stringByAppendingPathComponent:@"warm-fs.metallib"] error:nil]) return 0;
    if (![fm copyItemAtPath:[msl stringByAppendingPathComponent:@"warm-vs.metallib"] toPath:[msl stringByAppendingPathComponent:@"warm-cs.metallib"] error:nil]) return 0;
    [fm removeItemAtPath:[msl stringByAppendingPathComponent:@"warm-vs.metal"] error:nil];
    put([msl stringByAppendingPathComponent:@"warm-vs.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"entry\": \"vs\", \"entry_z\": \"vs\", \"bindings\": [], \"vertex_inputs\": [{\"location\": 0, \"msl_type\": \"float4\"}]}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"warm-fs.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"entry\": \"fs\", \"bindings\": [], \"vertex_inputs\": []}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"warm-cs.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"entry\": \"cs\", \"bindings\": []}", DMSH_CHAIN]);
    put([tables stringByAppendingPathComponent:@"shader_map.tsv"], @"package\tindex\tstage\tdm_ctl_sha16\tsource\np.bin\t0\tvertex\twarm-vs\tx\np.bin\t1\tpixel\twarm-fs\tx\np.bin\t2\tcompute\twarm-cs\tx\n");
    return 1;
}
static int float4_fmt(void) { for (int f = 1; f < 256; f++) if (vfmt(f) == MTLVertexFormatFloat4) return f; return 0; }
// The key a draw with one float4 attribute at this offset (stream 0, stride 32) makes
static pso_key key_at(NvShader *vs, NvShader *fs, unsigned offset) {
    state_reset(); S.nattr = 1; S.attr[0] = (va_st){.fmt = (uint32_t)float4_fmt(), .offset = offset, .stream = 0}; S.stream[0] = (vs_st){.stride = 32};
    uint32_t cfmt[8] = {MTLPixelFormatRGBA8Unorm}; pso_key k; pso_key_fill(&k, vs, fs, cfmt, 0, 0, 1, 0); return k;
}
static NSString *row_of(const pso_key *k) {
    NvShader *vs = [NvShader new], *fs = [NvShader new]; strcpy(vs->key, "warm-vs"); strcpy(fs->key, "warm-fs"); fs->stage = 1;
    pso_skey sk; char hex[sizeof sk * 2 + 1]; pso_skey_from(k, vs, fs, 0, NULL, &sk); pso_skey_hex(&sk, hex);
    return [NSString stringWithFormat:@"R\t%s\n", hex];
}
static MTLRenderPipelineDescriptor *desc_of(const pso_key *raw, NvShader *vs, NvShader *fs) { pso_key k = *raw; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; return pso_desc(&k, vs, fs, nil, 0); }
static int run_self(NSArray<NSString *> *args, NSDictionary<NSString *, NSString *> *env, NSString *log) {   // this binary again, with the log in a file
    NSMutableDictionary *e = [NSProcessInfo.processInfo.environment mutableCopy]; [e addEntriesFromDictionary:@{@"DM_MSL_DIR": @(getenv("DM_MSL_DIR")), @"DM_NVN_SHMAP": @(getenv("DM_NVN_SHMAP")), @"DM_NVN_PREDICT": @"0"}]; [e addEntriesFromDictionary:env];
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:NSProcessInfo.processInfo.arguments[0]]; t.arguments = args; t.environment = e;
    [NSFileManager.defaultManager createFileAtPath:log contents:nil attributes:nil]; NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:log]; t.standardOutput = h; t.standardError = h;
    NSError *err = nil; if (![t launchAndReturnError:&err]) return -1;
    [t waitUntilExit]; return t.terminationStatus;
}
static NSArray<NSString *> *mtl4a(NSString *dir) { return [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]] sortedArrayUsingSelector:@selector(compare:)]; }
static NSString *slurp(NSString *p) { return [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil] ?: @""; }

// --open <root>: the app's side. The bundle holds the tool's bases (offsets 0 and 16 to 28) in 3 files (the last one without its index) and one file that is no archive, the device's archive holds offset 4
static int open_run(NSString *root) {
    NvShader *vs = shader_load(@"warm-vs", 0, 0), *fs = shader_load(@"warm-fs", 1, 0);
    EXPECT("open: shaders loaded", 1, vs && fs);
    if (!vs || !fs) return 1;
    pso4_init();
    EXPECT("open: no pairs bundle for this GPU, so none", 1, g_arch_bundle == nil);
    EXPECT("open: the 3 prewarm files opened, the junk file left out", 3, g_prewarm_files);
    EXPECT("open: the file with no index first among the files of bases with a vertex layout, then the device's file", 2, g_arch4.count);
    EXPECT("open: the 3 prewarm files kept open", 3, g_prewarm_open.count);
    EXPECT("open: the other two are named by the index", 1, g_arch_ix.n > 0 && ![g_arch4 containsObject:g_prewarm_open[0]] && ![g_arch4 containsObject:g_prewarm_open[1]] && g_arch4[0] == g_prewarm_open[2]);
    int in_bundle[40], in_device[40]; memset(in_bundle, 0, sizeof in_bundle); memset(in_device, 0, sizeof in_device);
    unsigned offs[] = {0, 4, 8, 16, 20, 24, 28}; int named_n = 0;
    for (unsigned o = 0; o < sizeof offs / sizeof offs[0]; o++) {
        pso_key k = key_at(vs, fs, offs[o]); MTL4RenderPipelineDescriptor *b = pso_desc4(desc_of(&k, vs, fs), 1, !k.border);
        for (id<MTL4Archive> a in g_prewarm_open) if ([a newRenderPipelineStateWithDescriptor:b error:nil]) in_bundle[offs[o]]++;
        for (NSUInteger i = g_arch4.count - 1; i < g_arch4.count; i++) if ([g_arch4[i] newRenderPipelineStateWithDescriptor:b error:nil]) in_device[offs[o]]++;
        uint64_t key = arch_key(b); id<MTL4Archive> named = key ? (__bridge id<MTL4Archive>)nvn_hm_get(&g_arch_ix, &key) : nil;
        int bundled = offs[o] != 4 && offs[o] != 8;   // 5 bases in files of 2, 2 and 1: the index names 4 of them, the file without one holds the fifth
        if (named) { named_n++; EXPECT("open: the index names a prewarm file that holds the base", 1, [named newRenderPipelineStateWithDescriptor:b error:nil] != nil && [g_prewarm_open containsObject:named] && named != g_prewarm_open[2]); }
        else if (bundled) EXPECT("open: the base with no index entry is in the first of the files of bases with a vertex layout", 1, [g_arch4[0] newRenderPipelineStateWithDescriptor:b error:nil] != nil);
    }
    for (unsigned o = 0; o < sizeof offs / sizeof offs[0]; o++) {
        int bundled = offs[o] != 4 && offs[o] != 8;
        EXPECT("open: a tool base is in one prewarm file", bundled, in_bundle[offs[o]]);
        EXPECT("open: only the device's own base is in the device's file", offs[o] == 4, in_device[offs[o]]);
    }
    EXPECT("open: the index names 4 of the 5 tool bases", 4, named_n);
    uint64_t built0 = g_pb_built, arch0 = g_pb_archived; NSError *err = nil;
    for (unsigned o = 0; o < sizeof offs / sizeof offs[0]; o++) {
        pso_key k = key_at(vs, fs, offs[o]); NSString *nm = [NSString stringWithFormat:@"open: a pipeline for the attribute at %u", offs[o]];
        EXPECT(nm.UTF8String, 1, pso_build(desc_of(&k, vs, fs), !k.border, &err) != nil);
    }
    EXPECT("open: 6 bases taken from the files", 6, g_pb_archived - arch0);
    EXPECT("open: only the one in no file compiled", 1, g_pb_built - built0);
    (void)root;
    printf("t_pso_prewarm open: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}

int main(int argc, char **argv) { @autoreleasepool {
    alarm(240);
    unsetenv("DM_NVN_HILITE"); unsetenv("DM_NVN_MAGSCAN"); unsetenv("DM_PSO_RECORD"); unsetenv("DM_PSO_USER"); unsetenv("DM_SAVE"); setenv("DM_NVN_PREDICT", "0", 1);
    if (argc >= 3 && !strcmp(argv[1], "--tool")) {   // DM_PSO_PREBUILD=tool, the flush period as small as a test can make it
        if (getenv("T_PERIOD")) g_arch_period_max = atoi(getenv("T_PERIOD"));
        // T_HARVEST_HOLD=<ms>: the harvest queue (arch_harvest) busy that long, as a slow capture of a compute pipeline the MTLDevice built makes it
        if (getenv("T_HARVEST_HOLD")) { useconds_t us = (useconds_t)atoi(getenv("T_HARVEST_HOLD")) * 1000; dispatch_async(arch_harvest_q(), ^{ usleep(us); }); }
        return mtl_pso_prebuild_tool() == 1 ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "--open")) { mtl_init(); shmap_load(); return open_run(@(argv[2])); }
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s/t_pso_prewarm.XXXXXX", getenv("TMPDIR") ?: "/tmp"); if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *msl = [root stringByAppendingFormat:@"/msl/b-%s", DMSH_CHAIN], *tables = [root stringByAppendingPathComponent:@"tables"];
    if (!fixtures(msl, tables)) { printf("t_pso_prewarm: no Metal compiler (xcrun metal), not run\n"); [NSFileManager.defaultManager removeItemAtPath:root error:nil]; return 0; }
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_MSL_DIR", [root stringByAppendingPathComponent:@"msl"].UTF8String, 1); setenv("DM_NVN_SHMAP", [tables stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init(); shmap_load();
    NSString *gpu = g_dev.architecture.name;
    NvShader *vs = shader_load(@"warm-vs", 0, 0), *fs = shader_load(@"warm-fs", 1, 0);
    EXPECT("fixture shaders loaded", 1, vs && fs);
    if (!vs || !fs) return 1;
    // skey -> descriptor: the row a run records (pso_skey_from of the draw's key), parsed as warm_parse does, gives the draw's own descriptor and base key
    for (unsigned off = 0; off < 24; off += 8) {
        pso_key live = key_at(vs, fs, off);
        NSString *row = row_of(&live); NSString *list = [root stringByAppendingPathComponent:@"one.tsv"]; put(list, row);
        long lines = 0; NSArray<WarmTask *> *tasks = warm_parse(@[list], &lines);
        EXPECT("round trip: one task of one row", 1, tasks.count == 1 && lines == 1 && tasks[0]->kind == 1 && tasks[0]->rs.count == 1);
        if (tasks.count != 1) continue;
        pso_skey sk; [tasks[0]->rs[0] getBytes:&sk length:sizeof sk];
        EXPECT("round trip: shader names", 1, !strcmp(sk.vs, "warm-vs") && !strcmp(sk.fs, "warm-fs"));
        pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs;
        MTLRenderPipelineDescriptor *a = pso_desc(&live, vs, fs, nil, 0), *b = pso_desc(&k, vs, fs, nil, 0);
        MTLVertexAttributeDescriptor *va = b.vertexDescriptor.attributes[0]; MTLVertexBufferLayoutDescriptor *vl = b.vertexDescriptor.layouts[VB_SLOT(0)];
        EXPECT("round trip: vertex format", MTLVertexFormatFloat4, va.format);
        EXPECT("round trip: vertex offset", off, va.offset);
        EXPECT("round trip: buffer slot", VB_SLOT(0), va.bufferIndex);
        EXPECT("round trip: stride", 32, vl.stride);
        pbase_key ka, kb; pbase_key_of(a, 0, &ka); pbase_key_of(b, 0, &kb);
        EXPECT("round trip: the same base key", 0, memcmp(&ka, &kb, sizeof ka));
        MTL4RenderPipelineDescriptor *ma = pso_desc4(a, 1, 0), *mb = pso_desc4(b, 1, 0);
        EXPECT("round trip: a Metal 4 descriptor", 1, ma && mb);
        EXPECT("round trip: the colour state left open", MTLPixelFormatUnspecialized, mb.colorAttachments[0].pixelFormat);
    }
    // the tool: 5 bases with a period of 2 captures
    NSString *bundle = [root stringByAppendingPathComponent:@"bundle"], *work1 = [root stringByAppendingPathComponent:@"work1"];
    NSString *rows = @"";
    const unsigned tool_offs[] = {0, 16, 20, 24, 28};
    for (unsigned i = 0; i < 5; i++) { pso_key k = key_at(vs, fs, tool_offs[i]); rows = [rows stringByAppendingString:row_of(&k)]; }
    put([tables stringByAppendingPathComponent:@"pso_manifest.tsv"], [@"S\twarm-vs\t0\nC\tnot-in-the-map\n" stringByAppendingString:rows]);   // the default list, beside the shader map; S and stale C rows build nothing
    NSDictionary *tool = @{@"DM_PSO_PREBUILD": @"tool", @"DM_PSO_ARCHIVE": work1, @"DM_PSO_PREBUILD_OUT": bundle, @"DM_PSO_MANIFEST": @"", @"T_PERIOD": @"2", @"DM_PSO_BUNDLE": @"off"};
    EXPECT("tool: exit 0", 0, run_self(@[@"--tool", root], tool, [root stringByAppendingPathComponent:@"tool1.log"]));
    NSString *log1 = slurp([root stringByAppendingPathComponent:@"tool1.log"]);
    NSArray<NSString *> *files = mtl4a(bundle);
    EXPECT("tool: 5 bases, 2 to a period: 3 files", 3, files.count);
    for (NSUInteger i = 0; i < files.count; i++) {
        NSString *want = [NSString stringWithFormat:@"%@-prewarm-%02lu.mtl4a", gpu, (unsigned long)i];
        EXPECT("tool: file name", 1, [files[i] isEqualToString:want]);
        EXPECT("tool: file is an archive", 1, arch_file_ok([bundle stringByAppendingPathComponent:files[i]]));
    }
    EXPECT("tool: log names the bases", 1, [log1 containsString:@"5 render pipelines (bases 5 compiled, 0 from the archives)"]);
    NSString *stamp = slurp([bundle stringByAppendingPathComponent:@"prewarm.tsv"]);
    EXPECT("tool: stamp, depth near=-w", 1, [stamp containsString:@"depth\tnear=-w\n"] && [stamp containsString:@"files\t3\n"] && [stamp containsString:@"lines\t5\n"]);
    for (NSUInteger i = 0; i < files.count; i++) {   // each period is also a pipelines script, for metal-tt and the other GPUs
        NSData *js = [NSData dataWithContentsOfFile:[bundle stringByAppendingFormat:@"/prewarm-%02lu.mtl4-json", (unsigned long)i]];
        id j = js ? [NSJSONSerialization JSONObjectWithData:js options:0 error:nil] : nil;
        EXPECT("tool: each file has its pipelines script, with its libraries", 1, [j isKindOfClass:NSDictionary.class] && [j[@"libraries"] count] > 0);
    }
    // the tool again on the same work folder: the bases are in its files, nothing is compiled, the output is the same
    EXPECT("tool again: exit 0", 0, run_self(@[@"--tool", root], tool, [root stringByAppendingPathComponent:@"tool1b.log"]));
    NSString *log1b = slurp([root stringByAppendingPathComponent:@"tool1b.log"]);
    EXPECT("tool again: none compiled", 1, [log1b containsString:@"(bases 0 compiled, 5 from the archives)"]);
    EXPECT("tool again: same files", 3, mtl4a(bundle).count);
    // the device's own base (offset 4), made by the tool for the zero depth mode into another folder and used as a device archive file
    NSString *devout = [root stringByAppendingPathComponent:@"devout"], *dev = [root stringByAppendingPathComponent:@"dev"], *list2 = [root stringByAppendingPathComponent:@"two.tsv"];
    { pso_key k = key_at(vs, fs, 4); put(list2, row_of(&k)); }
    NSDictionary *tool2 = @{@"DM_PSO_PREBUILD": @"tool", @"DM_PSO_ARCHIVE": [root stringByAppendingPathComponent:@"work2"], @"DM_PSO_PREBUILD_OUT": devout, @"DM_PSO_MANIFEST": list2,
                            @"DM_PSO_PREBUILD_DEPTH": @"zero", @"DM_PSO_BUNDLE": @"off"};
    EXPECT("tool 2: exit 0", 0, run_self(@[@"--tool", root], tool2, [root stringByAppendingPathComponent:@"tool2.log"]));
    EXPECT("tool 2: stamp, depth near=0", 1, [slurp([devout stringByAppendingPathComponent:@"prewarm.tsv"]) containsString:@"depth\tnear=0\n"]);
    NSError *err = nil; [NSFileManager.defaultManager createDirectoryAtPath:dev withIntermediateDirectories:YES attributes:nil error:nil];
    BOOL copied = [NSFileManager.defaultManager copyItemAtPath:[devout stringByAppendingFormat:@"/%@-prewarm-00.mtl4a", gpu] toPath:[dev stringByAppendingPathComponent:@"00000.mtl4a"] error:&err];
    EXPECT("device file copied", 1, copied);
    for (NSUInteger i = 0; i < files.count; i++) {
        NSString *kp = [bundle stringByAppendingPathComponent:[files[i].stringByDeletingPathExtension stringByAppendingString:@".keys"]]; int cx = 0;
        EXPECT("tool: each file has its index, complete", 1, arch_keys_read(kp, &cx).length >= sizeof(uint64_t) && cx);
    }
    EXPECT("tool: the last file's index removed for the open", 1, unlink([bundle stringByAppendingFormat:@"/%@-prewarm-02.keys", gpu].fileSystemRepresentation) == 0);
    put([bundle stringByAppendingFormat:@"/%@-prewarm-09.mtl4a", gpu], @"not an archive");   // Metal crashes on such a file: never handed to it
    // the bundle form without a work folder: a failure, not an empty success
    EXPECT("bundle form without a work folder: fails", 1, run_self(@[@"--tool", root], @{@"DM_PSO_PREBUILD": @"tool", @"DM_PSO_ARCHIVE": @"off", @"DM_PSO_PREBUILD_OUT": [root stringByAppendingPathComponent:@"out3"], @"DM_PSO_MANIFEST": @"", @"DM_PSO_BUNDLE": @"off"}, [root stringByAppendingPathComponent:@"tool3.log"]));
    // the device form (the NSP script's --platform mac, regress.py): the lists' bases go into the device's archive, with no pipelines script and no files for an app
    NSString *devarch = [root stringByAppendingPathComponent:@"devarch"];
    EXPECT("device form: exit 0", 0, run_self(@[@"--tool", root], @{@"DM_PSO_PREBUILD": @"tool", @"DM_PSO_ARCHIVE": devarch, @"DM_PSO_PREBUILD_OUT": @"", @"DM_PSO_MANIFEST": @"", @"DM_PSO_BUNDLE": @"off"}, [root stringByAppendingPathComponent:@"tool4.log"]));
    EXPECT("device form: the 5 bases compiled into the device's archive", 1, [slurp([root stringByAppendingPathComponent:@"tool4.log"]) containsString:@"(bases 5 compiled, 0 from the archives)"] && mtl4a(devarch).count > 0);
    EXPECT("device form: no pipelines script", 0, [[NSFileManager.defaultManager contentsOfDirectoryAtPath:devarch error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4-json'"]].count);
    // DM_PSO_PREBUILD_WARM=0: the lists are left to the first start
    NSString *nowarm = [root stringByAppendingPathComponent:@"nowarm"];
    EXPECT("warm 0: exit 0", 0, run_self(@[@"--tool", root], @{@"DM_PSO_PREBUILD": @"tool", @"DM_PSO_PREBUILD_WARM": @"0", @"DM_PSO_ARCHIVE": nowarm, @"DM_PSO_PREBUILD_OUT": @"", @"DM_PSO_MANIFEST": @"", @"DM_PSO_BUNDLE": @"off"}, [root stringByAppendingPathComponent:@"tool5.log"]));
    EXPECT("warm 0: nothing built", 1, ![slurp([root stringByAppendingPathComponent:@"tool5.log"]) containsString:@"prewarm lines"] && mtl4a(nowarm).count == 0);
    // a compute row: the MTLDevice builds it, the harvest queue captures it in the background; the bundle waits for that capture (a slow one was left out, and written
    // at exit, where Metal's pipelines-script serializer met a finalized C library and the tool crashed)
    NSString *out6 = [root stringByAppendingPathComponent:@"out6"], *list6 = [root stringByAppendingPathComponent:@"six.tsv"], *log6p = [root stringByAppendingPathComponent:@"tool6.log"];
    put(list6, @"C\twarm-cs\n");
    EXPECT("compute row: exit 0", 0, run_self(@[@"--tool", root], @{@"DM_PSO_PREBUILD": @"tool", @"DM_PSO_ARCHIVE": [root stringByAppendingPathComponent:@"work6"], @"DM_PSO_PREBUILD_OUT": out6,
                                                                      @"DM_PSO_MANIFEST": list6, @"DM_PSO_BUNDLE": @"off", @"T_HARVEST_HOLD": @"500"}, log6p));
    NSString *log6 = slurp(log6p); NSRange cap = [log6 rangeOfString:@"and 1 compute pipelines written"], out = [log6 rangeOfString:@"for the app to bundle"];
    EXPECT("compute row: captured before the bundle is made", 1, cap.location != NSNotFound && out.location != NSNotFound && cap.location < out.location);
    EXPECT("compute row: its file in the bundle", 1, mtl4a(out6).count);
    EXPECT("compute row: the stamp counts it", 1, [slurp([out6 stringByAppendingPathComponent:@"prewarm.tsv"]) containsString:@"files\t1\n"]);
    EXPECT("compute row: nothing written at exit", 0, [log6 containsString:@"pipeline archive at exit"]);
    // the app's side
    NSDictionary *app = @{@"DM_PSO_BUNDLE": bundle, @"DM_PSO_ARCHIVE": dev, @"DM_PSO_MANIFEST": @"off", @"DM_PSO_PREBUILD": @"0", @"MTL_DEBUG_LAYER": @"1"};
    int r = run_self(@[@"--open", root], app, [root stringByAppendingPathComponent:@"open.log"]);
    NSString *olog = slurp([root stringByAppendingPathComponent:@"open.log"]);
    EXPECT("open: exit 0", 0, r);
    if (r) fprintf(stderr, "%s\n", olog.UTF8String);
    EXPECT("open: log, prewarm files", 1, [olog containsString:@"prewarm archive bundled with the app: 3 of 4 files, 2 with an index of 4 bases"] && [olog containsString:@"[RED] prewarm archive bundled with the app"]);
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_prewarm: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
