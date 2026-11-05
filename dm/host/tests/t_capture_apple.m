// Explicit hardware test: asks microphone permission, counts samples, saves no audio.
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <math.h>
#include "../capture_apple.h"
static _Atomic uint64_t frames, nonzero;
static void samples(void *context, const float *pcm, uint32_t n) {
    (void)context;
    uint32_t nz = 0;
    for (uint32_t i = 0; i < n; i++) if (isfinite(pcm[i]) && pcm[i] != 0) nz++;
    atomic_fetch_add(&nonzero, nz); atomic_fetch_add(&frames, n);
}
static void wait_seconds(double seconds) {
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2 || strcmp(argv[1], "--microphone")) {
            fprintf(stderr, "Run with --microphone to test the default microphone (no recording is saved).\n"); return 2;
        }
        dm_capture *c = dm_capture_open(48000, samples, NULL);
        if (!c || !dm_capture_start(c)) { dm_capture_close(c); return 1; }
        wait_seconds(1);
        dm_capture_suspend(1);
        uint64_t before = atomic_load(&frames);
        wait_seconds(0.1);
        if (atomic_load(&frames) != before || !before) return 1;
        dm_capture_suspend(0);
        wait_seconds(0.5);
        dm_capture_reset();
        wait_seconds(0.5);
        dm_capture_stop(c);
        uint64_t after = atomic_load(&frames);
        wait_seconds(0.1);
        int ok = after > before && atomic_load(&frames) == after;
        dm_capture_close(c);
        printf("capture: %llu frames, %llu nonzero samples; stop/suspend/resume/reset %s\n",
               (unsigned long long)after, (unsigned long long)atomic_load(&nonzero), ok ? "PASS" : "FAIL");
        return !ok;
    }
}
