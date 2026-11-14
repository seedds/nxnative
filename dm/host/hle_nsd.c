// The NSD service boundary is separate from unrelated account/parental-control fixtures.
#include "hle.h"
#include "nsd.h"

HLE_FORCE("_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", "bounded retail Fqdn expansion in explicit Nextendo mode", Result, h_nsd_Resolve, void *out, const void *in) { return dm_nsd_resolve(out,in); }
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "bounded retail Fqdn expansion in explicit Nextendo mode", Result, h_nsd_ResolveEx, void *out, const void *in) { return dm_nsd_resolve(out,in); }
