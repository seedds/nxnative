// The NVN resource owner: nvn_owner.h says what it holds, what a lookup returns and the lock order. Moved from nvn_mtl.m (the serial tables, the sharing table, the change
// generation, retirement's eligibility).
#import <Metal/Metal.h>
#include "nvn_owner.h"
#include "nvn_tex.h"
#include "../../host/dm.h"
#include <mach/mach.h>
#include <os/lock.h>

#define ODIE(...) do { dm_log("[dm] [nvn-mtl] FATAL " __VA_ARGS__); abort(); } while (0)

pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;

// The change generation of what serials resolve to: +1 when an entry is added, replaced or removed, a shared texture goes or is renamed, or the replay layer changes what a serial's
// object stands for (a packaged texture's upload replaced, own_note / own_epoch). Downstream caches (texture views, programs, pool tables) record it at fetch time and use what they
// fetched while it is unchanged. Which serial each change was about, for the texture ID cache (ser_sync): own_note records it; a change with no note (own_epoch: an upgrade, a pool's
// shared textures dropped, an upload lost) sends every texture ID down the slow path at the next sync. A full list is the same as a missing note. The same notes by generation, for the
// pool tables (pool_gpu), which outlive the texture ID cache's next sync: an entry whose generation is not the one asked for (a change without a note, or one the ring has overwritten)
// makes the table be built whole. Under g_snote_lk (taken under g_smu or alone, never the other way)
_Atomic uint64_t g_ser_gen = 1;
static os_unfair_lock g_snote_lk = OS_UNFAIR_LOCK_INIT; static uint64_t g_snote[SER_NOTES]; static unsigned g_nsnote;
typedef struct { uint64_t gen, serial; } ser_lnote;
static ser_lnote g_ser_log[SER_LOG];
void own_note(uint64_t serial) {
    os_unfair_lock_lock(&g_snote_lk); if (g_nsnote < SER_NOTES) g_snote[g_nsnote++] = serial;
    uint64_t g = atomic_fetch_add(&g_ser_gen, 1) + 1; g_ser_log[g % SER_LOG] = (ser_lnote){g, serial};
    os_unfair_lock_unlock(&g_snote_lk);
}
void own_epoch(void) { atomic_fetch_add(&g_ser_gen, 1); }
unsigned own_notes_take(uint64_t out[SER_NOTES], uint64_t *gen) {
    os_unfair_lock_lock(&g_snote_lk); *gen = g_ser_gen; unsigned n = g_nsnote; memcpy(out, g_snote, n * sizeof *out); g_nsnote = 0; os_unfair_lock_unlock(&g_snote_lk);
    return n;
}
int own_note_at(uint64_t gen, uint64_t *serial) {
    os_unfair_lock_lock(&g_snote_lk); const ser_lnote *sn = &g_ser_log[gen % SER_LOG]; int known = sn->gen == gen; if (known) *serial = sn->serial; os_unfair_lock_unlock(&g_snote_lk);
    return known;
}
static nvn_hmap g_by_serial = {.klen = 8};     // Serial → MTLTexture / NvPkg / NvProg
// How many entries of g_by_serial hold each object (key: its address; a view's parents too, count_add), kept by every writer of g_by_serial (serial_put, serial_del, tex_put, the upgrade's in-place
// replacement), so alias_take_over asks whether a live texture object still resolves to a shared texture in one lookup. It walked the serial table's whole capacity for
// each candidate instead: in Ocarina of Time's cutscenes, with about 7,500 serials and thousands of shared textures, that walk was the submit thread's hottest function and
// frames took up to a second. An entry holds its object (CFBridgingRetain), so a counted address cannot be reused while counted. Under g_smu
static nvn_hmap g_by_serial_held = {.klen = sizeof(uintptr_t)};
// The same count for the ghost table (g_ghost, finalized serials a replayed record still resolves; tex_put and the upgrade's in-place replacement write it)
static nvn_hmap g_ghost_held = {.klen = sizeof(uintptr_t)};
// Texture serials owned by a live NVN texture object (TextureInitialize until Finalize or re-initialization, hle_nvn.c). Command records and texture pool registrations snapshot the
// texture object (tex_snap in hle_nvn.c, mtl_reg_tex), so replay can resolve a texture the game has already finalized: Breath of the Wild records each frame's transient render targets
// (the HUD captures, the 640x360 half-resolution buffers) and finalizes their objects before it submits. Those serials went back into g_by_serial and stayed there for good, as no second
// Finalize comes: about 10 a frame, 200,000 after an hour in the first shrine. A dead serial resolves through g_ghost instead, so a record replayed again soon still finds its texture.
// So does a registered ID whose texture the game finalized (a retired registration): its view goes at the end of each submission (treg_finish_submission in nvn_mtl.m) and the next
// one resolves it again. The table was a ring of NGHOST (512) whose oldest entry went when a new one came, whatever its use: while Ocarina of Time streams an area in, or rides into
// Hyrule Field, its pools name thousands of finalized textures at once, so each resolution evicted one the next would need, whose note sent its ID to be resolved again, and the ring
// never settled (3.7 million puts in 300 frames at an area transition, frames of 100-490 ms on the submit thread; up to 43 million in a window and 10-25 fps for minutes in
// GitHub issue #2213). The table is now kept by use: each entry's last lookup or put (frame and submission) in a list from least to most recently used, and past NGHOST entries
// the least recently used goes only once it is idle (GHOST_IDLE frames or GHOST_IDLE_SUBS submissions without a use: ghost_trim), so a working set larger than NGHOST stays while
// it is used and the table is back at NGHOST once it idles. Frames, not submissions alone: that game makes about 25 submissions a frame, and a window of 16 submissions left
// 99.6 % of the evictions coming back within a frame (PR #2389)
static nvn_hmap g_tex_live = {.klen = 8};   // serial → the texture object it was initialized in (mtl_tex_release), under g_smu
static nvn_hmap g_ghost = {.klen = 8}; static uint64_t g_ghost_puts;
typedef struct ghost_use { struct ghost_use *prev, *next; uint64_t serial, frame, sub; } ghost_use;   // a ghost entry's last use
static nvn_hmap g_ghost_use = {.klen = 8};    // serial → its ghost_use: the same serials as g_ghost
static ghost_use *g_ghost_lru, *g_ghost_mru;  // the list, least recently used first
// Deferred command descriptors and persistent bindings retain a texture serial independently of the guest descriptor and the bounded recently-finalized cache
typedef struct { unsigned refs; void *object; } tex_pin;
static nvn_hmap g_tex_pins = {.klen = 8}, g_tex_pin_objects = {.klen = sizeof(uintptr_t)};

