// Client TLS adapter. Guest objects are registry keys; no guessed SDK object fields are written.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { DM_SSL_OK = 0, DM_SSL_FAIL = 0x2a2, DM_SSL_BAD_POINTER = 0xe27b,
       DM_SSL_NO_SOCKET = 0xce7b, DM_SSL_BAD_SOCKET = 0xd47b,
       DM_SSL_WOULD_BLOCK = 0x1987b, DM_SSL_TIMEOUT = 0x19a7b };
typedef bool (*dm_ssl_peer_policy_provider)(const char *guest_host, char *tls_host, size_t host_cap,
                                           const uint8_t **ca_der, size_t *ca_size);
// Set before guest startup. A provider returns true only for its explicit host whitelist;
// its DER bytes remain immutable for the duration of the callback (the adapter copies them).
void dm_ssl_set_peer_policy(dm_ssl_peer_policy_provider provider);
// Separate, explicit exact-service certificate enrollment. This must never be
// installed as a catch-all or populated automatically from the current peer.
void dm_ssl_set_peer_pin_policy(dm_ssl_peer_policy_provider provider);
// Optional exact-host admission guard. Install before startup; NULL retains the
// adapter's normal strict certificate/hostname validation without routing policy.
void dm_ssl_set_peer_guard(bool (*guard)(const char *guest_host));
// Optional stand-in for the player's firmware certificate store. With no store, a built-in certificate request is answered with this one CA, under each ID
// asked for: the online service's own CA, which hle_nextendo_routes.c gives in Nextendo mode from its checked service profile. NULL or false: no stand-in,
// the request fails as before. A game whose own TLS takes its roots from the store (gRPC) then needs no file from the player. The DER stays valid for the process
void dm_ssl_set_builtin_fallback(bool (*provider)(const uint8_t **ca_der, size_t *ca_size));
uint32_t dm_ssl_initialize(unsigned concurrency);
uint32_t dm_ssl_finalize(void);
uint32_t dm_ssl_context_create(void *object, unsigned version);
uint32_t dm_ssl_context_destroy(void *object);
uint32_t dm_ssl_context_id(void *object, uint64_t *out);
uint32_t dm_ssl_import_server(void *object, uint64_t *id, const void *bytes, unsigned size, int format);
uint32_t dm_ssl_import_client(void *object, uint64_t *id, const void *bytes, const char *password, unsigned size, unsigned password_size);
uint32_t dm_ssl_remove_pki(void *object, uint64_t id);
uint32_t dm_ssl_connection_create(void *object, void *context);
uint32_t dm_ssl_connection_destroy(void *object);
uint32_t dm_ssl_connection_id(void *object, uint64_t *out, bool context);
uint32_t dm_ssl_set_socket(void *object, int socket);
uint32_t dm_ssl_get_socket(void *object, int *out);
uint32_t dm_ssl_set_hostname(void *object, const char *name, unsigned size);
uint32_t dm_ssl_get_hostname(void *object, char *out, unsigned *size, unsigned capacity);
enum { DM_SSL_IO_MODE, DM_SSL_VERIFY_OPTIONS, DM_SSL_SESSION_CACHE, DM_SSL_IO_TIMEOUT };
uint32_t dm_ssl_set_value(void *object, int property, unsigned value);
uint32_t dm_ssl_get_value(void *object, int property, unsigned *out);
uint32_t dm_ssl_set_option(void *object, int option, unsigned value);
uint32_t dm_ssl_get_option(void *object, void *out_bool, int option);
uint32_t dm_ssl_handshake(void *object);
uint32_t dm_ssl_handshake_certs(void *object, unsigned *size, unsigned *count, void *buffer, unsigned capacity, bool registered_buffer);
uint32_t dm_ssl_set_cert_buffer(void *object, void *buffer, unsigned capacity);
uint32_t dm_ssl_needed_cert_size(void *object, unsigned *out);
uint32_t dm_ssl_cipher_info(void *object, void *out);
uint32_t dm_ssl_read(void *object, void *buffer, int *out, unsigned capacity, bool peek);
uint32_t dm_ssl_write(void *object, const void *buffer, int *out, unsigned size);
uint32_t dm_ssl_pending(void *object, int *out);
uint32_t dm_ssl_poll(void *object, unsigned *out, const unsigned *events, unsigned timeout_ms);
uint32_t dm_ssl_last_error(void *object, uint32_t *out, bool verify);
uint32_t dm_ssl_set_alpn(void *object, const uint8_t *data, unsigned size);
uint32_t dm_ssl_get_alpn(void *object, unsigned *state, uint8_t *buffer, unsigned *size, unsigned capacity);
uint32_t dm_ssl_cert_detail(void *out, const void *buffer, unsigned index);
uint32_t dm_ssl_builtin_size(unsigned *out, const uint32_t *ids, unsigned count);
uint32_t dm_ssl_builtin_certificates(void **out, uint8_t *buffer, unsigned capacity, const uint32_t *ids, unsigned count);
