// Networking and peripheral system services (out of scope: online play, local wireless, friends, telemetry, SSL) as seen through newer SDKs: all made "unavailable", the game takes its offline branch.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
#include "hle.h"


// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "telemetry report save (user); nothing is sent, reports success", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "telemetry report save; nothing is sent, reports success", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "telemetry init: the original connects to the prepo service (through sm), the host has no service process; no-op (Engage calls it explicitly)", Result, h_prepo_Init, void) { return RES_OK; }
// Add the session ID / connection status to a report: on hardware these first connect to the prepo service for the session (Super Mario 3D World + Bowser's Fury calls them entering the save select); the host adds nothing and reports success
HLE_FORCE("_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc", "telemetry session ID: no service connection, nothing added, reports success", Result, h_prepo_AddSess, void *rep, const char *key) { (void)rep; (void)key; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "telemetry internet connection status: no service connection, nothing added, reports success", Result, h_prepo_AddNet, void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "telemetry immediate upload; nothing is sent, reports success", Result, h_prepo_Tx, void) { return RES_OK; }

// ---------- ldn: local wireless. Without DM_LDN_PEER every call fails (a cut feature); with it, the station side of ldn_mitm's LAN protocol ----------
// DM_LDN_PEER=<IPv4 of a Switch running ldn_mitm> (env.txt): Scan sends ldn_mitm's Scan packet to that address (UDP 11452, a unicast needs no multicast entitlement on iOS) and collects the
// ScanResp; Connect opens its TCP 11452 and sends Connect, the host answers with SyncNetwork. The protocol is spacemeowx2/ldn_mitm's (lan_protocol.cpp, lan_discovery.cpp): a 12-byte header
// (magic 0x11451400, type, compressed flag, length, decompressed length), zero-run compression, NetworkInfo / NodeInfo as the SDK lays them out (0x480 / 0x40 bytes). Only the station role exists:
// CreateNetwork and the access point calls still fail. What the game then sends to the nodes' addresses is nn::socket's business (hle_net.c)
#include <mach/mach.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
void dm_guest_sleep_ns(int64_t ns);   // hle_thread.c: sleeps as a guest thread (gives the core back)
typedef struct { uint8_t raw[6]; } ld_mac;
typedef struct { uint8_t length; char raw[33]; } ld_ssid;
typedef struct { ld_mac bssid; ld_ssid ssid; int16_t channel; int8_t linkLevel; uint8_t networkType; uint32_t unk; } ld_common;
typedef struct { uint32_t ipv4; ld_mac mac; int8_t nodeId; int8_t isConnected; char userName[33]; uint8_t unk1; int16_t lcVersion; uint8_t unk2[16]; } ld_node;
typedef struct { uint8_t unkRandom[16]; uint16_t securityMode; uint8_t acceptPolicy; uint8_t unk1[3]; uint8_t nodeCountMax; uint8_t nodeCount; ld_node nodes[8]; uint16_t unk2; uint16_t advSize; uint8_t adv[384]; uint8_t unk3[148]; } ld_ldn;
typedef struct { uint64_t lcid; uint8_t unk1[2]; uint16_t sceneId; uint8_t unk2[4]; } ld_intent;
typedef struct { uint64_t high, low; } ld_session;
typedef struct { ld_intent intent; ld_session session; } ld_netid;
typedef struct { ld_netid id; ld_common common; ld_ldn ldn; } ld_netinfo;
typedef struct { ld_netid id; uint32_t networkType; ld_mac bssid; ld_ssid ssid; uint8_t unk[16]; uint32_t flag; } ld_filter;
typedef struct { uint16_t securityMode, passphraseSize; uint8_t passphrase[64]; } ld_security;
_Static_assert(sizeof(ld_netinfo) == 0x480, "NetworkInfo is 0x480 bytes");
_Static_assert(sizeof(ld_node) == 0x40, "NodeInfo is 0x40 bytes");
_Static_assert(sizeof(ld_filter) == 0x60, "ScanFilter is 0x60 bytes");
enum { LD_NONE, LD_INIT, LD_AP, LD_AP_CREATED, LD_STA, LD_STA_CONNECTED };   // nn::ldn::State
enum { LD_PKT_SCAN, LD_PKT_SCANRESP, LD_PKT_CONNECT, LD_PKT_SYNC };
enum { LD_PORT = 11452, LD_MAGIC = 0x11451400, LD_HDR = 12, LD_BUF = 2048, LD_INVALID_STATE = 0x40CB };   // Result(203, 32), what ldn_mitm answers in a wrong state
enum { LDF_LCID = 1, LDF_SESSION = 2, LDF_NETTYPE = 4, LDF_SSID = 16, LDF_SCENE = 32 };

static pthread_mutex_t g_ld_mu = PTHREAD_MUTEX_INITIALIZER;
// The first events of each kind are logged (a trace of what the game's network library does, for the next report); failures always, up to a limit
static _Atomic int g_bt_n[48], g_bt_fail;
#define BT(kind, lim, ...) do { if (atomic_fetch_add(&g_bt_n[kind], 1) < (lim)) LOG("[ldn] %llu ms: " FIRST_ARG(__VA_ARGS__), (unsigned long long)(now_ns() / 1000000) % 1000000 REST_ARGS(__VA_ARGS__)); } while (0)
#define FIRST_ARG(f, ...) f
#define REST_ARGS(f, ...) , ##__VA_ARGS__
// A guest backtrace at the first few calls of a kind (module + offset: the trace of where the game's network code runs from; see docs/agents on reading them)
static _Atomic int g_bs_n[8];
#define BSTACK(kind, lim, fp, ...) do { if (atomic_fetch_add(&g_bs_n[kind], 1) < (lim)) { LOG("[ldn] " __VA_ARGS__); dm_backtrace((uint64_t)(fp)); dm_stack_scan((uint64_t)__builtin_frame_address(0)); } } while (0)
void crash_off(const char *tag, uint64_t a);
// The game's code keeps no frame chain, so dm_backtrace finds nothing: this walks the words above the hook's frame and lists the ones that point just after a bl / blr in a game module
// (return addresses of the callers still on the stack, innermost first; stale ones may be among them, so read the list as candidates)
static void dm_stack_scan(uint64_t from) {
    fprintf(stderr, "[dm] stack scan:");
    for (int i = 0; i < 256; i++) {
        uint64_t v; vm_size_t got = 0;   // read through the kernel: past the top of the thread's stack it fails instead of faulting
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)(from + 8ull * i), sizeof v, (vm_address_t)&v, &got) != KERN_SUCCESS || got != sizeof v) break;
        module_t *m = (v & 3) ? NULL : module_of((void *)v);
        if (!m || v < (uint64_t)m->base + 4) continue;
        uint32_t ins; if (vm_read_overwrite(mach_task_self(), (vm_address_t)(v - 4), sizeof ins, (vm_address_t)&ins, &got) != KERN_SUCCESS) continue;
        if ((ins >> 26) == 0x25 || (ins & 0xfffffc1f) == 0xd63f0000) crash_off("", v);
    }
    fputc('\n', stderr);
}
static int g_ld_state, g_ld_udp = -1, g_ld_tcp = -1, g_ld_reason;
static unsigned g_ld_gen;                 // a TCP connection's number: its reader thread ends when this moves on
static void *g_ld_ev;                     // the state change event the game attached
static ld_netinfo g_ld_info;              // the network the station joined (SyncNetwork)
static ld_netinfo g_ld_scan[8]; static int g_ld_nscan;
static uint32_t g_ld_peer_n, g_ld_local_n, g_ld_mask_n;   // network byte order; 0 = not known
static int g_ld_peer_done, g_ld_localmode;

