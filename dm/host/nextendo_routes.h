// Public endpoint policy, separate from account credentials and game eligibility.
#pragma once
#include <stddef.h>
enum { DM_NEXTENDO_ROUTES_UNINITIALIZED = -1, DM_NEXTENDO_ROUTES_READY = 0,
       DM_NEXTENDO_ROUTES_MISSING = 1, DM_NEXTENDO_ROUTES_INVALID = 2,
       DM_NEXTENDO_ROUTES_IO = 3, DM_NEXTENDO_ROUTES_LIMIT = 4,
       DM_NEXTENDO_ROUTES_CERTIFICATE = 5 };
// Call after network preferences and before the guest. Reads DM_NEXTENDO_ROUTES
// once; no token is accepted through the file or environment. Repeated calls are
// idempotent. DNS/TLS callbacks also initialize lazily if registered at startup.
// V2's pinned-exact kind requires an explicitly enrolled, hash-checked DER leaf
// for one exact host, with unchanged SNI. It never discovers or updates a pin.
// pinned-address pairs one numeric handoff with that exact identity, address and
// certificate. An unpaired address or a changed certificate is rejected.
int dm_nextendo_routes_initialize(void);
int dm_nextendo_routes_status(void);
size_t dm_nextendo_routes_count(void);
// Explicit setup recovery only. A successfully published policy stays immutable.
int dm_nextendo_routes_retry_unconfigured(void);

// Strong capability hook for an optional BCAT backend; absent providers default off.
int dm_bcat_online_enabled(void);
