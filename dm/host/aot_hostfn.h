// The host-function page of the x86-64 execution engine (docs/design/aot-recompiler.md 2.4, 5.2): every host function the guest can call (the HLE, HLE_FORCE and
// HLE_FORCE_BIG entries of hle.h) gets a guest-visible address, a 16-byte slot in a page inside the guest arena. A guest call to a slot reaches the dispatcher, which
// runs the slot's thunk (generated at build time by dm/host/tools/gen_hle_thunks.py into gen/hle_thunks.c): it reads the arguments from the state block, calls the
// C function on the host thread's own stack and writes the result back. Slot i belongs to hle_thunks[i], so an address names its entry by (addr - base) / 16.
// The slots hold brk words, so native execution of the page faults. Plain C on every host; Apple's native builds do not use it (a GOT slot holds the C function there)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nxaot/guest_state.h"   // dm/tools/aot/include: the state block, shared with the lifter

typedef struct {
    const char *mangled;                 // the Switch symbol the entry implements (sorted by strcmp in hle_thunks[])
    const char *cname;                   // the C function's name, for logs and crash reports
    void (*thunk)(dm_guest_state *st);   // arguments from st, the call, the result into st->x[0] or st->v[0]
    const void *fn;                      // the C function
} hle_thunk_ent;
extern const hle_thunk_ent hle_thunks[];   // gen/hle_thunks.c (a test links its own table)
extern const size_t hle_thunks_count;

#define DM_AOT_HOSTFN_SLOT 16u               // bytes per slot
#define DM_AOT_HOSTFN_POISON 0xD43E1E00u     // brk #0xF0F0, the word that fills every slot

// Place the page at guest_base, bytes long (the arena owner maps it readable and writable; the page is then filled with poison and made read-only). Called once, before any
// other function here; without the call the first use places the page in a host allocation of its own (tests, and until the arena exists). Dies when bytes cannot hold
// every slot or when the page is already placed
void dm_aot_hostfn_init(uint64_t guest_base, size_t bytes);
uint64_t dm_aot_hostfn_base(void);    // the page's guest address
size_t dm_aot_hostfn_size(void);      // bytes in use: hle_thunks_count slots
// The guest-visible address of a host function, for host code that hands one to the guest: a GOT slot (loader.c bind_sym / force_lookup on x86 return dm_guest_fn(fn)),
// the FMalloc vtable of hle_uemalloc.c, get-proc-address results. Dies with the function's address when it is no entry, so a missed site is loud
uint64_t dm_guest_fn(const void *host_fn);
const hle_thunk_ent *dm_aot_hostfn_lookup(uint64_t guest_addr);   // the entry whose slot starts at guest_addr; NULL for any other address
const char *dm_aot_hostfn_name(uint64_t guest_addr);              // the C name of the slot holding guest_addr (any byte of it), for crash reports; NULL outside the page
// A guest call reached slot guest_addr: run its thunk on st. The caller continues the guest at st->x[30]. Dies with the address and st->pc when it is no slot
void dm_aot_hostcall(dm_guest_state *st, uint64_t guest_addr);