static uint32_t ld_peer(void) {
    if (!g_ld_peer_done) {
#ifndef DM_GAME_LDN_PEER
#define DM_GAME_LDN_PEER ""
#endif
        const char *s = getenv("DM_LDN_PEER"); struct in_addr a;
        if (!s || !*s) s = DM_GAME_LDN_PEER;   // the profile's ldn_peer (game.tsv)
        if (s && *s && inet_pton(AF_INET, s, &a) == 1) { g_ld_peer_n = a.s_addr; LOG("[ldn] local wireless over the network: ldn_mitm host %s, UDP/TCP %d", s, LD_PORT); }
        g_ld_peer_done = 1;
    }
    return g_ld_peer_n;
}
int dm_ldn_enabled(void) { return ld_peer() != 0; }
// The address and mask the game's own packets leave from: what the OS would use to reach the peer
static void ld_local(void) {
    if (g_ld_local_n || !ld_peer()) return;
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return;
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_port = htons(LD_PORT), .sin_addr.s_addr = g_ld_peer_n};
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) { socklen_t n = sizeof a; if (!getsockname(s, (struct sockaddr *)&a, &n)) g_ld_local_n = a.sin_addr.s_addr; }
    close(s);
    g_ld_mask_n = htonl(0xFFFFFF00u);
    struct ifaddrs *ifa, *p;
    if (g_ld_local_n && !getifaddrs(&ifa)) {
        for (p = ifa; p; p = p->ifa_next)
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && p->ifa_netmask && ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr == g_ld_local_n) { g_ld_mask_n = ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr; break; }
        freeifaddrs(ifa);
    }
    char b[16]; inet_ntop(AF_INET, &g_ld_local_n, b, sizeof b); LOG("[ldn] this device is %s", g_ld_local_n ? b : "(no route to the peer)");
}
// nifm answers for local play: a network exists once the game asked for the local network mode
int dm_nifm_available(void) { int r = dm_ldn_enabled() && g_ld_localmode && (ld_local(), g_ld_local_n != 0); BT(8, 12, "nifm: network available = %d (local network mode %d)", r, g_ld_localmode); return r; }
void dm_nifm_ipconfig(uint32_t *out5) { BT(9, 12, "nifm: IP configuration asked"); ld_local(); uint32_t v[5] = {g_ld_local_n, g_ld_mask_n, g_ld_peer_n, 0, 0}; for (int i = 0; i < 5; i++) out5[i] = v[i]; }

static void ld_signal(void) { if (g_ld_ev) hle_signal_system_event(g_ld_ev); }
// ldn_mitm's zero-run coding: a non-zero byte is itself, 0 is followed by how many more zeros there are
static int ld_compress(const uint8_t *in, size_t n, uint8_t *out, size_t *on) {
    size_t i = 0, o = 0, cap = *on;
    while (i < n) {
        uint8_t c = in[i++];
        if (c) { if (o >= cap) return -1; out[o++] = c; }
        else { unsigned cnt = 0; while (i < n && !in[i] && cnt < 255) { cnt++; i++; } if (o + 2 > cap) return -1; out[o++] = 0; out[o++] = (uint8_t)cnt; }
    }
    *on = o; return 0;
}
static int ld_decompress(const uint8_t *in, size_t n, uint8_t *out, size_t *on) {
    size_t i = 0, o = 0, cap = *on;
    while (i < n && o < cap) {
        uint8_t c = in[i++]; out[o++] = c;
        if (!c) { if (i == n) return -1; uint8_t cnt = in[i++]; for (unsigned k = 0; k < cnt && o < cap; k++) out[o++] = 0; }
    }
    *on = o; return i == n ? 0 : -1;
}
static int ld_send(int fd, int type, const void *data, size_t size, const struct sockaddr_in *to) {
    uint8_t buf[LD_BUF]; uint8_t body[LD_BUF]; size_t blen = size, comp = 0;
    if (size > LD_BUF - LD_HDR) return -1;
    if (size) { size_t on = size; if (!ld_compress(data, size, body, &on)) { comp = 1; blen = on; } else memcpy(body, data, size); }
    uint32_t magic = LD_MAGIC; memcpy(buf, &magic, 4); buf[4] = (uint8_t)type; buf[5] = (uint8_t)comp;
    uint16_t len = (uint16_t)blen, dlen = comp ? (uint16_t)size : 0; memcpy(buf + 6, &len, 2); memcpy(buf + 8, &dlen, 2); buf[10] = buf[11] = 0;
    memcpy(buf + LD_HDR, body, blen);
    if (to) return (int)sendto(fd, buf, LD_HDR + blen, 0, (const struct sockaddr *)to, sizeof *to);
    return (int)send(fd, buf, LD_HDR + blen, 0);
}
// One packet out of buf (n bytes at the front): its type, body (decompressed into out) and the bytes it used; 0 = more bytes needed, -1 = not a packet
static int ld_parse(const uint8_t *buf, size_t n, int *type, uint8_t *out, size_t *outn) {
    if (n < LD_HDR) return 0;
    uint32_t magic; memcpy(&magic, buf, 4); if (magic != (uint32_t)LD_MAGIC) return -1;
    uint16_t len, dlen; memcpy(&len, buf + 6, 2); memcpy(&dlen, buf + 8, 2);
    if (len > LD_BUF - LD_HDR) return -1;
    if (n < LD_HDR + (size_t)len) return 0;
    *type = buf[4];
    if (buf[5]) { size_t on = LD_BUF; if (ld_decompress(buf + LD_HDR, len, out, &on) || on != dlen) return -1; *outn = on; }
    else { memcpy(out, buf + LD_HDR, len); *outn = len; }
    return LD_HDR + len;
}
static void ld_close_tcp(void) {
    int fd = g_ld_tcp; g_ld_tcp = -1; g_ld_gen++;
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
}
// The reader of one TCP connection to the host: SyncNetwork packets carry the network as the host sees it (our node's number, the other stations)
static void *ld_reader(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg; int fd = g_ld_tcp;
    static _Thread_local uint8_t rb[LD_BUF * 2]; static _Thread_local uint8_t body[LD_BUF]; size_t rn = 0;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, 200);
        if (g_ld_gen != gen) return NULL;
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        ssize_t k = recv(fd, rb + rn, sizeof rb - rn, 0);
        if (k <= 0) break;
        rn += (size_t)k;
        for (;;) {
            int type = 0; size_t bn = 0; int used = ld_parse(rb, rn, &type, body, &bn);
            if (used < 0) { rn = 0; break; }
            if (used == 0) break;
            if (type == LD_PKT_SYNC && bn == sizeof(ld_netinfo)) {
                pthread_mutex_lock(&g_ld_mu);
                if (g_ld_gen == gen) { memcpy(&g_ld_info, body, bn); if (g_ld_state == LD_STA) g_ld_state = LD_STA_CONNECTED;
                    LOG("[ldn] SyncNetwork: %d of %d nodes, our node %d", g_ld_info.ldn.nodeCount, g_ld_info.ldn.nodeCountMax, g_ld_info.ldn.nodeCount ? g_ld_info.ldn.nodeCount - 1 : -1);
                    LOG("[ldn] network: security mode %u, accept policy %u, channel %d, type %u, ssid length %u, advertise %u bytes", g_ld_info.ldn.securityMode, g_ld_info.ldn.acceptPolicy, g_ld_info.common.channel, g_ld_info.common.networkType, g_ld_info.common.ssid.length, g_ld_info.ldn.advSize);
                    for (int i = 0; i < 8; i++) { const ld_node *n = &g_ld_info.ldn.nodes[i]; if (n->ipv4 || n->isConnected) { struct in_addr ia = {.s_addr = htonl(n->ipv4)};
                        LOG("[ldn]   node %d: id %d, connected %d, %s, mac %02x:%02x:%02x:%02x:%02x:%02x, name '%.32s', version %d", i, n->nodeId, n->isConnected, inet_ntoa(ia), n->mac.raw[0], n->mac.raw[1], n->mac.raw[2], n->mac.raw[3], n->mac.raw[4], n->mac.raw[5], n->userName, n->lcVersion); } } }
                pthread_mutex_unlock(&g_ld_mu);
                ld_signal();
            } else LOG("[ldn] unexpected packet type %d (%zu bytes) from the host", type, bn);
            memmove(rb, rb + used, rn - (size_t)used); rn -= (size_t)used;
        }
    }
    pthread_mutex_lock(&g_ld_mu);
    if (g_ld_gen == gen) { LOG("[ldn] the host closed the connection"); close(fd); g_ld_tcp = -1; g_ld_gen++; if (g_ld_state == LD_STA_CONNECTED) g_ld_state = LD_STA; }
    pthread_mutex_unlock(&g_ld_mu);
    ld_signal();
    return NULL;
}

