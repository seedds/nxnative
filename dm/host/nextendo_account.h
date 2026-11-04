// Native account bridge. The embedding host owns storage and authorization.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { DM_ACCOUNT_OK = 0, DM_ACCOUNT_FAIL = 0x2a2 };
// Explicit embedding provider, installed before guest startup. It copies schema-1
// JSON into the bounded caller buffer. It must not show UI or wait for authorization.
// No provider means unavailable; no token is accepted through the environment.
typedef bool (*dm_nextendo_credential_provider)(uint8_t *buffer, size_t capacity, size_t *size);
void dm_nextendo_set_credential_provider(dm_nextendo_credential_provider provider);
// Bundle metadata is read from the original game's CFBundleShortVersionString.
// An embedding host may provide its actual version before guest startup (not an update substitute).
void dm_nextendo_set_game_version(const char *version);
uint32_t dm_nextendo_account_available(uint8_t *out);
uint32_t dm_nextendo_account_id(uint64_t *out);
uint32_t dm_nextendo_account_ensure(void);
uint32_t dm_nextendo_account_load_token(uint64_t *size, char *buffer, uint64_t capacity);
// Public PKCS#1 RSA key for verifying this process's local envelope signature.
// This does not claim registration with Nintendo or a Nextendo JWKS service.
uint32_t dm_nextendo_copy_public_signing_key(void *buffer, uint64_t capacity, uint64_t *size);
void dm_nextendo_async_construct(void *object);
void dm_nextendo_async_destroy(void *object);
void *dm_nextendo_async_move(void *object, void *source);
void dm_nextendo_async_swap(void *object, void *other);
uint32_t dm_nextendo_async_ensure(void *object);
// Execute an actual service operation with the same cancellation, move and
// completion-event lifetimes as account requests. The callback may block.
typedef uint32_t (*dm_nextendo_async_operation)(const void *context);
uint32_t dm_nextendo_async_execute(void *object, dm_nextendo_async_operation operation, const void *context);
uint32_t dm_nextendo_async_cancel(void *object);
uint32_t dm_nextendo_async_done(void *object, uint8_t *out);
uint32_t dm_nextendo_async_result(void *object);
bool dm_nextendo_async_valid(void *object);
uint64_t dm_nextendo_async_lifetime(void *object);
uint32_t dm_nextendo_async_attach(void *object, uint64_t lifetime, uint64_t event_binding);
