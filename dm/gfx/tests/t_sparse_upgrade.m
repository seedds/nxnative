// A sparse texture (flag SPARSE) in a sparse virtual pool resolves to a Metal placement sparse texture (sp_make) with the format-view and, for an uncompressed format, the
// render-target usage from the start, so no usage upgrade makes it whole; and the one upgrade left (image writes) drops its tile record and keeps the texture whole from then on,
// instead of leaving the record on a texture that is not sparse (the next sync called updateTextureMappings on it). Needs the Metal 4 host (macOS 26); without placement sparse
// textures the resolve takes the whole-texture path and the checks that need the sparse one are skipped
#define DM_NVN_SPARSE_HEAP_MIB 64u
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

// GPU fence completion can precede the feedback handler's residency removal.
// It may already own the deferred item, so another defer_drain cannot join it.
static int await_residency_removal(id<MTLHeap> heap) {
    uint64_t end = bp_now() + 1000000000ull;
    do {
        defer_drain();
        pthread_mutex_lock(&g_res_mu); int present = [g_res_objects containsObject:heap]; pthread_mutex_unlock(&g_res_mu);
        if (!present) return 1;
        usleep(1000);
    } while (bp_now() < end);
    return 0;
}

// Read actual GPU texels, not the CPU-side guest pool, so a skipped sparse upload cannot make this pass.
static void check_tile(id<MTLTexture> mt, tex_t *t) {
    id<MTLBuffer> read = [g_dev newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
    CHECK(read != nil, "GPU tile readback allocation succeeds"); if (!read) return;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:mt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(128, 128, 1)
             toBuffer:read destinationOffset:0 destinationBytesPerRow:512 destinationBytesPerImage:0x10000];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint8_t tile[0x10000], expected[0x10000]; unsigned th, td;
    nvn_sparse_copy(t->pool, tile, (uint64_t)(uintptr_t)t->mem, sizeof tile);
    nvn_tex_level_tiles(&t->d, 0, &th, &td);
    nvn_deswizzle_tile(th, 8, 512, 128, tile, expected, 512);
    CHECK(!cb.error && !memcmp(read.contents, expected, sizeof expected), "GPU tile equals current guest pages, including unmapped zero bytes");
}

static void check_rendered_tile(id<MTLTexture> mt) {
    id<MTLBuffer> read = [g_dev newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
    CHECK(read != nil, "rendered tile readback allocation succeeds"); if (!read) return;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:mt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(128, 128, 1)
             toBuffer:read destinationOffset:0 destinationBytesPerRow:512 destinationBytesPerImage:0x10000];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *bytes = read.contents; int bad = 0;
    for (int i = 0; i < 0x10000; i += 4) bad |= bytes[i] != 255 || bytes[i + 1] != 0 || bytes[i + 2] != 0 || bytes[i + 3] != 255;
    CHECK(!cb.error && !bad, "GPU-rendered tile survives unrelated mapping changes");
}

