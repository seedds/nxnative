// The NVN resource owner (nvn_owner.m): what an NVN texture or program serial resolves to, which texture holds a storage's bytes (the sharing table), who holds each object, the
// change generation of all that, and whether a retired object may leave the residency set. The replay layer (nvn_mtl.m) makes the objects, encodes with them, keeps the
// deferred-release queue and the residency set; it changes the owner's state only through the functions below.
//
// Tables (all under g_smu but the program pins):
//   live    serial → object (MTLTexture, NvPkg, NvProg) of a live serial: serial_put / serial_del, and tex_put for a texture whose object is live or an external texture (2^62 and up)
//   ghost   serial → object of a finalized texture serial a replayed record still resolves (tex_put), a ring of NGHOST: the oldest goes when a new serial comes; a stale view's
//           entry goes with its slot (own_view_drop_stale). Each ghost serial has one ring slot
//   tex live    serial → the texture object it was initialized in (mtl_tex_init until its finalize), which decides between live and ghost
//   tex pins    serial → a retained object a queued descriptor or saved binding still resolves after the serial's entries go (mtl_tex_pin / mtl_tex_unpin)
//   prog pins   serial → pins on a program a queued descriptor or saved binding holds: its release waits for the last unpin (under the submission lock, not g_smu)
//   sharing     alias_key (pool, offset, description) → the texture that holds those bytes (own_alias_*): NVN textures over the same storage with the same description are one
//               texture. Kept resident by the table; an entry goes when its pool is finalized (own_alias_drop_pool_locked) or a new texture takes its bytes over (own_alias_publish).
//               With it: an index by pool and 64 KB cell (the takeover's candidates, the image families' lookup), the submission of each key's last lookup, and the set of its textures
//   waiting     retired objects an entry still holds (own_retire_locked), retained until the last entry lets them go
// Every live entry and every texture pin counts its object, and each parent of a view, in the live held count; every ghost entry in the ghost held count. The sharing table holds its
// textures apart from those counts. own_check_locked checks the counts against the tables, the sharing table's index, set and last lookups, and the waiting objects.
//
// Lifetime of what a lookup returns: borrowed. The owner never releases an object it lets go: it hands it to the replay layer (own_hook_drop), which releases it once the submission in
// flight has completed (defer_obj_release / defer_obj_unref). So an object looked up while encoding stays valid at least until the submission it is encoded in completes. A caller that
// keeps one longer retains it (a strong reference).
//
// Retirement: GPU completion and eligibility are two conditions. The replay layer's queue waits for the first (the fence of the submission in flight at the retirement); then it asks
// the owner for the second (own_retire_locked: no entry, pin or view's entry holds the object). An object held waits in the owner, which hands it back to the queue
// (own_hook_drop(o, 0), behind the submission then in flight) when its last count goes, unless it is pinned or shared by then. The queue entry's reference is released by the drain;
// the owner's by the hand-back; at quit nothing is drained (the process exits with both).
//
// Generation: g_ser_gen counts every change of what a serial resolves to; own_note names the serial, own_epoch names none (every cache entry is checked again).
//
// Lock order: g_smu, then the replay layer's g_cpu_tex_mu; g_smu, then the notes' lock. nvn_mtl.m holds g_smu for its own tables beside these (the shared packaged textures
// g_pkg_share, the image descriptions), and calls the _locked functions under it.
#pragma once
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>
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

// ---- What the replay layer calls

// Lookups (borrowed, see above)
const void *own_live_ref(uint64_t k);       // the live entry
const void *own_resolve_ref(uint64_t k);    // live, then ghost, then a texture pin
static inline id serial_get(uint64_t k) { return (__bridge id)own_live_ref(k); }
static inline id tex_get(uint64_t k) { return (__bridge id)own_resolve_ref(k); }

// Changes (each takes g_smu)
void serial_put(uint64_t k, id o);   // a live serial's object; the one it replaces is dropped
void serial_del(uint64_t k);         // a live serial's entry goes (a finalize, a program's release); a ghost entry of the serial stays, as a replayed record may still resolve it
void tex_put(uint64_t k, id o);      // a texture serial's object: live table for a live texture object or an external texture, else the ghost ring

// The sharing table. A key's padding is zero (it is hashed): build one with memset or a zeroed initializer, and change a copy's fields
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } alias_key;
enum { ALIAS_CELL = 16,    // the index's cells: 64 KB of a pool
       ALIAS_IDLE = 16 };  // a takeover spares an entry a lookup found in the last ALIAS_IDLE submissions
// Resolution: each takes g_smu, and a texture it publishes under a serial is published in the same hold as the lookup or the check (borrowed, as a serial lookup)
id own_alias_take(uint64_t serial, const alias_key *k);                 // k's shared texture, marked looked up and published under serial (tex_put); nil (nothing changed) when k has none
id own_alias_publish(uint64_t serial, const alias_key *k, id o);        // registration: o takes k's bytes over (the entries of the pool it overlaps under another description go, unless a
                                                                        // live entry or a pin holds them or a lookup found them lately), goes in under k and is published under serial.
                                                                        // → what serial resolves to now: o, or the texture another thread registered under k first (o is then the caller's to retire)
