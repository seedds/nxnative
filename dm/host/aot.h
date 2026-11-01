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
// hle_svc.c with its heap, alias and stack regions, TLS blocks and thread-local regions, the host-function page, the stacks the host makes for guest code, NVN's
// virtual pool ranges, the guest Vulkan layer's mappings, Unreal's heap on the host allocator, and every host allocation handed to the guest: the garena below).
// State blocks and native stacks stay outside it. 256 GiB of address space (nothing is charged until taken): a game's guest block (5 GB for Unreal games, 64 GB with a
// Switch-sized DM_ALIAS_SIZE), an RE Engine virtual pool of 64 GB (hle_nvn.c vpool_reserve), the module images and the host's guest heaps fit with room, and its end
// (320 GiB) stays below 2^39 (512 GiB)
#define DM_AOT_ARENA_BASE 0x1000000000ull    // 64 GiB (2^36)
#define DM_AOT_ARENA_SIZE 0x4000000000ull    // 256 GiB reserved, nothing accessible until taken
#define DM_AOT_HOST_RETURN (DM_AOT_ARENA_BASE + 0x10)   // the x30 host code enters guest code with: an address no code lives at (the arena's first page stays inaccessible)
void dm_aot_arena_init(void);              // reserve the arena and place the host-function page in it; once, later calls do nothing; dies when the range is taken
int dm_aot_in_arena(uint64_t a, uint64_t n);
void *dm_guest_reserve(size_t n);          // n bytes of arena (whole pages, a range given back first), inaccessible: the owner opens them with plat_vm_protect (hle_svc.c's guest block)
void *dm_guest_alloc(size_t n);            // n bytes of arena, readable, writable and zero
void dm_guest_free(void *p, size_t n);     // a dm_guest_alloc or dm_guest_reserve range back to the arena (inaccessible until taken again)

