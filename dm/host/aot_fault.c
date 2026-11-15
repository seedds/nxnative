// Faults of the x86-64 execution engine (docs/design/aot-recompiler.md 5.4; aot.h), and the part of crash.c the portable host needs (crash.c itself is Apple's:
// Mach thread states, ARM signal contexts, ESR): the registry of page-fault hooks and host pre-write hooks (dm.h), a fault handler per platform, the
// alternate signal stacks, dm_backtrace, and crash.c's progress watchdog and state dump (DM_WATCH, DM_WATCH_STALL, crash_watch_*, crash_dump_now).
// The order of 5.4: the hooks first (nvn_dirty.c's dirty-page tracker; is_write comes from the page-fault error code; the hook unprotects the page and the
// faulting instruction runs again, which is safe because every guest access is one x86 instruction, 6.4), then, with the JIT built in (NXHOST_JIT),
// dynarmic's fastmem handler (a fault in its code cache falls back to its memory callbacks), then the report. A fault in a pack's code, or in a memory access
// the JIT's callbacks make, is a guest fault: the thread leaves its guest code and the report names the guest instruction the pack's line table gives (3.5),
// or the JIT's access was made for, as a [RED] line with the guest's frames, and the process stops (status DM_AOT_FAULT). Anything else is a host crash, reported
// and ended with exit status 128 + the signal, as crash.c ends one on Apple.
// Linux: sigaction on the thread's alternate stack. Windows: a vectored exception handler, which redirects a guest fault to an ordinary frame that leaves; it
// passes every other exception on, dynarmic's fastmem faults included, whose handler is frame-based (below) (written, not run here: no Windows machine)
#include "aot.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>   // backtrace: a guest fault's native frames
#endif
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

// ---------- Backtraces (aot.h, "The calling guest's context, and backtraces") ----------
// The guest's frame-record chain from fp (x29), read without faulting, appended to line: return addresses as module offsets, as crash.c prints them on Apple. Every
// record lies in the arena (what the guest can address), 8-byte aligned; the chain ends at a caller fp that does not grow, a read that fails, or 32 records. The host-return
// address (the outermost frame of a guest call the host made, aot_run.c guest_vcall) is "host", and the walk goes on to the frames of the guest code around that call only
// when its record's fp leads there (a nested call keeps the x29 of the host call it is nested in); at a thread's outermost call that fp is 0 and the line ends
static void bt_walk(char *line, int *p, size_t cap, uint64_t fp) {
    for (int i = 0; i < 32 && fp && !(fp & 7) && dm_aot_in_arena(fp, 16) && *p < (int)cap - 64; i++) {
        uint64_t w[2]; size_t got = 0;
        if (plat_mem_read(fp, w, sizeof w, &got) || got != sizeof w) break;
        const int more = w[0] > fp;
        char nm[64];
        if (w[1] == DM_AOT_HOST_RETURN) {
            if (!more || (w[0] & 7) || !dm_aot_in_arena(w[0], 16)) break;
            snprintf(nm, sizeof nm, "host");
        } else dm_aot_guest_name(w[1], nm, sizeof nm);
        *p += snprintf(line + *p, cap - (size_t)*p, " %s", nm);
        if (!more) break;
        fp = w[0];
    }
}
void dm_backtrace(uint64_t fp) {
    char line[1024];
    int p = snprintf(line, sizeof line, "[dm] bt:");
    bt_walk(line, &p, sizeof line, fp);
    fprintf(stderr, "%s\n", line);
}

// The calling guest's context: the block during a host call or an SVC, whose guest address aot_run.c keeps in prof_at (host_call, stub_call: x30; aot_svc: the svc
// instruction; 0 in guest code and the run time, DM_AOT_PROF_JIT in the JIT fallback; a nested guest call sets 0 and puts it back, with x19..x30, when it returns)
uint64_t dm_aot_guest_lr(void) {
    const dm_aot_thread *t = dm_aot_self();
    const uint64_t at = t && t->depth ? t->prof_at : 0;
    return at > DM_AOT_PROF_JIT ? at : 0;
}
void dm_aot_guest_backtrace(void) {
    const uint64_t lr = dm_aot_guest_lr();
    if (!lr) { fprintf(stderr, "[dm] bt: (no guest call)\n"); return; }
    char line[1024], nm[64];
    if (lr == DM_AOT_HOST_RETURN) snprintf(nm, sizeof nm, "host");   // a host function that is a guest thread's entry (hle_thread.c)
    else dm_aot_guest_name(lr, nm, sizeof nm);
    int p = snprintf(line, sizeof line, "[dm] bt: %s", nm);
    bt_walk(line, &p, sizeof line, dm_aot_self()->st.x[29]);
    fprintf(stderr, "%s\n", line);
}