int serial_table_holds(const void *o) { uintptr_t k = (uintptr_t)o; return nvn_hm_get(&g_by_serial_held, &k) != NULL; }
int entry_holds(const void *o) { uintptr_t k = (uintptr_t)o; return serial_table_holds(o) || (g_ghost_held.n && nvn_hm_get(&g_ghost_held, &k)); }
int tex_object_pinned(id o) { uintptr_t key = (uintptr_t)(__bridge void *)o; return o && nvn_hm_get(&g_tex_pin_objects, &key) != NULL; }
int own_object_free(id o) { pthread_mutex_lock(&g_smu); int r = !entry_holds((__bridge void *)o); pthread_mutex_unlock(&g_smu); return r; }
static CFMutableSetRef g_alias_set;   // the sharing table's textures (below)
// By address alone (no message to the object: gone_note may ask after the last reference went)
int own_holds_locked(const void *o) { uintptr_t k = (uintptr_t)o; return o && (entry_holds(o) || nvn_hm_get(&g_tex_pin_objects, &k) || (g_alias_set && CFSetContainsValue(g_alias_set, o))); }
// The objects that left their last holder, for the replay layer's view cache (own_gone_take_locked): noted where a count reaches zero (count_one) and where an object leaves the
// sharing table (alias_del, own_rename_locked). Past GONE_MAX the list stops and says so, and the taker checks everything it keeps. Under g_smu
enum { GONE_MAX = 65536 };
static const void **g_gone; static unsigned g_gone_n, g_gone_cap; static int g_gone_over;
static void gone_note(const void *o) {
    if (!o || g_gone_over || own_holds_locked(o)) return;
    if (g_gone_n == GONE_MAX) { g_gone_over = 1; return; }
    if (g_gone_n == g_gone_cap) {
        unsigned cap = g_gone_cap ? 2 * g_gone_cap : 256; const void **p = realloc(g_gone, cap * sizeof *p);
        if (!p) ODIE("the owner's released-object list");
        g_gone = p; g_gone_cap = cap;
    }
    g_gone[g_gone_n++] = o;
}
int own_gone_take_locked(const void *const **list, unsigned *n) {
    int over = g_gone_over; *list = g_gone; *n = g_gone_n; g_gone_n = 0; g_gone_over = 0;
    return over;
}

// Retirement eligibility. The replay layer retires a resource through its deferred-release queue (own_hook_drop(o, 0): out of the residency set once the submissions encoded up to
// then have completed), and asks here, once that completion has come, whether it may really leave (own_retire_locked): GPU completion says the work encoded before the retirement is
// done, not that no serial resolves to the object any more. Several entries can hold one texture that no table owns: at the GPU's first write over storage a packaged texture was
// taken from (cpu_unc_drop), every serial of the package gets the package's texture itself, live serials and finalized ones in the ghost table. The package's release and each
// holder's retired it, taking it out of the residency set while the others still drew with it: GPU address faults 15-45 minutes into Ocarina of Time, on 260x260 terrain tiles
// (GitHub issue #1326). So an object an entry (live, ghost or pin, or a view's parent) still holds waits here, retained, and the queue drops only its own reference; the last count
// of it going hands it back to the queue (retire_resume: own_hook_drop(o, 0), behind the submission then in flight, as the entries' last encodings may be in it), unless it is
// pinned or shared meanwhile (the sharing table retires its own). Who releases what: the queue entry's reference goes with the queue entry (the replay layer's drain, after the
// residency set's commit); the reference held here goes when the object is handed back; at quit nothing is drained (the process exits with both). Under g_smu
static nvn_hmap g_retire_wait = {.klen = sizeof(uintptr_t)};   // address → the retired object (retained) waiting for its last entry
static void retire_resume(const void *o) {
    uintptr_t k = (uintptr_t)o;
    if (!g_retire_wait.n || entry_holds(o)) return;
    void *v = nvn_hm_del(&g_retire_wait, &k); if (!v) return;
    id obj = CFBridgingRelease(v);
    if (!own_keeps_locked(obj)) own_hook_drop(obj, 0);
}
int own_retire_locked(id o) {
    if (!entry_holds((__bridge void *)o)) return 1;
    uintptr_t k = (uintptr_t)(__bridge void *)o;
    if (!nvn_hm_get(&g_retire_wait, &k)) nvn_hm_put(&g_retire_wait, &k, (void *)CFBridgingRetain(o));
    return 0;
}
static void count_one(nvn_hmap *m, const void *o, int d) {
    if (!o) return;
    uintptr_t k = (uintptr_t)o, n = (uintptr_t)nvn_hm_get(m, &k);
    if (d > 0) nvn_hm_put(m, &k, (void *)(n + 1));
    else if (n > 1) nvn_hm_put(m, &k, (void *)(n - 1));
    else if (n) { nvn_hm_del(m, &k); retire_resume(o); gone_note(o); }   // a slot written past these helpers (a test's table) was never counted: nothing to take away
}
// An entry that holds a view holds the textures the view is of as well, and they are counted with it: an sRGB / linear twin's serial holds a view of the shared texture
// (tex_mtl_), so that texture is in use as long as the twin's entry is. Counted alone, the view left its parent looking unheld: a takeover evicted it under a live twin
// (alias_take_over), and its retirement took it out of the residency set while pool tables still named the view (GitHub issue #2125). A view's parent never changes, so
// what an entry adds it takes away
static void count_add(nvn_hmap *m, const void *o, int d) {
    count_one(m, o, d);
    id t = (__bridge id)o;
    if (t && [t respondsToSelector:@selector(parentTexture)]) for (id<MTLTexture> p = ((id<MTLTexture>)t).parentTexture; p; p = p.parentTexture) count_one(m, (__bridge void *)p, d);
}
static void held_add(const void *o, int d) { count_add(&g_by_serial_held, o, d); }
static void ghost_held_add(const void *o, int d) { count_add(&g_ghost_held, o, d); }

