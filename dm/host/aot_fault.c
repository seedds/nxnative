// Faults of the x86-64 execution engine (docs/design/aot-recompiler.md 5.4; aot.h), and the part of crash.c the portable host needs (crash.c itself is Apple's:
// Mach thread states, ARM signal contexts, ESR): the registry of page-fault hooks and host pre-write hooks (dm.h), a fault handler per platform, the
// alternate signal stacks, dm_backtrace, and crash.c's progress watchdog and state dump (DM_WATCH, DM_WATCH_STALL, crash_watch_*, crash_dump_now).
// The order of 5.4: the hooks first (nvn_dirty.c's dirty-page tracker; is_write comes from the page-fault error code; the hook unprotects the page and the
// faulting instruction runs again, which is safe because every guest access is one x86 instruction, 6.4), then, with the JIT built in (NXHOST_JIT),
// dynarmic's fastmem handler (a fault in its code cache falls back to its memory callbacks), then the report. A fault in a pack's code, or in a memory access
// the JIT's callbacks make, is a guest fault: the thread leaves its guest code and the report names the guest instruction the pack's line table gives (3.5),
// as a [RED] line, and the process stops (status DM_AOT_FAULT). Anything else is a host crash, reported and ended with exit status 128 + the signal, as crash.c
// ends one on Apple.
// Linux: sigaction on the thread's alternate stack. Windows: a vectored exception handler, which redirects a guest fault to an ordinary frame that leaves; it
// passes every other exception on (written, not run here: neither a Windows machine nor a Windows pack exists yet)
#include "aot.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#if NXHOST_JIT
#include "rt/jit.h"   // dm_jit_access_pc: the guest access a memory callback of the JIT is making
#endif

// ---------- The progress watchdog and the state dump (dm.h; crash.c's on Apple, the same rules) ----------
// DM_WATCH=<s> ends the process after that many seconds (crash.c: alarm and SIGALRM, its report, status 128 + 14) and turns the stall watchdog on; DM_WATCH_STALL=<s> alone turns the
// stall watchdog on (15 s with DM_WATCH): no progress (crash_watch_progress, or a presented frame before the game's run) for that long logs a [RED] line, dumps the guest threads
// and the graphics wait points and exits with 128 + 14; a stall of a second or more after the game's run began dumps them (at most 3 times, 10 s apart). Neither is on unless set.
// The timer is plat_after_ms (Windows has no SIGALRM); DM_WATCH_SAMPLE is the Mac's (/usr/bin/sample) and not read here
static _Atomic uint64_t g_watch_last_ns, g_watch_count;
static _Atomic uint32_t g_watch_tick;
static _Atomic int g_watch_pregame = 1, g_watch_stage_idx;
static char g_watch_stage[2][256] = {"startup", "startup"};
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);   // the guest threads' state (hle_thread.c svc_thread_dump, registered by the host program)
static void (*g_wait_dump)(void);   // the graphics side's wait points
enum { WATCH_SIGNAL = 14 };         // SIGALRM's number, the status crash.c ends a watchdog run with (128 + 14)
uint64_t crash_watch_progress_count(void) { return atomic_load(&g_watch_count); }
void crash_watch_progress(uint32_t tick, const char *stage) {
    int next = 1 - atomic_load(&g_watch_stage_idx);
    if (stage && *stage) snprintf(g_watch_stage[next], sizeof g_watch_stage[next], "%s", stage);
    atomic_store(&g_watch_tick, tick);
    atomic_store(&g_watch_stage_idx, next);
    atomic_fetch_add(&g_watch_count, 1);
    atomic_store(&g_watch_last_ns, plat_now_ns());
}
void crash_watch_frame(void) { if (atomic_load(&g_watch_pregame)) crash_watch_progress(0, "present frame"); }   // hle_os.c: every present
void crash_watch_pregame_done(void) { atomic_store(&g_watch_pregame, 0); }
void crash_watch_pregame_again(void) { atomic_store(&g_watch_pregame, 1); }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
void crash_dump_now(const char *why) {
    fprintf(stderr, "[dm] state dump (%s): tick %u / stage %s\n", why, atomic_load(&g_watch_tick), g_watch_stage[atomic_load(&g_watch_stage_idx)]);
    if (g_dump_hook) g_dump_hook();
    if (g_wait_dump) g_wait_dump();
}
static void watch_alarm(void) {   // DM_WATCH's time is up
    fprintf(stderr, "[dm] CRASH sig=%d (DM_WATCH: the run's time limit)\n", WATCH_SIGNAL);
    if (g_dump_hook) g_dump_hook();
    fflush(stderr);
    _exit(128 + WATCH_SIGNAL);
}
static void *watchdog_main(void *arg) {
    (void)arg;
    plat_thread_set_name("watchdog");
    for (;;) {
        plat_sleep_ns(250000000ull);
        uint64_t last = atomic_load(&g_watch_last_ns), now = plat_now_ns();   // last first: then last <= now (crash.c has the reason)
        if (last > now) continue;
        { static int dumps; static uint64_t last_dump, dumped_for;
          if (g_watch_stall && !atomic_load(&g_watch_pregame) && atomic_load(&g_watch_tick) > 0 && now - last >= 1000000000ull && now - last < (uint64_t)g_watch_stall * 1000000000ull
              && dumps < 3 && dumped_for != last && (!last_dump || now - last_dump >= 10000000000ull)) { dumps++; last_dump = now; dumped_for = last; crash_dump_now("progress stalled for over 1 second"); } }
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            int idx = atomic_load(&g_watch_stage_idx);
            LOG("[RED] watchdog: no progress since second %llu, stuck at tick %u / stage %s", (unsigned long long)((now - last) / 1000000000ull), atomic_load(&g_watch_tick), g_watch_stage[idx]);
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();
            fflush(stderr);
            _exit(128 + WATCH_SIGNAL);
        }
    }
    return NULL;
}
static void watch_install(void) {
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch && atoi(watch) > 0) plat_after_ms((unsigned)atoi(watch) * 1000u, watch_alarm);
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, plat_now_ns());
        pthread_t th;
        if (pthread_create(&th, NULL, watchdog_main, NULL)) die("watchdog thread");
        pthread_detach(th);
    }
}