// A guest instruction word at guest address a is a call (bl, blr): where code inlined at a call site, or a frame's return, maps to
static int guest_is_call(uint64_t a) {
    uint32_t w; size_t got = 0;
    if (plat_mem_read(a, &w, sizeof w, &got) || got != sizeof w) return 0;
    return (w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u;
}
// The guest frames the JIT fallback had made when its driver called compiled code (aot.h dm_aot_jitcall), appended to line: its x30 at the call (the return
// into the JIT's code; not when it is the level's lr, a tail call into the compiled entry), then the x29 chain from its x29 there, down to the record whose return
// address is the level's lr or a record at or above the SP the level began with (a frame of the code further out), then, for a level compiled code entered, that lr:
// the return into the frame further out, whose own native frame a compiled code's walk meets next. dup: an address not to print first (0 for none)
static void jit_frames(char *line, int *p, size_t cap, const dm_aot_jitcall *c, uint64_t dup) {
    char nm[64];
    if (c->x30 != c->lr && c->x30 != dup) {
        dm_aot_guest_name(c->x30, nm, sizeof nm);
        *p += snprintf(line + *p, cap - (size_t)*p, " %s", nm);
    }
    uint64_t fp = c->x29;
    for (int i = 0; i < 32 && fp && !(fp & 7) && fp < c->sp && dm_aot_in_arena(fp, 16) && *p < (int)cap - 64; i++) {
        uint64_t w[2]; size_t got = 0;
        if (plat_mem_read(fp, w, sizeof w, &got) || got != sizeof w) break;
        if (w[1] == c->lr || w[1] == DM_AOT_HOST_RETURN) break;
        dm_aot_guest_name(w[1], nm, sizeof nm);
        *p += snprintf(line + *p, cap - (size_t)*p, " %s", nm);
        if (w[0] <= fp) break;
        fp = w[0];
    }
    if (c->entered && c->lr != DM_AOT_HOST_RETURN) {
        dm_aot_guest_name(c->lr, nm, sizeof nm);
        *p += snprintf(line + *p, cap - (size_t)*p, " %s", nm);
    }
}
// A guest fault's backtrace (aot_run.c fault_report, after its [RED] line), the frames above the faulting instruction:
//   compiled code with its native frames taken (Linux): compiled code keeps x29 in a host register between the calls it makes (2.3: the block holds it only at a
//     call or an escape), so the x29 chain from the block may start a frame or more too far out; the native frames hold every compiled frame instead (3.5). Each
//     native return address in a pack's function is its call's guest return address (the call instruction's position, the outermost one in inlined code, + 4), and
//     the faulting instruction's own position, when it is a call other than the instruction (code inlined there), comes first. Where the JIT fallback's driver
//     called the compiled code (its native_call frame, met by its return address), the guest frames the JIT had made come next (jit_frames), and the frames of
//     the driver up to the compiled code that handed over to the JIT are no host gap; the function that handed the rest of itself over (no call at its position:
//     msr fpcr, an encoding the lifter leaves, a branch to no entry) is the JIT's outermost frame, already named. "host" marks host frames between compiled ones
//     (a host function a nested guest call came from, a guest call's outermost JIT level)
//   the JIT fallback: the walk from the guest's own x29 at the faulting access, which fault_report took from the JIT instance into the block (dynarmic keeps the
//     guest's registers in its own state during a run, current at every memory callback: dm/tools/aot/src/rt/jit.h dm_jit_fault_state), as Apple's crash.c walks
//     from the fp of the faulting thread. Were they not there (fault_jit_regs 0), the block would hold the x29 of the run's start or its last SVC: "(JIT from
//     <pc>)" then names that guest pc, and the frames the JIT made since are not shown
//   no native frames (Windows: a pack has no unwind tables there, design 3.5; an unwinder that found none): the walk from the last x29 written back
void dm_aot_fault_backtrace(const dm_aot_thread *t) {
    char line[1024], nm[64];
    int p = snprintf(line, sizeof line, "[dm] bt:");
    int i = 0, n = t->fault_nbt;
    while (i < n && t->fault_bt[i] != t->fault_pc) i++;   // the faulting frame (the unwinder's frames before it are the handler's)
    if (t->fault_jit_pc && t->fault_jit_regs) bt_walk(line, &p, sizeof line, t->st.x[29]);
    else if (t->fault_jit_pc) {
        dm_aot_guest_name(t->st.pc, nm, sizeof nm);
        p += snprintf(line + p, sizeof line - (size_t)p, " (JIT from %s)", nm);
        bt_walk(line, &p, sizeof line, t->st.x[29]);
    } else if (i < n) {
        int pk = -1;
        long fn = dm_aot_fn_of_code(t->fault_pc, &pk);
        const dm_aot_pack *pp = dm_aot_pack_at(pk);
        const int64_t at = fn >= 0 && pp ? dm_aot_offset_of_code(t->fault_pc, pk, fn) : -1;
        if (at >= 0 && dm_aot_pack_base(pp) + (uint64_t)at != dm_aot_line_of_code(t->fault_pc) && guest_is_call(dm_aot_pack_base(pp) + (uint64_t)at)) {
            dm_aot_guest_name(dm_aot_pack_base(pp) + (uint64_t)at + 4, nm, sizeof nm);
            p += snprintf(line + p, sizeof line - (size_t)p, " %s", nm);
        }
        // after: 1 once the JIT's frames of a level compiled code entered are named (they end with its lr), until the compiled frame that entered it; 2 after that
        // frame, when it was the function that handed the rest of itself over, until the next frame, which returns to dup (the lr already named) when it is its caller
        int any = 0, gap = 0, after = 0, jn = 0;
        uint64_t dup = 0;
        for (i++; i < n && p < (int)sizeof line - 64; i++) {
            if (jn < t->fault_njit && t->fault_bt[i] == t->fault_jit[jn].ra) {
                // The driver's call of the compiled frame below it (the frames between are that call's: the pack's enter, native_call), so no host gap here
                const dm_aot_jitcall *c = &t->fault_jit[jn++];
                jit_frames(line, &p, sizeof line, c, after == 2 && !gap ? dup : 0);
                after = c->entered; dup = c->lr;
                any = 1; gap = 0;
                continue;
            }
            const uint64_t ra = t->fault_bt[i] - 1;   // inside the call instruction
            if ((fn = dm_aot_fn_of_code(ra, &pk)) < 0 || !(pp = dm_aot_pack_at(pk))) { if (after != 1) gap = any; continue; }   // after 1: the driver's own frames
            const int64_t off = dm_aot_offset_of_code(ra, pk, fn);
            const uint64_t base = dm_aot_pack_base(pp);
            const int call = off >= 0 && guest_is_call(base + (uint64_t)off);
            const uint64_t at = call ? base + (uint64_t)off + 4 : 0;   // the guest return address this frame names (0: none, its function is named)
            if (after == 1) {
                // The frame that entered the level: a call of the JIT's code returning to lr (named), or the function that handed the rest of itself over at an
                // instruction that is no call (msr fpcr, an encoding the lifter leaves, a br or b to no entry), which the JIT's frames named as theirs
                after = at == dup ? 0 : 2;
                continue;
            }
            if (after == 2) { after = 0; if (at == dup && !gap) continue; }   // its caller, whose call the level's lr named
            if (call) dm_aot_guest_name(at, nm, sizeof nm);
            else { dm_aot_guest_name(base + dm_aot_pack_fn(pp, (size_t)fn), nm, sizeof nm); snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "(fn)"); }   // no position: the function
            p += snprintf(line + p, sizeof line - (size_t)p, "%s %s", gap ? " host" : "", nm);
            any = 1; gap = 0;
        }
    } else bt_walk(line, &p, sizeof line, t->st.x[29]);
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
    // The guest frames around the host code: inside a host call or an SVC its call site and the guest's frames (the block is whole there), else from the last x29 written back
    if (dm_aot_guest_lr()) dm_aot_guest_backtrace();
    else if (t && t->depth) dm_backtrace(t->st.x[29]);
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
        // A guest fault: the facts only, the report is made after the jump (aot_run.c fault_report). In compiled code its native frames too, which the jump discards:
        // the unwinder goes through this handler's frame and the signal frame (glibc's __restore_rt has unwind tables) into the pack's code (async unwind tables, 3.5)
        t->fault_nbt = 0; t->fault_njit = 0;
#if defined(__GLIBC__)
        if (!jit_pc) {
            void *bt[sizeof t->fault_bt / sizeof *t->fault_bt];
            const int n = backtrace(bt, (int)(sizeof bt / sizeof *bt));
            for (int i = 0; i < n; i++) t->fault_bt[i] = (uint64_t)(uintptr_t)bt[i];
            t->fault_nbt = n > 0 ? n : 0;
            // The JIT driver's calls into compiled code in progress (aot.h dm_aot_jitcall), copied: they live in the native frames the jump discards
            for (const dm_aot_jitcall *c = t->jit_calls; c && t->fault_njit < DM_AOT_FAULT_JITCALLS; c = c->outer) t->fault_jit[t->fault_njit++] = *c;
        }
#endif
        t->fault_signal = sig; t->fault_address = (uint64_t)(uintptr_t)si->si_addr; t->fault_pc = pc; t->fault_jit_pc = jit_pc;
        t->left = DM_AOT_LEFT_FAULT;
        t_in_next = 0;
        dm_aot_longjmp(t->base);
    }
    host_crash(sig, si->si_addr, pc);
}

