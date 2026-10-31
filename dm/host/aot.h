// The x86-64 execution engine's run time in the host (docs/design/aot-recompiler.md): the guest's code, recompiled ahead of time into a pack's library by
// `nxaot build` (dm/tools/aot), runs on host threads with the registers it does not keep in host registers in a state block per thread (design 2.1,
// dm/tools/aot/include/nxaot/guest_state.h). It reaches the host through the SVC path and the host-function page (2.4), leaves for the JIT fallback where it has
// no compiled code (2.5, 11), and stops at safepoint polls (6.2). Built by dm/host/CMakeLists.txt only, where DM_NATIVE_EXEC is 0 (plat/plat.h): the Apple ARM64
// builds, which run the guest's code natively, never compile these files.
//   aot_arena.c   the arena below 2^39 (5.2), guest memory the host allocates in it, module images mapped one to one (5.1, 5.3), packs loaded and looked up (9.4)
//   aot_run.c     what a pack imports (include/nxaot/aot_module.h): the SVC path, the dispatcher's miss path, traps, the JIT fallback, safepoints, the counter;
//                 dm_guest_call and the outermost level of a guest thread; process stop
//   aot_thread.c  a state block per guest thread, its native and alternate signal stacks (6.1), the live threads
//   aot_fault.c   faults (5.4): the dirty-tracking hooks first, then (with the JIT) dynarmic's fastmem handler, then the report through the line table (3.5)
//   aot_fp.c      the floating-point and AES helpers lifted code calls (dm/tools/aot/src/lift/runtime.h), in C
// The JIT fallback is dynarmic (dm/tools/aot/src/rt/jit.cpp), built in only with the CMake option NXHOST_JIT (it needs C++20 and Boost's headers)
#pragma once
#include "dm.h"
#include "nxaot/aot_module.h"
#include <setjmp.h>
#include <stdatomic.h>

#if DM_NATIVE_EXEC
#error "aot.h belongs to the x86-64 execution engine: native ARM64 builds run the guest's code directly"
#endif

// What a pack's library resolves against the host executable (CMake ENABLE_EXPORTS)
#if defined(_WIN32)
#define DM_AOT_EXPORT __declspec(dllexport)
#else
#define DM_AOT_EXPORT __attribute__((visibility("default")))
#endif

// ---------- The arena (design 5.2) ----------
// One reservation below 2^39, the Switch's 39-bit address space, taken at startup; everything the guest can see is in it (module images, the guest block of
// hle_svc.c with its heap, alias and stack regions, TLS blocks and thread-local regions, the host-function page, the stacks the host makes for guest code).
// State blocks and native stacks stay outside it
#define DM_AOT_ARENA_BASE 0x1000000000ull    // 64 GiB (2^36)
#define DM_AOT_ARENA_SIZE 0x200000000ull     // 8 GiB reserved, nothing accessible until taken
#define DM_AOT_HOST_RETURN (DM_AOT_ARENA_BASE + 0x10)   // the x30 host code enters guest code with: an address no code lives at (the arena's first page stays inaccessible)
void dm_aot_arena_init(void);              // reserve the arena and place the host-function page in it; once, later calls do nothing; dies when the range is taken
int dm_aot_in_arena(uint64_t a, uint64_t n);
void *dm_guest_reserve(size_t n);          // n bytes of fresh arena (whole pages), inaccessible: the owner opens them with plat_vm_protect (hle_svc.c's guest block)
void *dm_guest_alloc(size_t n);            // n bytes of arena, readable, writable and zero
void dm_guest_free(void *p, size_t n);     // a dm_guest_alloc range back to the arena (inaccessible until taken again)
// A module image at a fresh base in the arena, one to one (5.1): len bytes, then bss zero bytes; [0, ro_end) readable and never executable (3.2, 5.3: a host jump
// into guest text faults at once), the rest readable and writable. *mapped gets the size taken (whole pages). Dies when the arena is full
uint8_t *dm_aot_image_map(const uint8_t *bytes, size_t len, size_t bss, uint64_t ro_end, size_t *mapped);