static void check_compressed_tile(id<MTLTexture> mt, tex_t *t, unsigned x) {
    id<MTLBuffer> read = [g_dev newBufferWithLength:0x10000 options:MTLResourceStorageModeShared];
    CHECK(read != nil, "compressed tile readback allocation succeeds"); if (!read) return;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:mt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x * 256, 0, 0) sourceSize:MTLSizeMake(256, 256, 1)
             toBuffer:read destinationOffset:0 destinationBytesPerRow:1024 destinationBytesPerImage:0x10000];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint8_t tile[0x10000], expected[0x10000]; unsigned th, td;
    nvn_sparse_copy(t->pool, tile, (uint64_t)(uintptr_t)t->mem + x * 0x10000, sizeof tile);
    nvn_tex_level_tiles(&t->d, 0, &th, &td);
    nvn_deswizzle_tile(th, 16, 1024, 64, tile, expected, 1024);
    CHECK(!cb.error && !memcmp(read.contents, expected, sizeof expected), "GPU compressed tile matches physical pages including partial holes");
}
// Successive GPU writes through one sparse address must be visible when reading
// another address of the same heap tile, without CPU waits between the passes.
static void check_shared_gpu_updates(id<MTLTexture> writer, id<MTLTexture> reader) {
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void sample_alias(texture2d<float,access::read> t [[texture(0)]], device uint4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { o[p.y*256+p.x]=uint4(round(t.read(p)*255.0f)); }\n" options:nil error:&error];
    id<MTLComputePipelineState> ps = lib ? [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sample_alias"] error:&error] : nil;
    CHECK(ps != nil, "sparse alias texture reader compiles"); if (!ps) return;
    enum { N = 4, TILE = 65536, OUT = 256 * 256 * 16 };
    id<MTLBuffer> src = [g_dev newBufferWithLength:N * TILE options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:N * OUT options:MTLResourceStorageModeShared];
    CHECK(src && out, "sparse alias update buffers allocate"); if (!src || !out) return;
    // ASTC LDR void-extent blocks, with a different constant colour per pass.
    for (int n = 0; n < N; n++) for (int i = 0; i < TILE; i += 16) {
        uint16_t *p = (uint16_t *)((uint8_t *)src.contents + n * TILE + i);
        p[0] = 0xfdfc; p[1] = p[2] = p[3] = 0xffff;
        for (int c = 0; c < 4; c++) p[4 + c] = (c == n || c == 3 || n == 3) ? 0xffff : 0;
    }
    res_add(src); res_add(out);
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 1;
    id<MTL4ArgumentTable> at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);
    CHECK(at != nil, "sparse alias argument table allocates"); if (!at) { res_remove(src); res_remove(out); return; }
    CHECK(resource_vis() & MTL4VisibilityOptionResourceAlias, "shared sparse storage requests alias visibility at pass boundaries");
    mtl_sub_begin();
    for (int n = 0; n < N; n++) {
        enc_b_copy_buffer_texture(blit(), src, n * TILE, 1024, TILE, MTLSizeMake(256, 256, 1), writer, 0, 0, MTLOriginMake(0, 0, 0));
        enc_b_end(g_blit); g_blit = nil;
        [at setTexture:reader.gpuResourceID atIndex:0]; [at setAddress:out.gpuAddress + n * OUT atIndex:0];
        id<MTL4ComputeCommandEncoder> e = enc_c_begin(nil); [e setArgumentTable:at]; enc_c_pso(e, ps);
        enc_c_dispatch(e, MTLSizeMake(32, 32, 1), MTLSizeMake(8, 8, 1)); enc_c_end(e);
    }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "ordered sparse alias GPU passes complete");
    const uint32_t *p = out.contents; int valid = 1;
    for (int n = 0; n < N; n++) for (int i = 0; i < OUT / 16; i++) for (int c = 0; c < 4; c++)
        valid &= p[n * OUT / 4 + i * 4 + c] == ((c == n || c == 3 || n == 3) ? 255u : 0u);
    CHECK(valid, "each sparse alias read sees its current GPU write, not an older pass");
    res_remove(src); res_remove(out);
}

// Hold the encode worker before a submitted segment, then change its source's
// sparse mapping. Queue operations must not overtake that earlier segment.
// Both physical tiles are already populated, so the two readbacks distinguish
// old and new contents without relying on undefined newly allocated bytes.
static void check_mapping_order(pool_t *phys, pool_t *virt, tex_t *a, tex_t *b) {
    map_req extra = {(NVNmemoryPool *)phys, 0x110000, 0x310000, 0x10000, 0, 0};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&extra), "ordering fixture maps the second physical tile");
    mtl_sub_begin(); tex_mtl(b);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "both physical tiles populated before ordering fixture");
    NvPkg *pkg = tex_get(a->serial); id<MTLTexture> mt = pkg->tex;
    id<MTLBuffer> out = [g_dev newBufferWithLength:0x20000 options:MTLResourceStorageModeShared];
    CHECK(out != nil, "sparse ordering readback allocates"); if (!out) return;
    res_add(out);
    uint8_t tile[0x10000], expected[2][0x10000]; unsigned th, td;
    nvn_tex_level_tiles(&a->d, 0, &th, &td);
    nvn_sparse_copy(virt, tile, (uint64_t)(uintptr_t)a->mem, sizeof tile);
    nvn_deswizzle_tile(th, 16, 1024, 64, tile, expected[0], 1024);
    nvn_sparse_copy(virt, tile, (uint64_t)(uintptr_t)b->mem + 0x10000, sizeof tile);
    nvn_deswizzle_tile(th, 16, 1024, 64, tile, expected[1], 1024);
    CHECK(memcmp(expected[0], expected[1], sizeof tile), "ordering fixture has distinct tile versions");
    _Atomic uint64_t reached = 0;
    if (rec_on()) {
        pthread_mutex_lock(&g_note_mu);
        rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&reached; r->a[1] = 1;
    }
    mtl_sub_begin();
    enc_b_copy_texture_buffer(blit(), mt, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(256, 256, 1), out, 0, 1024, 0x10000);
    g_cut_copy = 1; sub_cut();
    map_req move = {(NVNmemoryPool *)phys, 0x110000, 0x200000, 0x10000, 0, 0};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&move), "ordering fixture remaps the source after submitting its read");
    tex_mtl(a);
    if (rec_on()) pthread_mutex_unlock(&g_note_mu);
    enc_b_copy_texture_buffer(blit(), mt, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(256, 256, 1), out, 0x10000, 1024, 0x10000);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "sparse mapping order submissions complete");
    CHECK(!memcmp(out.contents, expected[0], 0x10000), "an earlier submitted read retains the pre-remap tile");
    CHECK(!memcmp((uint8_t *)out.contents + 0x10000, expected[1], 0x10000), "a later read sees the remapped tile");
    res_remove(out);
    move.poff = 0x100000; extra.phys = NULL;
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&move), "ordering fixture restores the first tile");
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&extra), "ordering fixture unmaps its extra tile");
    mtl_sub_begin(); tex_mtl(a); tex_mtl(b);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "ordering fixture restores original mappings");
}

