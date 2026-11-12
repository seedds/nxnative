// The NVN resource owner (nvn_owner.m) on its own: linked without the replay layer, whose four hooks are recorded or set here instead (what is dropped and whether it stays resident,
// which object's last count went, which serial changed, the submission being encoded). What it holds to, as the tables behaved before they moved out of nvn_mtl.m:
// a live serial's registration, replacement and removal; where tex_put sends a serial (live table for a live texture object or an external texture from 2^62 up, ghost table for a
// finalized one); a finalize, and a copy's finalize that ends nothing; ghost replacement in place; the ghost table kept by use (past NGHOST the least recently used entry goes once
// idle, by frames or, with the frame standing still, by submissions; a working set larger than NGHOST stays while used and drains GHOST_TRIM a commit once idle); texture pins and
// program pins; a view's entry counting its parent; renaming an object everywhere; and after every step the held counts against the tables (own_check_locked). The owner releases
// nothing itself: everything it lets go goes to own_hook_drop, which nvn_mtl.m releases once the submission in flight completes, so what a lookup returned outlives the change that
// dropped it. Each ghost serial has one place in the list of use.
// sRGB / linear views (tex_mtl_'s twins): the old sequence, publish then serial_del when the parent turned out gone, left a finalized serial resolving to the stale view (the evidence,
// kept as a case); own_view_put publishes only while the parent is shared, and own_view_drop_stale takes a stale view's live or ghost entry (with its place in the list) only while it still
// names the view, in each order of finalize, publish, the parent leaving and resolution.
// The sharing table (own_alias_*): a lookup publishes in its own hold, a registration takes the storage over, goes in and publishes in one hold (and a second registration under the
// key takes the first), the takeover spares entries a live entry or pin holds or a lookup found within ALIAS_IDLE submissions and takes the rest (a ghost entry keeps its texture
// resolving), a pool drop takes that pool's entries, an upgrade renames the sharing table with the serial tables and publishes the asking serial's view in the same hold; a pool drop
// or an upgrade between a twin's lookup and its publication refuses it; own_check_locked also checks the sharing table's index, set and last lookups
#import <Metal/Metal.h>
#include "../nvn/nvn_owner.h"
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

// The replay layer's side, recorded
static NSMutableArray *g_dropped;   // what the owner let go, kept alive here as the deferred release keeps it until the submission completes
static NSMutableArray<NSNumber *> *g_dropped_keep;
// The owner's change generation and its last note, read as the replay layer's caches read them
static uint64_t last_note(void) { uint64_t s = ~0ull; own_note_at(atomic_load(&g_ser_gen), &s); return s; }
#define g_changed_n ((int)atomic_load(&g_ser_gen))
#define g_changed_last (last_note())
static uint64_t g_sub = 1;          // the submission being encoded
static uint64_t g_frame = 1;        // the frame being replayed
void own_hook_drop(id o, int keep_resident) { [g_dropped addObject:o]; [g_dropped_keep addObject:@(keep_resident)]; }
uint64_t own_hook_submission(void) { return g_sub; }
uint64_t own_hook_frame(void) { return g_frame; }
static int dropped(id o, int keep) { for (NSUInteger i = 0; i < g_dropped.count; i++) if (g_dropped[i] == o && g_dropped_keep[i].intValue == keep) return 1; return 0; }
static int ndropped(id o, int keep) { int n = 0; for (NSUInteger i = 0; i < g_dropped.count; i++) n += g_dropped[i] == o && g_dropped_keep[i].intValue == keep; return n; }
static int retire(id o) { pthread_mutex_lock(&g_smu); int r = own_retire_locked(o); pthread_mutex_unlock(&g_smu); return r; }   // the drain's question, once the fence has passed
static uint32_t waiting(void) { pthread_mutex_lock(&g_smu); uint32_t n = own_stats_locked().retire_wait; pthread_mutex_unlock(&g_smu); return n; }