// ---------- Packs (design 9.4) ----------
typedef struct dm_aot_pack dm_aot_pack;
// The identity a pack's descriptor must match: the image it was compiled from
typedef struct { uint8_t sha256[32]; uint64_t image_size, text_end, bss_size; } dm_aot_identity;
// Load the pack's library for module m (mapped, relocated): dlopen (LoadLibrary on Windows), the descriptor of ABI 2 checked against id, the module's base
// written to *desc->base, the line table and function map indexed, the pack registered with the dispatcher. NULL with the reason in err when it does not load
// or does not match
dm_aot_pack *dm_aot_pack_load(const char *path, module_t *m, const dm_aot_identity *id, char *err, size_t errn);
const nxaot_module_desc *dm_aot_pack_desc(const dm_aot_pack *p);
uint32_t dm_aot_module_fpcr(void);        // the FPCR the loaded packs are specialised for (7.1): the first pack's, 0 before any
const nxaot_lookup_entry *dm_aot_find(uint64_t guest, const dm_aot_pack **pack);   // the lookup slot of the function at a guest address, NULL when none
const void *dm_aot_entry_of(const nxaot_lookup_entry *e);                            // its standard entry
int dm_aot_is_return(uint64_t guest);     // the address after a call site of compiled code (desc->returns): where a native frame may wait
const dm_aot_pack *dm_aot_pack_of_code(uint64_t host_pc);                            // the pack whose library holds host_pc (async-signal-safe)
const void *dm_aot_pack_jit_entry(const dm_aot_pack *p);
uint64_t dm_aot_pack_base(const dm_aot_pack *p);     // the guest address of its module's image
// A guest address as "name+0x…" of the module (pack) that holds it; the address alone outside them (async-signal-safe: no allocation)
void dm_aot_guest_name(uint64_t guest, char *out, size_t n);
// A host pc in compiled code as the guest function and instruction the function map and line table give (3.5); 0 when host_pc is not in a pack (signal-safe)
int dm_aot_where(uint64_t host_pc, char *out, size_t n);
uint64_t dm_aot_code_lo(void), dm_aot_code_hi(void);   // the lowest and highest guest text address of the loaded packs' images (the JIT's code range)

// ---------- Guest threads (design 6.1) ----------
// A guest thread's engine state: the state block (64-byte aligned, outside the arena, at offset 0 so that a block names its thread), the base its guest code
// leaves through, its JIT instance. One per host thread that runs guest code, made at its first use
#if defined(_WIN32)
typedef void *dm_aot_jmp_buf[5];
#define dm_aot_setjmp(b) __builtin_setjmp(b)          // no SEH unwind on the way out: guest frames hold nothing to run, as siglongjmp skips them on POSIX
#define dm_aot_longjmp(b) __builtin_longjmp((b), 1)
#else
typedef sigjmp_buf dm_aot_jmp_buf;
#define dm_aot_setjmp(b) sigsetjmp((b), 1)            // the signal mask too: the fault path leaves from a signal handler
#define dm_aot_longjmp(b) siglongjmp((b), 1)
#endif
enum { DM_AOT_LEFT_NONE, DM_AOT_LEFT_EXIT, DM_AOT_LEFT_STOP, DM_AOT_LEFT_FAULT };   // why a thread's guest code left (dm_aot_thread.left)
typedef struct dm_aot_thread {
    dm_guest_state st;                 // first: (dm_aot_thread *)st is its thread
    dm_aot_jmp_buf base;               // the outermost dm_guest_call on this thread
    int depth;                         // dm_guest_call nesting: 0 while no guest code runs on the thread
    volatile int left;                 // DM_AOT_LEFT_*: set before the jump to base
    _Atomic int exiting;               // ExitThread was answered (hle_thread.c thread_exit): the thread leaves when the SVC returns
    void *jit;                         // its dm_jit instance (NXHOST_JIT), made at its first use
    uint64_t stack_lo, stack_size;     // a guest stack the host made for it (the main thread's: hle_svc.c QueryMemory reports it), 0 when none
    // A fault in guest code (aot_fault.c): the facts, reported after the jump to base
    int fault_signal;
    uint64_t fault_address, fault_pc, fault_jit_pc;
    struct dm_aot_thread *next;        // the live threads (process stop)
    void *mem;                         // the allocation that holds this struct
} dm_aot_thread;
dm_aot_thread *dm_aot_self(void);          // the calling thread's, NULL when it never ran guest code
dm_guest_state *dm_aot_thread_begin(void); // the calling thread's state block, made at its first call (x30 the host-return address, FPCR the packs')
dm_guest_state *dm_aot_self_state(void);   // NULL on a thread without one
uint64_t dm_aot_threads_made(void);        // threads that ran guest code so far
void dm_aot_thread_end(void);              // the calling thread runs no more guest code: its block goes (hle_thread.c, after the thread left)
// A guest thread's body (hle_thread.c thread_main): its entry with x0 = arg on the guest stack sp until it leaves (ExitThread, a process stop, a fault).
// Returns the DM_AOT_LEFT_* reason; an entry that returns is a red line and a process stop (Horizon requires svcExitThread)
int dm_aot_thread_run(uint64_t entry, uint64_t arg, uint64_t sp);
void dm_aot_thread_poke(dm_guest_state *st);   // a pause or stop request: the poll byte set and the thread's JIT instance halted (safe on any thread)
void dm_aot_thread_exiting(void);              // ExitThread was answered on the calling thread: it leaves when the SVC returns
void dm_aot_set_tls(uint64_t tpidr_el0, uint64_t tpidrro_el0);   // thread.c: the TLS registers' initial values, for the calling thread's block (now or when made)
// loader.c: a module image as it is (an NRO), mapped one to one in the arena, parsed, relocated and bound as load_modules does, loaded (design 9.4)
module_t *loader_add_image(const char *name, const uint8_t *bytes, size_t len, size_t bss, uint64_t text_end, uint64_t ro_off, uint64_t data_off);
void dm_aot_thread_set_jit(void *jit);         // the calling thread's JIT instance, halted at once when a pause or stop was requested before it existed
// Every live guest thread poked (process stop); returns how many other than the caller are live
int dm_aot_threads_poke_all(void);
int dm_aot_threads_live(void);                 // live guest threads other than the caller
int dm_aot_threads_wait(unsigned ms);          // wait until the caller is the last live guest thread; 0 when it is, else how many are left
int dm_aot_main_stack(uint64_t *lo, uint64_t *size);   // the guest stack the host made for the calling thread (QueryMemory); 0 when none

