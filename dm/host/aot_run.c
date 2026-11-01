// The run time of the x86-64 execution engine (docs/design/aot-recompiler.md 2.4, 2.5, 3.3, 4, 6.2, 8, 11; aot.h): what a pack's library imports
// (include/nxaot/aot_module.h), dm_guest_call with the outermost level of a guest thread, and process stop.
//   dm_aot_svc          svc #n: the block holds the whole register file; svc_dispatch (hle_svc.c) answers on st->x as on the gate frame on Apple, between
//                       thr_svc_enter / thr_svc_leave (hle_thread.c: a thread paused inside an SVC); svcExitProcess stops the process
//   nxaot_rt_dispatch   an inline-cache miss: the lookup table of the pack that holds the target, the caller's or another of the same standard-entry signature (ABI 3:
//                       a descriptor published in the site's slot, so the next call goes there directly), else the caller pack's jit_entry with the target in
//                       st->pc: the host-function page (a host call), another pack of another signature (its enter), or the JIT fallback
//   nxaot_rt_jit        what compiled code hands over (jit_entry's call, msr fpcr, untranslated encodings, refused functions): host calls, compiled entries
//                       called natively through the pack's enter trampoline, non-local transfers propagated (3.3), the rest in the JIT (dynarmic, NXHOST_JIT)
//                       or, without it, a [RED] line per address and a process stop with status DM_AOT_JIT_NEEDED
//   nxaot_rt_trap       brk, undefined encodings, a no-return call that returned: a [RED] line and a process stop
//   nxaot_rt_safepoint  a poll found the poll byte set (6.2): the pause of SetThreadActivity (hle_thread.c), or a process stop
//   dm_aot_cntpct       mrs cntpct_el0 / cntvct_el0 (8): the guest clock in 19.2 MHz ticks, as svcGetSystemTick and Apple's counter gate read it
//   nxaot_rt_gate       the lifter's call of design 1.1 at a gate site of the pack's descriptor: dm_aot_gate at its guest address
//   dm_aot_gate         a manifest's observation gate or spin wait back-edge (loader.c registers them in hle.c's gate table): the observer, or the back-off as an SVC frame
// dm_guest_call nests: a host function guest code called may call guest code again on the same thread (aot.h), each level a base of its own that guest code leaving
// returns through, the host call around it leaving in turn (host_call). dm_gcall is the form the host's plain-pointer call sites use (aot.h DM_GUEST_CALL)
// A call to the stub page (aot.h: an import nothing implements) is answered as a host call is, by hle.c's unimpl_c
// Guest code runs with the MXCSR of the module's FPCR (7.1), set at every entry into compiled code from the host; host calls run with the guest thread's MXCSR,
// as host calls run with the guest's FPCR on Apple
#include "aot.h"
#include "aot_hostfn.h"
#include <stdarg.h>
#include <xmmintrin.h>
#if NXHOST_JIT
#include "rt/jit.h"   // dm/tools/aot/src/rt: the JIT fallback (dynarmic) behind its C interface
#endif

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c
void thr_svc_enter(uint64_t *regs, uint64_t pc);                       // hle_thread.c
void thr_svc_leave(void);

#define FPCR_MODE 0x07C80000u   // the FPCR bits code is specialised on (AHP, DN, FZ, RMode, FZ16: dynarmic's location descriptor keeps these)
// Why compiled code handed a continuation to nxaot_rt_jit, besides the NXAOT_TRAP_* reasons: host-private values of st->exit_reason
enum { REASON_HOSTCALL = 0x100, REASON_CROSS = 0x101 };

static _Atomic uint64_t c_svcs, c_host_calls, c_misses, c_cross, c_safepoints, c_jit_runs, c_cross_handovers, c_obs_gates, c_spin_gates;
static _Atomic int g_stop = -1;   // the status of the process stop, -1 while running

// ---------- Process stop and leaving guest code ----------
int dm_aot_stop_status(void) { return atomic_load(&g_stop); }

void dm_aot_stop(int status, const char *why) {
    int none = -1;
    atomic_compare_exchange_strong(&g_stop, &none, status);   // the first status wins
    if (why) LOG("%s", why);
    dm_aot_threads_poke_all();   // the poll byte of every guest thread, and its JIT instance halted
    thr_stop_all();              // hle_thread.c: every kernel wait woken and cancelled, every pause released
}

_Noreturn void dm_aot_leave(int why) {
    dm_aot_thread *t = dm_aot_self();
    if (!t || !t->depth) die("aot: a thread left guest code it is not running");
    t->left = why;
    dm_aot_longjmp(t->base);
}

static void check_stop(dm_aot_thread *t) {
    (void)t;
    if (atomic_load_explicit(&g_stop, memory_order_relaxed) >= 0) dm_aot_leave(DM_AOT_LEFT_STOP);
}

