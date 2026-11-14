// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, start the pipeline archive's write, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), wait for the archive's write, then _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, keeping the pipelines the run
// compiled, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
void hlecount_flush(void); void dm_rec_finish(void);
// The time the exit gives the pipeline archive beyond the 1.5 s for the rest (DM_EXIT_ARCH_MS). Writing takes 1.8-4 s for 900 bases on Macs; with 1.3 s in all, every warm-up run of
// Ocarina of Time (2026-11-10) ended with 400-500 bases not written, built again at the next start. iOS ends an app about 5 s after its termination callback: 3 s there. A write the
// deadline cuts short leaves only its .tmp, which the next start removes (nvn_mtl.m pso4_init)
enum { EXIT_BASE_MS = 1500,
#if TARGET_OS_IPHONE
       EXIT_ARCH_MS = 3000
#else
       EXIT_ARCH_MS = 8000
#endif
};
static int exit_arch_ms(void) { const char *e = getenv("DM_EXIT_ARCH_MS"); int v = e && *e ? atoi(e) : EXIT_ARCH_MS; return v < 0 ? 0 : v; }
static int ms_left(uint64_t t0, int budget_ms) { int64_t left = budget_ms - (int64_t)((now_ns() - t0) / 1000000); return left > 0 ? (int)left : 0; }
void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    static _Atomic int claimed;   // the one path that runs dm_gfx_quit, then and _exit: the main path after the suspend, or the 1.5 s fallback if graphics are not suspended by then
    uint64_t t0 = now_ns(); int arch_ms = exit_arch_ms(); LOG("received %s: graceful exit", why);
    // Graphics not suspended in 1.5 s: a draw is still under way, most often one compiling its pipeline (the stall the harness's watcher stops a run for). The archive has been
    // writing since the start (dm_gfx_quit_begin): what it holds is written still, within the same deadline, and then the process ends
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, EXIT_BASE_MS * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        if (atomic_exchange(&claimed, 1)) return;
        LOG("[RED] graceful exit: graphics not suspended in 1.5 seconds, exiting after the pipeline archive's write (up to %d ms)", ms_left(t0, EXIT_BASE_MS + arch_ms));
        dm_gfx_quit(ms_left(t0, EXIT_BASE_MS + arch_ms)); if (then) then(arg); _exit(0); });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(EXIT_BASE_MS + arch_ms + 500) * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        int mine = !atomic_exchange(&claimed, 1);   // then runs once: here only if neither path above got to it
        LOG("[RED] graceful exit not done in %.1f seconds, exiting at once", (EXIT_BASE_MS + arch_ms + 500) / 1e3); if (mine && then) then(arg); _exit(0); });
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_rec_finish();                                // DM_REC: the held state and the buffered steps (hle_hid.c)
    dm_cheats_exit();                               // a clean exit: no safe mode for the cheats at the next launch (atexit does not run either)
    dm_audio_suspend(1);
    dm_gfx_quit_begin();                            // the pipeline archive starts writing what it holds now, not after the drain
    dm_gfx_suspend(1);
    if (atomic_exchange(&claimed, 1)) for (;;) pause();   // the suspend returned after the fallback took over: it writes the archive and ends the process
    dm_gfx_quit(ms_left(t0, EXIT_BASE_MS - 200 + arch_ms));   // what the archive has not written yet (captures since), within the budget
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    if (then) then(arg);
    _exit(0);
}
void dm_host_shutdown(const char *why) { dm_host_shutdown_then(why, NULL, NULL); }
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
