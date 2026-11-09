// Native DrawMeshTasks has no fallback when its pipeline is missing. Its very first
// submission must draw, including task payloads and programs with no fragment stage.
// A mesh shader with no task draws more workgroups than Metal's mesh grid holds (DMSH_MESH_GRID) through its object
// function's blocks (dmsh_meshstage.py main0_o), directly and indirectly: every workgroup draws its own pixel once.
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

static NvShader *shader(id<MTLLibrary> library, NSString *name, int stage) {
    NvShader *s = [NvShader new]; s->fn = [library newFunctionWithName:name]; s->stage = stage;
    snprintf(s->key, sizeof s->key, "first_use_%s", name.UTF8String);
    require(s->fn != nil, "test shader function exists"); return s;
}
int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 position [[position]]; }; struct Payload { uint first; }; struct Block { uint first; };\n"
        "using Mesh = metal::mesh<V, void, 3, 1, metal::topology::triangle>;\n"
        "void triangle(Mesh m, uint first) { float z = float(first) * 0.125f;\n"
        "  m.set_vertex(0, V{float4(-1,-1,z,1)}); m.set_vertex(1, V{float4(3,-1,z,1)}); m.set_vertex(2, V{float4(-1,3,z,1)});\n"
        "  m.set_index(0,0); m.set_index(1,1); m.set_index(2,2); m.set_primitive_count(1); }\n"
        // As dmsh_meshstage.py emits them: the task and object functions read the draw's {count, first} at buffer 29, and a mesh shader with no task its block's first workgroup
        "[[object]] void block(object_data Block& b [[payload]], mesh_grid_properties grid, uint otg [[threadgroup_position_in_grid]], constant uint* draw [[buffer(29)]]) {\n"
        "  uint base = otg * 1024u; b.first = draw[1] + base; grid.set_threadgroups_per_grid(uint3(min(draw[0] - base, 1024u), 1u, 1u)); }\n"
        "[[mesh]] void direct(Mesh m, const object_data Block& b [[payload]], uint tg [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) { if (!tid) triangle(m, b.first + tg); }\n"
        "[[object]] void task(object_data Payload& p [[payload]], mesh_grid_properties grid, uint tid [[thread_index_in_threadgroup]], constant uint* draw [[buffer(29)]]) {\n"
        "  if (!tid) { p.first = draw[1]; grid.set_threadgroups_per_grid(uint3(1)); } }\n"
        // a workgroup's own pixel of a GW x GH target (its number modulo the pixels), at depth (number + 1) / 8192: a triangle around that pixel's centre and no other
        "constant uint GW = 64, GH = 48;\n"
        "[[mesh]] void pixel(Mesh m, const object_data Block& b [[payload]], uint tg [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) {\n"
        "  if (tid) return; uint wg = b.first + tg, p = wg % (GW * GH); float dx = 2.0f / GW, dy = 2.0f / GH, z = float(wg + 1) / 8192.0f;\n"
        "  float2 c = float2((float(p % GW) + 0.5f) * dx - 1.0f, (float(p / GW) + 0.5f) * dy - 1.0f);\n"   // NVN's origin is the lower left: row 0 at y = -1
        "  m.set_vertex(0, V{float4(c.x - 0.25f * dx, c.y + 0.25f * dy, z, 1)}); m.set_vertex(1, V{float4(c.x + 0.75f * dx, c.y + 0.25f * dy, z, 1)});\n"
        "  m.set_vertex(2, V{float4(c.x - 0.25f * dx, c.y - 0.75f * dy, z, 1)}); m.set_index(0,0); m.set_index(1,1); m.set_index(2,2); m.set_primitive_count(1); }\n"
        "[[mesh]] void tasked(Mesh m, const object_data Payload& p [[payload]], uint tid [[thread_index_in_threadgroup]]) { if (!tid) triangle(m, p.first); }\n"
        "vertex float4 vertex_only(uint vid [[vertex_id]]) { const float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[vid % 3], float(vid / 3 + 1) * 0.125f, 1); }\n"
        "fragment float4 color() { return float4(0,1,0,1); }\n"
        options:nil error:&error];
    if (!library) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    require(library != nil, "synthetic task and mesh shaders compile");
    NvShader *ms = shader(library, @"direct", 4), *tasked = shader(library, @"tasked", 4), *ts = shader(library, @"task", 3), *fs = shader(library, @"color", 1);
    NvShader *vs = shader(library, @"vertex_only", 0), *px = shader(library, @"pixel", 4);
    ms->mesh_kind = tasked->mesh_kind = px->mesh_kind = 2; ms->mesh_notask = px->mesh_notask = 1; px->mesh_threads = 32;
    ms->mesh_obj = px->mesh_obj = [library newFunctionWithName:@"block"]; require(ms->mesh_obj != nil, "the block object function exists");
    ms->mesh_threads = tasked->mesh_threads = ts->mesh_threads = 32; ts->mesh_kind = 1; ts->mesh_payload = 4; fs->rf.color_mask = 1;
    prog_t programs[4] = {0};
    for (int kind = 0; kind < 4; kind++) {
        NvProg *p = [NvProg new]; p->sh[0] = kind == 3 ? vs : nil; p->sh[4] = kind == 3 ? nil : kind ? tasked : ms;
        p->sh[3] = kind == 1 || kind == 2 ? ts : nil; p->sh[1] = kind == 2 ? nil : fs;
        programs[kind].magic = MAGIC('P','R','O','G'); programs[kind].serial = nvn_new_serial(); serial_put(programs[kind].serial, p);
    }
    NVNdevice device; NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db);
    require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    NVNqueue queue; NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device);
    require(P(nvnQueueInitialize)(&queue, &qb), "queue initialized");
    uint8_t *memory = NULL; require(!posix_memalign((void **)&memory, getpagesize(), 65536), "command storage allocated"); memset(memory, 0, 65536);
    NVNmemoryPool pool; NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, 65536); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24);
    require(P(nvnMemoryPoolInitialize)(&pool, &pb), "command memory pool initialized");
    NVNcommandBuffer command; require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized");
    P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, 65536);
    typedef void (*DrawMeshTasks)(NVNcommandBuffer *, int, int);
    DrawMeshTasks draw = (DrawMeshTasks)nvn_lookup("nvnCommandBufferDrawMeshTasks"); require(draw != NULL, "native DrawMeshTasks entry point exists");
    enum { W = 8, H = 8 };
    tex_t color = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()}, depth = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()};
    color.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1};
    depth.d = color.d; depth.d.format = NVN_FORMAT_DEPTH32F;
    NVNdepthStencilState ds; P(nvnDepthStencilStateSetDefaults)(&ds); P(nvnDepthStencilStateSetDepthTestEnable)(&ds, 1);
    P(nvnDepthStencilStateSetDepthWriteEnable)(&ds, 1); P(nvnDepthStencilStateSetDepthFunc)(&ds, NVN_DEPTH_FUNC_LESS);
    const NVNtexture *colors[] = {(const NVNtexture *)&color}; const float viewport[] = {0, 0, W, H}, blue[] = {0, 0, 1, 1};
    for (int kind = 0; kind < 4; kind++) for (int pass = 0; pass < 2; pass++) {
        printf("%s, %s pipeline\n", kind == 0 ? "mesh only" : kind == 1 ? "task and mesh" : kind == 2 ? "depth only" : "ordinary vertex", pass ? "cached" : "first use");
        P(nvnCommandBufferBeginRecording)(&command);
        P(nvnCommandBufferSetRenderTargets)(&command, kind == 2 ? 0 : 1, colors, NULL, (const NVNtexture *)&depth, NULL);
        P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
        P(nvnCommandBufferSetDepthRange)(&command, 0, 1); P(nvnCommandBufferBindDepthStencilState)(&command, &ds);
        P(nvnCommandBufferClearDepthStencil)(&command, 1, 1, 0, 0);
        if (kind != 2) P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
        P(nvnCommandBufferBindProgram)(&command, (const NVNprogram *)&programs[kind], 0xc3);
        if (kind == 3) P(nvnCommandBufferDrawArrays)(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, pass * 3, 3);
        else draw(&command, pass + 1, 1);
        NVNcommandHandle handle = P(nvnCommandBufferEndRecording)(&command);
        P(nvnQueueSubmitCommands)(&queue, 1, &handle); P(nvnQueueFinish)(&queue);
        id<MTLBuffer> pixels = [g_dev newBufferWithLength:W * H * 8 options:MTLResourceStorageModeShared]; res_add(pixels);
        mtl_sub_begin();
        enc_b_copy_texture_buffer(blit(), tex_mtl(&depth), 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), pixels, 0, W * 4, W * H * 4);
        if (kind != 2) enc_b_copy_texture_buffer(blit(), tex_mtl(&color), 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(W,H,1), pixels, W * H * 4, W * 4, W * H * 4);
        require(mtl_wait(mtl_sub_commit(), 5000000000ull), "readback finishes"); CHECK(!mtl_last_commit_error(), "Metal reports no execution error");
        const float *z = pixels.contents; int good_depth = 1, good_color = 1;
        const uint8_t green[] = {0, 255, 0, 255};
        for (int i = 0; i < W * H; i++) {
            good_depth &= z[i] == (pass + 1) * 0.125f;
            if (kind != 2) good_color &= !memcmp((const uint8_t *)pixels.contents + W * H * 4 + i * 4, green, 4);
        }
        CHECK(good_depth, "this submission writes every expected depth pixel");
        if (kind != 2) CHECK(good_color, "this submission draws every expected color pixel");
        res_remove(pixels);
    }
    // More workgroups than a mesh grid holds: 3,072 from 5 (three object blocks, the last wrapping to pixels 0-4), drawn directly and from a {count, first} record in a memory pool
    {
        enum { GW = 64, GH = 48, N = GW * GH, FIRST = 5 };
        prog_t big = {0}; NvProg *bp = [NvProg new]; bp->sh[4] = px; bp->sh[1] = fs;
        big.magic = MAGIC('P','R','O','G'); big.serial = nvn_new_serial(); serial_put(big.serial, bp);
        uint32_t *rec = NULL; require(!posix_memalign((void **)&rec, getpagesize(), 65536), "record storage allocated"); memset(rec, 0, 65536); rec[0] = N; rec[1] = FIRST;
        NVNmemoryPool rp; NVNmemoryPoolBuilder rb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&rb); P(nvnMemoryPoolBuilderSetDevice)(&rb, &device);
        P(nvnMemoryPoolBuilderSetStorage)(&rb, rec, 65536); P(nvnMemoryPoolBuilderSetFlags)(&rb, 0x24);
        require(P(nvnMemoryPoolInitialize)(&rp, &rb), "record memory pool initialized");
        typedef void (*DrawMeshTasksIndirect)(NVNcommandBuffer *, NVNbufferAddress);
        DrawMeshTasksIndirect drawi = (DrawMeshTasksIndirect)nvn_lookup("nvnCommandBufferDrawMeshTasksIndirect"); require(drawi != NULL, "native DrawMeshTasksIndirect entry point exists");
        tex_t bc = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()}, bd = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial()};
        bc.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = GW, .height = GH, .depth = 1, .levels = 1, .samples = 1};
        bd.d = bc.d; bd.d.format = NVN_FORMAT_DEPTH32F;
        const NVNtexture *bcs[] = {(const NVNtexture *)&bc}; const float bview[] = {0, 0, GW, GH};
        for (int ind = 0; ind < 2; ind++) {
            printf("mesh only, %d workgroups from %d, %s\n", N, FIRST, ind ? "indirect" : "direct");
            P(nvnCommandBufferBeginRecording)(&command);
            P(nvnCommandBufferSetRenderTargets)(&command, 1, bcs, NULL, (const NVNtexture *)&bd, NULL);
            P(nvnCommandBufferSetViewports)(&command, 0, 1, bview); P(nvnCommandBufferSetScissor)(&command, 0, 0, GW, GH);
            P(nvnCommandBufferSetDepthRange)(&command, 0, 1); P(nvnCommandBufferBindDepthStencilState)(&command, &ds);
            P(nvnCommandBufferClearDepthStencil)(&command, 1, 1, 0, 0); P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
            P(nvnCommandBufferBindProgram)(&command, (const NVNprogram *)&big, 0xc3);
            if (ind) drawi(&command, P(nvnMemoryPoolGetBufferAddress)(&rp)); else draw(&command, FIRST, N);
            NVNcommandHandle handle = P(nvnCommandBufferEndRecording)(&command);
            P(nvnQueueSubmitCommands)(&queue, 1, &handle); P(nvnQueueFinish)(&queue);
            id<MTLBuffer> pixels = [g_dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared]; res_add(pixels);
            mtl_sub_begin();
            enc_b_copy_texture_buffer(blit(), tex_mtl(&bd), 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(GW,GH,1), pixels, 0, GW * 4, N * 4);
            require(mtl_wait(mtl_sub_commit(), 5000000000ull), "readback finishes"); CHECK(!mtl_last_commit_error(), "Metal reports no execution error");
            const float *z = pixels.contents; int good = 1, missing = 0;
            for (int p = 0; p < N; p++) { uint32_t wg = p < FIRST ? p + N : p; int ok = z[p] == (float)(wg + 1) / 8192.0f; good &= ok; missing += z[p] == 1.0f; }
            if (!good) { fprintf(stderr, "%d of %d pixels not drawn\n", missing, N);
                for (int p = 0, shown = 0; p < N && shown < 6; p++) { uint32_t wg = p < FIRST ? p + N : p; if (z[p] != (float)(wg + 1) / 8192.0f) { shown++; fprintf(stderr, "  pixel %d (%d,%d): %.7f = number %.1f, expected %u\n", p, p % GW, p / GW, z[p], z[p] * 8192.0f - 1, wg); } } }
            CHECK(good, ind ? "an indirect draw past the mesh grid's limit draws every workgroup at its number" : "a direct draw past the mesh grid's limit draws every workgroup at its number");
            res_remove(pixels);
        }
        P(nvnQueueFinish)(&queue); mtl_drain(); mtl_tex_release(&bc); mtl_tex_release(&bd); P(nvnMemoryPoolFinalize)(&rp); free(rec);
    }
    P(nvnQueueFinish)(&queue); mtl_drain();
    P(nvnCommandBufferFinalize)(&command); mtl_tex_release(&color); mtl_tex_release(&depth);
    P(nvnMemoryPoolFinalize)(&pool); free(memory); P(nvnQueueFinalize)(&queue); P(nvnDeviceFinalize)(&device);
    printf("mesh first use: %d checks, %d failed\n", checks, failures); return failures != 0;
} }
