// DM_PSO_MESH_ASYNC=skip: a native DrawMeshTasks whose pipeline is not built yet draws nothing while the pipeline builds in the background, then draws once it is published.
// Also: indirect draws skip the same way; an occlusion query around a skipped draw reports at least one sample, and conditional rendering on that report draws; a skip does not
// leave the merged pipeline state (lmap / fsfn) to the next draw; a program that may write memory (writable storage, SBUFU / GMEML / run-time handles) is never skipped, through
// mtk_pso itself; the process ends cleanly with a build still in flight. DM_PSO_MESH_ASYNC=0 from outside runs the same checks without the switch: several must fail.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#define P(name) ((NVNT_##name)nvn_lookup(#name))
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failures;
#define CHECK(c, name) do { int check_ok = !!(c); checks++; failures += !check_ok; if (!check_ok) fprintf(stderr, "FAIL: %s\n", name); } while (0)
static void require(int ok, const char *name) { CHECK(ok, name); if (!ok) exit(2); }

// The write gate reads the MSL of the library each stage was loaded from (lib_path): here a fixture cache folder (g_msl_dir) holding <key>.metal, read through msl_data
static NSString *source, *fixture;
static void put_msl(NvShader *s, NSString *code) {   // the stage's MSL in the fixture cache (nil: none), its library path as shader_load sets it, and its proof as mtk_proof_note makes it
    NSString *m = [NSString stringWithFormat:@"%@/%s.metal", fixture, s->key];
    if (code) [code writeToFile:m atomically:YES encoding:NSUTF8StringEncoding error:nil]; else [NSFileManager.defaultManager removeItemAtPath:m error:nil];
    s->lib_path = [NSString stringWithFormat:@"%@/%s.metallib", fixture, s->key]; s->lib_cache = 1; s->mtk_store = (uint8_t)mtk_proof(s);
}
static NvShader *shader(id<MTLLibrary> library, NSString *name, int stage, const char *tag) {
    NvShader *s = [NvShader new]; s->fn = [library newFunctionWithName:name]; s->stage = stage;
    snprintf(s->key, sizeof s->key, "mesh_async_%s%s", name.UTF8String, tag);
    require(s->fn != nil, "test shader function exists");
    if ([name isEqualToString:@"direct"]) { s->mesh_obj = [library newFunctionWithName:@"block"]; require(s->mesh_obj != nil, "the block object function exists"); }   // a mesh shader with no task: its object function (main's mesh ABI)
    put_msl(s, source); return s;
}
enum { W = 8, H = 8, CMD = 65536, POOL = 131072 };
static NVNqueue queue; static NVNcommandBuffer command; static tex_t color, depth; static NVNdepthStencilState ds; static uint8_t *memory;
typedef void (*DrawMeshTasks)(NVNcommandBuffer *, int, int);
typedef void (*DrawMeshTasksIndirect)(NVNcommandBuffer *, NVNbufferAddress);
static DrawMeshTasks mesh_draw; static DrawMeshTasksIndirect mesh_draw_ind;
typedef struct { uint64_t *report; NVNbufferAddress ind, cond; int noreset, nodraw; prog_t *more[3]; } opts;   // noreset: report without a ResetCounter first; nodraw: no draw; more: further programs drawn after it
// One submission: clear, [reset the samples-passed counter], [render only if the report at cond is not 0], DrawMeshTasks(first, 1) or its indirect form,
// [report the counter]; returns 1 when every colour pixel is green
static int submit(prog_t *program, int first, opts o) {
    const NVNtexture *colors[] = {(const NVNtexture *)&color}; const float viewport[] = {0, 0, W, H}, blue[] = {0, 0, 1, 1};
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, (const NVNtexture *)&depth, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    P(nvnCommandBufferSetDepthRange)(&command, 0, 1); P(nvnCommandBufferBindDepthStencilState)(&command, &ds);
    P(nvnCommandBufferClearDepthStencil)(&command, 1, 1, 0, 0); P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    if (o.report) { o.report[0] = 0xdead; o.report[2] = 0; if (!o.noreset) P(nvnCommandBufferResetCounter)(&command, NVN_COUNTER_TYPE_SAMPLES_PASSED); }
    if (o.cond) P(nvnCommandBufferSetRenderEnableConditional)(&command, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, o.cond);
    if (!o.nodraw) { P(nvnCommandBufferBindProgram)(&command, (const NVNprogram *)program, 0xc3);
        if (o.ind) mesh_draw_ind(&command, o.ind); else mesh_draw(&command, first, 1); }
    for (int i = 0; i < 3 && o.more[i]; i++) { P(nvnCommandBufferBindProgram)(&command, (const NVNprogram *)o.more[i], 0xc3); mesh_draw(&command, first, 1); }
    if (o.cond) P(nvnCommandBufferSetRenderEnable)(&command, 1);
    if (o.report) P(nvnCommandBufferReportCounter)(&command, NVN_COUNTER_TYPE_SAMPLES_PASSED, (NVNbufferAddress)(uintptr_t)o.report);
    NVNcommandHandle handle = P(nvnCommandBufferEndRecording)(&command);
    P(nvnQueueSubmitCommands)(&queue, 1, &handle); P(nvnQueueFinish)(&queue);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared]; res_add(pixels);
    mtl_sub_begin();
    enc_b_copy_texture_buffer(blit(), tex_mtl(&color), 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), pixels, 0, W * 4, W * H * 4);
    require(mtl_wait(mtl_sub_commit(), 5000000000ull), "readback finishes"); CHECK(!mtl_last_commit_error(), "Metal reports no execution error");
    const uint8_t green[] = {0, 255, 0, 255}, blue8[] = {0, 0, 255, 255}; int all_green = 1, all_blue = 1;
    for (int i = 0; i < W * H; i++) { all_green &= !memcmp((const uint8_t *)pixels.contents + i * 4, green, 4); all_blue &= !memcmp((const uint8_t *)pixels.contents + i * 4, blue8, 4); }
    res_remove(pixels);
    CHECK(all_green || all_blue, "a submission draws the whole triangle or nothing");
    return all_green;
}
// Submits until the program draws (the background build is published), at most ~10 s; returns the submissions it took, 0 = never
static int until_drawn(prog_t *program, opts o) {
    for (int n = 1; n <= 1000; n++) { if (submit(program, 1, o)) return n; usleep(10000); }
    return 0;
}
static prog_t make_program(NvShader *ts, NvShader *ms, NvShader *fs) {
    NvProg *p = [NvProg new]; p->sh[4] = ms; p->sh[3] = ts; p->sh[1] = fs;
    prog_t pr = {0}; pr.magic = MAGIC('P','R','O','G'); pr.serial = nvn_new_serial(); serial_put(pr.serial, p); return pr;
}
// Depth pre-pass pairs: one frame (g_cur_frame + 1) of [the pre-pass program: LESS with depth write, red, inside a samples-passed query], [a copy of its depth into depth2,
// the colour pass's target], [the colour program: EQUAL, no depth write, green], over the same geometry (same task shader, z from the same first). Returns 0 nothing drawn (blue),
// 1 green (both, or the colour pass alone over its depth), 2 red (the pre-pass depth without its colour: the white-silhouette case), -1 mixed
static tex_t depth2; static NVNdepthStencilState ds_eq; static uint64_t test_frame = 1000;
static int frame_pair(prog_t *pre, prog_t *col, int copy, uint64_t *report) {
    atomic_store(&g_cur_frame, ++test_frame);
    const NVNtexture *colors[] = {(const NVNtexture *)&color}; const float viewport[] = {0, 0, W, H}, blue[] = {0, 0, 1, 1};
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, (const NVNtexture *)&depth, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    P(nvnCommandBufferSetDepthRange)(&command, 0, 1); P(nvnCommandBufferBindDepthStencilState)(&command, &ds);
    P(nvnCommandBufferClearDepthStencil)(&command, 1, 1, 0, 0); P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    if (report) { report[0] = 0xdead; P(nvnCommandBufferResetCounter)(&command, NVN_COUNTER_TYPE_SAMPLES_PASSED); }
    if (pre) { P(nvnCommandBufferBindProgram)(&command, (const NVNprogram *)pre, 0xc3); mesh_draw(&command, 1, 1); }
    if (report) P(nvnCommandBufferReportCounter)(&command, NVN_COUNTER_TYPE_SAMPLES_PASSED, (NVNbufferAddress)(uintptr_t)report);
    if (copy) { NVNcopyRegion r = {0, 0, 0, W, H, 1};
        P(nvnCommandBufferCopyTextureToTexture)(&command, (const NVNtexture *)&depth, NULL, &r, (const NVNtexture *)&depth2, NULL, &r, 0);
        P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, (const NVNtexture *)&depth2, NULL); }
    if (col) { P(nvnCommandBufferBindDepthStencilState)(&command, &ds_eq); P(nvnCommandBufferBindProgram)(&command, (const NVNprogram *)col, 0xc3); mesh_draw(&command, 1, 1); }
    NVNcommandHandle handle = P(nvnCommandBufferEndRecording)(&command);
    P(nvnQueueSubmitCommands)(&queue, 1, &handle); P(nvnQueueFinish)(&queue);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared]; res_add(pixels);
    mtl_sub_begin();
    enc_b_copy_texture_buffer(blit(), tex_mtl(&color), 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), pixels, 0, W * 4, W * H * 4);
    require(mtl_wait(mtl_sub_commit(), 5000000000ull), "readback finishes"); CHECK(!mtl_last_commit_error(), "Metal reports no execution error");
    const uint8_t want[3][4] = {{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}}; int all[3] = {1, 1, 1};
    for (int i = 0; i < W * H; i++) for (int c = 0; c < 3; c++) all[c] &= !memcmp((const uint8_t *)pixels.contents + i * 4, want[c], 4);
    res_remove(pixels);
    return all[0] ? 0 : all[1] ? 1 : all[2] ? 2 : -1;
}
static void make_pair(id<MTLLibrary> library, const char *tag, prog_t *pre, prog_t *col) {   // fresh keys: a task shader shared by a red pre-pass and a green colour pass
    NvShader *t = shader(library, @"task", 3, [NSString stringWithFormat:@"_pt%s", tag].UTF8String); t->mesh_kind = 1; t->mesh_threads = 32; t->mesh_payload = 4;
    NvShader *md = shader(library, @"tasked", 4, [NSString stringWithFormat:@"_pd%s", tag].UTF8String), *mc = shader(library, @"tasked", 4, [NSString stringWithFormat:@"_pc%s", tag].UTF8String);
    md->mesh_kind = mc->mesh_kind = 2; md->mesh_threads = mc->mesh_threads = 32;
    NvShader *fr = shader(library, @"red", 1, [NSString stringWithFormat:@"_pr%s", tag].UTF8String), *fg = shader(library, @"color2", 1, [NSString stringWithFormat:@"_pg%s", tag].UTF8String);
    fr->rf.color_mask = fg->rf.color_mask = 1;
    *pre = make_program(t, md, fr); *col = make_program(t, mc, fg);
}
// Frames of a pair until it draws green (at most n frames, gap us apart); *red counts frames that showed the pre-pass alone
static int pair_until_green(prog_t *pre, prog_t *col, int copy, int n, int gap, int *red) {
    for (int i = 1; i <= n; i++) { int r = frame_pair(pre, col, copy, NULL); if (r == 2) (*red)++; if (r == 1) return i; usleep((useconds_t)gap); }
    return 0;
}
// mtk_pso straight, as draw_ calls it for a task / mesh draw: the colour / depth formats of the test's pass. Returns its pipeline (nil = the draw is skipped)
static id<MTLRenderPipelineState> mtk_pso_for(NvShader *ts, NvShader *ms, NvShader *fs) {
    memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; g_pass.dfmt = MTLPixelFormatDepth32Float; g_pass.sfmt = 0; g_pass.samples = 1;
    NvProg *raster = [NvProg new]; raster->sh[1] = fs;
    g_mtk.on = 1; g_mtk.ts = ts; g_mtk.ms = ms;
    id<MTLRenderPipelineState> ps = mtk_pso(raster);
    g_mtk.on = 0; g_mtk.ts = g_mtk.ms = nil; return ps;
}
int main(void) { @autoreleasepool {
    setenv("DM_PSO_MESH_ASYNC", "skip", 0);   // DM_PSO_MESH_ASYNC=0 from outside runs the same checks without the switch (they must fail)
    mtl_init(); NSError *error = nil;
    CHECK(mtk_async_cap() >= 2, "the default cap is at least 2 (max(2, performance cores / 2))");
    g_mtk_async_max = 2;   // the cap and lost-cap checks below are written for 2
    char tmpl[] = "/tmp/t_mesh_async.XXXXXX"; require(mkdtemp(tmpl) != NULL, "fixture folder made"); fixture = @(tmpl); g_msl_dir = fixture;
    source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 position [[position]]; }; struct Payload { uint first; }; struct Block { uint first; };\n"
        "using Mesh = metal::mesh<V, void, 3, 1, metal::topology::triangle>;\n"
        "void triangle(Mesh m, uint first) { float z = float(first) * 0.125f;\n"
        "  m.set_vertex(0, V{float4(-1,-1,z,1)}); m.set_vertex(1, V{float4(3,-1,z,1)}); m.set_vertex(2, V{float4(-1,3,z,1)});\n"
        "  m.set_index(0,0); m.set_index(1,1); m.set_index(2,2); m.set_primitive_count(1); }\n"
        // As dmsh_meshstage.py emits them (as t_mesh_first_use): the task and object functions read the draw's {count, first} at buffer 29; a mesh shader with no task has its own
        // object function (mesh_obj) that hands each block's first workgroup to the mesh stage
        "[[object]] void block(object_data Block& b [[payload]], mesh_grid_properties grid, uint otg [[threadgroup_position_in_grid]], constant uint* draw [[buffer(29)]]) {\n"
        "  uint base = otg * 1024u; b.first = draw[1] + base; grid.set_threadgroups_per_grid(uint3(min(draw[0] - base, 1024u), 1u, 1u)); }\n"
        "[[mesh]] void direct(Mesh m, const object_data Block& b [[payload]], uint tg [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) { if (!tid) triangle(m, b.first + tg); }\n"
        "[[object]] void task(object_data Payload& p [[payload]], mesh_grid_properties grid, uint tid [[thread_index_in_threadgroup]], constant uint* draw [[buffer(29)]]) {\n"
        "  if (!tid) { p.first = draw[1]; grid.set_threadgroups_per_grid(uint3(1)); } }\n"
        "[[mesh]] void tasked(Mesh m, const object_data Payload& p [[payload]], uint tid [[thread_index_in_threadgroup]]) { if (!tid) triangle(m, p.first); }\n"
        "fragment float4 color() { return float4(0,1,0,1); }\n"
        "fragment float4 color2() { return float4(0,1,0,1); }\n"
        "fragment float4 red() { return float4(1,0,0,1); }\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!library) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    require(library != nil, "synthetic task and mesh shaders compile");
    NvShader *ms = shader(library, @"direct", 4, ""), *tasked = shader(library, @"tasked", 4, ""), *ts = shader(library, @"task", 3, ""), *fs = shader(library, @"color", 1, "");
    NvShader *fs2 = shader(library, @"color2", 1, ""), *ms_ind = shader(library, @"direct", 4, "_ind"), *ms_lm = shader(library, @"direct", 4, "_lmap");
    NvShader *tasked_end = shader(library, @"tasked", 4, "_end"), *ts_end = shader(library, @"task", 3, "_end");
    for (NvShader *m in @[ms, tasked, ms_ind, ms_lm, tasked_end]) { m->mesh_kind = 2; m->mesh_threads = 32; }
    ms->mesh_notask = ms_ind->mesh_notask = ms_lm->mesh_notask = 1;
    for (NvShader *t in @[ts, ts_end]) { t->mesh_kind = 1; t->mesh_threads = 32; t->mesh_payload = 4; }
    fs->rf.color_mask = fs2->rf.color_mask = 1;
    prog_t mesh_only = make_program(nil, ms, fs), task_mesh = make_program(ts, tasked, fs), indirect = make_program(nil, ms_ind, fs2), lmap = make_program(nil, ms_lm, fs2),
           at_end = make_program(ts_end, tasked_end, fs2);
    NVNdevice device; NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db);
    require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device);
    require(P(nvnQueueInitialize)(&queue, &qb), "queue initialized");
    require(!posix_memalign((void **)&memory, getpagesize(), POOL), "pool storage allocated"); memset(memory, 0, POOL);
    NVNmemoryPool pool; NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24);
    require(P(nvnMemoryPoolInitialize)(&pool, &pb), "memory pool initialized");
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized");
    P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, CMD);
    mesh_draw = (DrawMeshTasks)nvn_lookup("nvnCommandBufferDrawMeshTasks"); require(mesh_draw != NULL, "native DrawMeshTasks entry point exists");
    mesh_draw_ind = (DrawMeshTasksIndirect)nvn_lookup("nvnCommandBufferDrawMeshTasksIndirect"); require(mesh_draw_ind != NULL, "native DrawMeshTasksIndirect entry point exists");
    color = (tex_t){.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()}; depth = (tex_t){.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()};
    color.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1};
    depth.d = color.d; depth.d.format = NVN_FORMAT_DEPTH32F;
    depth2 = (tex_t){.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()}; depth2.d = depth.d;
    P(nvnDepthStencilStateSetDefaults)(&ds); P(nvnDepthStencilStateSetDepthTestEnable)(&ds, 1);
    P(nvnDepthStencilStateSetDepthWriteEnable)(&ds, 1); P(nvnDepthStencilStateSetDepthFunc)(&ds, NVN_DEPTH_FUNC_LESS);
    P(nvnDepthStencilStateSetDefaults)(&ds_eq); P(nvnDepthStencilStateSetDepthTestEnable)(&ds_eq, 1);
    P(nvnDepthStencilStateSetDepthWriteEnable)(&ds_eq, 0); P(nvnDepthStencilStateSetDepthFunc)(&ds_eq, NVN_DEPTH_FUNC_EQUAL);
    uint64_t *report = (uint64_t *)(memory + CMD + 256); uint32_t *rec = (uint32_t *)(memory + CMD + 1024);   // the report, then the indirect record

    // 1. mesh only: the first draw is skipped (the submission does not wait for the build), later ones draw once it is published
    printf("mesh only: first use skipped, then drawn\n");
    CHECK(!submit(&mesh_only, 1, (opts){0}), "the first draw of a pipeline not built yet draws nothing (skipped, not waited for)");
    CHECK(g_mtk_async_builds == 1 && g_mtk_async_skips >= 1, "the skip started one background build");
    int n = until_drawn(&mesh_only, (opts){0});
    CHECK(n > 0, "the program draws once its background build is published");
    CHECK(submit(&mesh_only, 1, (opts){0}), "later submissions draw with the finished pipeline");
    CHECK(g_mtk_async_builds == 1, "a published pipeline is not built again");
    printf("  drawn after %d more submissions, %llu draws skipped\n", n, (unsigned long long)g_mtk_async_skips);

    // 2. task and mesh inside an occlusion query: the skipped draw's report says visible (>= 1), never 0; conditional rendering on that report then draws
    printf("task and mesh: occlusion query and conditional rendering around a skipped draw\n");
    uint64_t skips0 = g_mtk_async_skips;
    CHECK(!submit(&task_mesh, 1, (opts){.report = report}), "the first task / mesh draw is skipped");
    CHECK(g_mtk_async_skips > skips0, "that draw was counted as skipped");
    CHECK(report[0] != 0xdead, "the samples-passed report was written");
    CHECK(report[0] >= 1, "a report over a skipped draw is not 0 (the object is not hidden by the skip)");
    printf("  report over the skipped draw: %llu samples\n", (unsigned long long)report[0]);
    CHECK(submit(&mesh_only, 1, (opts){.cond = (NVNbufferAddress)(uintptr_t)report}), "conditional rendering on that report draws (render if not equal to 0)");
    CHECK(!submit(NULL, 1, (opts){.report = report, .noreset = 1, .nodraw = 1}) && report[0] != 0xdead, "a second report without a reset was written");
    CHECK(report[0] >= 1, "a report is cumulative since the reset: a second one with no reset still covers the skipped draw (not 0)");
    n = until_drawn(&task_mesh, (opts){.report = report});
    CHECK(n > 0, "the task / mesh program draws once its build is published");
    CHECK(report[0] == W * H, "once drawn, the report counts the real samples again");

    // 3. an indirect draw (DrawMeshTasksIndirect, record {1, 1, 1}: one workgroup from 0) is skipped and drawn the same way
    printf("indirect: skipped, then drawn\n");
    rec[0] = 1; rec[1] = 1; rec[2] = 1; rec[3] = 0;
    skips0 = g_mtk_async_skips;
    CHECK(!submit(&indirect, 0, (opts){.ind = (NVNbufferAddress)(uintptr_t)rec}), "the first indirect draw of a pipeline not built yet draws nothing");
    CHECK(g_mtk_async_skips > skips0, "the indirect draw was counted as skipped");
    n = 0; for (int i = 1; i <= 1000 && !n; i++) { if (submit(&indirect, 0, (opts){.ind = (NVNbufferAddress)(uintptr_t)rec})) n = i; else usleep(10000); }
    CHECK(n > 0, "the indirect draw draws once its build is published");

    // 4. a skip does not leave the merged pipeline state of its draw to the next one
    printf("skip and the merged pipeline state\n");
    static const uint8_t lm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    g_pso_lmap = lm; g_pso_fsfn = fs2->fn;   // as a readable-tile merge or a same-pass feedback read sets them for the draw (draw_)
    CHECK(!submit(&lmap, 1, (opts){0}), "the draw of a new pipeline is skipped");
    CHECK(g_pso_lmap == NULL && g_pso_fsfn == nil, "the skip reset the draw's lmap / fsfn as a draw does");
    g_pso_lmap = NULL; g_pso_fsfn = nil;
    CHECK(until_drawn(&lmap, (opts){0}) > 0, "that program then draws");

    // 5. the write gate, through mtk_pso itself: a program is skipped only when no stage can write memory (reflection and MSL); any writer waits (its pipeline is built at once)
    printf("write gate\n");
    NSString *reader = @"constant uint* c [[buffer(0)]]; const device float4* s [[buffer(1)]]; texture2d<float> t [[texture(0)]]; float4 v = t.sample(sm, uv) + s[c[0]];\n"
        "template <typename T> struct spvUnsafeArray { T e[4]; device T& operator [] (size_t pos) device { return e[pos]; } };\n"
        "static inline void dm_hg_rec(uint site, uint v, device atomic_uint* hg)\n{\n    atomic_store_explicit(&hg[1], v, memory_order_relaxed);\n}\n"
        "    device atomic_uint* dm_hg = dm_hg_buf; atomic_fetch_add_explicit(&dm_hg[0], 1u, memory_order_relaxed);\n";
    NSString *img_store = [reader stringByAppendingString:@"texture2d<float, access::write> o = pool_tex(h); o.write(v, uint2(0));\n"];
    NSString *gmem_store = [reader stringByAppendingString:@"*(device uint*)(gmem_base + addr) = 1u;\n"];
    NSString *frag_atomic = [reader stringByAppendingString:@"atomic_fetch_add_explicit((device atomic_uint*)counter, 1u, memory_order_relaxed);\n"];
    NSString *tex_atomic = [reader stringByAppendingString:@"img.atomic_fetch_add(uint2(0), 1u);\n"];
    NSString *rw_param = [reader stringByAppendingString:@"[[mesh]] void m(device _25* cp_s1 [[id(3)]]) {}\n"];
    struct { const char *name; int kind, rw, bindless, dyn, pool, stage; NSString *code; int skip; } cases[] = {
        // reflection marks a write: waits whatever the code says
        {"SBUFP writable (mesh) waits", NVN_RB_SBUFP, 1, 0, 0, 0, 4, reader, 0}, {"SBUFU writable (mesh) waits", NVN_RB_SBUFU, 1, 0, 0, 0, 4, reader, 0},
        {"GMEM writable (task) waits", NVN_RB_GMEM, 1, 0, 0, 0, 3, reader, 0}, {"TBUF writable (task) waits", NVN_RB_TBUF, 1, 0, 0, 0, 3, reader, 0},
        {"TBUFU writable (pixel) waits", NVN_RB_TBUFU, 1, 0, 0, 0, 1, reader, 0}, {"IMG (pixel) waits", NVN_RB_IMG, 0, 0, 0, 0, 1, reader, 0},
        {"IMGU (mesh) waits", NVN_RB_IMGU, 0, 0, 0, 0, 4, reader, 0}, {"GMEML (mesh) waits", NVN_RB_GMEML, 0, 0, 0, 0, 4, reader, 0},
        // read-only bindings with read-only code: skipped (OoT's 990 ms mesh pipeline: read-only GMEM in the mesh stage, texture / sampler pools in the pixel stage)
        {"read-only GMEM (mesh) skips", NVN_RB_GMEM, 0, 0, 0, 0, 4, reader, 1}, {"read-only SBUFU (mesh) skips", NVN_RB_SBUFU, 0, 0, 0, 0, 4, reader, 1},
        {"TEXA (pixel) skips", NVN_RB_TEXA, 0, 0, 0, 0, 1, reader, 1}, {"SAMPA (mesh) skips", NVN_RB_SAMPA, 0, 0, 0, 0, 4, reader, 1},
        {"bindless reader (mesh) skips", 0, 0, 1, 0, 0, 4, reader, 1}, {"dyn reader (pixel) skips", 0, 0, 0, 1, 0, 1, reader, 1},
        {"texture / sampler pool reader (pixel) skips", NVN_RB_TEXP, 0, 0, 0, 1, 1, reader, 1}, {"loop guard writes only (task) skips", 0, 0, 0, 0, 0, 3, reader, 1},
        // read-only bindings, code that writes: waits
        {"bindless image store (pixel) waits", 0, 0, 1, 0, 0, 1, img_store, 0}, {"GMEM store through a pool address (mesh) waits", NVN_RB_GMEM, 0, 0, 0, 0, 4, gmem_store, 0},
        {"device atomic in the pixel stage waits", 0, 0, 0, 0, 0, 1, frag_atomic, 0}, {"texture atomic (task) waits", 0, 0, 0, 0, 0, 3, tex_atomic, 0},
        {"writable device parameter (mesh) waits", NVN_RB_SBUFP, 0, 0, 0, 0, 4, rw_param, 0}, {"no MSL to check (pixel) waits", 0, 0, 0, 0, 0, 1, nil, 0},
    };
    NSMutableArray *alive = [NSMutableArray array];   // pipeline keys hold shader pointers: a freed shader's address would come back for the next one (and hit its pipeline)
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        // a copy of a working stage (same function, so the pipeline builds) with the reflection and code of the case: never encoded, only built
        int st = cases[i].stage; NvShader *src = st == 4 ? ms : st == 3 ? ts : fs;
        NvShader *w = shader(library, src->fn.name, st, [NSString stringWithFormat:@"_g%zu", i].UTF8String); [alive addObject:w];
        w->mesh_kind = src->mesh_kind; w->mesh_threads = src->mesh_threads; w->mesh_notask = src->mesh_notask; w->mesh_payload = src->mesh_payload; w->rf.color_mask = src->rf.color_mask;
        if (cases[i].kind) { w->rf.nb = 1; w->rf.b[0].kind = (uint8_t)cases[i].kind; w->rf.b[0].rw = (uint8_t)cases[i].rw; }
        w->rf.bindless = (uint8_t)cases[i].bindless; w->rf.dyn = (uint8_t)cases[i].dyn; w->rf.pool = (uint8_t)cases[i].pool;
        put_msl(w, cases[i].code);
        NvShader *m = st == 4 ? w : ms;
        if (st == 3) {   // a fresh mesh partner: the key names the task stage by its function, which the library hands out as one object for every load of an entry
            m = shader(library, @"tasked", 4, [NSString stringWithFormat:@"_g%zu", i].UTF8String); m->mesh_kind = 2; m->mesh_threads = 32; [alive addObject:m]; }
        NvShader *t = st == 3 ? w : nil, *f = st == 1 ? w : fs;
        CHECK(mtk_async_may_skip(NULL, t, m, f) == cases[i].skip, cases[i].name);
        uint64_t s0 = g_mtk_async_skips, w0 = g_mtk_async_waits;
        id<MTLRenderPipelineState> ps = mtk_pso_for(t, m, f);
        if (cases[i].skip) CHECK(!mtk_async_on() || (ps == nil && g_mtk_async_skips == s0 + 1), "mtk_pso skips a reader's draw while its pipeline builds");
        else { CHECK(ps != nil && g_mtk_async_skips == s0, "mtk_pso builds a writer's pipeline at once instead of skipping its draw");
               if (mtk_async_on()) CHECK(g_mtk_async_waits == w0 + 1, "the wait is counted"); }
    }
    for (int i = 0; i < 500 && atomic_load(&g_mtk_async_running); i++) usleep(10000);   // the readers' builds finish before the cap test

    // 5b. the MSL scan on its own: whitespace, includes, the loop guard's exact forms, spvUnsafeArray, long lines
    printf("store proof of MSL text\n");
    struct { const char *name, *code; int ok; } texts[] = {
        {"plain reader", "#include <metal_stdlib>\nconst device float4* s [[buffer(1)]];\nfloat4 v = s[0];\n", 1},
        {"const and device on two lines", "const\n    device float4* s [[buffer(1)]];\n", 1}, {"const<TAB>device", "const\tdevice uint* s;\n", 1}, {"device const", "uint x = *reinterpret_cast<device const uint*>(w);\n", 1},
        {"another #include", "#include <metal_stdlib>\n#include \"helpers.h\"\n", 0}, {"# include of a game header, with a space", "# include \"game.h\"\n", 0}, {"# include of a system header, with a space", "# include <simd/simd.h>\n", 1},
        {"device split from its store", "device\n uint* p = (device\n uint*)a; *p = 1u;\n", 0},
        {"loop guard parameter", "[[object]] void main0(device atomic_uint* dm_hg_buf [[buffer(28)]]) { device atomic_uint* dm_hg = dm_hg_buf; }\n", 1},
        {"guard buffer at another index", "void main0(device atomic_uint* dm_hg_buf [[buffer(27)]]) {}\n", 0},
        {"dm_hgX is not the guard", "void main0(device atomic_uint* dm_hgX) {}\n", 0},
        {"the guard passed to dm_stage_body", "static inline void dm_stage_body(constant S& s, device atomic_uint* dm_hg, uint dm_hgcap)\n{\n}\n", 1},
        {"a dm_hg parameter of another function", "static inline void game_body(device atomic_uint* dm_hg)\n{\n}\n", 0},
        {"guard helper parameter", "static inline void dm_hg_rec(uint site, uint v, device atomic_uint* hg)\n{ atomic_store_explicit(&hg[1], v, memory_order_relaxed); }\n", 1},
        {"a game buffer named hg", "static inline void game_fn(device atomic_uint* hg) { atomic_store_explicit(&hg[0], 1u, memory_order_relaxed); }\n", 0},
        {"a game uint buffer hg in a guard-named function", "static inline void dm_hgz(device uint* hg) { hg[0] = 1u; }\n", 0},
        {"spvUnsafeArray operator", "template <typename T> struct spvUnsafeArray { T e[4]; device T& operator [] (size_t pos) device { return e[pos]; } };\n", 1},
        {"a write on an operator[] line", "device T& operator [] (size_t pos) device { img.write(float4(0), uint2(0)); return e[pos]; }\n", 0},
        {"texture write split over lines", "o\n  .write (v, uint2(0));\n", 0}, {"texture atomic", "img.atomic_fetch_add(uint2(0), 1u);\n", 0},
        {"access::write", "texture2d<float, access::write> o [[texture(0)]];\n", 0},
        {"system includes", "#include <metal_stdlib>\n#include <simd/simd.h>\n#include <metal_atomic>\n", 1}, {"#import", "#import <metal_stdlib>\n", 0},
        {"_Pragma", "_Pragma(\"clang diagnostic ignored\")\n", 0}, {"#define naming device", "#define W(p) (*(device uint*)(p) = 1u)\n", 0},
        {"#define continued onto a line naming write", "#define W(t) \\\n  t.write(v, c)\n", 0}, {"a harmless #define", "#define DM_HG_KEY_HI 0xa5288d0au\n", 1},
        {"#undef naming atomic", "#undef atomic_thing\n", 0},
        {"the translator's pool mark", "template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)\n{\n"
            "    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);\n    uint b = 1u << (i & 31u);\n"
            "    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);\n    return i;\n}\n", 1},
        {"a pool mark on another field", "template<typename P> static inline uint dm_pool_mark(constant P* p, uint i)\n{\n    device uint* w = reinterpret_cast<device uint*>(p->data) + (i >> 5);\n    return i;\n}\n", 0},
        {"the mark's statements in another function", "template<typename P> static inline uint game_mark(constant P* p, uint i)\n{\n    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);\n    return i;\n}\n", 0},
        {"another store in the mark's function", "template<typename P> static inline uint dm_pool_mark(constant P* p, uint i)\n{\n    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);\n    *w = 0u;\n    *(device uint*)(p->seen) = 1u;\n    return i;\n}\n", 0},
        {"trailing device without operator on its line", "device_thing(size_t pos) device\n", 0},
    };
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) CHECK(mtk_msl_no_stores(texts[i].code, strlen(texts[i].code)) == texts[i].ok, texts[i].name);
    NSMutableString *lng = [NSMutableString stringWithString:@"const device float4* s; float4 v = "]; for (int i = 0; i < 2000; i++) [lng appendString:@"s[0] + "]; [lng appendString:@"0;"];
    CHECK(lng.length > 8192 && mtk_msl_no_stores(lng.UTF8String, lng.length), "a reader line over 8 KB is checked whole");
    [lng appendString:@" *(device uint*)a = 1u;\n"];
    CHECK(!mtk_msl_no_stores(lng.UTF8String, lng.length), "a store at the end of a line over 8 KB is found");
    CHECK(!mtk_msl_no_stores("a\0device", 9), "text with a NUL inside is not proven");

    // 5c. the library path: the MSL beside the library shader_load chose, never another one
    printf("library path\n");
    NvShader *pth = shader(library, @"direct", 4, "_path");
    char otmpl[] = "/tmp/t_mesh_async_ovr.XXXXXX"; require(mkdtemp(otmpl) != NULL, "override folder made"); NSString *ovd = @(otmpl);   // outside g_msl_dir
    NSString *writer = @"*(device uint*)a = 1u;\n";
    pth->lib_path = [NSString stringWithFormat:@"%@/%s.metallib", fixture, pth->key];
    CHECK(mtk_proof(pth) == 1, "a cache library: the cache's <key>.metal (read through msl_data) proves it");
    pth->lib_path = [NSString stringWithFormat:@"%@/%s.metallib", ovd, pth->key]; pth->lib_cache = 0;
    CHECK(mtk_proof(pth) == 2, "an override library without its .metal is not proven (the cache's reader text is not used)");
    [writer writeToFile:[NSString stringWithFormat:@"%@/%s.metal", ovd, pth->key] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    CHECK(mtk_proof(pth) == 2, "an override library is judged by its own .metal (a writer here)");
    pth->lib_path = [NSString stringWithFormat:@"%@/%s.ovr.metallib", ovd, pth->key];
    CHECK(mtk_proof(pth) == 2, "an .ovr.metallib without its .ovr.metal is not proven (no fall-through to <key>.metal)");
    [source writeToFile:[NSString stringWithFormat:@"%@/%s.ovr.metal", ovd, pth->key] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    CHECK(mtk_proof(pth) == 1, "an .ovr.metallib is judged by its .ovr.metal");
    pth->lib_path = nil; CHECK(mtk_proof(pth) == 2, "no library path: not proven");
    [NSFileManager.defaultManager removeItemAtPath:ovd error:nil];
    NvShader *bg = shader(library, @"direct", 4, "_bg"); bg->mtk_store = 0;
    CHECK(!mtk_async_may_skip(NULL, nil, bg, fs), "a stage whose proof is not known yet waits");
    mtk_proof_note(bg); for (int i = 0; i < 500 && !__atomic_load_n(&bg->mtk_store, __ATOMIC_ACQUIRE); i++) usleep(2000);
    CHECK(!mtk_async_on() || __atomic_load_n(&bg->mtk_store, __ATOMIC_ACQUIRE) == 1, "the background proof at load proves it");

    // 5d. pipelines that bind another library's entry points (alpha test / logic operation from _bx) wait
    pso_key vk = {0}; vk.atest = 1; CHECK(!mtk_async_may_skip(&vk, nil, ms, fs), "an alpha-test pipeline waits");
    memset(&vk, 0, sizeof vk); vk.logic = 1; CHECK(!mtk_async_may_skip(&vk, nil, ms, fs), "a logic-operation pipeline waits");
    memset(&vk, 0, sizeof vk); CHECK(mtk_async_may_skip(&vk, nil, ms, fs), "the same program without them may be skipped");

    // 5e. optional, a real built cache: T_MESH_ASYNC_CACHE=<.../shader/cache/b-<chain>> T_MESH_ASYNC_KEYS="<key>=<0|1>,..." checks the proof through msl_data on the library
    // (folder or .img) as the host reads it: 1 = expected to be proven read-only
    { const char *cache = getenv("T_MESH_ASYNC_CACHE"), *keys = getenv("T_MESH_ASYNC_KEYS");
      if (cache && *cache && keys) { NSString *keep = g_msl_dir; g_msl_dir = @(cache); printf("real cache %s\n", cache);
          for (NSString *kv in [@(keys) componentsSeparatedByString:@","]) { NSArray *p = [kv componentsSeparatedByString:@"="]; if (p.count != 2) continue;
              int stg = [p[0] hasPrefix:@"04452448"] || [p[0] hasPrefix:@"3fa631a8"] ? 4 : [p[0] hasPrefix:@"a5288d0a"] ? 3 : 1;   // the test's keys: mesh, task, pixel
              NvShader *r = shader_load(p[0], stg, 0);   // as the host loads it: its library path and cache flag come from shader_load
              CHECK(r != nil, "the real shader loads"); if (!r) continue;
              NSString *want_lp = [NSString stringWithFormat:@"%s/%@.metallib", cache, p[0]]; int lp_ok = r->lib_cache && [r->lib_path isEqualToString:want_lp];
              CHECK(lp_ok, "shader_load set the cache library path");
              NSData *d = msl_data([NSString stringWithFormat:@"%s/%@.metal", cache, p[0]]); int got = mtk_proof(r);
              printf("  %s: MSL %lu bytes, proof %d\n", r->key, (unsigned long)d.length, got);
              CHECK(d.length > 0, "the real cache holds the stage's <key>.metal");
              CHECK(got == ([p[1] intValue] ? 1 : 2), "the real shader's proof is as expected"); }
          g_msl_dir = keep; } }

    // 6. the cap: three new pipelines in one submission start two builds at once (two concurrent mesh builds); the third starts once a slot is free
    printf("cap: at most %d builds at once\n", mtk_async_cap());
    NvShader *cm[3]; prog_t cp[3];
    for (int i = 0; i < 3; i++) { cm[i] = shader(library, @"direct", 4, [NSString stringWithFormat:@"_cap%d", i].UTF8String); cm[i]->mesh_kind = 2; cm[i]->mesh_threads = 32; cm[i]->mesh_notask = 1; cp[i] = make_program(nil, cm[i], fs2); }
    g_mtk_async_test_delay_us = 400000; uint64_t b0 = g_mtk_async_builds;
    CHECK(!submit(&cp[0], 1, (opts){.more = {&cp[1], &cp[2]}}), "three new pipelines in one submission are all skipped");
    CHECK(g_mtk_async_builds - b0 == 2 && atomic_load(&g_mtk_async_running) == mtk_async_cap(), "two builds run at once, the third waits for a slot");
    usleep(700000);
    submit(&cp[0], 1, (opts){.more = {&cp[1], &cp[2]}});
    CHECK(g_mtk_async_builds - b0 == 3, "the third build starts once a slot is free");
    g_mtk_async_test_delay_us = 0;
    n = 0; for (int i = 1; i <= 1000 && !n; i++) { int all = 1; for (int j = 0; j < 3; j++) all &= submit(&cp[j], 1, (opts){0}); if (all) n = i; else usleep(10000); }
    CHECK(n > 0, "all three draw once built");

    // 6b. FIFO: with one slot, keys start in the order they were first skipped, whichever key is drawn when a slot frees
    printf("fifo: oldest first\n");
    NvShader *fq[3]; prog_t fp[3];
    for (int i = 0; i < 3; i++) { fq[i] = shader(library, @"direct", 4, [NSString stringWithFormat:@"_fifo%d", i].UTF8String); fq[i]->mesh_kind = 2; fq[i]->mesh_threads = 32; fq[i]->mesh_notask = 1; fp[i] = make_program(nil, fq[i], fs2); }
    g_mtk_async_max = 1; g_mtk_async_test_delay_us = 300000;
    CHECK(!submit(&fp[0], 1, (opts){.more = {&fp[1], &fp[2]}}), "three new pipelines skipped in order A, B, C");
    MtkAsync *fa = nil, *fb = nil, *fc = nil;
    for (MtkAsync *a in g_mtk_async.allValues) { if (a->ms == fq[0]) fa = a; else if (a->ms == fq[1]) fb = a; else if (a->ms == fq[2]) fc = a; }
    CHECK(fa && fb && fc && fa->started && !fb->started && !fc->started, "with one slot only A started");
    if (!(fa && fb && fc)) { fa = fb = fc = [MtkAsync new]; }   // without the switch there are no entries: keep going on a dummy (the checks above and below fail, as they should)
    for (int i = 0; i < 100 && atomic_load(&g_mtk_async_running); i++) usleep(10000);
    submit(&fp[2], 1, (opts){0});   // only C is drawn when the slot frees
    CHECK(fb->started && !fc->started, "the freed slot went to B (the oldest), not to C (the one drawn)");
    for (int i = 0; i < 100 && atomic_load(&g_mtk_async_running); i++) usleep(10000);
    submit(&fp[2], 1, (opts){0});
    CHECK(fc->started, "then C");
    g_mtk_async_test_delay_us = 0; g_mtk_async_max = 2;
    n = 0; for (int i = 1; i <= 1000 && !n; i++) { int all = 1; for (int j = 0; j < 3; j++) all &= submit(&fp[j], 1, (opts){0}); if (all) n = i; else usleep(10000); }
    CHECK(n > 0, "all three draw");

    // 7. a build past the deadline gives its slot back (a compile that does not return must not hold the cap); its late result is still used
    printf("deadline: a slow build gives its slot back\n");
    NvShader *sm = shader(library, @"direct", 4, "_slow"); sm->mesh_kind = 2; sm->mesh_threads = 32; sm->mesh_notask = 1; prog_t slow = make_program(nil, sm, fs2);
    double slow0 = g_mtk_async_slow_s; g_mtk_async_slow_s = 0.1; g_mtk_async_test_delay_us = 800000; uint64_t ab0 = g_mtk_async_abandoned;
    CHECK(!submit(&slow, 1, (opts){0}), "the slow pipeline's draw is skipped");
    usleep(300000);
    CHECK(!submit(&slow, 1, (opts){0}), "still skipped past the deadline");
    CHECK(g_mtk_async_abandoned == ab0 + 1 && atomic_load(&g_mtk_async_running) == 0, "past the deadline the build gave its slot back");
    CHECK(atomic_load(&g_mtk_async_lost) == 1, "the abandoned build counts as lost until it returns");
    g_mtk_async_test_delay_us = 0;
    CHECK(until_drawn(&slow, (opts){0}) > 0, "the late result is still published and drawn");
    CHECK(atomic_load(&g_mtk_async_running) == 0, "the late return does not give the slot back twice");
    CHECK(atomic_load(&g_mtk_async_lost) == 0, "the late return took the abandoned build off the lost count");

    // 7b. the lost cap: mtk_async_cap() builds that never return stop new builds; draws of a new pipeline are skipped, not built at the draw, until one returns
    printf("lost cap\n");
    NvShader *lm2[3]; prog_t lp2[3];
    for (int i = 0; i < 3; i++) { lm2[i] = shader(library, @"direct", 4, [NSString stringWithFormat:@"_lost%d", i].UTF8String); lm2[i]->mesh_kind = 2; lm2[i]->mesh_threads = 32; lm2[i]->mesh_notask = 1; lp2[i] = make_program(nil, lm2[i], fs2); }
    g_mtk_async_test_delay_us = 1500000; uint64_t lb0 = g_mtk_async_builds;
    CHECK(!submit(&lp2[0], 1, (opts){.more = {&lp2[1]}}), "two slow pipelines are skipped");
    usleep(300000); submit(&lp2[0], 1, (opts){.more = {&lp2[1]}});
    CHECK(atomic_load(&g_mtk_async_lost) == mtk_async_cap() && atomic_load(&g_mtk_async_running) == 0, "both are abandoned and counted as lost");
    uint64_t lw0 = g_mtk_async_waits;
    CHECK(!submit(&lp2[2], 1, (opts){0}), "at the lost cap a new pipeline's draw is skipped (not built at the draw)");
    CHECK(g_mtk_async_builds - lb0 == 2 && g_mtk_async_waits == lw0, "no build started for it, and nothing waited");
    g_mtk_async_test_delay_us = 0;
    for (int i = 0; i < 400 && atomic_load(&g_mtk_async_lost); i++) usleep(10000);
    CHECK(atomic_load(&g_mtk_async_lost) == 0, "the late returns took both off the lost count");
    n = 0; for (int i = 1; i <= 1000 && !n; i++) { int all = 1; for (int j = 0; j < 3; j++) all &= submit(&lp2[j], 1, (opts){0}); if (all) n = i; else usleep(10000); }
    CHECK(n > 0 && g_mtk_async_builds - lb0 == 3, "then the third builds and all three draw");
    g_mtk_async_slow_s = slow0;

    // 9. depth pre-pass + EQUAL colour pass (PR #2084 review): the colour pass is never skipped while its geometry's depth pass draws (that leaves the pre-pass depth with no
    // colour: Ocarina of Time's white foliage silhouettes, issue #1720), and once it is skipped its depth passes are skipped with it until it is built
    printf("depth pre-pass pairs\n");
    prog_t p9a, c9a, p9b, c9b, p9c, c9c, p9c2, c9c2, p9d, c9d;
    make_pair(library, "a", &p9a, &c9a); make_pair(library, "b", &p9b, &c9b); make_pair(library, "c", &p9c, &c9c); make_pair(library, "c2", &p9c2, &c9c2); make_pair(library, "d", &p9d, &c9d);
    // 9a. both cold: both skipped; with one slot the pre-pass builds first, and while the colour pass still builds the ready pre-pass is skipped with it (never red)
    g_mtk_async_max = 1; g_mtk_async_test_delay_us = 300000;
    uint64_t ds0 = g_mtk_pair_depth_skips;
    int r = frame_pair(&p9a, &c9a, 0, report);
    CHECK(r == 0, "both passes cold: nothing drawn (both skipped, no pre-pass depth without its colour)");
    CHECK(g_mtk_pairs.count >= 1, "the skipped colour pass entered the partner table");
    usleep(400000);   // the pre-pass's build returns; the colour pass's starts at the next draw
    r = frame_pair(&p9a, &c9a, 0, report);
    CHECK(r == 0 && g_mtk_pair_depth_skips > ds0, "the built pre-pass is skipped while its colour pass still builds");
    CHECK(report[0] >= 1 && report[0] != 0xdead, "an occlusion query over the pre-pass skipped with its colour pass reports visible");
    int red = 0; n = pair_until_green(&p9a, &c9a, 0, 1000, 10000, &red);
    CHECK(n > 0, "the pair draws once the colour pass is built");
    CHECK(red == 0, "no frame showed the pre-pass without its colour pass");
    // 9b. the pre-pass is built and draws: its colour pass, not built yet, waits for its build instead of being skipped
    g_mtk_async_max = 2; g_mtk_async_test_delay_us = 0;
    n = 0; for (int i = 1; i <= 1000 && !n; i++) { if (frame_pair(&p9b, NULL, 0, NULL) == 2) n = i; else usleep(10000); }
    CHECK(n > 0, "the pre-pass alone draws (its depth, red)");
    uint64_t pw0 = g_mtk_pair_waits; r = frame_pair(&p9b, &c9b, 0, NULL);
    CHECK(r == 1, "pre-pass drawn: the cold colour pass waits for its build and draws in the same frame (no red silhouette)");
    CHECK(g_mtk_pair_waits == pw0 + 1, "the colour pass's wait is counted as a pair wait");
    // 9c. the colour pass reads a copy of the pre-pass depth (copy_t2t into depth2): the copy carries the notes
    g_mtk_async_max = 1; g_mtk_async_test_delay_us = 300000;
    uint64_t nn0 = g_mtk_pair_nonote, aw0 = g_mtk_async_waits;
    r = frame_pair(&p9c, &c9c, 1, NULL);
    CHECK(r == 0 && g_mtk_pair_nonote == nn0 && g_mtk_async_waits == aw0, "over a copy of the skipped pre-pass depth the colour pass is skipped too (the copy carried the notes)");
    red = 0; n = pair_until_green(&p9c, &c9c, 1, 1000, 10000, &red);
    CHECK(n > 0 && red == 0, "through the copy: drawn once built, never the pre-pass alone");
    g_mtk_async_max = 2; g_mtk_async_test_delay_us = 0;
    n = 0; for (int i = 1; i <= 1000 && !n; i++) { if (frame_pair(&p9c2, NULL, 1, NULL) == 2) n = i; else usleep(10000); }
    pw0 = g_mtk_pair_waits; r = frame_pair(&p9c2, &c9c2, 1, NULL);
    CHECK(n > 0 && r == 1 && g_mtk_pair_waits == pw0 + 1, "through the copy: pre-pass drawn, the cold colour pass waits and draws");
    // 9d. expiry: a colour pass still building after MTK_PAIR_FRAMES frames lets its depth passes draw again, and then it waits for its build
    g_mtk_async_max = 1; g_mtk_async_test_delay_us = 1500000;
    r = frame_pair(&p9d, &c9d, 0, NULL); CHECK(r == 0, "expiry pair: both skipped");
    usleep(1700000);   // the pre-pass's build returns, the colour pass's starts at the next draw (1.5 s)
    ds0 = g_mtk_pair_depth_skips; r = frame_pair(&p9d, &c9d, 0, NULL);
    CHECK(r == 0 && g_mtk_pair_depth_skips == ds0 + 1, "expiry pair: the built pre-pass is held while the colour pass builds");
    uint64_t ex0 = g_mtk_pair_expired; test_frame += MTK_PAIR_FRAMES;
    r = frame_pair(&p9d, &c9d, 0, NULL);
    CHECK(g_mtk_pair_expired == ex0 + 1, "after MTK_PAIR_FRAMES frames the partner entry expires");
    CHECK(r == 1, "after the expiry the pre-pass draws and the colour pass waits for its build (green, not red)");
    g_mtk_async_test_delay_us = 0; g_mtk_async_max = 2;
    for (int i = 0; i < 300 && atomic_load(&g_mtk_async_running); i++) usleep(10000);
    CHECK(frame_pair(&p9d, &c9d, 0, NULL) == 1, "the expiry pair keeps drawing after the background build returns");

    // 8. teardown with a build in flight: a fresh key is skipped and the test ends at once; the process must exit cleanly (the build holds its own references)
    printf("teardown with a build in flight\n");
    CHECK(!submit(&at_end, 1, (opts){0}), "a last new pipeline is skipped");
    printf("  builds still running at teardown: %d\n", atomic_load(&g_mtk_async_running));
    CHECK(atomic_load(&g_mtk_async_running) <= mtk_async_cap(), "no more than mtk_async_cap() builds run at once");

    P(nvnQueueFinish)(&queue); mtl_drain();
    P(nvnCommandBufferFinalize)(&command); mtl_tex_release(&color); mtl_tex_release(&depth);
    P(nvnMemoryPoolFinalize)(&pool); free(memory); P(nvnQueueFinalize)(&queue); P(nvnDeviceFinalize)(&device);
    for (int i = 0; i < 500 && atomic_load(&g_mtk_async_running); i++) usleep(10000);   // the build in flight finishes before the process ends
    CHECK(atomic_load(&g_mtk_async_running) == 0, "the build in flight finished after the teardown");
    [NSFileManager.defaultManager removeItemAtPath:fixture error:nil];
    printf("mesh async: %d checks, %d failed\n", checks, failures); return failures != 0;
} }