// The sharing table. Textures shared by storage (non-packaged): key = memory pool + offset + texture description. An NVN texture object is only a descriptor; an object rebuilt over the
// same storage with the same description is the same texture; SMBW rebuilds its transient render targets on new objects every frame and never Finalizes them, so building one per serial
// leaked about 13 MB of GPU memory per frame. Textures here are kept resident by the sharing table, and when the serial table replaces / deletes them it only drops its reference
// (own_hook_drop keeping it resident), never removing them from the residency set. Entries go when their pool is finalized (own_alias_drop_pool_locked) or another texture takes over
// their storage (alias_take_over). Under g_smu
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};
// The shared textures by where their storage lies, for alias_take_over and the image families' owner lookup (own_alias_at_locked): {pool, 64 KB cell of the pool} → the entries whose
// bytes reach into that cell, each with its end. It walked g_alias's whole capacity for every texture made instead: Mario Golf: Super Rush's All-Star Summit registers 134,400 textures
// and makes about 90,000 of them as the course loads, each walk 131,072 slots with a layout and a lookup per entry of the pool, and the load had not finished after 12 minutes on one
// core. An entry lies in every cell its bytes reach, so one of 8 MB is in 128 of them. Kept by alias_put and alias_del, the only writers of g_alias's keys (an upgrade replaces a value in place)
typedef struct { alias_key k; int64_t end; } alias_ent;
typedef struct { uint32_t n, cap; alias_ent e[]; } alias_cell;
static nvn_hmap g_alias_cells = {.klen = 16};
static void alias_index(const alias_key *k, int add) {
    nvn_texlayout L; nvn_tex_layout(&k->d, &L);
    int64_t end = k->off + (int64_t)L.size, c1 = (end > k->off ? end - 1 : k->off) >> ALIAS_CELL;
    for (int64_t c = k->off >> ALIAS_CELL; c <= c1; c++) {
        uint64_t ck[2] = {k->pool, (uint64_t)c}; alias_cell *v = nvn_hm_get(&g_alias_cells, ck);
        uint32_t i = 0; if (v) for (; i < v->n; i++) if (!memcmp(&v->e[i].k, k, sizeof *k)) break;
        if (add) {
            if (v && i < v->n) continue;
            if (!v || v->n == v->cap) {
                uint32_t cap = v ? v->cap * 2 : 4; alias_cell *nv = realloc(v, sizeof *nv + cap * sizeof nv->e[0]); if (!nv) abort();
                if (!v) nv->n = 0; nv->cap = cap; v = nv; nvn_hm_put(&g_alias_cells, ck, v);
            }
            v->e[v->n].k = *k; v->e[v->n++].end = end;
        } else if (v && i < v->n) {
            v->e[i] = v->e[--v->n];
            if (!v->n) { nvn_hm_del(&g_alias_cells, ck); free(v); }
        }
    }
}
static nvn_hmap g_alias_seen = {.klen = sizeof(alias_key)};   // key → the submission of its last lookup or creation (own_hook_submission; alias_take_over spares entries looked up lately)
static CFMutableSetRef g_alias_set;                           // the textures in the sharing table (non-owning; "is this shared")
static uint64_t g_alias_evicted, g_alias_taken;               // shared textures evicted on pool Finalize / when another texture took over their storage, in total (the memory report)
int own_shared_locked(id o) { return g_alias_set && o && CFSetContainsValue(g_alias_set, (__bridge const void *)o); }
int own_keeps_locked(id o) { return tex_object_pinned(o) || own_shared_locked(o); }
static void obj_drop(id o) { own_hook_drop(o, own_keeps_locked(o)); }   // caller holds g_smu
static void alias_seen(const alias_key *k, uint64_t now) { nvn_hm_put(&g_alias_seen, k, (void *)(uintptr_t)now); }
// The sharing table's two writers: an entry goes in and out with its cells (alias_index), its place in g_alias_set and its last lookup (g_alias_seen), so none of them can be left out.
// A removed texture is retired (deferred release, own_hook_drop). Caller holds g_smu
static int alias_del(const alias_key *k) {
    void *old = nvn_hm_del(&g_alias, k);
    if (!old) return 0;
    alias_index(k, 0); nvn_hm_del(&g_alias_seen, k);
    id o = CFBridgingRelease(old);
    if (g_alias_set) CFSetRemoveValue(g_alias_set, (__bridge const void *)o);
    gone_note((__bridge const void *)o);
    own_hook_drop(o, 0);
    return 1;
}
static void alias_put(const alias_key *k, id o, uint64_t seen) {   // an entry k had is retired first: replaced in the map alone, it stayed in g_alias_set and was released at once
    if (nvn_hm_get(&g_alias, k)) alias_del(k);
    if (!g_alias_set) g_alias_set = CFSetCreateMutable(NULL, 0, NULL);
    hm_put_id(&g_alias, k, o); alias_index(k, 1); CFSetAddValue(g_alias_set, (__bridge const void *)o); if (seen) alias_seen(k, seen);
}
id own_alias_get_locked(const alias_key *k) { return hm_get_id(&g_alias, k); }
id own_alias_peek(const alias_key *k) { pthread_mutex_lock(&g_smu); id o = hm_get_id(&g_alias, k); pthread_mutex_unlock(&g_smu); return o; }
void own_alias_put_locked(const alias_key *k, id o, uint64_t seen) { alias_put(k, o, seen); }
int own_alias_del_locked(const alias_key *k) { return alias_del(k); }
// On memory pool Finalize, the textures built on that pool leave the sharing table (the keys collected first, then deleted: deletion shifts entries back). The sharing table used not to
// evict: Mario's pools stay for good, so entries are bounded; Engage frees pools on scene changes and rebuilds on new ones, so the old pools' render targets and other private textures
// stayed in the table forever (about 0.8 GB more GPU memory over 850 s of the title demo), and linear textures built directly on a pool also kept the whole pool buffer alive. Also, when a
// pool object's address is reused, old entries would hand a texture on the old storage to the new pool's texture at the same offset and description. Evicted textures go through deferred
// release; if a serial entry still holds one, that is just one more reference
uint32_t own_alias_drop_pool_locked(const void *pool) {
    uint32_t n = 0; alias_key *ks = NULL;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k; memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        alias_key *nk = realloc(ks, (n + 1) * sizeof *ks); if (!nk) break; ks = nk; ks[n++] = k;
    }
    for (uint32_t j = 0; j < n; j++) alias_del(&ks[j]);
    free(ks); g_alias_evicted += n;
    if (n) own_epoch();   // no note: the IDs of the pool's textures are many, and its finalize marks them gone besides
    return n;
}
// A new shared texture takes over storage: entries of the same pool whose storage overlaps the new texture's under another description go, unless a live texture object still resolves
// to them (an object rebuilt over the same storage with the same description must keep finding it: SMBW's transient render targets) or a lookup found them in the last ALIAS_IDLE submissions
// (Breath of the Wild finalizes its transient render targets before it submits and makes them again the next frame: targets sharing storage would evict each other every frame). On NVN
// the memory now belongs to the new texture; the old private Metal texture only held a copy nothing can name again. STEINS;GATE RE:BOOT decodes every image it shows into one scratch
// region of a pool that lives for good and finalizes the texture before the next one: each image added an entry, 468 of them and 1.58 GB of GPU memory 24,000 frames into a new game.
// The texture is retired (deferred until the submissions that used it complete); one a finalized object still resolves through the ghost table stays resident until that table lets it go
// (own_retire_locked). The change reports no serial (own_note(0)), as it changes no serial's texture: an entry a live serial holds is not evicted
// (serial_table_holds), and one a finalized serial holds through the ghost table keeps its texture. A live sRGB / linear twin's serial holds a view of the shared texture, which counts as
// holding the texture (count_add), so a twin keeps its parent as a texture of the parent's own format would; with the view alone counted the parent went, and no note sent the twin's
// texture IDs to tex_mtl_'s stale-view check (GitHub issue #2125). A retired ID's view goes at the submission's end (nvn_mtl.m's treg_finish_submission), and the pool tables take
// those IDs again at the next one (pool_snap.trans). With no note, every takeover (about 8,400 in six minutes walking through Ocarina of Time's Kokiri Forest) sent every
// texture ID down the slow path and made the next pool walk build the 33,025-entry table whole. Caller holds g_smu
static uint32_t alias_take_over(const alias_key *nk, uint64_t now) {
    nvn_texlayout L; nvn_tex_layout(&nk->d, &L);
    if (!L.size) return 0;
    int64_t lo = nk->off, hi = nk->off + (int64_t)L.size;
    uint32_t n = 0; alias_key ks[16];
    for (int64_t c = lo >> ALIAS_CELL; c <= (hi - 1) >> ALIAS_CELL && n < 16; c++) {   // the entries whose storage reaches into this one's cells (alias_index)
        uint64_t ck[2] = {nk->pool, (uint64_t)c}; const alias_cell *v = nvn_hm_get(&g_alias_cells, ck);
        for (uint32_t i = 0; v && i < v->n && n < 16; i++) {
            const alias_key *k = &v->e[i].k;
            if (!memcmp(k, nk, sizeof *k) || k->off >= hi || v->e[i].end <= lo) continue;
            uint32_t j = 0; while (j < n && memcmp(&ks[j], k, sizeof *k)) j++;
            if (j < n) continue;   // met in an earlier cell
            void *val = nvn_hm_get(&g_alias, k); if (!val) continue;
            uint64_t seen = (uint64_t)(uintptr_t)nvn_hm_get(&g_alias_seen, k);
            if ((seen && now - seen < ALIAS_IDLE) || serial_table_holds(val)) continue;
            ks[n++] = *k;
        }
    }
    for (uint32_t j = 0; j < n; j++) alias_del(&ks[j]);
    if (n) { own_note(0); g_alias_taken += n; }
    return n;
}
uint32_t own_alias_take_over_locked(const alias_key *k) { return alias_take_over(k, own_hook_submission()); }
void own_alias_at_locked(const void *pool, int64_t off, void (^fn)(const alias_key *k, id o)) {
    uint64_t ck[2] = {(uint64_t)(uintptr_t)pool, (uint64_t)(off >> ALIAS_CELL)}; const alias_cell *cell = nvn_hm_get(&g_alias_cells, ck);   // every entry at this offset lies in its cell
    for (uint32_t i = 0; cell && i < cell->n; i++) {
        const alias_key *k = &cell->e[i].k; if (k->off != off) continue;
        id o = hm_get_id(&g_alias, k); if (o) fn(k, o);
    }
}
void own_alias_each_locked(void (^fn)(const alias_key *k, id o)) {
    for (uint32_t i = 0; i < g_alias.cap; i++) if (g_alias.vals[i]) fn((const alias_key *)(const void *)(g_alias.keys + (size_t)i * g_alias.klen), (__bridge id)g_alias.vals[i]);
}
own_alias_stats own_alias_stats_locked(void) { return (own_alias_stats){.n = g_alias.n, .cap = g_alias.cap, .cells = g_alias_cells.n, .evicted = g_alias_evicted, .taken = g_alias_taken}; }

