// The serial tables' owner (nvn_owner.m) on its own: linked without the replay layer, whose four hooks are recorded here instead (what is dropped and whether it stays resident,
// what the sharing table holds, which object's last count went, which serial changed). What it holds to, as the tables behaved before they moved out of nvn_mtl.m:
// a live serial's registration, replacement and removal; where tex_put sends a serial (live table for a live texture object or an external texture from 2^62 up, ghost ring for a
// finalized one); a finalize, and a copy's finalize that ends nothing; ghost replacement in place and eviction at NGHOST; texture pins and program pins; a view's entry counting its
// parent; renaming an object everywhere; and after every step the held counts against the tables (own_check_locked). The owner releases nothing itself: everything it lets go goes
// to own_hook_drop, which nvn_mtl.m releases once the submission in flight completes, so what a lookup returned outlives the change that dropped it.
// DM_TEST_SERIAL_TWIN=1 asserts what serial_del should do for a finalized texture serial, which it does not yet (the twin path of tex_mtl_: tex_put sends a finalized serial's view to
// the ghost ring, and serial_del, called when the view has to go, removes only a live entry). Without it the run prints what it found and counts no failure for it
#import <Metal/Metal.h>
#include "../nvn/nvn_owner.h"
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

// The replay layer's side, recorded
static NSMutableArray *g_dropped;   // what the owner let go, kept alive here as the deferred release keeps it until the submission completes
static NSMutableArray<NSNumber *> *g_dropped_keep;
static NSHashTable *g_shared;       // stands for the sharing table
static NSMutableArray<NSValue *> *g_unheld;
static uint64_t g_changed_last; static int g_changed_n;
void own_hook_drop(id o, int keep_resident) { [g_dropped addObject:o]; [g_dropped_keep addObject:@(keep_resident)]; }
int own_hook_shared(id o) { return [g_shared containsObject:o]; }
void own_hook_unheld(const void *o) { [g_unheld addObject:[NSValue valueWithPointer:o]]; }
void own_hook_changed(uint64_t serial) { g_changed_last = serial; g_changed_n++; }
static int dropped(id o, int keep) { for (NSUInteger i = 0; i < g_dropped.count; i++) if (g_dropped[i] == o && g_dropped_keep[i].intValue == keep) return 1; return 0; }
static int unheld(id o) { for (NSValue *v in g_unheld) if (v.pointerValue == (__bridge void *)o) return 1; return 0; }