// ---------- The hooks (dm.h) ----------
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static _Atomic int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    int n = atomic_load(&g_nfault_hook);
    if (n >= MAX_FAULT_HOOK) die("too many page fault hooks");
    g_fault_hook[n] = fn;
    atomic_store(&g_nfault_hook, n + 1);   // published after the slot: a fault on another thread sees a complete entry
}
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    int n = atomic_load(&g_nwrite_prep);
    if (n >= MAX_FAULT_HOOK) die("too many host pre-write hooks");
    g_write_prep[n] = fn;
    atomic_store(&g_nwrite_prep, n + 1);
}
// Before the host writes guest memory itself (a read() into a guest buffer: a system call meets a write-protected page with EFAULT, not with a fault)
void dm_guest_write_prepare(void *addr, size_t len) {
    int n = atomic_load(&g_nwrite_prep);
    for (int i = 0; i < n; i++) g_write_prep[i](addr, len);
}
static int run_hooks(void *addr, int is_write, void *uc) {
    int n = atomic_load(&g_nfault_hook);
    for (int i = 0; i < n; i++) if (g_fault_hook[i](addr, is_write, uc)) return 1;
    return 0;
}

// The guest's frame-record chain from fp (x29), read without faulting: return addresses as module offsets, as crash.c prints it on Apple
void dm_backtrace(uint64_t fp) {
    char line[1024];
    int p = snprintf(line, sizeof line, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7) && p < (int)sizeof line - 64; i++) {
        uint64_t w[2]; size_t got = 0;
        if (plat_mem_read(fp, w, sizeof w, &got) || got != sizeof w) break;
        char nm[64];
        dm_aot_guest_name(w[1], nm, sizeof nm);
        p += snprintf(line + p, sizeof line - (size_t)p, " %s", nm);
        if (w[0] <= fp) break;
        fp = w[0];
    }
    fprintf(stderr, "%s\n", line);
}

#if !defined(_WIN32)
// ---------- POSIX ----------
static struct sigaction g_next_segv, g_next_bus;   // dynarmic's handler, called after the hooks (NXHOST_JIT)
static _Atomic int g_have_next;
static _Thread_local int t_in_next;                // inside dynarmic's handler: it hands faults outside its code cache back to the handler it replaced

static _Noreturn void host_crash(int sig, void *addr, uint64_t pc) {
    static _Atomic int owner;
    if (atomic_exchange(&owner, 1)) for (;;) pause();   // the first crashing thread reports; the others wait for the exit
    char where[256];
    if (!dm_aot_where(pc, where, sizeof where)) {
        const char *sym = plat_addr_symbol((const void *)(uintptr_t)pc, NULL);
        snprintf(where, sizeof where, "host pc %#llx%s%s", (unsigned long long)pc, sym ? " in " : "", sym ? sym : "");
    }
    fprintf(stderr, "[dm] CRASH sig=%d addr=%p (%s)\n", sig, addr, where);
    dm_aot_thread *t = dm_aot_self();
    if (t && t->depth) dm_backtrace(t->st.x[29]);   // the guest frames around the host code, from the last x29 written back
    if (g_dump_hook) g_dump_hook();                 // every guest thread's state, as crash.c's report ends on Apple
    fflush(stderr);
    _exit(128 + sig);
}