HLE_FORCE("_ZN2nn3ldn10InitializeEv", "local wireless: ldn_mitm's LAN protocol with the Switch at DM_LDN_PEER, else initialization fails", Result, h_ldn_Init, void) { BT(16, 4, "Initialize called from %p", __builtin_return_address(0));
    if (!ld_peer()) return RES_FAIL;
    pthread_mutex_lock(&g_ld_mu);
    if (g_ld_udp < 0) {
        g_ld_udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_ld_udp >= 0) { int b = 1; setsockopt(g_ld_udp, SOL_SOCKET, SO_BROADCAST, &b, sizeof b); fcntl(g_ld_udp, F_SETFL, fcntl(g_ld_udp, F_GETFL) | O_NONBLOCK); }
    }
    Result r = g_ld_udp >= 0 ? RES_OK : RES_FAIL;
    if (r == RES_OK && g_ld_state == LD_NONE) g_ld_state = LD_INIT;
    pthread_mutex_unlock(&g_ld_mu);
    ld_local(); ld_signal();
    LOG("[ldn] Initialize: %s", r ? "failed (no socket)" : "ok");
    return r;
}
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "local wireless: closes the sockets", void, h_ldn_Fini, void) { BT(17, 4, "Finalize called from %p", __builtin_return_address(0));
    pthread_mutex_lock(&g_ld_mu); ld_close_tcp(); if (g_ld_udp >= 0) { close(g_ld_udp); g_ld_udp = -1; } g_ld_state = LD_NONE; pthread_mutex_unlock(&g_ld_mu);
    ld_signal();
}
HLE_FORCE("_ZN2nn3ldn8GetStateEv", "local wireless: the state of the LAN link", int, h_ldn_State, void) { BT(26, 6, "GetState called from %p", __builtin_return_address(0)); BT(6, 12, "GetState = %d", g_ld_state); return g_ld_state; }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "local wireless state event: fires when the LAN link changes state", void, h_ldn_Attach, void *ev) { hle_bind_system_event(ev, 1); g_ld_ev = ev; }
HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "local wireless: station role", Result, h_ldn_OpenSta, void) { BT(18, 4, "OpenStation called from %p", __builtin_return_address(0));
    if (g_ld_state == LD_NONE) return LD_INVALID_STATE;
    pthread_mutex_lock(&g_ld_mu); g_ld_reason = 0; ld_close_tcp(); g_ld_state = LD_STA; pthread_mutex_unlock(&g_ld_mu); ld_signal(); LOG("[ldn] OpenStation");
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "local wireless: leaves the station role", Result, h_ldn_CloseSta, void) { BT(19, 4, "CloseStation called from %p", __builtin_return_address(0));
    if (g_ld_state == LD_NONE) return LD_INVALID_STATE;
    pthread_mutex_lock(&g_ld_mu); ld_close_tcp(); g_ld_state = LD_INIT; pthread_mutex_unlock(&g_ld_mu); ld_signal();
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "local wireless: drops the TCP link to the host", Result, h_ldn_Disc, void) {
    BT(11, 12, "Disconnect called from %p", __builtin_return_address(0));
    BSTACK(4, 4, __builtin_frame_address(1), "Disconnect backtrace:");
    if (g_ld_state == LD_NONE) return LD_INVALID_STATE;
    pthread_mutex_lock(&g_ld_mu); ld_close_tcp(); g_ld_state = LD_STA; pthread_mutex_unlock(&g_ld_mu); ld_signal(); LOG("[ldn] Disconnect");
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "local wireless (station role only)", Result, h_ldn_OpenAp, void) { LOG("[ldn] OpenAccessPoint: not supported (station role only)"); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "local wireless (station role only)", Result, h_ldn_CloseAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "local wireless (station role only)", Result, h_ldn_Destroy, void) { return RES_FAIL; }
// Scan: UDP Scan to the peer, the ScanResp packets that come back in about a second (ldn_mitm waits a second as well), then the filter
static Result ld_scan(ld_netinfo *out, int *outCount, int bufCount, const ld_filter *f) {
    if (g_ld_udp < 0) return LD_INVALID_STATE;
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(LD_PORT), .sin_addr.s_addr = g_ld_peer_n};
    pthread_mutex_lock(&g_ld_mu); g_ld_nscan = 0; pthread_mutex_unlock(&g_ld_mu);
    for (int t = 0; t < 40; t++) {   // 40 × 25 ms; the request is sent again at 0.4 s and 0.7 s (UDP may drop it)
        if (t == 0 || t == 16 || t == 28) { if (ld_send(g_ld_udp, LD_PKT_SCAN, NULL, 0, &to) < 0) { LOG("[ldn] Scan: send failed (errno %d)", errno); if (!t) return RES_FAIL; } }
        for (;;) {
            uint8_t buf[LD_BUF], body[LD_BUF]; struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(g_ld_udp, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            int type = 0; size_t bn = 0;
            if (ld_parse(buf, (size_t)n, &type, body, &bn) > 0 && type == LD_PKT_SCANRESP && bn == sizeof(ld_netinfo)) {
                pthread_mutex_lock(&g_ld_mu);
                int dup = 0; for (int i = 0; i < g_ld_nscan; i++) if (!memcmp(&g_ld_scan[i].common.bssid, body + offsetof(ld_netinfo, common.bssid), 6)) { dup = 1; memcpy(&g_ld_scan[i], body, bn); }
                if (!dup && g_ld_nscan < 8) { memcpy(&g_ld_scan[g_ld_nscan++], body, bn); LOG("[ldn] Scan: found a network from %s", inet_ntoa(from.sin_addr)); }
                pthread_mutex_unlock(&g_ld_mu);
            }
        }
        dm_guest_sleep_ns(25 * 1000000);
    }
    int n = 0;
    pthread_mutex_lock(&g_ld_mu);
    for (int i = 0; i < g_ld_nscan && n < bufCount; i++) {
        const ld_netinfo *s = &g_ld_scan[i]; int ok = 1;
        if (f) {
            if (f->flag & LDF_LCID) ok &= f->id.intent.lcid == s->id.intent.lcid;
            if (f->flag & LDF_SESSION) ok &= f->id.session.high == s->id.session.high && f->id.session.low == s->id.session.low;
            if (f->flag & LDF_NETTYPE) ok &= f->networkType == s->common.networkType;
            if (f->flag & LDF_SSID) ok &= f->ssid.length == s->common.ssid.length && !memcmp(f->ssid.raw, s->common.ssid.raw, 33);
            if (f->flag & LDF_SCENE) ok &= f->id.intent.sceneId == s->id.intent.sceneId;
        }
        LOG("[ldn] Scan: network %d of the peer: local communication ID %016llx scene %u, %d nodes, advertise %u bytes%s", i, (unsigned long long)s->id.intent.lcid, s->id.intent.sceneId, s->ldn.nodeCount, s->ldn.advSize, ok ? "" : " (filtered out)");
        if (i == 0) { char hx[130] = ""; for (int k = 0; k < 48 && k < s->ldn.advSize; k++) snprintf(hx + k * 2, 3, "%02x", s->ldn.adv[k]); hx[0] = hx[0]; LOG("[ldn] Scan: advertise data starts %s; node 0 %08x '%.32s' version %d; security mode %u", hx, s->ldn.nodes[0].ipv4, s->ldn.nodes[0].userName, s->ldn.nodes[0].lcVersion, s->ldn.securityMode); }
        if (ok) out[n++] = *s;
    }
    pthread_mutex_unlock(&g_ld_mu);
    *outCount = n;
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan: ldn_mitm's Scan packet to DM_LDN_PEER", Result, h_ldn_Scan, void *out, int *n, int max, const void *f, int ch) { BT(20, 4, "Scan called from %p", __builtin_return_address(0)); BSTACK(6, 1, __builtin_frame_address(1), "Scan backtrace:");
    (void)ch; if (n) *n = 0; if (!ld_peer()) return RES_FAIL; if (!out || !n) return RES_FAIL;
    return ld_scan(out, n, max, f);
}
HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan (private form): the same as Scan", Result, h_ldn_ScanP, void *out, int *n, int max, const void *f, int ch) { BT(21, 4, "ScanPrivate called from %p", __builtin_return_address(0));
    (void)ch; if (n) *n = 0; if (!ld_peer()) return RES_FAIL; if (!out || !n) return RES_FAIL;
    return ld_scan(out, n, max, f);
}
HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "local wireless (station role only)", Result, h_ldn_Create,
          const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; LOG("[ldn] CreateNetwork: not supported (station role only)"); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "local wireless (station role only)",
          Result, h_ldn_CreateP, const void *a, const void *b, const void *c, const void *d, int n, const void *e) { (void)a; (void)b; (void)c; (void)d; (void)n; (void)e; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless: TCP to the host of the scanned network, then a Connect packet", Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) {
    (void)b; (void)o; const ld_netinfo *ni = a; const char *user = c; BT(27, 4, "Connect called from %p (version %d, option %d)", __builtin_return_address(0), v, o); BSTACK(7, 1, __builtin_frame_address(1), "Connect backtrace:");
    if (!ld_peer() || !ni) return RES_FAIL;
    if (g_ld_state != LD_STA && g_ld_state != LD_STA_CONNECTED) return LD_INVALID_STATE;
    if (!ni->ldn.nodeCount) return RES_FAIL;
    uint32_t host_n = ni->ldn.nodes[0].ipv4 ? htonl(ni->ldn.nodes[0].ipv4) : g_ld_peer_n;
    ld_local();
    pthread_mutex_lock(&g_ld_mu); ld_close_tcp(); pthread_mutex_unlock(&g_ld_mu);
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return RES_FAIL;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in to = {.sin_len = sizeof to, .sin_family = AF_INET, .sin_port = htons(LD_PORT), .sin_addr.s_addr = host_n};
    int rc = connect(fd, (struct sockaddr *)&to, sizeof to), err = rc ? errno : 0;
    for (int t = 0; rc && err == EINPROGRESS && t < 200; t++) {   // up to 5 s
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 0) > 0) { socklen_t l = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l); break; }
        dm_guest_sleep_ns(25 * 1000000);
    }
    if (err) { LOG("[ldn] Connect: TCP to %s failed (errno %d)", inet_ntoa(to.sin_addr), err); close(fd); return RES_FAIL; }
    ld_node me; memset(&me, 0, sizeof me);
    uint32_t lh = ntohl(g_ld_local_n);
    me.ipv4 = lh; me.mac.raw[0] = 0x02; memcpy(me.mac.raw + 2, &lh, 4); me.isConnected = 1; me.lcVersion = (int16_t)v;
    if (user) { strncpy(me.userName, user, 32); me.userName[32] = 0; }
    if (ld_send(fd, LD_PKT_CONNECT, &me, sizeof me, NULL) < 0) { LOG("[ldn] Connect: sending the Connect packet failed (errno %d)", errno); close(fd); return RES_FAIL; }
    pthread_mutex_lock(&g_ld_mu);
    g_ld_tcp = fd; unsigned gen = ++g_ld_gen; g_ld_state = LD_STA;
    pthread_mutex_unlock(&g_ld_mu);
    pthread_t th; if (pthread_create(&th, NULL, ld_reader, (void *)(uintptr_t)gen)) { LOG("[ldn] Connect: no reader thread"); return RES_FAIL; }
    pthread_detach(th);
    LOG("[ldn] Connect: joined the host %s as '%s' (communication version %d)", inet_ntoa(to.sin_addr), user ? user : "", v);
    for (int t = 0; t < 80 && g_ld_state != LD_STA_CONNECTED && g_ld_tcp >= 0; t++) dm_guest_sleep_ns(25 * 1000000);   // the host's SyncNetwork, up to 2 s
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless (ldn_mitm does not implement it either)",
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) { (void)a; (void)b; (void)c; (void)d; (void)v; (void)o; LOG("[ldn] ConnectPrivate: not supported"); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "local wireless: the network the station joined", Result, h_ldn_NetInfo, void *o) { BT(22, 6, "GetNetworkInfo called from %p", __builtin_return_address(0));
    if (!ld_peer() || !o) return RES_FAIL;
    pthread_mutex_lock(&g_ld_mu); int ok = g_ld_state == LD_STA_CONNECTED; if (ok) memcpy(o, &g_ld_info, sizeof g_ld_info); pthread_mutex_unlock(&g_ld_mu);
    BT(10, 12, "GetNetworkInfo = %s (state %d)", ok ? "ok" : "invalid state", g_ld_state);
    return ok ? RES_OK : LD_INVALID_STATE;
}
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "local wireless: this device's address on the LAN (host byte order, as ldn_mitm hands it out)", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) { BT(23, 6, "GetIpv4Address called from %p", __builtin_return_address(0));
    ld_local(); if (a) *a = ntohl(g_ld_local_n); if (m) *m = ntohl(g_ld_mask_n);
    BT(7, 12, "GetIpv4Address = %08x / %08x", ntohl(g_ld_local_n), ntohl(g_ld_mask_n));
    return ld_peer() && g_ld_local_n ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "local wireless: from the joined network", Result, h_ldn_SecPrm, void *o) { BT(24, 4, "GetSecurityParameter called from %p", __builtin_return_address(0));
    BT(11, 12, "GetSecurityParameter (state %d)", g_ld_state);
    if (!o || g_ld_state != LD_STA_CONNECTED) return ld_peer() ? LD_INVALID_STATE : RES_FAIL;
    pthread_mutex_lock(&g_ld_mu); memcpy(o, g_ld_info.ldn.unkRandom, 16); memcpy((uint8_t *)o + 16, &g_ld_info.id.session, 16); pthread_mutex_unlock(&g_ld_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", "local wireless: from the joined network", Result, h_ldn_NetCfg, void *o) { BT(25, 4, "GetNetworkConfig called from %p", __builtin_return_address(0));
    BT(11, 12, "GetNetworkConfig (state %d)", g_ld_state);
    if (!o || g_ld_state != LD_STA_CONNECTED) return ld_peer() ? LD_INVALID_STATE : RES_FAIL;
    pthread_mutex_lock(&g_ld_mu);
    memset(o, 0, 32); memcpy(o, &g_ld_info.id.intent, 16); uint16_t ch = (uint16_t)g_ld_info.common.channel; memcpy((uint8_t *)o + 16, &ch, 2);
    ((uint8_t *)o)[18] = g_ld_info.ldn.nodeCountMax; memcpy((uint8_t *)o + 20, &g_ld_info.ldn.nodes[0].lcVersion, 2);
    pthread_mutex_unlock(&g_ld_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "local wireless: the reason of the last disconnect (none)", int, h_ldn_DiscReason, void) { BT(11, 12, "GetDisconnectReason = %d", g_ld_reason); return g_ld_reason; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "local wireless: nothing to set for a station (ldn_mitm accepts it too)", Result, h_ldn_Policy, int p) { BT(11, 12, "SetStationAcceptPolicy(%d)", p); return ld_peer() ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "local wireless (station role only)", Result, h_ldn_Adv, const void *d, uint64_t n) { (void)d; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "local wireless (station role only)", Result, h_ldn_Filter, uint64_t mac) { (void)mac; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "local wireless (station role only)", Result, h_ldn_Reject, uint32_t a) { (void)a; return RES_FAIL; }

// ---------- nn::socket on the host's BSD sockets (only with DM_LDN_PEER / ldn_peer: the game's own traffic to the nodes of the LAN link; offline the calls below fail as before) ----------
// Horizon's bsd service speaks FreeBSD: the same sockaddr layout (length byte first), AF_INET / SOCK_* / SOL_SOCKET / SO_* / MSG_* / POLL* values and errno numbers as Apple's. A blocking call waits
// by polling with guest sleeps (it must not hold a guest core), and the descriptors are the host's own
static __thread int t_bsderr; static __thread void *t_bsdlr, *t_bsdfp;
void dm_bsd_note_lr(void *lr, void *fp) { t_bsdlr = lr; t_bsdfp = fp; }
static const char *bt_addr(const void *a, char *buf, size_t n) {
    const struct sockaddr_in *in = a; if (!in || in->sin_family != AF_INET) { snprintf(buf, n, "-"); return buf; }
    char ip[16]; inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip); snprintf(buf, n, "%s:%u", ip, ntohs(in->sin_port)); return buf;
}
// Horizon's bsd service reports Linux errno numbers (EAGAIN is 11, EINPROGRESS 115, ECONNREFUSED 111: Ryujinx's LinuxError), Apple's are FreeBSD's (35, 36, 61). A game that treats "nothing there
// yet" as retry, and anything else as a failure, needs the right number (Animal Crossing's Pia: one recvfrom answered 35 was a failed matchmaking, 2618-0006)
static int bsd_to_horizon(int e) {
    switch (e) {
    case EAGAIN: return 11; case EINPROGRESS: return 115; case EALREADY: return 114; case ENOTSOCK: return 88; case EDESTADDRREQ: return 89; case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91; case ENOPROTOOPT: return 92; case EPROTONOSUPPORT: return 93; case EOPNOTSUPP: return 95; case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98; case EADDRNOTAVAIL: return 99; case ENETDOWN: return 100; case ENETUNREACH: return 101; case ENETRESET: return 102;
    case ECONNABORTED: return 103; case ECONNRESET: return 104; case ENOBUFS: return 105; case EISCONN: return 106; case ENOTCONN: return 107;
    case ESHUTDOWN: return 108; case ETIMEDOUT: return 110; case ECONNREFUSED: return 111; case EHOSTDOWN: return 112; case EHOSTUNREACH: return 113;
    default: return e;
    }
}
int dm_bsd_errno(void) { return bsd_to_horizon(t_bsderr); }
struct bsd_stat { uint64_t t0; unsigned sent, recv, eagain; };
static struct bsd_stat g_bst[64];
static int bsd_fail(void) { t_bsderr = errno; if (atomic_fetch_add(&g_bt_fail, 1) < 60) LOG("[ldn] socket call failed: errno %d (%s)", errno, strerror(errno)); return -1; }
static int bsd_nonblocking(int fd) { int f = fcntl(fd, F_GETFL); return f >= 0 && (f & O_NONBLOCK); }
static int bsd_wait(int fd, short ev, int ms) {   // 1: ready, 0: timed out (ms < 0: never); waits as a guest thread does
    for (int left = ms;; ) {
        struct pollfd p = {.fd = fd, .events = ev};
        int r = poll(&p, 1, 0); if (r) return r < 0 ? -1 : 1;
        if (!left) return 0;
        dm_guest_sleep_ns(1000000); if (left > 0) left--;
    }
}
static int bsd_msgflags(int f) { return f & (0x1 | 0x2 | 0x4 | 0x40 | 0x80); }   // OOB, PEEK, DONTROUTE, WAITALL, DONTWAIT: the same on both; MSG_NOSIGNAL (0x20000) has no counterpart (SO_NOSIGPIPE is set instead)
int dm_bsd_socket(int af, int type, int proto) {
    int fd = socket(af, type, proto); if (fd < 0) return bsd_fail();
    int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);   // a send to a closed peer is an error, not a signal
    BT(0, 40, "socket(%d, %d, %d) = %d (called from %p)", af, type, proto, fd, t_bsdlr);
    BSTACK(0, 2, t_bsdfp, "socket() backtrace:");
    if (fd < 64) g_bst[fd] = (struct bsd_stat){.t0 = now_ns()};
    return fd;
}
int dm_bsd_close(int fd) { BSTACK(1, 4, t_bsdfp, "close(%d) backtrace:", fd); if (fd >= 0 && fd < 64 && g_bst[fd].t0) LOG("[ldn] close(%d) after %llu ms: %u datagrams sent, %u received, %u empty reads", fd, (unsigned long long)((now_ns() - g_bst[fd].t0) / 1000000), g_bst[fd].sent, g_bst[fd].recv, g_bst[fd].eagain); BT(5, 40, "close(%d) called from %p", fd, t_bsdlr); return close(fd) ? bsd_fail() : 0; }
int dm_bsd_shutdown(int fd, int how) { return shutdown(fd, how) ? bsd_fail() : 0; }
int dm_bsd_bind(int fd, const void *a, unsigned n) { char b[32]; int r = bind(fd, a, n); BT(1, 40, "bind(%d, %s) = %d%s%s", fd, bt_addr(a, b, sizeof b), r, r ? " errno " : "", r ? strerror(errno) : ""); return r ? bsd_fail() : 0; }
int dm_bsd_listen(int fd, int backlog) { BT(1, 40, "listen(%d, %d)", fd, backlog); return listen(fd, backlog) ? bsd_fail() : 0; }
int dm_bsd_connect(int fd, const void *a, unsigned n) {
    int blocking = !bsd_nonblocking(fd);
    if (blocking) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int rc = connect(fd, a, n), err = rc ? errno : 0;
    if (blocking) {
        if (err == EINPROGRESS) { bsd_wait(fd, POLLOUT, 10000); socklen_t l = sizeof err; err = 0; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l); }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    }
    { char b[32]; BT(2, 40, "connect(%d, %s) = %s", fd, bt_addr(a, b, sizeof b), err ? strerror(err) : "ok"); }
    if (err) { errno = err; return bsd_fail(); }
    return 0;
}
int dm_bsd_accept(int fd, void *a, unsigned *n) {
    if (!bsd_nonblocking(fd)) bsd_wait(fd, POLLIN, -1);
    socklen_t l = n ? *n : 0; int c = accept(fd, a, n ? &l : NULL); if (c < 0) return bsd_fail();
    if (n) *n = l;
    int on = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    return c;
}
static const char *bt_hex(const void *p, size_t n, char *out, size_t cap) {   // the first bytes of a datagram, for the trace
    const uint8_t *b = p; size_t k = 0; for (; k < n && 2 * k + 3 <= cap; k++) snprintf(out + 2 * k, 3, "%02x", b[k]);
    if (!k) out[0] = 0; return out;
}
long dm_bsd_recvfrom(int fd, void *b, unsigned long n, int flags, void *a, unsigned *al) {
    flags = bsd_msgflags(flags);
    if (!bsd_nonblocking(fd) && !(flags & MSG_DONTWAIT)) bsd_wait(fd, POLLIN, -1);
    socklen_t l = 0; ssize_t r; struct sockaddr_in src; memset(&src, 0, sizeof src);
    for (;;) {   // the datagrams this device sent to the subnet's broadcast address come back to its own socket (BSD loops a broadcast back; the host game's stack on a Switch does not
        // show it to the sender): dropped here, so Pia never reads its own packet as if a node had sent it
        socklen_t sl = sizeof src;
        r = recvfrom(fd, b, n, flags | MSG_DONTWAIT, (struct sockaddr *)&src, &sl);
        if (r < 0 || !g_ld_local_n) break;
        if (src.sin_family != AF_INET || src.sin_addr.s_addr != g_ld_local_n) break;
        char ab[32]; BT(15, 10, "dropped %zd bytes this device sent to itself (%s)", r, bt_addr(&src, ab, sizeof ab));
        if (flags & MSG_PEEK) { char dump[2048]; recvfrom(fd, dump, sizeof dump, MSG_DONTWAIT, NULL, NULL); }
    }
    if (r >= 0 && a && al) { l = sizeof src; unsigned cp = *al < l ? *al : l; memcpy(a, &src, cp); }
    if (r < 0) { if (errno != EAGAIN) return bsd_fail(); if (fd < 64) g_bst[fd].eagain++; BT(13, 20, "recvfrom(%d): nothing there yet (EAGAIN)", fd); t_bsderr = errno; return -1; }
    if (al) *al = l;
    if (fd >= 0 && fd < 64) g_bst[fd].recv++;
    BSTACK(3, 3, t_bsdfp, "recvfrom(%d) = %zd bytes, backtrace:", fd, r);
    { char ab[32], hx[420]; BT(3, 40, "recvfrom(%d) = %zd bytes from %s: %s", fd, r, bt_addr(a, ab, sizeof ab), bt_hex(b, (size_t)r < 200 ? (size_t)r : 200, hx, sizeof hx)); }
    return r;
}
// A datagram for the subnet's broadcast address also goes, as a unicast, to every other node of the LAN link: iOS may keep broadcast frames off the air without the multicast entitlement
// (the sender still gets its own copy back, and nothing leaves the device), while a unicast to a node's address is a plain local-network packet
static void ld_broadcast_copies(int fd, const void *b, size_t n, int flags, const void *a) {
    const struct sockaddr_in *to = a; if (to->sin_family != AF_INET || g_ld_state != LD_STA_CONNECTED) return;
    uint32_t dst = ntohl(to->sin_addr.s_addr), bc = ntohl(g_ld_local_n | ~g_ld_mask_n);
    if (dst != bc && dst != 0xFFFFFFFFu) return;
    uint32_t ips[8]; int k = 0;
    pthread_mutex_lock(&g_ld_mu);
    for (int i = 0; i < 8; i++) { const ld_node *nd = &g_ld_info.ldn.nodes[i]; if (nd->isConnected && nd->ipv4 && htonl(nd->ipv4) != g_ld_local_n) ips[k++] = htonl(nd->ipv4); }
    pthread_mutex_unlock(&g_ld_mu);
    for (int i = 0; i < k; i++) {
        struct sockaddr_in c = *to; c.sin_addr.s_addr = ips[i];
        ssize_t r = sendto(fd, b, n, flags | MSG_DONTWAIT, (struct sockaddr *)&c, sizeof c);
        char ab[32]; BT(14, 20, "broadcast of %zu bytes also sent to %s = %zd%s%s", n, bt_addr(&c, ab, sizeof ab), r, r < 0 ? " errno " : "", r < 0 ? strerror(errno) : "");
    }
}
long dm_bsd_sendto(int fd, const void *b, unsigned long n, int flags, const void *a, unsigned al) {
    flags = bsd_msgflags(flags);
    if (!bsd_nonblocking(fd) && !(flags & MSG_DONTWAIT)) bsd_wait(fd, POLLOUT, -1);
    ssize_t r = a ? sendto(fd, b, n, flags | MSG_DONTWAIT, a, al) : send(fd, b, n, flags | MSG_DONTWAIT);
    { char bb[32], hx[420]; BT(4, 40, "sendto(%d, %lu bytes, %s) = %zd%s%s from %p: %s", fd, n, bt_addr(a, bb, sizeof bb), r, r < 0 ? " errno " : "", r < 0 ? strerror(errno) : "", t_bsdlr, bt_hex(b, n < 200 ? n : 200, hx, sizeof hx)); }
    if (r >= 0 && fd >= 0 && fd < 64) g_bst[fd].sent++;
    BSTACK(2, 4, t_bsdfp, "sendto(%d, %lu bytes) backtrace:", fd, n);
    if (r >= 0 && a) ld_broadcast_copies(fd, b, n, flags, a);
    return r < 0 ? bsd_fail() : r;
}
int dm_bsd_getsockname(int fd, void *a, unsigned *n) { socklen_t l = n ? *n : 0; int r = getsockname(fd, a, &l); { char b[32]; BT(1, 40, "getsockname(%d) = %d %s", fd, r, r ? "-" : bt_addr(a, b, sizeof b)); } if (r) return bsd_fail(); if (n) *n = l; return 0; }
int dm_bsd_getsockopt(int fd, int lvl, int opt, void *v, unsigned *n) { socklen_t l = n ? *n : 0; int r = getsockopt(fd, lvl, opt, v, &l); if (r) return bsd_fail(); if (lvl == 0xffff && opt == 0x1007 && l >= 4) { int e; memcpy(&e, v, 4); e = bsd_to_horizon(e); memcpy(v, &e, 4); } if (n) *n = l; return 0; }
int dm_bsd_setsockopt(int fd, int lvl, int opt, const void *v, unsigned n) {
    if (!setsockopt(fd, lvl, opt, v, n)) { BT(1, 40, "setsockopt(%d, level %#x, option %#x, %u bytes) ok", fd, lvl, opt, n); return 0; }
    int e = errno; LOG("[ldn] setsockopt level %#x option %#x failed (errno %d): reported as done", lvl, opt, e);
    return e == EINVAL || e == ENOPROTOOPT ? 0 : bsd_fail();   // an option this OS does not have must not stop the game's network setup
}
int dm_bsd_fcntl(int fd, int cmd, long arg) {
    BT(1, 40, "fcntl(%d, %d, %#lx)", fd, cmd, arg);
    if (cmd == 4) { int f = fcntl(fd, F_GETFL); if (f < 0) return bsd_fail(); f = (arg & (0x4 | 0x800)) ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK); return fcntl(fd, F_SETFL, f) ? bsd_fail() : 0; }   // O_NONBLOCK is 0x4 here and 0x800 in some SDK headers
    int r = fcntl(fd, cmd, (int)arg); return r < 0 ? bsd_fail() : r;
}
int dm_bsd_poll(void *fds, uint64_t n, int ms) {
    { const struct pollfd *p0 = fds; BT(12, 20, "poll(%lu fds, first %d events %#x, timeout %d ms)", (unsigned long)n, n ? p0[0].fd : -1, n ? p0[0].events : 0, ms); }
    for (int left = ms;; ) {
        int r = poll(fds, (nfds_t)n, 0); if (r) { BT(12, 40, "poll = %d%s", r, r > 0 && n ? " (first fd ready)" : ""); return r < 0 ? bsd_fail() : r; }
        if (!left) return 0;
        dm_guest_sleep_ns(1000000); if (left > 0) left--;
    }
}

