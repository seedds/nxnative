// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // Pool destruction: restore protected pages to writable and mark invalid
void nvn_dirty_open(const void *addr, size_t len);                 // Make the covered protected pages writable, not counted as written, and hold off protection until nvn_dirty_close: around making a zero-copy buffer the GPU writes through
void nvn_dirty_close(void);                                        // Protect them again; those whose bytes changed meanwhile count as written
void nvn_dirty_open_stats(uint64_t *opened, uint64_t *changed);    // Cumulative: pages nvn_dirty_open opened, and those of them that changed while open
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // Set the covered pages read-only
void nvn_dirty_protect_sparse(const void *addr, size_t len);       // The same, for the pages behind a placement sparse tile: a later write to one moves nvn_dirty_sparse_writes
uint64_t nvn_dirty_sparse_writes(void);                            // Writes so far to pages protected that way
void nvn_dirty_own_thread(void);                                   // The calling thread is the one that replays submissions
uint64_t nvn_dirty_own_writes(void);                               // Writes to protected pages by that thread so far (faults and host writes): the ones in order with what it replays
void nvn_dirty_touch(void);                                        // Moves the generation as a write does, with no page written: whoever remembers "nothing written since" by it asks its pages again
uint64_t nvn_dirty_now(void);                                      // Current generation: take the content's stamp before nvn_dirty_protect and before reading it
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // Covered pages were written after the stamp -> 1
const void *nvn_dirty_span(const void *addr, size_t len);          // The tracked range holding the span (a handle to keep; NULL = untracked)
int nvn_dirty_since_cached(const void **span, const void *addr, size_t len, uint64_t stamp);   // nvn_dirty_since keeping the last range found in *span
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp);   // nvn_dirty_since through a kept handle (rechecked, scanned again only if it no longer holds the span)
// Pages of [addr, addr+len) written after the stamp that hold no byte of the ranges ex[0..nex) (a host write the caller accounts for): how many, the first one's address in *first
// (when not NULL); SIZE_MAX when the span is untracked
size_t nvn_dirty_since_except(const void *span, const void *addr, size_t len, uint64_t stamp, const uintptr_t (*ex)[2], int nex, uintptr_t *first);
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_describe(const void *addr, const void *span, char *out, size_t n);   // Diagnostic: every live range over the host page at addr (state, generation, registration; span = the caller's kept range) and the kernel's protection of it
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // Currently live slots, slot table high-water mark, total number of registrations
