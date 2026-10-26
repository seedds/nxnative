#ifndef DM_MAP_HEADROOM_H
#define DM_MAP_HEADROOM_H

#include <stdint.h>

// This is diagnostic-only. A b18 run proved that rejecting small runtime maps
// under pressure propagates ResultLimitReached into the game instead of making
// this SDK discard enough cached pages. Keep every platform's default off;
// DM_MAP_HEADROOM_MB can still reproduce and measure the behavior explicitly.
#define DM_MAP_HEADROOM_TV_DEFAULT_MB 0ull

static inline uint64_t dm_map_headroom_default_bytes(int is_tvos, int is_simulator) {
    return is_tvos && !is_simulator ? DM_MAP_HEADROOM_TV_DEFAULT_MB << 20 : 0;
}

// MapPhysicalMemory only changes protection on reserved virtual pages; it does
// not commit every byte in the requested range. In particular, Z-A maps its
// roughly 3.15 GiB StandardAllocator arena at startup while the process still
// uses only a few MiB. Refuse only after the process's measured allowance has
// fallen below an explicitly requested reserve. new_bytes keeps already-mapped
// or no-op requests out of the guard. Equality is allowed.
static inline int dm_map_headroom_should_refuse(uint64_t available, uint64_t new_bytes, uint64_t reserve) {
    if (!reserve || !new_bytes) return 0;
    return available < reserve;
}

#endif
