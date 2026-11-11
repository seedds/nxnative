// The tables of handles traced to no slot (pool_gpu) over noted changes: a pool added or removed records REG_POOL and no ID, so a table built before keeps its buffer when nothing
// it holds changed, takes again an ID whose storage came with a new pool, and the IDs of a finalized pool go to the dummy through notes of their own (treg_gone). A new submission
// takes again only the IDs whose entries were views of retired registrations, which went at the end of the last one, and a storage takeover builds nothing. A patch follows the
// walk's rules: past the budget an unread texture is not made (its entry is its stand-in, pool_stand), and a view the shader-read scan writes in place is taken again at the next submission. Each patched table holds
// what a table built whole holds.
// Ocarina of Time maps and unmaps virtual pool runs dozens of times a second and registers something between most of its draws; each of those walked its 33,025-entry texture table
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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

enum { LEN = 1 << 20, IDS = 10 };
static uint8_t *pages(void) { uint8_t *m = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (m == MAP_FAILED) abort(); return m; }
// The 2D float texture table at base 0, in a submission of its own, as a draw that reads it would ask
static id<MTLBuffer> table(void) {
    mtl_sub_begin();
    __unsafe_unretained id<MTLBuffer> b = nil;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> held = b;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    return held;
}
static pool_snap *snap(void) { for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf && !g_psnap[k].samp && g_psnap[k].dtt == MTLTextureType2D && g_psnap[k].dpf == NVN_DPF_FLOAT && !g_psnap[k].base) return &g_psnap[k]; return NULL; }
static uint64_t entry(id<MTLBuffer> b, int i) { return ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }
static uint64_t dummy_of(id<MTLBuffer> b) { return entry(b, (int)*(const uint32_t *)b.contents); }
static int lists(const pool_ids *l, int id_) { for (int i = 0; i < l->n; i++) if (l->v[i] == id_) return 1; return 0; }
// A patched table against the same table built whole (a bump the ring holds no note for) in the next submission, over the IDs the test registers. A retired registration's view is
// made again in each submission, so for its ID only whether it holds a texture can agree
static void same_as_whole(id<MTLBuffer> patched, const char *label) {
    uint64_t builds = g_stat_pool_builds; atomic_fetch_add(&g_reg_gen, 1);
    id<MTLBuffer> w = table();
    CHECK(g_stat_pool_builds == builds + 1, "a change without a note builds the table whole");
    int same = 1;
    for (int i = 0; i < IDS; i++) { int pd = entry(patched, i) == dummy_of(patched), wd = entry(w, i) == dummy_of(w);
        same &= pd == wd && (wd || g_treg[i].retired || entry(patched, i) == entry(w, i)); }
    CHECK(same, label);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_RT_RECYCLE", "0", 1);   // textures with allocations of their own: one placed in a recycled heap is resident through its heap, whatever retires it
    mtl_init();
    uint8_t *ma = pages(), *mb = pages(), *mc = pages();
    // Pool A holds texture ID 1; pool B is a pool object whose storage the host does not map yet (a virtual pool before its first mapping): ID 2 there has no storage
    pool_t a = {0}, b = {0}; pool_builder_t pb = {.mem = ma, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&a, (const NVNmemoryPoolBuilder *)&pb), "pool A initialized");
    NVNtextureBuilder tb = {0}; NVNtexture t1 = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&a, 0);
    CHECK(h_nvnTextureInitialize(&t1, &tb), "texture on pool A initialized");
    b.magic = MAGIC('P', 'O', 'O', 'L'); b.mem = mb; b.size = LEN; b.flags = pb.flags;
    tex_t t2 = *(const tex_t *)&t1; t2.serial = nvn_new_serial(); t2.pool = &b; t2.mem = mb; mtl_tex_init(&t2);
    mtl_reg_tex(1, (const tex_t *)&t1, NULL); mtl_reg_tex(2, &t2, NULL);

    uint64_t builds = g_stat_pool_builds;
    id<MTLBuffer> b0 = table();
    CHECK(g_stat_pool_builds == builds + 1, "the first walk builds the table");
    CHECK(entry(b0, 1) != dummy_of(b0) && entry(b0, 2) == dummy_of(b0), "ID 1 holds its texture, ID 2 (no storage) the dummy");
    CHECK(snap() && lists(&snap()->dead, 2) && !lists(&snap()->dead, 1), "the table lists ID 2 as without storage");

    // A pool elsewhere: the table holds nothing it changes
    uint64_t kept = g_stat_pool_kept, pchg = g_stat_pool_pchg; mtl_pool_add(mc, LEN);
    id<MTLBuffer> b1 = table();
    CHECK(g_stat_pool_builds == builds + 1 && g_stat_pool_kept == kept + 1 && g_stat_pool_pchg == pchg + 1, "an unrelated pool keeps the table");
    CHECK(b1 == b0 && entry(b1, 2) == dummy_of(b1), "the same buffer, ID 2 still without storage");

    // ID 2's storage arrives: only it is taken again
    uint64_t patched = g_stat_pool_patched; mtl_pool_add(mb, LEN);
    id<MTLBuffer> b2 = table();
    CHECK(g_stat_pool_builds == builds + 1 && g_stat_pool_patched == patched + 1, "the pool that brings ID 2's storage patches the table");
    CHECK(b2 != b0 && entry(b2, 2) != dummy_of(b2) && entry(b2, 1) == entry(b0, 1), "ID 2 now holds its texture, ID 1 unchanged");
    CHECK(!lists(&snap()->dead, 2), "and leaves the list of IDs without storage");
    same_as_whole(b2, "the patched table holds what a table built whole does (pool added)");

    // Pool A finalized: ID 1 is marked gone with a note of its own, whatever else the Finalize changes
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&a);
    id<MTLBuffer> b3 = table();
    CHECK(g_treg[1].gone && entry(b3, 1) == dummy_of(b3), "the finalized pool's ID holds the dummy");
    CHECK(entry(b3, 2) == entry(b2, 2), "the other pool's ID is unchanged");

    // A bump the ring holds no note for still builds the table whole
    builds = g_stat_pool_builds; atomic_fetch_add(&g_reg_gen, 1);
    id<MTLBuffer> b4 = table();
    CHECK(g_stat_pool_builds == builds + 1 && entry(b4, 2) == entry(b3, 2), "a change without a note builds the table whole");

    // A retired registration: ID 3 names a CPU-written texture (a Metal texture of its own, which the sharing table does not hold) that the game has finalized. Its view lasts one
    // submission (treg_finish_submission), so the next submission takes ID 3 again instead of building the table whole
    NVNtexture t3 = {0}; h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&b, 65536);
    CHECK(h_nvnTextureInitialize(&t3, &tb), "texture on pool B initialized");
    uint8_t pixels[64]; for (int i = 0; i < 64; i++) pixels[i] = (uint8_t)(i + 32);
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1}; h_nvnTextureWriteTexels(&t3, NULL, &region, pixels);
    mtl_reg_tex(3, (const tex_t *)&t3, NULL);
    id<MTLBuffer> b5 = table();
    CHECK(entry(b5, 3) != dummy_of(b5), "ID 3 holds its texture");
    h_nvnTextureFinalize(&t3);
    id<MTLBuffer> b6 = table();
    CHECK(g_treg[3].retired && entry(b6, 3) != dummy_of(b6), "the finalized texture's ID still resolves in this submission");
    CHECK(snap()->sub == g_sub_serial && lists(&snap()->trans, 3), "the table lists it as a view of this submission");
    CHECK(!g_treg[3].view, "its view went at the end of the submission");
    builds = g_stat_pool_builds; uint64_t schg = g_stat_pool_schg;
    id<MTLBuffer> b7 = table();
    CHECK(g_stat_pool_builds == builds && g_stat_pool_schg == schg + 1, "the next submission takes the retired ID again without building the table");
    CHECK(entry(b7, 3) != dummy_of(b7) && entry(b7, 2) == entry(b6, 2), "ID 3 resolves again, ID 2 unchanged");
    same_as_whole(b7, "the patched table holds what a table built whole does (retired ID)");

    // A storage takeover: ID 4 names a finalized texture that resolves to a shared one (a pool the CPU cannot reach). When a texture of another description takes its storage,
    // the change names no texture: no table is built, and no ID view is checked again. The evicted texture goes only behind the submission's fence (here the ghost table of finalized
    // serials keeps it), and a retired ID's view of it goes at the submission's end, so the next submission takes ID 4 again
    uint8_t *md = pages(); pool_t dpool = {0}; pool_builder_t pd = {.mem = md, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&dpool, (const NVNmemoryPoolBuilder *)&pd), "pool D initialized");
    NVNtexture t4 = {0}; h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&dpool, 0);
    CHECK(h_nvnTextureInitialize(&t4, &tb), "texture 4 initialized");
    mtl_reg_tex(4, (const tex_t *)&t4, NULL);
    table(); h_nvnTextureFinalize(&t4);
    id<MTLBuffer> b9 = table();
    CHECK(g_treg[4].retired && entry(b9, 4) != dummy_of(b9), "the finalized texture's ID resolves to the shared texture");
    CHECK(!g_treg[4].view && lists(&snap()->trans, 4), "its view went at the submission's end, and the table lists it");
    for (int i = 0; i <= ALIAS_IDLE; i++) { mtl_sub_begin(); mtl_wait(mtl_sub_commit(), 5000000000ull); }   // until the sharing table no longer spares the entry
    NVNtexture t5 = {0}; h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32F); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&dpool, 0);
    CHECK(h_nvnTextureInitialize(&t5, &tb), "texture 5 over the same storage initialized");
    mtl_reg_tex(5, (const tex_t *)&t5, NULL);
    builds = g_stat_pool_builds; uint64_t taken = g_alias_taken, ep = g_treg_ep;
    id<MTLBuffer> b10 = table();
    CHECK(g_alias_taken > taken, "texture 5 took the storage over");
    CHECK(g_stat_pool_builds == builds && entry(b10, 5) != dummy_of(b10) && entry(b10, 4) == entry(b9, 4), "the takeover builds no table, and ID 4 keeps its entry in that submission");
    CHECK(g_treg_ep == ep, "nor sends every texture ID down the slow path");
    CHECK(!g_treg[4].view, "ID 4's view went at the submission's end");
    id<MTLBuffer> b11 = table();
    CHECK(g_stat_pool_builds == builds && entry(b11, 4) != dummy_of(b11), "the next submission takes ID 4 again without building the table");
    same_as_whole(b11, "the patched table holds what a table built whole does (takeover)");

    // Past the budget: a texture registered since the walk that no shader has read is not made, as in a walk (its entry is its stand-in, or the dummy with DM_NVN_POOL_STANDIN=0), and the table is patched rather than built whole. ID 6 names a
    // texture the game finalized before anything made it. Once a shader reads ID 6, the scan of the read bitmap makes it whatever the budget and writes it into the table in place:
    // a view of a retired registration, which the table lists, so the next submission takes it again
    mtl_reg_tex(3, NULL, NULL); mtl_reg_tex(4, NULL, NULL); table(); table();   // no retired IDs left: the table lists none
    CHECK(snap()->sub == 0 && snap()->trans.n == 0, "a table without views of retired registrations");
    id<MTLBuffer> hog = [g_dev newBufferWithLength:pool_budget() + (1u << 20) options:MTLResourceStorageModePrivate];
    CHECK(hog && pool_budget_full(), "over the budget");
    NVNtexture t6 = {0}; h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&b, 131072);
    CHECK(h_nvnTextureInitialize(&t6, &tb), "texture 6 initialized");
    mtl_reg_tex(6, (const tex_t *)&t6, NULL);
    builds = g_stat_pool_builds; patched = g_stat_pool_patched; kept = g_stat_pool_kept;
    id<MTLBuffer> b12 = table();
    CHECK(g_stat_pool_builds == builds && g_stat_pool_patched + g_stat_pool_kept > patched + kept, "a registration past the budget patches the table");
    #define LEFT_OUT(buf, i) (!g_treg[i].view && (stand_on() ? g_treg[i].stand && entry(buf, i) == g_treg[i].stand_rid : entry(buf, i) == dummy_of(buf)))   // not the texture: its stand-in
    CHECK(LEFT_OUT(b12, 6) && snap()->left > 0 && !snap()->left_req, "the unread texture is left out");
    h_nvnTextureFinalize(&t6); table();
    CHECK(g_treg[6].retired && !tex_get(((const tex_t *)&t6)->serial) && LEFT_OUT(snap()->buf, 6), "finalized, it is still left out: nothing made it");
    ((uint32_t *)g_seen_buf.contents)[6 / 32] |= 1u << (6 % 32);
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); pool_seen_scan();
    pool_snap *s = snap(); id<MTLBuffer> b13 = s->buf;
    CHECK(entry(b13, 6) != dummy_of(b13), "a read goes into the table in place, past the budget");
    CHECK(s->sub == g_sub_serial && lists(&s->trans, 6), "as a view of a retired registration of this submission");
    os_unfair_lock_unlock(&g_rmu); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    CHECK(!g_treg[6].view, "the view went at the submission's end");
    builds = g_stat_pool_builds; schg = g_stat_pool_schg;
    id<MTLBuffer> b14 = table();
    CHECK(g_stat_pool_builds == builds && g_stat_pool_schg > schg && entry(b14, 6) != dummy_of(b14), "the next submission takes ID 6 again without building the table");
    hog = nil;

    // A live sRGB / linear twin: ID 8's serial resolves to a view of the shared texture that ID 7, finalized since, made (srgb_twins in tex_mtl_). The view's entry counts as
    // holding that texture (count_add), so a texture of another description over the storage does not take it while ID 8 is live: with the view alone counted it went, the
    // takeover's note named no ID of the twin, and the tables kept naming a view whose parent had left the sharing table and then the residency set (GitHub issue #2125).
    // The walk after the twin's first resolution takes that resolution's note, so nothing but the takeover could send ID 8 anywhere
    uint8_t *me = pages(); pool_t epool = {0}; pool_builder_t pe = {.mem = me, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&epool, (const NVNmemoryPoolBuilder *)&pe), "pool E initialized");
    NVNtexture t7 = {0}, t8 = {0}, t9 = {0}, t10 = {0};
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&epool, 0);
    CHECK(h_nvnTextureInitialize(&t7, &tb), "texture 7 initialized");
    mtl_reg_tex(7, (const tex_t *)&t7, NULL); table();
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    CHECK(h_nvnTextureInitialize(&t8, &tb), "texture 8, its sRGB twin over the same storage, initialized");
    mtl_reg_tex(8, (const tex_t *)&t8, NULL);
    id<MTLBuffer> b15 = table();
    uint64_t s8 = ((const tex_t *)&t8)->serial;
    pthread_mutex_lock(&g_smu); id<MTLTexture> v8 = hm_get_id(&g_by_serial, &s8); pthread_mutex_unlock(&g_smu);
    id<MTLTexture> base8 = v8.parentTexture;
    CHECK(v8 && base8 && entry(b15, 8) != dummy_of(b15), "ID 8's serial resolves to a view of the shared texture");
    table();
    mtl_reg_tex(7, NULL, NULL); h_nvnTextureFinalize(&t7);
    for (int i = 0; i <= ALIAS_IDLE; i++) { mtl_sub_begin(); mtl_wait(mtl_sub_commit(), 5000000000ull); }   // until the sharing table no longer spares the entry
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32F);
    CHECK(h_nvnTextureInitialize(&t9, &tb), "texture 9 over the same storage initialized");
    mtl_reg_tex(9, (const tex_t *)&t9, NULL);
    builds = g_stat_pool_builds; taken = g_alias_taken; ep = g_treg_ep;
    id<MTLBuffer> b16 = table();
    pthread_mutex_lock(&g_smu); id<MTLTexture> now8 = hm_get_id(&g_by_serial, &s8); int base_shared = CFSetContainsValue(g_alias_set, (__bridge const void *)base8), base_held = serial_table_holds((__bridge void *)base8); pthread_mutex_unlock(&g_smu);
    CHECK(base_held && g_alias_taken == taken && base_shared, "the twin's entry holds its view's parent, and texture 9 over the same storage leaves it shared");
    CHECK(now8 == v8 && g_treg_ep == ep, "ID 8's serial keeps its view, and no ID went down the slow path");
    CHECK(g_stat_pool_builds == builds && entry(b16, 8) == entry(b15, 8) && entry(b16, 9) != dummy_of(b16), "no table is built: the patch keeps ID 8's view and gives ID 9 its texture");
    same_as_whole(b16, "the patched table holds what a table built whole does (a texture over a live twin's base)");
    for (int i = 0; i <= ALIAS_IDLE; i++) { mtl_sub_begin(); mtl_wait(mtl_sub_commit(), 5000000000ull); }
    defer_drain();
    pthread_mutex_lock(&g_res_mu); int base_res = [g_res_objects containsObject:base8]; pthread_mutex_unlock(&g_res_mu);
    id<MTLBuffer> b17 = table();
    CHECK(!base8.heap && base_res && entry(b17, 8) == entry(b15, 8), "submissions later the table still names the view, and its parent, an allocation of its own, is resident");
    // The twin finalized and unregistered: nothing holds the base, and the next texture of another description over the storage takes it
    mtl_reg_tex(8, NULL, NULL); h_nvnTextureFinalize(&t8);
    for (int i = 0; i <= ALIAS_IDLE; i++) { mtl_sub_begin(); mtl_wait(mtl_sub_commit(), 5000000000ull); }
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RG16F);
    CHECK(h_nvnTextureInitialize(&t10, &tb), "texture 10 over the same storage initialized");
    mtl_reg_tex(7, (const tex_t *)&t10, NULL);
    builds = g_stat_pool_builds; taken = g_alias_taken; ep = g_treg_ep;
    id<MTLBuffer> b18 = table();
    pthread_mutex_lock(&g_smu); base_shared = CFSetContainsValue(g_alias_set, (__bridge const void *)base8); base_held = serial_table_holds((__bridge void *)base8); pthread_mutex_unlock(&g_smu);
    CHECK(!base_held && g_alias_taken > taken && !base_shared, "with the twin finalized, texture 10 took the storage over and evicted the base");
    CHECK(g_stat_pool_builds == builds && g_treg_ep == ep && entry(b18, 8) == dummy_of(b18) && entry(b18, 7) != dummy_of(b18) && entry(b18, 9) != dummy_of(b18),
          "no table is built and no ID went down the slow path: the takeover's note names no ID");
    same_as_whole(b18, "the patched table holds what a table built whole does (a finalized twin's base taken over)");

    mtl_reg_tex(1, NULL, NULL); mtl_reg_tex(2, NULL, NULL); mtl_reg_tex(5, NULL, NULL); mtl_reg_tex(6, NULL, NULL); mtl_reg_tex(7, NULL, NULL); mtl_reg_tex(9, NULL, NULL);
    mtl_tex_release(&t2); h_nvnTextureFinalize(&t1); h_nvnTextureFinalize(&t5); h_nvnTextureFinalize(&t9); h_nvnTextureFinalize(&t10);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&dpool); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&epool);
    mtl_pool_del(mb); mtl_pool_del(mc);
    defer_drain(); munmap(ma, LEN); munmap(mb, LEN); munmap(mc, LEN); munmap(md, LEN); munmap(me, LEN);
    printf("t_pool_notes: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