uint32_t dm_aot_guest_mxcsr(uint32_t fpcr) {
    static const uint32_t rc[4] = {0x0000, 0x4000, 0x2000, 0x6000};   // RN, RP, RM, RZ as MXCSR.RC
    uint32_t m = 0x1F80 | rc[(fpcr >> 22) & 3];                       // every exception masked
    if ((fpcr >> 24) & 1) m |= 0x8040;                                 // FZ: FTZ and DAZ
    return m;
}
static int module_mode(const dm_guest_state *st) { return (st->fpcr & FPCR_MODE) == (dm_aot_module_fpcr() & FPCR_MODE); }

// Addresses the JIT fallback ran from (or would have): the [RED] line once per address, and their count
#define MAX_SEEN 4096
static uint64_t g_seen[MAX_SEEN];
static int g_nseen;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static int first_time(uint64_t pc) {
    pthread_mutex_lock(&g_seen_lock);
    int first = 1;
    for (int i = 0; i < g_nseen; i++) if (g_seen[i] == pc) { first = 0; break; }
    if (first && g_nseen < MAX_SEEN) g_seen[g_nseen++] = pc;
    pthread_mutex_unlock(&g_seen_lock);
    return first;
}

static const char *jit_why(uint32_t reason) {
    switch (reason) {
    case NXAOT_TRAP_JIT_MISS: return "an indirect branch to an address with no compiled entry";
    case NXAOT_TRAP_JIT_TARGET: return "a branch or call to an address that is no compiled entry";
    case NXAOT_TRAP_JIT_FPCR: return "msr fpcr left the FPCR the module was compiled for";
    case NXAOT_TRAP_JIT_REFUSED: return "a function the lifter refused";
    case NXAOT_TRAP_NO_CODE: return "code that runs into no compiled entry";
    case NXAOT_TRAP_UNTRANSLATED: return "an encoding the lifter does not translate";
    case REASON_HOSTCALL: return "a host call";
    case REASON_CROSS: return "a call into another pack";
    default: return "a continuation compiled code handed over";
    }
}

// ---------- Traps ----------
static _Noreturn void trap(dm_aot_thread *t, uint64_t guest, uint32_t reason) {
    char at[64], msg[256]; int status = DM_AOT_TRAP;
    dm_aot_guest_name(guest, at, sizeof at);
    switch (reason) {
    case NXAOT_TRAP_BRK: snprintf(msg, sizeof msg, "[RED] aot: brk at %s", at); break;
    case NXAOT_TRAP_UNDEFINED: case NXAOT_TRAP_UNPREDICTABLE: snprintf(msg, sizeof msg, "[RED] aot: an undefined or unpredictable instruction at %s", at); break;
    case NXAOT_TRAP_NORETURN:
        status = DM_AOT_RETURNED;
        snprintf(msg, sizeof msg, "[RED] aot: the call at %s returned, but the analysis says its callee does not return", at);
        break;
    case NXAOT_TRAP_UNTRANSLATED: snprintf(msg, sizeof msg, "[RED] aot: an encoding neither the lifter nor the JIT fallback translates at %s", at); break;
    default: snprintf(msg, sizeof msg, "[RED] aot: trap %u at %s", reason, at); break;
    }
    dm_aot_stop(status, msg);
    dm_backtrace(t->st.x[29]);   // the block holds the whole register file at a trap
    dm_aot_leave(DM_AOT_LEFT_STOP);
}

// ---------- Host calls and native calls ----------
static void host_call(dm_aot_thread *t, uint64_t pc) {
    atomic_fetch_add_explicit(&c_host_calls, 1, memory_order_relaxed);
    dm_aot_hostcall(&t->st, pc);   // the slot's thunk: arguments from the block, the result into x0 / v0, on this thread's native stack (2.4)
    if (t->left != DM_AOT_LEFT_NONE) dm_aot_leave(t->left);         // a guest call the host function made left its guest code (nested re-entry): this level leaves too
    if (atomic_load(&t->exiting)) dm_aot_leave(DM_AOT_LEFT_EXIT);   // the host function answered ExitThread for this thread
    check_stop(t);
    t->st.pc = t->st.x[30];   // the guest goes on at its return address
}

// A guest call reached the slot of unimplemented import k (aot.h, the stub page): hle.c's unimpl_c logs it once and answers 0, as Apple's entry.S stub does
static void stub_call(dm_aot_thread *t, long k) {
    uint64_t unimpl_c(unsigned idx, uint64_t *regs);   // hle.c
    atomic_fetch_add_explicit(&c_host_calls, 1, memory_order_relaxed);
    t->st.x[0] = unimpl_c((unsigned)k, t->st.x);
    check_stop(t);
    t->st.pc = t->st.x[30];
}

