// Host lifecycle: the graceful exit, for SIGTERM (a harness or `kill` ending a run) and oe::ExecuteProgram's hand-over to the next program (hle_oe.c). A player's quit (⌘Q, closing
// the window, exit()) is atexit's, and the iOS app has no termination callback: an iPhone ends the process only through the hand-over or without a word.
// In order: stop the background threads that write state (hlecount, DM_REC, cheats), pause audio, stop the pipeline prewarm and start the archive's write, drain graphics (at most
// EXIT_DRAIN_MS), wait for the archive's write within DM_QUIT_ARCH_MS of the start, the same time a quit gives it, then the continuation (then: execve of the next program) or _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is that order, exit code 0, the pipelines the run compiled kept, and the next program.
#include "hle.h"
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void hlecount_flush(void); void dm_rec_finish(void);
// The drain waits for the NVN calls under way. One that compiles its pipeline can take seconds (a draw waiting in pso_get → arch_capture → MTLCompiler: the stall a harness stops a
// run for with SIGTERM), and before, the exit waited for it and ended at a 1.5 s fallback with nothing written: every SIGTERM run of Ocarina of Time without a window lost 400-500
// pipelines, built again at the next start (PR #2625). The drain runs beside the rest now and is waited for EXIT_DRAIN_MS; the archive's write goes on either way
enum { EXIT_DRAIN_MS = 1500, EXIT_LATE_MS = 500 };
static _Atomic int g_exit_taken;   // the one path that ends the process: the exit itself, or the deadline if the exit is stuck
static void exit_end(void (*then)(void *), void *arg) {
    if (atomic_exchange(&g_exit_taken, 1)) for (;;) pause();   // the other path is ending the process (then may be execve under way: one runs it)
    if (then) then(arg);   // returns only if it failed (prog_exec logs why)
    _exit(0);
}
static int ms_left(uint64_t t0, int budget_ms) { int64_t left = budget_ms - (int64_t)((now_ns() - t0) / 1000000); return left > 0 ? (int)left : 0; }
void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
    // Past every wait below (something else stuck): end the process all the same, and start the next program if the exit has not, so a hand-over does not become a quit
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(DM_QUIT_ARCH_MS + EXIT_LATE_MS) * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("[RED] graceful exit not done in %.1f seconds, exiting at once", (DM_QUIT_ARCH_MS + EXIT_LATE_MS) / 1e3); exit_end(then, arg); });
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_rec_finish();                                // DM_REC: the held state and the buffered steps (hle_hid.c)
    dm_cheats_exit();                               // a clean exit: no safe mode for the cheats at the next launch (atexit does not run either)
    dm_audio_suspend(1);
    dm_gfx_quit_begin();                            // no more prewarm compiles; what the pipeline archive holds starts writing now, beside the drain
    dispatch_semaphore_t drained = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ dm_gfx_suspend(1); dispatch_semaphore_signal(drained); });
    if (dispatch_semaphore_wait(drained, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms_left(t0, EXIT_DRAIN_MS) * NSEC_PER_MSEC)))
        LOG("[RED] graceful exit: graphics not suspended in %.1f seconds (a draw under way, most often one compiling its pipeline): the pipeline archive is written all the same", EXIT_DRAIN_MS / 1e3);
    dm_gfx_quit(ms_left(t0, DM_QUIT_ARCH_MS));      // what the archive has not written yet, within the time a quit gives it
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    exit_end(then, arg);
}
void dm_host_shutdown(const char *why) { dm_host_shutdown_then(why, NULL, NULL); }
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