const void *own_live_ref_locked(uint64_t k) { return nvn_hm_get(&g_by_serial, &k); }
const void *own_live_ref(uint64_t k) { pthread_mutex_lock(&g_smu); const void *o = nvn_hm_get(&g_by_serial, &k); pthread_mutex_unlock(&g_smu); return o; }
void serial_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { held_add((__bridge void *)old, -1); obj_drop(old); } hm_put_id(&g_by_serial, &k, o); held_add((__bridge void *)o, 1); own_note(k); pthread_mutex_unlock(&g_smu); }
static void live_del_locked(uint64_t k) { id old = hm_get_id(&g_by_serial, &k); if (old) { held_add((__bridge void *)old, -1); obj_drop(old); hm_del_id(&g_by_serial, &k); own_note(k); } }   // nothing to drop: the texture
void serial_del(uint64_t k) { pthread_mutex_lock(&g_smu); live_del_locked(k); pthread_mutex_unlock(&g_smu); }
id own_ext_take(uint64_t k) {   // not retired through the deferred-release queue, which only NVN submissions drain: the present path retires it (mtl_ext_tex_release)
    pthread_mutex_lock(&g_smu);
    void *v = nvn_hm_del(&g_by_serial, &k); id o = v ? CFBridgingRelease(v) : nil;
    if (o) { held_add((__bridge void *)o, -1); own_note(k); }
    pthread_mutex_unlock(&g_smu);
    return o;
}