// A compiled entry the JIT loop reached: its standard entry with the block's registers (2.5), under the MXCSR of the thread's FPCR
static void native_call(dm_aot_thread *t, const dm_aot_pack *p, const void *entry) {
    _mm_setcsr(dm_aot_guest_mxcsr(t->st.fpcr));
    t->st.pending = 0;
    dm_aot_pack_desc(p)->enter(&t->st, entry);
}

// ---------- The JIT fallback (11) ----------
static int aot_svc(dm_guest_state *st, uint32_t n);
static void safepoint(dm_aot_thread *t);

#if NXHOST_JIT
static pthread_once_t g_jit_once = PTHREAD_ONCE_INIT;
static int jit_stop_at(void *ctx, uint64_t pc) {
    (void)ctx;
    if (dm_aot_find(pc, NULL)) return DM_JIT_ENTRY;   // continued natively
    return dm_aot_is_return(pc) ? DM_JIT_RETURN : 0;  // a native frame may wait there
}
static int jit_svc(void *ctx, dm_guest_state *st, uint32_t n) { (void)ctx; return aot_svc(st, n); }
DM_AOT_EXPORT uint64_t dm_aot_cntpct(dm_guest_state *st);
static uint64_t jit_cntpct(void *ctx, dm_guest_state *st) { (void)ctx; return dm_aot_cntpct(st); }
static void jit_init_once(void) {
    dm_jit_host h;
    memset(&h, 0, sizeof h);
    h.code_lo = dm_aot_code_lo(); h.code_hi = dm_aot_code_hi();   // the loaded modules' text; the host-function page and DM_AOT_HOST_RETURN lie outside it
    h.fpcr = dm_aot_module_fpcr() & FPCR_MODE;
    h.max_threads = 64; h.code_cache = 16u << 20;
    h.stop_at = jit_stop_at; h.svc = jit_svc; h.cntpct = jit_cntpct;
    dm_jit_init(&h);
    LOG("aot: the JIT fallback (dynarmic) for guest text %#llx..%#llx", (unsigned long long)h.code_lo, (unsigned long long)h.code_hi);
}
#endif

// Run st->pc in the JIT until it stops; without it, the [RED] line and the process stop
static void run_jit(dm_aot_thread *t, uint64_t lr, const char **why) {
    const uint64_t pc = t->st.pc;
    char name[64];
    dm_aot_guest_name(pc, name, sizeof name);
#if !NXHOST_JIT
    (void)lr;
    char msg[320];
    snprintf(msg, sizeof msg, "[RED] aot: %s at %s needs the JIT fallback (design section 11), which this host is built without (CMake option NXHOST_JIT)", *why, name);
    dm_aot_stop(DM_AOT_JIT_NEEDED, first_time(pc) ? msg : NULL);
    dm_aot_leave(DM_AOT_LEFT_STOP);
#else
    static _Thread_local int t_jit_running;   // dm_jit_run is on this thread's stack: an SVC answered inside the JIT made a nested guest call (aot.h, the limits of nesting)
    if (t_jit_running) {
        char msg[320];
        snprintf(msg, sizeof msg, "[RED] aot: %s at %s needs the JIT fallback inside a guest call the host made while this thread's JIT instance runs (an SVC answered inside the JIT): one instance per thread, not re-entrant", *why, name);
        dm_aot_stop(DM_AOT_JIT_NEEDED, msg);
        dm_aot_leave(DM_AOT_LEFT_STOP);
    }
    pthread_once(&g_jit_once, jit_init_once);
    dm_jit *j = t->jit;
    if (!j) {
        if (!(j = dm_jit_create(&t->st))) {
            dm_aot_stop(DM_AOT_JIT_NEEDED, "[RED] aot: no JIT instance for this guest thread (more than 64 threads in the JIT fallback)");
            dm_aot_leave(DM_AOT_LEFT_STOP);
        }
        static atomic_flag faults_ordered = ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set(&faults_ordered)) dm_aot_fault_after_jit();   // dynarmic installed its fault handler with this first instance (5.4's order)
        dm_aot_thread_set_jit(j);   // halted at once when a pause or stop was requested before it existed
    }
    if (first_time(pc)) LOG("[RED] aot: %s at %s runs in the JIT fallback (design section 11)", *why, name);
    atomic_fetch_add_explicit(&c_jit_runs, 1, memory_order_relaxed);
    t_jit_running = 1;
    const uint64_t stop = dm_jit_run(j, pc);
    t_jit_running = 0;
    *why = "a non-local transfer to a continuation with no native frame";
    switch (dm_jit_reason(j)) {
    case DM_JIT_ENTRY: case DM_JIT_RETURN: return;   // st->pc = stop: the loop decides
    case DM_JIT_HALTED: safepoint(t); return;        // a pause or a stop
    case DM_JIT_CALLBACK: dm_aot_leave(t->left != DM_AOT_LEFT_NONE ? t->left : atomic_load(&t->exiting) ? DM_AOT_LEFT_EXIT : DM_AOT_LEFT_STOP);   // the svc ended the thread or the process
    case DM_JIT_NO_CODE:
        if (stop == DM_AOT_HOST_RETURN || stop == lr || dm_aot_hostfn_name(stop) || dm_aot_stub_index(stop) >= 0) return;
        {
            char at[64], msg[200];
            dm_aot_guest_name(stop, at, sizeof at);
            snprintf(msg, sizeof msg, "[RED] aot: the JIT fallback reached %s, which holds no guest code", at);
            dm_aot_stop(DM_AOT_FAULT, msg);
        }
        dm_aot_leave(DM_AOT_LEFT_STOP);
    default: {
        const uint32_t r = dm_jit_trap(j);
        trap(t, stop, r == DM_EXIT_BRK ? NXAOT_TRAP_BRK : r == DM_EXIT_UNTRANSLATED ? NXAOT_TRAP_UNTRANSLATED : NXAOT_TRAP_UNDEFINED);
    }
    }
