// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#include <stdatomic.h>
#include <unistd.h>
void hlecount_flush(void);
static void shutdown_late(void) { LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0); }
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
    plat_after_ms(1500, shutdown_late);
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    _exit(0);
}
// SIGTERM (Apple: a GCD signal source; Linux: a signal handler that wakes a thread; Windows: the console control handler) runs the graceful exit on an ordinary thread
void dm_term_install(void) { plat_on_terminate(dm_host_shutdown); }
