// Passive Joy-Con 0x30 button report decoder. No transport or controller routing.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Reports include the report-ID byte. Require the complete 49-byte standard report.
// On failure, clear the output so a caller cannot retain a stale pressed button.
static int dm_joycon_rails_decode(uint32_t product, uint32_t report_id,
                                 const uint8_t *bytes, size_t len, uint8_t *out) {
    if (!out) return 0;
    *out = 0;
    if ((product != 0x2006 && product != 0x2007) || report_id != 0x30 ||
        !bytes || len < 49 || bytes[0] != 0x30) return 0;
    *out = bytes[product == 0x2006 ? 5 : 3] & 0x30; // SL=0x20, SR=0x10; not L/R/ZL/ZR.
    return 1;
}