#endif
}

// A non-local transfer (3.3) to a return address of compiled code with an SP above the one a nested guest call was made with: its target frame lies beyond the host
// function that made the call (a callback that longjmps or throws past it), whose native frames cannot be unwound
static _Noreturn void nonlocal_out(dm_aot_thread *t, uint64_t pc) {
    (void)t;
    char at[64], msg[300];
    dm_aot_guest_name(pc, at, sizeof at);
    snprintf(msg, sizeof msg, "[RED] aot: a non-local transfer (longjmp or a C++ exception) to %s leaves guest code the host called from inside a host call, past the host function that called it: its frames cannot be unwound (design 3.3)", at);
    dm_aot_stop(DM_AOT_TRAP, msg);
    dm_aot_leave(DM_AOT_LEFT_STOP);
}

// The JIT driver (the reference is dm/tools/aot/src/rt/host.cpp Host::JitLoop): st->pc is where the guest continues, st holds the whole register file. Runs
// until the guest returns to lr (the native frame that handed over the continuation; DM_AOT_HOST_RETURN at a thread's outermost level), taking host calls and
// compiled entries natively on the way (their normal return continues at the lr they were called with, x30). A return address of compiled code reached
// with an SP above the one this frame was entered with is a non-local transfer (3.3): nested, it is propagated to the native frames further out
// (st->pending); at the outermost level of a guest call nothing can own it and the JIT runs it, unless that guest call is nested in a host call and the transfer
// leaves it (sp is then the SP the call was made with: nonlocal_out)
static void jit_loop(dm_aot_thread *t, uint64_t lr, uint64_t sp, int outermost, const char *why) {
    dm_guest_state *st = &t->st;
    for (;;) {
        check_stop(t);
        const uint64_t pc = st->pc;
        const int mode = module_mode(st);
        // The return the native frame waits for: lr alone decides, as a compiled function's own return does (3.3)
        if (pc == lr) {
            st->pending = outermost || mode ? 0 : pc;   // under another FPCR the frame cannot go on: its callers propagate the transfer
            return;
        }
        if (pc == DM_AOT_HOST_RETURN) { st->pending = pc; return; }   // the guest returned to the host while native frames wait
        if (dm_aot_hostfn_name(pc)) { host_call(t, pc); continue; }   // a host function through its guest-visible address (2.4)
        if (dm_aot_stub_index(pc) >= 0) { stub_call(t, dm_aot_stub_index(pc)); continue; }   // an unimplemented import
        if (mode) {
            const dm_aot_pack *p;
            const nxaot_lookup_entry *e = dm_aot_find(pc, &p);
            if (e) {
                const uint64_t ret = st->x[30], sp_at_call = st->sp;
                native_call(t, p, dm_aot_entry_of(e));
                if (st->pending) { st->pc = st->pending; st->pending = 0; }   // a non-local transfer: the block holds the whole register file
                else {
                    // a normal return is a ret to the lr the entry was called with; SP is the call's unless the function may change it (NXAOT_LOOKUP_SP_KEPT)
                    st->pc = ret; st->x[30] = ret;
                    if (e->flags & NXAOT_LOOKUP_SP_KEPT) st->sp = sp_at_call;
                }
                continue;
            }
            if (!outermost && st->sp > sp && dm_aot_is_return(pc)) { st->pending = pc; return; }   // a native frame further out may wait there
            if (outermost && t->depth > 1 && st->sp > sp && dm_aot_is_return(pc)) nonlocal_out(t, pc);
        }
        run_jit(t, lr, &why);
    }
}

