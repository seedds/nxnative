// Memory pressure frees the Metal copies of uploaded textures nothing used for 30 s (pkg_evict_idle): the copy leaves the residency set, its texture ID's pool table entry reads the dummy,
// a pool walk does not make it again up front, and a shader's next read through the pool does. A texture used lately, one the GPU wrote and a pinned one stay. On small devices a
// sweep every PKG_SWEEP_S frees them without pressure (pkg_sweep)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
static uint32_t clock_s(void) { return (uint32_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull) + 1; }
enum { NT = 4, ID0 = 401 };   // 0 idle, 1 used lately, 2 written by the GPU, 3 pinned
static uint64_t entry(int id_, uint64_t *dummy) {   // the pool table's entry for id_ after a walk, in a submission of its own
    __unsafe_unretained id<MTLBuffer> tb = nil; uint64_t v;
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, 0, 0, &tb);
    const uint64_t *e = (const uint64_t *)((const uint8_t *)tb.contents + DMSH_POOL_HEAD); uint32_t n = *(const uint32_t *)tb.contents;
    v = e[id_]; if (dummy) *dummy = e[n];
    os_unfair_lock_unlock(&g_rmu); finish();
    return v;
}
int main(void) { @autoreleasepool {
    mtl_init();
    size_t one = 1u << 20, poolsize = NT * one; uint8_t *mem = NULL; if (posix_memalign((void **)&mem, getpagesize(), poolsize)) abort(); memset(mem, 0x5a, poolsize);
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = poolsize, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, poolsize); nvn_dirty_add_range(mem, poolsize);
    tex_t t[NT];
    for (int i = 0; i < NT; i++) {
        t[i] = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = 88100 + (uint64_t)i, .pool = &pool, .mem = mem + (size_t)i * one, .off = (int64_t)(i * one)};
        t[i].d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 256, .height = 256, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&t[i].d, &t[i].L);
        mtl_tex_init(&t[i]); mtl_reg_tex(ID0 + i, &t[i], NULL);
    }
    (void)pool_seen_gpu();
    // A shader reads all four through the pool: each is made
    for (int i = 0; i < NT; i++) ((uint32_t *)g_seen_buf.contents)[(ID0 + i) / 32] |= 1u << ((ID0 + i) % 32);
    uint64_t dummy = 0, made[NT];
    (void)entry(ID0, &dummy);
    for (int i = 0; i < NT; i++) { made[i] = entry(ID0 + i, NULL); CHECK("a read texture is made", made[i] && made[i] != dummy); }
    NvPkg *pk[NT]; __weak id<MTLTexture> copy[NT];
    for (int i = 0; i < NT; i++) { pk[i] = tex_get(t[i].serial); CHECK("uploaded as a package", [pk[i] isKindOfClass:[NvPkg class]] && pk[i]->tex && resident(pk[i]->tex)); copy[i] = pk[i]->tex; }
    pthread_mutex_lock(&g_gws_mu); gws_set((uintptr_t)t[2].mem, (uintptr_t)t[2].mem + t[2].L.size); pthread_mutex_unlock(&g_gws_mu);   // the GPU wrote texture 2's storage
    mtl_tex_pin(t[3].serial);
    // 40 s since anything used 0, 2 and 3; 1 was used just now
    uint32_t old = clock_s() - 40;
    for (int i = 0; i < NT; i++) { g_tid_used[ID0 + i] = i == 1 ? clock_s() : old; pk[i]->used_s = i == 1 ? clock_s() : old; }
    uint64_t was_n = g_evict_n;
    pk[0] = pk[1] = pk[2] = pk[3] = nil;
    atomic_store(&g_mp_want, 2);
    mtl_sub_begin(); finish();   // the trim runs at the submission's start
    CHECK("one texture freed", g_evict_n == was_n + 1);
    CHECK("the idle texture's serial no longer resolves to its copy", tex_get(t[0].serial) == nil);
    CHECK("the used, GPU-written and pinned ones stay", tex_get(t[1].serial) && tex_get(t[2].serial) && tex_get(t[3].serial));
    CHECK("its texture ID is marked", g_treg[ID0].evicted && !g_treg[ID0].view && !pool_seen(ID0));
    mtl_sub_begin(); finish(); mtl_drain();
    @autoreleasepool { id<MTLTexture> c = copy[0]; CHECK("the freed copy left the residency set", !c || !resident(c)); }
    @autoreleasepool { id<MTLTexture> c = copy[1]; CHECK("the used one is still resident", c && resident(c)); }
    // The walk leaves it the dummy (it would make a texture this small up front), the others keep theirs
    CHECK("the table entry reads the dummy", entry(ID0, NULL) == dummy);
    CHECK("again at the next walk", entry(ID0, NULL) == dummy && tex_get(t[0].serial) == nil);
    CHECK("the used texture's entry is unchanged", entry(ID0 + 1, NULL) == made[1]);
    // A shader reads it: made again at the next scan
    ((uint32_t *)g_seen_buf.contents)[ID0 / 32] |= 1u << (ID0 % 32);
    uint64_t back = entry(ID0, NULL);
    CHECK("a read makes it again", back && back != dummy && [tex_get(t[0].serial) isKindOfClass:[NvPkg class]]);
    // Without memory pressure nothing goes
    for (int i = 0; i < NT; i++) g_tid_used[ID0 + i] = old;
    was_n = g_evict_n; mtl_sub_begin(); finish();
    CHECK("no pressure, no eviction", g_evict_n == was_n && tex_get(t[0].serial));
    // A small device's sweep (pkg_sweep): the same texture, idle again, goes without pressure; the next sweep waits PKG_SWEEP_S
    for (int i = 0; i < NT; i++) { g_tid_used[ID0 + i] = i == 1 ? clock_s() : old; NvPkg *p = tex_get(t[i].serial); if ([p isKindOfClass:[NvPkg class]]) p->used_s = i == 1 ? clock_s() : old; }
    g_sweep_on = 1; g_sweep_last = 0; was_n = g_evict_n; uint64_t sweep0 = g_sweep_n;
    mtl_sub_begin(); finish();
    CHECK("a small device's sweep frees the idle texture without pressure", g_evict_n == was_n + 1 && g_sweep_n == sweep0 + 1 && tex_get(t[0].serial) == nil);
    CHECK("and leaves the used, GPU-written and pinned ones", tex_get(t[1].serial) && tex_get(t[2].serial) && tex_get(t[3].serial));
    ((uint32_t *)g_seen_buf.contents)[ID0 / 32] |= 1u << (ID0 % 32);
    CHECK("a read makes it again", entry(ID0, NULL) != dummy && [tex_get(t[0].serial) isKindOfClass:[NvPkg class]]);
    { NvPkg *p = tex_get(t[0].serial); if (p) p->used_s = old; } g_tid_used[ID0] = old;
    was_n = g_evict_n; mtl_sub_begin(); finish();
    CHECK("the next sweep waits PKG_SWEEP_S", g_evict_n == was_n && tex_get(t[0].serial));
    g_sweep_on = 0;
    mtl_tex_unpin(t[3].serial);
    for (int i = 0; i < NT; i++) { mtl_reg_tex(ID0 + i, NULL, NULL); mtl_tex_release(&t[i]); }
    mtl_drain(); mtl_alias_drop_pool(&pool); nvn_dirty_remove_range(mem); mtl_pool_del(mem); mtl_drain(); free(mem);
    printf("t_pkg_evict: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
