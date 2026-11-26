// coop ldn (MIT, our own code): the nn::ldn state machine (None 0, Initialized 1, AccessPoint 2, AccessPointCreated 3, Station 4,
// StationConnected 5), the 0x480 NetworkInfo the host builds, scan and connect over the transport, configuration and address classes. Layouts and
// result codes follow switchbrew and research/06 §3.1, §4. The host is node 0 at 169.254.S.1; a station with node id k is 169.254.S.(k+1).
#include "coop_int.h"
#include <stdio.h>

// nxnative's constant local user (hle_account.c k_uid), the default when DM_UID is unset
static const uint64_t k_uid_default[2] = {0x78626c0000000001ull, 0x53574e4154495645ull};
#define SCAN_LINK_MS 500     // a Scan waits this long for its links to the hosts, then this long again for their answers
#define SCAN_LINK_INET_MS 1500   // internet play: round trips are longer and each link proves the room code (a relay pairs it first)
#define BROWSE_GRACE_MS 1500 // the first scans after DNS-SD browsing starts wait up to this long from its start for hosts to be found
#define CONNECT_MS 4000      // nn::ldn's connect timeout is about this

void coop_logf(coop_node *n, const char *fmt, ...) {
    if (!n->cfg.hooks.log) return;
    char b[640]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    n->cfg.hooks.log(n->cfg.hooks.user, b);
}
void cnode_lock(coop_node *n) { cplat_lock(&n->lk); }
void cnode_unlock(coop_node *n) {
    int owed = n->notify, st = n->ls; n->notify = 0;
    cplat_unlock(&n->lk);
    if (owed && n->cfg.hooks.on_state) n->cfg.hooks.on_state(n->cfg.hooks.user, st);
}
void cldn_notify(coop_node *n) { n->notify = 1; }
void cldn_set_state(coop_node *n, int st) {
    if (n->ls != st) coop_logf(n, "ldn: state %d -> %d", n->ls, st);
    n->ls = st; n->notify = 1;
}
void cldn_wait(coop_node *n, int64_t ns) {
    cplat_unlock(&n->lk);
    if (n->cfg.hooks.sleep_ns) n->cfg.hooks.sleep_ns(n->cfg.hooks.user, ns); else cplat_sleep_ns(ns);
    cplat_lock(&n->lk);
}
static uint64_t fnv64(const void *p, size_t n) {
    const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
void cldn_name(char *out, const uint8_t *p, size_t n) {   // up to n bytes to the first NUL, control bytes as '?'
    size_t k = 0;
    for (; k < n && p[k]; k++) out[k] = p[k] < 0x20 || p[k] == 0x7F ? '?' : (char)p[k];
    out[k] = 0;
}
void cldn_ident(coop_node *n) {   // what peers compare: main's build (16 hex digits, lower case) and the mods fingerprint
    const char *b = n->cfg.hooks.build ? n->cfg.hooks.build(n->cfg.hooks.user) : NULL;
    int k = 0;
    if (b) for (; k < 16 && b[k]; k++) { char ch = b[k]; if (ch >= 'A' && ch <= 'F') ch = (char)(ch - 'A' + 'a'); if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) { k = 0; break; } n->build[k] = ch; }
    n->build[k == 16 ? 16 : 0] = 0;
    char what[64] = "";
    uint64_t m = n->cfg.hooks.mods ? n->cfg.hooks.mods(n->cfg.hooks.user, what, sizeof what) : 0;
    if (m != n->mods || strcmp(what, n->mods_what)) {
        if (m) coop_logf(n, "compat: gameplay mods active (%s): fingerprint %016llx; only players with the same mods can join", what[0] ? what : "?", (unsigned long long)m);
        else if (n->mods) coop_logf(n, "compat: no gameplay mods active now");
    }
    n->mods = m; snprintf(n->mods_what, sizeof n->mods_what, "%s", what);
}