// ---------- Host memory the guest sees: the one rule (design 5.2) ----------
// Every byte of host memory the guest's code may read, write or keep the address of lies in the arena: on Apple the guest sees all host memory and these are the plain
// malloc / stack array / string literal, here they are the arena's. A host file keeps Apple's text and picks the form with its own macros, as GUEST_CALL picks the call:
// the plain C form where DM_NATIVE_EXEC (Apple) or DM_TEST_GUEST_C (a unit test that supplies the guest's functions as C functions), the dm_garena form here
// (hle_audren.c, hle_codec.c, hle_movie.c, hle_swkbd.c, ...: GUEST_MALLOC, GUEST_FREE, GUEST_STR, GUEST_LOCAL, ...). The family:
//   dm_garena_alloc(n, align)     n bytes, zero, aligned to align (a power of two; 0 means 16): a heap of size classes up to 16 KiB in 64 KiB slabs (each 64 KiB-aligned,
//                                 one class per slab, kept readable and writable), whole pages above (a freed block up to 1 MiB kept for its size, at most 64 MiB of them;
//                                 larger ones decommitted at once). Thread-safe; dies with a [RED] line when the arena is full, as the arena does
//   dm_garena_malloc(n, align)    the same without the zeroing (a host allocator handed to the guest: hle_uemalloc.c's Unreal heap)
//   dm_garena_realloc(p, n, align), dm_garena_free(p), dm_garena_size(p)   realloc, free (NULL: nothing) and the usable size (0: p is no garena block) of a garena block
//   dm_garena_strdup(s)           a copy of the host string s, freed with dm_garena_free
//   dm_garena_literal(s)          a copy of a string with static storage (a literal, a C library's zone name) made once and kept for good, found again by s's address:
//                                 for a host string the guest only reads (a key handed to an SDK function, a name an entry returns)
//   dm_garena_static(slot, init, n)   n bytes made once (a copy of init when given, else zero) and kept in *slot: a static the guest keeps the address of (an event
//                                 object the host owns, a struct handed back on every call); thread-safe
//   dm_gscope_mark / dm_gscope_alloc / dm_gscope_release   the scoped form, for the arrays Apple's text puts on the host stack and passes to guest code: a bump region of
//                                 DM_GSCOPE_SIZE per host thread in the arena, made at the thread's first use and given back at its exit (a pthread key's destructor);
//                                 an allocation is 16-byte aligned and zero, valid until the mark taken before it is released. One that does not fit is taken from the
//                                 garena instead, with a [RED] line once per thread, and released with the mark as well. DM_GLOCAL(type, name, n) declares type *name over
//                                 n zeroed elements and releases them when the enclosing block ends (GNU C's cleanup attribute, as hle_fs.c's PATH_LOCK), the scope of
//                                 Apple's stack array; a non-local exit (longjmp) past it leaves the region's top where it was until an outer mark is released
//   dm_garena_set_commit_hooks(map, unmap)   the graphics backend's view of the pages these functions commit and decommit (be_guest_map / be_guest_unmap, set by the
//                                 host program, main_x86.c): a pool the game places in such memory (Unreal's heap) is imported by Vulkan like any committed guest memory
// The guard: DM_AOT_ARENA_CHECK=1 (docs/env_switches.tsv) checks, at every host-call thunk (gen_hle_thunks.py: the pointer parameters and the pointer result of each entry,
// opaque handle types left out) and at every pointer argument of DM_GUEST_CALL (below), that a non-null address above the first 64 KiB is in the arena, and logs a [RED]
// line once per site for one that is not (dm_aot_arena_flag). dm_guest_call's own arguments are integers to it (their types are gone): its sites are listed in aot_arena.c
#define DM_GSCOPE_SIZE 0x10000u              // the scoped region of a host thread: 64 KiB of arena
void *dm_garena_alloc(size_t n, size_t align);
void *dm_garena_malloc(size_t n, size_t align);
void *dm_garena_realloc(void *p, size_t n, size_t align);
void dm_garena_free(void *p);
size_t dm_garena_size(const void *p);
char *dm_garena_strdup(const char *s);
const char *dm_garena_literal(const char *s);
void *dm_garena_static(void *_Atomic *slot, const void *init, size_t n);
void dm_garena_set_commit_hooks(void (*map)(void *mem, uint64_t size), void (*unmap)(void *mem, uint64_t size));
typedef uint64_t dm_gscope_t;
dm_gscope_t dm_gscope_mark(void);
void *dm_gscope_alloc(size_t n);
void dm_gscope_release(dm_gscope_t mark);
static inline void dm_gscope_cleanup(dm_gscope_t *mark) { dm_gscope_release(*mark); }
#define DM_GLOCAL(type, name, n)                                                                                                   \
    dm_gscope_t DM_GC_CAT(name, _gscope_) __attribute__((cleanup(dm_gscope_cleanup), unused)) = dm_gscope_mark();                  \
    type *name = (type *)dm_gscope_alloc(sizeof(type) * (size_t)(n))
// A static the guest keeps the address of, of type type, made once in the arena (zero, or a copy of *init): DM_GSTATIC(type, slot, init) is a type * (slot: a
// static void *_Atomic of the caller's)
#define DM_GSTATIC(type, slot, init) ((type *)dm_garena_static(&(slot), (init), sizeof(type)))
extern _Atomic int dm_aot_arena_check;     // DM_AOT_ARENA_CHECK: -1 until read (dm_aot_arena_init), then 0 or 1
// An address outside the arena where a guest-visible one belongs: a [RED] line naming site (a file or entry name), line and pos (the argument, 0 for a result), once per
// site and position
void dm_aot_arena_flag(const char *site, unsigned line, int pos, uint64_t v, const char *what);
// A module image at a fresh base in the arena, one to one (5.1): len bytes, then bss zero bytes; [0, ro_end) readable and never executable (3.2, 5.3: a host jump
// into guest text faults at once), the rest readable and writable. *mapped gets the size taken (whole pages). Dies when the arena is full
uint8_t *dm_aot_image_map(const uint8_t *bytes, size_t len, size_t bss, uint64_t ro_end, size_t *mapped);

