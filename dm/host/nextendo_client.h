// Nextendo Network client core: configuration, OAuth 2.0 + PKCE, nx2. token parsing,
// local BAAS id_token JWT minting, DNS/hostname redirection, SNI tracking, and friends/presence state.
// Opt-in via DM_NEXTENDO=1; offline mode remains the default when disabled.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    DM_NEXTENDO_MAX_FRIENDS = 128,
    DM_NEXTENDO_APP_FIELD_SIZE = 0xC0,
};

typedef enum {
    DM_NX_HOST_BLOCKED = 0,
    DM_NX_HOST_LITERAL_IP = 1,
    DM_NX_HOST_SERVER = 2,
    DM_NX_HOST_NAT = 3,
    DM_NX_HOST_PASSTHROUGH = 4,
} dm_nextendo_host_kind;

typedef struct {
    char verifier[64];
    char challenge[64];
    char state[48];
} dm_nextendo_pkce;

typedef struct {
    uint64_t pid;
    char name[33];
    int favorite;
    uint32_t online_status;   // 0 offline, 1 online, 2 online play
    uint64_t app_id;
    uint8_t app_field[DM_NEXTENDO_APP_FIELD_SIZE];
    size_t app_field_len;
} dm_nextendo_friend;

typedef struct {
    char api_url[256];
    char client_id[128];
    char server_host[256];
    char nat_host[256];
    char nat_domains[512];
    char baas_host[256];
    char baas_key_path[512];
    char nex_token[512];
    char access_token[512];
    char refresh_token[512];
    char username[64];
    char country[8];
    uint64_t pid;
    int64_t token_expiry;
    int allow_passthrough;
} dm_nextendo_config;

// Runtime gate: returns 1 iff DM_NEXTENDO is enabled (non-zero). Offline is the default (0).
int dm_nextendo_enabled(void);

// Reset cached configuration, DNS, token, and friend state (for unit tests).
void dm_nextendo_reset(void);

// Load current configuration (config file + environment overrides) into *out.
void dm_nextendo_get_config(dm_nextendo_config *out);

// Persist current config/tokens atomically to DM_NEXTENDO_CONFIG (mode 0600). Returns 0 on success.
int dm_nextendo_save_config(void);

// Base64url (RFC 4648 §5, unpadded) helpers. Return written length, or -1 on error/overflow.
int dm_nextendo_base64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
int dm_nextendo_base64url_decode(const char *in, uint8_t *out, size_t out_cap);

// OAuth 2.0 + PKCE (RFC 7636) helpers.
int dm_nextendo_pkce_generate(dm_nextendo_pkce *out);
int dm_nextendo_build_authorize_url(const dm_nextendo_pkce *pkce, const char *redirect_uri, char *out, size_t cap);
int dm_nextendo_parse_callback_query(const char *query, const char *expected_state, char *code_out, size_t code_cap);

// Parse an nx2.<base64url(pid.username.expiry)>.<base64url(hmac)> token. Returns 0 on valid structure.
int dm_nextendo_parse_nx2_token(const char *token, uint64_t *pid_out, char *user_out, size_t user_cap, int64_t *exp_out);

// Account state & token management.
int dm_nextendo_is_linked(void);
uint64_t dm_nextendo_get_pid(void);
void dm_nextendo_get_username(char *out, size_t cap);
void dm_nextendo_get_country(char *out, size_t cap);
int dm_nextendo_set_tokens(const char *nex_token, const char *access_token, const char *refresh_token);
int dm_nextendo_ensure_login(void);

// Mint (or return cached) BAAS id_token JWT embedding the nx2. token in the nnex claim.
int dm_nextendo_mint_baas_id_token(char *out, size_t cap, size_t *len_out);

// NSD & DNS redirection.
int dm_nextendo_nsd_resolve(const char *in, char *out, size_t cap);
dm_nextendo_host_kind dm_nextendo_classify_host(const char *host);
int dm_nextendo_resolve_host(const char *host, uint32_t *ipv4_host_order, char *sni_out, size_t sni_cap);
int dm_nextendo_lookup_sni(uint32_t ipv4_host_order, char *out, size_t cap);
int dm_nextendo_is_allowed_tls_peer(uint32_t ipv4_host_order);
int dm_nextendo_host_prefers_h2(const char *sni);

// Friends, presence, and BCAT.
void dm_nextendo_set_friends_cache(const dm_nextendo_friend *friends, int count);
int dm_nextendo_get_friends(dm_nextendo_friend *out, int max);
int dm_nextendo_refresh_friends(void);
int dm_nextendo_update_presence(int status, const uint8_t *app_field, size_t app_field_len, const char *detail);
int dm_nextendo_sync_bcat(const char *title_id, const char *dest_dir);

// Weak platform hooks implemented in nextendo_apple.m (overridable in unit tests).
int dm_nextendo_platform_sign_in(const dm_nextendo_config *cfg, char *nex_out, size_t nex_cap,
                                 char *access_out, size_t access_cap, char *refresh_out, size_t refresh_cap);
int dm_nextendo_platform_fetch_friends(const dm_nextendo_config *cfg, dm_nextendo_friend *out, int max);
int dm_nextendo_platform_post_presence(const dm_nextendo_config *cfg, int status, const char *app_id,
                                       const char *app_field_b64, const char *detail);
int dm_nextendo_platform_sync_bcat(const dm_nextendo_config *cfg, const char *title_id, const char *dest_dir);
int dm_nextendo_platform_sign_rs256(const char *pem_path, const uint8_t *msg, size_t msg_len,
                                    uint8_t *sig_out, size_t *sig_len);

// Platform TLS connection handle (implemented in nextendo_apple.m; stubbed weak for unit tests).
typedef struct dm_ssl_conn dm_ssl_conn;
dm_ssl_conn *dm_nextendo_ssl_create(void);
void dm_nextendo_ssl_destroy(dm_ssl_conn *c);
int dm_nextendo_ssl_set_fd(dm_ssl_conn *c, int host_fd);
int dm_nextendo_ssl_set_hostname(dm_ssl_conn *c, const char *host);
int dm_nextendo_ssl_set_alpn(dm_ssl_conn *c, const char *proto);
int dm_nextendo_ssl_set_io_mode(dm_ssl_conn *c, int nonblock);
int dm_nextendo_ssl_handshake(dm_ssl_conn *c);
int dm_nextendo_ssl_read(dm_ssl_conn *c, void *buf, size_t len, int peek);
int dm_nextendo_ssl_write(dm_ssl_conn *c, const void *buf, size_t len);
int dm_nextendo_ssl_pending(dm_ssl_conn *c);
int dm_nextendo_ssl_poll(dm_ssl_conn *c, int want_read, int want_write, int timeout_ms);
uint32_t dm_nextendo_ssl_last_error(dm_ssl_conn *c);

// System root CA certificates in DER format for nn::ssl::BuiltInManager (macOS/iOS SecTrustCopyAnchorCertificates).
uint32_t dm_nextendo_builtin_certs_bufsize(unsigned max_certs);
int dm_nextendo_builtin_certs_fill(void **out_infos, uint8_t *buf, uint32_t buf_size, unsigned max_certs, uint32_t *out_count);