// ---------- addresses ----------
int coop_addr4_class(uint32_t ip) {
    if (ip == 0 || ip == 0xFFFFFFFFu || (ip >> 28) == 0xE || (ip >> 24) == 0) return COOP_ADDR_INVALID;   // unspecified, broadcast, multicast, "this network"
    if ((ip >> 24) == 127) return COOP_ADDR_LOOPBACK;
    if ((ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8) return COOP_ADDR_PRIVATE;   // 10/8, 172.16/12, 192.168/16 (RFC 1918)
    if ((ip >> 16) == 0xA9FE) return COOP_ADDR_LINKLOCAL;                                           // 169.254/16
    return COOP_ADDR_PUBLIC;   // everything else, 100.64/10 (carrier NAT) included
}
const char *coop_addr4_vpn(uint32_t ip) {
    if ((ip >> 24) == 26) return " (Radmin VPN's range)";
    if ((ip >> 24) == 25) return " (Hamachi's range)";
    if ((ip >> 22) == 0x191) return " (carrier NAT, or Tailscale's range)";   // 100.64/10
    return "";
}
int coop_addr6_class(const uint8_t a[16]) {
    static const uint8_t lo[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, zero[16] = {0};
    static const uint8_t v4map[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (!memcmp(a, lo, 16)) return COOP_ADDR_LOOPBACK;
    if (!memcmp(a, zero, 16) || a[0] == 0xFF) return COOP_ADDR_INVALID;                              // unspecified, multicast
    if (!memcmp(a, v4map, 12)) return coop_addr4_class((uint32_t)a[12] << 24 | (uint32_t)a[13] << 16 | (uint32_t)a[14] << 8 | a[15]);
    if ((a[0] & 0xFE) == 0xFC) return COOP_ADDR_PRIVATE;                                              // fc00::/7 (unique local)
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return COOP_ADDR_LINKLOCAL;                            // fe80::/10
    return COOP_ADDR_PUBLIC;
}
int coop_addr_allowed(int scope, int cls) {
    if (cls == COOP_ADDR_LOOPBACK) return 1;
    if (scope == COOP_SCOPE_INTERNET) return cls != COOP_ADDR_INVALID;   // internet play: who may stay is decided by the room code
    return scope == COOP_SCOPE_LAN && (cls == COOP_ADDR_PRIVATE || cls == COOP_ADDR_LINKLOCAL);
}

// ---------- configuration ----------
static int parse_u(const char *s, const char *end, unsigned long max, unsigned long *out) {
    unsigned long v = 0;
    if (s == end) return -1;
    for (; s < end; s++) { if (*s < '0' || *s > '9') return -1; v = v * 10 + (unsigned long)(*s - '0'); if (v > max) return -1; }
    *out = v; return 0;
}
static int parse_ip(const char *s, const char *end, uint32_t *ip) {
    if ((size_t)(end - s) == 9 && !memcmp(s, "localhost", 9)) { *ip = 0x7F000001u; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char *d = s; while (d < end && *d != '.') d++;
        unsigned long b; if (d - s > 3 || parse_u(s, d, 255, &b)) return -1;
        v = v << 8 | (uint32_t)b;
        if (i < 3) { if (d == end) return -1; s = d + 1; } else if (d != end) return -1;
    }
    *ip = v; return 0;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int parse_uid(const char *s, uint8_t out[16]) {
    if (strlen(s) != 32) return -1;
    uint64_t w[2] = {0, 0};
    for (int i = 0; i < 32; i++) { int h = hexv(s[i]); if (h < 0) return -1; w[i / 16] = w[i / 16] << 4 | (uint64_t)h; }
    if (!w[0] && !w[1]) return -1;   // nn::account::InvalidUid
    put64(out, w[0]); put64(out + 8, w[1]);
    return 0;
}
int coop_cfg_parse(coop_cfg *c, const char *coop, const char *uid, char *err, size_t errn) { return coop_cfg_parse_ex(c, coop, uid, NULL, NULL, NULL, err, errn); }
int coop_cfg_parse_ex(coop_cfg *c, const char *coop, const char *uid, const char *internet, const char *room, const char *relay, char *err, size_t errn) {
    memset(c, 0, sizeof *c);
    c->platform = 1; c->discovery = 1;
    put64(c->uid, k_uid_default[0]); put64(c->uid + 8, k_uid_default[1]);
    if (!coop || !*coop || !strcmp(coop, "off") || !strcmp(coop, "0")) return 0;
    const char *end = coop + strlen(coop);
    unsigned long port;
    int inet = 0;
    if (internet && *internet && strcmp(internet, "0") && strcmp(internet, "off")) {
        if (strcmp(internet, "1") && strcmp(internet, "on")) { snprintf(err, errn, "DM_COOP_INTERNET=%s: 1 (internet play) or 0 wanted", internet); return -1; }
        inet = 1;
    }
    if (!strcmp(coop, "auto") || !strncmp(coop, "auto:", 5)) {
        port = COOP_PORT_DEFAULT;
        if (coop[4] == ':' && (parse_u(coop + 5, end, 65535, &port) || !port)) { snprintf(err, errn, "DM_COOP=%s: auto:<port 1-65535> wanted", coop); return -1; }
        c->role = COOP_ROLE_AUTO; c->scope = COOP_SCOPE_LAN; c->port = (uint16_t)port;
    } else if (!strncmp(coop, "host:", 5)) {
        if (parse_u(coop + 5, end, 65535, &port)) { snprintf(err, errn, "DM_COOP=%s: host:<port> wants a port 0-65535", coop); return -1; }
        c->role = COOP_ROLE_HOST; c->scope = COOP_SCOPE_LOOPBACK; c->addr = 0x7F000001u; c->port = (uint16_t)port;
    } else if (!strncmp(coop, "join:", 5)) {
        const char *colon = memchr(coop + 5, ':', (size_t)(end - coop - 5)), *ie = colon ? colon : end;
        port = COOP_PORT_DEFAULT;
        if (parse_ip(coop + 5, ie, &c->addr) || (colon && (parse_u(colon + 1, end, 65535, &port) || !port))) {
            snprintf(err, errn, "DM_COOP=%s: join:<IPv4>[:<port>] wanted", coop); return -1;
        }
        int cls = coop_addr4_class(c->addr);
        if (!coop_addr_allowed(inet ? COOP_SCOPE_INTERNET : COOP_SCOPE_LAN, cls)) {
            if (inet) snprintf(err, errn, "DM_COOP=%s: %u.%u.%u.%u is not an address a host can have", coop, c->addr >> 24, c->addr >> 16 & 0xFF, c->addr >> 8 & 0xFF, c->addr & 0xFF);
            else snprintf(err, errn, "DM_COOP=%s: %u.%u.%u.%u%s is not a loopback, private or link-local address; through a VPN or the internet, every player "
                          "turns on internet play (DM_COOP_INTERNET=1, the host's room code)", coop, c->addr >> 24, c->addr >> 16 & 0xFF, c->addr >> 8 & 0xFF, c->addr & 0xFF,
                          coop_addr4_vpn(c->addr));
            return -1;
        }
        c->role = COOP_ROLE_JOIN; c->scope = cls == COOP_ADDR_LOOPBACK ? COOP_SCOPE_LOOPBACK : inet ? COOP_SCOPE_INTERNET : COOP_SCOPE_LAN; c->port = (uint16_t)port;
    } else { snprintf(err, errn, "DM_COOP=%s: auto, auto:<port>, join:<IPv4>[:<port>], host:<port> or off wanted", coop); return -1; }
    if (uid && *uid && parse_uid(uid, c->uid)) { snprintf(err, errn, "DM_UID=%s: 32 hex digits, not all zero, wanted", uid); return -1; }
    if (!inet) return 1;
    // internet play: the room code every link proves (a visitor needs the host's; a host without one gets a new one), the relay, any address
    c->auth = 1;
    if (c->role == COOP_ROLE_AUTO) c->scope = COOP_SCOPE_INTERNET;   // host: links from anywhere (port forwarding); join: any address. host: stays 127.0.0.1
    if (room && *room) {
        if (!coop_room_normalize(room, c->room)) { snprintf(err, errn, "DM_COOP_ROOM: a room code is 16 letters and digits, XXXX-XXXX-XXXX-XXXX as the host's log shows it"); return -1; }
    } else if (c->role == COOP_ROLE_JOIN) { snprintf(err, errn, "DM_COOP_INTERNET=1: joining needs the host's room code (DM_COOP_ROOM=XXXX-XXXX-XXXX-XXXX)"); return -1; }
    else { coop_room_new(c->room); c->room_new = 1; }
    if (relay && *relay) {
        const char *re = relay + strlen(relay), *colon = memchr(relay, ':', (size_t)(re - relay)), *ie = colon ? colon : re;
        port = COOP_RELAY_PORT_DEFAULT;
        if (parse_ip(relay, ie, &c->relay_ip) || (colon && (parse_u(colon + 1, re, 65535, &port) || !port)) || coop_addr4_class(c->relay_ip) == COOP_ADDR_INVALID) {
            snprintf(err, errn, "DM_COOP_RELAY=%s: <IPv4>[:<port>] wanted", relay); return -1;
        }
        if (c->role != COOP_ROLE_AUTO) { snprintf(err, errn, "DM_COOP_RELAY needs DM_COOP=auto: it hosts through the relay while the game hosts and joins its room while the game scans"); return -1; }
        c->relay_port = (uint16_t)port; c->discovery = 0;   // the relay is the meeting place: no DNS-SD
    }
    return 1;
}
int coop_env_uid(uint8_t out[16]) {
    const char *c = getenv("DM_COOP"), *u = getenv("DM_UID");
    return c && *c && strcmp(c, "off") && strcmp(c, "0") && u && !parse_uid(u, out);
}
coop_node *coop_env_node(const coop_hooks *hooks) {
    static cmutex once = CMUTEX_INIT; static int done; static coop_node *node;
    cplat_lock(&once);
    if (!done) {
        done = 1;
        coop_cfg c; char err[320] = "", jm[96], msg[360];
        const char *mode = getenv("DM_COOP"), *inet = getenv("DM_COOP_INTERNET"), *room = getenv("DM_COOP_ROOM"), *relay = getenv("DM_COOP_RELAY");
        const char *join = getenv("DM_COOP_JOIN");   // the launcher's "Join address" under its Link Play switch: auto becomes join:<it>
        int joined = join && *join && mode && !strcmp(mode, "auto") && strlen(join) < 64;
        if (joined) { snprintf(jm, sizeof jm, "join:%s", join); mode = jm; }
        int r = coop_cfg_parse_ex(&c, mode, getenv("DM_UID"), inet, room, relay, err, sizeof err);
        const char *t = getenv("DM_COOP_TCP"), *d = getenv("DM_COOP_DISCOVERY");
        c.tcp_only = t && *t && strcmp(t, "0");
        if (d && !strcmp(d, "0")) c.discovery = 0;
        if (hooks) c.hooks = *hooks;
        if (r < 0 && joined) { snprintf(msg, sizeof msg, "DM_COOP_JOIN (the Join address): %s", err); snprintf(err, sizeof err, "%s", msg); }
        if (r < 0 && hooks && hooks->log) hooks->log(hooks->user, err);
        if (join && *join && !joined && r > 0 && hooks && hooks->log) hooks->log(hooks->user, "DM_COOP_JOIN ignored: it goes with DM_COOP=auto (the launcher's Link Play switch)");
        if (r == 0 && mode && *mode && hooks && hooks->log) hooks->log(hooks->user, "DM_COOP=off: Link Play stays offline");
        if (r > 0 && (node = coop_create(&c))) {
            uint8_t *u = c.uid; char a[16]; snprintf(a, sizeof a, "%u.%u.%u.%u", c.addr >> 24, c.addr >> 16 & 0xFF, c.addr >> 8 & 0xFF, c.addr & 0xFF);
            coop_logf(node, "DM_COOP %s%s%s, port %u, peers: %s, Uid %016llx%016llx%s%s", c.role == COOP_ROLE_HOST ? "host" : c.role == COOP_ROLE_JOIN ? "join " : "auto",
                      c.role == COOP_ROLE_JOIN ? a : "", c.role == COOP_ROLE_AUTO && !c.discovery ? " (DNS-SD off)" : "", c.port,
                      c.scope == COOP_SCOPE_LOOPBACK ? "loopback only" : c.scope == COOP_SCOPE_INTERNET ? "any address that proves the room code" : "loopback, private and link-local addresses only",
                      (unsigned long long)get64(u), (unsigned long long)get64(u + 8), getenv("DM_UID") ? "" : " (the constant: DM_UID unset)",
                      c.tcp_only ? ", game datagrams over the control link (DM_COOP_TCP)" : "");
            if (c.auth) {
                char shown[COOP_ROOM_SHOWN]; coop_room_format(c.room, shown);
                if (c.room_new) coop_logf(node, "internet play: this session's room code is %s; visitors need it (set DM_COOP_ROOM to keep one code across sessions)", shown);
                else coop_logf(node, "internet play: room code %s", shown);
                if (c.relay_ip) coop_logf(node, "internet play: through the relay %u.%u.%u.%u:%u (hosting opens the room there, scanning asks it for the room's host)",
                                          c.relay_ip >> 24, c.relay_ip >> 16 & 0xFF, c.relay_ip >> 8 & 0xFF, c.relay_ip & 0xFF, c.relay_port);
                coop_logf(node, "internet play: every link proves the room code and every message is tagged against changes, but nothing is encrypted: "
                                "anyone on the way (a relay too) can read the game's traffic (docs/coop-step1d.md)");
            } else if ((room && *room) || (relay && *relay)) coop_logf(node, "DM_COOP_ROOM / DM_COOP_RELAY ignored: internet play is off (DM_COOP_INTERNET=1 turns it on)");
        }
    }
    cplat_unlock(&once);
    return node;
}

coop_node *coop_create(const coop_cfg *c) {
    if (cplat_init()) return NULL;
    coop_node *n = calloc(1, sizeof *n);
    if (!n) { cplat_fini(); return NULL; }
    n->cfg = *c;
    cplat_mutex_init(&n->lk); cplat_mutex_init(&n->life); cplat_mutex_init(&n->dlk);
    n->lsock = n->husock = n->susock = CSOCK_BAD; n->upi = -1; n->join_peer = -1;
    n->proto = c->auth ? MP_PROTO_INET : MP_PROTO; n->uhdr = c->auth ? MP_UDP_HDR3 : MP_UDP_HDR;
    if (c->auth) croom_keys(n->cfg.room, n->k_auth, n->room_id);
    n->rc.s = CSOCK_BAD;
    for (int i = 0; i < HOST_CONNS; i++) n->hc[i].s = CSOCK_BAD;
    for (int i = 0; i < PEERS; i++) n->pr[i].c.s = CSOCK_BAD;
    n->next_eph = 49152;
    cdisc_init(n);
    return n;
}
void coop_destroy(coop_node *n) {
    if (!n) return;
    coop_ldn_finalize(n);
    cdisc_fini(n);
    cvnet_reset(n);
    cplat_mutex_destroy(&n->lk); cplat_mutex_destroy(&n->life); cplat_mutex_destroy(&n->dlk);
    cwipe(n, sizeof *n);   // the room code and keys
    free(n);
    cplat_fini();
}
uint16_t coop_port(coop_node *n) { cnode_lock(n); uint16_t p = n->hosting ? n->bound_port : 0; cnode_unlock(n); return p; }
int coop_role(coop_node *n) { return n->cfg.role; }

// ---------- the host's NetworkInfo ----------
static void node_mac(coop_node *n, uint8_t *mac, const uint8_t *uid, int node) {
    uint64_t h = fnv64(uid, 16);   // locally administered unicast, per device; the last two bytes keep it unique inside the network
    mac[0] = 0x02; mac[1] = (uint8_t)h; mac[2] = (uint8_t)(h >> 8); mac[3] = (uint8_t)(h >> 16);
    mac[4] = (uint8_t)(n->subnet >> 8); mac[5] = (uint8_t)(node + 1);
}
static void node_info(coop_node *n, uint8_t *nd, int node, const uint8_t *uid, const uint8_t *user, uint16_t lcv, uint8_t platform) {
    put32(nd, vip_of(n, node));                    // Ipv4 (host order)
    node_mac(n, nd + 4, uid, node);                // MacAddress
    nd[0xA] = (uint8_t)node; nd[0xB] = 1;          // NodeId, IsConnected
    memcpy(nd + 0xC, user, 0x20); nd[0x2C] = 0;    // UserName[33]
    nd[0x2D] = platform;                           // Platform (Switch 2 era)
    put16(nd + 0x2E, lcv);                         // LocalCommunicationVersion
}
void cldn_host_rebuild(coop_node *n) {
    uint8_t *p = n->info, *l = p + 0x50;
    memset(p, 0, LDN_NETINFO);
    memcpy(p, n->netcfg, 0x10);                    // NetworkId.IntentId
    memcpy(p + 0x10, n->session, 16);              // NetworkId.SessionId
    node_mac(n, p + 0x20, n->cfg.uid, 0);          // Common.Bssid: the access point's MAC
    static const char hx[] = "0123456789abcdef";
    p[0x26] = 32;                                  // Common.Ssid: length, 32 characters
    for (int i = 0; i < 16; i++) { p[0x27 + 2 * i] = (uint8_t)hx[n->session[i] >> 4]; p[0x28 + 2 * i] = (uint8_t)hx[n->session[i] & 15]; }
    uint16_t ch = get16(n->netcfg + 0x10);
    put16(p + 0x48, ch ? ch : 1);                  // Channel (0 asks for automatic)
    p[0x4A] = 3; p[0x4B] = 2;                      // LinkLevel, NetworkType Ldn
    memcpy(l, n->server_random, 16);               // Ldn.ServerRandom
    put16(l + 0x10, n->sec_mode);                  // SecurityMode
    l[0x12] = (uint8_t)n->policy;                  // StationAcceptPolicy
    l[0x16] = n->netcfg[0x12];                     // NodeCountMax
    int count = 1;
    node_info(n, l + 0x18, 0, n->cfg.uid, n->user, n->self_lcv, n->cfg.platform);
    for (int i = 0; i < HOST_CONNS; i++) {
        conn *c = &n->hc[i];
        if (c->state != CONN_READY || c->node <= 0) continue;
        node_info(n, l + 0x18 + 0x40 * c->node, c->node, c->uid, c->user, c->lcv, c->platform); count++;
    }
    l[0x17] = (uint8_t)count;                      // NodeCount
    put16(l + 0x21A, n->advn); memcpy(l + 0x21C, n->adv, n->advn);   // AdvertiseDataSize, AdvertiseData
    put64(l + 0x428, n->challenge);                // Challenge
    n->have_info = 1; n->info_gen++;
}
// A NetworkInfo a host sent: the game parses it as the system's own, so only one inside nn::ldn's limits is handed on. Node slots that are not
// connected and advertise bytes past the size are zeroed
int cldn_info_sane(uint8_t *p, char *why, size_t wn) {
    uint8_t *l = p + 0x50;
#define INSANE(...) do { snprintf(why, wn, __VA_ARGS__); return 0; } while (0)
    if (p[0x26] > 32) INSANE("an Ssid of %u bytes", p[0x26]);
    if (p[0x4B] != 2) INSANE("NetworkType %u", p[0x4B]);
    unsigned maxn = l[0x16], cnt = l[0x17], adv = get16(l + 0x21A);
    if (!maxn || maxn > LDN_NODES) INSANE("NodeCountMax %u", maxn);
    if (!cnt || cnt > maxn) INSANE("NodeCount %u of %u", cnt, maxn);
    if (adv > LDN_ADV_MAX) INSANE("AdvertiseDataSize %u", adv);
    uint32_t ip0 = get32(l + 0x18), sub = ip0 & 0xFFFFFF00u, s = sub >> 8 & 0xFF;
    if ((ip0 & 0xFFFF0000u) != VIP_PREFIX || !s || s == 255 || (ip0 & 0xFF) != 1) INSANE("host address %u.%u.%u.%u", ip0 >> 24, ip0 >> 16 & 0xFF, s, ip0 & 0xFF);
    unsigned listed = 0;
    for (int k = 0; k < LDN_NODES; k++) {
        uint8_t *nd = l + 0x18 + 0x40 * k;
        if (!nd[0xB]) { memset(nd, 0, 0x40); continue; }
        if (nd[0xB] != 1 || nd[0xA] != k || get32(nd) != (sub | (uint32_t)(k + 1))) INSANE("node %d's id or address", k);
        if (!memchr(nd + 0xC, 0, 33)) INSANE("node %d's name is not terminated", k);
        listed++;
    }
    if (listed != cnt || !l[0x18 + 0xB]) INSANE("%u nodes listed, NodeCount %u", listed, cnt);
    memset(l + 0x21C + adv, 0, LDN_ADV_MAX - adv);
#undef INSANE
    return 1;
}
static void host_teardown(coop_node *n, int reason) {   // the network ends: every station is told, nothing listens any more, the address plan goes
    for (int i = 0; i < HOST_CONNS; i++) if (n->hc[i].state == CONN_READY && n->hc[i].node > 0) cmp_host_kick(n, &n->hc[i], reason);
    cmp_host_close(n);
    n->have_info = 0; n->subnet = 0;
}
uint32_t cldn_host_join(coop_node *n, conn *c, const uint8_t *netid, const uint8_t *user, uint16_t lcv, uint64_t sec_hash, const char *build, uint64_t mods) {
    if (n->ls != COOP_LS_AP_CREATED || memcmp(netid, n->info, 0x20)) return COOP_LDN_CONNECT_NOT_FOUND;
    if (sec_hash != n->sec_hash || n->policy == 1) return COOP_LDN_CONNECT_REJECTED;
    if (lcv < n->self_lcv) return COOP_LDN_VERSION_TOO_LOW;
    if (lcv > n->self_lcv) return COOP_LDN_VERSION_TOO_HIGH;
    if (n->build[0] && build[0] && strncmp(n->build, build, 16)) {
        coop_logf(n, "compat: a station runs another game build (%s; this host %s)", build, n->build);
        return COOP_LDN_CONNECT_REJECTED;
    }
    if (mods != n->mods) {
        coop_logf(n, "compat: a station's mods or cheats differ (fingerprint %016llx; this host %016llx%s%s)", (unsigned long long)mods, (unsigned long long)n->mods,
                  n->mods_what[0] ? ": " : "", n->mods_what);
        return COOP_LDN_CONNECT_REJECTED;
    }
    int used = 1, taken[LDN_NODES] = {1};
    for (int i = 0; i < HOST_CONNS; i++) if (n->hc[i].state == CONN_READY && n->hc[i].node > 0) { used++; taken[n->hc[i].node] = 1; }
    if (used >= n->netcfg[0x12] || used >= LDN_NODES) return COOP_LDN_TOO_MANY_PLAYERS;
    int id = 1; while (taken[id]) id++;
    c->node = id; c->lcv = lcv; memcpy(c->user, user, 0x30); c->user[0x20] = 0;
    cplat_random(&c->token, sizeof c->token);
    cldn_host_rebuild(n); cldn_notify(n);
    return 0;
}
void cldn_station_lost(coop_node *n, int reason) {
    if (n->ls != COOP_LS_STATION_CONNECTED) return;
    coop_logf(n, "ldn: left the network (disconnect reason %d)", reason);
    n->reason = reason; n->have_info = 0; n->subnet = 0; n->token = 0; n->udp_ok = 0; n->self = 0; n->upi = -1;
    cldn_set_state(n, COOP_LS_STATION);
}

// ---------- nn::ldn ----------
static uint32_t move(coop_node *n, int from_mask, int to) {
    if (!(from_mask >> n->ls & 1)) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    cldn_set_state(n, to); cnode_unlock(n); return 0;
}
#define S(x) (1 << COOP_LS_##x)
static void browse(coop_node *n, int on) {   // auto: DNS-SD browsing from the first OpenStation or Scan to OpenAccessPoint or Finalize
    if (n->cfg.role != COOP_ROLE_AUTO || n->browse == on) return;
    n->browse = on; n->disc.browse_want = on && n->cfg.discovery;
    if (on) n->browse_t0 = cplat_now_ms();
}
uint32_t coop_ldn_initialize(coop_node *n) {
    cplat_lock(&n->life); cnode_lock(n);
    uint32_t r = 0;
    if (n->ls == COOP_LS_NONE) {
        cldn_ident(n);
        if (cmp_start(n)) { coop_logf(n, "ldn: Initialize: the transport did not start"); r = COOP_LDN_DEVICE_DISABLED; }
        else { n->reason = COOP_DR_NONE; cldn_set_state(n, COOP_LS_INITIALIZED); }
    }
    cnode_unlock(n); cplat_unlock(&n->life);
    return r;
}
void coop_ldn_finalize(coop_node *n) {
    cplat_lock(&n->life); cnode_lock(n);
    int was = n->ls;
    if (was == COOP_LS_AP_CREATED) host_teardown(n, COOP_DR_DESTROYED_BY_USER);
    if (was == COOP_LS_STATION_CONNECTED) cmp_station_leave(n);
    browse(n, 0);
    if (was != COOP_LS_NONE) { n->have_info = 0; n->subnet = 0; cldn_set_state(n, COOP_LS_NONE); }
    cnode_unlock(n);
    if (was != COOP_LS_NONE) cmp_stop(n);
    cplat_unlock(&n->life);
}
int coop_ldn_state(coop_node *n) { cnode_lock(n); int s = n->ls; cnode_unlock(n); return s; }
uint32_t coop_ldn_open_ap(coop_node *n) {
    cnode_lock(n);
    if (n->ls == COOP_LS_INITIALIZED) { n->advn = 0; n->policy = 0; browse(n, 0); }
    return move(n, S(INITIALIZED), COOP_LS_AP);
}
uint32_t coop_ldn_close_ap(coop_node *n) {
    cnode_lock(n);
    if (n->ls == COOP_LS_AP_CREATED) host_teardown(n, COOP_DR_DESTROYED_BY_USER);
    return move(n, S(AP) | S(AP_CREATED), COOP_LS_INITIALIZED);
}
uint32_t coop_ldn_create_network(coop_node *n, const void *netcfg, const void *sec, const void *user) {
    cnode_lock(n);
    if (n->ls != COOP_LS_AP) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    const uint8_t *cf = netcfg;
    if (!cf[0x12] || cf[0x12] > LDN_NODES) { cnode_unlock(n); return COOP_LDN_INVALID_ARGUMENT; }
    cldn_ident(n);
    if (n->cfg.role != COOP_ROLE_JOIN && cmp_host_open(n)) { cnode_unlock(n); return COOP_LDN_DEVICE_DISABLED; }   // nothing can reach a network nobody listens for
    memcpy(n->netcfg, cf, 0x20); memcpy(n->user, user, 0x30);
    if (n->cfg.lcid && get64(n->netcfg) == ~0ull) put64(n->netcfg, n->cfg.lcid);
    n->sec_mode = get16(sec); n->sec_hash = fnv64(sec, 0x44);   // the passphrase is compared as a hash and never leaves the node
    cplat_random(n->session, 16); cplat_random(n->server_random, 16); cplat_random(&n->challenge, 8);
    n->subnet = VIP_PREFIX | (uint32_t)(1 + n->session[0] % 254) << 8;
    n->self = 0; n->self_lcv = get16(cf + 0x14);
    cldn_host_rebuild(n);
    coop_logf(n, "ldn: CreateNetwork: NodeCountMax %u, LocalCommunicationVersion %u, network 169.254.%u.0/24%s", cf[0x12], n->self_lcv,
              n->subnet >> 8 & 0xFF, n->cfg.role == COOP_ROLE_JOIN ? " (DM_COOP is join: nobody can reach this network)" : "");
    return move(n, S(AP), COOP_LS_AP_CREATED);
}
uint32_t coop_ldn_destroy_network(coop_node *n) {
    cnode_lock(n);
    if (n->ls == COOP_LS_AP_CREATED) host_teardown(n, COOP_DR_DESTROYED_BY_USER);
    return move(n, S(AP_CREATED), COOP_LS_AP);
}
uint32_t coop_ldn_set_advertise(coop_node *n, const void *data, uint64_t size) {
    if (size > LDN_ADV_MAX) return COOP_LDN_INVALID_ARGUMENT;
    cnode_lock(n);
    if (n->ls != COOP_LS_AP && n->ls != COOP_LS_AP_CREATED) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    if (size) memcpy(n->adv, data, (size_t)size);
    n->advn = (uint16_t)size;
    if (n->ls == COOP_LS_AP_CREATED) { cldn_host_rebuild(n); cmp_host_sync(n); cldn_notify(n); }
    cnode_unlock(n);
    return 0;
}
uint32_t coop_ldn_open_station(coop_node *n) {
    cnode_lock(n);
    if (n->ls == COOP_LS_INITIALIZED) { n->reason = COOP_DR_NONE; browse(n, 1); }
    return move(n, S(INITIALIZED), COOP_LS_STATION);
}
uint32_t coop_ldn_close_station(coop_node *n) {
    cnode_lock(n);
    if (n->ls == COOP_LS_STATION_CONNECTED) { cmp_station_leave(n); n->have_info = 0; n->subnet = 0; }
    return move(n, S(STATION) | S(STATION_CONNECTED), COOP_LS_INITIALIZED);
}
static int filter_match(coop_node *n, const uint8_t *info, const uint8_t *f) {
    uint32_t fl = get32(f + 0x5C);
    uint64_t want = get64(f);
    if (n->cfg.lcid && want == ~0ull) want = n->cfg.lcid;
    if ((fl & 0x01) && get64(info) != want) return 0;                                    // LocalCommunicationId
    if ((fl & 0x02) && memcmp(info + 0x10, f + 0x10, 16)) return 0;                      // SessionId
    if ((fl & 0x04) && !(get32(f + 0x20) & info[0x4B])) return 0;                        // NetworkType (bit mask: 3 is any)
    if ((fl & 0x08) && memcmp(info + 0x20, f + 0x24, 6)) return 0;                       // Bssid
    if ((fl & 0x10) && (info[0x26] != f[0x2A] || memcmp(info + 0x27, f + 0x2B, info[0x26] < 33 ? info[0x26] : 33))) return 0;   // Ssid
    if ((fl & 0x20) && get16(info + 0xA) != get16(f + 0xA)) return 0;                     // SceneId
    return 1;
}
// Every host the station knows is asked for its NetworkInfo: the configured one (join), or the ones DNS-SD found (auto). A host whose build or
// mods differ, or that speaks another coop protocol, is not listed (coop_mpnet.c logs why once)
uint32_t coop_ldn_scan(coop_node *n, void *out, int *count, int max, const void *filter, int channel) {
    (void)channel;
    if (count) *count = 0;
    cnode_lock(n);
    if (!((S(AP) | S(AP_CREATED) | S(STATION) | S(STATION_CONNECTED)) >> n->ls & 1)) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    if (n->cfg.role == COOP_ROLE_HOST || max <= 0 || !out || n->ls == COOP_LS_AP || n->ls == COOP_LS_AP_CREATED) { cnode_unlock(n); return 0; }
    browse(n, 1);
    cldn_ident(n);   // cheats turned on or off since the last scan change which hosts match
    while (n->scan_busy && n->io_on) cldn_wait(n, 1000000);
    if (!n->io_on) { cnode_unlock(n); return 0; }
    n->scan_busy = 1;
    int64_t t0 = cplat_now_ms(), until = t0 + (n->cfg.auth ? SCAN_LINK_INET_MS : SCAN_LINK_MS);
    if (n->cfg.role == COOP_ROLE_AUTO && n->cfg.discovery && n->browse_t0 + BROWSE_GRACE_MS > until) until = n->browse_t0 + BROWSE_GRACE_MS;
    for (;;) {   // links: every host that is not known to be down or incompatible, or the deadline
        int ready = 0, waiting = 0; int64_t now = cplat_now_ms();
        for (int i = 0; i < PEERS; i++) {
            peer *p = &n->pr[i];
            if (!p->used || p->compat == -2) continue;
            if (p->c.state == CONN_READY && !p->c.closing) ready++;
            else if (now >= p->dead_until) waiting++;
        }
        int grace = n->cfg.role == COOP_ROLE_AUTO && n->cfg.discovery && now < n->browse_t0 + BROWSE_GRACE_MS;   // DNS-SD may still find one
        if ((ready && !waiting) || (!ready && !waiting && !grace) || now >= until || !n->io_on) break;
        cldn_wait(n, 2000000);
    }
    int asked = 0;
    for (int i = 0; i < PEERS; i++) {
        peer *p = &n->pr[i];
        p->scan_wait = 0;
        if (!p->used || p->compat == -2 || p->c.state != CONN_READY || p->c.closing) continue;
        p->scan_wait = 1; p->scan_done = 0; p->scan_has = 0;
        cmp_send_frame(n, &p->c, M_SCAN_REQ, NULL, 0);
        asked++;
    }
    int64_t until2 = cplat_now_ms() + (n->cfg.auth ? SCAN_LINK_INET_MS : SCAN_LINK_MS);
    while (asked && cplat_now_ms() < until2 && n->io_on) {
        int open = 0;
        for (int i = 0; i < PEERS; i++) open += n->pr[i].used && n->pr[i].scan_wait && !n->pr[i].scan_done;
        if (!open) break;
        cldn_wait(n, 1000000);
    }
    int found = 0, hidden = 0, filtered = 0;
    for (int i = 0; i < PEERS; i++) {
        peer *p = &n->pr[i];
        if (!p->used || !p->scan_wait) continue;
        p->scan_wait = 0;
        if (!p->scan_done || !p->scan_has) continue;
        if (p->compat < 0) { hidden++; continue; }
        if (filter && !filter_match(n, p->scan_info, filter)) { filtered++; continue; }
        if (found < max) memcpy((uint8_t *)out + (size_t)found * LDN_NETINFO, p->scan_info, LDN_NETINFO), found++;
    }
    if (count) *count = found;
    int code = found | hidden << 8 | filtered << 16;
    if (code != n->scan_logged) {
        coop_logf(n, "ldn: Scan: %d network(s) found, %d host(s) asked%s%s", found, asked, hidden ? ", some hidden (another build or other mods)" : "",
                  filtered ? ", some not matching the game's filter" : "");
        n->scan_logged = code;
    }
    n->scan_busy = 0;
    cnode_unlock(n);
    return 0;
}
uint32_t coop_ldn_connect(coop_node *n, const void *info, const void *sec, const void *user, int version, int option) {
    (void)option;
    cnode_lock(n);
    if (n->ls != COOP_LS_STATION) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    while (n->join_wait && n->io_on) cldn_wait(n, 1000000);
    peer *p = NULL;
    for (int i = 0; i < PEERS && !p; i++) if (n->pr[i].used && n->pr[i].scan_has && !memcmp(n->pr[i].scan_info, info, 0x20)) p = &n->pr[i];   // NetworkId
    if (n->cfg.role == COOP_ROLE_HOST || !p || !n->io_on) {
        coop_logf(n, "ldn: Connect: no known host has that network");
        cnode_unlock(n); return COOP_LDN_CONNECT_NOT_FOUND;
    }
    if (p->compat < 0) { coop_logf(n, "ldn: Connect: that host runs another build or other mods"); cnode_unlock(n); return COOP_LDN_CONNECT_REJECTED; }
    int64_t until = cplat_now_ms() + (n->cfg.auth ? SCAN_LINK_INET_MS : SCAN_LINK_MS);
    while ((p->c.state != CONN_READY || p->c.closing) && n->io_on && cplat_now_ms() < until) cldn_wait(n, 2000000);
    if (p->c.state != CONN_READY || p->c.closing) { cnode_unlock(n); return COOP_LDN_CONNECT_NOT_FOUND; }
    cldn_ident(n);   // cheats may have changed since the scan
    uint8_t m[MP_JOIN_LEN];
    memcpy(m, info, 0x20);
    if (n->cfg.lcid && get64(m) == ~0ull) put64(m, n->cfg.lcid);
    memcpy(m + 0x20, user, 0x30);
    put16(m + 0x50, (uint32_t)version);
    put64(m + 0x52, fnv64(sec, 0x44));
    memset(m + 0x5A, 0, 16); memcpy(m + 0x5A, n->build, strlen(n->build)); put64(m + 0x6A, n->mods);
    n->join_wait = 1; n->join_done = 0; n->join_result = COOP_LDN_CONNECT_TIMEOUT; n->join_peer = (int)(p - n->pr);
    cmp_send_frame(n, &p->c, M_JOIN, m, sizeof m);
    until = cplat_now_ms() + CONNECT_MS;
    while (!n->join_done && p->c.state == CONN_READY && cplat_now_ms() < until) cldn_wait(n, 1000000);
    uint32_t r = n->join_done ? n->join_result : p->c.state == CONN_READY ? COOP_LDN_CONNECT_TIMEOUT : COOP_LDN_CONNECT_NOT_FOUND;
    n->join_wait = 0; n->join_peer = -1;
    coop_logf(n, "ldn: Connect (LocalCommunicationVersion %d) -> %#x%s", version, r, r ? "" : ", joined");
    cnode_unlock(n);
    return r;
}
uint32_t coop_ldn_disconnect(coop_node *n) {
    cnode_lock(n);
    if (n->ls == COOP_LS_STATION_CONNECTED) { cmp_station_leave(n); n->have_info = 0; n->subnet = 0; n->reason = COOP_DR_BY_USER; }
    return move(n, S(STATION_CONNECTED), COOP_LS_STATION);
}
uint32_t coop_ldn_network_info(coop_node *n, void *out) {
    cnode_lock(n);
    uint32_t r = COOP_LDN_INVALID_STATE;
    if ((n->ls == COOP_LS_AP_CREATED || n->ls == COOP_LS_STATION_CONNECTED) && n->have_info) {
        memcpy(out, n->info, LDN_NETINFO); r = 0;
        if (n->info_seen != n->info_gen) {   // the game polls this every frame (Pokopia binds no state event): a reader of a new node list is worth a line
            if (n->info[0x67] != n->seen_count) coop_logf(n, "ldn: the game read the NetworkInfo: %u of %u nodes", n->info[0x67], n->info[0x66]);
            n->seen_count = n->info[0x67]; n->info_seen = n->info_gen;
            if (n->ls == COOP_LS_AP_CREATED) cmp_host_accepts(n);
        }
    }
    cnode_unlock(n);
    return r;
}
uint32_t coop_ldn_ipv4(coop_node *n, uint32_t *ip, uint32_t *mask) {
    cnode_lock(n);
    int ok = (n->ls == COOP_LS_AP_CREATED || n->ls == COOP_LS_STATION_CONNECTED) && n->subnet;
    if (ip) *ip = ok ? vip_of(n, n->self) : 0;
    if (mask) *mask = ok ? 0xFFFFFF00u : 0;
    cnode_unlock(n);
    return ok ? 0 : COOP_LDN_INVALID_STATE;
}
uint32_t coop_ldn_network_config(coop_node *n, void *out) {
    uint8_t *o = out; uint32_t r = 0;
    cnode_lock(n);
    if (n->ls == COOP_LS_AP_CREATED) memcpy(o, n->netcfg, 0x20);
    else if (n->ls == COOP_LS_STATION_CONNECTED && n->have_info) {
        memset(o, 0, 0x20); memcpy(o, n->info, 0x10);
        put16(o + 0x10, get16(n->info + 0x48)); o[0x12] = n->info[0x66]; put16(o + 0x14, get16(n->info + 0x68 + 0x2E));
    } else r = COOP_LDN_INVALID_STATE;
    cnode_unlock(n);
    return r;
}
uint32_t coop_ldn_security_parameter(coop_node *n, void *out) {
    uint8_t *o = out; uint32_t r = 0;
    cnode_lock(n);
    if ((n->ls == COOP_LS_AP_CREATED || n->ls == COOP_LS_STATION_CONNECTED) && n->have_info) { memcpy(o, n->info + 0x50, 16); memcpy(o + 16, n->info + 0x10, 16); }
    else r = COOP_LDN_INVALID_STATE;
    cnode_unlock(n);
    return r;
}
int coop_ldn_disconnect_reason(coop_node *n) { cnode_lock(n); int r = n->reason; cnode_unlock(n); return r; }
uint32_t coop_ldn_set_accept_policy(coop_node *n, int policy) {
    if (policy < 0 || policy > 3) return COOP_LDN_INVALID_ARGUMENT;
    cnode_lock(n);
    if (n->ls != COOP_LS_AP && n->ls != COOP_LS_AP_CREATED) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    n->policy = policy;
    if (n->ls == COOP_LS_AP_CREATED) { cldn_host_rebuild(n); cmp_host_sync(n); cldn_notify(n); }
    cnode_unlock(n);
    return 0;
}
uint32_t coop_ldn_reject(coop_node *n, uint32_t ip) {
    cnode_lock(n);
    if (n->ls != COOP_LS_AP_CREATED) { cnode_unlock(n); return COOP_LDN_INVALID_STATE; }
    conn *c = cmp_host_conn_by_ip(n, ip);
    if (c) { coop_logf(n, "ldn: Reject %u.%u.%u.%u (node %d)", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF, c->node); cmp_host_kick(n, c, COOP_DR_REJECTED); cmp_host_drop(n, c); }
    cnode_unlock(n);
    return 0;
}
