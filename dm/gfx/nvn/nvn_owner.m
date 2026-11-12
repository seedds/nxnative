// The serial tables' owner: nvn_owner.h says what it holds, what a lookup returns and the lock order. Moved from nvn_mtl.m as it was (only the hooks are new).
#import <Metal/Metal.h>
#include "nvn_owner.h"
#include "../../host/dm.h"
#include <mach/mach.h>

#define ODIE(...) do { dm_log("[dm] [nvn-mtl] FATAL " __VA_ARGS__); abort(); } while (0)

pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_by_serial = {.klen = 8};     // Serial → MTLTexture / NvPkg / NvProg
// How many entries of g_by_serial hold each object (key: its address; a view's parents too, count_add), kept by every writer of g_by_serial (serial_put, serial_del, tex_put, the upgrade's in-place
// replacement), so alias_take_over asks whether a live texture object still resolves to a shared texture in one lookup. It walked the serial table's whole capacity for
// each candidate instead: in Ocarina of Time's cutscenes, with about 7,500 serials and thousands of shared textures, that walk was the submit thread's hottest function and
// frames took up to a second. An entry holds its object (CFBridgingRetain), so a counted address cannot be reused while counted. Under g_smu
static nvn_hmap g_by_serial_held = {.klen = sizeof(uintptr_t)};
// The same count for the ghost ring (g_ghost, finalized serials a replayed record still resolves; tex_put and the upgrade's in-place replacement write it)
static nvn_hmap g_ghost_held = {.klen = sizeof(uintptr_t)};
// Texture serials owned by a live NVN texture object (TextureInitialize until Finalize or re-initialization, hle_nvn.c). Command records and texture pool registrations snapshot the
// texture object (tex_snap in hle_nvn.c, mtl_reg_tex), so replay can resolve a texture the game has already finalized: Breath of the Wild records each frame's transient render targets
// (the HUD captures, the 640x360 half-resolution buffers) and finalizes their objects before it submits. Those serials went back into g_by_serial and stayed there for good, as no second
// Finalize comes: about 10 a frame, 200,000 after an hour in the first shrine. A dead serial resolves through g_ghost instead, a small table whose oldest entry goes when a new one comes,
// so a record replayed again soon still finds its texture
static nvn_hmap g_tex_live = {.klen = 8};   // serial → the texture object it was initialized in (mtl_tex_release), under g_smu
static nvn_hmap g_ghost = {.klen = 8}; static uint64_t g_ghost_ring[NGHOST]; static unsigned g_ghost_next; static uint64_t g_ghost_puts;
// Deferred command descriptors and persistent bindings retain a texture serial independently of the guest descriptor and the bounded recently-finalized cache
typedef struct { unsigned refs; void *object; } tex_pin;
static nvn_hmap g_tex_pins = {.klen = 8}, g_tex_pin_objects = {.klen = sizeof(uintptr_t)};

int serial_table_holds(const void *o) { uintptr_t k = (uintptr_t)o; return nvn_hm_get(&g_by_serial_held, &k) != NULL; }
int entry_holds(const void *o) { uintptr_t k = (uintptr_t)o; return serial_table_holds(o) || (g_ghost_held.n && nvn_hm_get(&g_ghost_held, &k)); }
int tex_object_pinned(id o) { uintptr_t key = (uintptr_t)(__bridge void *)o; return o && nvn_hm_get(&g_tex_pin_objects, &key) != NULL; }
static void count_one(nvn_hmap *m, const void *o, int d) {
    if (!o) return;
    uintptr_t k = (uintptr_t)o, n = (uintptr_t)nvn_hm_get(m, &k);
    if (d > 0) nvn_hm_put(m, &k, (void *)(n + 1));
    else if (n > 1) nvn_hm_put(m, &k, (void *)(n - 1));
    else if (n) { nvn_hm_del(m, &k); own_hook_unheld(o); }   // a slot written past these helpers (a test's table) was never counted: nothing to take away
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
static void obj_drop(id o) { own_hook_drop(o, tex_object_pinned(o) || own_hook_shared(o)); }   // caller holds g_smu

const void *own_live_ref_locked(uint64_t k) { return nvn_hm_get(&g_by_serial, &k); }
const void *own_live_ref(uint64_t k) { pthread_mutex_lock(&g_smu); const void *o = nvn_hm_get(&g_by_serial, &k); pthread_mutex_unlock(&g_smu); return o; }
void serial_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { held_add((__bridge void *)old, -1); obj_drop(old); } hm_put_id(&g_by_serial, &k, o); held_add((__bridge void *)o, 1); own_hook_changed(k); pthread_mutex_unlock(&g_smu); }
void serial_del(uint64_t k) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) { held_add((__bridge void *)old, -1); obj_drop(old); hm_del_id(&g_by_serial, &k); own_hook_changed(k); } pthread_mutex_unlock(&g_smu); }   // nothing to drop: the texture

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
        held_add(pin->object, -1); own_hook_drop(old, tex_object_pinned(old) || own_hook_shared(old) || serial_table_holds((__bridge void *)old)); CFRelease(pin->object); pin->object = NULL;
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
const void *own_resolve_ref(uint64_t k) {
    pthread_mutex_lock(&g_smu); const void *o = nvn_hm_get(&g_by_serial, &k); if (!o && g_ghost.n) o = nvn_hm_get(&g_ghost, &k); if (!o) { tex_pin *pin = nvn_hm_get(&g_tex_pins, &k); if (pin) o = pin->object; } pthread_mutex_unlock(&g_smu); return o;
}
void tex_put(uint64_t k, id o) {   // serials from 2^62 up are external textures (mtl_ext_tex), never initialized by the game
    pthread_mutex_lock(&g_smu);
    nvn_hmap *m = k >= (1ull << 62) || nvn_hm_get(&g_tex_live, &k) ? &g_by_serial : &g_ghost;
    id old = hm_get_id(m, &k);
    if (old) count_add(m == &g_by_serial ? &g_by_serial_held : &g_ghost_held, (__bridge void *)old, -1);
    if (old) obj_drop(old);
    else if (m == &g_ghost) {
        uint64_t *slot = &g_ghost_ring[g_ghost_next++ % NGHOST];
        if (*slot) { id ev = hm_get_id(&g_ghost, slot); if (ev) { ghost_held_add((__bridge void *)ev, -1); obj_drop(ev); } hm_del_id(&g_ghost, slot); own_hook_changed(*slot); }   // the evicted serial resolves anew too
        *slot = k; g_ghost_puts++;
    }
    hm_put_id(m, &k, o); tex_pin *pin = nvn_hm_get(&g_tex_pins, &k); if (pin) tex_pin_set(pin, o); own_hook_changed(k);
    count_add(m == &g_by_serial ? &g_by_serial_held : &g_ghost_held, (__bridge void *)o, 1);
    pthread_mutex_unlock(&g_smu);
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
    return named;
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

own_stats own_stats_locked(void) { return (own_stats){.live = g_by_serial.n, .live_cap = g_by_serial.cap, .ghost = g_ghost.n, .ghost_puts = g_ghost_puts}; }
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
    if (ok && g_ghost.n > NGHOST) { snprintf(why, n, "%u ghost serials, more than the ring's %d", g_ghost.n, NGHOST); ok = 0; }
    CFRelease(live); CFRelease(ghost);
    return ok;
}