// ---------- Packs (design 9.4) ----------
typedef struct dm_aot_pack dm_aot_pack;
// The identity a pack's descriptor must match: the image it was compiled from
typedef struct { uint8_t sha256[32]; uint64_t image_size, text_end, bss_size; } dm_aot_identity;
// Load the pack's library for module m (mapped, relocated): dlopen (LoadLibrary on Windows), the descriptor of NXAOT_MODULE_ABI (3) checked against id, the module's base
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
// A game module's pack (loader.c, "The game-module path"): <dir>/<name>.aot.so (.aot.dll on Windows), beside the manifest <name>.aot.tsv and the behaviour image <name>.image.bin
// it was built from (nxaot build <dir>/<name>.aot.tsv -o <object> --shared <dir>/<name>.aot.so). The game's input folder is <NSP folder>/<id>_build/aot_<version>/, beside the
// rewritten modules of Apple's patched_<version>/
#if defined(_WIN32)
#define DM_AOT_PACK_SUFFIX ".aot.dll"
#else
#define DM_AOT_PACK_SUFFIX ".aot.so"
#endif
// Load module m's pack from dir with the identity of its image (the SHA-256 the loader computed over image.bin, its size, text and bss): 0, or 1 with the reason in err
int dm_aot_load_module_pack(module_t *m, const char *dir, const uint8_t sha256[32], uint64_t image_size, uint64_t text_end, uint64_t bss, char *err, size_t errn);
// The gates a module's manifest asks for (its obs and spin rows, design 1.1) against the gate sites its loaded pack's code calls (the descriptor's gates, ABI 3):
// kind NXAOT_GATE_OBS (offset: the observed instruction; arg: the number | parameter << 8) or NXAOT_GATE_SPIN (offset: a back-edge; arg: its head). A site of
// the pack's that no row names, or names otherwise, is a pack of another manifest: 1 with the reason in err. A row the pack does not serve (its code holds no
// discovered function, nxaot build said so) is a [RED] line each, and *unserved their count: its observer or back-off does not run. 0 otherwise
#ifndef DM_AOT_GATE_ROW
#define DM_AOT_GATE_ROW   // loader.c declares the same (it is compiled into unit tests without the engine's headers)
typedef struct dm_aot_gate_row { uint64_t offset, arg; uint32_t kind; } dm_aot_gate_row;
#endif
int dm_aot_pack_check_gates(const module_t *m, const dm_aot_gate_row *rows, int n, int *unserved, char *err, size_t errn);

// ---------- Imports of the modules the engine maps (design 2.4) ----------
// loader.c bind_sym: what an import of module m (named name) binds to, from the address v Apple would bind: unchanged for a module outside the arena (a test's hand-made image),
// 0 (an undefined weak import), a loaded module's definition and guest memory; a host function's slot in the host-function page (aot_hostfn.h dm_guest_fn); an
// unimplemented import's named stub (hle.c resolve_import returns &unimpl_stubs[k]) its slot in the stub page. A host function with no host-call thunk stops the load with a red line
uint64_t dm_aot_bind(const module_t *m, const char *name, uint64_t v);
// The named stubs of unimplemented imports: Apple's entry.S assembles 4096 (hle.c MAX_STUB); here the array is only their identities, never executed, and each has a slot of
// DM_AOT_HOSTFN_SLOT bytes in the stub page, in the arena after the host-function page (readable, brk words). A guest call to slot k runs hle.c unimpl_c(k, x): its UNIMPL line
// once and 0 in x0, as Apple's stub does
#define DM_AOT_STUBS 4096
extern uint64_t unimpl_stubs[DM_AOT_STUBS];
uint64_t dm_aot_stub_addr(unsigned k);     // the guest address of stub k's slot
long dm_aot_stub_index(uint64_t guest);    // the stub whose slot starts at guest, -1 for any other address
uint64_t dm_aot_hostfn_find(const void *host_fn);   // aot_hostfn.c: host_fn's slot in the host-function page, 0 when it is no entry's C function (dm_guest_fn dies there)