// ---------- socket: offline. Initialization succeeds (the network library's global init depends on it), every later operation fails with errno = ENETDOWN ----------
enum { SK_ENETDOWN = 50, SK_EAI_FAIL = 4 };
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "pairs with Initialize (cut)", Result, h_sk_Fini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "socket error code kept by the host; offline ENETDOWN", int, h_sk_Errno, void) { return dm_ldn_enabled() ? dm_bsd_errno() : SK_ENETDOWN; }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "offline (cut): -1", int, h_sk_Socket3i, int a, int b, int c) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_socket(a, b, c); } (void)a; (void)b; (void)c; return -1; }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "offline (cut)", int, h_sk_Shutdown, int s, int h) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_shutdown(s, h); } (void)s; (void)h; return -1; }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "offline (cut)", int, h_sk_Bind, int s, const void *a, unsigned n) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_bind(s, a, n); } (void)s; (void)a; (void)n; return -1; }
int dm_sk_poll(void *fds, uint64_t n, int ms);   // hle_net.c: nothing to wait on → the timeout passes, 0; else -1
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv);
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "offline: waits out the timeout when there is nothing to wait on, else fails", int, h_sk_Poll, void *p, uint64_t n, int t) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_poll(p, n, t); } return dm_sk_poll(p, n, t); }
// SDK 4's Select with plain fd_set / timeval (Dark Souls Remastered)
HLE_FORCE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", "offline: waits out the timeout when there is nothing to wait on, else fails", int, h_sk_Select2, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "offline (cut)", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_sendto(s, b, n, f, a, al); } (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "offline (cut)", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_sendto(s, b, n, f, a, al); } (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "offline (cut)", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_recvfrom(s, b, n, f, a, al); } (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "resolver error text", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
// SDK 11's overloads with plain sockaddr / int arguments (Big Brain Academy: Brain vs. Brain, through Pia)
HLE_FORCE("_ZN2nn6socket7ConnectEiPK8sockaddrj", "offline (cut)", int, h_sk_Connect2, int s, const void *a, unsigned n) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_connect(s, a, n); } (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket6AcceptEiP8sockaddrPj", "offline (cut)", int, h_sk_Accept2, int s, void *a, unsigned *n) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_accept(s, a, n); } (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", "offline (cut)", int, h_sk_GetSockName2, int s, void *a, unsigned *n) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_getsockname(s, a, n); } (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiiiPvPj", "offline (cut)", int, h_sk_GetSockOpt2, int s, int l, int o, void *v, unsigned *n) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_getsockopt(s, l, o, v, n); } (void)s; (void)l; (void)o; (void)v; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiiiPKvj", "offline (cut)", int, h_sk_SetSockOpt2, int s, int l, int o, const void *v, unsigned n) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_setsockopt(s, l, o, v, n); } (void)s; (void)l; (void)o; (void)v; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket5FcntlEiiz", "offline (cut)", int, h_sk_Fcntl2, int s, int c, long arg) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_fcntl(s, c, arg); } (void)s; (void)c; (void)arg; return -1; }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmi", "offline (cut)", long, h_sk_Recv2, int s, void *b, unsigned long n, int f) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_recvfrom(s, b, n, f, 0, 0); } (void)s; (void)b; (void)n; (void)f; return -1; }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmi", "offline (cut)", long, h_sk_Send2, int s, const void *b, unsigned long n, int f) { if (dm_ldn_enabled()) { dm_bsd_note_lr(__builtin_return_address(0), __builtin_frame_address(1)); return dm_bsd_sendto(s, b, n, f, 0, 0); } (void)s; (void)b; (void)n; (void)f; return -1; }

