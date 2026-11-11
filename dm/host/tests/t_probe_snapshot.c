#include "../probe_snapshot.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    probe_snapshot_t last = {0}, now = {.address = 0x1234, .readable = 1};
    assert(probe_snapshot_changed(&last, &now, 4));
    assert(!probe_snapshot_changed(&last, &now, 4));
    now.bytes[4] = 1; assert(!probe_snapshot_changed(&last, &now, 4));
    now.bytes[3] = 1; assert(probe_snapshot_changed(&last, &now, 4));
    now.address++; assert(probe_snapshot_changed(&last, &now, 4));
    now.readable = 0; assert(probe_snapshot_changed(&last, &now, 4));
    now.bytes[0] = 2; assert(!probe_snapshot_changed(&last, &now, 4));
    now.readable = 1; assert(probe_snapshot_changed(&last, &now, 4));
    now.bytes[15] = 1; assert(probe_snapshot_changed(&last, &now, 16));
    assert(!probe_snapshot_changed(&last, &now, 16));
    now.scalar = 0x3f800000; assert(probe_snapshot_changed(&last, &now, 16));
    assert(!probe_snapshot_changed(&last, &now, 16));
    puts("probe snapshots: 12 checks passed");
}