// ---------- Host functions handed to the guest (design 2.4) ----------
// A host function's address means nothing to recompiled code: whatever the host hands the guest as a function (a vtable slot it writes into guest memory, a thread
// entry or callback it passes to a guest function, a get-proc-address result) must be the function's guest-visible address. The one rule, for every such place:
//   dm_guest_fnptr(fn)               the slot of fn's host-call thunk in the host-function page. NULL stays NULL, and an address the guest can already reach (guest
//                                    memory, a loaded module, a slot) is returned as it is, so a table that mixes the guest's functions and the host's goes through it
//                                    whole. A host function with no thunk gets a [RED] line (once per function) and a stub slot named after it, which answers a call
//                                    as an unimplemented import's does (hle.c unimpl_c: an UNIMPL line, 0). On Apple the address is the guest's: no form is needed there
//                                    (the sites keep Apple's text under DM_NATIVE_EXEC)
//   dm_guest_fnptr_named(fn, name)   the same for a lookup by name (vkGetInstanceProcAddr, eglGetProcAddress, nvnDeviceGetProcAddress: the thunk of every entry that
//                                    returns a function pointer calls it, gen_hle_thunks.py), quietly: a result with no thunk (an NVN function the host does not
//                                    implement, DM_GL_STUBS's stubs) becomes the stub named name, whose first call says UNIMPL <name>, as Apple's stubs report theirs
// Which host functions have thunks: the entries of the import tables (HLE, HLE_FORCE, NVN, VK, GL, EGL, PASS) and every function defined with HLE_LINKAGE outside a
// macro (hle.h: static on Apple, external here), the form for a host function the guest reaches only through a pointer (hle_uemalloc.c's FMalloc slots, hle_misc.c's
// ProfanityFilter vtable, hle_guestthread.c's thread entry). Safe on any thread
void *dm_guest_fnptr(const void *fn);
void *dm_guest_fnptr_named(const void *fn, const char *name);

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
    dm_aot_jmp_buf base;               // the innermost dm_guest_call on this thread: each level keeps the one around it and puts it back when it returns
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
    int host;                          // a host thread that is no guest thread, given a guest context by dm_gcall (counted live only while it runs guest code)
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
// loader.c: the NRO the game hands ro::LoadModule (hle_ro.c), n bytes with that SHA-256, at its first load: its row in load_modules' nro.tsv names it, its image is mapped
// one to one in the arena and its pack <folder>/<name>.aot.so loaded with the image's identity; an NRO module then (m->nro), not yet loaded (nro_load relocates it). NULL
// with the reason in err when it has no row, is no NRO, or its pack is missing or of another image. The caller serializes loads (hle_ro.c's lock)
module_t *loader_nro_map(const uint8_t *image, uint32_t n, const uint8_t sha256[32], char *err, size_t errn);
void dm_aot_thread_set_jit(void *jit);         // the calling thread's JIT instance, halted at once when a pause or stop was requested before it existed
// Every live guest thread poked (process stop); returns how many other than the caller are live
int dm_aot_threads_poke_all(void);
int dm_aot_threads_live(void);                 // live guest threads other than the caller
int dm_aot_threads_wait(unsigned ms);          // wait until the caller is the last live guest thread; 0 when it is, else how many are left
int dm_aot_main_stack(uint64_t *lo, uint64_t *size);   // the guest stack the host made for the calling thread (QueryMemory); 0 when none

