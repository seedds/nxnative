// Original exact-host routing policy. Endpoint facts/configuration do not import
// another client's implementation, credentials, wildcard rules or TLS bypasses.
#include "nextendo_routes.h"
#include "net_socket.h"
#include "ssl_backend.h"
#include <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { ROUTE_MAX = 128, CONFIG_MAX = 65536, CERT_MAX = 65536, CERT_TOTAL_MAX = 1024 * 1024 };
typedef struct { char host[254], address[INET_ADDRSTRLEN], peer[254]; unsigned match; bool pinned, address_alias; uint8_t *ca; size_t ca_size; } route;
static route *routes;
static size_t route_count;
static pthread_mutex_t policy_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int policy_status = DM_NEXTENDO_ROUTES_UNINITIALIZED;

static bool hostname(const char *input, char out[254]) {
    if (!input) return false; size_t length = strnlen(input, 255); if (!length || length > 254) return false;
    if (input[length - 1] == '.') length--; if (!length || length > 253) return false;
    size_t label = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '.') { if (!label || input[i - 1] == '-') return false; label = 0; }
        else { if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || (!label && c == '-') || ++label > 63) return false; }
        out[i] = (char)c;
    }
    if (!label || input[length - 1] == '-') return false; out[length] = 0; return true;
}
static bool route_pattern(const char *kind, const char *input, route *entry) {
    if (!strcmp(kind, "exact")) entry->match = 0;
    else if (!strcmp(kind, "pinned-exact")) { entry->match = 0; entry->pinned = true; }
    else if (!strcmp(kind, "pinned-address")) { entry->match = 0; entry->pinned = true; entry->address_alias = true; }
    else if (!strcmp(kind, "suffix")) entry->match = 1;
    else if (!strcmp(kind, "label")) entry->match = 2;
    else return false;
    if (entry->match != 2) return hostname(input, entry->host);
    if (!input || strnlen(input, 254) >= 254) return false;
    char check[254]; strcpy(check, input); char *star = strchr(check, '*');
    if (!star || strchr(star + 1, '*')) return false;
    size_t offset = (size_t)(star - check); *star = 'x';
    if (!hostname(check, entry->host)) return false;
    entry->host[offset] = '*'; return true;
}
static unsigned match_score(const route *entry, const char *host) {
    size_t n = strlen(host), m = strlen(entry->host);
    if (!entry->match) return !strcmp(host, entry->host) ? 100000u + (unsigned)m : 0;
    if (entry->match == 1)
        return (n == m && !strcmp(host, entry->host)) ||
            (n > m && host[n - m - 1] == '.' && !strcmp(host + n - m, entry->host)) ? (unsigned)m : 0;
    const char *star = strchr(entry->host, '*'); size_t prefix = (size_t)(star - entry->host), suffix = m - prefix - 1;
    if (n < prefix + suffix || strncmp(host, entry->host, prefix) || strcmp(host + n - suffix, star + 1)) return 0;
    // A label wildcard cannot cross a DNS-label boundary.
    if (memchr(host + prefix, '.', n - prefix - suffix)) return 0;
    return 10000u + (unsigned)m;
}
static int read_regular(int fd, size_t limit, uint8_t **bytes, size_t *length) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_uid != geteuid() && st.st_uid != 0) || (st.st_mode & 022) || st.st_size <= 0) return DM_NEXTENDO_ROUTES_IO;
    if ((uint64_t)st.st_size > limit) return DM_NEXTENDO_ROUTES_LIMIT;
    uint8_t *data = malloc((size_t)st.st_size + 1); if (!data) return DM_NEXTENDO_ROUTES_LIMIT; size_t done = 0;
    while (done < (size_t)st.st_size) { ssize_t n = read(fd, data + done, (size_t)st.st_size - done); if (n < 0 && errno == EINTR) continue; if (n <= 0) { free(data); return DM_NEXTENDO_ROUTES_IO; } done += (size_t)n; }
    data[done] = 0; *bytes = data; *length = done; return 0;
}
static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int load_ca(int directory, const char *name, const char *expected, route *entry) {
    if (!strcmp(name, "-") && !strcmp(expected, "-")) return 0;
    if (!name[0] || strlen(name) > NAME_MAX || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || strlen(expected) != 64) return DM_NEXTENDO_ROUTES_CERTIFICATE;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    for (unsigned i = 0; i < sizeof digest; i++) { int a = hex_digit(expected[i * 2]), b = hex_digit(expected[i * 2 + 1]); if (a < 0 || b < 0) return DM_NEXTENDO_ROUTES_CERTIFICATE; digest[i] = (uint8_t)(a * 16 + b); }
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) return DM_NEXTENDO_ROUTES_CERTIFICATE;
    int status = read_regular(fd, CERT_MAX, &entry->ca, &entry->ca_size); close(fd); if (status) return status;
    uint8_t actual[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(entry->ca, (CC_LONG)entry->ca_size, actual); if (memcmp(actual, digest, sizeof actual)) return DM_NEXTENDO_ROUTES_CERTIFICATE;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, entry->ca, (CFIndex)entry->ca_size);
    SecCertificateRef certificate = data ? SecCertificateCreateWithData(kCFAllocatorDefault, data) : NULL; if (data) CFRelease(data);
    if (!certificate) return DM_NEXTENDO_ROUTES_CERTIFICATE; CFRelease(certificate); return 0;
}
static void free_routes(route *entries, size_t count) { if (!entries) return; for (size_t i = 0; i < count; i++) free(entries[i].ca); free(entries); }
static int load_policy(const char *path, route **output, size_t *count) {
    if (!path || !path[0]) return DM_NEXTENDO_ROUTES_MISSING;
    size_t path_size = strnlen(path, PATH_MAX); if (path[0] != '/' || path_size >= PATH_MAX) return DM_NEXTENDO_ROUTES_INVALID;
    char parent[PATH_MAX]; memcpy(parent, path, path_size + 1); char *slash = strrchr(parent, '/'); if (!slash || !slash[1]) return DM_NEXTENDO_ROUTES_INVALID;
    const char *file = path + (slash - parent) + 1; if (slash == parent) slash[1] = 0; else *slash = 0;
    int directory = open(parent, O_RDONLY | O_CLOEXEC | O_DIRECTORY); if (directory < 0) return errno == ENOENT ? DM_NEXTENDO_ROUTES_MISSING : DM_NEXTENDO_ROUTES_IO;
    int fd = openat(directory, file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) { int status = errno == ENOENT ? DM_NEXTENDO_ROUTES_MISSING : DM_NEXTENDO_ROUTES_IO; close(directory); return status; }
    uint8_t *data = NULL; size_t size = 0; int status = read_regular(fd, CONFIG_MAX, &data, &size); close(fd);
    if (status) { close(directory); return status; }
    if (memchr(data, 0, size)) { free(data); close(directory); return DM_NEXTENDO_ROUTES_INVALID; }
    route *entries = calloc(ROUTE_MAX, sizeof *entries); if (!entries) { free(data); close(directory); return DM_NEXTENDO_ROUTES_LIMIT; }
    char *cursor = (char *)data, *end = (char *)data + size; size_t used = 0, ca_total = 0; bool magic = false, version2 = false;
    while (cursor < end && !status) {
        char *line = cursor, *newline = memchr(cursor, '\n', (size_t)(end - cursor)); if (newline) { *newline = 0; cursor = newline + 1; } else cursor = end;
        size_t length = strlen(line); if (length && line[length - 1] == '\r') line[--length] = 0;
        if (!magic) {
            version2 = !strcmp(line, "NXNEXTENDO_ROUTES_2");
            if (!version2 && strcmp(line, "NXNEXTENDO_ROUTES_1")) status = DM_NEXTENDO_ROUTES_INVALID;
            else magic = true; continue;
        }
        if (!length || line[0] == '#') continue;
        if (used >= ROUTE_MAX || length > 2048) { status = DM_NEXTENDO_ROUTES_LIMIT; break; }
        char *fields[5], *part = line; const char *kind = version2 ? strsep(&part, "\t") : "exact";
        if (!kind || !part) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        for (unsigned i = 0; i < 5; i++) { fields[i] = strsep(&part, "\t"); if (!fields[i] || !fields[i][0]) { status = DM_NEXTENDO_ROUTES_INVALID; break; } }
        if (status || part) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        route *entry = &entries[used]; struct in_addr ip, literal;
        bool dynamic_peer = version2 && !strcmp(fields[2], "@host");
        if (!route_pattern(kind, fields[0], entry) || (inet_pton(AF_INET, entry->host, &literal) == 1) != entry->address_alias || inet_pton(AF_INET, fields[1], &ip) != 1 || (ntohl(ip.s_addr) >> 24) == 0 || (ntohl(ip.s_addr) >> 24) >= 224 ||
            (!strcmp(fields[2], "-") ? (strcmp(fields[3], "-") || strcmp(fields[4], "-")) : (!dynamic_peer && !hostname(fields[2], entry->peer)))) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        if (dynamic_peer) strcpy(entry->peer, "@host");
        // A leaf pin enrolls one exact logical service identity. It cannot be
        // inherited by wildcard hosts or used to rename the TLS service.
        if (entry->pinned && !entry->address_alias && (entry->match || (!dynamic_peer && strcmp(entry->host, entry->peer)))) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        if (entry->address_alias && (dynamic_peer || literal.s_addr != ip.s_addr || !entry->peer[0] || inet_pton(AF_INET, entry->peer, &literal) == 1)) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        if (!inet_ntop(AF_INET, &ip, entry->address, sizeof entry->address)) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        for (size_t i = 0; i < used; i++) if (entries[i].match == entry->match && !strcmp(entries[i].host, entry->host)) { status = DM_NEXTENDO_ROUTES_INVALID; break; }
        if (!status) status = load_ca(directory, fields[3], fields[4], entry);
        if (!status && entry->pinned && !entry->ca_size) status = DM_NEXTENDO_ROUTES_CERTIFICATE;
        if (status) { free(entry->ca); entry->ca = NULL; break; }
        ca_total += entry->ca_size; if (ca_total > CERT_TOTAL_MAX) { free(entry->ca); entry->ca = NULL; status = DM_NEXTENDO_ROUTES_LIMIT; break; } used++;
    }
    free(data); close(directory); if (!magic || !used) status = status ? status : DM_NEXTENDO_ROUTES_INVALID;
    // NEX secure-server handoffs can name an IP instead of the original DNS
    // service. A configured numeric alias inherits only an existing exact pin
    // for the same address and certificate; it grants no independent identity.
    for (size_t i = 0; !status && i < used; i++) if (entries[i].address_alias) {
        const route *alias = &entries[i]; bool paired = false;
        for (size_t j = 0; j < used; j++) {
            const route *identity = &entries[j];
            if (identity->pinned && !identity->address_alias && !identity->match &&
                !strcmp(alias->peer, identity->host) && !strcmp(alias->address, identity->address) &&
                alias->ca_size == identity->ca_size && !memcmp(alias->ca, identity->ca, alias->ca_size)) { paired = true; break; }
        }
        if (!paired) status = DM_NEXTENDO_ROUTES_INVALID;
    }
    if (status) { free_routes(entries, used); return status; } *output = entries; *count = used; return 0;
}
static const route *find_route(const char *host) {
    dm_nextendo_routes_initialize(); if (atomic_load(&policy_status) != DM_NEXTENDO_ROUTES_READY) return NULL;
    char normalized[254]; if (!hostname(host, normalized)) return NULL;
    const route *best = NULL; unsigned score = 0;
    for (size_t i = 0; i < route_count; i++) { unsigned value = match_score(&routes[i], normalized); if (value > score) { best = &routes[i]; score = value; } }
    return best;
}
static int resolve_host(const char *host, char *output, size_t capacity) {
    if (!dm_net_nextendo_enabled() || !host) return 0; struct in_addr ip; if (inet_pton(AF_INET, host, &ip) == 1) return 0;
    const route *entry = find_route(host); if (!entry || capacity <= strlen(entry->address)) return -1; strcpy(output, entry->address); return 1;
}
static bool admit_peer(const char *host) { if (!dm_net_nextendo_enabled()) return true; const route *entry = find_route(host); return entry && entry->peer[0]; }
static bool copy_peer_policy(const char *host, char *output, size_t capacity, const uint8_t **ca, size_t *size, bool pinned) {
    if (!dm_net_nextendo_enabled()) return false; const route *entry = find_route(host); if (!entry || !entry->peer[0]) return false;
    if (entry->pinned != pinned) return false;
    const char *peer = entry->peer; char normalized[254];
    if (!strcmp(peer, "@host")) { if (!hostname(host, normalized)) return false; peer = normalized; }
    if (capacity <= strlen(peer)) return false;
    strcpy(output, peer); *ca = entry->ca; *size = entry->ca_size; return true;
}
static bool peer_policy(const char *host, char *output, size_t capacity, const uint8_t **ca, size_t *size) { return copy_peer_policy(host, output, capacity, ca, size, false); }
static bool pinned_peer_policy(const char *host, char *output, size_t capacity, const uint8_t **ca, size_t *size) { return copy_peer_policy(host, output, capacity, ca, size, true); }
// The service's CA for the SSL service's built-in certificates when the player has no firmware store (hle_ssl_apple.m): a game whose own TLS takes its roots
// from the store (Super Mario Bros. Wonder's gRPC) then needs only the sign-in. The profile's routes share the provider's one CA (dm/net/nextendo-services.json
// provider.caSHA256, checked when the table loaded); an enrolled leaf of a pinned route is not a CA and is skipped
static bool provider_ca(const uint8_t **ca, size_t *size) {
    if (!dm_net_nextendo_enabled() || dm_nextendo_routes_initialize() != DM_NEXTENDO_ROUTES_READY) return false;
    for (size_t i = 0; i < route_count; i++) if (routes[i].ca_size && !routes[i].pinned) { *ca = routes[i].ca; *size = routes[i].ca_size; return true; }
    return false;
}
__attribute__((constructor)) static void register_policy(void) { dm_net_set_dns_provider(resolve_host); dm_ssl_set_peer_guard(admit_peer); dm_ssl_set_peer_policy(peer_policy); dm_ssl_set_peer_pin_policy(pinned_peer_policy); dm_ssl_set_builtin_fallback(provider_ca); }
int dm_nextendo_routes_initialize(void) {
    pthread_mutex_lock(&policy_lock); int status = atomic_load(&policy_status);
    if (status == DM_NEXTENDO_ROUTES_UNINITIALIZED) { status = load_policy(getenv("DM_NEXTENDO_ROUTES"), &routes, &route_count); atomic_store(&policy_status, status); }
    pthread_mutex_unlock(&policy_lock); return status;
}
int dm_nextendo_routes_retry_unconfigured(void) {
    pthread_mutex_lock(&policy_lock); int status = atomic_load(&policy_status);
    // A failed load publishes no route pointers. An explicit launcher setup
    // action may retry that state; a valid published table is never replaced.
    if (status != DM_NEXTENDO_ROUTES_READY) atomic_store(&policy_status, DM_NEXTENDO_ROUTES_UNINITIALIZED);
    pthread_mutex_unlock(&policy_lock); return status;
}
int dm_nextendo_routes_status(void) { return atomic_load(&policy_status); }
size_t dm_nextendo_routes_count(void) { return atomic_load(&policy_status) == DM_NEXTENDO_ROUTES_READY ? route_count : 0; }

// Optional BCAT capability bridge. A valid explicit service profile is required;
// checking it performs no network/account request and does not grant game access.
int dm_bcat_online_enabled(void) {
    return dm_net_nextendo_enabled() && dm_nextendo_routes_initialize() == DM_NEXTENDO_ROUTES_READY && dm_nextendo_routes_count() > 0;
}