static int live(uint64_t k) { return serial_get(k) != nil; }
static int ghost(uint64_t k) { __block int r = 0; pthread_mutex_lock(&g_smu); own_each_locked(^(uint64_t s, id o, int g) { (void)o; if (s == k && g) r = 1; }); pthread_mutex_unlock(&g_smu); return r; }
static uint32_t ghost_n(void) { pthread_mutex_lock(&g_smu); uint32_t n = own_stats_locked().ghost; pthread_mutex_unlock(&g_smu); return n; }
static uint64_t ghost_puts(void) { pthread_mutex_lock(&g_smu); uint64_t n = own_stats_locked().ghost_puts; pthread_mutex_unlock(&g_smu); return n; }
static void idle(void) { g_frame += GHOST_IDLE; }   // every ghost entry unused for GHOST_IDLE frames
static void trim_all(void) { for (int i = 0; i < 4096 && ghost_n() > NGHOST; i++) own_ghost_trim(); }   // commits until the idle entries past NGHOST have gone
static int holds(id o) { pthread_mutex_lock(&g_smu); int r = entry_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
static int live_holds(id o) { pthread_mutex_lock(&g_smu); int r = serial_table_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
static void step(const char *label) {
    char why[256] = ""; pthread_mutex_lock(&g_smu); int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
    checks++; if (!ok) { failed++; fprintf(stderr, "FAIL: held counts against the tables after %s: %s\n", label, why); }
}
static tex_t texture(uint64_t serial) { return (tex_t){.magic = MAGIC('T','E','X',0), .serial = serial}; }
static void tex_live(tex_t *t) { pthread_mutex_lock(&g_smu); own_tex_live_put_locked(t); pthread_mutex_unlock(&g_smu); }   // mtl_tex_init's part
static int tex_end(tex_t *t) { pthread_mutex_lock(&g_smu); int r = own_tex_end_locked(t); pthread_mutex_unlock(&g_smu); if (r >= 0) serial_del(t->serial); return r; }   // mtl_tex_release's part
// Sharing-table keys: a 4x4 texture of a format at an offset of a (fake) pool, padding zeroed
static alias_key akey(uint64_t pool, int64_t off, NVNformat f) {
    alias_key k; memset(&k, 0, sizeof k); k.pool = pool; k.off = off;
    k.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = f, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1}; return k;
}
static NSMapTable *g_keys;   // object → the key share() put it under (each object its own 64 KB cell of pool 0x5000)
static alias_key key_of(id o) {
    NSNumber *n = [g_keys objectForKey:o]; if (!n) { n = @(g_keys.count); [g_keys setObject:n forKey:o]; }
    return akey(0x5000, (int64_t)n.intValue << ALIAS_CELL, NVN_FORMAT_RGBA8);
}
static void share(id o, int on) { alias_key k = key_of(o); pthread_mutex_lock(&g_smu); if (on) own_alias_put_locked(&k, o, 0); else own_alias_del_locked(&k); pthread_mutex_unlock(&g_smu); }   // as a registration / a pool drop would
static int shared(id o) { pthread_mutex_lock(&g_smu); int r = own_shared_locked(o); pthread_mutex_unlock(&g_smu); return r; }
static int has(const alias_key *k) { pthread_mutex_lock(&g_smu); int r = own_alias_get_locked(k) != nil; pthread_mutex_unlock(&g_smu); return r; }
static own_alias_stats ast(void) { pthread_mutex_lock(&g_smu); own_alias_stats s = own_alias_stats_locked(); pthread_mutex_unlock(&g_smu); return s; }

int main(void) { @autoreleasepool {
    g_dropped = [NSMutableArray array]; g_dropped_keep = [NSMutableArray array];
    g_keys = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory];
    step("start");

    {   // a live serial: registration, lookup, replacement, removal
        uint64_t k = 93001; id a = [NSObject new], b = [NSObject new];
        int notes = g_changed_n;
        serial_put(k, a);
        CHECK(live(k) && serial_get(k) == a && tex_get(k) == a && live_holds(a), "serial_put: the serial resolves to its object, which is held");
        CHECK(g_changed_n == notes + 1 && g_changed_last == k, "serial_put reports the serial's change");
        step("serial_put");
        __weak id wa = a; a = nil;
        serial_put(k, b);
        CHECK(serial_get(k) == b && live_holds(b) && !live_holds(wa), "replacement: the new object is held, the old one no longer");
        CHECK(dropped(wa, 0) && wa != nil, "replacement: the old object goes to the replay layer's deferred release, not released by the owner");
        CHECK(retire(wa) == 1 && !waiting(), "replacement: nothing holds the old object, so its retirement may take it out of the residency set");
        step("replacement");
        serial_del(k);
        CHECK(!live(k) && !tex_get(k) && !live_holds(b) && dropped(b, 0), "serial_del: nothing resolves the serial, its object is dropped and not held");
        step("serial_del");
        notes = g_changed_n; serial_del(k);
        CHECK(g_changed_n == notes, "serial_del of a serial with no entry changes and reports nothing");
        step("serial_del again");
        id s = [NSObject new]; share(s, 1);
        serial_put(k, s); serial_del(k);
        CHECK(dropped(s, 1), "an object the sharing table holds stays resident when its entry goes");
        share(s, 0);
        CHECK(dropped(s, 0) && !shared(s), "and is retired when the sharing table lets it go");
        step("shared object");
    }

    {   // one object under two serials: held until both go
        id o = [NSObject new]; uint64_t s1 = (1ull << 62) + 93011, s2 = s1 + 1;
        tex_put(s1, o); tex_put(s2, o);
        serial_del(s1);
        CHECK(live_holds(o) && retire(o) == 0 && waiting() == 1, "an object two serials name stays held when one of them goes: a retirement of it waits, the owner keeping it");
        CHECK(retire(o) == 0 && waiting() == 1, "asked again, it waits once");
        int before = ndropped(o, 0);
        serial_del(s2);
        CHECK(!holds(o) && !waiting() && ndropped(o, 0) == before + 2, "once both have gone it is not held, and besides the entry's own drop the owner hands it back to retirement");
        step("two serials");
    }

    {   // where tex_put sends a texture serial, finalizing, and a copy's finalize
        uint64_t ext = (1ull << 62) + 93021, fk = 93023; tex_t lt = texture(93022); tex_live(&lt);
        id e = [NSObject new], l = [NSObject new], f = [NSObject new];
        tex_put(ext, e); tex_put(lt.serial, l); tex_put(fk, f);
        CHECK(live(ext) && !ghost(ext), "an external texture (2^62 and up) goes to the live table");
        CHECK(live(lt.serial) && !ghost(lt.serial) && own_tex_live(lt.serial), "a live texture object's serial goes to the live table");
        CHECK(!live(fk) && ghost(fk) && tex_get(fk) == f && !serial_get(fk), "a finalized serial goes to the ghost table: tex_get resolves it, serial_get (live only) does not");
        CHECK(holds(f) && !live_holds(f), "a ghost entry holds its object, counted apart from the live table");
        step("tex_put");
        pthread_mutex_lock(&g_smu); uint64_t puts = own_stats_locked().ghost_puts; pthread_mutex_unlock(&g_smu);
        id f2 = [NSObject new]; tex_put(fk, f2);
        pthread_mutex_lock(&g_smu); uint64_t puts2 = own_stats_locked().ghost_puts; pthread_mutex_unlock(&g_smu);
        CHECK(tex_get(fk) == f2 && !holds(f) && dropped(f, 0) && puts2 == puts, "a ghost serial put again replaces its object in place, counted as no new put");
        step("ghost replacement");
        tex_t copy = lt;
        CHECK(tex_end(&copy) == -1 && own_tex_live(lt.serial) && live(lt.serial), "a copy's finalize ends nothing while the object the texture was initialized in holds it");
        CHECK(tex_end(&lt) == 1 && !own_tex_live(lt.serial) && !live(lt.serial) && !ghost(lt.serial) && !holds(l), "the object's own finalize ends the texture; it does not move to the ghost table");
        step("finalize");
        tex_t own = texture(93024), moved; tex_live(&own); moved = own; own.magic = 0;   // the object the texture was initialized in no longer holds it (the game moved or freed it)
        CHECK(tex_end(&moved) == 1 && !own_tex_live(moved.serial), "a copy outliving the object it was initialized in ends the texture");
        tex_put(lt.serial, l);
        CHECK(ghost(lt.serial) && tex_get(lt.serial) == l, "the finalized serial put again (a replayed record) goes to the ghost table");
        step("tex_put after finalize");
        serial_del(ext);
    }

    {   // the ghost table: past NGHOST, the least recently used entry goes once it is idle; one in use stays
        enum { BASE = 940000 };
        idle();   // the earlier cases' entries go first
        NSMutableArray *objs = [NSMutableArray array];
        for (int i = 0; i <= NGHOST; i++) { id o = [NSObject new]; [objs addObject:o]; tex_put(BASE + (uint64_t)i, o); }
        CHECK(ghost_n() == NGHOST + 1 && ghost(BASE) && holds(objs[0]), "past NGHOST, entries used within GHOST_IDLE frames stay: the NGHOST + 1st evicts none of them");
        step("ghost table past NGHOST");
        int notes = g_changed_n;
        idle(); tex_get(BASE + 1);   // one of them used again
        tex_put(BASE + NGHOST + 1, [NSObject new]);
        CHECK(!ghost(BASE) && !holds(objs[0]) && dropped(objs[0], 0), "once idle, the least recently used goes at the next put: dropped, no longer held");
        CHECK(ghost(BASE + 1) && !ghost(BASE + 2) && ghost(BASE + 3), "one used again lately is passed over for the next least recently used");
        CHECK(ghost_n() == NGHOST && g_changed_n == notes + 3, "the table is back at NGHOST, with a note for the put and each eviction (their serials resolve anew)");
        own_ghost_trim();
        CHECK(ghost_n() == NGHOST, "a commit evicts nothing at NGHOST");
        step("ghost eviction");
    }
    {   // more finalized serials than NGHOST resolved every frame (Ocarina of Time streaming an area in): none goes while in use and none is put again; once idle they go,
        // GHOST_TRIM a commit at most, until the table is back at NGHOST
        enum { BASE = 941000, N = 4 * NGHOST };
        for (int i = 0; i < N; i++) tex_put(BASE + (uint64_t)i, [NSObject new]);
        uint64_t puts = ghost_puts(); int found = 0;
        for (int f = 0; f < 2 * GHOST_IDLE; f++) {
            g_frame++; g_sub += 25; own_ghost_trim();   // 25 submissions a frame, each commit trimming
            for (int i = 0; i < N; i++) found += tex_get(BASE + (uint64_t)i) != nil;
        }
        CHECK(found == N * 2 * GHOST_IDLE && ghost_puts() == puts, "finalized serials resolved every frame stay past NGHOST: every lookup found, no put");
        CHECK(ghost_n() == N, "and only they: the earlier entries went as they idled");
        step("ghost working set");
        idle(); uint32_t n0 = ghost_n(); int notes = g_changed_n; own_ghost_trim();
        CHECK(n0 - ghost_n() == GHOST_TRIM && g_changed_n - notes == GHOST_TRIM, "once idle, a commit evicts GHOST_TRIM of them and notes as many, no more");
        trim_all();
        CHECK(ghost_n() == NGHOST && ghost(BASE + N - 1) && !ghost(BASE), "commits take the table back to NGHOST, least recently used first");
        step("ghost working set idle");
    }
    {   // with the frame standing still (no present), GHOST_IDLE_SUBS submissions without a use make an entry idle
        enum { BASE = 946000, EXTRA = 64 };
        for (int i = 0; i < NGHOST + EXTRA; i++) tex_put(BASE + (uint64_t)i, [NSObject new]);   // the last case's entries, idle, go as these come
        CHECK(ghost_n() == NGHOST + EXTRA && ghost(BASE), "entries put in this frame and submission all stay");
        g_sub += GHOST_IDLE_SUBS - 1; own_ghost_trim();
        CHECK(ghost_n() == NGHOST + EXTRA, "not idle one submission short of GHOST_IDLE_SUBS");
        g_sub++; own_ghost_trim(); own_ghost_trim();
        CHECK(ghost_n() == NGHOST && !ghost(BASE) && ghost(BASE + NGHOST + EXTRA - 1), "idle after GHOST_IDLE_SUBS submissions, the frame unchanged: the table is back at NGHOST");
        step("ghost idle by submissions");
    }

    {   // texture pins
        uint64_t k = (1ull << 62) + 93031; id o = [NSObject new], o2 = [NSObject new];
        tex_put(k, o);
        mtl_tex_pin(k); mtl_tex_pin(k);
        pthread_mutex_lock(&g_smu); int pinned = tex_object_pinned(o); pthread_mutex_unlock(&g_smu);
        CHECK(pinned && live_holds(o), "a pinned object is held and marked pinned (no takeover or retirement drops it)");
        step("pin");
        serial_del(k);
        CHECK(!live(k) && tex_get(k) == o && live_holds(o) && dropped(o, 1), "after its entry goes, the pinned serial still resolves to its object, which stays resident");
        step("pinned serial_del");
        tex_put(k, o2);
        pthread_mutex_lock(&g_smu); int moved = tex_object_pinned(o2) && !tex_object_pinned(o); pthread_mutex_unlock(&g_smu);
        CHECK(tex_get(k) == o2 && moved && !live_holds(o), "tex_put moves the pin to the new object");
        step("pin follows tex_put");
        mtl_tex_unpin(k);
        CHECK(tex_get(k) == o2, "one of two pins goes: the pin stays");
        serial_del(k); mtl_tex_unpin(k);
        CHECK(!tex_get(k) && !holds(o2), "the last unpin of a serial with no entry: it resolves to nothing, its object is not held");
        step("unpin");
        uint64_t k2 = (1ull << 62) + 93032; id p = [NSObject new];
        mtl_tex_pin(k2);
        CHECK(!tex_get(k2), "a pin on a serial with no object yet resolves to nothing");
        tex_put(k2, p); serial_del(k2);
        CHECK(tex_get(k2) == p, "and takes the object tex_put gives the serial later");
        mtl_tex_unpin(k2);
        step("pin before the object");
    }

    {   // program pins (the caller holds the submission lock; this test has one thread)
        uint64_t ps = 93041; id pr = [NSObject new]; prog_t p = {.serial = ps};
        serial_put(ps, pr); mtl_prog_release(&p);
        CHECK(!live(ps), "an unpinned program's release removes its entry at once");
        serial_put(ps, pr); own_prog_pin(ps); own_prog_pin(ps);
        mtl_prog_release(&p);
        CHECK(serial_get(ps) == pr, "a pinned program's release keeps its entry");
        mtl_prog_unpin(&p);
        CHECK(live(ps), "while one pin remains");
        mtl_prog_unpin(&p);
        CHECK(!live(ps), "the last unpin of a released program removes its entry");
        serial_put(ps, pr); own_prog_pin(ps); mtl_prog_unpin(&p);
        CHECK(live(ps), "an unpin of a program never released leaves its entry");
        serial_del(ps);
        step("program pins");
    }

    {   // a view counts its parent; renaming an object moves every name and count
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        id<MTLTexture> base = [dev newTextureWithDescriptor:d], base2 = [dev newTextureWithDescriptor:d];
        id<MTLTexture> view = [base newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
        CHECK(dev && base && base2 && view, "two textures and an sRGB view of the first");
        if (view && base2) {
            uint64_t k = (1ull << 62) + 93051;
            tex_put(k, view);
            CHECK(live_holds(view) && live_holds(base), "an entry holding a view holds its parent too");
            step("view");
            serial_del(k);
            CHECK(!holds(view) && !holds(base), "and lets both go with it");
            step("view removed");
            uint64_t a = (1ull << 62) + 93052, b = 93053, c = (1ull << 62) + 93054;
            tex_put(a, base); tex_put(b, base); mtl_tex_pin(c); tex_put(c, base); serial_del(c);   // a live name, a ghost name and a pin
            pthread_mutex_lock(&g_smu); int named = own_rename_locked(base, base2, b); pthread_mutex_unlock(&g_smu);
            CHECK(named && tex_get(a) == base2 && tex_get(b) == base2 && tex_get(c) == base2, "a rename moves the live name, the ghost name and the pin");
            CHECK(!holds(base) && live_holds(base2) && holds(base2), "and the counts with them");
            step("rename");
            serial_del(a); mtl_tex_unpin(c);
        }
    }

    {   // sRGB / linear views of a shared texture (tex_mtl_'s twins); share() puts the parent in the sharing table and takes it out, as a registration and a pool drop do
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> b = [dev newTextureWithDescriptor:d], b2 = [dev newTextureWithDescriptor:d];
        id<MTLTexture> (^view_of)(id<MTLTexture>) = ^(id<MTLTexture> p) { return [p newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB]; };
        CHECK(b && b2 && view_of(b), "two shared textures and a view of one");
        if (b && b2) {
            alias_key kb = key_of(b);   // the parent's key
            {   // the evidence: the old sequence (publish, find the parent gone, serial_del) on a finalized serial leaves the stale view reachable, as serial_del ends a live entry only
                uint64_t k = 93061; id<MTLTexture> v = view_of(b);
                share(b, 1); tex_put(k, v); share(b, 0);   // the parent leaves on another thread between the lookup and the check
                CHECK(tex_get(k) == v, "old sequence: a reader between the publish and the check resolves the view of a parent already gone");
                serial_del(k);
                CHECK(tex_get(k) == v && ghost(k) && holds(v) && holds(b), "old sequence: after serial_del the finalized serial still resolves to the stale view, which holds its parent");
                step("old twin sequence");
                CHECK(own_view_drop_stale(k, v) == 1 && !tex_get(k) && !ghost(k) && !holds(v) && !holds(b) && dropped(v, 0), "own_view_drop_stale takes the stale view's ghost entry");
                step("old twin sequence cleared");
            }
            {   // own_view_put: published only while the parent is shared
                uint64_t fk = 93062; tex_t lt = texture(93063); tex_live(&lt);
                id<MTLTexture> v = view_of(b);
                int notes = g_changed_n;
                CHECK(own_view_put(fk, &kb, v) == 0 && !tex_get(fk) && !holds(v) && !holds(b) && g_changed_n == notes, "a parent not shared: the view is not published, nothing is held or reported");
                id x = [NSObject new]; tex_put(fk, x);
                CHECK(own_view_put(fk, &kb, v) == 0 && tex_get(fk) == x, "and an entry the serial has stays as it was");
                step("view refused");
                share(b, 1);
                CHECK(own_view_put(fk, &kb, v) == 1 && tex_get(fk) == v && ghost(fk) && holds(b) && !holds(x) && dropped(x, 0), "a shared parent: a finalized serial's view goes to the ghost table in place of its old entry, holding its parent");
                id<MTLTexture> lv = view_of(b);
                CHECK(own_view_put(lt.serial, &kb, lv) == 1 && serial_get(lt.serial) == lv && live_holds(b), "a live texture object's view goes to the live table");
                step("view published");
                CHECK(own_view_drop_stale(fk, v) == 0 && own_view_drop_stale(lt.serial, lv) == 0 && tex_get(fk) == v && tex_get(lt.serial) == lv, "a view whose parent is shared is valid: its entries stay");
                share(b, 0); mtl_tex_pin(fk);
                CHECK(own_view_drop_stale(fk, v) == 0 && tex_get(fk) == v, "a pinned view stays valid after its parent leaves the sharing table");
                mtl_tex_unpin(fk);
                uint64_t pk = (1ull << 62) + 93064; tex_put(pk, b); mtl_tex_pin(pk); serial_del(pk);
                CHECK(own_view_drop_stale(fk, v) == 0, "and so does a view whose parent is pinned");
                mtl_tex_unpin(pk);
                step("valid views");
                // stale-entry rejection: the view a caller resolved is stale, but the serial's entry changed meanwhile: the new entry stays
                id y = [NSObject new]; tex_put(lt.serial, y);
                CHECK(own_view_drop_stale(lt.serial, lv) == 1 && serial_get(lt.serial) == y && live_holds(y), "a stale view whose live entry was replaced meanwhile: the replacement stays");
                tex_t again = texture(fk); tex_live(&again); tex_put(fk, y);   // the serial is live again, and its live entry is another object
                CHECK(own_view_drop_stale(fk, v) == 1 && serial_get(fk) == y && ghost(fk) && tex_get(fk) == y, "with a live entry that is not the view, the ghost entry is not touched (the live one resolves)");
                CHECK(tex_end(&again) == 1, "the serial's object finalizes again");
                CHECK(own_view_drop_stale(fk, v) == 1 && !ghost(fk) && !tex_get(fk) && !holds(v), "then the stale view's ghost entry goes");
                step("stale views");
                CHECK(own_view_drop_stale(lt.serial, y) == 0, "an object that is no view is never stale");
                tex_end(&lt);
                step("views removed");
            }
            {   // orders of finalize, publish, the parent leaving and resolution
                tex_t t = texture(93065); tex_live(&t);
                id<MTLTexture> v = view_of(b);
                share(b, 1);
                CHECK(own_view_put(t.serial, &kb, v) == 1 && serial_get(t.serial) == v, "published live");
                tex_end(&t);
                CHECK(!tex_get(t.serial) && !holds(v) && !holds(b), "finalized: the live entry goes (a finalize, not an invalidation)");
                CHECK(own_view_put(t.serial, &kb, v) == 1 && ghost(t.serial) && tex_get(t.serial) == v, "a replayed record publishes it again: ghost table");
                share(b, 0);
                CHECK(tex_get(t.serial) == v, "the parent leaves after the publish: the entry still names the view until a resolution finds it stale");
                CHECK(own_view_drop_stale(t.serial, v) == 1 && !tex_get(t.serial) && !holds(b), "that resolution takes it");
                CHECK(own_view_put(t.serial, &kb, view_of(b)) == 0 && !tex_get(t.serial), "the parent gone before the publish: nothing is published");
                step("orders");
                // parent replacement (an upgrade): every name of the parent moves to the upgraded texture, which takes its place in the sharing table; a view of the old one is stale
                share(b, 1); tex_t u = texture(93066); tex_live(&u); id<MTLTexture> uv = view_of(b); uint64_t pn = (1ull << 62) + 93067;
                tex_put(pn, b); CHECK(own_view_put(u.serial, &kb, uv) == 1, "a view and its parent's own name");
                pthread_mutex_lock(&g_smu); own_rename_locked(b, b2, pn); id under = own_alias_get_locked(&kb); pthread_mutex_unlock(&g_smu);
                CHECK(tex_get(pn) == b2 && serial_get(u.serial) == uv && live_holds(b), "after the upgrade the parent's name is the new texture; the view still holds the old one");
                CHECK(under == b2 && shared(b2) && !shared(b), "the rename moves the parent's place in the sharing table: its key names the new texture");
                step("parent upgraded");
                CHECK(own_view_drop_stale(u.serial, uv) == 1 && !serial_get(u.serial) && !holds(b), "the view of the old parent is stale: it goes, and the old parent with it");
                CHECK(own_view_put(u.serial, &kb, view_of(b)) == 0, "a view of the old parent made after the rename is refused (its key names the new texture)");
                CHECK(own_view_put(u.serial, &kb, view_of(b2)) == 1 && [(id<MTLTexture>)serial_get(u.serial) parentTexture] == b2, "made again on the upgraded parent");
                tex_end(&u); serial_del(pn); share(b, 0);   // b's key, which names b2 now
                CHECK(!shared(b2) && !has(&kb), "the upgraded parent leaves with its key");
                step("parent upgraded, view made again");
            }
            {   // a ghost entry taken away leaves no place in the list: the serial put again has one place, as the most recently used, and goes in its turn
                uint64_t k = 93070; id<MTLTexture> v = view_of(b); id x = [NSObject new];
                share(b, 1); own_view_put(k, &kb, v); share(b, 0);
                own_view_drop_stale(k, v); tex_put(k, x);
                step("ghost list after a stale view");
                for (int i = 0; i < NGHOST - 1; i++) tex_put(950000 + (uint64_t)i, [NSObject new]);
                idle(); trim_all();
                CHECK(ghost(k) && ghost_n() == NGHOST, "idle, the serial put again stays among the NGHOST most recently used");
                tex_put(950000 + NGHOST, [NSObject new]);
                CHECK(!ghost(k) && ghost(950000) && ghost_n() == NGHOST, "and is the next to go: its place is where its last put left it");
                step("ghost list after a stale view, evicted");
            }
        }
    }

    {   // the sharing table: lookups, registration, takeover, pool drops, upgrades, and their interleavings with finalize and resolution
        enum { P = 0x7000, Q = 0x8000 };
        alias_key ka = akey(P, 0, NVN_FORMAT_RGBA8), kr = akey(P, 0, NVN_FORMAT_R32F), kg = akey(P, 0, NVN_FORMAT_RG16F), kq = akey(Q, 0, NVN_FORMAT_RGBA8);
        id a = [NSObject new], a2 = [NSObject new], r = [NSObject new];
        tex_t s1 = texture(97001), s2 = texture(97002); tex_live(&s1); tex_live(&s2);
        int notes = g_changed_n;
        CHECK(!own_alias_take(s1.serial, &ka) && !tex_get(s1.serial) && g_changed_n == notes, "a lookup of a key with no shared texture finds nothing and publishes nothing");
        CHECK(own_alias_publish(s1.serial, &ka, a) == a && serial_get(s1.serial) == a && shared(a) && live_holds(a), "a registration shares the texture and publishes it under the serial, in one hold");
        CHECK(own_alias_publish(s2.serial, &ka, a2) == a && serial_get(s2.serial) == a && !shared(a2) && !holds(a2), "a second registration under the key (another thread's) takes the first; the caller's own is shared nowhere");
        step("registration");
        tex_t s3 = texture(97003); tex_live(&s3);
        CHECK(own_alias_take(s3.serial, &ka) == a && serial_get(s3.serial) == a, "a lookup publishes the shared texture under the serial");
        tex_t f = texture(97004);   // finalized: no live texture object
        CHECK(own_alias_take(f.serial, &ka) == a && ghost(f.serial) && tex_get(f.serial) == a, "a finalized serial's lookup goes to the ghost table");
        step("lookups");
        // the takeover: another description over the bytes. a is held by live entries: spared whatever its last lookup
        g_sub += ALIAS_IDLE + 1;
        tex_t s4 = texture(97005); tex_live(&s4);
        CHECK(own_alias_publish(s4.serial, &kr, r) == r && has(&ka) && has(&kr), "the takeover spares an entry live serials hold");
        step("takeover spares held");
        tex_end(&s1); tex_end(&s2); tex_end(&s3);
        CHECK(!live_holds(a) && holds(a), "its live serials finalized: only the ghost entry holds it");
        id g = [NSObject new]; tex_t s5 = texture(97006), f2 = texture(97012); tex_live(&s5);
        CHECK(own_alias_take(f2.serial, &ka) == a && ghost(f2.serial), "a replayed record looks it up (a finalized serial: ghost table)");
        pthread_mutex_lock(&g_smu); uint32_t took = own_alias_take_over_locked(&kg); pthread_mutex_unlock(&g_smu);
        CHECK(took == 0 && has(&ka), "an entry no live serial holds is still spared within ALIAS_IDLE submissions of its last lookup");
        g_sub += ALIAS_IDLE;
        uint64_t taken = ast().taken; notes = g_changed_n; [g_dropped removeAllObjects]; [g_dropped_keep removeAllObjects];
        CHECK(own_alias_publish(s5.serial, &kg, g) == g && !has(&ka) && !shared(a) && has(&kg), "past ALIAS_IDLE submissions an entry only a ghost entry holds is taken over");
        CHECK(dropped(a, 0) && tex_get(f.serial) == a && holds(a), "it is retired (out of the residency set unless held: nvn_mtl.m's retirement waits for the ghost entry), and the ghost entry still resolves it");
        CHECK(ast().taken == taken + 1 + !has(&kr) && g_changed_n > notes, "the takeover is counted and reported");
        CHECK(has(&kr), "the entry s4 holds live stays");
        step("takeover");
        // a lookup refreshes an entry's last lookup
        tex_end(&s4); g_sub += ALIAS_IDLE + 1;
        tex_t s6 = texture(97007); tex_live(&s6);
        CHECK(own_alias_take(s6.serial, &kr) == r, "a lookup of the idle entry");
        tex_end(&s6);
        id a3 = [NSObject new]; tex_t s7 = texture(97008); tex_live(&s7);
        CHECK(own_alias_publish(s7.serial, &ka, a3) == a3 && has(&kr), "the lookup's mark spares it from the next takeover");
        step("lookup marks");
        {   // replacement under a key retires the old texture: replaced in the map alone, it stayed in the set of shared textures and was released at once
            alias_key kx = akey(Q, 1 << 20, NVN_FORMAT_RGBA8); id x1 = [NSObject new], x2 = [NSObject new];
            pthread_mutex_lock(&g_smu); own_alias_put_locked(&kx, x1, g_sub); own_alias_put_locked(&kx, x2, g_sub); id now = own_alias_get_locked(&kx); pthread_mutex_unlock(&g_smu);
            CHECK(now == x2 && shared(x2) && !shared(x1) && dropped(x1, 0), "a texture put over a key's entry replaces it, and the old one is retired");
            step("replacement");
            pthread_mutex_lock(&g_smu); int del = own_alias_del_locked(&kx), again = own_alias_del_locked(&kx); pthread_mutex_unlock(&g_smu);
            CHECK(del && !again && !shared(x2), "removal: once");
            step("removal");
        }
        // a pool drop: that pool's entries go, another pool's stay; a lookup after it finds nothing, and the serial it had published keeps its texture (held)
        id q = [NSObject new]; tex_t s8 = texture(97009); tex_live(&s8);
        own_alias_publish(s8.serial, &kq, q);
        own_alias_stats before = ast();
        uint64_t g0 = atomic_load(&g_ser_gen), noted = 0;
        pthread_mutex_lock(&g_smu); uint32_t gone = own_alias_drop_pool_locked((const void *)(uintptr_t)P); pthread_mutex_unlock(&g_smu);
        CHECK(atomic_load(&g_ser_gen) == g0 + 1 && !own_note_at(g0 + 1, &noted), "a pool drop is one change no serial is noted for (a new epoch)");
        own_alias_stats after = ast();
        CHECK(gone == before.n - 1 && after.n == 1 && has(&kq) && after.evicted == before.evicted + gone, "a pool drop takes the pool's entries, counted, and leaves another pool's");
        CHECK(!own_alias_take(s7.serial + 100, &ka) && serial_get(s7.serial) == a3 && live_holds(a3) && !shared(a3), "after it a lookup finds nothing; a serial keeps the texture it had, no longer shared");
        step("pool drop");
        // a twin's lookup (peek), then its parent's pool goes, then the publication: refused
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> pt = [dev newTextureWithDescriptor:d], up = [dev newTextureWithDescriptor:d];
        if (pt && up) {
            alias_key kt = akey(P, 1 << 20, NVN_FORMAT_RGBA8); tex_t base = texture(97010), twin = texture(97011); tex_live(&base); tex_live(&twin);
            own_alias_publish(base.serial, &kt, pt);
            id<MTLTexture> b = own_alias_peek(&kt), v = [b newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
            pthread_mutex_lock(&g_smu); own_alias_drop_pool_locked((const void *)(uintptr_t)P); pthread_mutex_unlock(&g_smu);
            CHECK(b == pt && own_view_put(twin.serial, &kt, v) == 0 && !tex_get(twin.serial), "a twin whose parent's pool went between its lookup and its publication is not published");
            own_alias_publish(base.serial, &kt, pt);
            v = [pt newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
            pthread_mutex_lock(&g_smu); own_rename_locked(pt, up, base.serial); pthread_mutex_unlock(&g_smu);
            CHECK(own_view_put(twin.serial, &kt, v) == 0, "nor one whose parent was upgraded in between");
            pthread_mutex_lock(&g_smu); own_rename_locked(up, pt, base.serial); pthread_mutex_unlock(&g_smu);
            CHECK(own_view_put(twin.serial, &kt, v) == 1 && serial_get(twin.serial) == v && live_holds(pt), "a twin of the shared parent is published");
            step("twin interleavings");
            // own_upgrade: the rename and the twin's view made again, in one hold
            id<MTLTexture> nv = [up newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
            uint64_t u0 = atomic_load(&g_ser_gen), us = 0;
            int named = own_upgrade(pt, up, twin.serial, nv);
            CHECK(atomic_load(&g_ser_gen) == u0 + 2 && !own_note_at(u0 + 1, &us) && own_note_at(u0 + 2, &us) && us == twin.serial, "an upgrade is a new epoch, then the twin's note");
            CHECK(named == 0 && serial_get(twin.serial) == nv && serial_get(base.serial) == up && shared(up) && !shared(pt) && !holds(pt),
                  "an upgrade through the twin: every name of the parent moves, the twin's serial gets its new view, the old parent is held by nothing");
            CHECK(own_view_drop_stale(twin.serial, nv) == 0, "and the new view is valid");
            id<MTLTexture> up2 = [dev newTextureWithDescriptor:d];
            CHECK(own_upgrade(up, up2, base.serial, up2) == 1 && serial_get(base.serial) == up2 && shared(up2), "an upgrade through the parent's own serial: renamed, nothing more to publish");
            CHECK(own_view_drop_stale(twin.serial, nv) == 1 && !serial_get(twin.serial), "the twin's view of the replaced parent is stale");
            step("upgrade");
            tex_end(&base); tex_end(&twin);
            pthread_mutex_lock(&g_smu); own_alias_drop_pool_locked((const void *)(uintptr_t)P); pthread_mutex_unlock(&g_smu);
        }
        pthread_mutex_lock(&g_smu); own_alias_drop_pool_locked((const void *)(uintptr_t)Q); pthread_mutex_unlock(&g_smu);
        tex_end(&s5); tex_end(&s7); tex_end(&s8);
        pthread_mutex_lock(&g_smu); int keeps = own_keeps_locked(a3); pthread_mutex_unlock(&g_smu);
        CHECK(!keeps && !has(&kq) && !has(&kg), "the pools gone, nothing is shared");
        step("sharing table torn down");
    }

    {   // retirement eligibility: the queue's fence has passed (completion); the owner says whether the object may leave the residency set (no entry, pin or view's entry holds it)
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> p = [dev newTextureWithDescriptor:d];
        if (p) {
            uint64_t lk = (1ull << 62) + 98001, gk = 98002, pk = (1ull << 62) + 98003; [g_dropped removeAllObjects]; [g_dropped_keep removeAllObjects];
            id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
            tex_put(lk, v);   // a live entry holds a view of p
            CHECK(retire(p) == 0 && waiting() == 1, "a parent a live view's entry holds waits");
            step("retire: a view's parent waits");
            serial_del(lk);
            CHECK(!waiting() && ndropped(p, 0) == 1 && retire(p) == 1, "the view's entry gone, the parent is handed back to retirement (behind the submission then in flight) and is eligible");
            step("retire: handed back");
            tex_put(gk, p);   // a ghost entry
            CHECK(retire(p) == 0, "a ghost entry keeps it too");
            mtl_tex_pin(pk); tex_put(pk, p); serial_del(pk);   // and a pin, after its entry went
            idle(); for (int i = 0; i < NGHOST; i++) tex_put(990000 + (uint64_t)i, [NSObject new]);   // idle, the ghost entry goes as newer ones come
            trim_all();
            CHECK(!ghost(gk) && waiting() == 1 && retire(p) == 0, "with the ghost entry evicted, the pin still holds it: it keeps waiting");
            int before = ndropped(p, 0);
            mtl_tex_unpin(pk);
            CHECK(!waiting() && ndropped(p, 0) == before + 2, "the last unpin hands it back (besides the pin's own drop)");
            step("retire: ghost and pin");
            // a shared texture: when its last entry goes it is not handed back, as the sharing table keeps it resident and retires it itself when it lets it go
            share(p, 1); tex_put(lk, p);
            CHECK(retire(p) == 0, "a shared texture a live entry holds waits");
            before = ndropped(p, 0); serial_del(lk);
            CHECK(!waiting() && ndropped(p, 0) == before, "its entry gone, the sharing table still keeps it: not handed back");
            share(p, 0);
            CHECK(ndropped(p, 0) == before + 1 && retire(p) == 1, "the sharing table lets it go and retires it; then it is eligible");
            step("retire: shared");
            CHECK(own_object_free(p), "nothing holds it: the replay layer may write it over");
            tex_put(lk, v);
            CHECK(!own_object_free(p), "a view's entry holding it: it may not");
            serial_del(lk);
            step("retire: free");
        }
    }

    {   // teardown: every ghost serial, idle, evicted as NGHOST new ones come, then those taken away as stale views; nothing is held, every count is gone
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLTexture> p = [dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]];
        NSMutableArray *views = [NSMutableArray array];
        if (p) {
            share(p, 1); alias_key kp = key_of(p);
            idle();
            for (int i = 0; i < NGHOST; i++) { id<MTLTexture> v = [p newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]; [views addObject:v]; own_view_put(960000 + (uint64_t)i, &kp, v); }
            trim_all();
            share(p, 0);
            for (int i = 0; i < NGHOST; i++) own_view_drop_stale(960000 + (uint64_t)i, views[i]);
            pthread_mutex_lock(&g_smu); own_stats st = own_stats_locked(); pthread_mutex_unlock(&g_smu);
            own_alias_stats as = ast();
            CHECK(st.ghost == 0 && st.live == 0 && !holds(p), "after teardown no serial resolves and nothing is held");
            CHECK(as.n == 0 && as.cells == 0 && !shared(p) && st.retire_wait == 0, "and the sharing table, its index and the retired objects waiting are empty");
            step("teardown");
        }
    }

    printf("serial owner: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