// ---------- The run time (aot_run.c) ----------
// Exit statuses of a run (the same numbers as dm/tools/aot's nxaot_host, rt::Status)
enum { DM_AOT_EXITED = 0, DM_AOT_TRAP = 1, DM_AOT_USAGE = 2, DM_AOT_JIT_NEEDED = 3, DM_AOT_FAULT = 4, DM_AOT_RETURNED = 6, DM_AOT_STUCK = 7 };
// Call guest code at addr from the host (design 2.4): x0..x7 from the arguments, the 9th to the 16th in the guest stack's 8-byte slots below SP (AAPCS64's stacked
// arguments, each one 64 bits wide), x30 = the host-return address, the module's MXCSR, then the native entry, the host-function page or the JIT. The registers
// it does not set (x8, v0..v7) go in as the block holds them, so a caller with an indirect result or floating-point arguments sets them first; x1 and v0 hold the
// rest of a result after it. Returns x0 when the guest returns; the callee-saved registers, x30, SP and pc of the guest code around the call are kept.
// Nested re-entry: a host function that guest code called (its host-call thunk on this thread's native stack) may call guest code again on the same block; the
// guest goes on below the SP of the host call, the thunk gets its block back as it left it but for the argument registers, and every level is a base of its own.
// When guest code leaves (ExitThread, a process stop, a trap, a fault), the innermost call returns 0 with dm_aot_self()->left saying why, so the host function
// around it runs to its end (its locks released), and the host call that made it leaves in turn when it returns (aot_run.c host_call), level by level to the outermost
// call; a further guest call before that returns 0 at once. A fault is reported at the level that catches it. Limits: a non-local transfer (longjmp, a C++ exception)
// out of nested guest code past the host function that called it stops the process with a [RED] line (the host's frames between cannot be unwound); a nested call made
// while the thread's JIT instance runs (an SVC answered inside the JIT fallback) cannot use the JIT and stops the same way
uint64_t dm_guest_call(dm_guest_state *st, uint64_t addr, int nargs, ...);
// The process's main guest thread: entry with x0, x1, on a guest stack the host makes (8 MiB in the arena), until the process stops (svcExitProcess, a trap,
// the JIT fallback needed and not built in, a fault, guest code returning to the host); then every other guest thread is stopped (6.2's polls, SVCs, waits)
// and waited for, at most 10 s. Returns the status; *seconds the guest time
int dm_aot_run(uint64_t entry, uint64_t x0, uint64_t x1, double *seconds);
// dm_aot_run in parts, for a start that makes several outermost calls on the main thread (the host program's SDK bootstrap, main_x86.c: nn::init::Start's steps, then nnMain):
//   dm_aot_main_begin  the calling thread's block with the main guest stack and the packs' FPCR, left reset; every dm_guest_call on it then starts from that stack
//   dm_aot_main_left   why the calling thread's guest code left (DM_AOT_LEFT_*): DM_AOT_LEFT_NONE after a call that returned, else the start goes no further
//   dm_aot_main_end    the end of the main thread's guest code: a return to the host (left still DM_AOT_LEFT_NONE) is a process stop with DM_AOT_RETURNED and the [RED] line
//                      `returned`, or with DM_AOT_EXITED when returned is NULL (nnMain returning ends the process as on Horizon); then as dm_aot_run, the status
dm_guest_state *dm_aot_main_begin(void);
int dm_aot_main_left(void);
int dm_aot_main_end(const char *returned);
// Stop the process's guest code: the first status wins; every guest thread leaves at its next poll, SVC, JIT block boundary or kernel wait. why: a [RED]
// line, logged once (NULL for none)
void dm_aot_stop(int status, const char *why);
int dm_aot_stop_status(void);              // -1 while running
_Noreturn void dm_aot_leave(int why);      // the calling guest thread leaves its guest code (to its innermost dm_guest_call, which returns 0)
// ---------- Guest functions the host calls (design 2.4) ----------
// Apple's host calls guest code through plain C function pointers: an SDK function from guest_sym (the original of a forced take-over, the audio renderer's and the
// movie decoder's SDK functions, nn::os's system events), a callback the game registered (NVN's command-memory callback, the inline keyboard's), a module's DT_INIT.
// Here each such call is a guest call:
//   dm_gcall(fn, nargs, ...)      fn's guest address called with nargs integer or pointer arguments (each passed as uint64_t), on the calling thread's state block:
//                                 nested inside the host call the thread is answering (the usual case: every such site runs on a guest thread, inside an HLE entry or
//                                 the body of a host-created guest thread, hle_guestthread.c), or as an outermost call (the main thread between its start's steps).
//                                 A host thread that is no guest thread gets a guest context of its own at its first call (aot_thread.c dm_aot_host_thread_begin: a state
//                                 block, a 1 MiB guest stack in the arena and thread.c's TLS block and thread-local region, all freed when the thread ends), with a
//                                 [RED] line once per thread: it has no SDK thread, so guest code that asks nn::os for its current thread or makes a thread SVC fails
//                                 there (hle_thread.c dies: "thread SVC called from a non-guest thread"), as such a call has no meaning on Apple either
//   DM_GUEST_CALL(f, args...)     the call site's form, where Apple writes f(args...): the arguments widened to 64 bits (pointers and integers; a float or double
//                                 argument stops the link at dm_gcall_float_argument), at most 16, the result converted to the type f returns from x0 (an integer
//                                 narrower than 64 bits keeps x0's low bits, which AAPCS64 defines; a float or double result stops the link at
//                                 dm_gcall_float_result, and a struct result does not compile: such a site reads x1 / v0 or passes x8 itself, with dm_guest_call)
// The host files keep Apple's text: each defines GUEST_CALL as the plain call where the guest's code is native (DM_NATIVE_EXEC) or where a unit test supplies the
// guest's functions as C functions (DM_TEST_GUEST_C), and as DM_GUEST_CALL here
uint64_t dm_gcall(const void *fn, int nargs, ...);
dm_guest_state *dm_aot_host_thread_begin(uint64_t why_addr);   // aot_thread.c: the calling host thread's guest context (dm_gcall above); the block when it has one
void dm_aot_host_thread_active(int on);   // aot_thread.c: a host thread's outermost guest call starts (1) or ends (0): it counts as a live guest thread meanwhile
uint64_t dm_gcall_float_argument(void);   // never defined: a float or double argument of DM_GUEST_CALL fails the link
uint64_t dm_gcall_float_result(void);     // never defined: DM_GUEST_CALL of a function returning float or double fails the link
#define DM_GC_ISFP(x) (__builtin_types_compatible_p(__typeof__(x), float) || __builtin_types_compatible_p(__typeof__(x), double))
// A pointer argument (an array or a string literal decays to one; the argument is not evaluated) goes through the arena guard (DM_AOT_ARENA_CHECK, above), pos counting from 1
#define DM_GC_ISPTR(x) (__builtin_classify_type(x) == 5)
static inline uint64_t dm_gc_arg(uint64_t v, int isptr, const char *file, unsigned line, int pos) {
    if (isptr && v >= 0x10000 && v - DM_AOT_ARENA_BASE >= DM_AOT_ARENA_SIZE && atomic_load_explicit(&dm_aot_arena_check, memory_order_relaxed) > 0)
        dm_aot_arena_flag(file, line, pos, v, "guest call argument");
    return v;
}
#define DM_GC_A(x, pos) dm_gc_arg((uint64_t)__builtin_choose_expr(DM_GC_ISFP(x), dm_gcall_float_argument(), (uint64_t)(uintptr_t)(x)), DM_GC_ISPTR(x), __FILE__, __LINE__, (pos))
#define DM_GC_N(...) DM_GC_N_(0, ##__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define DM_GC_N_(z, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, n, ...) n
#define DM_GC_CAT(a, b) DM_GC_CAT_(a, b)
#define DM_GC_CAT_(a, b) a##b
// DM_GC_Mk(n, args...): the last k of the n arguments, each widened; the first of them is argument n - k + 1
#define DM_GC_M0(n, ...)
#define DM_GC_M1(n, a) , DM_GC_A(a, (n))
#define DM_GC_M2(n, a, ...) , DM_GC_A(a, (n) - 1) DM_GC_M1(n, __VA_ARGS__)
#define DM_GC_M3(n, a, ...) , DM_GC_A(a, (n) - 2) DM_GC_M2(n, __VA_ARGS__)
#define DM_GC_M4(n, a, ...) , DM_GC_A(a, (n) - 3) DM_GC_M3(n, __VA_ARGS__)
#define DM_GC_M5(n, a, ...) , DM_GC_A(a, (n) - 4) DM_GC_M4(n, __VA_ARGS__)
#define DM_GC_M6(n, a, ...) , DM_GC_A(a, (n) - 5) DM_GC_M5(n, __VA_ARGS__)
#define DM_GC_M7(n, a, ...) , DM_GC_A(a, (n) - 6) DM_GC_M6(n, __VA_ARGS__)
#define DM_GC_M8(n, a, ...) , DM_GC_A(a, (n) - 7) DM_GC_M7(n, __VA_ARGS__)
#define DM_GC_M9(n, a, ...) , DM_GC_A(a, (n) - 8) DM_GC_M8(n, __VA_ARGS__)
#define DM_GC_M10(n, a, ...) , DM_GC_A(a, (n) - 9) DM_GC_M9(n, __VA_ARGS__)
#define DM_GC_M11(n, a, ...) , DM_GC_A(a, (n) - 10) DM_GC_M10(n, __VA_ARGS__)
#define DM_GC_M12(n, a, ...) , DM_GC_A(a, (n) - 11) DM_GC_M11(n, __VA_ARGS__)
#define DM_GC_M13(n, a, ...) , DM_GC_A(a, (n) - 12) DM_GC_M12(n, __VA_ARGS__)
#define DM_GC_M14(n, a, ...) , DM_GC_A(a, (n) - 13) DM_GC_M13(n, __VA_ARGS__)
#define DM_GC_M15(n, a, ...) , DM_GC_A(a, (n) - 14) DM_GC_M14(n, __VA_ARGS__)
#define DM_GC_M16(n, a, ...) , DM_GC_A(a, (n) - 15) DM_GC_M15(n, __VA_ARGS__)
#define DM_GUEST_CALL(f, ...)                                                                                                         \
    ((void)__builtin_choose_expr(DM_GC_ISFP((f)(__VA_ARGS__)), dm_gcall_float_result(), 0),                                          \
     (__typeof__((f)(__VA_ARGS__)))dm_gcall((const void *)(uintptr_t)(f), DM_GC_N(__VA_ARGS__)                                       \
                                            DM_GC_CAT(DM_GC_M, DM_GC_N(__VA_ARGS__))(DM_GC_N(__VA_ARGS__), __VA_ARGS__)))

