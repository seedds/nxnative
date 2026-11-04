// Switch 2 scanout HDR state. Front ends publish real display capability; vi answers it.
#pragma once
#include <stdint.h>
#define DM_HDR_PAPER_WHITE 203
#define DM_HDR_MASTERING_PEAK 1000
#define DM_HDR_LINEAR_UNIT 80
int dm_hdr_requested(void);
void dm_hdr_display(int available);
int dm_hdr_query(void);
int dm_hdr_advertised(void);
void dm_hdr_gamut(int gamut);
int dm_hdr_content_gamut(void); // -1 = unspecified; retains the guest metadata enum
void dm_hdr_luminance(int extended, uint16_t low, uint16_t high);
void dm_hdr_luminance_range(int extended, uint16_t *low, uint16_t *high);
// Called on the UI thread before the guest starts, and when its screen changes.
void dm_hdr_configure_layer(void *layer, double potential_headroom);
// Called by the present owner; applies content metadata for following drawables.
void dm_hdr_layer_metadata(void *layer, int hdr_content);
