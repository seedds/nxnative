// Change-only diagnostic snapshots; never changes the observed guest memory.
#ifndef DM_PROBE_SNAPSHOT_H
#define DM_PROBE_SNAPSHOT_H
#include <stdint.h>
#include <string.h>
typedef struct {
    uint64_t address;
    uint32_t scalar;
    uint8_t bytes[16];
    int initialized, readable;
} probe_snapshot_t;
static inline int probe_snapshot_changed(probe_snapshot_t *last, const probe_snapshot_t *now, unsigned bytes) {
    int changed = !last->initialized || last->address != now->address || last->scalar != now->scalar || last->readable != now->readable
        || (now->readable && memcmp(last->bytes, now->bytes, bytes));
    if (changed) { *last = *now; last->initialized = 1; }
    return changed;
}
// Polled sites can be sampled by presented frame without sampling away an event-only probe.
typedef struct { uint64_t frame; int initialized; } probe_interval_t;
static inline int probe_interval_due(probe_interval_t *last, uint64_t frame, unsigned interval) {
    if (last->initialized && frame >= last->frame && frame - last->frame < interval) return 0;
    last->frame = frame; last->initialized = 1;
    return 1;
}
#endif