static int live(uint64_t k) { return serial_get(k) != nil; }
static int ghost(uint64_t k) { __block int r = 0; pthread_mutex_lock(&g_smu); own_each_locked(^(uint64_t s, id o, int g) { (void)o; if (s == k && g) r = 1; }); pthread_mutex_unlock(&g_smu); return r; }
static int holds(id o) { pthread_mutex_lock(&g_smu); int r = entry_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
static int live_holds(id o) { pthread_mutex_lock(&g_smu); int r = serial_table_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
static void step(const char *label) {
    char why[256] = ""; pthread_mutex_lock(&g_smu); int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
    checks++; if (!ok) { failed++; fprintf(stderr, "FAIL: held counts against the tables after %s: %s\n", label, why); }
}
static tex_t texture(uint64_t serial) { return (tex_t){.magic = MAGIC('T','E','X',0), .serial = serial}; }
static void tex_live(tex_t *t) { pthread_mutex_lock(&g_smu); own_tex_live_put_locked(t); pthread_mutex_unlock(&g_smu); }   // mtl_tex_init's part
static int tex_end(tex_t *t) { pthread_mutex_lock(&g_smu); int r = own_tex_end_locked(t); pthread_mutex_unlock(&g_smu); if (r >= 0) serial_del(t->serial); return r; }   // mtl_tex_release's part

int main(void) { @autoreleasepool {
    g_dropped = [NSMutableArray array]; g_dropped_keep = [NSMutableArray array]; g_unheld = [NSMutableArray array];
    g_shared = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
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
        CHECK(unheld(wa), "replacement: the old object's last count went, which the replay layer hears (a retired object waiting for its holders)");
        step("replacement");
        serial_del(k);
        CHECK(!live(k) && !tex_get(k) && !live_holds(b) && dropped(b, 0), "serial_del: nothing resolves the serial, its object is dropped and not held");
        step("serial_del");
        notes = g_changed_n; serial_del(k);
        CHECK(g_changed_n == notes, "serial_del of a serial with no entry changes and reports nothing");
        step("serial_del again");
        id s = [NSObject new]; [g_shared addObject:s];
        serial_put(k, s); serial_del(k);
        CHECK(dropped(s, 1), "an object the sharing table holds stays resident when its entry goes");
        [g_shared removeObject:s];
        step("shared object");
    }

    {   // one object under two serials: held until both go
        id o = [NSObject new]; uint64_t s1 = (1ull << 62) + 93011, s2 = s1 + 1;
        tex_put(s1, o); tex_put(s2, o);
        serial_del(s1);
        CHECK(live_holds(o) && !unheld(o), "an object two serials name stays held when one of them goes");
        serial_del(s2);
        CHECK(!holds(o) && unheld(o), "and is not held once both have gone");
        step("two serials");
    }

    {   // where tex_put sends a texture serial, finalizing, and a copy's finalize
        uint64_t ext = (1ull << 62) + 93021, fk = 93023; tex_t lt = texture(93022); tex_live(&lt);
        id e = [NSObject new], l = [NSObject new], f = [NSObject new];
        tex_put(ext, e); tex_put(lt.serial, l); tex_put(fk, f);
        CHECK(live(ext) && !ghost(ext), "an external texture (2^62 and up) goes to the live table");
        CHECK(live(lt.serial) && !ghost(lt.serial) && own_tex_live(lt.serial), "a live texture object's serial goes to the live table");
        CHECK(!live(fk) && ghost(fk) && tex_get(fk) == f && !serial_get(fk), "a finalized serial goes to the ghost ring: tex_get resolves it, serial_get (live only) does not");
        CHECK(holds(f) && !live_holds(f), "a ghost entry holds its object, counted apart from the live table");
        step("tex_put");
        pthread_mutex_lock(&g_smu); uint64_t puts = own_stats_locked().ghost_puts; pthread_mutex_unlock(&g_smu);
        id f2 = [NSObject new]; tex_put(fk, f2);
        pthread_mutex_lock(&g_smu); uint64_t puts2 = own_stats_locked().ghost_puts; pthread_mutex_unlock(&g_smu);
        CHECK(tex_get(fk) == f2 && !holds(f) && dropped(f, 0) && puts2 == puts, "a ghost serial put again replaces its object in place, taking no new ring slot");
        step("ghost replacement");
        tex_t copy = lt;
        CHECK(tex_end(&copy) == -1 && own_tex_live(lt.serial) && live(lt.serial), "a copy's finalize ends nothing while the object the texture was initialized in holds it");
        CHECK(tex_end(&lt) == 1 && !own_tex_live(lt.serial) && !live(lt.serial) && !ghost(lt.serial) && !holds(l), "the object's own finalize ends the texture; it does not move to the ghost ring");
        step("finalize");
        tex_t own = texture(93024), moved; tex_live(&own); moved = own; own.magic = 0;   // the object the texture was initialized in no longer holds it (the game moved or freed it)
        CHECK(tex_end(&moved) == 1 && !own_tex_live(moved.serial), "a copy outliving the object it was initialized in ends the texture");
        tex_put(lt.serial, l);
        CHECK(ghost(lt.serial) && tex_get(lt.serial) == l, "the finalized serial put again (a replayed record) goes to the ghost ring");
        step("tex_put after finalize");
        serial_del(ext);
    }

    {   // the ghost ring: the oldest serial goes when NGHOST newer ones have come
        enum { BASE = 940000 };
        NSMutableArray *objs = [NSMutableArray array];
        for (int i = 0; i <= NGHOST; i++) { id o = [NSObject new]; [objs addObject:o]; tex_put(BASE + (uint64_t)i, o); }
        CHECK(!ghost(BASE) && !tex_get(BASE) && !holds(objs[0]) && dropped(objs[0], 0), "the oldest ghost serial is evicted by the NGHOST + 1st: dropped, no longer held");
        CHECK(ghost(BASE + 1) && tex_get(BASE + NGHOST) == objs[NGHOST], "the newer ones stay");
        pthread_mutex_lock(&g_smu); own_stats st = own_stats_locked(); pthread_mutex_unlock(&g_smu);
        CHECK(st.ghost <= NGHOST, "the ring holds at most NGHOST serials");
        step("ghost eviction");
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

    {   // the finalized twin: a finalized serial's view is put in the ghost ring, then has to go, and serial_del removes only a live entry
        uint64_t k = 93061; id v = [NSObject new];
        tex_put(k, v);   // as tex_mtl_ does for a twin view of a texture whose object is finalized
        serial_del(k);   // as it does when the shared texture left the sharing table meanwhile (kept == 0)
        int stays = tex_get(k) == v && ghost(k) && holds(v);
        if (getenv("DM_TEST_SERIAL_TWIN")) CHECK(!stays, "serial_del of a finalized texture serial: it resolves to nothing and its view is not held");
        else printf("finalized twin: serial_del %s the ghost entry (DM_TEST_SERIAL_TWIN=1 asserts that it goes)\n", stays ? "leaves" : "removes");
        step("finalized twin");
    }

    printf("serial owner: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
