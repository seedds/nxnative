// Exercise real manifest loading, shader loading, pipeline builds, cache claims and GPU draws.
// Synthetic shaders need no game files; old rows differ in unused state, while live changes and logical target 7 in a merged pass must remain distinct.
// Observe a real owner join without replacing its wait or relying on a sleep. The worker
// signals the same event on return: before releasing the device we can distinguish join
// from a second completed preparation, then compare the actual returned objects and draw.
#include <dispatch/dispatch.h>
#include <os/lock.h>
static __thread void *shader_wait_event;
static __thread os_unfair_lock_t shader_wait_ignore_lock;
static void shader_observe_lock(os_unfair_lock_t lock) {
    if (shader_wait_event && lock != shader_wait_ignore_lock) {
        dispatch_semaphore_t event = (__bridge dispatch_semaphore_t)shader_wait_event;
        shader_wait_event = NULL; dispatch_semaphore_signal(event);
    }
    os_unfair_lock_lock(lock);
}
#define os_unfair_lock_lock shader_observe_lock
#include "../nvn/nvn_mtl.m"
#undef os_unfair_lock_lock
static void shader_watch_wait(dispatch_semaphore_t event) {
    // Observe the real per-owner wait, not the brief registry lookup before it.
    // Assertions below concern returned objects, progress, lifetime and GPU pixels.
    shader_wait_ignore_lock = &g_shader_lock; shader_wait_event = (__bridge void *)event;
}

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w_ = (want), g_ = (got); checks++; \
    if (w_ != g_) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); failed++; } } while (0)

static int fixtures(NSString *dir) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * 2 - 1, 0, 1); }\n"
        "vertex float4 vs_zero(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * 2 - 1, -0.5, 1); }\n"
        "struct Layer { float4 p [[position]]; uint slice [[render_target_array_index]]; };\n"
        "vertex Layer vs_layer(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return {float4(uv * 2 - 1, 0, 1), 1}; }\n"
        "fragment float4 fs() { return float4(0.5, 0.25, 1, 0.5); }\n";
    NSString *metal = [dir stringByAppendingPathComponent:@"fixture.metal"];
    NSString *library = [dir stringByAppendingPathComponent:@"warm-vs.metallib"];
    if (![source writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil]) return 0;
    NSTask *compiler = [NSTask new]; compiler.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
    compiler.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", library];
    NSError *error = nil;
    if (![compiler launchAndReturnError:&error]) { fprintf(stderr, "fixture compiler: %s\n", error.localizedDescription.UTF8String); return 0; }
    [compiler waitUntilExit]; if (compiler.terminationStatus) return 0;
    if (![NSFileManager.defaultManager copyItemAtPath:library toPath:[dir stringByAppendingPathComponent:@"warm-fs.metallib"] error:nil]) return 0;
    for (NSString *stage in @[@"vs", @"fs"]) {
        NSMutableDictionary *reflection = @{@"chain": @DMSH_CHAIN, @"route": @"B", @"entry": stage, @"entry_z": stage,
            @"bindings": @[], @"vertex_inputs": @[]}.mutableCopy;
        if ([stage isEqual:@"vs"]) { reflection[@"entry_z"] = @"vs_zero"; reflection[@"entry_layer"] = @"vs_layer"; reflection[@"entry_layer_z"] = @"vs_layer"; }
        NSData *json = [NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil];
        if (![json writeToFile:[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"warm-%@.json", stage]] atomically:YES]) return 0;
    }
    g_msl_dir = dir; g_shmap = @{@"warm-vs": @"fixture", @"warm-fs": @"fixture"}.mutableCopy;
    return 1;
}

// Hold the real first library load, not a replacement shader loader. The S and R tasks below
// then reach warm_shader concurrently, while a different shader must still load successfully.
@interface WarmGateDevice : NSObject {
@public id<MTLDevice> device; dispatch_semaphore_t entered, release; _Atomic unsigned library_calls; unsigned gate_call;
}
@end
@implementation WarmGateDevice
- (id)forwardingTargetForSelector:(SEL)selector { return device; }
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
    if (atomic_fetch_add(&library_calls, 1) == gate_call) {
        dispatch_semaphore_signal(entered);
        dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
    }
    return [device newLibraryWithData:data error:error];
}
@end


