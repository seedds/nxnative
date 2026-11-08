// DM_NVN_DRAW_CLAMP=1 (nvn_mtl.m dcl_prepass / dcl_get, ixc.metal dcl_clamp): plain DrawArraysIndirect / DrawElementsIndirect records bounded on the GPU.
// Part 1, through the replay: a direct draw opens the render pass, then an indirect draw in the same pass. A record within the count x instances cap draws; one over it
// (DM_NVN_DRAW_CLAMP_MAX=64 here, 3 x 100 instances) draws nothing; the pass is not split for the clamp. Without the clamp the over-cap record draws (its pixel is lit).
// Part 2: the GPU's count of the records drawn as nothing; multi-draw elements and the draws ixc_run converts are not taken.
// Every record drawn here is small, so the test cannot hang the GPU with or without the clamp.
#import <Foundation/Foundation.h>
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) {
    (void)p;
    return NULL;
}
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

enum { PIXELS = 8, POOL_BYTES = 64 * 1024 };
static _Alignas(16) uint8_t storage[4096];
static int checks, failed;
#define CHECK(c, label)                                                                                                                                                   \
    do {                                                                                                                                                                  \
        checks++;                                                                                                                                                         \
        if (!(c)) {                                                                                                                                                       \
            failed++;                                                                                                                                                     \
            fprintf(stderr, "FAIL: %s\n", label);                                                                                                                         \
        }                                                                                                                                                                 \
    } while (0)

