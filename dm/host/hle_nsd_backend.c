#include "nsd.h"
#include "net_socket.h"
#include <string.h>

static int valid_fqdn(const char *name, size_t length) {
    if (!length || length > 253) return 0;
    size_t label = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '.') {
            if (!label || name[i - 1] == '-') return 0;
            label = 0; continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        if ((!label && c == '-') || ++label > 63) return 0;
    }
    return label && name[length - 1] != '-';
}

uint32_t dm_nsd_resolve(void *output, const void *input) {
    // Ordinary offline/LAN exports retain their existing service boundary.
    // No DNS query, socket connection or success placeholder occurs here.
    if (!output || !input || !dm_net_nextendo_enabled()) return DM_NSD_FAIL;
    const char *source = input;
    size_t length = strnlen(source, DM_NSD_FQDN_CAPACITY);
    if (!length || length == DM_NSD_FQDN_CAPACITY) return DM_NSD_FAIL;
    char expanded[DM_NSD_FQDN_CAPACITY]; size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)source[i];
        size_t needed = c == '%' ? 3 : 1;
        if (written + needed >= sizeof expanded) return DM_NSD_FAIL;
        if (c == '%') { memcpy(expanded + written, "lp1", 3); written += 3; }
        else expanded[written++] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    if (written && expanded[written - 1] == '.') written--;
    expanded[written] = 0;
    if (!valid_fqdn(expanded, written)) return DM_NSD_FAIL;
    // Retail NSD account aliases are logical service names, not IP redirects.
    // The subsequent socket resolver still enforces its scoped Nextendo policy.
    const char *resolved = expanded;
    if (!strcmp(expanded, "accounts.nintendo.com") || !strcmp(expanded, "api.accounts.nintendo.com"))
        resolved = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
    // Compute first so in-place and overlapping Fqdn buffers remain valid.
    char result[DM_NSD_FQDN_CAPACITY] = {0};
    memcpy(result, resolved, strlen(resolved));
    memcpy(output, result, sizeof result);
    return 0;
}
