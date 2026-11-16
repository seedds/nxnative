// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#include <stdatomic.h>
#include <unistd.h>
void hlecount_flush(void);
static void shutdown_late(void) { LOG("[RED] graceful exit not done in 1.5 seconds after the files were written, exiting at once"); _exit(0); }
static void files_late(void) { LOG("[RED] graceful exit: the session's files not written in 20 seconds, exiting at once"); _exit(0); }
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
    // The session's files first, under a bound of their own: each is written to a temporary name and renamed over the old one (aot_profile.c), and the 1.5 s bound of the
    // suspend below, armed before them, cut a long write between the two (a sample profile, the misses, the targets and three modules' bindings under load took longer),
    // leaving <module>.bind.tsv.tmp beside the profile and the file not updated
    plat_after_ms(20000, files_late);
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
#if !DM_NATIVE_EXEC
    { int dm_aot_profile_write(void); dm_aot_profile_write(); }   // DM_AOT_PROFILE (aot_profile.c): the sample profile too, for the same reason; nothing without the switch
    { int dm_aot_targets_write(void); dm_aot_targets_write(); }   // and beside it the inline caches' targets (aot_targets.c, design 12's hot indirect targets)
#endif
    plat_after_ms(1500, shutdown_late);   // the suspend's bound: audio and graphics stop in order, or the process ends at once
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    _exit(0);
}
// SIGTERM (Apple: a GCD signal source; Linux: a signal handler that wakes a thread; Windows: the console control handler) runs the graceful exit on an ordinary thread
void dm_term_install(void) { plat_on_terminate(dm_host_shutdown); }