id own_alias_peek(const alias_key *k);                                  // k's shared texture (not marked looked up)
// sRGB / linear views of a shared texture (tex_mtl_'s twins). A view is valid while its parent is in the sharing table or it or its parent is pinned; each decision is taken in the
// same hold of g_smu as the entry it changes (docs/reference/nvn-objects.md, A finalized twin view left in the ghost ring)
int own_view_put(uint64_t k, const alias_key *pk, id<MTLTexture> view);   // tex_put of the view only while its parent is pk's shared texture (pk marked looked up): 1 published, 0 nothing changed
int own_view_drop_stale(uint64_t k, id view);        // a view no longer valid: 1, and k's live entry, or with none its ghost entry (and ring slot), goes if it still names the view; 0 valid
// An upgrade (a texture made again with more usage): every name of `from`, the serial tables, the pins and the sharing table, moves to `to` in place (a new epoch); then `put` (when not
// nil) is published under serial unless the rename already named it there (an sRGB / linear twin passes its view made again on `to`). One hold of g_smu. → whether serial was one of from's names
int own_upgrade(id from, id to, uint64_t serial, id put);
// Caller holds g_smu
uint32_t own_alias_drop_pool_locked(const void *pool);        // a pool's Finalize: its entries go (retired, own_hook_drop; a new epoch). → how many
void own_alias_at_locked(const void *pool, int64_t off, void (^fn)(const alias_key *k, id o));   // the entries that start at off in the pool (its cell of the index)
void own_alias_each_locked(void (^fn)(const alias_key *k, id o));
typedef struct { uint32_t n, cap, cells; uint64_t evicted, taken; } own_alias_stats;   // entries, capacity, index cells, evicted with pools / on takeover in total
own_alias_stats own_alias_stats_locked(void);
uint32_t own_alias_unref_locked(void);                        // entries no live or ghost entry resolves to (a walk: the memory report)

// Retirement eligibility (see above). Caller holds g_smu
int own_retire_locked(id o);    // a retired resource whose fence has passed: 1, it may leave the residency set; 0, an entry holds it: the owner keeps it (the caller drops only its reference)
int own_keeps_locked(id o);     // o stays resident whatever its entries do: pinned or shared
int own_object_free(id o);      // takes g_smu: no entry, pin or view's entry holds o (the replay layer may write it over: a package's spare texture)

// Live texture objects. Caller holds g_smu
void own_tex_live_put_locked(const tex_t *t);   // mtl_tex_init
int own_tex_live_locked(uint64_t serial);
int own_tex_live(uint64_t serial);
int own_tex_end_locked(tex_t *t);   // a finalize: -1 when t is a copy and the object the texture was initialized in still holds it (nothing ends), else whether it was live (it is not now)

// Program pins (the caller holds the submission lock); mtl_prog_release and mtl_prog_unpin (nvn_obj.h) are the other two
void own_prog_pin(uint64_t serial);

// The change generation. g_ser_gen is read directly (an atomic load on the hot paths); only the owner's functions change it
enum { SER_NOTES = 64,     // notes kept between two takes
       SER_LOG = 4096 };   // notes kept by generation: a cache further behind rebuilds whole
extern _Atomic uint64_t g_ser_gen;
void own_note(uint64_t serial);                                  // what serial stands for changed (the replay layer: a packaged texture's upload replaced); +1
void own_epoch(void);                                            // a change no one serial names (an upload lost); +1
unsigned own_notes_take(uint64_t out[SER_NOTES], uint64_t *gen); // the serials noted since the last take (SER_NOTES of them at most: then as many as were noted), and the generation they reach
int own_note_at(uint64_t gen, uint64_t *serial);                 // the serial noted at generation gen (0 for a change that touches no one serial's texture, a takeover): 1; 0 when that
                                                                 // change made no note (own_epoch) or the log has been overwritten since: the reader rebuilds whole

// The tables' sizes, and every object they hold (live entries first, then ghost ones). Caller holds g_smu
typedef struct { uint32_t live, live_cap, ghost, retire_wait; uint64_t ghost_puts; } own_stats;
own_stats own_stats_locked(void);
void own_each_locked(void (^fn)(uint64_t serial, id o, int ghost));

// ---- What the replay layer provides (nvn_mtl.m). Called with g_smu held
void own_hook_drop(id o, int keep_resident);   // an object the owner lets go: released after the submission in flight (keep_resident: it stays in the residency set, as something else holds it)
uint64_t own_hook_submission(void);            // the submission being encoded (g_fence + 1): a lookup's mark, which ALIAS_IDLE counts from

// ---- Queries and seams for the tests (t_serial_owner links the owner alone; the replay layer does not call these). Caller holds g_smu
const void *own_live_ref_locked(uint64_t k);
int serial_table_holds(const void *o);   // a live entry or a texture pin holds o (or a view of it)
int entry_holds(const void *o);          // that, or a ghost entry
int tex_object_pinned(id o);
int own_shared_locked(id o);             // the sharing table holds o
id own_alias_get_locked(const alias_key *k);
int own_rename_locked(id from, id to, uint64_t serial);                // own_upgrade's rename alone (no epoch). → whether `serial` was one of from's names
void own_alias_put_locked(const alias_key *k, id o, uint64_t seen);   // o under k, retiring an entry it replaces; seen: the submission of its last lookup (0: none)
int own_alias_del_locked(const alias_key *k);                          // 1 when k had an entry (retired), 0
uint32_t own_alias_take_over_locked(const alias_key *k);               // the takeover alone. → how many went
int own_check_locked(char *why, size_t n);   // the invariants: 1, or 0 with the first difference in why
