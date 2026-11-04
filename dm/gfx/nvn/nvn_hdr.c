#include "nvn_hdr.h"
#include <stdatomic.h>
#include <stdlib.h>
static atomic_int display_available;
static atomic_int advertised;
static atomic_int gamut = -1;
static _Atomic uint32_t standard_range = DM_HDR_PAPER_WHITE << 16;
static _Atomic uint32_t extended_range = DM_HDR_MASTERING_PEAK << 16;
int dm_hdr_requested(void) {
    const char *setting = getenv("DM_HDR");
    return setting && atoi(setting) == 1;
}
void dm_hdr_display(int available) {
    atomic_store(&display_available, available != 0);
}
int dm_hdr_query(void) {
    int enabled = dm_hdr_requested() && atomic_load(&display_available);
    // Remember negotiation: a queued HDR image must still be decoded when a display disconnects.
    if (enabled) {
        atomic_store(&advertised, 1);
    }
    return enabled;
}
int dm_hdr_advertised(void) {
    return atomic_load(&advertised);
}
void dm_hdr_gamut(int value) {
    if (value >= -1 && value <= 1) {
        atomic_store(&gamut, value);
    }
}
int dm_hdr_content_gamut(void) {
    return atomic_load(&gamut);
}
void dm_hdr_luminance(int extended, uint16_t low, uint16_t high) {
    if (high < low || !high) {
        return;
    }
    atomic_store(extended ? &extended_range : &standard_range, low | (uint32_t)high << 16);
}
void dm_hdr_luminance_range(int extended, uint16_t *low, uint16_t *high) {
    uint32_t range = atomic_load(extended ? &extended_range : &standard_range);
    *low = range & 0xffff;
    *high = range >> 16;
}
