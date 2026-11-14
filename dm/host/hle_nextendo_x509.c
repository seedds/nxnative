// An alternative verifier for the service's explicit private CA, not public PKI.
// OpenSSL applies strict X.509 chain, purpose, SAN, strength and current-time checks.
// Apple public-PKI maximum leaf lifetimes do not define this private CA's lifetime.
#include "nextendo_x509.h"
#include <string.h>
#include <time.h>
#ifdef DM_OPENSSL_X509
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>

static bool valid_host(const char *host) {
    if (!host) return false;
    size_t length = strnlen(host, 254), label = 0;
    if (!length || length > 253) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c == '.') { if (!label || host[i - 1] == '-') return false; label = 0; }
        else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-') || (!label && c == '-') || ++label > 63) return false;
        }
    }
    return label && host[length - 1] != '-';
}
static X509 *certificate(const uint8_t *bytes, size_t size) {
    if (!bytes || !size || size > 65536) return NULL;
    const unsigned char *cursor = bytes;
    X509 *value = d2i_X509(NULL, &cursor, (long)size);
    if (value && cursor != bytes + size) { X509_free(value); value = NULL; }
    return value;
}
bool dm_nextendo_x509_available(void) { return true; }
bool dm_nextendo_x509_verify_pin(const char *host, const uint8_t *pin, size_t pin_size,
                                const uint8_t *bytes, size_t size) {
    if (!valid_host(host) || !pin || !bytes || !size || size > 65536 || pin_size != size ||
        CRYPTO_memcmp(pin, bytes, size)) return false;
    X509 *leaf = certificate(bytes, size); if (!leaf) return false;
    EVP_PKEY *public_key = X509_get_pubkey(leaf);
    ASN1_TIME *now = ASN1_TIME_set(NULL, time(NULL));
    int before = now ? ASN1_TIME_compare(X509_get0_notBefore(leaf), now) : -2;
    int after = now ? ASN1_TIME_compare(X509_get0_notAfter(leaf), now) : -2;
    int signature_bits = 0;
    // The enrolled route supplies identity, rather than an absent SAN. Retain
    // current-time, server-purpose, strong-key and self-signature checks. TLS
    // independently proves possession of this exact certificate's private key.
    bool valid = public_key && EVP_PKEY_security_bits(public_key) >= 112 &&
        X509_NAME_cmp(X509_get_subject_name(leaf), X509_get_issuer_name(leaf)) == 0 &&
        before != -2 && before <= 0 && after >= 0 &&
        X509_check_purpose(leaf, X509_PURPOSE_SSL_SERVER, 0) == 1 &&
        X509_get_signature_info(leaf, NULL, NULL, &signature_bits, NULL) == 1 && signature_bits >= 112 &&
        X509_verify(leaf, public_key) == 1;
    // Some private endpoints serve their self-signed CA-marked certificate
    // directly. Exact peer-DER equality does not grant it issuer authority:
    // a different leaf signed by this key still fails the comparison above.
    ASN1_TIME_free(now); EVP_PKEY_free(public_key); X509_free(leaf); ERR_clear_error();
    return valid;
}
bool dm_nextendo_x509_verify(const char *host, const uint8_t *anchor, size_t anchor_size,
                            const uint8_t *const *chain, const size_t *sizes, size_t count) {
    if (!valid_host(host) || !chain || !sizes || !count || count > 16) return false;
    bool ok = false; size_t total = 0;
    X509 *root = certificate(anchor, anchor_size), *leaf = NULL;
    X509_STORE *store = X509_STORE_new();
    X509_STORE_CTX *context = X509_STORE_CTX_new();
    STACK_OF(X509) *untrusted = sk_X509_new_null();
    if (!root || !store || !context || !untrusted || X509_check_ca(root) <= 0 ||
        X509_check_issued(root, root) != X509_V_OK || X509_STORE_add_cert(store, root) != 1) goto done;
    for (size_t i = 0; i < count; i++) {
        if (!sizes[i] || sizes[i] > 65536 || total > 1048576 - sizes[i]) goto done;
        total += sizes[i]; X509 *item = certificate(chain[i], sizes[i]); if (!item) goto done;
        if (!i) leaf = item;
        else if (!sk_X509_push(untrusted, item)) { X509_free(item); goto done; }
    }
    if (X509_STORE_CTX_init(context, store, leaf, untrusted) != 1) goto done;
    X509_VERIFY_PARAM *parameters = X509_STORE_CTX_get0_param(context);
    if (!parameters || X509_VERIFY_PARAM_set_purpose(parameters, X509_PURPOSE_SSL_SERVER) != 1 ||
        X509_VERIFY_PARAM_set_flags(parameters, X509_V_FLAG_X509_STRICT | X509_V_FLAG_CHECK_SS_SIGNATURE) != 1 ||
        X509_VERIFY_PARAM_set1_host(parameters, host, 0) != 1) goto done;
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    X509_VERIFY_PARAM_set_auth_level(parameters, 2);
    X509_VERIFY_PARAM_set_depth(parameters, 12);
    // No default paths, partial-chain trust, time suppression, custom verify
    // callback or automatically adopted certificate supplied by the peer.
    ok = X509_verify_cert(context) == 1;
done:
    X509_STORE_CTX_free(context); X509_STORE_free(store); X509_free(root);
    X509_free(leaf); sk_X509_pop_free(untrusted, X509_free); ERR_clear_error();
    return ok;
}
#else
bool dm_nextendo_x509_available(void) { return false; }
bool dm_nextendo_x509_verify_pin(const char *host, const uint8_t *pin, size_t pin_size,
                                const uint8_t *leaf, size_t leaf_size) {
    (void)host; (void)pin; (void)pin_size; (void)leaf; (void)leaf_size; return false;
}
bool dm_nextendo_x509_verify(const char *host, const uint8_t *anchor, size_t anchor_size,
                            const uint8_t *const *chain, const size_t *sizes, size_t count) {
    (void)host; (void)anchor; (void)anchor_size; (void)chain; (void)sizes; (void)count; return false;
}
#endif