static NSString *row(const pso_key *key, int merged) {
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    strcpy(vs->key, "warm-vs"); strcpy(fs->key, "warm-fs"); fs->stage = 1;
    pso_skey sk; char hex[sizeof sk * 2 + 1];
    pso_skey_from(key, vs, fs, merged, NULL, &sk); pso_skey_hex(&sk, hex);
    return [NSString stringWithFormat:@"R\t%s\n", hex];
}

static void pixels(id<MTLRenderPipelineState> pipeline, uint8_t out[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> queue = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    [enc setRenderPipelineState:pipeline]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    EXPECT("GPU draw completed", MTLCommandBufferStatusCompleted, cb.status);
    [target getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
}

static void check_draw(const pso_key *raw, int merged, const uint8_t expected[4]) {
    NvShader *vs = g_shcache[@"warm-vs"], *fs = g_shcache[@"warm-fs"];
    if (!vs || !fs) { EXPECT("warmed shaders claimed", 1, 0); return; }
    state_reset(); S.color.blend_en = raw->blend_en; S.chmask.mask = raw->chmask; S.ms.a2c = raw->a2c;
    memcpy(S.blend, raw->blend, sizeof S.blend);
    memcpy(g_pass.cfmt, raw->cfmt, sizeof g_pass.cfmt); g_pass.dfmt = raw->dfmt; g_pass.sfmt = raw->sfmt; g_pass.samples = raw->samples;
    g_pso_lmap = merged ? raw->lmap : NULL;
    pso_key live; pso_key_fill(&live, vs, fs, raw->cfmt, raw->dfmt, raw->sfmt, raw->samples, merged);
    if (merged) memcpy(live.lmap, raw->lmap, sizeof live.lmap);
    EXPECT("live key finds warmed pipeline", 1, hm_get_id(&g_pso, &live) != nil);
    uint64_t before = g_pb_spec + g_pb_legacy;
    NvProg *program = [NvProg new]; program->sh[0] = vs; program->sh[1] = fs;
    id<MTLRenderPipelineState> warmed = pso_get_(program, nil);
    EXPECT("draw lookup adds no pipeline build", before, g_pb_spec + g_pb_legacy);
    EXPECT("draw lookup returns a pipeline", 1, warmed != nil);
    if (!warmed) { g_pso_lmap = NULL; return; }
    // Build the descriptor from the unnormalized historical state as a rendering oracle.
    pso_key original = *raw; original.vs = (__bridge void *)vs; original.fs = (__bridge void *)fs;
    NSError *error = nil; id<MTLRenderPipelineState> reference = pso_build(pso_desc(&original, vs, fs, nil, merged), &error);
    EXPECT("original descriptor builds", 1, reference != nil);
    if (reference) {
        uint8_t a[4], b[4]; pixels(warmed, a); pixels(reference, b);
        EXPECT("normalization preserves rendered pixels", 1, memcmp(a, b, sizeof a) == 0);
        int correct = 1; for (int i = 0; i < 4; i++) correct &= abs((int)a[i] - expected[i]) <= 1;
        EXPECT("pixels have the intended blend and channel mask", 1, correct);
    }
    g_pso_lmap = NULL;
}

static void singleflight(NSString *dir) {
    g_warm_items = [NSMutableArray array]; atomic_store(&g_warm_ready, 0);
    for (NSString *extension in @[@"json", @"metallib"]) {
        NSString *source = [dir stringByAppendingPathComponent:[@"warm-fs." stringByAppendingString:extension]];
        NSString *target = [dir stringByAppendingPathComponent:[@"warm-other." stringByAppendingString:extension]];
        if (![NSFileManager.defaultManager fileExistsAtPath:target])
            EXPECT("unrelated fixture copied", 1, [NSFileManager.defaultManager copyItemAtPath:source toPath:target error:nil]);
    }
    pso_key key = {0}; key.cfmt[0] = MTLPixelFormatRGBA8Unorm; key.samples = 1; key.chmask = 15;
    NSString *manifest = [dir stringByAppendingPathComponent:@"concurrent.tsv"];
    NSString *text = [@"S\twarm-vs\t0\n" stringByAppendingString:row(&key, 0)];
    EXPECT("concurrent manifest written", 1, [text writeToFile:manifest atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    long lines = 0; NSArray<WarmTask *> *tasks = warm_parse(@[manifest], &lines);
    EXPECT("S and R are distinct real warm tasks", 2, tasks.count);
    if (tasks.count != 2) return;
    pso4_init();   // Match warm_start: initialize the real compiler before substituting the test device.
    WarmGateDevice *gate = [WarmGateDevice new]; gate->device = g_dev;
    gate->entered = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0);
    g_dev = (id<MTLDevice>)gate;
    dispatch_group_t callers = dispatch_group_create();
    dispatch_group_async(callers, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ warm_task(tasks[0]); });
    EXPECT("first real shader load reaches device gate", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_semaphore_t waiter = dispatch_semaphore_create(0); __block _Atomic int second_returned = 0;
    dispatch_group_async(callers, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        shader_watch_wait(waiter); warm_task(tasks[1]); shader_wait_event = NULL;
        atomic_store(&second_returned, 1); dispatch_semaphore_signal(waiter);
    });
    EXPECT("R task reaches S task's running preparation", 0, dispatch_semaphore_wait(waiter, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("R task joins rather than preparing a second shader", 0, atomic_load(&second_returned));
    // A dedicated worker avoids a failed assertion hanging the test behind the very global load lock it is meant to catch.
    dispatch_group_t unrelated = dispatch_group_create();
    __block NvShader *other = nil;
    dispatch_group_async(unrelated, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { other = warm_shader(@"warm-other", 1); } });
    EXPECT("unrelated shader finishes before owner is released", 0, dispatch_group_wait(unrelated, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_semaphore_signal(gate->release);
    dispatch_group_wait(callers, DISPATCH_TIME_FOREVER); dispatch_group_wait(unrelated, DISPATCH_TIME_FOREVER);
    EXPECT("unrelated shader loaded", 1, other != nil);
    EXPECT("actual library loads", 3, atomic_load(&gate->library_calls));
    NvShader *published = warm_shader(@"warm-vs", 0);
    unsigned references = 0;
    for (WarmItem *item in g_warm_items) {
        if (item->kind == 0) { EXPECT("S task retains the published shader", 1, item->shader == published); references++; }
        if (item->kind == 1) { EXPECT("R task retains the published shader", 1, item->vs == published); references++; }
    }
    EXPECT("both real callers returned the same shader", 2, references);
    EXPECT("cached lookup returns the published shader", 1, warm_shader(@"warm-vs", 0) == published);
    EXPECT("cached lookups perform no library loads", 3, atomic_load(&gate->library_calls));
    g_dev = gate->device;
    warm_claim();
    NvShader *vs = g_shcache[@"warm-vs"];
    EXPECT("S and R publish one shader identity", 1, vs == published);
    // Missing geometry reflection returns nil quietly. Warm callers share its negative result;
    // a later demand caller still retries, and succeeds once real reflection appears.
    EXPECT("failed warm load", 1, warm_shader(@"retry-gs", 2) == nil);
    NSDictionary *reflection = @{@"chain": @DMSH_CHAIN, @"gs_slice": @{@"loc": @0, @"comp": @"x"}};
    NSString *path = [dir stringByAppendingPathComponent:@"retry-gs.json"];
    EXPECT("retry reflection written", 1, [[NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil] writeToFile:path atomically:YES]);
    EXPECT("failure stays cached for this warm phase", 1, warm_shader(@"retry-gs", 2) == nil);
    EXPECT("demand loading still retries failed warm key", 1, shader_get(@"retry-gs", 2) != nil);
    EXPECT("retry fixture removed", 1, [NSFileManager.defaultManager removeItemAtPath:path error:nil]);
    g_warm_t0 = bp_now(); warm_finish();
    g_shcache = nil; nvn_hm_clear(&g_pso, hm_rel);
}

static NSArray *prefetch_queues(void) {
    NSMutableArray *queues = [NSMutableArray array];
    for (int i = 0; i < 16; i++) { id q = pp_bg_queue(); if (![queues containsObject:q]) [queues addObject:q]; }
    return queues;
}
static void drain_prefetch(NSArray *queues) { for (dispatch_queue_t q in queues) dispatch_sync(q, ^{}); }

// Hash a real synthetic control section through the production program-key API.
static NSString *program_fixture(NSString *dir, prog_t *program, unsigned tag, int vertex) {
    static uint8_t controls[7][0x880];
    memset(program, 0, sizeof *program); program->nstage = 1;
    controls[tag - 1][0] = (uint8_t)tag; program->sd[0].control = controls[tag - 1];
    uint8_t digest[8]; prog_key(program, 0, digest);
    NSString *map = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x",
        digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7]];
    g_shmap[map] = vertex ? @"vertex" : @"pixel";
    NSString *pc = [map stringByAppendingString:@"_b"];
    for (NSString *ext in @[@"json", @"metallib"]) {
        NSString *from = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"warm-%@.%@", vertex ? @"vs" : @"fs", ext]];
        NSString *to = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.%@", pc, ext]];
        EXPECT("program fixture copied", 1, [NSFileManager.defaultManager copyItemAtPath:from toPath:to error:nil]);
    }
    if (vertex) {
        NSString *json = [dir stringByAppendingPathComponent:[pc stringByAppendingString:@".json"]];
        NSData *data = [NSData dataWithContentsOfFile:json];
        if (!data) { EXPECT("pulling reflection readable", 1, 0); return pc; }
        NSMutableDictionary *reflection = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
        if (!reflection) { EXPECT("pulling reflection parsed", 1, 0); return pc; }
        reflection[@"pull"] = @{};
        EXPECT("pulling reflection written", 1, [[NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil] writeToFile:json atomically:YES]);
        if (tag != 2) EXPECT("pulling library copied", 1, [NSFileManager.defaultManager copyItemAtPath:[dir stringByAppendingPathComponent:@"warm-vs.metallib"]
            toPath:[dir stringByAppendingPathComponent:[map stringByAppendingString:@"_bp.metallib"]] error:nil]);
    }
    return pc;
}

static void shader_pixels(NvShader *vs, NvShader *fs, int covered) {
    if (!vs || !fs) { EXPECT("acquisition returns drawable shaders", 1, 0); return; }
    pso_key key = {0}; key.vs = (__bridge void *)vs; key.fs = (__bridge void *)fs;
    key.cfmt[0] = MTLPixelFormatRGBA8Unorm; key.samples = 1; key.chmask = 15;
    NSError *error = nil; id<MTLRenderPipelineState> pipeline = pso_build(pso_desc(&key, vs, fs, nil, 0), &error);
    EXPECT("acquired functions build a real pipeline", 1, pipeline != nil);
    if (!pipeline) return;
    uint8_t value[4]; pixels(pipeline, value);
    const uint8_t color[4] = {128, 64, 255, 128}, clear[4] = {255, 0, 0, 255};
    const uint8_t *expected = covered ? color : clear;
    int correct = 1; for (int i = 0; i < 4; i++) correct &= abs((int)value[i] - expected[i]) <= 1;
    EXPECT("acquired entry renders the intended pixels", 1, correct);
}

static void layer_pixels(NvShader *vs, NvShader *fs) {
    if (!vs || !fs) return;
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new]; desc.vertexFunction = fn_dev(vs->fn); desc.fragmentFunction = fn_dev(fs->fn);
    desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *error = nil; id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:desc error:&error];
    EXPECT("layer entry builds a real pipeline", 1, pipeline != nil); if (!pipeline) return;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.textureType = MTLTextureType2DArray; td.arrayLength = 2; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.renderTargetArrayLength = 2;
    pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id<MTLCommandQueue> queue = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    [enc setRenderPipelineState:pipeline]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted]; EXPECT("layer GPU draw completed", MTLCommandBufferStatusCompleted, cb.status);
    uint8_t values[2][4]; for (int i = 0; i < 2; i++) [target getBytes:values[i] bytesPerRow:16 bytesPerImage:64 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0 slice:i];
    const uint8_t expected[2][4] = {{255, 0, 0, 255}, {128, 64, 255, 128}};
    int correct = 1; for (int i = 0; i < 2; i++) for (int j = 0; j < 4; j++) correct &= abs((int)values[i][j] - expected[i][j]) <= 1;
    EXPECT("layer entry writes only intended array slice", 1, correct);
}

