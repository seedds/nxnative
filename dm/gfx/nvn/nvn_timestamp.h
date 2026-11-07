// NVN counter timestamps are 614.4 MHz ticks (384 ticks per 625 ns), not the CPU's 19.2 MHz system counter.
// Keep the guest clock's epoch in both directions. Wide products avoid overflow on long host uptimes.
// The rate is the profile's gpu_ts_hz (nvn_ts_profile_hz, defined in nvn_mtl.m from game_profile.h; 0 = 614.4 MHz); DM_NVN_TS_HZ=<Hz> overrides it.
// The Legend of Zelda: Ocarina of Time (Switch 2) turns the difference of two raw reports into its wind and particle time step at 31.25 MHz, the
// Switch 2's system counter rate: at 614.4 MHz its foliage, falling leaves and hanging plants moved 19.66 times too fast (an agl pass's time input
// advanced 0.3456 in a 17.88 ms frame, = 17.58 ms of GPU time read at 31.25 MHz)
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define NVN_TS_HZ_DEFAULT 614400000ull
extern uint64_t nvn_ts_profile_hz;   // the profile's gpu_ts_hz (nvn_mtl.m), 0 = the default
static inline uint64_t nvn_ts_hz(void) {
    static uint64_t hz;
    if (!hz) { const char *e = getenv("DM_NVN_TS_HZ"); uint64_t v = e && *e ? strtoull(e, NULL, 10) : 0; hz = v ? v : nvn_ts_profile_hz ? nvn_ts_profile_hz : NVN_TS_HZ_DEFAULT; }
    return hz;
}
static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) {
    uint64_t hz = nvn_ts_hz();
    return hz == NVN_TS_HZ_DEFAULT ? (uint64_t)((__uint128_t)ns * 384 / 625) : (uint64_t)((__uint128_t)ns * hz / 1000000000u);
}
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) {
    uint64_t hz = nvn_ts_hz();
    return hz == NVN_TS_HZ_DEFAULT ? (uint64_t)((__uint128_t)ticks * 625 / 384) : (uint64_t)((__uint128_t)ticks * 1000000000u / hz);
}