// Counters (the engine's own; the tests read them)
//   misses               dispatcher misses that bound an inline cache to a standard entry: of the calling pack's, or of another pack's (cross_pack)
//   cross_pack           of those, the ones into another pack: the inline cache then calls that pack's standard entry directly (the same signature, ABI 3)
//   cross_pack_handovers calls into another pack of another signature or mode, handed over through jit_entry and that pack's enter at every call
//   obs_gates, spin_gates  calls of the packs' gates (nxaot_rt_gate, design 1.1): observation gates, spin waits' back-edges
typedef struct {
    uint64_t svcs, host_calls, misses, cross_pack, safepoints, jit_runs, jit_addresses, threads;
    uint64_t cross_pack_handovers, obs_gates, spin_gates;
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
// An observation gate or a spin wait's back-edge at guest address pc (a manifest's obs and spin rows, registered by loader.c in hle.c's gate table): st holds the whole register
// file, which an observer may change (OBS_STDALLOC_INIT writes x2), so the caller reloads x0..x29 after it; a spin wait's back-off runs as an SVC frame (hle.c gate_dispatch)
DM_AOT_EXPORT void dm_aot_gate(dm_guest_state *st, uint64_t pc);
// The lifter's call of design 1.1 (include/nxaot/aot_module.h): compiled code at the gate site at module offset site of its pack (an entry of the descriptor's gates)
// with st holding the whole register file and st->pc; dm_aot_gate at the site's guest address, then a process stop leaves from here
DM_AOT_EXPORT void nxaot_rt_gate(dm_guest_state *st, uint64_t site);

// ---------- Faults (aot_fault.c) ----------
void crash_install(void);                  // the fault handlers (signals on POSIX, a vectored exception handler on Windows); once
void crash_thread_init(void);              // the calling thread's alternate signal stack (POSIX) or stack guarantee (Windows)
void dm_aot_fault_after_jit(void);         // after the first JIT instance: the host's handler again in front of dynarmic's (5.4's order)

// ---------- The thread layer's side (hle_thread.c, x86-64 only) ----------
int thr_safepoint(void);                   // a poll found the poll byte set: wait here while SetThreadActivity pauses this thread; 1 when it waited
void thr_stop_all(void);                   // process stop: every kernel wait woken (cancelled), every pause released
int thr_handles(uint32_t *out, int max, uint32_t except);   // the handles of the live (started, not exited) guest threads, but except's
