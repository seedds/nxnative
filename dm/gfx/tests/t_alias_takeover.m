// The sharing table's takeover (nvn_owner.m, own_alias_take_over_locked; own_alias_publish runs it): when a new shared texture takes over storage, an entry a live texture object still resolves to stays, and one nothing resolves to any more goes once
// no lookup has found it for ALIAS_IDLE submissions. Whether a live object resolves to it is one lookup in g_by_serial_held, which every writer of g_by_serial keeps (serial_put,
// serial_del, tex_put and the upgrade's in-place replacement): two serials naming one object count twice and the object stays held until both go. Last, a takeover past 1,000
// held entries with 20,000 serials in the table, which walked the serial table for each entry before (it prints the time).
// The candidates come from the owner's index by pool and 64 KB cell, which its writers keep (own_alias_put_locked, own_alias_del_locked, the takeover, a pool drop): an entry that starts in an earlier cell is found, one in the same
// cell that ends before the new texture is not, one that spans several of the new texture's cells goes once, another pool's entries are not met, and a removal or a pool drop
// leaves no cell behind. Last, a takeover with 100,000 entries in the table, which walked the table's whole capacity before (it prints the time).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static tex_t tex(uint64_t serial, NVNformat format, int w) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = w, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static int shared(id<MTLTexture> t) { pthread_mutex_lock(&g_smu); int r = own_keeps_locked(t); pthread_mutex_unlock(&g_smu); return r; }
static int held(id o) { pthread_mutex_lock(&g_smu); int r = serial_table_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
static alias_key akey(const void *pool, int64_t off, NVNformat format, int w) { alias_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)pool; k.off = off; k.d = tex(0, format, w).d; return k; }
static int64_t ksize(const alias_key *k) { nvn_texlayout L; nvn_tex_layout(&k->d, &L); return (int64_t)L.size; }
static void idle_put(const alias_key *k, id o) { own_alias_put_locked(k, o, 0); }   // an entry no lookup found lately. Under g_smu
static void seen_put(const alias_key *k, id o) { own_alias_put_locked(k, o, own_hook_submission()); }   // one looked up in this submission. Under g_smu
static own_alias_stats ast(void) { return own_alias_stats_locked(); }   // under g_smu
static int consistent(void) { char why[256] = ""; int ok = own_check_locked(why, sizeof why); if (!ok) fprintf(stderr, "owner: %s\n", why); return ok; }   // under g_smu
static int has(const alias_key *k) { return own_alias_get_locked(k) != nil; }
static void next_submission(void) { CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completes"); mtl_sub_begin(); }

int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    tex_t a = tex(91001, NVN_FORMAT_RGBA8, 64), b = tex(91002, NVN_FORMAT_R32F, 64), c = tex(91003, NVN_FORMAT_RG16F, 64);
    mtl_tex_init(&a); mtl_tex_init(&b); mtl_tex_init(&c);
    id<MTLTexture> ta = tex_mtl(&a);
    CHECK(ta && shared(ta) && held(ta), "a live object's shared texture is counted as held");
    id<MTLTexture> tb = tex_mtl(&b);
    CHECK(tb && tb != ta && shared(ta) && shared(tb), "another description over the storage leaves the texture a live object resolves to");
    mtl_tex_release(&a);
    CHECK(!held(ta) && shared(ta), "the finalized object's texture is no longer held, and stays shared until its storage is taken");
    for (int i = 0; i <= ALIAS_IDLE; i++) next_submission();
    id<MTLTexture> tc = tex_mtl(&c);
    CHECK(tc && !shared(ta), "a texture nothing resolves to goes when another description takes its storage");
    CHECK(shared(tb) && shared(tc), "the live objects' textures stay");

    // Two serials naming one object: held until both go
    id o = [NSObject new]; uint64_t s1 = (1ull << 62) + 1, s2 = s1 + 1;
    tex_put(s1, o); tex_put(s2, o);
    serial_del(s1);
    CHECK(held(o), "an object two serials name stays held when one of them goes");
    serial_del(s2);
    CHECK(!held(o), "and is not held once both have gone");
    tex_put(s1, o); serial_put(s1, tb);
    CHECK(!held(o) && held(tb), "replacing a serial's object moves the count");
    serial_del(s1);

    {   // The cost: 1,000 held entries over the storage of a 1024 x 1024 texture, 20,000 serials in the table
        enum { NS = 20000, NA = 1000 };
        NSMutableArray *keep = [NSMutableArray array];
        pthread_mutex_lock(&g_smu);
        for (int i = 0; i < NA; i++) {
            id e = [NSObject new]; [keep addObject:e];
            alias_key k = akey(NULL, (int64_t)i * 4096, NVN_FORMAT_RGBA8, 32);
            idle_put(&k, e);   // not looked up lately, so only the serial table keeps it
        }
        pthread_mutex_unlock(&g_smu);
        for (int i = 0; i < NS; i++) tex_put((1ull << 62) + 100 + i, i < NA ? keep[i] : [NSObject new]);
        tex_t big = tex(91010, NVN_FORMAT_RGBA8, 1024); alias_key nk; memset(&nk, 0, sizeof nk); nk.d = big.d;
        pthread_mutex_lock(&g_smu);
        uint32_t before = ast().n; uint64_t t0 = bp_now();
        own_alias_take_over_locked(&nk);
        double ms = (bp_now() - t0) / 1e6; uint32_t after = ast().n, nser = own_stats_locked().live;
        pthread_mutex_unlock(&g_smu);
        printf("a takeover past %d held entries with %u serials in the table: %.3f ms\n", NA, nser, ms);
        CHECK(after == before, "held entries are kept");
        CHECK(ms < 1, "the takeover asks each entry in one lookup (under 1 ms; walking the serial table for each took 4.3 ms on an M5 Max)");
        for (int i = 0; i < NS; i++) serial_del((1ull << 62) + 100 + i);
    }
    {   // The index: which entries a takeover meets
        static pool_t pl, other; const void *P = &pl, *Q = &other;   // zeroed pools: only their addresses are used
        NSMutableArray *keep = [NSMutableArray array]; id (^obj)(void) = ^{ id e = [NSObject new]; [keep addObject:e]; return e; };
        pthread_mutex_lock(&g_smu);
        uint32_t cells0 = ast().cells, n0 = ast().n;
        alias_key big = akey(P, 0, NVN_FORMAT_RGBA8, 512); int64_t bs = ksize(&big), last = (bs - 1) >> ALIAS_CELL;
        CHECK(last >= 4, "the large texture spans several cells");
        idle_put(&big, obj());
        CHECK(ast().cells == cells0 + (uint32_t)last + 1, "an entry lies in every cell its bytes reach");
        alias_key far = akey(Q, 0, NVN_FORMAT_RGBA8, 512); idle_put(&far, obj());   // the same bytes of another pool
        alias_key tail = akey(P, last << ALIAS_CELL, NVN_FORMAT_R32F, 16);
        CHECK(ksize(&tail) <= 0x8000 && bs > tail.off, "the small texture fits half a cell and lies in the large one's last");
        own_alias_take_over_locked(&tail); idle_put(&tail, obj());
        CHECK(!has(&big), "an entry that starts in an earlier cell and reaches into the new texture's goes");
        CHECK(has(&far), "another pool's entry over the same offsets stays");
        CHECK(ast().cells == cells0 + (uint32_t)last + 1 + 1, "the entry taken over left every cell it lay in");
        alias_key after = akey(P, tail.off + 0x8000, NVN_FORMAT_RGBA8, 16);
        own_alias_take_over_locked(&after); idle_put(&after, obj());
        CHECK(has(&tail), "an entry in the same cell that ends before the new texture stays");
        alias_key before = akey(P, tail.off, NVN_FORMAT_RGBA8, 16);   // the small one's bytes under another description
        own_alias_take_over_locked(&before); idle_put(&before, obj());
        CHECK(!has(&tail) && has(&after), "and goes when its bytes are taken");
        alias_key big2 = akey(P, 0, NVN_FORMAT_R32F, 512); int over = after.off < ksize(&big2);   // its last cell holds the two small entries
        own_alias_take_over_locked(&big2); idle_put(&big2, obj());
        CHECK(!has(&before) && has(&after) == !over, "a larger texture takes the entries in its cells that it overlaps");
        alias_key big3 = akey(P, 0, NVN_FORMAT_RG16F, 512); uint64_t t2 = ast().taken; uint32_t n2 = ast().n;
        own_alias_take_over_locked(&big3);
        CHECK(!has(&big2) && ast().taken - t2 == n2 - ast().n && n2 - ast().n == 1u + (has(&after) ? 0 : !over), "an entry that spans several of the new texture's cells is taken once");
        seen_put(&big3, obj());
        pthread_mutex_unlock(&g_smu);
        mtl_alias_drop_pool(P);
        pthread_mutex_lock(&g_smu);
        CHECK(!has(&big3) && !has(&after) && has(&far), "a pool drop takes that pool's entries only");
        CHECK(ast().cells == cells0 + (uint32_t)((ksize(&far) - 1) >> ALIAS_CELL) + 1, "and leaves none of its cells");
        own_alias_del_locked(&far);
        CHECK(ast().cells == cells0 && ast().n == n0, "a removal leaves the index as it was");
        CHECK(!own_alias_del_locked(&far), "removing an entry that is not there does nothing");
        CHECK(consistent(), "the index, the set and the last lookups agree with the table");
        pthread_mutex_unlock(&g_smu);
    }
    {   // The cost: 100,000 entries in the table, one per cell of a pool, and a texture over 16 of them that were looked up lately (so they stay)
        enum { NA = 100000 };
        static pool_t pl; const void *P = &pl;
        NSMutableArray *keep = [NSMutableArray arrayWithCapacity:NA];
        pthread_mutex_lock(&g_smu);
        for (int i = 0; i < NA; i++) { id e = [NSObject new]; [keep addObject:e]; alias_key k = akey(P, (int64_t)i << ALIAS_CELL, NVN_FORMAT_RGBA8, 32); seen_put(&k, e); }
        alias_key nk = akey(P, 5000ll << ALIAS_CELL, NVN_FORMAT_R32F, 512);
        uint32_t before = ast().n; uint64_t t0 = bp_now();
        for (int i = 0; i < 100; i++) own_alias_take_over_locked(&nk);
        double ms = (bp_now() - t0) / 1e6 / 100; uint32_t after = ast().n;
        printf("a takeover with %u entries in the table (capacity %u): %.4f ms\n", ast().n, ast().cap, ms);
        CHECK(after == before, "entries looked up lately are kept");
        CHECK(ms < 0.1, "the takeover reads only its own cells (under 0.1 ms; walking the table took 0.65 ms on an M6)");
        pthread_mutex_unlock(&g_smu);
        mtl_alias_drop_pool(P);
        pthread_mutex_lock(&g_smu); CHECK(ast().n == before - NA, "the pool's entries go with it"); pthread_mutex_unlock(&g_smu);
    }
    mtl_tex_release(&b); mtl_tex_release(&c);
    next_submission(); defer_drain();
    printf("t_alias_takeover: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
