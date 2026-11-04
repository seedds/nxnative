// Internal native HTTPS boundary. Never exposed to the guest HLE symbol table.
#pragma once
#import <Foundation/Foundation.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Returns only a currently valid NEX proof, in explicit Nextendo mode.
NSString *dm_nextendo_api_bearer(void);
NSData *dm_nextendo_api_identity(void);
bool dm_nextendo_api_baas_namespace(const char *kind);
uint32_t dm_nextendo_api_baas_id(uint64_t *output);
// Only the canonical provider's friends, me and presence resources are admitted.
uint32_t dm_nextendo_api_request(const char *resource, const uint8_t *body, size_t body_size,
                               uint8_t *output, size_t capacity, size_t *output_size);
#ifdef DM_NEXTENDO_API_TEST
typedef bool (*dm_nextendo_api_transport)(const char *url, const char *method, const char *authorization,
    const uint8_t *body, size_t body_size, uint8_t *output, size_t capacity, size_t *size, unsigned *status);
void dm_nextendo_api_test_transport(dm_nextendo_api_transport transport);
#endif