void own_tex_live_put_locked(const tex_t *t) { nvn_hm_put(&g_tex_live, &t->serial, (void *)t); }
int own_tex_live_locked(uint64_t serial) { return nvn_hm_get(&g_tex_live, &serial) != NULL; }
int own_tex_live(uint64_t serial) { pthread_mutex_lock(&g_smu); int r = own_tex_live_locked(serial); pthread_mutex_unlock(&g_smu); return r; }
int own_tex_end_locked(tex_t *t) {
    // NVNtexture is plain data, and engines finalize copies of it (Breath of the Wild about 100 a frame, Ocarina of Time every few frames, for textures whose own object lives on). A copy's Finalize ends
    // only the copy. Taken for the texture's end, it deleted the host texture, which the next read made and uploaded again: with a pool walk touching every ID (pool_gpu), 5,500 uploads and 500 MB to 1 GB
    // a frame in Ocarina of Time's Kokiri Forest, the batches' memory held until they retired. If the object the texture was initialized in no longer holds it (the game moved it), this was the last one
    const tex_t *own = nvn_hm_get(&g_tex_live, &t->serial);
    if (own && own != t) {   // read through the kernel: the game may have freed and unmapped that object
        tex_t seen; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)own, sizeof seen, (vm_address_t)(uintptr_t)&seen, &got) == KERN_SUCCESS && got == sizeof seen && seen.magic == MAGIC('T', 'E', 'X', 0) && seen.serial == t->serial) return -1;
    }
    return nvn_hm_del(&g_tex_live, &t->serial) != NULL;
}

