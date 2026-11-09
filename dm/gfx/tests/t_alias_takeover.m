// alias_take_over (nvn_mtl.m): when a new shared texture takes over storage, an entry a live texture object still resolves to stays, and one nothing resolves to any more goes once
// no lookup has found it for ALIAS_IDLE submissions. Whether a live object resolves to it is one lookup in g_by_serial_held, which every writer of g_by_serial keeps (serial_put,
// serial_del, tex_put and the upgrade's in-place replacement): two serials naming one object count twice and the object stays held until both go. Last, a takeover past 1,000
// held entries with 20,000 serials in the table, which walked the serial table for each entry before (it prints the time).
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
static int shared(id<MTLTexture> t) { pthread_mutex_lock(&g_smu); int r = alias_has(t); pthread_mutex_unlock(&g_smu); return r; }
static int held(id o) { pthread_mutex_lock(&g_smu); int r = serial_table_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
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
            alias_key k; memset(&k, 0, sizeof k); k.off = (int64_t)i * 4096; k.d = tex(0, NVN_FORMAT_RGBA8, 32).d;
            hm_put_id(&g_alias, &k, e);
        }
        pthread_mutex_unlock(&g_smu);
        for (int i = 0; i < NS; i++) tex_put((1ull << 62) + 100 + i, i < NA ? keep[i] : [NSObject new]);
        tex_t big = tex(91010, NVN_FORMAT_RGBA8, 1024); alias_key nk; memset(&nk, 0, sizeof nk); nk.d = big.d;
        pthread_mutex_lock(&g_smu);
        uint32_t before = g_alias.n; uint64_t t0 = bp_now();
        alias_take_over(&nk);
        double ms = (bp_now() - t0) / 1e6; uint32_t after = g_alias.n;
        pthread_mutex_unlock(&g_smu);
        printf("a takeover past %d held entries with %u serials in the table: %.3f ms\n", NA, g_by_serial.n, ms);
        CHECK(after == before, "held entries are kept");
        CHECK(ms < 1, "the takeover asks each entry in one lookup (under 1 ms; walking the serial table for each took 4.3 ms on an M5 Max)");
        for (int i = 0; i < NS; i++) serial_del((1ull << 62) + 100 + i);
    }
    mtl_tex_release(&b); mtl_tex_release(&c);
    next_submission(); defer_drain();
    printf("t_alias_takeover: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