// kartnative c383bac247fd: aliases within one texture and across descriptors,
// followed by a moved tile and a partial unmap (which must leave sharing).
static void check_sparse_sharing(pool_t *phys, pool_t *virt) { @autoreleasepool {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, 512); h_nvnTextureBuilderSetHeight(&tb, 256); h_nvnTextureBuilderSetLevels(&tb, 1);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA_ASTC_4x4);
    NVNtexture a = {0}, b = {0};
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)virt, 0x200000); CHECK(h_nvnTextureInitialize(&a, &tb), "first compressed sparse texture initializes");
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)virt, 0x300000); CHECK(h_nvnTextureInitialize(&b, &tb), "second compressed sparse texture initializes");
    map_req rq[3] = {{(NVNmemoryPool *)phys, 0x100000, 0x200000, 0x10000, 0, 0},
                     {(NVNmemoryPool *)phys, 0x100000, 0x210000, 0x10000, 0, 0},
                     {(NVNmemoryPool *)phys, 0x100000, 0x300000, 0x10000, 0, 0}};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 3, (const NVNmappingRequest *)rq), "three virtual tiles map one physical page");
    tex_t *ta = (tex_t *)&a, *ttb = (tex_t *)&b; mtl_tex_init(ta); mtl_tex_init(ttb);
    mtl_sub_begin(); id<MTLTexture> ma = tex_mtl(ta), mb = tex_mtl(ttb);
    NvPkg *pa = tex_get(ta->serial), *pb = tex_get(ttb->serial);
    CHECK(pa->sp && pb->sp, "both compressed textures use sparse storage"); if (!pa->sp || !pb->sp) abort();
    CHECK(pa->sp->map[0] == pa->sp->map[1] && pa->sp->map[0] == pb->sp->map[0], "same physical page shares within and across textures");
    CHECK(g_sp_shared.n == 1, "one unique shared physical allocation exists");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "shared upload completes");
    check_compressed_tile(ma, ta, 0); check_compressed_tile(ma, ta, 1); check_compressed_tile(mb, ttb, 0);
    check_shared_gpu_updates(ma, mb);
    // Restore the guest bytes after the direct GPU writes in the fixture.
    mtl_sub_begin(); sp_sync(pa, ta, 1); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "shared tile guest contents restored");
    check_mapping_order(phys, virt, ta, ttb);
    rq[1].poff = 0x110000;
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&rq[1]), "one tile moves to a different physical page");
    mtl_sub_begin(); tex_mtl(ta);
    CHECK(pa->sp->map[0] != pa->sp->map[1] && pa->sp->map[0] == pb->sp->map[0] && g_sp_shared.n == 2, "remap separates only the moved tile");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "shared remap upload completes");
    check_compressed_tile(ma, ta, 0); check_compressed_tile(ma, ta, 1); check_compressed_tile(mb, ttb, 0);
    map_req hole = {NULL, 0, 0x200000, 0x1000, 0, 0};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&hole), "compressed tile partial unmap succeeds");
    mtl_sub_begin(); tex_mtl(ta);
    CHECK(pa->sp->map[0] != pb->sp->map[0], "partial mapping gets independent storage, preserving the complete alias");
    CHECK(!nvn_sparse_backing(virt, (uint64_t)(uintptr_t)ta->mem), "partial run cannot share a physical page");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "partial-hole upload completes");
    check_compressed_tile(ma, ta, 0); check_compressed_tile(mb, ttb, 0);
    h_nvnTextureFinalize(&a); h_nvnTextureFinalize(&b);
} }

