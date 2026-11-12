// The serial tables' owner (nvn_owner.m): what an NVN texture or program serial resolves to, and who holds each object. The replay layer (nvn_mtl.m) makes the objects, encodes
// with them and retires them; it changes these tables only through the functions below.
//
// Tables (all under g_smu but the program pins):
//   live    serial → object (MTLTexture, NvPkg, NvProg) of a live serial: serial_put / serial_del, and tex_put for a texture whose object is live or an external texture (2^62 and up)
//   ghost   serial → object of a finalized texture serial a replayed record still resolves (tex_put), a ring of NGHOST: the oldest goes when a new serial comes; a stale view's
//           entry goes with its slot (own_view_drop_stale). Each ghost serial has one ring slot
//   tex live    serial → the texture object it was initialized in (mtl_tex_init until its finalize), which decides between live and ghost
//   tex pins    serial → a retained object a queued descriptor or saved binding still resolves after the serial's entries go (mtl_tex_pin / mtl_tex_unpin)
//   prog pins   serial → pins on a program a queued descriptor or saved binding holds: its release waits for the last unpin (under the submission lock, not g_smu)
// Every live entry and every texture pin counts its object, and each parent of a view, in the live held count; every ghost entry in the ghost held count (entry_holds asks both).
// own_check_locked checks the counts against the tables.
//
// Lifetime of what a lookup returns: borrowed. The owner never releases an object it lets go: it hands it to the replay layer (own_hook_drop), which releases it once the submission in
// flight has completed (defer_obj_release / defer_obj_unref). So an object looked up while encoding stays valid at least until the submission it is encoded in completes. A caller that
// keeps one longer retains it (a strong reference). Every change of a serial's object is reported (own_hook_changed: ser_note, which bumps g_ser_gen for the replay layer's caches).
//
// Lock order: g_smu, then the replay layer's g_cpu_tex_mu. Until the sharing table moves here, nvn_mtl.m's sharing table and the upgrade path use g_smu as well.
#pragma once
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdint.h>
#include "nvn_hmap.h"
#include "nvn_obj.h"
@protocol MTLTexture;

// Tables from serial (or key) to a retained object
#ifdef __clang_analyzer__
void dm_analyzer_map_takes(CFTypeRef CF_CONSUMED ref);   // a declaration with no body: hands the reference to the map, which is what the map does (the analyzer cannot follow it into nvn_hmap)
#endif
static inline id hm_get_id(const nvn_hmap *m, const void *k) { return (__bridge id)nvn_hm_get(m, k); }
static inline void hm_put_id(nvn_hmap *m, const void *k, id o) {   // the map owns the reference CFBridgingRetain makes until the entry is replaced (released here) or removed (hm_del_id)
    CFTypeRef ref = CFBridgingRetain(o);
#ifdef __clang_analyzer__
    dm_analyzer_map_takes(ref);
#endif
    void *old = nvn_hm_put(m, k, (void *)ref); if (old) CFRelease(old);
}
static inline void hm_del_id(nvn_hmap *m, const void *k) { void *old = nvn_hm_del(m, k); if (old) CFRelease(old); }

extern pthread_mutex_t g_smu;
enum { NGHOST = 512 };

// Lookups (borrowed, see above)
const void *own_live_ref(uint64_t k);       // the live entry
const void *own_resolve_ref(uint64_t k);    // live, then ghost, then a texture pin
const void *own_live_ref_locked(uint64_t k);
static inline id serial_get(uint64_t k) { return (__bridge id)own_live_ref(k); }
static inline id tex_get(uint64_t k) { return (__bridge id)own_resolve_ref(k); }

// Changes (each takes g_smu)
void serial_put(uint64_t k, id o);   // a live serial's object; the one it replaces is dropped
void serial_del(uint64_t k);         // a live serial's entry goes (a finalize, a program's release); a ghost entry of the serial stays, as a replayed record may still resolve it
void tex_put(uint64_t k, id o);      // a texture serial's object: live table for a live texture object or an external texture, else the ghost ring

// sRGB / linear views of a shared texture (tex_mtl_'s twins). A view is valid while its parent is in the sharing table (own_hook_shared) or it or its parent is pinned; each decision
// is taken in the same hold of g_smu as the entry it changes (docs/reference/nvn-objects.md, A finalized twin view left in the ghost ring)
int own_view_put(uint64_t k, id<MTLTexture> view);   // tex_put of the view only while its parent is shared: 1 published, 0 nothing changed (the parent left the sharing table)
int own_view_drop_stale(uint64_t k, id view);        // a view no longer valid: 1, and k's live entry, or with none its ghost entry (and ring slot), goes if it still names the view; 0 valid

// Live texture objects. Caller holds g_smu
void own_tex_live_put_locked(const tex_t *t);   // mtl_tex_init
int own_tex_live_locked(uint64_t serial);
int own_tex_live(uint64_t serial);
int own_tex_end_locked(tex_t *t);   // a finalize: -1 when t is a copy and the object the texture was initialized in still holds it (nothing ends), else whether it was live (it is not now)

// Who holds an object. Caller holds g_smu
int serial_table_holds(const void *o);   // a live entry or a texture pin
int entry_holds(const void *o);          // that, or a ghost entry
int tex_object_pinned(id o);

// Every name of `from` (live and ghost entries, texture pins) moves to `to`, counts with them, in place (no table grows). Caller holds g_smu. → whether `serial` was one of them
int own_rename_locked(id from, id to, uint64_t serial);

// Program pins (the caller holds the submission lock); mtl_prog_release and mtl_prog_unpin (nvn_obj.h) are the other two
void own_prog_pin(uint64_t serial);

// The tables' sizes, and every object they hold (live entries first, then ghost ones). Caller holds g_smu
typedef struct { uint32_t live, live_cap, ghost; uint64_t ghost_puts; } own_stats;
own_stats own_stats_locked(void);
void own_each_locked(void (^fn)(uint64_t serial, id o, int ghost));
int own_check_locked(char *why, size_t n);   // the held counts against the tables and pins: 1, or 0 with the first difference in why

// What the replay layer provides (nvn_mtl.m). Called with g_smu held
void own_hook_drop(id o, int keep_resident);   // an object the owner lets go: released after the submission in flight (keep_resident: it stays in the residency set, as something else holds it)
int own_hook_shared(id o);                     // whether the sharing table holds it
void own_hook_unheld(const void *o);           // its last count in a table went (a retired object waiting for its holders resumes)
void own_hook_changed(uint64_t serial);        // a serial's object changed (0: no one serial)