static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    const uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    if (sig == SIGSEGV || sig == SIGBUS) {
        const int is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;   // the page-fault error code's W/R bit
        if (run_hooks(si->si_addr, is_write, uc)) return;                 // handled: the instruction runs again
        if (atomic_load(&g_have_next) && !t_in_next) {
            struct sigaction *n = sig == SIGSEGV ? &g_next_segv : &g_next_bus;
            if (n->sa_flags & SA_SIGINFO) {
                t_in_next = 1;
                n->sa_sigaction(sig, si, ucv);   // returns when it redirected a fault of its code cache; else it called this handler again (t_in_next set)
                t_in_next = 0;
                return;
            }
        }
    }
    uint64_t jit_pc = 0;
#if NXHOST_JIT
    jit_pc = dm_jit_access_pc();
#endif
    dm_aot_thread *t = dm_aot_self();
    if (t && t->depth && (dm_aot_pack_of_code(pc) || jit_pc)) {
        // A guest fault: the facts only, the report is made after the jump (aot_run.c fault_report)
        t->fault_signal = sig; t->fault_address = (uint64_t)(uintptr_t)si->si_addr; t->fault_pc = pc; t->fault_jit_pc = jit_pc;
        t->left = DM_AOT_LEFT_FAULT;
        t_in_next = 0;
        dm_aot_longjmp(t->base);
    }
    host_crash(sig, si->si_addr, pc);
}

static void install_once(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
    watch_install();
}
void crash_install(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, install_once);
    crash_thread_init();
}

// dynarmic installs its handler with its first instance, keeping the one it replaced (this one) for faults outside its code cache: install this one again
// in front of it, so that the hooks see a write to a tracked page first even when JIT code makes it
void dm_aot_fault_after_jit(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_next_segv);
    sigaction(SIGBUS, &sa, &g_next_bus);
    if ((g_next_segv.sa_flags & SA_SIGINFO) && g_next_segv.sa_sigaction != on_fault) atomic_store(&g_have_next, 1);
}

// The alternate signal stack is per thread (a native stack overflow from deep guest recursion still reaches the handler); freed when the thread exits
static pthread_key_t g_alt_key;
static pthread_once_t g_alt_once = PTHREAD_ONCE_INIT;
static void alt_free(void *p) { stack_t off; memset(&off, 0, sizeof off); off.ss_flags = SS_DISABLE; sigaltstack(&off, NULL); free(p); }
static void alt_key_init(void) { if (pthread_key_create(&g_alt_key, alt_free)) die("aot: alternate stack key"); }
void crash_thread_init(void) {
    pthread_once(&g_alt_once, alt_key_init);
    if (pthread_getspecific(g_alt_key)) return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_size = 65536;
    if (!(ss.ss_sp = malloc(ss.ss_size))) die("aot: alternate stack");
    sigaltstack(&ss, NULL);
    pthread_setspecific(g_alt_key, ss.ss_sp);
}

#else
// ---------- Windows ----------
static PVOID g_veh;
static void fault_leave(void) {   // the context of a guest fault resumes here, an ordinary frame on the thread's stack
    dm_aot_thread *t = dm_aot_self();
    t->left = DM_AOT_LEFT_FAULT;
    dm_aot_longjmp(t->base);
}
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    const uint64_t pc = (uint64_t)e->ContextRecord->Rip;
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        void *addr = (void *)(uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
        const int is_write = e->ExceptionRecord->ExceptionInformation[0] == 1;
        if (run_hooks(addr, is_write, e->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    const int sig = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_STACK_OVERFLOW ? 11
                  : code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION ? 4 : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? 8 : 0;
    dm_aot_thread *t = dm_aot_self();
    if (sig && t && t->depth && dm_aot_pack_of_code(pc)) {
        t->fault_signal = sig; t->fault_pc = pc; t->fault_jit_pc = 0;
        t->fault_address = code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2 ? (uint64_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
        DWORD64 sp = (e->ContextRecord->Rsp - 512) & ~(DWORD64)15;
        e->ContextRecord->Rsp = sp - 8;   // as after a call: fault_leave's frame aligned as the ABI expects
        e->ContextRecord->Rip = (DWORD64)(uintptr_t)fault_leave;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;   // dynarmic's handler for its code cache, a system DLL's own handler, or the process's unhandled-exception path
}
static void install_once(void) { g_veh = AddVectoredExceptionHandler(1, on_exception); watch_install(); }
void crash_install(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, install_once);
    crash_thread_init();
}
void dm_aot_fault_after_jit(void) {   // vectored handlers registered first run first: this one again in front of dynarmic's
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = AddVectoredExceptionHandler(1, on_exception);
}
void crash_thread_init(void) {
    ULONG guarantee = 65536;   // stack kept for the handler when a guest recursion overflows the thread's stack
    SetThreadStackGuarantee(&guarantee);
}
#endif