static void check_indirect_spill(uint8_t *arguments) {
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void spilled(device uint* m [[buffer(30)]], uint3 g [[threadgroup_position_in_grid]]) { m[4 + g.x * 12288] = g.x + 1; }" options:nil error:&error];
    CHECK(lib != nil, "indirect spill fixture compiles"); if (!lib) return;
    NvShader *cs = [NvShader new]; cs->fn = [lib newFunctionWithName:@"spilled"]; cs->rf.tg[0] = cs->rf.tg[1] = cs->rf.tg[2] = 1; cs->rf.tgmem = 49152;
    NvProg *pr = [NvProg new]; pr->sh[5] = cs; prog_t guest = {.serial = UINT64_MAX - 1};
    serial_put(guest.serial, pr); S.cprog = &guest; // NOLINT(clang-analyzer-osx.cocoa.RetainCount): serial_del below releases the table's CF retain; ARC owns pr.
    // The CPU's stale arguments name no work. Queued GPU fills replace them with 256 groups; the dispatch must allocate for those current counts.
    mtl_sub_fill((uint64_t)(uintptr_t)arguments, 12, 0);
    CHECK(mdi_wait(), "initial argument clear completes");
    mtl_sub_fill((uint64_t)(uintptr_t)arguments, 4, 256);
    mtl_sub_fill((uint64_t)(uintptr_t)arguments + 4, 8, 1);
    p_dispatch dp = {.ind = (uint64_t)(uintptr_t)arguments}; dispatch(&dp);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "indirect spilled dispatch completes");
    CHECK(((uint32_t *)arguments)[0] == 256 && ((uint32_t *)arguments)[1] == 1 && ((uint32_t *)arguments)[2] == 1,
          "CPU sees GPU writes through the no-copy pool buffer");
    CHECK(!mtl_last_commit_error(), "indirect spilled dispatch has no GPU error");
    CHECK(g_tgm_buf.length >= DMSH_TGMEM_HEAD + 256ull * 49152, "spill allocation covers GPU-written counts before dispatch");
    uint32_t *words = g_tgm_buf.contents; int bad = 0;
    for (uint32_t g = 0; g < 256; g++) bad += words[4 + g * 12288] != g + 1;
    CHECK(!bad, "all indirectly dispatched groups have separate storage");
    serial_del(guest.serial); S.cprog = NULL; mtl_sub_begin();
}