// ---------- The run time (aot_run.c) ----------
// Exit statuses of a run (the same numbers as dm/tools/aot's nxaot_host, rt::Status)
enum { DM_AOT_EXITED = 0, DM_AOT_TRAP = 1, DM_AOT_USAGE = 2, DM_AOT_JIT_NEEDED = 3, DM_AOT_FAULT = 4, DM_AOT_RETURNED = 6, DM_AOT_STUCK = 7 };
// Call guest code at addr from the host (design 2.4): x0.. from the arguments (at most 8), x30 = the host-return address, the module's MXCSR, then the native
// entry, the host-function page or the JIT. Returns x0 when the guest returns; the callee-saved registers, x30, SP and pc of the guest code around the call
// are kept. The outermost call on a thread is the base the thread leaves through: it returns 0 then, with dm_aot_self()->left saying why
uint64_t dm_guest_call(dm_guest_state *st, uint64_t addr, int nargs, ...);
// The process's main guest thread: entry with x0, x1, on a guest stack the host makes (8 MiB in the arena), until the process stops (svcExitProcess, a trap,
// the JIT fallback needed and not built in, a fault, guest code returning to the host); then every other guest thread is stopped (6.2's polls, SVCs, waits)
// and waited for, at most 10 s. Returns the status; *seconds the guest time
int dm_aot_run(uint64_t entry, uint64_t x0, uint64_t x1, double *seconds);
// Stop the process's guest code: the first status wins; every guest thread leaves at its next poll, SVC, JIT block boundary or kernel wait. why: a [RED]
// line, logged once (NULL for none)
void dm_aot_stop(int status, const char *why);
int dm_aot_stop_status(void);              // -1 while running
_Noreturn void dm_aot_leave(int why);      // the calling guest thread leaves its guest code (to its outermost dm_guest_call)
// Counters (the engine's own; the corpus test reads them)
typedef struct {
    uint64_t svcs, host_calls, misses, cross_pack, safepoints, jit_runs, jit_addresses, threads;
} dm_aot_stats_t;
void dm_aot_stats(dm_aot_stats_t *out);
uint32_t dm_aot_guest_mxcsr(uint32_t fpcr);   // the MXCSR guest code runs with (7.1): exceptions masked, RMode as rounding control, FZ as FTZ and DAZ

// What a pack's library imports (include/nxaot/aot_module.h), defined by aot_run.c and aot_fp.c and exported from the executable
DM_AOT_EXPORT void dm_aot_svc(dm_guest_state *st, uint32_t number);
DM_AOT_EXPORT void *nxaot_rt_dispatch(dm_guest_state *st, uint64_t target, const nxaot_ic_desc **slot, uint64_t site);
DM_AOT_EXPORT void nxaot_rt_trap(dm_guest_state *st, uint64_t offset, uint32_t reason);
DM_AOT_EXPORT void nxaot_rt_jit(dm_guest_state *st, uint64_t lr, uint64_t sp);
DM_AOT_EXPORT void nxaot_rt_safepoint(dm_guest_state *st);
DM_AOT_EXPORT uint64_t dm_aot_cntpct(dm_guest_state *st);

// ---------- Faults (aot_fault.c) ----------
void crash_install(void);                  // the fault handlers (signals on POSIX, a vectored exception handler on Windows); once
void crash_thread_init(void);              // the calling thread's alternate signal stack (POSIX) or stack guarantee (Windows)
void dm_aot_fault_after_jit(void);         // after the first JIT instance: the host's handler again in front of dynarmic's (5.4's order)

// ---------- The thread layer's side (hle_thread.c, x86-64 only) ----------
int thr_safepoint(void);                   // a poll found the poll byte set: wait here while SetThreadActivity pauses this thread; 1 when it waited
void thr_stop_all(void);                   // process stop: every kernel wait woken (cancelled), every pause released
int thr_handles(uint32_t *out, int max, uint32_t except);   // the handles of the live (started, not exited) guest threads, but except's
