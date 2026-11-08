// A real guest-clock epoch for GPU timestamp tests. The renderer's weak zero
// clock deliberately disables conversion of timestamp-heap entries; using it
// would test wall-clock fallback rather than the reports' GPU boundaries.
#include <stdint.h>
#include <mach/mach_time.h>
uint64_t guest_ns(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (uint64_t)((long double)mach_absolute_time() * tb.numer / tb.denom);
}
