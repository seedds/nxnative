// Horizon retail service-name expansion. This is separate from socket DNS and
// from the explicit provider routing/trust policy.
#pragma once
#include <stdint.h>
enum { DM_NSD_FQDN_CAPACITY = 0x100, DM_NSD_FAIL = 0x2a2 };
uint32_t dm_nsd_resolve(void *output, const void *input);
