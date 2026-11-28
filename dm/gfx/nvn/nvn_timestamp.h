// NVN counter timestamps, the raw value a counter report records: on the Switch (NVN, the X1) 614.4 MHz ticks (384 ticks per 625 ns), not the CPU's 19.2 MHz system counter;
// on the Switch 2 (NVN2) nanoseconds. nn::gfx shows both: its GetDuration subtracts two raw timestamps and converts the difference, by 625 / 384 in its NVN backend (ApiVersion 8,
// Mario Golf: Super Rush main+0xaad0, tail to main+0x7210) and not at all in its NVN2 backend (ApiVersion 200, Welcome Tour main+0x613610, tail to a bare ret).
// nvn_timestamp_ns is set with the device (nvnDeviceInitialize: dm_sdk_ounce). Keep the guest clock's epoch in both directions. Wide products avoid overflow on long host uptimes.
#pragma once
#include <stdint.h>

__attribute__((weak)) int nvn_timestamp_ns;   // 1: the raw unit is the nanosecond (NVN2)

static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) { return nvn_timestamp_ns ? ns : (uint64_t)((__uint128_t)ns * 384 / 625); }
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) { return nvn_timestamp_ns ? ticks : (uint64_t)((__uint128_t)ticks * 625 / 384); }
