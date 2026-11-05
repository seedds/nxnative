#include "nvn_hdr.h"
#include <stdatomic.h>
#include <stdlib.h>
static atomic_int display_available, advertised, gamut = -1;
static _Atomic uint32_t standard_range = DM_HDR_PAPER_WHITE << 16;
static _Atomic uint32_t extended_range = DM_HDR_MASTERING_PEAK << 16;
int dm_hdr_requested(void) { const char *e = getenv("DM_HDR"); return e && atoi(e) == 1; }
void dm_hdr_display(int available) { atomic_store(&display_available, available != 0); }
int dm_hdr_query(void) {
    int on = dm_hdr_requested() && atomic_load(&display_available);
    // Remember negotiation: a queued HDR image must still be decoded when a display disconnects.
    if (on) atomic_store(&advertised, 1);
    return on;
}
int dm_hdr_advertised(void) { return atomic_load(&advertised); }
void dm_hdr_gamut(int g) { if (g == 0 || g == 1) atomic_store(&gamut, g); }
int dm_hdr_content_gamut(void) { return atomic_load(&gamut); }
void dm_hdr_luminance(int extended, uint16_t low, uint16_t high) {
    if (high < low || !high) return;
    atomic_store(extended ? &extended_range : &standard_range, low | (uint32_t)high << 16);
}
void dm_hdr_luminance_range(int extended, uint16_t *low, uint16_t *high) {
    uint32_t r = atomic_load(extended ? &extended_range : &standard_range);
    *low = r & 0xffff; *high = r >> 16;
}
