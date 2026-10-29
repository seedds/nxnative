// What a pack's library holds for one module, and what it asks of the host (docs/design/aot-recompiler.md sections 3.5, 4, 9.4): `nxaot build`
// (src/build) writes it, the host run time (src/rt today, dm/host/aot_*.c later) reads it. Plain C, like the state block it is shared with.
//
// The library exports one descriptor per module, `nxaot_module` (struct nxaot_module_desc). Its tables are read-only and image-relative: guest
// addresses are module offsets, native addresses are 32-bit offsets from the field that holds them, so loading applies no relocation to them:
//   lookup   the dispatcher's open-addressing hash, module offset -> the standard entry of a function (section 4: every address-taken function)
//   fns      every compiled function, by module offset: its body and size, for crash reports and profilers
//   lines    the line table (section 3.5): the native address of every guest memory access with the module offset of its guest instruction
//            (what a fault report needs, 5.4), written by LLVM's !pcsections into the section `nxaot_lines`, {int32 native - &entry, uint32
//            offset} per entry. LLVM 18 keeps !pcsections on memory instructions only (calls lose it), so call sites and every other native
//            address map through the DWARF line table of the library (line = guest offset, section 9.2) and the function map
// The host writes the module's base address to *base before guest code runs (section 3.3: return addresses are base + offset) and calls guest
// functions through `enter`, a C-convention trampoline that passes st's registers to a standard entry.
//
// The library imports these from the host (the C convention; the library's preserve_all / preserve_most wrappers keep the call sites free of
// spills, section 2.3):
//   void  dm_aot_svc(dm_guest_state *st, uint32_t number)                      svc #number: st holds the whole register file, st->pc the next pc
//   void *nxaot_rt_dispatch(dm_guest_state *st, uint64_t target, const nxaot_ic_desc **slot, uint64_t site)
//                                                                              an inline-cache miss at module offset site: the native standard
//                                                                              entry of guest address target, with *slot updated, or no return
//   void  nxaot_rt_trap(dm_guest_state *st, uint64_t offset, uint32_t reason) a trap or a case the JIT fallback takes (reason: NXAOT_TRAP_*);
//                                                                              does not return
//   uint64_t dm_aot_cntpct(dm_guest_state *st)                                 mrs cntpct_el0 / cntvct_el0: the guest counter, 19.2 MHz ticks
//   the floating-point helpers of src/lift/runtime.h (nxaot_rt_fp_*, nxaot_rt_aes)
#ifndef NXAOT_AOT_MODULE_H
#define NXAOT_AOT_MODULE_H

#include <stdint.h>

#include "nxaot/guest_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXAOT_MODULE_ABI 1u
#define NXAOT_LOOKUP_EMPTY UINT64_MAX  // the module offset of an empty lookup slot

// Why compiled code stopped (nxaot_rt_trap): the exits of guest_state.h, and the cases the JIT fallback of section 11 takes (phase 3)
enum nxaot_trap {
    NXAOT_TRAP_BRK = DM_EXIT_BRK,                    // brk at offset
    NXAOT_TRAP_UNDEFINED = DM_EXIT_UNDEFINED,        // an unallocated or reserved encoding (udf among them)
    NXAOT_TRAP_UNPREDICTABLE = DM_EXIT_UNPREDICTABLE,
    NXAOT_TRAP_NO_CODE = DM_EXIT_NO_CODE,            // execution ran out of the function into what is no function's entry
    NXAOT_TRAP_UNTRANSLATED = DM_EXIT_UNTRANSLATED,  // an encoding dynarmic's frontend does not translate (LSE atomics, hvc, ...)
    NXAOT_TRAP_JIT_TARGET = 64,                      // a direct branch to an address that is no compiled function's entry: the JIT
    NXAOT_TRAP_JIT_FPCR = 65,                        // msr fpcr left the FPCR the module was compiled for (section 7.1): the JIT
    NXAOT_TRAP_NORETURN = 66,                        // a call returned that the analysis said does not return
    NXAOT_TRAP_JIT_REFUSED = 67,                     // a function the lifter refused (section 11, case 4): the JIT
};

// Pack modes, conventions and options the code was built with (nxaot_module_desc.flags)
#define NXAOT_FLAG_GHC 1u            // the ghccc convention (else swifttailcc)
#define NXAOT_FLAG_CORRECTNESS 2u    // every register stored and reloaded at every call (the reference mode of the differential tests)
#define NXAOT_FLAG_ALL_ENTRIES 4u    // every function has a standard entry and a lookup slot (nxaot_funcdiff)
#define NXAOT_FLAG_FPSR 8u           // the cumulative FPSR flags are modelled (section 7.4)

// An inline cache's descriptor (section 4): immutable once published; a cache slot points at one, and a miss replaces the pointer atomically
typedef struct nxaot_ic_desc {
    uint64_t guest;   // the guest address it was made for (UINT64_MAX: none)
    void *native;     // the standard entry to call
} nxaot_ic_desc;

typedef struct nxaot_lookup_entry {
    uint64_t offset;  // module offset of the function, NXAOT_LOOKUP_EMPTY for an empty slot
    int32_t entry;    // its standard entry, relative to this field
    uint32_t pad;
} nxaot_lookup_entry;

typedef struct nxaot_fn_entry {
    uint64_t offset;  // module offset of the guest function
    int32_t body;     // its compiled body, relative to this field
    uint32_t size;    // bytes of guest code
} nxaot_fn_entry;

typedef struct nxaot_line_entry {
    int32_t native;   // the native address, relative to this field
    uint32_t offset;  // the module offset of the guest instruction
} nxaot_line_entry;

typedef struct nxaot_module_desc {
    uint32_t abi;               // NXAOT_MODULE_ABI
    uint32_t flags;             // NXAOT_FLAG_*
    char name[32];              // the module's name ("main", "nro-<hash>", the NRO's file name), NUL-terminated
    uint8_t sha256[32];         // identity: the SHA-256 of the image the code was compiled from (section 1.4)
    uint64_t image_size;        // bytes of the image (bss excluded) and of its text, as the code assumes them
    uint64_t text_end;
    uint64_t bss_size;
    uint64_t entry;             // module offset of the module's entry (_start)
    uint32_t fpcr;              // the FPCR the code is specialised for (section 7.1)
    uint32_t lookup_mask;       // lookup slots - 1 (a power of two minus one)
    uint32_t rv;                // which of v0..v3 the standard entries return in registers (bits 0..3); `enter` stores them to st->v
    uint32_t pad;
    uint64_t *base;             // the host writes the module's base address here
    const nxaot_lookup_entry *lookup;
    const nxaot_fn_entry *fns;
    uint64_t fn_count;
    const nxaot_line_entry *lines, *lines_end;  // the line table (the section nxaot_lines); either may be null when the module has none
    void (*enter)(dm_guest_state *st, const void *standard_entry);  // calls a standard entry with st's registers; stores x0, x1 and rv back
    const char *tool;           // the toolchain and options it was built with, for the logs
} nxaot_module_desc;

// The lookup hash of a module offset: lookup_mask + 1 slots, linear probing (src/build/pack.cpp builds it, the dispatcher probes it)
static inline uint32_t nxaot_lookup_hash(uint64_t offset, uint32_t mask) {
    return (uint32_t)(((offset >> 2) * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

#ifdef __cplusplus
}
#endif

#endif