static void install_once(void) {
#if defined(__GLIBC__)
    { void *b[2]; (void)backtrace(b, 2); }   // glibc loads its unwinder (libgcc_s) at the first call: here, not in the fault handler
#endif
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

static void alt_own(void);
// dynarmic installs its handler with its first instance, keeping the one it replaced (this one) for faults outside its code cache: install this one again
// in front of it, so that the hooks see a write to a tracked page first even when JIT code makes it. Called once, on the thread that made that instance
// (aot_run.c), whose alternate signal stack dynarmic's SigHandler replaced with a 2 MiB block of its own (third_party/dynarmic exception_handler_posix.cpp
// SigHandler::SigHandler), which its static destructor frees at exit without taking it off the thread: a signal after that (the sampler's SIGPROF, while
// exit's handlers run on that thread) was delivered onto unmapped memory, and the kernel killed the process with SIGSEGV. The thread gets its own stack
// back (alt_own), so the host never runs a handler on a stack it does not own; every other thread never had dynarmic's (its constructor runs once).
// Called after every instance made (aot_run.c: the JIT's made hook, on the thread that made it, which may be the helper thread that makes them ahead):
// the handlers are taken over only while dynarmic's is in front (once), the thread's own stack installed again each time (cheap: one call)
void dm_aot_fault_after_jit(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    struct sigaction cur;
    if (!sigaction(SIGSEGV, NULL, &cur) && !((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_fault)) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_next_segv);
        sigaction(SIGBUS, &sa, &g_next_bus);
        if ((g_next_segv.sa_flags & SA_SIGINFO) && g_next_segv.sa_sigaction != on_fault) atomic_store(&g_have_next, 1);
    }
    pthread_mutex_unlock(&lock);
    alt_own();
}