// ---------- The outermost level of a host-to-guest call ----------
// pc with st's registers: a host function, a compiled entry natively, a non-local transfer that reaches this level dispatched as a new call, what has no
// compiled entry in the JIT. Returns when the guest returns to the host
static void run_guest(dm_aot_thread *t, uint64_t pc) {
    dm_guest_state *st = &t->st;
    const char *why = "a call from the host to an address with no compiled entry";
    const uint64_t sp0 = st->sp;   // the SP the call was made with: a nested call's guest code stays below it
    for (;;) {
        check_stop(t);
        if (pc == DM_AOT_HOST_RETURN) return;
        st->pc = pc;
        if (dm_aot_hostfn_name(pc)) { host_call(t, pc); pc = st->pc; continue; }
        if (dm_aot_stub_index(pc) >= 0) { stub_call(t, dm_aot_stub_index(pc)); pc = st->pc; continue; }
        if (module_mode(st)) {
            const dm_aot_pack *p;
            const nxaot_lookup_entry *e = dm_aot_find(pc, &p);
            if (e) {
                _mm_setcsr(dm_aot_guest_mxcsr(st->fpcr));
                st->pending = 0;
                dm_aot_pack_desc(p)->enter(st, dm_aot_entry_of(e));
                if (!st->pending) return;
                pc = st->pending; st->pending = 0;
                if (t->depth > 1 && st->sp > sp0) nonlocal_out(t, pc);
                why = "a non-local transfer to a continuation with no native frame";
                continue;
            }
        }
        jit_loop(t, DM_AOT_HOST_RETURN, sp0, 1, why);
        st->pending = 0;
        return;
    }
}

// A fault in guest code (aot_fault.c jumped to the thread's base with the facts): the report and the process stop
static void fault_report(dm_aot_thread *t) {
    if (!t->fault_signal) return;   // reported at a nested level already
    char where[200], msg[400];
    if (t->fault_jit_pc) { char g[64]; dm_aot_guest_name(t->fault_jit_pc, g, sizeof g); snprintf(where, sizeof where, "a memory access of the JIT fallback near guest %s", g); }
    else if (!dm_aot_where(t->fault_pc, where, sizeof where)) snprintf(where, sizeof where, "host pc %#llx", (unsigned long long)t->fault_pc);
    const char *sig = t->fault_signal == 11 ? "SIGSEGV" : t->fault_signal == 7 ? "SIGBUS" : t->fault_signal == 4 ? "SIGILL" : t->fault_signal == 8 ? "SIGFPE" : "an exception";
    snprintf(msg, sizeof msg, "[RED] aot: %s at address %#llx in guest code (%s)", sig, (unsigned long long)t->fault_address, where);
    dm_aot_stop(DM_AOT_FAULT, msg);
    t->fault_signal = 0;
}

// dm_guest_call's arguments from a va_list (dm_gcall's too)
static uint64_t guest_vcall(dm_guest_state *st, uint64_t addr, int nargs, va_list ap) {
    dm_aot_thread *t = dm_aot_self();
    if (!t || &t->st != st) die("aot: dm_guest_call(%#llx) with a state block that is not the calling thread's", (unsigned long long)addr);
    if (nargs < 0 || nargs > 16) die("aot: dm_guest_call(%#llx) with %d arguments (x0..x7 and 8 stacked at most)", (unsigned long long)addr, nargs);
    uint64_t a[16] = {0};
    for (int i = 0; i < nargs; i++) a[i] = va_arg(ap, uint64_t);
    // A nested call after one that left (aot.h): the host frames unwind to the next level first, and no guest code runs meanwhile
    if (t->depth && t->left != DM_AOT_LEFT_NONE) return 0;
    // What the guest code around this call keeps (AAPCS64 lets the callee change the rest): x19..x30, SP, pc, a pending transfer
    uint64_t keep[12], keep_sp = st->sp, keep_pc = st->pc, keep_pending = st->pending;
    const uint32_t keep_reason = st->exit_reason, host_mxcsr = _mm_getcsr();
    memcpy(keep, &st->x[19], sizeof keep);
    // Each level is a base: the one around it (a nested call's) is kept here and put back when this one returns or is left
    dm_aot_jmp_buf outer;
    const int depth0 = t->depth;
    if (depth0) memcpy(&outer, &t->base, sizeof outer);
    if (dm_aot_setjmp(t->base)) {
        // The thread left its guest code: ExitThread, a process stop, a trap or a fault (reported once, at the level that caught it)
        t->depth = depth0;
        if (depth0) memcpy(&t->base, &outer, sizeof outer);
        _mm_setcsr(host_mxcsr);
        if (t->left == DM_AOT_LEFT_FAULT) fault_report(t);
        return 0;
    }
    t->depth++;
    for (int i = 0; i < nargs && i < 8; i++) st->x[i] = a[i];
    if (nargs > 8) {   // the stacked arguments in 8-byte slots from the callee's SP up, which stays 16-byte aligned
        st->sp = (st->sp - 8 * (uint64_t)(nargs - 8)) & ~(uint64_t)15;
        memcpy((void *)(uintptr_t)st->sp, &a[8], 8 * (size_t)(nargs - 8));
    }
    st->x[30] = DM_AOT_HOST_RETURN;
    st->pending = 0;
    _mm_setcsr(dm_aot_guest_mxcsr(st->fpcr));
    run_guest(t, addr);
    const uint64_t r = st->x[0];
    memcpy(&st->x[19], keep, sizeof keep);
    st->sp = keep_sp; st->pc = keep_pc; st->pending = keep_pending; st->exit_reason = keep_reason;
    t->depth--;
    if (depth0) memcpy(&t->base, &outer, sizeof outer);
    _mm_setcsr(host_mxcsr);
    return r;
}

