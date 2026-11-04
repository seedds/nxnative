// A bounded IPv4 discovery sweep. Addresses are in network byte order.
#pragma once
#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

enum { DM_DISCOVERY_BATCH = 32, DM_DISCOVERY_TICK_MS = 8, DM_DISCOVERY_MAX_HOSTS = 4094 };
typedef struct {
    uint32_t local, first, last, target, cursor;
    unsigned count, automatic;
} dm_net_discovery_plan;
// Darwin can report a negative ARP entry as EHOSTDOWN. These are destination
// failures, not permission/interface failures; skip that neighbor in a sweep.
static inline int dm_net_discovery_unreachable(int error) {
    return error==EHOSTUNREACH||error==ENETUNREACH||error==EHOSTDOWN;
}

// This takes a validated adapter snapshot, not a guessed address range. /31,
// /32, noncontiguous masks and networks broader than /20 are not discoverable.
static inline int dm_net_discovery_subnet(dm_net_discovery_plan *p, uint32_t address, uint32_t mask) {
    memset(p, 0, sizeof *p);
    uint32_t a = ntohl(address), m = ntohl(mask), hosts = ~m;
    if (!m || (hosts & (hosts + 1)) || hosts < 3 || hosts > DM_DISCOVERY_MAX_HOSTS + 1u ||
        !a || a >= 0xe0000000u || !(a & hosts) || (a & hosts) == hosts) return -1;
    p->local = a; p->first = (a & m) + 1; p->last = (a | hosts) - 1;
    p->cursor = p->first; p->count = hosts - 2; p->automatic = 1;
    return 0;
}
static inline int dm_net_discovery_next(dm_net_discovery_plan *p, uint32_t *address) {
    if (!p->automatic) {
        if (p->cursor++) return 0;
        *address = p->target; return 1;
    }
    while (p->cursor <= p->last) {
        uint32_t a = p->cursor++;
        if (a != p->local) { *address = htonl(a); return 1; }
    }
    return 0;
}
static inline unsigned dm_net_discovery_duration(const dm_net_discovery_plan *p) {
    return p->automatic ? ((p->count + DM_DISCOVERY_BATCH - 1) / DM_DISCOVERY_BATCH) * DM_DISCOVERY_TICK_MS : 0;
}