static void tex_pin_set(tex_pin *pin, id object) {   // g_smu held
    id old = (__bridge id)pin->object; if (old == object) return;
    if (old) {
        uintptr_t key = (uintptr_t)pin->object, count = (uintptr_t)nvn_hm_get(&g_tex_pin_objects, &key);
        if (count > 1) nvn_hm_put(&g_tex_pin_objects, &key, (void *)(count - 1)); else nvn_hm_del(&g_tex_pin_objects, &key);
        held_add(pin->object, -1); own_hook_drop(old, own_keeps_locked(old) || serial_table_holds((__bridge void *)old)); CFRelease(pin->object); pin->object = NULL;
    }
    if (object) {
        pin->object = (void *)CFBridgingRetain(object); uintptr_t key = (uintptr_t)pin->object;
        uintptr_t count = (uintptr_t)nvn_hm_get(&g_tex_pin_objects, &key);
        nvn_hm_put(&g_tex_pin_objects, &key, (void *)(count + 1)); held_add(pin->object, 1);
    }
}
void mtl_tex_pin(uint64_t serial) {
    if (!serial) return; pthread_mutex_lock(&g_smu);
    tex_pin *pin = nvn_hm_get(&g_tex_pins, &serial);
    if (!pin) { pin = calloc(1, sizeof *pin); if (!pin) ODIE("texture retention allocation failed"); nvn_hm_put(&g_tex_pins, &serial, pin); }
    pin->refs++;
    if (!pin->object) { id o = hm_get_id(&g_by_serial, &serial); if (!o) o = hm_get_id(&g_ghost, &serial); tex_pin_set(pin, o); }
    pthread_mutex_unlock(&g_smu);
}
void mtl_tex_unpin(uint64_t serial) {
    if (!serial) return; pthread_mutex_lock(&g_smu); tex_pin *pin = nvn_hm_get(&g_tex_pins, &serial);
    if (pin && !--pin->refs) { nvn_hm_del(&g_tex_pins, &serial); tex_pin_set(pin, nil); free(pin); }
    pthread_mutex_unlock(&g_smu);
}
// The ghost table's order of use. Caller holds g_smu
static void use_unlink(ghost_use *u) {
    if (u->prev) u->prev->next = u->next; else g_ghost_lru = u->next;
    if (u->next) u->next->prev = u->prev; else g_ghost_mru = u->prev;
    u->prev = u->next = NULL;
}
static void ghost_used(uint64_t k) {   // a lookup or a put of k's ghost entry: stamped, and the most recently used
    ghost_use *u = nvn_hm_get(&g_ghost_use, &k);
    if (!u) { u = calloc(1, sizeof *u); if (!u) ODIE("ghost table allocation failed"); u->serial = k; nvn_hm_put(&g_ghost_use, &k, u); }
    else if (u != g_ghost_mru) use_unlink(u);
    u->frame = own_hook_frame(); u->sub = own_hook_submission();
    if (u != g_ghost_mru) { u->prev = g_ghost_mru; if (g_ghost_mru) g_ghost_mru->next = u; else g_ghost_lru = u; g_ghost_mru = u; }
}
static void ghost_del_locked(uint64_t k) {   // k's ghost entry goes with its place in the list; the serial resolves anew (a note)
    ghost_use *u = nvn_hm_del(&g_ghost_use, &k); if (u) { use_unlink(u); free(u); }
    id old = hm_get_id(&g_ghost, &k); if (!old) return;
    ghost_held_add((__bridge void *)old, -1); obj_drop(old); hm_del_id(&g_ghost, &k); own_note(k);
}
// Idle entries past NGHOST go, least recently used first, at most `budget` of them; the first entry in use ends the walk (every one after it was used later)
static void ghost_trim(unsigned budget) {
    uint64_t frame = own_hook_frame(), sub = own_hook_submission();
    for (; budget && g_ghost.n > NGHOST && g_ghost_lru; budget--) {
        const ghost_use *u = g_ghost_lru;
        if (frame - u->frame < GHOST_IDLE && sub - u->sub < GHOST_IDLE_SUBS) break;
        ghost_del_locked(u->serial);
    }
}
void own_ghost_trim(void) { pthread_mutex_lock(&g_smu); ghost_trim(GHOST_TRIM); pthread_mutex_unlock(&g_smu); }
const void *own_resolve_ref(uint64_t k) {
    pthread_mutex_lock(&g_smu); const void *o = nvn_hm_get(&g_by_serial, &k);
    if (!o && g_ghost.n && (o = nvn_hm_get(&g_ghost, &k))) ghost_used(k);
    if (!o) { tex_pin *pin = nvn_hm_get(&g_tex_pins, &k); if (pin) o = pin->object; }
    pthread_mutex_unlock(&g_smu); return o;
}
static void tex_put_locked(uint64_t k, id o) {   // serials from 2^62 up are external textures (mtl_ext_tex), never initialized by the game
    nvn_hmap *m = k >= (1ull << 62) || nvn_hm_get(&g_tex_live, &k) ? &g_by_serial : &g_ghost;
    id old = hm_get_id(m, &k);
    if (old) count_add(m == &g_by_serial ? &g_by_serial_held : &g_ghost_held, (__bridge void *)old, -1);
    if (old) obj_drop(old);
    else if (m == &g_ghost) g_ghost_puts++;
    hm_put_id(m, &k, o); tex_pin *pin = nvn_hm_get(&g_tex_pins, &k); if (pin) tex_pin_set(pin, o); own_note(k);
    count_add(m == &g_by_serial ? &g_by_serial_held : &g_ghost_held, (__bridge void *)o, 1);
    if (m == &g_ghost) { ghost_used(k); ghost_trim(2); }   // two per put: idle entries drain faster than new ones come
}
void tex_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); tex_put_locked(k, o); pthread_mutex_unlock(&g_smu); }

// A serial's shared texture is found and published in one hold: a pool's Finalize on the game's thread between the two (as when tex_mtl_ looked up, let g_smu go and then called
// tex_put) gave the serial a texture already evicted and retired
id own_alias_take(uint64_t serial, const alias_key *k) {
    pthread_mutex_lock(&g_smu);
    id o = hm_get_id(&g_alias, k); if (o) { alias_seen(k, own_hook_submission()); tex_put_locked(serial, o); }
    pthread_mutex_unlock(&g_smu); return o;
}
// The takeover, the entry and the serial's publication in one hold. A texture another thread registered under the key since the caller's lookup missed is the one the storage's
// serials share: the serial takes it, and the caller retires its own (replacing it instead, as the table did, left the first serial a texture no longer shared)
id own_alias_publish(uint64_t serial, const alias_key *k, id o) {
    pthread_mutex_lock(&g_smu);
    uint64_t now = own_hook_submission(); id cur = hm_get_id(&g_alias, k);
    if (cur) { alias_seen(k, now); o = cur; }
    else { alias_take_over(k, now); alias_put(k, o, now); }
    tex_put_locked(serial, o);
    pthread_mutex_unlock(&g_smu); return o;
}

// sRGB / linear views (tex_mtl_'s twin of a shared texture). A view shares its parent's bytes only while the parent is in the sharing table: a parent that left it (evicted with its
// pool, taken over, upgraded) no longer holds the storage's bytes, and neither does the view. The parent's place there is checked in the same hold of g_smu as the view's entry
// changes, so no entry names a view whose parent had already gone. Publishing first and taking the entry back with serial_del when the parent turned out gone (as tex_mtl_ did)
// let another thread resolve the view in between, and for a finalized serial left it in the ghost table for good: serial_del ends a live entry only, which is what a finalize wants.
static id view_parent(id o) { return [o respondsToSelector:@selector(parentTexture)] ? ((id<MTLTexture>)o).parentTexture : nil; }
static int view_kept(id o) { return own_keeps_locked(o); }   // a pinned or shared object keeps its view valid
int own_view_put(uint64_t k, const alias_key *pk, id<MTLTexture> view) {
    pthread_mutex_lock(&g_smu);
    id p = view_parent(view); int ok = p && hm_get_id(&g_alias, pk) == p;
    if (ok) { alias_seen(pk, own_hook_submission()); tex_put_locked(k, view); }
    pthread_mutex_unlock(&g_smu); return ok;
}
int own_view_drop_stale(uint64_t k, id view) {
    pthread_mutex_lock(&g_smu);
    id p = view_parent(view); int stale = p && !view_kept(view) && !view_kept(p);
    if (stale) {
        id live = hm_get_id(&g_by_serial, &k);
        if (live == view) live_del_locked(k);
        else if (!live && hm_get_id(&g_ghost, &k) == view) ghost_del_locked(k);   // an entry another thread changed meanwhile is not this view's: it stays
    }
    pthread_mutex_unlock(&g_smu); return stale;
}

