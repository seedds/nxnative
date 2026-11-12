// The set of CPU-written textures (mtl_tex_cpu_written, tex_cpu_written, mtl_tex_release): membership, no duplicates, a released texture leaving it, and lookups of textures
// not in it staying cheap with thousands of consecutive serials in it (a CFSet that hashed serials as numbers probed a whole bucket run per miss: half of Batman: Arkham Knight's
// rendering thread with 6,442 CPU-written textures)
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

static void retired_classification(void) {
    uint8_t mem[2]; pool_t pools[2] = {{.mem=&mem[0]}, {.mem=&mem[1]}};
    tex_t a = {.serial=nvn_new_serial(), .pool=&pools[0], .mem=&mem[0]};
    tex_t b = {.serial=nvn_new_serial(), .pool=&pools[1], .mem=&mem[1]};
    mtl_tex_init(&a); mtl_tex_init(&b);
    mtl_tex_cpu_written(&a); mtl_tex_cpu_written(&b);
    mtl_tex_release(&a);
    CHECK(mtl_cpu_tex_count() == 1 && !nvn_hm_get(&g_cpu_tex,&a.serial), "release removes live membership");
    CHECK(tex_cpu_written(a.serial), "recorded snapshots retain CPU classification");
    uint64_t gen = g_ser_gen; mtl_tex_cpu_written(&a);
    CHECK(mtl_cpu_tex_count() == 1 && g_ser_gen == gen, "late repeated marking cannot resurrect a released texture");
    mtl_alias_drop_pool(&pools[0]);
    CHECK(!tex_cpu_written(a.serial) && tex_cpu_written(b.serial), "pool cleanup preserves the other pool's live membership");
    // A poll may first discover CPU writes after another thread finalized the texture.
    tex_t late = {.serial=nvn_new_serial(), .pool=&pools[1], .mem=&mem[1]};
    mtl_tex_init(&late); mtl_tex_release(&late); gen = g_ser_gen; mtl_tex_cpu_written(&late);
    CHECK(tex_cpu_written(late.serial) && mtl_cpu_tex_count() == 1 && g_ser_gen == gen+1, "first marking after release records only retired classification");
    mtl_tex_release(&b); mtl_tex_release(&b);
    CHECK(mtl_cpu_tex_count() == 0 && tex_cpu_written(b.serial), "repeated release keeps retired snapshots classified");
    mtl_alias_drop_pool(&pools[1]);
    CHECK(!tex_cpu_written(b.serial) && !tex_cpu_written(late.serial) && !g_cpu_tex_retired.n, "pool cleanup removes all retired classifications");
}
static void descriptor_replacement(void) {
    tex_t a = {.serial=nvn_new_serial()}, b = {.serial=nvn_new_serial()};
    mtl_tex_init(&a); mtl_tex_init(&b);
    mtl_reg_tex(1,&a,NULL); mtl_reg_tex(2,&a,NULL); mtl_reg_tex(3,&a,NULL);
    mtl_reg_tex(2,&b,NULL); mtl_reg_tex(3,NULL,NULL);
    mtl_tex_release(&a);
    CHECK(g_treg[1].retired && !g_treg[2].retired && !g_treg[3].ok, "replacing middle and head descriptors preserves serial ownership");
    mtl_reg_tex(1,&b,NULL); mtl_reg_tex(2,NULL,NULL);
    CHECK(!nvn_hm_get(&g_treg_serial,&a.serial), "replacing the last descriptor removes the old serial");
    mtl_tex_release(&b);
    CHECK(g_treg[1].retired, "the replacement texture retires its current descriptors");
    mtl_reg_tex(1,NULL,NULL);
    CHECK(!g_treg_serial.n, "clearing descriptors removes serial links");
}
static void finalized_upload(int write_after_record) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture without packaged layout initialized");
    uint64_t serial = ((tex_t *)&tex)->serial;
    uint8_t pixels[64]; for (int i=0;i<64;i++) pixels[i]=(uint8_t)(i+32);
    NVNcopyRegion region = {0,0,0,4,4,1};
    if (!write_after_record) h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    NVNcommandBuffer cmd; CHECK(h_nvnCommandBufferInitialize(&cmd, NULL), "command buffer initialized");
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (uint64_t)mem+131072, 0);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
    if (write_after_record) h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    // Cached descriptor views must not own every upload after its API object
    // is finalized. Two IDs and a compatible view exercise all lookup caches.
    mtl_reg_tex(2, (const tex_t *)&tex, NULL);
    mtl_sub_begin();
    id<MTLTexture> before = tex_by_id(1), second = tex_by_id(2); uint64_t compatible_id = 0;
    CHECK(before && second && g_treg[1].view && g_treg[2].view, "live descriptors cache their texture");
    CHECK(tex_fit(2, second, MTLTextureType2DArray, &compatible_id) && g_treg[2].alt, "live descriptor caches a compatible view");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "initial descriptor uploads completed");
    h_nvnTextureFinalize(&tex);
    CHECK(mtl_cpu_tex_count() == 0, "finalized texture leaves the live CPU set");
    CHECK(!g_treg[1].view && !g_treg[2].view && !g_treg[2].alt, "finalization retires every descriptor's cached views");
    // Reuse the API object: commands and registered descriptors must use their snapshots.
    memset(&tex, 0xa5, sizeof tex);
    state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(handle, NULL));
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "recorded texture readback completed");
    CHECK(!mtl_last_commit_error(), "recorded texture readback has no GPU error");
    CHECK(!memcmp(mem+131072, pixels, sizeof pixels), "finalized command snapshot uploads CPU-written pixels");
    mtl_sub_begin(); id<MTLTexture> registered = tex_by_id(1);
    CHECK(registered && registered.width == 4, "finalized registered descriptor resolves a Metal texture");
    CHECK(tex_fit(2, tex_by_id(2), MTLTextureType2DArray, &compatible_id), "late compatible view resolves");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "registered descriptor upload completed");
    CHECK(!g_treg[1].view && !g_treg[2].view && !g_treg[2].alt, "late descriptor views retire at submission end");
    // Resolve again: a retired serial may already hold an NvPkg in the ghost cache.
    mtl_sub_begin(); mtl_exec(nvn_rec_linear(handle, NULL));
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "recorded snapshot can be replayed again");
    CHECK(!memcmp(mem+131072, pixels, sizeof pixels), "replayed retired snapshot keeps its CPU content");
    mtl_reg_tex(1,NULL,NULL); mtl_reg_tex(2,NULL,NULL); h_nvnCommandBufferFinalize(&cmd);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    CHECK(!tex_cpu_written(serial), "pool finalization drops retired CPU classification");
    defer_drain(); munmap(mem,len);
}
// nvnMemoryPoolFinalize on a game thread while the submit thread walks the texture IDs (pool_gpu, holding g_rmu): the finalize dropped the pool's CPU-written classification before it
// marked the pool's IDs gone, so a walk in between found a live ID, a pool still set and a texture no longer CPU-written, and tex_mtl_ handed out its packaged entry (an NvPkg) as the
// texture: "-[NvPkg buffer]: unrecognized selector" in tex_by_id (Ocarina of Time, reference/nvn-objects.md). Here the test holds g_rmu as the walk does while another thread finalizes
static void *finalize_pool(void *pool) { h_nvnMemoryPoolFinalize((NVNmemoryPool *)pool); return NULL; }
static void finalize_during_walk(void) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture initialized");
    uint64_t serial = ((tex_t *)&tex)->serial;
    uint8_t pixels[64]; for (int i=0;i<64;i++) pixels[i]=(uint8_t)(i+32);
    NVNcopyRegion region = {0,0,0,4,4,1};
    h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> first = tex_by_id(1);
    CHECK(first && first.width == 4, "the packaged texture resolves");
    CHECK([tex_get(serial) isKindOfClass:[NvPkg class]], "its table entry is an NvPkg");
    pthread_t th; pthread_create(&th, NULL, finalize_pool, &pool);
    usleep(100000);   // the finalize runs as far as it can while the walk holds g_rmu
    CHECK(tex_cpu_written(serial), "the pool's classification stays while a walk can still find its IDs live");
    id again = tex_mtl((const tex_t *)&tex);
    CHECK(again && ![again isKindOfClass:[NvPkg class]], "tex_mtl does not hand out the NvPkg during the finalize");
    os_unfair_lock_unlock(&g_rmu);
    pthread_join(th, NULL);
    CHECK(!tex_cpu_written(serial) && g_treg[1].gone, "after the finalize the ID is gone and the classification dropped");
    os_unfair_lock_lock(&g_rmu); id after = tex_by_id(1); os_unfair_lock_unlock(&g_rmu);
    CHECK(![after isKindOfClass:[NvPkg class]], "the gone ID does not hand out the NvPkg either");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "submission completed");
    mtl_reg_tex(1,NULL,NULL); h_nvnTextureFinalize(&tex);
    defer_drain(); munmap(mem,len);
}
#if DM_GAME_TEX_DIRTY_UPLOAD
// nvn_ft_poll: a registered texture whose storage the GPU wrote after the CPU's rewrite is left to the GPU and stays registered, so the CPU's next rewrite is still uploaded
static void poll_asks_again(void) {
    const size_t len = 1 << 20;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 256); h_nvnTextureBuilderSetHeight(&tb, 256); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture initialized");
    tex_t *t = (tex_t *)&tex;
    CHECK(nvn_ft_tracked(t->serial), "the texture is registered");
    dm_guest_write_prepare(t->mem, t->L.size); memset(t->mem, 0x40, t->L.size);
    mtl_tex_gpu_written(t);
    nvn_ft_poll();
    CHECK(!tex_cpu_written(t->serial) && nvn_ft_tracked(t->serial), "a rewrite the GPU wrote over afterwards is left to the GPU, and the texture stays registered");
    dm_guest_write_prepare(t->mem, t->L.size); memset(t->mem, 0x80, t->L.size);
    nvn_ft_poll();
    CHECK(tex_cpu_written(t->serial) && !nvn_ft_tracked(t->serial), "the CPU's next rewrite is uploaded");
    h_nvnTextureFinalize(&tex); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    defer_drain(); munmap(mem, len);
}
// Writes beside a registered texture (ft_written): storage 0x200 past a host page shares its first and last pages with other data. A write there leaves the texture registered and
// not CPU-written, at the poll and at first use (nvn_ft_written); a write of the texture's own bytes in those pages, or of a page wholly inside it, is still taken as written.
// The Great Ace Attorney Chronicles' window textures and render targets lie so beside textures it fills from the CPU, and were all made CPU-written
static void poll_beside(void) {
    const size_t len = 4 << 20, pg = (size_t)getpagesize(), off = 4 * pg + 0x200;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture a = {0}, b = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 256); h_nvnTextureBuilderSetHeight(&tb, 256); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, (ptrdiff_t)off);
    CHECK(h_nvnTextureInitialize(&a, &tb), "texture a initialized");
    tex_t *ta = (tex_t *)&a;
    size_t boff = (off + ta->L.size + 2 * pg) & ~(pg - 1); boff += 0x200;   // b: two pages on, 0x200 past a page too
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, (ptrdiff_t)boff);
    CHECK(h_nvnTextureInitialize(&b, &tb), "texture b initialized");
    tex_t *tb_ = (tex_t *)&b;
    CHECK(ta->L.size > 2 * pg && nvn_ft_tracked(ta->serial) && nvn_ft_tracked(tb_->serial), "both registered, each with pages wholly inside");
    // beside a: the bytes before it in its first page, and after it in its last
    uint8_t *before = ta->mem - 0x100, *after = ta->mem + ta->L.size + 0x40;
    dm_guest_write_prepare(before, 0x100); memset(before, 0x5a, 0x100);
    dm_guest_write_prepare(after, 0x40); memset(after, 0x5a, 0x40);
    nvn_ft_poll();
    CHECK(!tex_cpu_written(ta->serial) && nvn_ft_tracked(ta->serial), "a write beside it, in the pages it shares, does not make it CPU-written (poll)");
    dm_guest_write_prepare(after, 0x40); memset(after, 0x33, 0x40);   // again, after the restamp
    nvn_ft_poll();
    CHECK(!tex_cpu_written(ta->serial) && nvn_ft_tracked(ta->serial), "nor a second write beside it");
    dm_guest_write_prepare(ta->mem + ta->L.size - 4, 4); ta->mem[ta->L.size - 1] ^= 0xff;
    nvn_ft_poll();
    CHECK(tex_cpu_written(ta->serial) && !nvn_ft_tracked(ta->serial), "a write of its own bytes in its last page is uploaded");
    // b at first use: a write beside it, then one into a page wholly inside it
    uint8_t *bb = tb_->mem - 0x80;
    dm_guest_write_prepare(bb, 0x80); memset(bb, 0x11, 0x80);
    CHECK(!nvn_ft_written(tb_) && nvn_ft_tracked(tb_->serial) && !tex_cpu_written(tb_->serial), "a write beside it does not make it CPU-written at first use");
    uint8_t *mid = (uint8_t *)(((uintptr_t)tb_->mem + 2 * pg) & ~(uintptr_t)(pg - 1));
    dm_guest_write_prepare(mid, 1); mid[0] = 1;
    CHECK(nvn_ft_written(tb_) && tex_cpu_written(tb_->serial), "a write of a page wholly inside it does");
    // c: COMPRESSIBLE (out of the first-use rule, as GAAC's targets), resolved as a GPU texture that can be rendered into: a write beside it does not make it CPU-written
    size_t coff = (boff + tb_->L.size + 2 * pg) & ~(pg - 1); coff += 0x200;
    NVNtexture c = {0};
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_COMPRESSIBLE); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, (ptrdiff_t)coff);
    CHECK(h_nvnTextureInitialize(&c, &tb), "texture c initialized");
    tex_t *tc = (tex_t *)&c;
    mtl_sub_begin(); id<MTLTexture> hc = tex_mtl(tc); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "c resolved");
    CHECK(hc && mtl_tex_gpu_owned(tc->serial) && nvn_ft_tracked(tc->serial), "c is a GPU texture that can be rendered into, still registered");
    uint8_t *cb = tc->mem - 0x80;
    dm_guest_write_prepare(cb, 0x80); memset(cb, 0x22, 0x80);
    nvn_ft_poll();
    CHECK(!tex_cpu_written(tc->serial) && mtl_tex_gpu_owned(tc->serial), "a write beside a GPU texture leaves it the GPU's, not CPU-written");
    h_nvnTextureFinalize(&a); h_nvnTextureFinalize(&b); h_nvnTextureFinalize(&c); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    defer_drain(); munmap(mem, len);
}
#else
static void poll_asks_again(void) { printf("poll_asks_again: skipped, the profile has no tex_dirty_upload\n"); }
static void poll_beside(void) { printf("poll_beside: skipped, the profile has no tex_dirty_upload\n"); }
#endif
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
static void settle(void) { for (int i = 0; i < 2; i++) { g_fence++; pthread_mutex_lock(&g_mu); g_done = g_fence; pthread_mutex_unlock(&g_mu); defer_drain(); } }   // the GPU passed every fence
// The GPU's first write over a texture taken from its storage (cpu_unc_drop): every serial that held the package now holds its texture, the package goes, and the serials go one by
// one, the last through the ghost ring. The texture stays resident while any serial entry, live or finalized, holds it, and leaves the residency set with the last. Ocarina of Time's
// 260x260 terrain tiles handed over this way left the set with the package, or with the first holder a ghost-ring eviction dropped, while still drawn: GPU address faults (#1326)
static void handover_holders(void) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    memset(mem + 65536, 0x5a, 4096);   // what the CPU wrote before the first use
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "hand-over: pool initialized");
    NVNtextureBuilder tb; NVNtexture a = {0}, b = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&a, &tb) && h_nvnTextureInitialize(&b, &tb), "hand-over: two texture objects over the same storage");
    uint64_t sa = ((tex_t *)&a)->serial, sb = ((tex_t *)&b)->serial;
    mtl_sub_begin();
    id<MTLTexture> ta = tex_mtl((const tex_t *)&a), tb2 = tex_mtl((const tex_t *)&b);
    CHECK(ta && ta == tb2 && [tex_get(sa) isKindOfClass:[NvPkg class]] && tex_get(sa) == tex_get(sb), "hand-over: both resolve to one package taken from the storage");
    mtl_tex_gpu_written((const tex_t *)&a);   // the GPU writes over the storage
    cpu_unc_drop();
    CHECK(tex_get(sa) == (id)ta && tex_get(sb) == (id)ta, "hand-over: both serials hold the package's texture");
    NVNtexture c = {0}; CHECK(h_nvnTextureInitialize(&c, &tb), "hand-over: a new texture object over the same storage");
    id<MTLTexture> tc = tex_mtl((const tex_t *)&c);
    CHECK(tc && tc != ta, "hand-over: the new object gets a texture of its own");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "hand-over: submission completed");
    settle();
    CHECK(resident(ta), "hand-over: the package's release leaves the texture resident for its serials");
    h_nvnTextureFinalize(&a); settle();
    CHECK(resident(ta), "hand-over: one serial's release leaves it resident for the other");
    tex_put(sa, ta);   // a record of the finalized object replayed: its serial resolves through the ghost ring
    h_nvnTextureFinalize(&b); settle();
    CHECK(resident(ta), "hand-over: a finalized serial in the ghost ring keeps it resident");
    for (uint64_t i = 0; i < NGHOST; i++) tex_put((1ull << 61) + i, [NSObject new]);   // the ring evicts that entry
    settle();
    pthread_mutex_lock(&g_smu); int left = own_stats_locked().retire_wait; pthread_mutex_unlock(&g_smu);
    CHECK(!resident(ta) && !left, "hand-over: the last holder's release takes it out of the residency set");
    h_nvnTextureFinalize(&c); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool); settle();
    CHECK(!resident(tc), "hand-over: the pool's own texture goes with the pool");
    munmap(mem, len);
}
// Retirement's two conditions, one after the other: the queue waits for the fence of the submission in flight at the retirement (completion), then asks the owner (eligibility,
// own_retire_locked). An object an entry holds waits in the owner; its last count going hands it back behind the submission then in flight, so the entry's last encodings complete
// before it leaves the residency set. The entry here holds a view of the texture (an sRGB / linear twin's): the entry's own drop retires the view, and only the owner's hand-back
// retires the texture. One step of the GPU: fence() passes one fence
static void fence(void) { g_fence++; pthread_mutex_lock(&g_mu); g_done = g_fence; pthread_mutex_unlock(&g_mu); defer_drain(); }
static void retire_after_completion(void) {
    id<MTLTexture> x = [g_dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]];
    res_add(x); res_commit();
    uint64_t k = (1ull << 62) + 990001; tex_put(k, [x newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]);   // a serial's entry holds a view of it
    pthread_mutex_lock(&g_smu); uint32_t w0 = own_stats_locked().retire_wait; pthread_mutex_unlock(&g_smu);
    defer_obj_release(x);   // retired: behind the submission now in flight
    defer_drain();
    CHECK(resident(x), "retire: before its fence passes, a retired texture stays resident (the queue does not ask the owner yet)");
    fence();
    pthread_mutex_lock(&g_smu); uint32_t w1 = own_stats_locked().retire_wait; pthread_mutex_unlock(&g_smu);
    CHECK(resident(x) && w1 == w0 + 1, "retire: its fence passed, an entry still holds it: it stays resident and waits in the owner");
    serial_del(k);   // the last entry goes: handed back behind the submission in flight
    pthread_mutex_lock(&g_smu); uint32_t w2 = own_stats_locked().retire_wait; pthread_mutex_unlock(&g_smu);
    defer_drain();
    CHECK(resident(x) && w2 == w0, "retire: handed back, it stays resident until the submission in flight when its last entry went completes");
    fence(); res_commit();
    CHECK(!resident(x), "retire: then it leaves the residency set");
}
// The same two conditions across threads, as the host runs them: a completion handler's drain (defer_drain, its try of g_smu) while the submit thread's entries come
// and go. One live entry holds a view of the texture throughout, so whatever the interleaving it must never leave the residency set; the other thread retires it again
// and again and passes fences. When that entry goes too, the owner hands the texture back and it leaves after the next fence. TSan: the shared state is the owner's
// (under g_smu), the queue's (g_deferred_mu) and the residency set's (g_res_mu)
enum { CHURN = 4000 };
static _Atomic int g_churn_done, g_lost_residency;
static id<MTLTexture> g_churn_x;
static void *retirer(void *arg) {
    (void)arg;
    for (int i = 0; !atomic_load(&g_churn_done); i++) { if (!(i % 8)) defer_obj_release(g_churn_x); fence(); }
    return NULL;
}
static void retire_across_threads(void) {
    @autoreleasepool {
    id<MTLTexture> x = g_churn_x = [g_dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]];
    res_add(x); res_commit();
    uint64_t hold = (1ull << 62) + 990101, churn = (1ull << 62) + 990102, ghost = 990103;
    tex_put(hold, [x newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]);
    pthread_t t; atomic_store(&g_churn_done, 0); atomic_store(&g_lost_residency, 0);
    pthread_create(&t, NULL, retirer, NULL);
    for (int i = 0; i < CHURN; i++) {   // the texture, a view of it, a finalized serial's ghost entry: put and taken away
        id o = i % 3 == 0 ? (id)x : (id)[x newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
        tex_put(i % 3 == 2 ? ghost : churn, o);
        serial_del(churn);
        if (!resident(x)) atomic_store(&g_lost_residency, 1);
    }
    atomic_store(&g_churn_done, 1); pthread_join(t, NULL);
    CHECK(!atomic_load(&g_lost_residency) && resident(x), "across threads: held by a live entry's view, the texture never left the residency set");
    for (uint64_t i = 0; i < NGHOST; i++) tex_put((1ull << 61) + 1000 + i, [NSObject new]);   // the ghost entry goes with the ring
    serial_del(hold); defer_drain(); fence(); fence(); res_commit();
    pthread_mutex_lock(&g_smu); uint32_t waiting = own_stats_locked().retire_wait; char why[256] = ""; int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
    CHECK(!resident(x) && !waiting, "across threads: its last entry gone, it is handed back and leaves the residency set");
    if (!ok) fprintf(stderr, "owner: %s\n", why);
    CHECK(ok, "across threads: the owner's counts and waiting objects agree with its tables");
    g_churn_x = nil;
    }
}
int main(int argc, const char **argv) { @autoreleasepool {
    enum { N = 6442, BASE = 1000 };
    static tex_t t[N];
    for (int i = 0; i < N; i++) { t[i].serial = BASE + i; t[i].d.width = t[i].d.height = 4; t[i].d.depth = 1; t[i].d.format = 0x25; mtl_tex_init(&t[i]); }
    CHECK(mtl_cpu_tex_count() == 0 && !tex_cpu_written(BASE), "empty at start");
    for (int i = 0; i < N; i++) mtl_tex_cpu_written(&t[i]);
    mtl_tex_cpu_written(&t[7]);   // again: no duplicate
    CHECK(mtl_cpu_tex_count() == N, "every CPU-written texture counted once");
    int in = 0; for (int i = 0; i < N; i++) in += tex_cpu_written(BASE + i);
    CHECK(in == N, "every one found");
    CHECK(!tex_cpu_written(BASE - 1) && !tex_cpu_written(BASE + N) && !tex_cpu_written(0), "serials outside the set are not in it");
    // Misses: a million lookups of serials past the set, as every resolve of a GPU-only texture asks. With the old set's probing this took seconds; with a mixed hash, far less
    uint64_t t0 = bp_now(); int hits = 0;
    for (int i = 0; i < 1000000; i++) hits += tex_cpu_written(BASE + N + 1 + (uint64_t)i);
    double ms = (bp_now() - t0) / 1e6;
    printf("1,000,000 lookups of serials not in a set of %d consecutive ones: %.1f ms\n", N, ms);
    CHECK(hits == 0, "no miss is reported as a hit");
    CHECK(ms < 1000, "a million misses take well under a second");
    mtl_tex_release(&t[100]); mtl_tex_release(&t[101]);
    CHECK(!tex_cpu_written(BASE + 100) && !tex_cpu_written(BASE + 101) && tex_cpu_written(BASE + 102), "a released texture leaves the set, the others stay");
    CHECK(mtl_cpu_tex_count() == N - 2, "the count follows the releases");
    tex_t z = {0}; mtl_tex_cpu_written(&z);
    CHECK(mtl_cpu_tex_count() == N - 2, "a texture with no serial is not recorded");
    for (int i=0;i<N;i++) mtl_tex_release(&t[i]);
    // NVNtexture is plain data: finalizing a copy ends the copy. The texture stays live while the object it was initialized in holds it; if that object no longer does, the copy was the last one
    static tex_t own, cpy, moved;
    own.magic = MAGIC('T', 'E', 'X', 0); own.serial = 900001; own.d = t[0].d; mtl_tex_init(&own); mtl_tex_cpu_written(&own);
    cpy = own; mtl_tex_release(&cpy);
    CHECK(tex_cpu_written(own.serial) == 1, "finalizing a copy leaves the texture live");
    mtl_tex_release(&own);
    CHECK(!own_tex_live(own.serial), "finalizing the object itself ends it");
    own.serial = 900002; mtl_tex_init(&own); moved = own; own.magic = 0; mtl_tex_release(&moved);
    CHECK(!own_tex_live(moved.serial), "a copy outliving the object it was initialized in ends the texture");
    retired_classification(); descriptor_replacement();
    if (argc > 1 && !strcmp(argv[1], "--metal")) { mtl_init(); finalized_upload(0); finalized_upload(1); finalize_during_walk(); poll_asks_again(); poll_beside(); handover_holders(); retire_after_completion(); retire_across_threads(); }
    printf("t_cpu_tex: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
