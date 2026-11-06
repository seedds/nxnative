// What the tests that link hle_nvn.c and hle_bigstack.c without the host need of it: weak definitions, so a test that defines its own keeps it
#include <stdint.h>
__attribute__((weak)) uint64_t g_t0_ns;   // process start time (host main.c), read by hle_nvn's per-frame timing
__attribute__((weak)) _Atomic uint64_t dm_present_ns;   // when the last present was made, for DM_HITCH_DUMP (hle_thread.c)
__attribute__((weak)) int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }   // no guest address space (hle_bigstack.c)
int g_shape_w, g_shape_h; void front_picture_shape(int w, int h) { g_shape_w = w; g_shape_h = h; }   // the shape the present reports to the front end (t_present_crop reads it)
__attribute__((weak)) void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }   // no vi layer crop (hle_vi.c), read by hle_nvn's present
