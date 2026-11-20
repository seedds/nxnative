// Strict private-PKI verification for an explicitly configured Nextendo route.
// No default trust store, user credentials, clock override or permissive callback.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool dm_nextendo_x509_available(void);
bool dm_nextendo_x509_verify(const char *hostname, const uint8_t *anchor, size_t anchor_size,
                            const uint8_t *const *chain, const size_t *sizes, size_t count);
// Explicitly enrolled self-signed leaf identity for one exact service route.
// No peer certificate is adopted automatically; DER must equal the enrolled pin.
bool dm_nextendo_x509_verify_pin(const char *hostname, const uint8_t *pin, size_t pin_size,
                                const uint8_t *leaf, size_t leaf_size);