int main(void) { @autoreleasepool {
    mtl_init();
    // A 2 MB physical pool and a 4 MB virtual pool (flags 0x2a1) whose first mapping is not the whole pool: sparse, with a page table
    const size_t plen = 2u << 20, vlen = 4u << 20;
    int backing_fd = -1;
    if (getenv("DM_TEST_FILE_BACKING")) {
        char path[PATH_MAX]; snprintf(path, sizeof path, "%s/dm-sparse-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        backing_fd = mkstemp(path); if (backing_fd < 0 || unlink(path) || ftruncate(backing_fd, (off_t)plen)) abort();
    }
    uint8_t *pmem = mmap(NULL, plen, PROT_READ | PROT_WRITE, backing_fd < 0 ? MAP_ANON | MAP_PRIVATE : MAP_SHARED, backing_fd, 0); if (pmem == MAP_FAILED) abort();
    for (size_t i = 0; i < plen; i++) pmem[i] = (uint8_t)(i * 7 + 3 + (i >> 16));
    pool_t phys = {0}, virt = {0};
    pool_builder_t ppb = {.mem = pmem, .size = plen, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_builder_t vpb = {.mem = NULL, .size = vlen, .flags = 0x2a1};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys, (const NVNmemoryPoolBuilder *)&ppb), "physical pool initialized");
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virt, (const NVNmemoryPoolBuilder *)&vpb), "virtual pool initialized");
    // A 512x512 RGBA8 sparse texture of 3 levels at offset 0: level 0 is 16 tiles of 128x128 (1 MB), the rest its mip tail. Map level 0's first half (8 tiles) and the tail
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    int render_target = getenv("DM_TEST_SPARSE_RENDER_TARGET") != NULL;
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE | (render_target ? NVN_TEXTURE_FLAGS_COMPRESSIBLE : 0)); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, 512); h_nvnTextureBuilderSetHeight(&tb, 512); h_nvnTextureBuilderSetDepth(&tb, 1); h_nvnTextureBuilderSetLevels(&tb, 3); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, 0);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "sparse texture initialized in the virtual pool");
    tex_t *t = (tex_t *)&tex;
    CHECK(t->L.size > (1u << 20) && t->L.size <= (2u << 20), "the texture's storage is level 0's 16 tiles and a tail");
    map_req rq[2] = {{(NVNmemoryPool *)&phys, 0, 0, 0x80000, 0, 0}, {(NVNmemoryPool *)&phys, 0x80000, 0x100000, (int64_t)(t->L.size - 0x100000), 0, 0}};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt, 2, (const NVNmappingRequest *)rq), "level 0's first 8 tiles and the mip tail mapped");
    CHECK(nvn_sparse_pool(&virt), "the pool is sparse");
    mtl_tex_init(t);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    id<MTLTexture> mt = tex_mtl(t);
    CHECK(mt != nil, "the sparse texture resolves");
    if (render_target) { CHECK(mt.isSparse, "COMPRESSIBLE does not allocate the entire sparse render target"); if (!mt.isSparse) return failed != 0; }
    if (render_target) {
        CHECK(mt.width == 512 && mt.mipmapLevelCount == 3, "sparse render target preserves its full dimensions and mip numbering");
        MTLRenderPassColorAttachmentDescriptor *a = [MTLRenderPassColorAttachmentDescriptor new];
        view_t view = {.set_levels = 1, .base_level = 2}; attach(a, t, &view, 1);
        CHECK(a.texture.width == 512 && a.level == 2 && a.level < a.texture.mipmapLevelCount,
              "attachment level remains relative to the full sparse render target");
    }
    NvPkg *pk = tex_get(t->serial);
    CHECK([pk isKindOfClass:[NvPkg class]] && pk->sparse, "resolved through the page table (packaged, sparse)");
    if (pk->sp) {
        CHECK(pk->tex.usage & MTLTextureUsageRenderTarget, "an uncompressed placement sparse texture has render-target use from the start");
        CHECK(pk->tex.usage & MTLTextureUsagePixelFormatView, "and format-view use");
        CHECK(pk->sp->nunit > 0 && pk->sp->map[0] && !pk->sp->map[15], "tile 0 is placed, the unmapped tile 15 is not");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "initial sparse upload completes");
        check_tile(pk->tex, t);
        map_req partial = {NULL, 0, 0, 0x4000, 0, 0};
        CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt, 1, (const NVNmappingRequest *)&partial), "first quarter of guest tile unmapped");
        mtl_sub_begin(); tex_mtl(t);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "partial unmap upload completes");
        CHECK(pk->sp->map[0] != 0, "partly mapped Metal tile stays resident");
        check_tile(pk->tex, t);
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = pk->tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(!cb.error, "sparse attachment rendering completes"); check_rendered_tile(pk->tex);
        if (render_target) {
            NVNtexture alias = {0};
            CHECK(h_nvnTextureInitialize(&alias, &tb), "a second descriptor over the same sparse storage initializes");
            tex_t *at = (tex_t *)&alias; mtl_tex_init(at);
            id<MTLTexture> amt = tex_mtl(at);
            CHECK(amt == pk->tex && tex_get(at->serial) == pk, "sparse GPU aliases share both rendered content and tile ownership");
            check_rendered_tile(amt);
            h_nvnTextureFinalize(&alias);
            CHECK(tex_mtl(t) == pk->tex, "finalizing one alias preserves the live render target");
        }
        map_req adjacent = {(NVNmemoryPool *)&phys, 0x180000, 0x80000, 0x10000, 0, 0};
        CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt, 1, (const NVNmappingRequest *)&adjacent), "adjacent render-target tile mapped");
        mtl_sub_begin(); tex_mtl(t); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "render-target mapping refresh completes");
        check_rendered_tile(pk->tex);
        mtl_sub_begin();
        // An upgrade that sp_make cannot give up front (image writes): the texture becomes whole, the tile record goes, and the next resolve after a mapping change stays whole
        id<MTLTexture> up = tex_upgrade(t, pk->tex, MTLTextureUsageShaderWrite, pk->tex.pixelFormat);
        CHECK(up != mt && pk->tex == up, "the upgrade replaced the holder's texture");
        CHECK(pk->sp == nil, "the upgrade dropped the tile record");
        CHECK(!up.isSparse, "the upgraded texture is whole");
        map_req more = {(NVNmemoryPool *)&phys, 0x80000, 0x80000, 0x40000, 0, 0};   // tiles 8..11 mapped now: the next resolve re-uploads through the whole path
        CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt, 1, (const NVNmappingRequest *)&more), "four more tiles mapped");
        id<MTLTexture> again = tex_mtl(t);
        CHECK(again != nil && !again.isSparse && pk->sp == nil, "after the upgrade the texture resolves whole, with no tile record");
    } else printf("no placement sparse texture on this host: the upgrade checks are skipped\n");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "prior sparse work completes before sharing fixture");
    check_sparse_sharing(&phys, &virt);
    mtl_sub_begin(); check_indirect_spill(pmem);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completes");
    CHECK(!mtl_last_commit_error(), "no GPU error");
    if (backing_fd >= 0) {
        CHECK(!msync(pmem, plen, MS_SYNC), "GPU-written file pages synchronize to their backing");
        uint32_t words[3] = {0};
        CHECK(pread(backing_fd, words, sizeof words, 0) == sizeof words && words[0] == 256 && words[1] == 1 && words[2] == 1,
              "backing file sees GPU-written indirect arguments, without a private copy");
        void *alias = mmap(NULL, plen, PROT_READ, MAP_SHARED, backing_fd, 0);
        CHECK(alias != MAP_FAILED && !memcmp(alias, pmem, plen), "a second file mapping sees all CPU and GPU bytes");
        if (alias != MAP_FAILED) munmap(alias, plen);
        close(backing_fd);
    }
    h_nvnTextureFinalize(&tex); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    defer_drain(); munmap(pmem, plen);
    // Release the retired texture/record, trim its now-empty heaps, then retire the heap behind a completed fence.
    for (int i = 0; i < 3; i++) { mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "retirement fence completes"); defer_drain(); }
    int live_heaps = 0; for (int c = 0; c < g_nsp_chunks; c++) live_heaps += g_sp_chunks[c].heap != nil;
    CHECK(live_heaps == 0, "empty sparse heaps leave the allocator");
    CHECK(!g_sp_shared.n && !g_sp_tiles_mapped, "last aliases release every shared physical page");
    CHECK(!g_sp_alias_n && !(resource_vis() & MTL4VisibilityOptionResourceAlias), "retiring the last shared tile removes sparse alias visibility");
    if (sp_on()) {
        __weak id<MTLHeap> retired;
        @autoreleasepool {
            uint32_t h = sp_tile_alloc(); CHECK(h != 0, "heap can be allocated again after retirement");
            CHECK(((h - 1) >> SP_IDX_BITS) == 0, "empty heap slot reused without renumbering handles");
            retired = g_sp_chunks[(h - 1) >> SP_IDX_BITS].heap;
            sp_tile_free(h); sp_trim();
            CHECK(retired != nil, "heap retained until GPU retirement fence");
            mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "heap retirement completes"); defer_drain(); res_commit();
        }
        CHECK(await_residency_removal(retired), "heap removed from residency after fence (Metal may retain completed command resources)");
    }
    if (sp_on()) {
        // Cross a chunk boundary, then release only the first chunk. The live
        // tile in the second chunk must keep its original handle and storage.
        uint32_t n = SP_CHUNK / g_sp_tile_bytes;
        uint32_t *handles = calloc(n + 1, sizeof *handles);
        for (uint32_t i = 0; i <= n; i++) handles[i] = sp_tile_alloc();
        CHECK(handles[0] && handles[n], "allocations span the smaller chunk boundary");
        uint32_t first = (handles[0] - 1) >> SP_IDX_BITS, last = (handles[n] - 1) >> SP_IDX_BITS;
        CHECK(first != last, "overflow gets a separate heap");
        CHECK(g_sp_chunks[last].nfree == n - 1, "overflow reserves one tile, leaving bounded tail capacity");
        for (uint32_t i = 0; i < n; i++) sp_tile_free(handles[i]);
        sp_trim();
        CHECK(!g_sp_chunks[first].heap && g_sp_chunks[last].heap, "empty first chunk retires without moving the live second chunk");
        uint32_t reuse = sp_tile_alloc();
        CHECK(((reuse - 1) >> SP_IDX_BITS) == last, "allocation fills existing free capacity first");
        sp_tile_free(reuse); sp_tile_free(handles[n]); free(handles); sp_trim();
        mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "split chunk retirement completes"); defer_drain(); res_commit();
        mem_resource_stat();
    }
    printf("t_sparse_upgrade: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
