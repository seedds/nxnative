// What the tests that link hle_nvn.c and hle_bigstack.c without the host need of it: weak definitions, so a test that defines its own keeps it
#include <stdint.h>
__attribute__((weak)) uint64_t g_t0_ns;   // process start time (host main.c), read by hle_nvn's per-frame timing
__attribute__((weak)) _Atomic uint64_t dm_present_ns;   // when the last present was made, for DM_HITCH_DUMP (hle_thread.c)
__attribute__((weak)) int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }   // no guest address space (hle_bigstack.c)
__attribute__((weak)) int front_screen_max_fps(void) { return 60; }   // ordinary test display; dedicated pacing tests override this