// The alternate signal stack is per thread (a native stack overflow from deep guest recursion still reaches the handler); freed when the thread exits
#define ALT_STACK_SIZE 65536
static pthread_key_t g_alt_key;
static pthread_once_t g_alt_once = PTHREAD_ONCE_INIT;
static void alt_free(void *p) { stack_t off; memset(&off, 0, sizeof off); off.ss_flags = SS_DISABLE; sigaltstack(&off, NULL); free(p); }
static void alt_key_init(void) { if (pthread_key_create(&g_alt_key, alt_free)) die("aot: alternate stack key"); }
void crash_thread_init(void) {
    pthread_once(&g_alt_once, alt_key_init);
    if (pthread_getspecific(g_alt_key)) return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_size = ALT_STACK_SIZE;
    if (!(ss.ss_sp = malloc(ss.ss_size))) die("aot: alternate stack");
    sigaltstack(&ss, NULL);
    pthread_setspecific(g_alt_key, ss.ss_sp);
}
// The calling thread's own alternate stack installed again (made first when it has none), in place of one something else installed (dynarmic's: above)
static void alt_own(void) {
    pthread_once(&g_alt_once, alt_key_init);
    void *sp = pthread_getspecific(g_alt_key);
    if (!sp) { crash_thread_init(); return; }
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = sp;
    ss.ss_size = ALT_STACK_SIZE;
    if (sigaltstack(&ss, NULL)) LOG("[RED] aot: the thread's own alternate signal stack could not be installed again after the JIT's (errno %d)", errno);
}

