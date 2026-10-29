// The guest state block: one per guest thread, the home of every guest register that recompiled code does not hold in host registers
// (docs/design/aot-recompiler.md section 2.1). Shared by the lifted code (src/lift/lifter.cpp addresses its fields by these offsets), the vector harness,
// and later the host runtime and the JIT glue, so it is plain C and every offset is asserted below. A change of layout is a change of the lifter's
// semantics: it will be versioned with the recompiler's chain (AOT_CHAIN, section 13).
#ifndef NXAOT_GUEST_STATE_H
#define NXAOT_GUEST_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define DM_ALIGNAS(n) alignas(n)
#define DM_ALIGNOF(t) alignof(t)
#define DM_STATIC_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
#define DM_ALIGNAS(n) _Alignas(n)
#define DM_ALIGNOF(t) _Alignof(t)
#define DM_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

// Why lifted code returned to its caller (dm_guest_state.exit_reason). pc says where the guest continues or what faulted
enum dm_exit_reason {
    DM_EXIT_NONE = 0,           // the block ended normally: the guest continues at pc
    DM_EXIT_SVC = 1,            // svc #exit_arg; pc is the instruction after the svc
    DM_EXIT_BRK = 2,            // brk; pc is the brk
    DM_EXIT_UNDEFINED = 3,      // an unallocated or reserved encoding (udf among them); pc is the instruction
    DM_EXIT_UNPREDICTABLE = 4,  // an encoding the Arm ARM calls UNPREDICTABLE; pc is the instruction
    DM_EXIT_NO_CODE = 5,        // no instruction could be read at pc
    DM_EXIT_UNTRANSLATED = 6,   // an encoding the frontend does not translate (dynarmic's Interpret terminal: LSE atomics, hvc, ...); pc is it
};

typedef struct dm_guest_state {
    DM_ALIGNAS(64) uint64_t x[31];   // x0..x30, in the order of the gate frame that svc_dispatch reads (dm/host/hle_svc.c, svc_dispatch(num, regs, ...))
    uint64_t sp, pc;                 // pc is valid at escapes: SVCs, host calls, safepoints, fault reports
    uint32_t nzcv, fpcr, fpsr, pad;  // nzcv as mrs nzcv reads it (N, Z, C, V in bits 31..28); materialised at escapes only
    DM_ALIGNAS(16) uint8_t v[32][16];  // q0..q31
    uint64_t tpidr_el0, tpidrro_el0;   // replace the K18 / KTLS / KTLR thread slots of the Apple rewriter
    uint64_t excl_addr, excl_val[2];   // the exclusive monitor (section 6.5): address and value of the last exclusive load (excl_val[1]: the high half of a pair)
    uint32_t excl_size, excl_valid;    // its size in bytes; excl_valid = 0 when no exclusive load is outstanding
    uint64_t pending;                  // target of a non-local transfer (section 3.3), 0 = none
    uint8_t poll;                      // safepoint request (section 6.2)
    uint8_t pad2[7];
    // Host-only fields: guest code never sees them
    uint64_t cnt;                      // the value mrs cntpct_el0 / cntvct_el0 read (19.2 MHz guest ticks); the host keeps it current
    uint32_t exit_reason, exit_arg;    // enum dm_exit_reason, and its argument (the svc immediate)
} dm_guest_state;

DM_STATIC_ASSERT(offsetof(dm_guest_state, x) == 0, "dm_guest_state.x");
DM_STATIC_ASSERT(offsetof(dm_guest_state, sp) == 248, "dm_guest_state.sp");
DM_STATIC_ASSERT(offsetof(dm_guest_state, pc) == 256, "dm_guest_state.pc");
DM_STATIC_ASSERT(offsetof(dm_guest_state, nzcv) == 264, "dm_guest_state.nzcv");
DM_STATIC_ASSERT(offsetof(dm_guest_state, fpcr) == 268, "dm_guest_state.fpcr");
DM_STATIC_ASSERT(offsetof(dm_guest_state, fpsr) == 272, "dm_guest_state.fpsr");
DM_STATIC_ASSERT(offsetof(dm_guest_state, pad) == 276, "dm_guest_state.pad");
DM_STATIC_ASSERT(offsetof(dm_guest_state, v) == 288, "dm_guest_state.v");
DM_STATIC_ASSERT(offsetof(dm_guest_state, tpidr_el0) == 800, "dm_guest_state.tpidr_el0");
DM_STATIC_ASSERT(offsetof(dm_guest_state, tpidrro_el0) == 808, "dm_guest_state.tpidrro_el0");
DM_STATIC_ASSERT(offsetof(dm_guest_state, excl_addr) == 816, "dm_guest_state.excl_addr");
DM_STATIC_ASSERT(offsetof(dm_guest_state, excl_val) == 824, "dm_guest_state.excl_val");
DM_STATIC_ASSERT(offsetof(dm_guest_state, excl_size) == 840, "dm_guest_state.excl_size");
DM_STATIC_ASSERT(offsetof(dm_guest_state, excl_valid) == 844, "dm_guest_state.excl_valid");
DM_STATIC_ASSERT(offsetof(dm_guest_state, pending) == 848, "dm_guest_state.pending");
DM_STATIC_ASSERT(offsetof(dm_guest_state, poll) == 856, "dm_guest_state.poll");
DM_STATIC_ASSERT(offsetof(dm_guest_state, cnt) == 864, "dm_guest_state.cnt");
DM_STATIC_ASSERT(offsetof(dm_guest_state, exit_reason) == 872, "dm_guest_state.exit_reason");
DM_STATIC_ASSERT(offsetof(dm_guest_state, exit_arg) == 876, "dm_guest_state.exit_arg");
DM_STATIC_ASSERT(sizeof(dm_guest_state) == 896, "dm_guest_state size");
DM_STATIC_ASSERT(DM_ALIGNOF(dm_guest_state) == 64, "dm_guest_state alignment");

#ifdef __cplusplus
}
#endif

#endif
