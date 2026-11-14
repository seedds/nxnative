// Texture pool tables of one type holding textures of another (pool_fit, pool_bridge_pair). To Maxwell 2D, 2D-array, cube and cube-array textures are all layers, and a shader may
// declare a pool's element type differently from the texture the game registered: PRAGMATA's Character_CabinDisplay material samples its five sprite slots as texture2d_array
// through the pool, and the sprites are 2D textures (Cabin's monitor face, ps ecd01ea9b68f5390). A table held textures of its own type only, so the face read zero. Now a table of
// another layered type holds a view that covers what Metal can serve, for an ID a shader read through a table of that type (the read bitmap's section of the type, pool_seen_t):
//   2D in a 2D-array table: one layer; a 2D array in a 2D table: layer 0; a cube (held as a 6-layer 2D array) in a cube table: a cube view, in a 2D-array table: its 6 faces;
//   a 6-layer 2D array in a cube table: a cube; a cube array (12 faces) in a 2D-array or cube-array table.
// What no view serves holds the dummy (zero), as for slot binds (tex_fit): a 2D texture or a 3D texture in a cube table, a non-square array in a cube table. An ID read only through
// its own type's table is not bridged elsewhere. A shader reads each bridged texel through the table. Fails without pool_fit: entries are the dummy and the reads are zero.
// The views are one per table type: a table stands while nothing is registered, so its view of an ID stays when a table of a third type takes the ID (fails with one view per ID)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static uint64_t entry(id<MTLBuffer> b, int i) { return ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }
static uint64_t dummy(id<MTLBuffer> b) { return entry(b, (int)*(const uint32_t *)b.contents); }
static id<MTLBuffer> table(int dtt) {   // the float table of type dtt at base 0, in a submission of its own
    mtl_sub_begin();
    __unsafe_unretained id<MTLBuffer> b = nil;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, dtt, NVN_DPF_FLOAT, 0, &b); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> held = b;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    CHECK(!mtl_last_commit_error(), "the submission has no GPU error");
    return held;
}
// A shader's read of ID i through a table of type dtt: the bit in that table's section of the bitmap (the address a table's head holds)
static void read_mark(int i, int dtt) { pool_seen_gpu(); atomic_fetch_or((_Atomic uint32_t *)g_seen_buf.contents + seen_sec(dtt) * POOL_SEEN_WORDS + i / 32, 1u << (i % 32)); }
static unsigned layers_of(const tex_t *t) { return t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? t->d.depth : t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : 1; }
static void fill(const tex_t *t, uint8_t v) {   // layer y holds v + 8 * y
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned z = t->d.target == NVN_TEXTURE_TARGET_3D ? t->d.depth : 1;
    size_t pitch = (size_t)((t->d.width + bw - 1) / bw) * bpb, rows = (t->d.height + bh - 1) / bh; uint8_t *lin = malloc(pitch * rows * z);
    for (unsigned y = 0; y < layers_of(t); y++) { memset(lin, v + 8 * (int)y, pitch * rows * z); if (nvn_swizzle(&t->d, 0, y, lin, pitch, t->mem)) abort(); }
    free(lin);
}
// What a shader reads through element i of a table declared as an array of want (texture2d_array: layer, texture2d, texturecube: face by direction), as a pool shader would
static int gpu_read(id<MTLBuffer> tab, int want, int i, unsigned layer, float out[4]) {
    static id<MTLComputePipelineState> ps[4];
    int k = want == MTLTextureType2DArray ? 0 : want == MTLTextureType2D ? 1 : want == MTLTextureTypeCube ? 2 : 3;
    if (!ps[k]) {
        static NSString *ty[4] = {@"texture2d_array<float>", @"texture2d<float>", @"texturecube<float>", @"texturecube_array<float>"};
        static NSString *rd[4] = {@"t.read(uint2(0, 0), l, 0u)", @"t.read(uint2(0, 0), 0u)", @"t.read(uint2(0, 0), l, 0u)", @"t.read(uint2(0, 0), l % 6u, l / 6u, 0u)"};
        NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct P { uint n; ulong seen; array<%@, 1> e; };\n"
            "kernel void k(constant P& p [[buffer(0)]], constant uint2& a [[buffer(1)]], device float4 *o [[buffer(2)]]) { auto t = (&p.e[0])[min(a.x, p.n)]; uint l = a.y; o[0] = %@; }\n", ty[k], rd[k]];
        NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) { fprintf(stderr, "library: %s\n", err.description.UTF8String); return 0; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"k"]; if (!fn) return 0;
        ps[k] = [g_dev newComputePipelineStateWithFunction:fn error:&err];
        if (!ps[k]) return 0;
    }
    id<MTLBuffer> o = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(o.contents, 0, 16);
    uint32_t a[2] = {(uint32_t)i, layer};
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps[k]]; [en setBuffer:tab offset:0 atIndex:0]; [en setBytes:a length:8 atIndex:1]; [en setBuffer:o offset:0 atIndex:2];
    if (i > 0 && i < g_ntreg) { void *vs[5] = {g_treg[i].palt[0], g_treg[i].palt[1], g_treg[i].palt[2], g_treg[i].palt[3], g_treg[i].view}; for (int j = 0; j < 5; j++) if (vs[j]) [en useResource:(__bridge id<MTLTexture>)vs[j] usage:MTLResourceUsageRead]; }
    [en dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    memcpy(out, o.contents, 16);
    return cb.status == MTLCommandBufferStatusCompleted;
}
static int reads(id<MTLBuffer> tab, int want, int i, unsigned layer, uint8_t v) {   // the texel read is v (RGBA8 unorm)
    float px[4] = {0}; return gpu_read(tab, want, i, layer, px) && fabsf(px[0] - v / 255.0f) < 1e-3f && fabsf(px[3] - v / 255.0f) < 1e-3f;
}
static id<MTLTexture> palt(int i, int tt) { int k = pool_lk(tt); return k < 0 ? nil : (__bridge id<MTLTexture>)g_treg[i].palt[k]; }   // the view a table of type tt holds for ID i
static uint64_t prid(int i, int tt) { int k = pool_lk(tt); return k < 0 ? 0 : g_treg[i].palt_rid[k]; }
int main(void) { @autoreleasepool {
    mtl_init();
    enum { SLOT = 1 << 17, N = 9, LEN = N * SLOT };
    uint8_t *mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    pool_t p = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, LEN);
    // 0: 2D (the sprite); 1: 2D no shader reads as an array; 2: 2D array of 2; 3: 3D; 4: cube; 5: 2D array of 6 (square); 6: cube array of 2 cubes; 7: 2D array of 6, 16x8; 8: 2D for a cube table
    tex_t t[N] = {0};
    for (int n = 0; n < N; n++) {
        t[n].magic = MAGIC('T', 'E', 'X', 0); t[n].has_layout = 1; t[n].serial = 990500 + n; t[n].pool = &p; t[n].off = n * SLOT; t[n].mem = mem + t[n].off;
        t[n].d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1};
        if (n == 2) { t[n].d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t[n].d.depth = 2; }
        if (n == 3) { t[n].d.target = NVN_TEXTURE_TARGET_3D; t[n].d.depth = 2; }
        if (n == 4) { t[n].d.target = NVN_TEXTURE_TARGET_CUBEMAP; t[n].d.depth = 6; }
        if (n == 5) { t[n].d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t[n].d.depth = 6; }
        if (n == 6) { t[n].d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; t[n].d.depth = 12; }
        if (n == 7) { t[n].d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t[n].d.depth = 6; t[n].d.height = 8; }
        nvn_tex_layout(&t[n].d, &t[n].L); if (t[n].L.size > SLOT) abort();
        fill(&t[n], (uint8_t)(0x20 + 0x10 * n)); mtl_tex_init(&t[n]);
    }
    for (int n = 0; n < N; n++) mtl_reg_tex(10 + n, &t[n], NULL);
    const int A = MTLTextureType2DArray, D2 = MTLTextureType2D, C = MTLTextureTypeCube, CA = MTLTextureTypeCubeArray;
    // Reads: 10 as 2D and as a 2D array; 11 as 2D only; 12 (2D array) as 2D; 13 (3D), 14 (cube), 15 (6 layers), 16 (cube array), 17 (16x8 x6), 18 (2D) as cubes; 14 and 16 as 2D arrays; 14 as 2D; 16 as a cube array
    read_mark(10, D2); read_mark(10, A); read_mark(11, D2); read_mark(12, D2); read_mark(13, C); read_mark(14, C); read_mark(14, A); read_mark(14, D2); read_mark(16, C); read_mark(15, C); read_mark(16, A); read_mark(16, CA); read_mark(17, C); read_mark(18, C);
    id<MTLBuffer> d2 = table(D2);   // the read IDs' textures are made (pool_seen_scan)
    CHECK(g_treg[10].view && entry(d2, 10) == g_treg[10].view_rid, "the 2D table holds the 2D texture itself");
    CHECK(palt(12, D2) && palt(12, D2).textureType == MTLTextureType2D && entry(d2, 12) == prid(12, D2) && reads(d2, D2, 12, 0, 0x40), "a 2D array read as 2D: its layer 0");

    id<MTLBuffer> a = table(A);
    CHECK(palt(10, A) && palt(10, A).textureType == MTLTextureType2DArray && palt(10, A).arrayLength == 1 && entry(a, 10) == prid(10, A), "a 2D texture read as a 2D array: a one-layer view, not the dummy");
    CHECK(reads(a, A, 10, 0, 0x20), "a shader reads its texel through the 2D-array handle");
    CHECK(entry(a, 11) == dummy(a) && !palt(11, A), "a 2D texture read only as 2D: no view in the 2D-array table");
    CHECK(entry(a, 12) == g_treg[12].view_rid, "a 2D array is the 2D-array table's own type");
    CHECK(entry(a, 14) != dummy(a) && reads(a, A, 14, 3, 0x60 + 24), "a cube read as a 2D array: its faces as layers (face 3)");
    CHECK(entry(a, 16) != dummy(a) && reads(a, A, 16, 7, 0x80 + 56), "a cube array read as a 2D array: its faces as layers (layer 7)");

    id<MTLBuffer> c = table(C);
    CHECK(entry(c, 14) != dummy(c) && reads(c, C, 14, 2, 0x60 + 16), "a cube in a cube table: a cube (face 2)");
    CHECK(entry(c, 15) != dummy(c) && reads(c, C, 15, 5, 0x70 + 40), "a square 6-layer 2D array read as a cube: a cube view of its layers");
    CHECK(entry(c, 13) == dummy(c), "a 3D texture read as a cube: the dummy (no view serves it)");
    CHECK(entry(c, 18) == dummy(c), "a 2D texture read as a cube: the dummy (no view serves it)");
    CHECK(entry(c, 17) == dummy(c), "a non-square 6-layer array read as a cube: the dummy");
    CHECK(g_pool_tlog_n == 3 && (g_pool_tlog[0] >> 13 & 1) && (g_pool_tlog[0] >> 17 & 1) && (g_pool_tlog[0] >> 18 & 1), "each of the three is reported once ([pool-type] left out)");
    // A table stands while no registration or storage changes (pool_gpu), so the view it holds of an ID stays while a table of a third type holds another view of it
    CHECK(palt(14, D2) && palt(14, C) && entry(d2, 14) == prid(14, D2) && reads(d2, D2, 14, 0, 0x60), "a cube read as 2D and as a cube: the 2D table's view of it stays");
    id<MTLBuffer> ca = table(CA);
    CHECK(entry(ca, 16) != dummy(ca) && reads(ca, CA, 16, 9, 0x80 + 72), "a cube array in a cube-array table: a cube array (cube 1, face 3)");
    CHECK(palt(16, C) && palt(16, CA) && entry(c, 16) == prid(16, C) && reads(c, C, 16, 2, 0x80 + 16), "a cube array read as a cube and as a cube array: the cube table's view of it stays");

    // An ID first read through another type later: the existing table takes it in place at the next scan
    read_mark(11, A);
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); pool_seen_scan(); os_unfair_lock_unlock(&g_rmu); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    CHECK(palt(11, A) && entry(a, 11) == prid(11, A) && reads(a, A, 11, 0, 0x30), "an ID newly read through the 2D-array table: its view goes into that table in place");

    // The view is cached: asked again for the same texture, pool_fit makes no new one
    os_unfair_lock_lock(&g_rmu);
    void *tv = NULL; tex_fast_rid(10, &tv); id<MTLTexture> cur = (__bridge id<MTLTexture>)tv;
    uint64_t r1 = pool_fit(10, cur, A); void *v1 = g_treg[10].palt[pool_lk(A)]; uint64_t r2 = pool_fit(10, cur, A);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(r1 && r1 == r2 && g_treg[10].palt[pool_lk(A)] == v1, "the view is cached: no new view for the same texels");
    // An ID given a texture of another type: the marks of the types shaders read the old one through go, the read mark stays (Resident Evil 4 puts its 1x1 default 2D texture into
    // the IDs of cubes it lets go). Kept, the next cube table reported the 2D texture as read through it, with no shader reading it. One of the same type keeps them (the bridges stand)
    mtl_reg_tex(12, &t[5], NULL);
    CHECK(pool_seen_t(12, D2), "a 2D array replaced by a 2D array: its 2D read mark stays");
    unsigned logged = g_pool_tlog_n;
    mtl_reg_tex(14, &t[8], NULL);
    CHECK(!pool_seen_t(14, C) && !pool_seen_t(14, A) && !pool_seen_t(14, D2) && pool_seen(14), "a cube replaced by a 2D texture: its typed marks go, its read mark stays");
    id<MTLBuffer> c2 = table(C);
    CHECK(entry(c2, 14) == dummy(c2) && g_pool_tlog_n == logged, "the cube table holds the dummy for it and reports nothing: no shader read the 2D texture as a cube");
    read_mark(14, C);
    id<MTLBuffer> c3 = table(C);
    CHECK(entry(c3, 14) == dummy(c3) && g_pool_tlog_n == logged + 1 && (g_pool_tlog[0] >> 14 & 1), "a shader that does read it as a cube marks it again, and that is reported");
    // Emptying the ID lets the view go, and its typed marks
    mtl_reg_tex(10, NULL, NULL);
    CHECK(!palt(10, A) && !prid(10, A) && !pool_seen_t(10, A) && !pool_seen_t(10, D2), "an emptied ID drops its view and its read marks");
    id<MTLBuffer> a3 = table(A);
    CHECK(entry(a3, 10) == dummy(a3), "and its table entry is the dummy");
    for (int n = 0; n < N; n++) mtl_reg_tex(10 + n, NULL, NULL);
    for (int n = 0; n < N; n++) mtl_tex_release(&t[n]);
    mtl_pool_del(mem); mtl_drain(); munmap(mem, LEN);
    fprintf(stderr, "t_pool_typebridge: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
} }