// ---------- irsensor: the Joy-Con IR camera (Big Brain Academy: Brain vs. Brain's input layer makes one per controller). The host has none: every camera is unconnected,
// processors cannot run and report no states. The default-config getters and MakeRect only fill the game's structs and keep running in the SDK ----------
enum { IRS_UNCONNECTED = 2 };   // IrCameraStatus: 0 available, 1 unsupported, 2 unconnected (the game takes 0 as "available", main+0x497fd4)
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraHandleERKj", "IR camera handle of a controller (player number in the low byte)", uint32_t, h_irs_Handle, const uint32_t *npad) { return *npad & 0xff; }
HLE_FORCE("_ZN2nn8irsensor10InitializeERKNS0_14IrCameraHandleE", "IR camera (cut): nothing to initialize", void, h_irs_Init, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor8FinalizeERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Fini, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraStatusERKNS0_14IrCameraHandleE", "IR camera (cut): unconnected", int, h_irs_Status, const void *h) { (void)h; return IRS_UNCONNECTED; }
HLE_FORCE("_ZN2nn8irsensor18StopImageProcessorERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Stop, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor18RunMomentProcessorERKNS0_14IrCameraHandleERKNS0_21MomentProcessorConfigE", "IR camera (cut)", Result, h_irs_RunMoment, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor22RunClusteringProcessorERKNS0_14IrCameraHandleERKNS0_25ClusteringProcessorConfigE", "IR camera (cut)", Result, h_irs_RunCluster, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor15RunHandAnalysisERKNS0_14IrCameraHandleERKNS0_18HandAnalysisConfigE", "IR camera (cut)", Result, h_irs_RunHand, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_28ImageTransferProcessorConfigEPvm", "IR camera (cut)", Result, h_irs_RunImage,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_30ImageTransferProcessorExConfigEPvm", "IR camera (cut)", Result, h_irs_RunImageEx,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor24GetMomentProcessorStatesEPNS0_20MomentProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Moment,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor28GetClusteringProcessorStatesEPNS0_24ClusteringProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Cluster,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25GetHandAnalysisImageStateEPNS0_22HandAnalysisImageStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandImage,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetHandAnalysisSilhouetteStateEPNS0_27HandAnalysisSilhouetteStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandSil,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetImageTransferProcessorStateEPNS0_27ImageTransferProcessorStateEPvmRKNS0_14IrCameraHandleE", "IR camera (cut)", Result, h_irs_ImageState,
          void *s, void *b, size_t n, const void *h) { (void)s; (void)b; (void)n; (void)h; return RES_FAIL; }

// ---------- ssl: offline, creating a context fails ----------
HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL (cut)", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL (cut)", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL (cut)", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL (cut)", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

// ---------- nifm / nsd: offline ----------
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "the local network mode switch: it makes the LAN link of local wireless (DM_LDN_PEER) the network, ignored offline", void, h_nifm_LocalMode, unsigned on) { g_ld_localmode = on & 0xff; }
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "offline: the network request is not submitted (returns at once, IsNetworkAvailable is always false)", void, h_nifm_SubmitWait, void) {}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "the IP configuration of the LAN link of local wireless (DM_LDN_PEER); offline none", Result, h_nifm_IpCfg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}, v[5] = {0}; if (dm_nifm_available()) dm_nifm_ipconfig(v);
    for (int i = 0; i < 5; i++) if (p[i]) *p[i] = v[i];
    return v[0] ? RES_OK : RES_FAIL;
}
// SDK 10's functions of the same name take socket::InAddr / in_addr, exactly the other way round from the generation above and in hle_net.c (Super Mario 3D World + Bowser's Fury)
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "the IP configuration of the LAN link of local wireless (DM_LDN_PEER); offline none (SDK 10 overload)", Result, h_nifm_IpCfg10, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}, v[5] = {0}; if (dm_nifm_available()) dm_nifm_ipconfig(v);
    for (int i = 0; i < 5; i++) if (p[i]) *p[i] = v[i];
    return v[0] ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "this device's address on the LAN link of local wireless (DM_LDN_PEER); offline none (SDK 10 overload)", Result, h_nifm_GetIp10, uint32_t *a) { uint32_t v[5] = {0}; if (dm_nifm_available()) dm_nifm_ipconfig(v); if (a) *a = v[0]; return v[0] ? RES_OK : RES_FAIL; }
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "server environment name: retail lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }

// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "friends library allocator; no session is created, ignored", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "friend list is empty (cut)", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "friend list unavailable (cut)", Result, h_fr_ListAvail, uint8_t *out, const void *uid) { (void)uid; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "friend list (cut): the request fails at once", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "async context: always done", Result, h_fr_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "play history is not kept; accepted with local wireless over the network (DM_LDN_PEER), else not registered (cut)",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; if (dm_ldn_enabled()) { LOG("[ldn] friends: play history entry accepted"); return RES_OK; } return RES_FAIL; }

// ---------- ngc: newer SDK overloads (with bool). Always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "profanity filter init; the host has no word list", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "profanity check: no profanity in anything", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words; for (uint64_t i = 0; out && i < n; i++) out[i] = 0; return RES_OK;
}
// The China-region checks (Cruis'n Blast's name entry after a race) go to the ngct:u service through nn::ngc::t::Match, which the host does not serve
HLE_FORCE("_ZN2nn3ngc47CheckProfanityWordsInTextForPlatformRegionChinaEPbPKc", "China-region profanity check (ngct:u): no profanity", Result, h_ngc_CheckCn, uint8_t *out, const char *text) { (void)text; if (out) *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc46MaskProfanityWordsInTextForPlatformRegionChinaEPc", "China-region profanity masking (ngct:u): nothing is masked", Result, h_ngc_MaskCn, char *text) { (void)text; return RES_OK; }

// ---------- err: error display applet. Only logged, no UI is shown ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "error display applet; the host only logs it", void, h_err_ShowCode, uint64_t code) { DM_APPLET("error"); LOG("game asks to show an error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "error display applet; the host only logs it", void, h_err_ShowVar, const void *v) { void *lr = __builtin_return_address(0); BSTACK(5, 4, __builtin_frame_address(1), "ShowError backtrace:"); DM_APPLET("error"); const uint32_t *w = v; LOG("game asks to show an error from %p (ErrorResultVariant): %08x %08x %08x %08x %08x %08x %08x %08x", lr, w ? w[0] : 0, w ? w[1] : 0, w ? w[2] : 0, w ? w[3] : 0, w ? w[4] : 0, w ? w[5] : 0, w ? w[6] : 0, w ? w[7] : 0); }
HLE_FORCE("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", "add-on content version mismatch error applet; the host only logs it", void, h_err_AocVer, void) { DM_APPLET("error"); LOG("game asks to show an error: add-on content version mismatch"); }
HLE_FORCE("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "game version mismatch error applet; the host only logs it", void, h_err_AppVer, void) { DM_APPLET("error"); LOG("game asks to show an error: game version mismatch"); }
// Pokémon Legends: Z-A (SDK 19): the application's own error screen (an error code number and two messages in the argument) goes through the error applet as well
HLE_FORCE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet; the host only logs it", void, h_err_ShowApp, const void *arg) { DM_APPLET("error"); (void)arg; LOG("game asks to show an application error (ApplicationErrorArg)"); }
// Pokémon Sword (SDK 7.3.2): the error applet's network service form, after a failed online call
HLE_FORCE("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", "network service error applet; the host only logs it", void, h_err_ShowNet, uint64_t code) { DM_APPLET("error"); LOG("game asks to show a network service error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }

// ---------- pdm: the system's play log ----------
// Pokémon Sword asks at boot how often this user has launched two other games (main+0xc88074, one ID each call, 0x18 bytes {application ID, play time, launch count} per entry; the result is
// not read, a launch count of at least 1 is): the host keeps no play log, so every one was never played. The SDK's own call opens the pdm service (null read at sdk+0x336ccc)
HLE_FORCE("_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsERKNS_7account3UidEPKNS_13ApplicationIdEi", "play statistics of other games: never played (the host keeps no play log)",
          Result, h_pdm_QueryStats, uint64_t *out, const void *uid, const uint64_t *ids, int n) {
    (void)uid; for (int i = 0; out && ids && i < n; i++) { out[3 * i] = ids[i]; out[3 * i + 1] = 0; out[3 * i + 2] = 0; }
    return RES_OK;
}

// ---------- web: web page applet (online lobby). Unavailable ----------
HLE_FORCE("_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", "online lobby web applet (cut)", Result, h_web_Lobby, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowLobbyPage (cut)"); return RES_FAIL; }
// Tears of the Kingdom imports the general web page applet too (the game constructs the return value itself, so it stays as constructed)
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web page applet (cut)", Result, h_web_Page, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowWebPage (cut)"); return RES_FAIL; }
// Octopath Traveler 0 imports the offline HTML page applet too (pages shipped in the game's HTML document NCA, which the host does not extract)
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "offline HTML page applet (cut)", Result, h_web_Offline, void *rv, const void *arg) {
    (void)rv; (void)arg; LOG("web::ShowOfflineHtmlPage (cut)"); return RES_FAIL;
}

// ---------- C interface (nnsocket* / nnssl*): offline as well. The arguments are all in registers and the host does not look at them ----------
HLE_FORCE("nnsocketAccept", "offline (cut)", int, h_c_sk_Accept, void) { return -1; }
HLE_FORCE("nnsocketListen", "offline (cut)", int, h_c_sk_Listen, void) { return -1; }
HLE_FORCE("nnsocketRecvMsg", "offline (cut)", int, h_c_sk_RecvMsg, void) { return -1; }
HLE_FORCE("nnsocketSendMsg", "offline (cut)", int, h_c_sk_SendMsg, void) { return -1; }
HLE_FORCE("nnsocketShutdown", "offline (cut)", int, h_c_sk_Shutdown, void) { return -1; }
HLE_FORCE("nnsocketGetAddrInfo", "name resolution (cut): EAI_FAIL", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketFreeAddrInfo", "pairs with GetAddrInfo (cut)", void, h_c_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("nnsocketGAIStrError", "resolver error text", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }

// ---------- bcat: news / data delivered from the server (Super Mario Odyssey). Initialize talks to the bcat service (an SDK abort without it); mounting the delivery cache fails,
// which the game treats as "nothing delivered" (main+0x6c9258: a failure skips the read unless the caller asked for an error dialog, which only logs here). Syncing needs the network, which nifm reports unavailable ----------
HLE_FORCE("_ZN2nn4bcat10InitializeEv", "data delivery init (connects to the bcat service); the host has none, no-op", void, h_bcat_Init, void) {}
HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "mount delivery cache: no delivery data, fails", Result, h_bcat_Mount, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "unmount delivery cache", void, h_bcat_Unmount, void) {}

// ---------- album: the capture album (Monster Hunter Rise saves its own photo-mode pictures there). Initialize connects to the capture service (capsrv, an abort in sm::Initialize at the start of
// nnMain, main+0x6bc5478); the host has no album, so saving fails and showing it does nothing. GetImageWidth / GetImageHeight only compute and keep running in the SDK ----------
HLE_FORCE("_ZN2nn5album10InitializeEv", "capture album init (capsrv); the host has no album, no-op", void, h_album_Init, void) {}
HLE_FORCE("_ZN2nn5album8FinalizeEv", "capture album", void, h_album_Fini, void) {}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save,
          const void *p, size_t n, int size, int orient, int report) { (void)p; (void)n; (void)size; (void)orient; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
// The SDK 7 overload, without an orientation (DRAGON QUEST XI S)
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save7,
          const void *p, size_t n, int size, int report) { (void)p; (void)n; (void)size; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_Share,
          const void *p, size_t n, int size, int orient, int report, const void *uid, const char *text) { (void)p; (void)n; (void)size; (void)orient; (void)report; (void)uid; (void)text; return RES_FAIL; }
// The SDK 20 overload that hands back a share ID, and the URL of the last post (Animal Crossing: New Horizons 3.0.3)
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_ShareId,
          void *id, const void *p, size_t n, int size, int report, const void *uid, const char *text) { (void)id; (void)p; (void)n; (void)size; (void)report; (void)uid; (void)text; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE", "URL of the last shared post: nothing was shared, fails", Result, h_album_ShareUrl, char *url, size_t n, const void *id) {
    (void)id; if (url && n) url[0] = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album14ShowAlbumFilesEv", "album applet: not shown", Result, h_album_Show, void) { return RES_FAIL; }
// Pokémon Legends: Z-A (SDK 19): a screenshot of the current frame, taken by the capture service
HLE_FORCE("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE", "screenshot of the current frame (capsrv): no album, fails", Result, h_album_SaveCur, int report) { (void)report; LOG("album::SaveCurrentScreenshot (no album)"); return RES_FAIL; }
