// Internal native HTTPS boundary. Never exposed to the guest HLE symbol table.
#pragma once
#import <Foundation/Foundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// In explicit Nextendo mode with a currently valid game credential: the sign-in's OAuth access token, or the nx2 game token without one.
NSString *dm_nextendo_api_bearer(void);
NSData *dm_nextendo_api_identity(void);
bool dm_nextendo_api_baas_namespace(const char *kind);
uint32_t dm_nextendo_api_baas_id(uint64_t *output);
// Only the canonical provider's friends, me, profile, presence (posted) and history (read, or written with a body) resources are admitted.
// Without an output buffer (NULL and 0) only the status counts.
uint32_t dm_nextendo_api_request(const char *resource, const uint8_t *body, size_t body_size,
                               uint8_t *output, size_t capacity, size_t *output_size);
#ifdef DM_NEXTENDO_API_TEST
typedef bool (*dm_nextendo_api_transport)(const char *url, const char *method, const char *authorization,
    const uint8_t *body, size_t body_size, uint8_t *output, size_t capacity, size_t *size, unsigned *status);
void dm_nextendo_api_test_transport(dm_nextendo_api_transport transport);
#endif