int own_rename_locked(id from, id to, uint64_t serial) {
    nvn_hmap *tables[] = {&g_by_serial, &g_ghost}; int named = 0;
    for (unsigned j = 0; j < 2; j++) for (uint32_t i = 0; i < tables[j]->cap; i++) {
        if (tables[j]->vals[i] != (__bridge void *)from) continue;
        uint64_t k; memcpy(&k, tables[j]->keys + (size_t)i * tables[j]->klen, sizeof k);
        void *old = tables[j]->vals[i]; tables[j]->vals[i] = (void *)CFBridgingRetain(to);
        nvn_hmap *cnt = j == 0 ? &g_by_serial_held : &g_ghost_held; count_add(cnt, old, -1); count_add(cnt, (__bridge void *)to, 1);   // the count moves with the name
        CFRelease(old); named |= k == serial;
    }
    for (uint32_t i = 0; i < g_tex_pins.cap; i++) if (g_tex_pins.vals[i]) { tex_pin *pin = g_tex_pins.vals[i]; if (pin->object == (__bridge void *)from) tex_pin_set(pin, to); }
    int shared = 0;   // the sharing table: values replaced in place (its keys, cells and last lookups stay)
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (g_alias.vals[i] != (__bridge void *)from) continue;
        void *old = g_alias.vals[i]; g_alias.vals[i] = (void *)CFBridgingRetain(to); CFRelease(old); shared = 1;
    }
    if (shared) { CFSetRemoveValue(g_alias_set, (__bridge const void *)from); CFSetAddValue(g_alias_set, (__bridge const void *)to); }
    gone_note((__bridge const void *)from);   // its counts went to `to` above while it was still shared
    return named;
}
// The rename and the asking serial's new object in one hold: published after g_smu was let go (as the upgrade did), a reader in between found the serial's old view, of a parent no
// longer shared
int own_upgrade(id from, id to, uint64_t serial, id put) {
    pthread_mutex_lock(&g_smu);
    int named = own_rename_locked(from, to, serial);
    own_epoch();   // every serial that named `from` resolves to `to` now: no one note
    if (put && !(named && put == to)) tex_put_locked(serial, put);
    pthread_mutex_unlock(&g_smu); return named;
}

// A submitted descriptor and a queue binding hold the compiled program by its immutable initialization serial. Retaining it must not mint serial aliases:
// alias churn invalidates texture tables and changes composite-program keys
typedef struct { unsigned refs, retired; } prog_pin;
static nvn_hmap g_prog_pins = {.klen = 8};   // protected by the submission lock
void mtl_prog_release(prog_t *p) {
    if (!p->serial) return;
    prog_pin *pin = nvn_hm_get(&g_prog_pins, &p->serial);
    if (pin) pin->retired = 1; else serial_del(p->serial);
}
void mtl_prog_unpin(prog_t *p) {
    prog_pin *pin = nvn_hm_get(&g_prog_pins, &p->serial);
    if (pin && !--pin->refs) { nvn_hm_del(&g_prog_pins, &p->serial); if (pin->retired) serial_del(p->serial); free(pin); }
}
void own_prog_pin(uint64_t serial) {
    prog_pin *pin = nvn_hm_get(&g_prog_pins, &serial);
    if (!pin) { pin = calloc(1, sizeof *pin); if (!pin) ODIE("program retention allocation failed"); nvn_hm_put(&g_prog_pins, &serial, pin); }
    pin->refs++;
}