uint64_t dm_guest_call(dm_guest_state *st, uint64_t addr, int nargs, ...) {
    va_list ap; va_start(ap, nargs);
    const uint64_t r = guest_vcall(st, addr, nargs, ap);
    va_end(ap);
    return r;
}

// A plain-pointer call site of the host (aot.h DM_GUEST_CALL): on the calling thread's block, nested or outermost. A host thread with no guest thread gets a guest
// context of its own (aot_thread.c), which counts as a live guest thread while its outermost call runs, so that a process stop waits for it to leave
uint64_t dm_gcall(const void *fn, int nargs, ...) {
    const uint64_t addr = (uint64_t)(uintptr_t)fn;
    if (!addr) die("aot: a guest call of a null function from the host");
    dm_guest_state *st = dm_aot_self() ? &dm_aot_self()->st : dm_aot_host_thread_begin(addr);
    dm_aot_thread *t = dm_aot_self();
    const int host_outer = t->host && !t->depth;
    if (host_outer) {
        st->sp = t->stack_lo + t->stack_size;   // each outermost call from the top of its guest stack (a call that left kept no SP)
        t->left = DM_AOT_LEFT_NONE;
        dm_aot_host_thread_active(1);
    }
    va_list ap; va_start(ap, nargs);
    const uint64_t r = guest_vcall(st, addr, nargs, ap);
    va_end(ap);
    if (host_outer) dm_aot_host_thread_active(0);
    return r;
}

dm_guest_state *dm_aot_main_begin(void) {
    dm_guest_state *st = dm_aot_thread_begin();
    dm_aot_thread *t = dm_aot_self();
    if (!t->stack_lo) {   // the main thread's guest stack (2.4): its native stack is the host's, and QueryMemory reports this one (hle_svc.c)
        t->stack_size = 8u << 20;
        t->stack_lo = (uint64_t)(uintptr_t)dm_guest_alloc(t->stack_size);
    }
    st->sp = t->stack_lo + t->stack_size;
    st->fpcr = dm_aot_module_fpcr();   // the FPCR the packs are specialised for (7.1): its block may predate them (hle_thread.c svc_thread_main_init)
    t->left = DM_AOT_LEFT_NONE;
    return st;
}

int dm_aot_main_left(void) { const dm_aot_thread *t = dm_aot_self(); return t ? t->left : DM_AOT_LEFT_NONE; }

int dm_aot_main_end(const char *returned) {
    dm_aot_thread *t = dm_aot_self();
    if (t->left == DM_AOT_LEFT_NONE) {
        if (returned) dm_aot_stop(DM_AOT_RETURNED, returned);
        else dm_aot_stop(DM_AOT_EXITED, NULL);   // nnMain returned: Horizon's process ends as after svcExitProcess
    } else if (t->left == DM_AOT_LEFT_EXIT) {
        // The main thread ended through svcExitThread: the process goes on with its other threads until one ends it, or none is left
        while (dm_aot_stop_status() < 0 && dm_aot_threads_live() > 0) plat_sleep_ns(10000000);
        if (dm_aot_stop_status() < 0) dm_aot_stop(DM_AOT_EXITED, NULL);
    }
    int status = dm_aot_stop_status();
    int stuck = dm_aot_threads_wait(10000);
    if (stuck) { LOG("[RED] aot: %d guest threads did not stop within 10 s of the end of the run (no poll, SVC or wait reached?)", stuck); status = DM_AOT_STUCK; }
    return status;
}

