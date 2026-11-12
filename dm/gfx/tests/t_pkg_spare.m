// A packaged texture's spare (NvPkg): the texture a re-upload replaced stays resident for a quick second re-upload, and leaves the residency set once it has waited
// g_spare_idle_ns with no re-upload taking it, or at a memory pressure trim, after the submissions that used it completed (spare_trim)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
static void write_content(tex_t *t, void *mem, size_t n, unsigned round) {
    dm_guest_write_prepare(mem, n);
    uint32_t *p = malloc((size_t)t->d.width * t->d.height * 4);
    for (unsigned i = 0; i < t->d.width * t->d.height; i++) p[i] = i * 2654435761u + round;
    CHECK("swizzle succeeds", !nvn_swizzle(&t->d, 0, 0, p, t->d.width * 4, t->mem));
    free(p);
}
// One packaged texture re-uploaded once; then `how` lets its spare go: 0 idle past the limit, 1 a memory pressure trim, 2 a re-upload soon after (kept and reused)
static void run(int how) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1}; static uint64_t serial = 77100; t.serial = serial++;
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 256, .height = 256, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&t.d, &t.L);
    size_t poolsize = (t.L.size + 4096 + getpagesize() - 1) & ~(size_t)(getpagesize() - 1); void *mem = NULL; if (posix_memalign(&mem, getpagesize(), poolsize)) abort(); memset(mem, 0, poolsize);
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = poolsize, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED}; t.pool = &pool; t.mem = (uint8_t *)mem + 4096;
    mtl_pool_add(mem, poolsize); nvn_dirty_add_range(mem, poolsize);
    write_content(&t, mem, poolsize, 0);
    mtl_sub_begin(); id<MTLTexture> first = tex_mtl(&t); finish();
    CHECK("first upload resolves", first != nil);
    write_content(&t, mem, poolsize, 1);
    uint64_t re = g_pkg_reuploads;
    mtl_sub_begin(); id<MTLTexture> second = tex_mtl(&t); finish();
    CHECK("a write re-uploads", g_pkg_reuploads == re + 1 && second && second != first);
    NvPkg *pk = tex_get(t.serial);
    CHECK("the package keeps the old texture as its spare", [pk isKindOfClass:[NvPkg class]] && pk->spare == first);
    CHECK("the spare is resident", resident(first));
    __weak id<MTLTexture> weak_first = first; first = nil;
    if (how == 2) {
        write_content(&t, mem, poolsize, 2);
        mtl_sub_begin(); id<MTLTexture> third = tex_mtl(&t); finish();
        CHECK("a re-upload within the limit reuses the spare", third == weak_first);
        CHECK("and the second texture becomes the spare", pk->spare == second);
    } else {
        uint64_t trimmed = g_spare_trimmed;
        if (how == 0) { g_spare_idle_ns = 0; g_spare_last = 0; } else atomic_store(&g_mp_want, 2);
        mtl_sub_begin(); finish();
        CHECK("the spare was retired", g_spare_trimmed > trimmed && pk->spare == nil);   // a pressure trim also takes the earlier case's spares
        mtl_sub_begin(); finish();   // its submission completed: the next start drains it
        mtl_drain();
        @autoreleasepool { id<MTLTexture> f = weak_first; CHECK("the retired spare left the residency set", !f || !resident(f)); }
        CHECK("the current texture stays resident", resident(second) && pk->tex == second);
        CHECK("the current texture still resolves", tex_mtl(&t) == second);
        g_spare_idle_ns = 3000000000ull;
        write_content(&t, mem, poolsize, 3);
        mtl_sub_begin(); id<MTLTexture> third = tex_mtl(&t); finish();
        CHECK("the next re-upload makes a new texture", third && third != second && pk->spare == second);
    }
    pk = nil; second = nil;
    mtl_tex_release(&t); mtl_drain(); mtl_alias_drop_pool(&pool); nvn_dirty_remove_range(mem); mtl_pool_del(mem); mtl_drain(); free(mem);
}
int main(void) { @autoreleasepool {
    mtl_init();
    for (int how = 0; how < 3; how++) run(how);
    printf("t_pkg_spare: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