uint32_t own_alias_unref_locked(void) {
    CFMutableSetRef held = CFSetCreateMutable(NULL, 0, NULL); uint32_t n = 0;
    own_each_locked(^(uint64_t serial, id o, int ghost) { (void)serial; (void)ghost; CFSetAddValue(held, (__bridge const void *)o); });
    for (uint32_t i = 0; i < g_alias.cap; i++) if (g_alias.vals[i] && !CFSetContainsValue(held, g_alias.vals[i])) n++;
    CFRelease(held); return n;
}
own_stats own_stats_locked(void) { return (own_stats){.live = g_by_serial.n, .live_cap = g_by_serial.cap, .ghost = g_ghost.n, .retire_wait = g_retire_wait.n, .ghost_puts = g_ghost_puts}; }
void own_each_locked(void (^fn)(uint64_t serial, id o, int ghost)) {
    const nvn_hmap *tables[] = {&g_by_serial, &g_ghost};
    for (int j = 0; j < 2; j++) for (uint32_t i = 0; i < tables[j]->cap; i++) {
        if (!tables[j]->vals[i]) continue;
        uint64_t k; memcpy(&k, tables[j]->keys + (size_t)i * 8, 8); fn(k, (__bridge id)tables[j]->vals[i], j);
    }
}
static void expect_one(CFMutableDictionaryRef want, id o) {   // o, and each texture it is a view of, once more
    for (; o; o = [o respondsToSelector:@selector(parentTexture)] ? ((id<MTLTexture>)o).parentTexture : nil)
        CFDictionarySetValue(want, (__bridge const void *)o, (const void *)((uintptr_t)CFDictionaryGetValue(want, (__bridge const void *)o) + 1));
}
static int counts_match(const char *what, CFMutableDictionaryRef want, const nvn_hmap *held, char *why, size_t n) {
    for (uint32_t i = 0; i < held->cap; i++) {
        if (!held->vals[i]) continue;
        uintptr_t k; memcpy(&k, held->keys + (size_t)i * held->klen, sizeof k);
        uintptr_t w = (uintptr_t)CFDictionaryGetValue(want, (const void *)k);
        if (w != (uintptr_t)held->vals[i]) { snprintf(why, n, "%s count of %#lx: %lu, the tables say %lu", what, (unsigned long)k, (unsigned long)(uintptr_t)held->vals[i], (unsigned long)w); return 0; }
    }
    if ((CFIndex)held->n != CFDictionaryGetCount(want)) { snprintf(why, n, "%s counts: %u objects, the tables hold %ld", what, held->n, (long)CFDictionaryGetCount(want)); return 0; }
    return 1;
}
int own_check_locked(char *why, size_t n) {
    CFMutableDictionaryRef live = CFDictionaryCreateMutable(NULL, 0, NULL, NULL), ghost = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    for (uint32_t i = 0; i < g_by_serial.cap; i++) if (g_by_serial.vals[i]) expect_one(live, (__bridge id)g_by_serial.vals[i]);
    for (uint32_t i = 0; i < g_tex_pins.cap; i++) { tex_pin *pin = g_tex_pins.vals[i]; if (pin && pin->object) expect_one(live, (__bridge id)pin->object); }
    for (uint32_t i = 0; i < g_ghost.cap; i++) if (g_ghost.vals[i]) expect_one(ghost, (__bridge id)g_ghost.vals[i]);
    int ok = counts_match("live", live, &g_by_serial_held, why, n) && counts_match("ghost", ghost, &g_ghost_held, why, n);
    // Each ghost serial has one place in the list of use and each place a ghost serial (a place left behind would evict a later entry of the serial, one missing would keep its
    // entry for good), the list's links agree both ways, and its uses go from oldest to newest (ghost_trim stops at the first entry in use)
    uint32_t places = 0; const ghost_use *prev = NULL;
    for (const ghost_use *u = g_ghost_lru; ok && u; prev = u, u = u->next) {
        if (++places > g_ghost_use.n) { snprintf(why, n, "the ghost list is longer than its %u records (a cycle)", g_ghost_use.n); ok = 0; break; }
        if (u->prev != prev) { snprintf(why, n, "ghost list: serial %llu's back link is wrong", (unsigned long long)u->serial); ok = 0; }
        else if (nvn_hm_get(&g_ghost_use, &u->serial) != u) { snprintf(why, n, "ghost list: serial %llu's record is not the one its table names", (unsigned long long)u->serial); ok = 0; }
        else if (!nvn_hm_get(&g_ghost, &u->serial)) { snprintf(why, n, "ghost list: serial %llu has no ghost entry", (unsigned long long)u->serial); ok = 0; }
        else if (prev && (u->frame < prev->frame || u->sub < prev->sub)) { snprintf(why, n, "ghost list: serial %llu (frame %llu, submission %llu) used before the one ahead of it", (unsigned long long)u->serial, (unsigned long long)u->frame, (unsigned long long)u->sub); ok = 0; }
    }
    if (ok && prev != g_ghost_mru) { snprintf(why, n, "the ghost list does not end at its most recently used entry"); ok = 0; }
    if (ok && (places != g_ghost.n || g_ghost_use.n != g_ghost.n)) { snprintf(why, n, "%u places in the ghost list, %u records, %u ghost serials", places, g_ghost_use.n, g_ghost.n); ok = 0; }
    // The sharing table: g_alias_set is the set of its textures, each key lies in every cell its bytes reach and in no other, and a last lookup is kept for entries only
    CFMutableSetRef vals = CFSetCreateMutable(NULL, 0, NULL); uint64_t want_ents = 0;
    for (uint32_t i = 0; ok && i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        const alias_key *k = (const alias_key *)(const void *)(g_alias.keys + (size_t)i * g_alias.klen);
        CFSetAddValue(vals, g_alias.vals[i]);
        if (!own_shared_locked((__bridge id)g_alias.vals[i])) { snprintf(why, n, "shared texture %p (pool %#llx offset %#llx) is not in the set of shared textures", g_alias.vals[i], (unsigned long long)k->pool, (unsigned long long)k->off); ok = 0; break; }
        nvn_texlayout L; nvn_tex_layout(&k->d, &L);
        int64_t end = k->off + (int64_t)L.size, c1 = (end > k->off ? end - 1 : k->off) >> ALIAS_CELL;
        for (int64_t c = k->off >> ALIAS_CELL; ok && c <= c1; c++) {
            uint64_t ck[2] = {k->pool, (uint64_t)c}; const alias_cell *v = nvn_hm_get(&g_alias_cells, ck); uint32_t j = 0;
            if (v) for (; j < v->n; j++) if (!memcmp(&v->e[j].k, k, sizeof *k)) break;
            if (!v || j == v->n) { snprintf(why, n, "shared entry (pool %#llx offset %#llx) is missing from cell %lld of the index", (unsigned long long)k->pool, (unsigned long long)k->off, (long long)c); ok = 0; }
            want_ents++;
        }
    }
    uint64_t ents = 0;
    for (uint32_t i = 0; ok && i < g_alias_cells.cap; i++) if (g_alias_cells.vals[i]) ents += ((const alias_cell *)g_alias_cells.vals[i])->n;
    if (ok && ents != want_ents) { snprintf(why, n, "%llu index entries for the %llu cells the shared entries reach", (unsigned long long)ents, (unsigned long long)want_ents); ok = 0; }
    CFIndex nset = g_alias_set ? CFSetGetCount(g_alias_set) : 0;
    if (ok && nset != CFSetGetCount(vals)) { snprintf(why, n, "%ld textures in the set of shared textures, %ld in the table", (long)nset, (long)CFSetGetCount(vals)); ok = 0; }
    for (uint32_t i = 0; ok && i < g_alias_seen.cap; i++)
        if (g_alias_seen.vals[i] && !nvn_hm_get(&g_alias, g_alias_seen.keys + (size_t)i * g_alias_seen.klen)) { snprintf(why, n, "a last lookup kept for a key with no shared entry"); ok = 0; }
    CFRelease(vals);
    // A retired object waits only while an entry holds it: the last count going hands it back (retire_resume)
    for (uint32_t i = 0; ok && i < g_retire_wait.cap; i++)
        if (g_retire_wait.vals[i] && !entry_holds(g_retire_wait.vals[i])) { snprintf(why, n, "retired object %p waits with no entry holding it", g_retire_wait.vals[i]); ok = 0; }
    CFRelease(live); CFRelease(ghost);
    return ok;
}