int dm_aot_run(uint64_t entry, uint64_t x0, uint64_t x1, double *seconds) {
    dm_guest_state *st = dm_aot_main_begin();
    const uint64_t t0 = plat_now_ns();
    dm_guest_call(st, entry, 2, x0, x1);
    char at[64], msg[160];
    dm_aot_guest_name(entry, at, sizeof at);
    snprintf(msg, sizeof msg, "[RED] aot: guest code returned to the host from its entry %s", at);
    const int status = dm_aot_main_end(msg);
    if (seconds) *seconds = (double)(plat_now_ns() - t0) * 1e-9;
    return status;
}

void dm_aot_stats(dm_aot_stats_t *o) {
    memset(o, 0, sizeof *o);
    o->svcs = atomic_load(&c_svcs); o->host_calls = atomic_load(&c_host_calls); o->misses = atomic_load(&c_misses); o->cross_pack = atomic_load(&c_cross);
    o->safepoints = atomic_load(&c_safepoints); o->jit_runs = atomic_load(&c_jit_runs); o->threads = dm_aot_threads_made();
    o->cross_pack_handovers = atomic_load(&c_cross_handovers); o->obs_gates = atomic_load(&c_obs_gates); o->spin_gates = atomic_load(&c_spin_gates);
    pthread_mutex_lock(&g_seen_lock); o->jit_addresses = (uint64_t)g_nseen; pthread_mutex_unlock(&g_seen_lock);
}

// ---------- What a pack imports ----------
// svc #n (2.4): 1 when the thread must leave (it exited, or the process stops)
static int aot_svc(dm_guest_state *st, uint32_t n) {
    dm_aot_thread *t = (dm_aot_thread *)st;
    atomic_fetch_add_explicit(&c_svcs, 1, memory_order_relaxed);
    if (dm_aot_stop_status() >= 0) return 1;
    if (n == 0x07) { dm_aot_stop(DM_AOT_EXITED, NULL); return 1; }   // svcExitProcess: Horizon ends the process; every guest thread stops at its next poll, SVC or wait
    const uint64_t at = st->pc - 4;   // the svc instruction
    const module_t *m = module_of((const void *)(uintptr_t)at);
    thr_svc_enter(st->x, at);         // the block is complete here: a thread paused inside the SVC has its context there (GetThreadContext3)
    svc_dispatch((int)n, st->x, m ? at - (uint64_t)(uintptr_t)m->base : at);
    thr_svc_leave();                  // waits here while SetThreadActivity pauses the thread
    return t->left != DM_AOT_LEFT_NONE || atomic_load(&t->exiting) || dm_aot_stop_status() >= 0;   // left: a guest call the SVC's host code made left (nested)
}

DM_AOT_EXPORT void dm_aot_svc(dm_guest_state *st, uint32_t n) {
    dm_aot_thread *t = (dm_aot_thread *)st;
    if (aot_svc(st, n)) dm_aot_leave(t->left != DM_AOT_LEFT_NONE ? t->left : atomic_load(&t->exiting) ? DM_AOT_LEFT_EXIT : DM_AOT_LEFT_STOP);
}

// The descriptors the inline caches point at (4): one per guest address, immutable once published, never freed
typedef struct desc_node { nxaot_ic_desc d; struct desc_node *next; } desc_node;
static desc_node *g_desc[4096];
static pthread_mutex_t g_desc_lock = PTHREAD_MUTEX_INITIALIZER;
static const nxaot_ic_desc *desc_for(uint64_t guest, const void *native) {
    const unsigned h = (unsigned)(((guest >> 2) * 0x9E3779B97F4A7C15ull) >> 52);
    pthread_mutex_lock(&g_desc_lock);
    desc_node *n = g_desc[h];
    while (n && n->d.guest != guest) n = n->next;
    if (!n) {
        if (!(n = malloc(sizeof *n))) die("aot: no memory for an inline-cache descriptor");
        n->d.guest = guest; n->d.native = (void *)native; n->next = g_desc[h]; g_desc[h] = n;
    }
    pthread_mutex_unlock(&g_desc_lock);
    return &n->d;
}