// One submission: a direct draw of vertices 0-2 (the left of the target), then an indirect draw of the record at rec (vertices 3-5, the right). Returns the right and left
// probe pixels' red, and the render passes it took
static void run(prog_t *pg, tex_t *rt, id<MTLTexture> out, uint8_t *pool, const uint32_t *words, int indexed, unsigned *left, unsigned *right, uint64_t *passes) {
    uint64_t base = (uint64_t)(uintptr_t)pool, rec = base + 256, ibuf = base + 64;
    uint32_t *indices = (uint32_t *)(pool + 64);
    indices[0] = 3; indices[1] = 4; indices[2] = 5;
    memcpy(pool + 256, words, 20);
    NVNcommandBuffer cb = {0};
    CB(&cb)->cur = storage; CB(&cb)->end = storage + sizeof storage;
    h_nvnCommandBufferBeginRecording(&cb);
    h_nvnCommandBufferDrawArrays(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    if (indexed) h_nvnCommandBufferDrawElementsIndirect(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, ibuf, rec);
    else h_nvnCommandBufferDrawArraysIndirect(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, rec);
    uint64_t handle = h_nvnCommandBufferEndRecording(&cb);
    uint8_t pixels[PIXELS * PIXELS * 4] = {0};
    [out replaceRegion:MTLRegionMake2D(0, 0, PIXELS, PIXELS) mipmapLevel:0 withBytes:pixels bytesPerRow:PIXELS * 4];
    state_reset();
    memset(&R, 0, sizeof R);
    mtl_sub_begin();
    mtl_sub_index(&handle, 1);
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = rt;
    S.prog = pg; S.ds.dt = S.ds.dw = S.ds.st = 0; S.poly.cull = 0; S.color.blend_en = 0; S.chmask.mask = 0xffff;
    uint64_t p0 = g_stat_pass;
    mtl_exec((const uint8_t *)(uintptr_t)handle);
    *passes = g_stat_pass - p0;
    uint64_t fence = mtl_sub_commit();
    int done = mtl_wait(fence, 10000000000ull) && !mtl_last_commit_error();
    CHECK(done, "the submission completes with no GPU error");
    [out getBytes:pixels bytesPerRow:PIXELS * 4 fromRegion:MTLRegionMake2D(0, 0, PIXELS, PIXELS) mipmapLevel:0];
    *left = pixels[((PIXELS / 2) * PIXELS + 1) * 4];
    *right = pixels[((PIXELS / 2) * PIXELS + PIXELS - 2) * 4];
}

int main(void) {
    @autoreleasepool {
        setenv("DM_NVN_DRAW_CLAMP", "1", 1);
        setenv("DM_NVN_DRAW_CLAMP_MAX", "64", 1);
        mtl_init();
        NSError *err = nil;
        // Vertices 0-2: a triangle over the left probe pixel; 3-5: one over the right probe pixel
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                                          "vertex float4 v(uint i [[vertex_id]]){const float2 p[6]={float2(-1,-1),float2(-0.2,-1),float2(-1,3),"
                                                          "float2(0.2,-1),float2(3,-1),float2(0.2,3)};return float4(p[i%6],0,1);}\n"
                                                          "fragment float4 f(){return float4(1,0,0,1);}"
                                                 options:nil
                                                   error:&err];
        if (!lib) {
            fprintf(stderr, "%s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        uint8_t *pool = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (pool == MAP_FAILED) return 2;
        NVNmemoryPool physical;
        pool_builder_t builder = {.mem = pool, .size = POOL_BYTES, .flags = 0x24};
        if (!h_nvnMemoryPoolInitialize(&physical, (const NVNmemoryPoolBuilder *)&builder)) return 2;
        NvProg *pr = [NvProg new];
        pr->sh[0] = [NvShader new];
        pr->sh[1] = [NvShader new];
        pr->sh[0]->fn = [lib newFunctionWithName:@"v"];
        pr->sh[1]->fn = [lib newFunctionWithName:@"f"];
        pr->sh[1]->stage = 1;
        pr->sh[1]->rf.color_known = 1;
        pr->sh[1]->rf.color_mask = 1;
        prog_t pg = {.serial = nvn_new_serial()};
        serial_put(pg.serial, pr);
        tex_t rt = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
        rt.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = PIXELS, .height = PIXELS, .depth = 1, .levels = 1, .samples = 1};
        MTLTextureDescriptor *td = tex_desc(&rt, 0);
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:td];
        res_add(out);
        mtl_tex_init(&rt);
        serial_put(rt.serial, out);

        // ---- Part 1: through the replay ----
        typedef struct { uint32_t words[5]; int indexed; unsigned want; const char *what; } rcase;
        const rcase rc[] = {
            {{3, 1, 3, 0, 0}, 0, 255, "arrays: a record within the cap draws"},
            {{3, 100, 3, 0, 0}, 0, 0, "arrays: count x instances over DM_NVN_DRAW_CLAMP_MAX draws nothing"},
            {{64, 1, 3, 0, 0}, 0, 255, "arrays: a record of exactly DM_NVN_DRAW_CLAMP_MAX vertices draws"},
            {{65, 1, 3, 0, 0}, 0, 0, "arrays: one vertex over DM_NVN_DRAW_CLAMP_MAX draws nothing"},
            {{3, 1, 0, 0, 0}, 1, 255, "indexed: a record within the cap draws"},
            {{3, 100, 0, 0, 0}, 1, 0, "indexed: count x instances over DM_NVN_DRAW_CLAMP_MAX draws nothing"},
        };
        {   // a first submission, not checked: the pool's pages are tracked from their first use on, so a record written before it is not seen (t_large_mdi's count-0 pass does the same)
            unsigned left = 0, right = 0; uint64_t passes = 0; run(&pg, &rt, out, pool, rc[0].words, 0, &left, &right, &passes); }
        for (size_t i = 0; i < sizeof rc / sizeof *rc; i++) {
            unsigned left = 0, right = 0; uint64_t passes = 0;
            run(&pg, &rt, out, pool, rc[i].words, rc[i].indexed, &left, &right, &passes);
            char label[200];
            snprintf(label, sizeof label, "%s (right pixel %u, want %u)", rc[i].what, right, rc[i].want);
            CHECK(right == rc[i].want, label);
            snprintf(label, sizeof label, "%s: the direct draw before it draws (left pixel %u)", rc[i].what, left);
            CHECK(left == 255, label);
            snprintf(label, sizeof label, "%s: one render pass, not split for the clamp (%llu)", rc[i].what, (unsigned long long)passes);
            CHECK(passes == 1, label);
        }

        // ---- Part 2: what the clamp takes, and the GPU's report ----
        CHECK(g_dcl_inpass == 0, "no draw met an open pass without its copy");
        dcl_report();
        CHECK(g_dcl_seen == 3, "the GPU counted the three over-cap records");
        { p_draw dw = {.prim = 4, .ind = (uint64_t)(uintptr_t)pool, .mdi_cnt = (uint64_t)(uintptr_t)pool + 16}; CHECK(!dcl_wanted(&dw, NULL), "a counted multi-draw's element is not clamped"); }
        { p_draw dw = {.prim = 7, .ind = (uint64_t)(uintptr_t)pool}; CHECK(!dcl_wanted(&dw, NULL), "an indirect quad draw keeps its conversion (ixc_run)"); }
        { p_draw_idx di = {.prim = 4, .itype = 0, .ind = (uint64_t)(uintptr_t)pool}; CHECK(!dcl_wanted(NULL, &di), "an indirect draw of 8-bit indices keeps its conversion (ixc_run)"); }

        mtl_tex_release(&rt);
        h_nvnMemoryPoolFinalize(&physical);
        serial_del(pg.serial);
        mtl_drain();
        munmap(pool, POOL_BYTES);
        printf("t_draw_clamp: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
}
