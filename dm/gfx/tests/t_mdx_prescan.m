// Indirect-count multi-draws after an ordinary draw in the same render pass (nvn_mtl.m mdx_prescan): once the game has drawn one, the pass the ordinary draw opens
// copies their records first and is not split for them. An ORDER_INDIRECT_DATA barrier after a draw with shader writes ends those copies (g_mdx_wr): the multi-draw
// after it copies again, which splits the pass; the same barrier with no shader write since the copies keeps them
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

enum { MAXD = 4, STRIDE = 16, PIXELS = 8 };
static _Alignas(16) uint8_t storage[16 * 1024];

// The pool: the count at +0 (written on the GPU from +16), the multi-draw's {count, instances, first, base instance} records at +256
static int run(const char *name, uint8_t *pool, prog_t *green, prog_t *red, int barrier, int writer, uint32_t live, uint64_t want_splits, int want_pre) {
    uint64_t base = (uint64_t)(uintptr_t)pool;
    uint32_t *rec = (uint32_t *)(pool + 256);
    memset(rec, 0, MAXD * STRIDE);
    rec[0] = 3; rec[1] = 1;   // the first draw covers the target; the others draw nothing
    NvProg *gp = serial_get(green->serial);
    gp->c_sw = writer ? 2 : 1;   // prog_swrites: the ordinary draw's vertex stage writes memory (only the host's bookkeeping; the shader writes nothing)

    NVNcommandBuffer cb = {0};
    CB(&cb)->cur = storage; CB(&cb)->end = storage + sizeof storage;
    h_nvnCommandBufferBeginRecording(&cb);
    h_nvnCommandBufferCopyBufferToBuffer(&cb, base + 16, base, sizeof(uint32_t), 0);
    h_nvnCommandBufferBindProgram(&cb, (const NVNprogram *)green, 3);
    h_nvnCommandBufferDrawArrays(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    if (barrier) h_nvnCommandBufferBarrier(&cb, NVN_BARRIER_ORDER_INDIRECT_DATA_BIT);
    h_nvnCommandBufferBindProgram(&cb, (const NVNprogram *)red, 3);
    h_nvnCommandBufferMultiDrawArraysIndirectCount(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, base + 256, base, MAXD, STRIDE);
    uint64_t handle = (uintptr_t)nvn_rec_linear(h_nvnCommandBufferEndRecording(&cb), NULL);

    tex_t rt = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    rt.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = PIXELS, .height = PIXELS, .depth = 1, .levels = 1, .samples = 1};
    MTLTextureDescriptor *td = tex_desc(&rt, 0);
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> out = [g_dev newTextureWithDescriptor:td];
    res_add(out);
    mtl_tex_init(&rt);
    serial_put(rt.serial, out);
    uint8_t pixels[PIXELS * PIXELS * 4] = {0};
    [out replaceRegion:MTLRegionMake2D(0, 0, PIXELS, PIXELS) mipmapLevel:0 withBytes:pixels bytesPerRow:PIXELS * 4];

    *(uint32_t *)(pool + 16) = live;
    *(uint32_t *)pool = ~live;
    state_reset();
    memset(&R, 0, sizeof R);
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &rt;
    S.ds.dt = S.ds.dw = S.ds.st = 0; S.poly.cull = 0; S.color.blend_en = 0; S.chmask.mask = 0xffff;
    uint64_t splits = g_mdx_splits, pre = g_mdx_pre, waits = g_mdi_waits;
    mtl_sub_begin();
    mtl_sub_index(&handle, 1);
    mtl_exec((const uint8_t *)(uintptr_t)handle);
    int done = mtl_wait(mtl_sub_commit(), 10000000000ull) && !mtl_last_commit_error();
    free((void *)(uintptr_t)handle);   // the linear copy (nvn_rec_linear); the replay is done with it
    [out getBytes:pixels bytesPerRow:PIXELS * 4 fromRegion:MTLRegionMake2D(0, 0, PIXELS, PIXELS) mipmapLevel:0];
    const uint8_t *c = &pixels[((PIXELS / 2) * PIXELS + PIXELS / 2) * 4];
    int red_px = c[0] == 255 && c[1] == 0, green_px = c[0] == 0 && c[1] == 255;
    int good = done && (live ? red_px : green_px) && g_mdx_splits - splits == want_splits && (int)(g_mdx_pre - pre) >= want_pre && g_mdi_waits == waits;
    printf("%-44s pixel=%u,%u splits=%llu (want %llu) prescans=%llu waits=%llu %s\n", name, c[0], c[1], (unsigned long long)(g_mdx_splits - splits),
           (unsigned long long)want_splits, (unsigned long long)(g_mdx_pre - pre), (unsigned long long)(g_mdi_waits - waits), good ? "ok" : "FAILED");
    mtl_tex_release(&rt);
    return good;
}
int main(void) {
    @autoreleasepool {
        mtl_init();
        NSError *err = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                                          "vertex float4 v(uint i [[vertex_id]]){const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i%3],0,1);}\n"
                                                          "fragment float4 g(){return float4(0,1,0,1);}\nfragment float4 r(){return float4(1,0,0,1);}"
                                                 options:nil
                                                   error:&err];
        if (!lib) {
            fprintf(stderr, "%s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        size_t bytes = 64 * 1024;
        uint8_t *pool = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (pool == MAP_FAILED) return 2;
        NVNmemoryPool physical;
        pool_builder_t builder = {.mem = pool, .size = bytes, .flags = 0x24};
        if (!h_nvnMemoryPoolInitialize(&physical, (const NVNmemoryPoolBuilder *)&builder)) return 2;
        prog_t pg[2];
        const char *frag[2] = {"g", "r"};
        for (int k = 0; k < 2; k++) {
            NvProg *pr = [NvProg new];
            pr->sh[0] = [NvShader new]; pr->sh[1] = [NvShader new];
            pr->sh[0]->fn = [lib newFunctionWithName:@"v"];
            pr->sh[1]->fn = [lib newFunctionWithName:@(frag[k])];
            pr->sh[1]->stage = 1; pr->sh[1]->rf.color_known = 1; pr->sh[1]->rf.color_mask = 1;
            pg[k] = (prog_t){.serial = nvn_new_serial()};
            serial_put(pg[k].serial, pr);
        }
        int failures = 0;
        // No multi-draw met yet: no scan ahead, and the multi-draw's copy splits the pass the ordinary draw opened
        failures += !run("first multi-draw: split", pool, &pg[0], &pg[1], 0, 0, 1, 1, 0);
        failures += !run("pass opened by an ordinary draw: no split", pool, &pg[0], &pg[1], 0, 0, 1, 0, 1);
        failures += !run("count 0: the ordinary draw only", pool, &pg[0], &pg[1], 0, 0, 0, 0, 1);
        failures += !run("indirect barrier, no shader write: no split", pool, &pg[0], &pg[1], 1, 0, 1, 0, 1);
        failures += !run("indirect barrier after a shader write: copy", pool, &pg[0], &pg[1], 1, 1, 1, 1, 1);
        failures += !run("shader write, no barrier: no split", pool, &pg[0], &pg[1], 0, 1, 1, 0, 1);
        for (int k = 0; k < 2; k++) serial_del(pg[k].serial);
        h_nvnMemoryPoolFinalize(&physical);
        mtl_drain();
        munmap(pool, bytes);
        return failures != 0;
    }
}