DM_AOT_EXPORT void *nxaot_rt_dispatch(dm_guest_state *st, uint64_t target, const nxaot_ic_desc **slot, uint64_t site) {
    (void)site;
    // The pack whose code missed: its call sites pass the standard entries' signature its descriptor records (std_x, std_v, rv: ABI 3)
    const dm_aot_pack *caller = dm_aot_pack_of_code((uint64_t)(uintptr_t)__builtin_return_address(0));
    if (!caller) die("aot: a dispatcher miss from outside every pack (return address %p)", __builtin_return_address(0));
    const dm_aot_pack *p = NULL;
    const nxaot_lookup_entry *e = dm_aot_find(target, &p);
    // A standard entry of the caller's pack, or of another pack with the same signature, convention and mode (a cross-pack call: an NRO's function from main, main's
    // called back from the NRO): published in the site's slot, so the next call there enters it directly, with no host on the way
    if (e && (p == caller || nxaot_same_std_abi(dm_aot_pack_desc(caller), dm_aot_pack_desc(p)))) {
        const void *native = dm_aot_entry_of(e);
        atomic_fetch_add_explicit(&c_misses, 1, memory_order_relaxed);
        if (p != caller) atomic_fetch_add_explicit(&c_cross, 1, memory_order_relaxed);
        __atomic_store_n(slot, desc_for(target, native), __ATOMIC_RELEASE);
        return (void *)native;
    }
    // No standard entry the caller can call: jit_entry with the target in st->pc (a host call; another pack's entry of another signature, through its enter; the JIT
    // fallback). The slot keeps its descriptor, so the next call misses again
    if (e) atomic_fetch_add_explicit(&c_cross_handovers, 1, memory_order_relaxed);
    st->pc = target;
    st->exit_reason = e ? REASON_CROSS : dm_aot_hostfn_name(target) || dm_aot_stub_index(target) >= 0 ? REASON_HOSTCALL : NXAOT_TRAP_JIT_MISS;
    return (void *)dm_aot_pack_jit_entry(caller);
}

DM_AOT_EXPORT void nxaot_rt_trap(dm_guest_state *st, uint64_t offset, uint32_t reason) {
    const dm_aot_pack *caller = dm_aot_pack_of_code((uint64_t)(uintptr_t)__builtin_return_address(0));
    trap((dm_aot_thread *)st, caller ? dm_aot_pack_base(caller) + offset : offset, reason);
}

DM_AOT_EXPORT void nxaot_rt_jit(dm_guest_state *st, uint64_t lr, uint64_t sp) {
    dm_aot_thread *t = (dm_aot_thread *)st;
    const uint32_t reason = st->exit_reason;
    st->exit_reason = 0;
    jit_loop(t, lr, sp, 0, jit_why(reason));
    _mm_setcsr(dm_aot_guest_mxcsr(dm_aot_module_fpcr()));   // back to compiled code under its specialisation (jit_loop returns pending otherwise)
}

// The poll byte was set: clear it first (a request made after the clear sets it again), then the stop, the pause, the stop again
static void safepoint(dm_aot_thread *t) {
    __atomic_store_n(&t->st.poll, (uint8_t)0, __ATOMIC_SEQ_CST);
    check_stop(t);
    if (thr_safepoint()) atomic_fetch_add_explicit(&c_safepoints, 1, memory_order_relaxed);
    check_stop(t);
}

DM_AOT_EXPORT void nxaot_rt_safepoint(dm_guest_state *st) { safepoint((dm_aot_thread *)st); }

DM_AOT_EXPORT uint64_t dm_aot_cntpct(dm_guest_state *st) {
    (void)st;
    guest_clock_read_wait();          // waits while the app is in the background (hle_os.c)
    return guest_ns() * 12 / 625;     // 19.2 MHz, as svcGetSystemTick (hle_svc.c) and the counter gate (hle.c) compute it
}

// An observation gate or a spin wait's back-edge (aot.h): hle.c's gate table, as dm_gate's frame reaches it on Apple, with the block's registers as the frame
DM_AOT_EXPORT void dm_aot_gate(dm_guest_state *st, uint64_t pc) {
    uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);   // hle.c
    (void)gate_dispatch(pc, st->x);
}

// The lifter's call of design 1.1 (include/nxaot/aot_module.h): the gate site at module offset site of the pack whose code called (through its nxaot.gate wrapper, so the
// return address lies in the pack's code). The block holds the whole register file and st->pc: an observer reads and may change x0..x29 (the code reloads them), the spin
// back-off may sleep and be paused there as in an SVC (hle.c gate_dispatch). A process stop leaves at once
DM_AOT_EXPORT void nxaot_rt_gate(dm_guest_state *st, uint64_t site) {
    dm_aot_thread *t = (dm_aot_thread *)st;
    const dm_aot_pack *p = dm_aot_pack_of_code((uint64_t)(uintptr_t)__builtin_return_address(0));
    const nxaot_gate_site *g = p ? nxaot_gate_at(dm_aot_pack_desc(p), site) : NULL;
    if (!g) die("[RED] aot: a gate call at module offset %#llx that no loaded pack's gate table lists (return address %p)", (unsigned long long)site, __builtin_return_address(0));
    atomic_fetch_add_explicit(g->kind == NXAOT_GATE_OBS ? &c_obs_gates : &c_spin_gates, 1, memory_order_relaxed);
    dm_aot_gate(st, dm_aot_pack_base(p) + site);
    if (atomic_load(&t->exiting)) dm_aot_leave(DM_AOT_LEFT_EXIT);
    check_stop(t);
}
