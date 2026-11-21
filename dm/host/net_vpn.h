// VPN play (DM_NET_VPN=1): which adapter is the VPN's, and the mask the game is told. No system calls, so t_net_discovery.c drives them with
// synthetic adapters; hle_net_backend.c (net_vpn_interface) feeds them the real ones. Addresses and masks are in network byte order.
#pragma once
#include <arpa/inet.h>
#include <net/if.h>
#include <stdint.h>

typedef struct { uint32_t address, mask; unsigned index, flags, type; } dm_net_vpn_candidate;   // type: the SIOCGIFFUNCTIONALTYPE answer

// A VPN adapter is up, IPv4 and not loopback or link-local (169.254/16), and its functional type is none of the physical links. iOS names VPN tunnels
// utunN, but names are not API (TN3179) and the type of a tunnel is not documented, so this excludes what is known not to be one rather than matching a VPN.
// Point-to-point is allowed: tunnels are.
static inline int dm_net_vpn_eligible(const dm_net_vpn_candidate *c) {
    uint32_t a = ntohl(c->address);
    if (!(c->flags & IFF_UP) || (c->flags & IFF_LOOPBACK) || !a || (a & 0xffff0000u) == 0xa9fe0000u) return 0;
    switch (c->type) {
    case IFRTYPE_FUNCTIONAL_LOOPBACK: case IFRTYPE_FUNCTIONAL_WIRED: case IFRTYPE_FUNCTIONAL_WIFI_INFRA: case IFRTYPE_FUNCTIONAL_WIFI_AWDL: case IFRTYPE_FUNCTIONAL_CELLULAR:
#ifdef IFRTYPE_FUNCTIONAL_INTCOPROC
    case IFRTYPE_FUNCTIONAL_INTCOPROC:
#endif
#ifdef IFRTYPE_FUNCTIONAL_COMPANIONLINK
    case IFRTYPE_FUNCTIONAL_COMPANIONLINK:
#endif
        return 0;
    }
    return 1;
}
enum { DM_NET_VPN_NONE = -1, DM_NET_VPN_SEVERAL = -2 };
// routed is the local address the system routes the host's address through (DM_NET_DISCOVERY_PEER), 0 when no host address is set or it has no route.
// With it, the adapter that holds it is the answer, and only if it is a VPN adapter: the route settles which tunnel is the VPN. Without it (the host, or a
// joiner that searches), exactly one VPN adapter must be up. Returns its index in list, DM_NET_VPN_NONE or DM_NET_VPN_SEVERAL; never a guess.
static inline int dm_net_vpn_pick(const dm_net_vpn_candidate *list, int n, uint32_t routed) {
    int found = DM_NET_VPN_NONE;
    for (int i = 0; i < n; i++) {
        if (!dm_net_vpn_eligible(&list[i])) continue;
        if (routed) { if (list[i].address == routed) return i; continue; }
        if (found >= 0) return DM_NET_VPN_SEVERAL;
        found = i;
    }
    return found;
}
// A VPN that gives each device a /31, a /32 or no mask (Tailscale, NetBird) still puts every player in one private block. The game is told that block:
// Tailscale's and NetBird's 100.64.0.0/10, or the RFC 1918 range the address is in. Each device works it out from its own address alone, so all agree, and
// the broadcast address the game derives from it is one net_is_discovery recognizes. A mask that already names a network (ZeroTier's /24) is kept.
static inline uint32_t dm_net_vpn_mask(uint32_t address, uint32_t mask) {
    uint32_t a = ntohl(address), m = ntohl(mask);
    if (m && m < 0xfffffffeu) return mask;
    if ((a & 0xffc00000u) == 0x64400000u) m = 0xffc00000u;
    else if ((a & 0xff000000u) == 0x0a000000u) m = 0xff000000u;
    else if ((a & 0xfff00000u) == 0xac100000u) m = 0xfff00000u;
    else if ((a & 0xffff0000u) == 0xc0a80000u) m = 0xffff0000u;
    else return mask;
    return htonl(m);
}