#else
// ---------- Windows ----------
// The order of 5.4 on Windows, with the JIT fallback (NXHOST_JIT). The system's dispatcher (RtlDispatchException) calls every vectored handler before any frame-based
// one. This vectored handler, registered first (AddVectoredExceptionHandler(1, ...)): the hooks, then a guest fault (a pack's code, or an access a JIT callback makes:
// dm_jit_access_pc) leaves through fault_leave; anything else goes on. dynarmic's handler is frame-based: each JIT instance registers a function table over its code
// cache with an exception handler of its own (RtlAddFunctionTable; third_party/dynarmic backend/x64/exception_handler_windows.cpp), which the dispatcher reaches only
// when this handler passed the fault on and the faulting rip lies in that cache: a fastmem access the arena does not back, which it sends to the JIT's memory
// callbacks (jit.cpp halts there and steps the instruction again with the access made by a callback: a fault then is this handler's, at dm_jit_access_pc, and
// fault_report, after fault_leave's jump, takes the guest's registers there from the instance as on Linux: jit.h dm_jit_fault_state). A write to
// a page the dirty tracker protects reaches the hooks here first, from JIT code too, as on Linux. The frame-based search walks the thread's own stack within its TEB
// limits, which the JIT's frames are on (guest threads run on their host thread's native stack, aot_thread.c). Written from the Windows documentation, not run here
// (no Windows machine): dm/host/tests/run_win64_tests.ps1's AOT tests are the check (t_aot_host's corpus programs that need the JIT, t_aot_module's JIT-forced runs)
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
    uint64_t jit_pc = 0;
#if NXHOST_JIT
    // Nonzero only while a JIT callback makes a guest access on this thread (jit.cpp Access .. the access): a fastmem fault in dynarmic's code cache comes with 0
    // and goes on to dynarmic's frame-based handler below (unrun on Windows)
    jit_pc = dm_jit_access_pc();
#endif
    dm_aot_thread *t = dm_aot_self();
    if (sig && t && t->depth && (dm_aot_pack_of_code(pc) || jit_pc)) {
        t->fault_signal = sig; t->fault_pc = pc; t->fault_jit_pc = jit_pc;
        t->fault_address = code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2 ? (uint64_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
        DWORD64 sp = (e->ContextRecord->Rsp - 512) & ~(DWORD64)15;
        e->ContextRecord->Rsp = sp - 8;   // as after a call: fault_leave's frame aligned as the ABI expects
        e->ContextRecord->Rip = (DWORD64)(uintptr_t)fault_leave;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;   // dynarmic's frame-based handler for its code cache, a system DLL's own handler, or the process's unhandled-exception path
}
static void install_once(void) { AddVectoredExceptionHandler(1, on_exception); watch_install(); }
void crash_install(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, install_once);
    crash_thread_init();
}
// Nothing to put back on Windows (unrun there): dynarmic registers no vectored handler and changes nothing of the thread's (no alternate stack), so this handler
// stays in front of its frame-based one by the dispatcher's order (above). Taking this handler off and adding it again, as the Linux branch reinstalls its
// sigaction, would leave a moment in which a fault on another thread (a write to a tracked page) found no handler at all
void dm_aot_fault_after_jit(void) {}
void crash_thread_init(void) {
    ULONG guarantee = 65536;   // stack kept for the handler when a guest recursion overflows the thread's stack
    SetThreadStackGuarantee(&guarantee);
}
#endif