static void acquisition(NSString *dir) {
    NSArray *queues = prefetch_queues();
    static prog_t programs[7];
    NSString *sequential = program_fixture(dir, &programs[0], 1, 1);
    NSString *queued = program_fixture(dir, &programs[1], 2, 1);
    NSString *running = program_fixture(dir, &programs[2], 3, 1);
    NSString *retry = program_fixture(dir, &programs[3], 4, 0);
    NSString *other = program_fixture(dir, &programs[4], 5, 1);
    NSString *pulling = program_fixture(dir, &programs[5], 6, 1);
    NSString *other_pulling = program_fixture(dir, &programs[6], 7, 1);
    NvShader *fs = shader_get(@"warm-fs", 1);

    // The observed game case is COMPLETED, sequential producers, not just an in-flight race.
    mtl_prog_prefetch(&programs[0]); drain_prefetch(queues);
    __weak NvShader *kept = nil;
    @autoreleasepool {
        NvShader *warmed = warm_shader(sequential, 0), *demand = shader_get(sequential, 0);
        EXPECT("completed program prefetch and warm reuse one object", 1, warmed == demand);
        for (WarmItem *item in g_warm_items) if ([item->key isEqual:sequential])
            EXPECT("completed prefetch and demand reuse one object", 1, item->shader == demand);
        kept = demand; shader_pixels(demand, fs, 1);
        warm_claim(); g_shcache = nil; g_warm_t0 = bp_now(); warm_finish();
    }
    EXPECT("completed canonical shader survives transient publication owners", 1, kept != nil);
    if (kept) EXPECT("later warm acquisition preserves completed identity", 1, warm_shader(sequential, 0) == kept);

    // A paused utility queue cannot own demand's progress. Drain it afterwards and compare
    // actual objects: a stale queued block must not prepare a second shader after takeover.
    for (dispatch_queue_t q in queues) dispatch_suspend(q);
    mtl_prog_prefetch(&programs[1]);
    dispatch_group_t demand_done = dispatch_group_create(); __block NvShader *taken = nil;
    dispatch_group_async(demand_done, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { taken = shader_get(queued, 0); } });
    EXPECT("demand finishes while utility prefetch is still queued", 0, dispatch_group_wait(demand_done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    // With no pulling library available during demand, an eager attempted preparation
    // would permanently fail. Make it available now and exercise the real lazy entry.
    NSString *pull_path = [dir stringByAppendingPathComponent:[[queued substringToIndex:16] stringByAppendingString:@"_bp.metallib"]];
    EXPECT("queued demand's pulling library supplied later", 1, [NSFileManager.defaultManager copyItemAtPath:[dir stringByAppendingPathComponent:@"warm-vs.metallib"] toPath:pull_path error:nil]);
    id<MTLFunction> taken_pull = taken ? pull_fn(taken) : nil;
    EXPECT("ordinary demand did not eagerly fail unused pulling entry", 1, taken_pull != nil);
    for (dispatch_queue_t q in queues) dispatch_resume(q);
    drain_prefetch(queues); dispatch_group_wait(demand_done, DISPATCH_TIME_FOREVER);
    EXPECT("queued takeover returns a shader", 1, taken != nil);
    for (WarmItem *item in g_warm_items) if ([item->key isEqual:queued])
        EXPECT("queued worker cannot replace demand identity", 1, item->shader == taken);
    EXPECT("warm after queued takeover reuses demand object", 1, warm_shader(queued, 0) == taken);
    shader_pixels(taken, fs, 1);

    WarmGateDevice *gate = [WarmGateDevice new]; gate->device = g_dev;
    gate->entered = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0);
    g_dev = (id<MTLDevice>)gate;
    mtl_prog_prefetch(&programs[2]);
    EXPECT("running prefetch reaches real Metal library load", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_semaphore_t progress = dispatch_semaphore_create(0);
    __block _Atomic int returned = 0; __block NvShader *joined = nil;
    dispatch_group_t join = dispatch_group_create();
    dispatch_group_async(join, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        shader_watch_wait(progress); joined = shader_get(running, 0);
        shader_wait_event = NULL; atomic_store(&returned, 1); dispatch_semaphore_signal(progress);
    } });
    EXPECT("demand reaches an owner wait or completes", 0, dispatch_semaphore_wait(progress, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("demand joins running owner rather than making a duplicate", 0, atomic_load(&returned));
    NvShader *independent = warm_shader(other, 0);
    EXPECT("independent full key completes while owner is held", 1, independent != nil);
    dispatch_semaphore_signal(gate->release);
    dispatch_group_wait(join, DISPATCH_TIME_FOREVER); drain_prefetch(queues);
    for (WarmItem *item in g_warm_items) if ([item->key isEqual:running])
        EXPECT("running cross-domain acquisition returns the owner object", 1, item->shader == joined);
    EXPECT("other key retains independent identity", 1, independent != joined);
    g_dev = gate->device; shader_pixels(joined, fs, 1);

    // Hold the optional pulling-library load after the canonical core was published.
    // Demand can acquire the core, but its first use of the same lazy entry must join
    // preparation rather than observe a transient failure or create a second function.
    // Only the Metal 4 hybrid path starts background pulling preparation.
    if (hybrid_on()) {
        WarmGateDevice *pull_gate = [WarmGateDevice new]; pull_gate->device = g_dev; pull_gate->gate_call = 1;
        pull_gate->entered = dispatch_semaphore_create(0); pull_gate->release = dispatch_semaphore_create(0);
        g_dev = (id<MTLDevice>)pull_gate; mtl_prog_prefetch(&programs[5]);
        EXPECT("background pulling preparation reaches real device gate", 0, dispatch_semaphore_wait(pull_gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        NvShader *pull_core = shader_get(pulling, 0);
        EXPECT("demand acquires core while optional pulling load is running", 1, pull_core != nil);
        dispatch_group_t pull_join = dispatch_group_create(); dispatch_semaphore_t pull_entered = dispatch_semaphore_create(0);
        __block id<MTLFunction> pull_function = nil; __block _Atomic int pull_returned = 0;
        dispatch_group_async(pull_join, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            shader_watch_wait(pull_entered); if (pull_core) pull_function = pull_fn(pull_core);
            shader_wait_event = NULL; atomic_store(&pull_returned, 1); dispatch_semaphore_signal(pull_entered);
        });
        EXPECT("demand pulling caller reaches owner wait or completes", 0, dispatch_semaphore_wait(pull_entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        EXPECT("pulling caller cannot finish while its owner is gated", 0, atomic_load(&pull_returned));
        dispatch_group_t pull_other = dispatch_group_create(); __block id<MTLFunction> independent_pull = nil;
        dispatch_group_async(pull_other, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
            NvShader *shader = shader_get(other_pulling, 0); if (shader) independent_pull = pull_fn(shader);
        } });
        EXPECT("independent pulling entry completes while owner is held", 0, dispatch_group_wait(pull_other, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        dispatch_semaphore_signal(pull_gate->release);
        dispatch_group_wait(pull_join, DISPATCH_TIME_FOREVER); dispatch_group_wait(pull_other, DISPATCH_TIME_FOREVER); drain_prefetch(queues);
        EXPECT("joined pulling entry is usable", 1, pull_function != nil);
        EXPECT("independent pulling entry is usable", 1, independent_pull != nil);
        EXPECT("background and demand use one pulling function identity", 1, pull_function && pull_function == pull_fn(warm_shader(pulling, 0)));
        g_dev = pull_gate->device;
        if (pull_function) { NvShader *draw_pull = [NvShader new]; draw_pull->fn = pull_function; shader_pixels(draw_pull, fs, 1); }
    }

    NvShader *before_lookahead = warm_shader(sequential, 0);
    g_shcache = nil;
    uint8_t stream[128] = {0}; size_t used = 0;
    for (unsigned i = 0; i < 2; i++) {
        p_bind_program bind = {&programs[i ? 4 : 0], 1}; op_hdr h = {OP_BIND_PROGRAM, (uint32_t)((8 + sizeof bind + 7) & ~7u)};
        memcpy(stream + used, &h, sizeof h); memcpy(stream + used + 8, &bind, sizeof bind); used += h.size;
    }
    op_hdr end = {OP_END, 8}; memcpy(stream + used, &end, sizeof end);
    g_exec_next = stream; shader_prefetch_rest(); g_exec_next = NULL;
    EXPECT("lookahead reuses completed prefetch/warm identity", 1, shader_get(sequential, 0) == before_lookahead);
    EXPECT("lookahead preserves independent completed identity", 1, shader_get(other, 0) == independent);

    NSString *retry_json = [dir stringByAppendingPathComponent:[retry stringByAppendingString:@".json"]];
    NSData *reflection = [NSData dataWithContentsOfFile:retry_json];
    EXPECT("retry fixture reflection removed", 1, [NSFileManager.defaultManager removeItemAtPath:retry_json error:nil]);
    mtl_prog_prefetch(&programs[3]); drain_prefetch(queues);
    EXPECT("retry fixture reflection restored", 1, [reflection writeToFile:retry_json atomically:YES]);
    NvShader *retried = shader_get(retry, 1);
    EXPECT("failed background acquisition allows later demand retry", 1, retried != nil);
    shader_pixels(taken, retried, 1);

    // Real objects supply positive lifetime controls; no function-map or ownership-field counts.
    __weak NvShader *released = nil;
    @autoreleasepool { NvShader *control = shader_load(other, 0, 0, 0); released = control; EXPECT("uncached lifetime control exists", 1, released != nil); }
    EXPECT("uncached shader positive release control", 1, released == nil);
    warm_claim(); g_shcache = nil;
    NvShader *base = shader_get(other, 0), *layer = shader_get_layer(base);
    EXPECT("layer and ordinary entries have independent shader identities", 1, layer && layer != base);
    EXPECT("layer acquisition is canonical", 1, shader_get_layer(base) == layer);
    layer_pixels(layer, fs);
    NvShader *different_stage = shader_get(other, 3);
    EXPECT("stage is part of full variant identity", 1, different_stage && different_stage != base);
    atomic_store(&g_depth_zero, 1);
    NvShader *zero = shader_get(other, 0);
    EXPECT("depth entry is part of full variant identity", 1, zero && zero != base);
    shader_pixels(zero, fs, 0);
    atomic_store(&g_depth_zero, 0);
    EXPECT("returning to prior depth reuses prior shader identity", 1, shader_get(other, 0) == base);
    shader_pixels(base, fs, 1);
    warm_claim(); g_shcache = nil;
}

static void scenario(NSString *dir, int merged) {
    nvn_hm_clear(&g_pso, hm_rel); g_shcache = nil; g_warm_items = [NSMutableArray array];
    g_warm_rps = g_warm_claim_rps = 0; atomic_store(&g_warm_ready, 0);
    pso_key keys[4] = {0}; keys[0].cfmt[0] = MTLPixelFormatRGBA8Unorm; keys[0].samples = 1;
    int logical = merged ? 7 : 0;
    keys[0].chmask = 0xfu << (4 * logical);
    blend_st enabled = {.src_rgb = NVN_BLEND_FUNC_SRC_ALPHA, .dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
        .src_a = NVN_BLEND_FUNC_SRC_ALPHA, .dst_a = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, .eq_rgb = NVN_BLEND_EQUATION_ADD, .eq_a = NVN_BLEND_EQUATION_ADD};
    if (merged) { memset(keys[0].lmap, 0xff, sizeof keys[0].lmap); keys[0].lmap[0] = 7; keys[0].blend_en = 1u << 7; keys[0].blend[7] = enabled; }
    keys[1] = keys[0];
    // Neither row matches byte-for-byte, but both describe the same pipeline.
    if (merged) keys[1].blend[1].src_rgb = NVN_BLEND_FUNC_DST_ALPHA;
    else { keys[1].blend_en = 1u << 7; keys[1].chmask |= 0xfu << 28; }   // old pso_key_fill copied no blend factors for absent targets
    keys[2] = keys[0]; keys[2].chmask = 2u << (4 * logical);   // a real write-mask difference
    keys[3] = keys[0]; keys[3].blend_en ^= 1u << logical;   // a real blend-enable difference
    if (!merged) keys[3].blend[0] = enabled;
    NSString *a = [dir stringByAppendingPathComponent:@"seed.tsv"], *b = [dir stringByAppendingPathComponent:@"user.tsv"];
    NSString *r0 = row(&keys[0], merged), *r1 = row(&keys[1], merged);
    EXPECT("historical rows differ before loading", 0, [r0 isEqual:r1]);
    NSString *seed = [r0 stringByAppendingString:r1];
    // The second list repeats both old rows, then changes two pieces of effective state.
    NSString *user = [NSString stringWithFormat:@"%@%@%@%@", r1, r0, row(&keys[2], merged), row(&keys[3], merged)];
    EXPECT("seed manifest written", 1, [seed writeToFile:a atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    EXPECT("user manifest written", 1, [user writeToFile:b atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    uint64_t before = g_pb_spec + g_pb_legacy; g_warm_t0 = bp_now(); atomic_store(&g_warm_running, 1);
    long lines = 0; for (WarmTask *t in warm_parse(@[a, b], &lines)) warm_task(t);
    warm_finish();
    EXPECT("only three effective pipelines compiled across both manifests", 3, g_pb_spec + g_pb_legacy - before);
    EXPECT("only three render pipelines enqueued", 3, g_warm_rps);
    warm_claim();
    EXPECT("three warmed pipelines claimed into cache", 3, g_warm_claim_rps);
    EXPECT("cache has exactly three effective keys", 3, g_pso.n);
    const uint8_t plain[4] = {128, 64, 255, 128}, green[4] = {255, 64, 0, 255};
    const uint8_t blended[4] = {191, 32, 128, 191}, blended_green[4] = {255, 32, 0, 255};
    check_draw(&keys[0], merged, merged ? blended : plain);
    check_draw(&keys[1], merged, merged ? blended : plain);
    check_draw(&keys[2], merged, merged ? blended_green : green);
    check_draw(&keys[3], merged, merged ? plain : blended);
    printf("%s manifests: six rows -> %llu builds, %u cache keys\n", merged ? "merged" : "normal", (unsigned long long)g_warm_rps, g_pso.n);
}

int main(void) { @autoreleasepool {
    alarm(60);   // A regression in owner/waiter progress must fail rather than hang the suite.
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_NVN_PREDICT", "0", 1);
    unsetenv("DM_NVN_HILITE"); unsetenv("DM_NVN_MAGSCAN"); unsetenv("DM_PSO_RECORD"); unsetenv("DM_PSO_USER"); unsetenv("DM_SAVE");
    char dir[] = "/tmp/t_pso_warm.XXXXXX"; if (!mkdtemp(dir)) return 1;
    mtl_init();
    if (!fixtures(@(dir))) { fprintf(stderr, "failed to compile synthetic shader fixtures\n"); [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil]; return 1; }
    singleflight(@(dir));
    acquisition(@(dir));
    scenario(@(dir), 0); scenario(@(dir), 1);
    const char *mode = getenv("DM_PSO_MTL4");
    if (mode) EXPECT("requested compiler path selected", strcmp(mode, "0") != 0, g_comp4 != nil);
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    printf("t_pso_warm: %d checks, %d failed; %s\n", checks, failed, g_comp4 ? "Metal 4" : "MTLDevice");
    return failed != 0;
} }
