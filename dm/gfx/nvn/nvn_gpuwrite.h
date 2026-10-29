// The ledger of GPU writes to guest memory, shared by every graphics backend (pure C, every host): the writes queued in the submission being encoded, which the GPU has not run yet
// (copies, inline uniform updates, fills, shader storage writes, counter reports), and the pages earlier submissions' writes cover until their fence completes. Barriers, CPU reads
// of guest memory and uniform shadows ask it. Moved out of the Metal replay layer (nvn_mtl.m: pend_*, sbw_*, gw_*) with no change of logic (docs/design/vulkan-backend.md,
// sections 1.3 and 1.7, step M2); nvn_mtl.m keeps the old names as aliases. It has no lock, as before the move: it is used on the submit thread only, under hle_nvn.c's submission lock
#pragma once
#include <stdint.h>

// ---------- Writes queued in this submission (pend) ----------
// Target ranges of buffer writes (copies / inline uniform updates) already encoded in this submission but not yet executed on the GPU: reading these addresses on the CPU side at encode time yields old values (uv_proof avoids them when reading uniforms)
// Inline updates keep where their data sits in the update ring (g_pend_upd: index + 1 into g_upd), so a bindless handle they wrote can be read as the GPU will see it (nvn_gw_pend_read).
// Monster Hunter Rise queues more than 256 writes in a submission (the old limit, after which every address counted as written). The writes are indexed by 4 KB page (a chain per hash
// bucket, newest first); a write over more than PEND_SPAN pages goes to a short list that every lookup scans
enum { PEND_MAX = 8192, PEND_HB = 2048, PEND_NODES = 16384, PEND_SPAN = 8, PEND_WIDE = 256 };
extern uint64_t g_pend_lo[PEND_MAX], g_pend_hi[PEND_MAX]; extern int g_npend, g_pend_over; extern uint8_t g_pend_kind[PEND_MAX]; extern uint16_t g_pend_upd[PEND_MAX];   // kind: 1 inline update, 2 copy, 3 fill
extern uint32_t g_pend_gen;   // this submission's generation (bumped by nvn_gw_reset)
// An inline update's bytes for [lo, lo + its size): in the backend's update ring, whose block lives until this submission completes (nvn_mtl.m mtl_sub_update)
typedef struct { uint64_t lo; const uint8_t *data; } upd_ent;
extern upd_ent g_upd[PEND_MAX];

// ---------- Other GPU writes in this submission (sbw) ----------
// Other writes to guest memory in this submission, which the pend list does not hold: writable storage buffers bound to a shader, texture→buffer copies, counter reports (ubp_get).
// Kept by 4 KB page (Breath of the Wild's particle dispatches bind about ten storage buffers each, hundreds a frame): a set of pages tagged with the submission (g_pend_gen) and with what
// wrote them. A counter report does not count against this submission's shadows: the host writes reports on the CPU when the whole submission has completed, so no draw of it reads one
// (Fire Emblem Engage writes its query reports into the page of a uniform block); it counts for later submissions while this one runs (nvn_gw_commit)
enum { SBW_N = 16384, SBW_PAGES = 4096 };
enum { SBW_GPU = 1, SBW_REPORT = 2 };
extern int g_sbw_over;        // a write could not be kept (too large, set full, size not known)
extern uint32_t g_sbw_gen;    // bumped by every write noted that this submission's shadows must respect (a cached shadow made before it is checked again)

// ---------- GPU writes of earlier submissions still running (gw), by 4 KB page ----------
enum { GW_N = 4096, GW_PAGES = 4096 };
typedef struct { uint64_t page, fence; } gw_page;
extern gw_page g_gw[GW_N];    // page ~0: two live pages met in the slot, so any page there counts as written until fence completes
extern uint64_t g_gw_all;     // every page counts as written until this fence completes (a submission whose writes could not all be listed)

void nvn_gw_reset(void);                                            // a submission begins: its lists are emptied (pend_reset)
int  nvn_gw_pend_add(uint64_t dst, uint64_t n, int kind);           // a write queued on [dst, dst + n): its index, -1 when the list is full (everything then counts as written)
void nvn_gw_pend_data(int e, uint64_t lo, const void *data);        // queued write e (an inline update of lo) has its bytes at data
int  nvn_gw_pend_last(uint64_t a, uint64_t n);                      // the newest queued write overlapping [a, a+n): its index, -1 for none, -2 when it cannot be told (overflow)
int  nvn_gw_pend_hits(uint64_t a, uint64_t n);
// The n bytes at a as the last write queued in this submission leaves them, when that write is an inline update covering them all → 1; otherwise → 0 and *kind = what wrote them (0 = too many writes to tell)
int  nvn_gw_pend_read(uint64_t a, uint64_t n, void *out, int *kind);
// The writes queued in this submission from index since on that overlap [a, b), newest first, into out (room for every entry a lookup can meet); -1 when they cannot all be told
int  nvn_gw_pend_since(uint64_t a, uint64_t b, int since, int *out);
void nvn_gw_sbw_note(uint64_t lo, uint64_t n, int src);             // src: SBW_GPU or SBW_REPORT
int  nvn_gw_sbw_hits(uint64_t lo, uint64_t hi);                     // a shader / copy write (SBW_GPU) of this submission covers a page of [lo, hi)
void nvn_gw_put(uint64_t lo, uint64_t hi, uint64_t fence, uint64_t done);   // [lo, hi) counts as written until fence completes; done: the fence completed now
int  nvn_gw_live(uint64_t lo, uint64_t hi, uint64_t done);          // a write of an earlier submission whose fence is past done covers a page of [lo, hi)
void nvn_gw_commit(uint64_t fence, uint64_t done);                  // the submission's GPU writes go into the page table with its fence (mtl_sub_commit)
