#include "../map_headroom.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    const uint64_t mb = 1ull << 20;
    const uint64_t reserve = 384 * mb;

    assert(dm_map_headroom_default_bytes(1, 0) == 0);
    assert(dm_map_headroom_default_bytes(1, 1) == 0);
    assert(dm_map_headroom_default_bytes(0, 0) == 0);

    assert(!dm_map_headroom_should_refuse(0, 64 * mb, 0));
    assert(!dm_map_headroom_should_refuse(reserve - 1, 0, reserve));
    assert(dm_map_headroom_should_refuse(reserve - 1, 1, reserve));
    assert(!dm_map_headroom_should_refuse(reserve, 1, reserve));
    assert(!dm_map_headroom_should_refuse(reserve + 4 * mb, 4 * mb, reserve));
    assert(!dm_map_headroom_should_refuse(reserve + 4 * mb - 1, 4 * mb, reserve));

    // A large virtual map does not immediately consume its length in physical
    // memory. This is Z-A's startup arena shape and must not be rejected.
    assert(!dm_map_headroom_should_refuse(2076 * mb, 0xc8ab9000ull, reserve));

    // The b16 observations used for the explicit diagnostic experiment.
    assert(!dm_map_headroom_should_refuse(394 * mb, 4 * mb, reserve));
    assert(dm_map_headroom_should_refuse(364 * mb, 4 * mb, reserve));

    assert(!dm_map_headroom_should_refuse(UINT64_MAX, UINT64_MAX - 1, 1));
    assert(!dm_map_headroom_should_refuse(UINT64_MAX, UINT64_MAX, 1));
    assert(dm_map_headroom_should_refuse(1, UINT64_MAX, UINT64_MAX));

    puts("map headroom: PASS");
    return 0;
}
