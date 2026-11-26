// coop mpnet (MIT, our own code): the real transport, a star around the host (the LDN access point).
// - Control: one TCP link per station and host, framed messages (u32 length, u8 type, body): HELLO / WELCOME (the coop protocol; the host's build and
//   mods fingerprint), SCAN_REQ / SCAN_RESP (the host's NetworkInfo and identity), JOIN / ACCEPT / REFUSE (the host checks the network, passphrase,
//   version, build, mods and room, as nn::ldn's Connect results say), SYNC (NetworkInfo after every join, leave, advertise or policy change), LEAVE,
//   KICK (Reject, network destroyed), PING / PONG (1 s; a link silent for 10 s is lost).
// - Game datagrams: UDP between each station and the host, unreliable and unordered as the game expects (it has its own resends; TCP would
//   add head-of-line stalls). A station proves its UDP endpoint with the token from ACCEPT (UHELLO / UACK, from the address of its control link)
//   and uses the control link until then, or always with cfg.tcp_only. The host relays station-to-station datagrams and rewrites source addresses.
// - Protection (docs/coop-step1c.md): the host listens only while the game hosts; links and UDP packets from addresses the scope refuses are
//   dropped (LAN: loopback, RFC 1918, 169.254/16); every frame and packet is length-checked before it is read and a malformed one closes its link;
//   accepts, half-open links per address, frames, scans, joins and datagrams are rate-limited; a station checks every NetworkInfo, ACCEPT and REFUSE
//   it is given before the game sees it.
// A station keeps one link per host it knows (the configured one, or the ones DNS-SD found), scans over all of them and joins over one.
// One I/O thread per node owns accepting, dialing, reading, DNS-SD and closing; game threads only append to a link's send buffer under the node lock.
// Internet play (cfg.auth, protocol 3, docs/coop-step1d.md): any address may connect, but a link counts only once the station has proven the room
// code (AUTH_CHAL / AUTH_PROOF) and the host has proven it back (AUTH_OK); then every frame carries a tag over an implicit sequence number and every
// datagram a sequence number and a tag (coop_auth.c), so changed, dropped, repeated or foreign messages close the link or are dropped. Failed proofs
// are limited per minute, per address and for all links. Nothing is encrypted. Through a relay (cfg.relay_ip) the host keeps a control link open
// at the relay while the game hosts and dials one more link per visitor the relay announces; a station dials the relay and asks for the room; the
// relay pairs the two and copies bytes, and game datagrams then use the control link.
#include "coop_int.h"
#include <stdio.h>

#define IO_TICK_MS 10
#define PING_MS 1000
#define LINK_TIMEOUT_MS 10000
#define HELLO_TIMEOUT_MS 3000
#define CONNECT_TIMEOUT_MS 3000
#define DIAL_RETRY_MS 500
#define DIAL_BACKOFF_MS 5000     // a host that keeps refusing is dialled at most this often
#define UHELLO_MS 100
#define UHELLO_FOR_MS 3000
#define TX_MAX (4u << 20)
#define RX_MAX (MP_FRAME_MAX + 5 + 0x2000)   // a link's unread bytes: a whole frame and one read
#define FLUSH_ON_STOP_MS 200
#define CLOSE_FLUSH_MS 300       // a link closed after its goodbye gets this long to send it
#define ACCEPT_WAIT_MS 300       // longest a station's ACCEPT waits for the host's game to read the NetworkInfo (it polls every frame)
// What the other end may send. The game sends about 50 datagrams a second per player (docs/coop-step1b.md), so the limits leave 20x room
#define RATE_ACCEPTS 20          // new links per second
#define PENDING_PER_IP 4         // links from one address that still owe their HELLO
#define RATE_FRAMES 2000         // frames per second per link (burst twice that); beyond, datagrams are dropped and other frames ignored
#define RATE_DGRAMS 1000         // game datagrams per second per link, TCP and UDP together (burst twice that)
#define RATE_BYTES (4 << 20)     // their bytes per second (burst twice that)
#define RATE_SCANS 20            // SCAN_REQ per second per link (burst as much); the game scans about once a second
#define RATE_JOINS 5             // JOIN per second per link (burst as much); a faster one is refused (ConnectRejected) unread
#define UDP_READS 512            // packets read per wakeup
#define LOG_GAP_MS 5000          // repeated refusals and drops: one line per this
// Internet play
#define AUTH_HELLO_MS 6000       // HELLO to READY with the room-code handshake (a relay adds a pairing and longer round trips)
#define AUTH_FAILS_MIN 30        // failed proofs a minute for all links (burst as much); beyond, new handshakes are refused until the budget refills
#define AUTH_FAILS_IP_MIN 6      // and from one address
#define AUTH_RETRY_MS 30000      // a station whose code a host refused, or a host that did not prove it, is not dialled again for this long
#define RELAY_WAIT_MS 12000      // a station's link to the relay waits this long for the relay to pair it (the relay gives up after 10 s)
#define RELAY_PING_MS 5000       // the host's control link to the relay: a ping this often; silent for RELAY_TIMEOUT_MS, it is dialled again
#define RELAY_TIMEOUT_MS 20000
#define RELAY_RETRY_MS 2000      // a refused or lost control link: dialled again after this (and up to 15 s while it keeps failing)

static void conn_init(conn *c, csock s, int state) {
    free(c->rx); free(c->tx);
    memset(c, 0, sizeof *c); c->s = s; c->state = state; c->node = -1;
    c->last_rx = c->last_tx = c->opened = cplat_now_ms();
}
static int grow(uint8_t **b, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap : 4096; while (nc < need) nc *= 2;
    uint8_t *p = realloc(*b, nc); if (!p) return -1;
    *b = p; *cap = nc; return 0;
}
static void conn_flush(conn *c) {
    while (c->txn && c->closing < 2) {
        int64_t k = cplat_send(c->s, c->tx, c->txn);
        if (k == -1) break;
        if (k < 0) { c->closing = 2; break; }
        memmove(c->tx, c->tx + k, c->txn - (size_t)k); c->txn -= (size_t)k;
        c->last_tx = cplat_now_ms();
    }
}
void cmp_send_frame(coop_node *n, conn *c, int type, const void *p, size_t len) {
    (void)n;
    size_t tl = c->keyed ? CAUTH_TAG : 0;   // internet play: the tag after the body
    if (c->state < CONN_HELLO || c->closing || len + 1 + tl > MP_FRAME_MAX) return;
    if (c->txn + len + 5 + tl > TX_MAX || grow(&c->tx, &c->txcap, c->txn + len + 5 + tl)) { c->closing = 2; return; }   // a peer that stopped reading is lost
    uint8_t *h = c->tx + c->txn;
    put32(h, (uint32_t)(len + 1 + tl)); h[4] = (uint8_t)type;
    if (len) memcpy(h + 5, p, len);
    if (tl) cauth_frame_tag(&c->ktx, c->seq_tx++, h, h + 5, len, h + 5 + len);
    c->txn += len + 5 + tl;
    conn_flush(c);
}
static void conn_raw(conn *c, const void *p, size_t len) {   // the relay's own messages: unframed, before the pairing (and on its control link)
    if (c->closing || c->txn + len > TX_MAX || grow(&c->tx, &c->txcap, c->txn + len)) { c->closing = 2; return; }
    memcpy(c->tx + c->txn, p, len); c->txn += len;
    conn_flush(c);
}
static void relay_hello(conn *c, int kind, const uint8_t room[16], uint32_t id, uint32_t cookie) {
    uint8_t h[RLY_HELLO_LEN] = {0};
    put32(h, RLY_MAGIC); h[4] = (uint8_t)kind; h[5] = 1; memcpy(h + 8, room, 16); put32(h + 24, id); put32(h + 28, cookie);
    conn_raw(c, h, sizeof h);
}
static const char *relay_err(int code) {
    switch (code) {
    case RLY_E_NOROOM: return "no room with this code is open there (is the host hosting yet?)";
    case RLY_E_TAKEN: return "a host already holds this room";
    case RLY_E_FULL: return "the room is full";
    case RLY_E_LIMIT: return "the relay is at one of its limits";
    case RLY_E_TIMEOUT: return "the host did not answer through the relay in time";
    case RLY_E_EXPIRED: return "the room reached the relay's age or byte limit";
    default: return "it refused the request";
    }
}
static void ipstr(char *o, uint32_t ip) { snprintf(o, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF); }
static const char *peer_tag(peer *p, char *o, size_t n) {   // "192.168.1.20:47600 (Pokopia-1a2b3c4d)" for logs
    char a[16]; ipstr(a, p->ip);
    if (p->name[0]) snprintf(o, n, "%s:%u (%s)", a, p->port, p->name); else snprintf(o, n, "%s:%u", a, p->port);
    return o;
}
static void zap(coop_node *n, csock s) {   // a socket the I/O thread may be polling right now: it closes it after the poll
    if (s == CSOCK_BAD) return;
    if (!n->io_on || n->nzap >= (int)(sizeof n->zap / sizeof n->zap[0])) { cplat_close(s); return; }
    n->zap[n->nzap++] = s;
}
static void bad_input(coop_node *n, conn *c, const char *who, const char *why) {   // a malformed frame: the link is closed
    n->bad_input++;
    if (n->bad_input <= 8 || n->bad_input % 100 == 0) {
        char a[16]; ipstr(a, c->peer_ip);
        coop_logf(n, "mp: %s %s:%u sent %s: link closed (%llu malformed inputs so far)", who, a, c->peer_port, why, (unsigned long long)n->bad_input);
    }
    c->closing = 2;
}
static void rate_drop(coop_node *n, conn *c, const char *what) {
    int64_t now = cplat_now_ms();
    n->rate_drops++; c->drops++;
    if (now - c->drop_log >= LOG_GAP_MS) {
        char a[16]; ipstr(a, c->peer_ip);
        coop_logf(n, "mp: %s:%u over its rate: %s dropped (%llu from this link so far)", a, c->peer_port, what, (unsigned long long)c->drops);
        c->drop_log = now;
    }
}
static int dgram_ok(coop_node *n, conn *c, size_t len) {
    int64_t now = cplat_now_ms();
    if (bucket_take(&c->dgrams, now, RATE_DGRAMS, 2 * RATE_DGRAMS, 1) && bucket_take(&c->bytes, now, RATE_BYTES, 2 * RATE_BYTES, (int64_t)len)) return 1;
    rate_drop(n, c, "game datagrams");
    return 0;
}
int cmp_src_ok(coop_node *n, uint32_t ip) {
    int cls = coop_addr4_class(ip);
    if (coop_addr_allowed(n->cfg.scope, cls)) return 1;
    int64_t now = cplat_now_ms();
    n->refused_src++;
    if (n->refused_src <= 4 || now - n->log_src_at >= LOG_GAP_MS) {
        char a[16]; ipstr(a, ip);
        const char *vpn = n->cfg.scope == COOP_SCOPE_LAN ? coop_addr4_vpn(ip) : "";
        coop_logf(n, "mp: refused %s%s: not a %s address (%llu refused so far)%s", a, vpn, n->cfg.scope == COOP_SCOPE_LOOPBACK ? "loopback" :
                  n->cfg.scope == COOP_SCOPE_INTERNET ? "valid unicast" : "loopback, private or link-local", (unsigned long long)n->refused_src,
                  *vpn ? "; through a VPN, every player turns on internet play" : "");
        n->log_src_at = now;
    }
    return 0;
}
// Failed room-code proofs (internet play): a budget per minute for all links and per address, as thousandths of a failure in a bucket
static int fail_left(bucket *b, int64_t now, int64_t per_min) {
    if (!b->last) { b->last = now; b->milli = per_min * 1000; }
    int64_t add = (now - b->last) * per_min / 60;
    if (add > 0) { b->milli += add; b->last = now; if (b->milli > per_min * 1000) b->milli = per_min * 1000; }
    return b->milli >= 1000;
}
static bucket *fail_ip(coop_node *n, uint32_t ip) {
    int k = 0;
    for (int i = 0; i < 16; i++) { if (n->fail_ip[i].ip == ip) return &n->fail_ip[i].b; if (n->fail_ip[i].b.last < n->fail_ip[k].b.last) k = i; }
    n->fail_ip[k].ip = ip; memset(&n->fail_ip[k].b, 0, sizeof n->fail_ip[k].b);   // the address unseen the longest makes room
    return &n->fail_ip[k].b;
}
static int auth_allowed(coop_node *n, conn *c) {   // may this link try the room code now?
    int64_t now = cplat_now_ms();
    int ok = fail_left(&n->fail_all, now, AUTH_FAILS_MIN) && (c->relay || fail_left(fail_ip(n, c->peer_ip), now, AUTH_FAILS_IP_MIN));
    if (!ok) {
        n->auth_fail++;
        if (now - n->log_auth_at >= LOG_GAP_MS) {
            char a[16]; ipstr(a, c->peer_ip);
            coop_logf(n, "mp: a link from %s closed before its handshake: too many wrong room codes in the last minute%s (%llu failures so far)", a,
                      c->relay ? "" : " (from it, or from everyone)", (unsigned long long)n->auth_fail);
            n->log_auth_at = now;
        }
    }
    return ok;
}
static void auth_failed(coop_node *n, conn *c) {
    int64_t now = cplat_now_ms();
    fail_left(&n->fail_all, now, AUTH_FAILS_MIN); n->fail_all.milli -= 1000;
    if (!c->relay) { bucket *b = fail_ip(n, c->peer_ip); fail_left(b, now, AUTH_FAILS_IP_MIN); b->milli -= 1000; }
    n->auth_fail++;
    if (n->auth_fail <= 4 || now - n->log_auth_at >= LOG_GAP_MS) {
        char a[16]; ipstr(a, c->peer_ip);
        coop_logf(n, "mp: a link from %s:%u%s did not prove the room code (a wrong code, or not a player): closed (%llu failures so far)", a, c->peer_port,
                  c->relay ? " (through the relay)" : "", (unsigned long long)n->auth_fail);
        n->log_auth_at = now;
    }
}
// The identity in WELCOME, SCAN_RESP and JOIN: main's build ID as 16 hex digits (zeros: unknown), the mods fingerprint
static void ident_put(coop_node *n, uint8_t *p) { memset(p, 0, 16); memcpy(p, n->build, strlen(n->build)); put64(p + 16, n->mods); }
static int ident_get(const uint8_t *p, char build[17], uint64_t *mods) {
    int zero = 1;
    for (int i = 0; i < 16; i++) zero &= p[i] == 0;
    if (zero) build[0] = 0;
    else {
        for (int i = 0; i < 16; i++) if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
        memcpy(build, p, 16); build[16] = 0;
    }
    *mods = get64(p + 16);
    return 1;
}
// A datagram's header and payload go to n->scratch + n->uhdr; send_udp puts the UDP header in front (and, in internet play, the tag behind)
static size_t dg_build(coop_node *n, uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport, const uint8_t *p, size_t len) {
    uint8_t *b = n->scratch + n->uhdr;
    put32(b, sip); put32(b + 4, dip); put16(b + 8, sport); put16(b + 10, dport);
    if (len) memcpy(b + MP_DG_HDR, p, len);
    return MP_DG_HDR + len;
}
static void send_udp(coop_node *n, csock s, conn *c, int type, uint64_t token, uint32_t ip, uint16_t port, size_t blen) {
    uint8_t *b = n->scratch;
    if (n->cfg.auth) {
        if (!c->keyed) return;
        put32(b, MP_UMAGIC3); b[4] = (uint8_t)type; b[5] = b[6] = b[7] = 0; put64(b + 8, token); put64(b + 16, ++c->useq_tx);
        cauth_udp_tag(&c->kutx, b, MP_UDP_HDR3 + blen, b + MP_UDP_HDR3 + blen);
        cplat_sendto(s, b, MP_UDP_HDR3 + blen + CAUTH_TAG, ip, port);
        return;
    }
    put32(b, MP_UMAGIC); b[4] = (uint8_t)type; b[5] = b[6] = b[7] = 0; put64(b + 8, token);
    cplat_sendto(s, b, MP_UDP_HDR + blen, ip, port);
}
peer *cmp_up(coop_node *n) { return n->upi >= 0 && n->upi < PEERS && n->pr[n->upi].used ? &n->pr[n->upi] : NULL; }

// ---------- routing ----------
void cmp_route(coop_node *n, int from_node, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, size_t len) {
    int bc = vip_is_bcast(n, dst_ip);
    uint32_t own = vip_of(n, n->self);
    if (n->ls == COOP_LS_AP_CREATED) {   // the hub: its own sockets, then every station the datagram is for, never back to the sender
        if (from_node != n->self ? bc || dst_ip == own : dst_ip == own) cvnet_deliver(n, src_ip, src_port, dst_ip, dst_port, p, len);
        size_t bl = 0;
        for (int i = 0; i < HOST_CONNS; i++) {
            conn *c = &n->hc[i];
            if (c->state != CONN_READY || c->node <= 0 || c->accept_pending || c->closing || c->node == from_node || !(bc || dst_ip == vip_of(n, c->node))) continue;
            if (!bl) bl = dg_build(n, src_ip, src_port, dst_ip, dst_port, p, len);
            if (!n->cfg.tcp_only && c->udp_port && n->husock != CSOCK_BAD) { send_udp(n, n->husock, c, U_DGRAM, c->token, c->udp_ip, c->udp_port, bl); n->st_udp_tx++; }
            else { cmp_send_frame(n, c, M_DGRAM, n->scratch + n->uhdr, bl); n->st_tcp_tx++; }
        }
    } else if (n->ls == COOP_LS_STATION_CONNECTED) {   // a station: everything not for itself goes to the hub
        if (dst_ip == own) { cvnet_deliver(n, src_ip, src_port, dst_ip, dst_port, p, len); return; }
        peer *up = cmp_up(n);
        if (!up) return;
        size_t bl = dg_build(n, src_ip, src_port, dst_ip, dst_port, p, len);
        if (!n->cfg.tcp_only && n->udp_ok && n->susock != CSOCK_BAD) { send_udp(n, n->susock, &up->c, U_DGRAM, n->token, up->ip, up->udp_port, bl); n->st_udp_tx++; }
        else { cmp_send_frame(n, &up->c, M_DGRAM, n->scratch + n->uhdr, bl); n->st_tcp_tx++; }
    }
}
static void host_dgram(coop_node *n, conn *c, const uint8_t *b, size_t len, int udp) {   // a station's datagram: its source address is the station's own
    if (c->node <= 0 || c->accept_pending || len < MP_DG_HDR || len - MP_DG_HDR > COOP_DGRAM_MAX || n->ls != COOP_LS_AP_CREATED) return;
    if (!dgram_ok(n, c, len)) return;
    if (udp) n->st_udp_rx++; else n->st_tcp_rx++;
    cmp_route(n, c->node, vip_of(n, c->node), get16(b + 8), get32(b + 4), get16(b + 10), b + MP_DG_HDR, len - MP_DG_HDR);
}
static void station_dgram(coop_node *n, peer *p, const uint8_t *b, size_t len, int udp) {
    if (n->ls != COOP_LS_STATION_CONNECTED || cmp_up(n) != p || len < MP_DG_HDR || len - MP_DG_HDR > COOP_DGRAM_MAX) return;
    if (!dgram_ok(n, &p->c, len)) return;
    if (udp) n->st_udp_rx++; else n->st_tcp_rx++;
    uint32_t src = get32(b), dst = get32(b + 4);
    if ((src & 0xFFFFFF00u) != n->subnet) return;   // the hub names senders inside the network only
    if (vip_is_bcast(n, dst) || dst == vip_of(n, n->self)) cvnet_deliver(n, src, get16(b + 8), dst, get16(b + 10), b + MP_DG_HDR, len - MP_DG_HDR);
}

// ---------- host side ----------
void cmp_host_sync(coop_node *n) {
    for (int i = 0; i < HOST_CONNS; i++) if (n->hc[i].state == CONN_READY && n->hc[i].node > 0 && !n->hc[i].accept_pending) cmp_send_frame(n, &n->hc[i], M_SYNC, n->info, LDN_NETINFO);
}
static void unjoin(conn *c) { c->node = -1; c->token = 0; c->udp_ip = 0; c->udp_port = 0; c->accept_pending = 0; }
void cmp_host_accepts(coop_node *n) {
    int64_t now = cplat_now_ms();
    for (int i = 0; i < HOST_CONNS; i++) {
        conn *c = &n->hc[i];
        if (c->state != CONN_READY || c->node <= 0 || !c->accept_pending) continue;
        int read = n->info_seen >= c->accept_gen;
        if (!read && now < c->accept_due) continue;
        uint8_t *a = n->scratch;   // also reached from the game's GetNetworkInfo: nothing big on its stack
        a[0] = (uint8_t)c->node; put64(a + 1, c->token); memcpy(a + 9, n->info, LDN_NETINFO);
        cmp_send_frame(n, c, M_ACCEPT, a, 9 + LDN_NETINFO);
        c->accept_pending = 0;
        coop_logf(n, "mp: node %d joined (ACCEPT sent %s)", c->node, read ? "after the game read the NetworkInfo" : "without the game reading the NetworkInfo");
    }
}
void cmp_host_kick(coop_node *n, conn *c, int reason) {
    uint8_t r = (uint8_t)reason;
    coop_logf(n, "mp: node %d removed (disconnect reason %d)", c->node, reason);
    cmp_send_frame(n, c, M_KICK, &r, 1);
    unjoin(c);
}
void cmp_host_drop(coop_node *n, conn *c) {
    if (c->node > 0) coop_logf(n, "mp: node %d left", c->node);
    unjoin(c);
    if (n->ls == COOP_LS_AP_CREATED) { cldn_host_rebuild(n); cmp_host_sync(n); cldn_notify(n); }
}
conn *cmp_host_conn_by_ip(coop_node *n, uint32_t ip) {
    for (int i = 0; i < HOST_CONNS; i++) if (n->hc[i].state == CONN_READY && n->hc[i].node > 0 && vip_of(n, n->hc[i].node) == ip) return &n->hc[i];
    return NULL;
}
static void host_welcome(coop_node *n, conn *c, int full) {   // not full: only the protocol, for a link that is closed next (internet play tells it nothing more)
    uint8_t w[MP_WELCOME_LEN] = {0}; put32(w, MP_MAGIC); put16(w + 4, (uint32_t)n->proto);
    if (full) { put16(w + 6, c->relay ? 0 : n->udp_port); ident_put(n, w + 8); }
    cmp_send_frame(n, c, M_WELCOME, w, sizeof w);
}
static void host_up(coop_node *n, conn *c) {
    char a[16]; ipstr(a, c->peer_ip);
    c->state = CONN_READY;
    host_welcome(n, c, 1);
    if (c->relay) coop_logf(n, "mp: station link %d up through the relay (visitor %u)%s", (int)(c - n->hc), c->rly_id, c->keyed ? ", room code proven" : "");
    else coop_logf(n, "mp: station link %d up from %s:%u%s", (int)(c - n->hc), a, c->peer_port, c->keyed ? ", room code proven" : "");
}
static void host_frame(coop_node *n, conn *c, int type, const uint8_t *b, size_t len) {
    int64_t now = cplat_now_ms();
    if (c->state == CONN_HELLO) {
        if (type != M_HELLO || len < 6 || get32(b) != MP_MAGIC) { bad_input(n, c, "a link from", "no coop HELLO"); return; }
        uint16_t proto = get16(b + 4);
        if (proto != n->proto) {   // told this host's protocol, so the other end can say what to update
            host_welcome(n, c, !n->cfg.auth); c->closing = 1;
            char a[16]; ipstr(a, c->peer_ip);
            if (proto == MP_PROTO_INET || n->proto == MP_PROTO_INET)
                coop_logf(n, "mp: %s speaks coop protocol %u, this host %u: closed (internet play must be on for both players, or off for both)", a, proto, n->proto);
            else coop_logf(n, "mp: %s speaks coop protocol %u, this build %u: closed (both players need the same build of this app)", a, proto, n->proto);
            return;
        }
        if (len < (n->cfg.auth ? MP_HELLO3_LEN : MP_HELLO_LEN)) { bad_input(n, c, "a link from", "a short HELLO"); return; }
        c->platform = b[6]; memcpy(c->uid, b + 7, 16);
        if (n->cfg.auth) {   // internet play: the station proves the room code before anything else is said
            if (!auth_allowed(n, c)) { c->closing = 2; return; }
            memcpy(c->hello, b + 6, 17); memcpy(c->ns, b + MP_HELLO_LEN, CAUTH_NONCE);
            cplat_random(c->nh, CAUTH_NONCE);
            uint8_t ch[MP_CHAL_LEN]; put32(ch, MP_MAGIC); put16(ch + 4, MP_PROTO_INET); memcpy(ch + 6, c->nh, CAUTH_NONCE);
            cmp_send_frame(n, c, M_AUTH_CHAL, ch, sizeof ch);
            c->state = CONN_AUTH;
            return;
        }
        host_up(n, c);
        return;
    }
    if (c->state == CONN_AUTH) {
        if (type != M_AUTH_PROOF || len < 32) { bad_input(n, c, "a link from", "no room-code proof"); return; }
        uint8_t want[32]; cauth_proof(n->k_auth, 0, c->ns, c->nh, c->hello, 17, want);
        int ok = cct_eq(want, b, 32);
        cwipe(want, sizeof want);
        if (!ok) { auth_failed(n, c); cmp_send_frame(n, c, M_AUTH_FAIL, NULL, 0); c->closing = 1; return; }
        uint8_t th[32]; cauth_proof(n->k_auth, 1, c->ns, c->nh, NULL, 0, th);
        cmp_send_frame(n, c, M_AUTH_OK, th, 32);   // the last frame without a tag
        cauth_link_keys(c, n->k_auth, c->ns, c->nh, 1);
        n->auth_ok++;
        host_up(n, c);
        return;
    }
    if (!bucket_take(&c->frames, now, RATE_FRAMES, 2 * RATE_FRAMES, 1)) { rate_drop(n, c, "frames"); return; }
    switch (type) {
    case M_SCAN_REQ: {
        if (!bucket_take(&c->scans, now, RATE_SCANS, RATE_SCANS, 1)) { rate_drop(n, c, "scan requests"); break; }
        uint8_t *r = n->scratch;   // 1 + 24 + 0x480 bytes
        r[0] = n->ls == COOP_LS_AP_CREATED && n->have_info;
        ident_put(n, r + 1);
        if (r[0]) memcpy(r + 1 + MP_IDENT_LEN, n->info, LDN_NETINFO);
        cmp_send_frame(n, c, M_SCAN_RESP, r, 1 + MP_IDENT_LEN + (r[0] ? LDN_NETINFO : 0));
        break;
    }
    case M_JOIN: {
        if (len < MP_JOIN_LEN) { bad_input(n, c, "station", "a short JOIN"); return; }
        if (!bucket_take(&c->joins, now, RATE_JOINS, RATE_JOINS, 1)) { uint8_t r[4]; put32(r, COOP_LDN_CONNECT_REJECTED); cmp_send_frame(n, c, M_REFUSE, r, 4); rate_drop(n, c, "joins"); break; }
        char sb[17], name[40]; uint64_t sm;
        if (!ident_get(b + 0x5A, sb, &sm)) { bad_input(n, c, "station", "a JOIN with a bad build ID"); return; }
        uint16_t lcv = get16(b + 0x50);
        cldn_name(name, b + 0x20, 32);
        cldn_ident(n);
        uint32_t res = c->node > 0 ? COOP_LDN_INVALID_STATE : cldn_host_join(n, c, b, b + 0x20, lcv, get64(b + 0x52), sb, sm);
        if (res) {
            uint8_t r[4]; put32(r, res); cmp_send_frame(n, c, M_REFUSE, r, 4);
            n->refused_join++;
            coop_logf(n, "mp: join from link %d (\"%s\", LocalCommunicationVersion %u) refused: %#x", (int)(c - n->hc), name, lcv, res);
            break;
        }
        // ACCEPT waits until the host's game has read a NetworkInfo that lists the station: KTGL knows senders only from its member table, and the
        // station sends its first datagram as soon as Connect returns
        c->accept_pending = 1; c->accept_gen = n->info_gen; c->accept_due = now + ACCEPT_WAIT_MS;
        for (int i = 0; i < HOST_CONNS; i++) if (&n->hc[i] != c && n->hc[i].state == CONN_READY && n->hc[i].node > 0 && !n->hc[i].accept_pending) cmp_send_frame(n, &n->hc[i], M_SYNC, n->info, LDN_NETINFO);
        uint32_t ip = vip_of(n, c->node);
        coop_logf(n, "mp: node %d joins (\"%s\", LocalCommunicationVersion %u) as 169.254.%u.%u; %u of %u nodes", c->node, name, lcv, ip >> 8 & 0xFF, ip & 0xFF,
                  n->info[0x67], n->info[0x66]);
        break;
    }
    case M_LEAVE: if (c->node > 0) cmp_host_drop(n, c); break;
    case M_DGRAM: host_dgram(n, c, b, len, 0); break;
    case M_PING: cmp_send_frame(n, c, M_PONG, NULL, 0); break;
    default: break;   // PONG, and types of later protocols
    }
}
static void host_accept(coop_node *n) {
    csock s; uint32_t ip; uint16_t port;
    while ((s = cplat_tcp_accept(n->lsock, &ip, &port)) != CSOCK_BAD) {
        int64_t now = cplat_now_ms();
        if (!cmp_src_ok(n, ip)) { cplat_close(s); continue; }
        int pending = 0; conn *slot = NULL;
        for (int j = 0; j < HOST_CONNS; j++) {
            if (n->hc[j].state == CONN_FREE) { if (!slot) slot = &n->hc[j]; }
            else if ((n->hc[j].state == CONN_HELLO || n->hc[j].state == CONN_AUTH) && n->hc[j].peer_ip == ip) pending++;
        }
        if (!slot || pending >= PENDING_PER_IP || !bucket_take(&n->accepts, now, RATE_ACCEPTS, RATE_ACCEPTS, 1)) {
            n->rate_drops++;
            if (now - n->log_junk_at >= LOG_GAP_MS) {
                char a[16]; ipstr(a, ip);
                coop_logf(n, "mp: a link from %s closed at once: %s", a, !slot ? "every slot is taken" : pending >= PENDING_PER_IP ? "it has links waiting for their HELLO" : "too many new links a second");
                n->log_junk_at = now;
            }
            cplat_close(s); continue;
        }
        conn_init(slot, s, CONN_HELLO); slot->peer_ip = ip; slot->peer_port = port;
    }
}
int cmp_host_open(coop_node *n) {
    if (n->hosting) return 0;
    for (int i = 0; i < 25 && n->nzap && n->io_on; i++) cldn_wait(n, 2000000);   // the last network's sockets close after the I/O thread's poll
    if (n->cfg.relay_ip) {   // through the relay: nothing listens here; the I/O thread opens the room and dials a link per visitor
        n->hosting = 1; n->rc_open = 0; n->rc_next = 0; n->rc_fails = 0; n->rc_said = 0; n->udp_port = 0; n->bound_port = 0;
        coop_logf(n, "mp: hosting through the relay %u.%u.%u.%u:%u (nothing listens on this device); game build %s, mods %016llx", n->cfg.relay_ip >> 24,
                  n->cfg.relay_ip >> 16 & 0xFF, n->cfg.relay_ip >> 8 & 0xFF, n->cfg.relay_ip & 0xFF, n->cfg.relay_port, n->build[0] ? n->build : "unknown", (unsigned long long)n->mods);
        return 0;
    }
    int lo = n->cfg.role == COOP_ROLE_HOST || n->cfg.bind_loopback;
    uint32_t ip = lo ? 0x7F000001u : 0;
    n->lsock = cplat_tcp_listen(ip, n->cfg.port, &n->bound_port);
    if (n->lsock == CSOCK_BAD && n->cfg.role == COOP_ROLE_AUTO && n->cfg.port) {
        coop_logf(n, "mp: port %u is taken (another instance?): the system picks one; DNS-SD tells visitors, join by address needs the port from this log", n->cfg.port);
        n->lsock = cplat_tcp_listen(ip, 0, &n->bound_port);
    }
    if (n->lsock == CSOCK_BAD) { coop_logf(n, "mp: cannot listen on port %u", n->cfg.port); return -1; }
    if (n->cfg.role == COOP_ROLE_HOST && !n->cfg.port) n->cfg.port = n->bound_port;   // a port the system picked stays this host's: stations were given it
    n->husock = cplat_udp_open(ip, n->bound_port, &n->udp_port);
    if (n->husock == CSOCK_BAD) n->husock = cplat_udp_open(ip, 0, &n->udp_port);
    if (n->husock == CSOCK_BAD) n->udp_port = 0;   // stations stay on their control links
    n->hosting = 1;
    n->disc.adv_port = n->bound_port;
    n->disc.adv_want = n->cfg.role == COOP_ROLE_AUTO && n->cfg.discovery;
    if (n->cfg.scope == COOP_SCOPE_INTERNET)
        coop_logf(n, "mp: hosting on %s:%u (TCP control), UDP datagrams on %u; links from any address, kept only when they prove the room code; game build %s, "
                     "mods %016llx", lo ? "127.0.0.1" : "every interface", n->bound_port, n->udp_port, n->build[0] ? n->build : "unknown", (unsigned long long)n->mods);
    else coop_logf(n, "mp: hosting on %s:%u (TCP control), UDP datagrams on %u; links from %s addresses only%s; game build %s, mods %016llx", lo ? "127.0.0.1" : "every interface",
                   n->bound_port, n->udp_port, n->cfg.scope == COOP_SCOPE_LOOPBACK ? "loopback" : "loopback, private (RFC 1918) and link-local",
                   n->cfg.auth ? " that prove the room code" : "", n->build[0] ? n->build : "unknown", (unsigned long long)n->mods);
#ifdef _WIN32
    if (!lo) coop_logf(n, "mp: Windows asks once whether to allow this app on networks: allow Private networks only (docs/coop-step1c.md)");
#endif
    return 0;
}
void cmp_host_close(coop_node *n) {
    if (!n->hosting) return;
    n->hosting = 0; n->disc.adv_want = 0;
    if (n->rc.state != CONN_FREE && !n->rc.closing) { n->rc.closing = 1; coop_logf(n, "mp: the relay room closes"); }   // the relay forgets the room
    n->rc_open = 0;
    zap(n, n->lsock); zap(n, n->husock);
    n->lsock = n->husock = CSOCK_BAD; n->bound_port = n->udp_port = 0;
    int k = 0;
    for (int i = 0; i < HOST_CONNS; i++) if (n->hc[i].state != CONN_FREE) { if (!n->hc[i].closing) n->hc[i].closing = 1; k++; }   // after their KICKs
    coop_logf(n, "mp: stopped listening; %d link(s) closed", k);
}

// ---------- station side ----------
void cmp_station_leave(coop_node *n) {
    peer *up = cmp_up(n);
    if (up && up->c.state == CONN_READY) cmp_send_frame(n, &up->c, M_LEAVE, NULL, 0);
    n->token = 0; n->udp_ok = 0; n->self = 0; n->upi = -1;
}
int cmp_peer_cfg(coop_node *n) {
    peer *p = &n->pr[0];
    conn_init(&p->c, CSOCK_BAD, CONN_FREE);
    p->used = 1; p->src = PEER_CFG; p->ip = n->cfg.addr; p->port = n->cfg.port; p->compat = 0; p->name[0] = 0;
    if (n->cfg.relay_ip) { p->ip = n->cfg.relay_ip; p->port = n->cfg.relay_port; p->relay = 1; snprintf(p->name, sizeof p->name, "the relay's room"); }
    return 0;
}
static void peer_say(coop_node *n, peer *p, int what, const char *fmt, const char *why) {   // a relay or room-code problem: a line when it changes
    if (p->said == what) return;
    p->said = what;
    char t[96]; coop_logf(n, fmt, peer_tag(p, t, sizeof t), why);
}
static void peer_compat(coop_node *n, peer *p) {
    char t[96]; const char *why = NULL;
    if (n->build[0] && p->build[0] && strncmp(n->build, p->build, 16)) why = "game build";
    else if (p->mods != n->mods) why = "mods or cheats";
    int was = p->compat; p->compat = why ? -1 : 1;
    if (why && was != -1)
        coop_logf(n, "mp: host %s hidden from scans: its %s differs (host %s, mods %016llx; this one %s, mods %016llx)", peer_tag(p, t, sizeof t), why,
                  p->build[0] ? p->build : "unknown", (unsigned long long)p->mods, n->build[0] ? n->build : "unknown", (unsigned long long)n->mods);
    if (!why && was == -1) coop_logf(n, "mp: host %s matches this build again", peer_tag(p, t, sizeof t));
}
static int info_lists(coop_node *n, const uint8_t *info, int node) {   // the NetworkInfo has this station at its node id and address
    const uint8_t *nd = info + 0x50 + 0x18 + 0x40 * node;
    (void)n;
    return node > 0 && node < LDN_NODES && nd[0xB] && nd[0xA] == node && get32(nd) == ((get32(info + 0x68) & 0xFFFFFF00u) | (uint32_t)(node + 1));
}
static uint32_t refuse_code(uint32_t r) {   // a host's REFUSE: only nn::ldn's Connect results reach the game
    switch (r) {
    case COOP_LDN_CONNECT_NOT_FOUND: case COOP_LDN_CONNECT_REJECTED: case COOP_LDN_VERSION_TOO_LOW: case COOP_LDN_VERSION_TOO_HIGH:
    case COOP_LDN_TOO_MANY_PLAYERS: case COOP_LDN_INVALID_STATE: return r;
    default: return COOP_LDN_CONNECT_REJECTED;
    }
}
static void station_frame(coop_node *n, peer *p, int type, const uint8_t *b, size_t len) {
    conn *c = &p->c; int idx = (int)(p - n->pr); int64_t now = cplat_now_ms(); char t[96];
    if (c->state == CONN_HELLO && n->cfg.auth && !c->keyed && type == M_AUTH_CHAL) {   // internet play: prove the room code
        if (len < MP_CHAL_LEN || get32(b) != MP_MAGIC || get16(b + 4) != MP_PROTO_INET) { bad_input(n, c, "host", "a bad room-code challenge"); return; }
        memcpy(c->nh, b + 6, CAUTH_NONCE);
        uint8_t pr[32]; cauth_proof(n->k_auth, 0, c->ns, c->nh, c->hello, 17, pr);
        cmp_send_frame(n, c, M_AUTH_PROOF, pr, 32);
        cwipe(pr, sizeof pr);
        c->state = CONN_AUTH;
        return;
    }
    if (c->state == CONN_AUTH) {   // the host's answer: its own proof, or a refusal of this one
        if (type == M_AUTH_FAIL) {
            n->auth_fail++; p->fails++; p->dead_until = now + AUTH_RETRY_MS; c->closing = 1;
            peer_say(n, p, 10, "mp: host %s refused the room code: %s", "check DM_COOP_ROOM against the code the host shows (dialled again in 30 s)");
            return;
        }
        if (type != M_AUTH_OK || len < 32) { bad_input(n, c, "host", "no answer to the room-code proof"); return; }
        uint8_t want[32]; cauth_proof(n->k_auth, 1, c->ns, c->nh, NULL, 0, want);
        int ok = cct_eq(want, b, 32);
        cwipe(want, sizeof want);
        if (!ok) {
            n->auth_fail++; p->fails++; p->dead_until = now + AUTH_RETRY_MS; c->closing = 2;
            peer_say(n, p, 11, "mp: host %s %s", "did not prove the room code: not the host this code belongs to (dialled again in 30 s)");
            return;
        }
        cauth_link_keys(c, n->k_auth, c->ns, c->nh, 0);
        n->auth_ok++;
        c->state = CONN_HELLO;   // the WELCOME comes next, tagged
        return;
    }
    if (c->state == CONN_HELLO) {
        if (type != M_WELCOME || len < 6 || get32(b) != MP_MAGIC) { bad_input(n, c, "host", "no coop WELCOME"); return; }
        uint16_t proto = get16(b + 4);
        if (proto != n->proto) {   // not dialled again while this peer lasts (a DNS-SD entry that comes back is a new one)
            if (proto == MP_PROTO_INET || n->proto == MP_PROTO_INET)
                coop_logf(n, "mp: host %s speaks coop protocol %u, this build %u: hidden from scans (internet play must be on for both players, or off for both)",
                          peer_tag(p, t, sizeof t), proto, n->proto);
            else coop_logf(n, "mp: host %s speaks coop protocol %u, this build %u: hidden from scans (both players need the same build of this app)",
                           peer_tag(p, t, sizeof t), proto, n->proto);
            p->compat = -2; c->closing = 1;
            return;
        }
        if (n->cfg.auth && !c->keyed) { bad_input(n, c, "host", "a WELCOME without the room-code handshake"); return; }
        if (len < MP_WELCOME_LEN || !ident_get(b + 8, p->build, &p->mods)) { bad_input(n, c, "host", "a short or bad WELCOME"); return; }
        p->udp_port = p->relay ? 0 : get16(b + 6);   // through a relay, game datagrams stay on the link
        c->state = CONN_READY; p->fails = 0; p->dead_until = 0; p->said = 0;
        peer_compat(n, p);
        coop_logf(n, "mp: host link up: %s%s", peer_tag(p, t, sizeof t), c->keyed ? ", room code proven" : "");
        return;
    }
    if (!bucket_take(&c->frames, now, RATE_FRAMES, 2 * RATE_FRAMES, 1)) { rate_drop(n, c, "frames"); return; }
    switch (type) {
    case M_SCAN_RESP: {
        if (len < 1 + MP_IDENT_LEN || (b[0] && len < 1 + MP_IDENT_LEN + LDN_NETINFO)) { bad_input(n, c, "host", "a short SCAN_RESP"); return; }
        if (!ident_get(b + 1, p->build, &p->mods)) { bad_input(n, c, "host", "a SCAN_RESP with a bad build ID"); return; }
        if (!p->scan_wait) break;
        peer_compat(n, p);
        p->scan_has = 0;
        if (b[0]) {
            uint8_t *info = n->scratch; char why[96];
            memcpy(info, b + 1 + MP_IDENT_LEN, LDN_NETINFO);
            if (cldn_info_sane(info, why, sizeof why)) { memcpy(p->scan_info, info, LDN_NETINFO); p->scan_has = 1; }
            else { n->bad_input++; coop_logf(n, "mp: host %s sent a NetworkInfo the game must not read (%s): not listed", peer_tag(p, t, sizeof t), why); }
        }
        p->scan_done = 1;
        break;
    }
    case M_ACCEPT: {
        if (len < 9 + LDN_NETINFO) { bad_input(n, c, "host", "a short ACCEPT"); return; }
        if (!n->join_wait || n->join_peer != idx || n->ls != COOP_LS_STATION || n->join_done) { cmp_send_frame(n, c, M_LEAVE, NULL, 0); break; }   // Connect gave up already
        uint8_t *info = n->scratch; char why[96];
        memcpy(info, b + 9, LDN_NETINFO);
        int ok = cldn_info_sane(info, why, sizeof why);
        if (ok && !info_lists(n, info, b[0])) { ok = 0; snprintf(why, sizeof why, "node %u is not this station", b[0]); }
        if (ok && memcmp(info, p->scan_info, 0x20)) { ok = 0; snprintf(why, sizeof why, "another network than the one scanned"); }
        if (!ok) {
            n->bad_input++;
            coop_logf(n, "mp: host %s sent an ACCEPT the game must not read (%s): not joined", peer_tag(p, t, sizeof t), why);
            cmp_send_frame(n, c, M_LEAVE, NULL, 0);
            n->join_result = COOP_LDN_CONNECT_REJECTED; n->join_done = 1;
            break;
        }
        n->self = b[0]; n->token = get64(b + 1);
        memcpy(n->info, info, LDN_NETINFO); n->have_info = 1; n->info_gen++;
        n->subnet = get32(n->info + 0x68) & 0xFFFFFF00u;   // node 0's address
        n->upi = idx;
        n->udp_ok = 0; n->next_uhello = now; n->uhello_until = now + UHELLO_FOR_MS;
        if (!n->cfg.tcp_only && p->udp_port && n->susock == CSOCK_BAD)
            n->susock = cplat_udp_open(coop_addr4_class(p->ip) == COOP_ADDR_LOOPBACK ? 0x7F000001u : 0, 0, NULL);
        n->reason = COOP_DR_NONE;
        cldn_set_state(n, COOP_LS_STATION_CONNECTED);
        n->join_result = 0; n->join_done = 1;
        break;
    }
    case M_REFUSE:
        if (len < 4) { bad_input(n, c, "host", "a short REFUSE"); return; }
        if (n->join_wait && n->join_peer == idx && !n->join_done) { n->join_result = refuse_code(get32(b)); n->join_done = 1; }
        break;
    case M_SYNC:
        if (len < LDN_NETINFO) { bad_input(n, c, "host", "a short SYNC"); return; }
        if (n->ls == COOP_LS_STATION_CONNECTED && n->upi == idx) {
            uint8_t *info = n->scratch; char why[96];
            memcpy(info, b, LDN_NETINFO);
            if (!cldn_info_sane(info, why, sizeof why) || !info_lists(n, info, n->self) || (get32(info + 0x68) & 0xFFFFFF00u) != n->subnet || memcmp(info, n->info, 0x20)) {
                n->bad_input++;
                if (n->bad_input <= 8) coop_logf(n, "mp: host %s sent a NetworkInfo update the game must not read: ignored", peer_tag(p, t, sizeof t));
                break;
            }
            memcpy(n->info, info, LDN_NETINFO); n->info_gen++; cldn_notify(n);
        }
        break;
    case M_KICK:
        if (n->upi == idx) { int r = len ? b[0] : COOP_DR_DESTROYED_BY_USER; cldn_station_lost(n, r >= COOP_DR_BY_USER && r <= COOP_DR_SIGNAL_LOST ? r : COOP_DR_SIGNAL_LOST); }
        break;
    case M_DGRAM: station_dgram(n, p, b, len, 0); break;
    case M_PING: cmp_send_frame(n, c, M_PONG, NULL, 0); break;
    default: break;
    }
}

// ---------- the I/O thread ----------
static void conn_close(coop_node *n, conn *c, peer *p, const char *why) {
    char t[96];
    if (p) {
        if (c->state == CONN_READY) coop_logf(n, "mp: host link down: %s (%s)", peer_tag(p, t, sizeof t), why);
        if (cmp_up(n) == p) { cldn_station_lost(n, COOP_DR_SIGNAL_LOST); n->udp_ok = 0; }
        if (p->scan_wait) p->scan_done = 1;
        if (n->join_wait && n->join_peer == (int)(p - n->pr) && !n->join_done) { n->join_result = COOP_LDN_CONNECT_NOT_FOUND; n->join_done = 1; }
    } else {
        if (c->state == CONN_READY) { char a[16]; ipstr(a, c->peer_ip); coop_logf(n, "mp: station link %d down: %s (%s)", (int)(c - n->hc), a, why); }
        if (c->node > 0) cmp_host_drop(n, c);
    }
    cplat_close(c->s);
    conn_init(c, CSOCK_BAD, CONN_FREE);
}
static void conn_read(coop_node *n, conn *c, peer *p) {
    for (int round = 0; round < 64 && (c->closing == 0 || c->closing == 3); round++) {   // a link being closed after its goodbye reads nothing more
        int more = 0;
        if (c->rxn < RX_MAX && !c->closing) {   // never more than a frame and a read unparsed: a peer cannot make the buffer grow
            if (grow(&c->rx, &c->rxcap, RX_MAX)) { c->closing = 2; return; }
            int64_t k = cplat_recv(c->s, c->rx + c->rxn, RX_MAX - c->rxn);
            if (k == 0 || k < -1) c->closing = 3;   // what arrived before the end is still read
            else if (k > 0) { c->rxn += (size_t)k; c->last_rx = cplat_now_ms(); more = c->rxn == RX_MAX; }
        }
        size_t off = 0;
        if (c->state == CONN_RELAY && p && c->rxn >= RLY_MSG_LEN) {   // the relay's answer to a station's JOIN: paired with the host, or why not
            const uint8_t *m = c->rx;
            int64_t now = cplat_now_ms();
            off = RLY_MSG_LEN;
            if (get32(m) == RLY_MAGIC && m[4] == RLY_OK) {
                uint8_t h[MP_HELLO3_LEN]; put32(h, MP_MAGIC); put16(h + 4, (uint32_t)n->proto); h[6] = n->cfg.platform; memcpy(h + 7, n->cfg.uid, 16);
                memcpy(c->hello, h + 6, 17); cplat_random(c->ns, CAUTH_NONCE); memcpy(h + MP_HELLO_LEN, c->ns, CAUTH_NONCE);
                c->state = CONN_HELLO; c->opened = now;
                cmp_send_frame(n, c, M_HELLO, h, n->cfg.auth ? MP_HELLO3_LEN : MP_HELLO_LEN);
            } else {
                int code = get32(m) == RLY_MAGIC && m[4] == RLY_ERR ? m[5] : RLY_E_BAD;
                peer_say(n, p, 100 + code, "mp: %s: the relay says %s", relay_err(code));
                p->fails++; p->dead_until = now + (code == RLY_E_NOROOM || code == RLY_E_TIMEOUT ? 2000 : 5000);   // within the relay's lookups a minute
                c->closing = 2;
            }
        }
        size_t tl = c->keyed ? CAUTH_TAG : 0;
        while (c->state != CONN_RELAY && c->rxn - off >= 5 && (c->closing == 0 || c->closing == 3)) {
            uint32_t fl = get32(c->rx + off);
            if (fl < 1 + tl || fl > MP_FRAME_MAX) { bad_input(n, c, p ? "host" : "a link from", "a bad frame length"); break; }
            if (c->rxn - off < 4 + (size_t)fl) break;
            const uint8_t *b = c->rx + off + 5;
            size_t bl = fl - 1 - tl;
            if (tl) {   // internet play: the tag over this direction's frame count, the header and the body
                uint8_t t[CAUTH_TAG]; cauth_frame_tag(&c->krx, c->seq_rx, c->rx + off, b, bl, t);
                if (!cct_eq(t, b + bl, CAUTH_TAG)) { n->bad_tag++; bad_input(n, c, p ? "host" : "a link from", "a frame whose tag does not verify (changed on the way, repeated or dropped)"); break; }
                c->seq_rx++;
            }
            if (p) station_frame(n, p, c->rx[off + 4], b, bl); else host_frame(n, c, c->rx[off + 4], b, bl);
            off += 4 + fl;
            if (c->state == CONN_FREE) return;
            tl = c->keyed ? CAUTH_TAG : 0;   // keyed from the frame after AUTH_OK on
        }
        if (off) { memmove(c->rx, c->rx + off, c->rxn - off); c->rxn -= off; }
        if (!more) break;
    }
}
// Internet play's datagrams: the token finds the link, its key checks the tag over header and body, the window refuses repeats. A tagged datagram
// from a new endpoint moves the link there (a NAT that changed its mapping), since nobody without the key can make one
static void udp_read_auth(coop_node *n, int host, uint8_t *b, size_t k, int cut, uint32_t ip, uint16_t port) {
    if (k < MP_UDP_HDR3 + CAUTH_TAG || cut || get32(b) != MP_UMAGIC3) { n->udp_junk++; return; }
    int type = b[4]; uint64_t token = get64(b + 8), seq = get64(b + 16);
    const uint8_t *body = b + MP_UDP_HDR3; size_t blen = k - MP_UDP_HDR3 - CAUTH_TAG;
    conn *c = NULL; peer *up = NULL;
    if (host) {
        for (int j = 0; j < HOST_CONNS && !c; j++) if (n->hc[j].state == CONN_READY && n->hc[j].node > 0 && n->hc[j].token == token && !n->hc[j].closing && n->hc[j].keyed) c = &n->hc[j];
    } else if ((up = cmp_up(n)) && n->token && token == n->token && up->c.keyed) c = &up->c;
    if (!c) { n->udp_junk++; return; }
    uint8_t t[CAUTH_TAG]; cauth_udp_tag(&c->kurx, b, k - CAUTH_TAG, t);
    if (!cct_eq(t, b + k - CAUTH_TAG, CAUTH_TAG)) { n->bad_tag++; n->udp_junk++; return; }
    if (!cauth_replay_ok(&c->useq_max, &c->uwin, seq)) { n->replays++; return; }
    if (host) {
        if (type == U_HELLO) {
            if (!c->udp_port) coop_logf(n, "mp: node %d datagrams over UDP", c->node);
            c->udp_ip = ip; c->udp_port = port;
            send_udp(n, n->husock, c, U_ACK, token, ip, port, 0);
        } else if (type == U_DGRAM) {
            if (c->udp_port && (c->udp_ip != ip || c->udp_port != port)) {
                int64_t now = cplat_now_ms();
                if (now - n->log_roam_at >= LOG_GAP_MS) { char a[16]; ipstr(a, ip); coop_logf(n, "mp: node %d's datagrams now come from %s:%u", c->node, a, port); n->log_roam_at = now; }
                c->udp_ip = ip; c->udp_port = port;
            }
            if (c->udp_port) host_dgram(n, c, body, blen, 1);
        } else n->udp_junk++;
    } else {
        if (ip != up->ip || port != up->udp_port) { n->udp_junk++; return; }   // the host's own endpoint (it does not move)
        if (type == U_ACK && !n->udp_ok) { n->udp_ok = 1; coop_logf(n, "mp: datagrams over UDP"); }
        else if (type == U_DGRAM) station_dgram(n, up, body, blen, 1);
    }
}
static void udp_read(coop_node *n, int host) {
    uint8_t b[MP_UDP_HDR3 + MP_DG_HDR + COOP_DGRAM_MAX + CAUTH_TAG + 64];   // the I/O thread's own stack
    csock s = host ? n->husock : n->susock;
    for (int i = 0; i < UDP_READS; i++) {
        uint32_t ip; uint16_t port;
        int64_t k = cplat_recvfrom(s, b, sizeof b, &ip, &port);
        if (k == -1 || k == -2) break;
        if (k < 0) { n->udp_junk++; continue; }   // over-long (dropped by the system) or an ICMP report
        if (host && !cmp_src_ok(n, ip)) continue;
        if (n->cfg.auth) { udp_read_auth(n, host, b, (size_t)k, k == (int64_t)sizeof b, ip, port); continue; }
        if (k < MP_UDP_HDR || k == (int64_t)sizeof b || get32(b) != MP_UMAGIC) { n->udp_junk++; continue; }   // short, over-long (cut) or not ours
        int type = b[4]; uint64_t token = get64(b + 8);
        const uint8_t *body = b + MP_UDP_HDR; size_t blen = (size_t)k - MP_UDP_HDR;
        if (host) {
            conn *c = NULL;
            for (int j = 0; j < HOST_CONNS && !c; j++) if (n->hc[j].state == CONN_READY && n->hc[j].node > 0 && n->hc[j].token == token && !n->hc[j].closing) c = &n->hc[j];
            if (!c) { n->udp_junk++; continue; }
            if (type == U_HELLO) {
                if (ip != c->peer_ip) { n->udp_junk++; continue; }   // the endpoint must be the control link's own device
                if (!c->udp_port) coop_logf(n, "mp: node %d datagrams over UDP", c->node);
                c->udp_ip = ip; c->udp_port = port;
                send_udp(n, n->husock, c, U_ACK, token, ip, port, 0);
            } else if (type == U_DGRAM && c->udp_ip == ip && c->udp_port == port) host_dgram(n, c, body, blen, 1);
            else n->udp_junk++;
        } else {
            peer *up = cmp_up(n);
            if (!up || ip != up->ip || port != up->udp_port || !n->token || token != n->token) { n->udp_junk++; continue; }
            if (type == U_ACK && !n->udp_ok) { n->udp_ok = 1; coop_logf(n, "mp: datagrams over UDP"); }
            else if (type == U_DGRAM) station_dgram(n, up, body, blen, 1);
        }
    }
    int64_t now = cplat_now_ms();
    static const char *what = "mp: %llu UDP packets ignored so far (not ours, an unknown token or not the proven endpoint)";
    if (n->udp_junk && now - n->log_junk_at >= LOG_GAP_MS) { coop_logf(n, what, (unsigned long long)n->udp_junk); n->log_junk_at = now; }
}
static void peer_free(coop_node *n, peer *p) {
    if (p->c.state != CONN_FREE) { cplat_close(p->c.s); }
    conn_init(&p->c, CSOCK_BAD, CONN_FREE);
    memset(p, 0, sizeof *p);
    p->c.s = CSOCK_BAD; p->c.node = -1;
    (void)n;
}
static void peer_failed(coop_node *n, peer *p) {
    int64_t now = cplat_now_ms();
    if (p->src == PEER_DISC && p->nalt > 1) {   // a host on several networks: its next address right away; the back-off counts whole rounds
        char was[16], next[16]; ipstr(was, p->ip);
        p->alti = (p->alti + 1) % p->nalt; p->ip = p->alt[p->alti]; ipstr(next, p->ip);
        if (!p->fails) coop_logf(n, "mp: host %s does not answer at %s: trying %s", p->name, was, next);
        if (p->alti) { p->dead_until = p->next_dial = now; return; }
    }
    p->fails++;
    int64_t back = (int64_t)DIAL_RETRY_MS * p->fails, cap = p->src == PEER_CFG ? 1000 : DIAL_BACKOFF_MS;   // the configured host comes back quickly
    p->dead_until = now + (back > cap ? cap : back);
    if (p->src == PEER_DISC && p->fails == 6) { char t[96]; coop_logf(n, "mp: host %s does not answer", peer_tag(p, t, sizeof t)); }
}
static void disc_sync(coop_node *n) {   // the station's peers follow what DNS-SD knows while it browses
    cdisc_entry e[DISC_MAX];
    cplat_lock(&n->dlk); memcpy(e, n->disc.e, sizeof e); cplat_unlock(&n->dlk);
    int present[PEERS] = {0};
    for (int i = 0; i < DISC_MAX; i++) {
        if (!e[i].used || !e[i].ip || !e[i].port || !coop_addr_allowed(COOP_SCOPE_LAN, coop_addr4_class(e[i].ip))) continue;
        int k = -1, fr = -1;
        for (int j = 0; j < PEERS; j++) {   // the same host: its address, or its instance name while a dial tries its other addresses
            peer *q = &n->pr[j];
            if (q->used && q->port == e[i].port && (q->ip == e[i].ip || (q->src == PEER_DISC && !strcmp(q->name, e[i].name)))) { k = j; break; }
            if (!q->used && fr < 0) fr = j;
        }
        if (k >= 0 && n->pr[k].src == PEER_DISC) {   // new addresses (the host moved networks): from the best one again
            peer *p = &n->pr[k]; int known = 0;
            for (int x = 0; x < p->nalt; x++) known |= p->alt[x] == e[i].ip;
            if (!known) {
                memcpy(p->alt, e[i].addrs, sizeof p->alt); p->nalt = e[i].naddrs; p->alti = 0;
                if (p->c.state == CONN_FREE) p->ip = e[i].ip;
            }
        }
        if (k < 0 && fr >= 0) {
            peer *p = &n->pr[fr]; peer_free(n, p);
            p->used = 1; p->src = PEER_DISC; p->ip = e[i].ip; p->port = e[i].port;
            memcpy(p->alt, e[i].addrs, sizeof p->alt); p->nalt = e[i].naddrs;
            snprintf(p->name, sizeof p->name, "%s", e[i].name);
            if (e[i].has_txt && e[i].proto != n->proto) {   // the TXT says it speaks another protocol: not dialled
                p->compat = -2; char t[96];
                coop_logf(n, "mp: host %s speaks coop protocol %d, this build %d: hidden from scans", peer_tag(p, t, sizeof t), e[i].proto, n->proto);
            }
            k = fr;
        }
        if (k >= 0) present[k] = 1;
    }
    for (int j = 0; j < PEERS; j++) {
        peer *p = &n->pr[j];
        if (p->used && p->src == PEER_DISC && !present[j] && j != n->upi && !p->scan_wait && !(n->join_wait && n->join_peer == j)) peer_free(n, p);
    }
}
static void station_timers(coop_node *n, int64_t now) {
    int active = n->cfg.role != COOP_ROLE_HOST && (n->ls == COOP_LS_INITIALIZED || n->ls == COOP_LS_STATION || n->ls == COOP_LS_STATION_CONNECTED);
    if (n->cfg.role == COOP_ROLE_AUTO) {
        if (n->browse && active) disc_sync(n);
        else for (int j = 0; j < PEERS; j++) if (n->pr[j].used && n->pr[j].src == PEER_DISC && j != n->upi) peer_free(n, &n->pr[j]);
    }
    for (int j = 0; j < PEERS; j++) {
        peer *p = &n->pr[j]; conn *c = &p->c;
        if (!p->used) continue;
        int want = active && (!p->relay || n->browse);   // the relay's room only while the game looks for one (OpenStation or Scan on): a host-to-be stays out
        if (c->state == CONN_FREE) {
            if (want && p->compat != -2 && now >= p->next_dial && now >= p->dead_until) {   // a link to every host while the game may scan or join
                p->next_dial = now + DIAL_RETRY_MS;
                csock s = cplat_tcp_connect(p->ip, p->port);
                if (s != CSOCK_BAD) { conn_init(c, s, CONN_CONNECTING); c->peer_ip = p->ip; c->peer_port = p->port; } else peer_failed(n, p);
            }
        } else if (!want && j != n->upi && !c->closing) c->closing = 1;
        else if (c->state == CONN_CONNECTING && now - c->opened > CONNECT_TIMEOUT_MS) { conn_close(n, c, p, "connect timeout"); peer_failed(n, p); }
    }
    peer *up = cmp_up(n);
    if (up && n->ls == COOP_LS_STATION_CONNECTED && !n->cfg.tcp_only && !n->udp_ok && n->token && up->udp_port && n->susock != CSOCK_BAD &&
        now < n->uhello_until && now >= n->next_uhello) {
        n->next_uhello = now + UHELLO_MS;
        send_udp(n, n->susock, &up->c, U_HELLO, n->token, up->ip, up->udp_port, 0);
    }
}
static void link_timers(coop_node *n, conn *c, peer *p, int64_t now) {
    if (c->state == CONN_READY) {
        if (now - c->last_tx >= PING_MS) cmp_send_frame(n, c, M_PING, NULL, 0);
        if (now - c->last_rx >= LINK_TIMEOUT_MS) conn_close(n, c, p, "silent for 10 s");
    } else if ((c->state == CONN_HELLO || c->state == CONN_AUTH || c->state == CONN_RELAY) &&
               now - c->opened >= (c->state == CONN_RELAY ? RELAY_WAIT_MS : n->cfg.auth ? AUTH_HELLO_MS : HELLO_TIMEOUT_MS)) {
        if (!p) { n->bad_input++; }
        conn_close(n, c, p, c->state == CONN_RELAY ? "the relay did not pair this link in time" : "no HELLO / WELCOME in time");
        if (p) peer_failed(n, p);
    } else if (c->state == CONN_CONNECTING && !p && now - c->opened > CONNECT_TIMEOUT_MS) conn_close(n, c, NULL, "connect timeout");   // a host's link to the relay
}

// ---------- host through a relay: the control link that keeps the room open while the game hosts ----------
static void rc_close(coop_node *n, const char *why, int quiet) {
    conn *c = &n->rc; int64_t now = cplat_now_ms();
    if (!quiet && n->hosting) {
        if (n->rc_open || now - n->rc_said_at >= LOG_GAP_MS) { coop_logf(n, "mp: the link to the relay closed (%s): dialled again", why); n->rc_said_at = now; }
        n->rc_fails++;
        int64_t back = (int64_t)RELAY_RETRY_MS * n->rc_fails; if (back > 15000) back = 15000;
        if (n->rc_next < now + back) n->rc_next = now + back;
    }
    cplat_close(c->s); conn_init(c, CSOCK_BAD, CONN_FREE);
    n->rc_open = 0;
}
static void rc_msg(coop_node *n, const uint8_t *m) {
    int64_t now = cplat_now_ms();
    if (get32(m) != RLY_MAGIC) { n->bad_input++; n->rc.closing = 2; return; }
    switch (m[4]) {
    case RLY_OK:
        if (!n->rc_open) {
            n->rc_open = 1; n->rc_fails = 0; n->rc_said = 0;
            coop_logf(n, "mp: the relay opened the room (id %02x%02x%02x%02x...): visitors with the room code reach this host through it", n->room_id[0], n->room_id[1],
                      n->room_id[2], n->room_id[3]);
        }
        break;
    case RLY_VISITOR: {   // a visitor asks for this room: a link of its own to the relay, which pairs the two
        uint32_t id = get32(m + 8), cookie = get32(m + 12);
        conn *slot = NULL;
        for (int i = 0; i < HOST_CONNS && !slot; i++) if (n->hc[i].state == CONN_FREE) slot = &n->hc[i];
        if (!slot) { n->rate_drops++; coop_logf(n, "mp: visitor %u through the relay not answered: every link slot is taken", id); break; }
        csock s = cplat_tcp_connect(n->cfg.relay_ip, n->cfg.relay_port);
        if (s == CSOCK_BAD) break;
        conn_init(slot, s, CONN_CONNECTING);
        slot->peer_ip = n->cfg.relay_ip; slot->peer_port = n->cfg.relay_port; slot->relay = 1; slot->rly_id = id; slot->rly_cookie = cookie;
        break;
    }
    case RLY_ERR:
        if (n->rc_said != m[5]) { coop_logf(n, "mp: the relay refused the room: %s", relay_err(m[5])); n->rc_said = m[5]; }
        n->rc.closing = 2; n->rc_open = 0; n->rc_said_at = now;   // said: the close that follows adds no line
        if (m[5] == RLY_E_TAKEN && n->rc_next < now + 10000) n->rc_next = now + 10000;
        break;
    default: break;   // RLY_GONE (the relay closes that visitor's link itself), RLY_PONG
    }
}
static void rc_io(coop_node *n, csock s, int got) {
    conn *c = &n->rc;
    if (c->state == CONN_FREE || c->s != s) return;
    if (c->state == CONN_CONNECTING) {
        if (cplat_tcp_connected(c->s) != 1) { rc_close(n, "the relay cannot be reached", 0); return; }
        c->state = CONN_READY; c->opened = c->last_rx = cplat_now_ms();
        relay_hello(c, RLY_HOST, n->room_id, 0, 0);
        return;
    }
    for (int round = 0; round < 16 && (got & (CP_IN | CP_ERR)) && !c->closing; round++) {
        if (grow(&c->rx, &c->rxcap, 1024)) { c->closing = 2; break; }
        int64_t k = cplat_recv(c->s, c->rx + c->rxn, 1024 - c->rxn);
        if (k == 0 || k < -1) { c->closing = 3; break; }
        if (k < 0) break;
        c->rxn += (size_t)k; c->last_rx = cplat_now_ms();
        size_t off = 0;
        while (c->rxn - off >= RLY_MSG_LEN && !c->closing) { rc_msg(n, c->rx + off); off += RLY_MSG_LEN; }
        memmove(c->rx, c->rx + off, c->rxn - off); c->rxn -= off;
    }
    if (c->state != CONN_FREE && (got & CP_OUT)) conn_flush(c);
}
static void rc_timers(coop_node *n, int64_t now) {
    conn *c = &n->rc;
    if (!n->hosting || !n->cfg.relay_ip) { if (c->state != CONN_FREE && !c->closing) c->closing = 1; }
    else if (c->state == CONN_FREE) {
        if (now >= n->rc_next) {
            n->rc_next = now + RELAY_RETRY_MS;
            csock s = cplat_tcp_connect(n->cfg.relay_ip, n->cfg.relay_port);
            if (s != CSOCK_BAD) { conn_init(c, s, CONN_CONNECTING); c->peer_ip = n->cfg.relay_ip; c->peer_port = n->cfg.relay_port; }
        }
    } else if (c->state == CONN_CONNECTING && now - c->opened > CONNECT_TIMEOUT_MS) rc_close(n, "the relay does not answer", 0);
    else if (c->state == CONN_READY && !c->closing) {
        if (now - c->last_tx >= RELAY_PING_MS) { uint8_t m[RLY_MSG_LEN] = {0}; put32(m, RLY_MAGIC); m[4] = RLY_PING; conn_raw(c, m, sizeof m); c->last_tx = now; }
        if (now - c->last_rx >= RELAY_TIMEOUT_MS) rc_close(n, "the relay went silent", 0);
    }
    if (c->state != CONN_FREE && c->closing) {
        if (c->closing == 1 && !c->close_by) c->close_by = now + CLOSE_FLUSH_MS;
        if (c->closing >= 2 || !c->txn || now >= c->close_by) rc_close(n, c->closing == 3 ? "closed by the relay" : "an error", c->closing == 1);
    }
}

static void timers(coop_node *n) {
    int64_t now = cplat_now_ms();
    if (!n->profile_said && (n->cfg.scope == COOP_SCOPE_LAN || (n->cfg.scope == COOP_SCOPE_INTERNET && !n->cfg.relay_ip)) &&
        ((n->hosting && !n->cfg.bind_loopback) || n->browse || n->cfg.role == COOP_ROLE_JOIN)) {
        n->profile_said = 1;   // once per Initialize: a Public network keeps other devices out, silently. Only the LAN ones count (not a phone's USB link
        caddr mine[16]; int nm = cplat_local_addrs(mine, 16, 1), lan = 0, pub = 0; char pubs[64] = "", oks[64] = "";   // beside the Wi-Fi, say)
        for (int i = 0; i < nm; i++) {
            int cls = coop_addr4_class(mine[i].ip); char a[16]; ipstr(a, mine[i].ip);
            if (cls != COOP_ADDR_PRIVATE && cls != COOP_ADDR_LINKLOCAL) continue;
            char *to = mine[i].is_public ? pubs : oks; size_t l = strlen(to);
            if (l + 17 < sizeof pubs) snprintf(to + l, sizeof pubs - l, "%s%s", l ? ", " : "", a);
            lan++; pub += mine[i].is_public;
        }
        if (lan && pub == lan)
            coop_logf(n, "mp: this PC's network is set to Public: Windows Firewall keeps other devices out (visitors cannot reach a host here, DNS-SD answers may not "
                         "arrive). Make it Private: Settings, Network & internet, your connection, Network profile type: Private (docs/coop-step1c.md)");
        else if (pub) coop_logf(n, "mp: %s is on a Public network (Windows Firewall keeps visitors out there); visitors reach this PC at %s", pubs, oks);
    }
    if (n->cfg.role != COOP_ROLE_HOST) station_timers(n, now);
    if (n->hosting) cmp_host_accepts(n);
    rc_timers(n, now);
    for (int i = 0; i < HOST_CONNS; i++) link_timers(n, &n->hc[i], NULL, now);
    for (int j = 0; j < PEERS; j++) if (n->pr[j].used) link_timers(n, &n->pr[j].c, &n->pr[j], now);
}
static void conn_io(coop_node *n, conn *c, peer *p, csock s, int got) {
    if (c->state == CONN_FREE || c->s != s) return;   // closed or replaced since the poll set was made
    if (c->state == CONN_CONNECTING) {
        if (cplat_tcp_connected(c->s) != 1) { cplat_close(c->s); conn_init(c, CSOCK_BAD, CONN_FREE); if (p) peer_failed(n, p); return; }
        c->opened = c->last_rx = cplat_now_ms();
        if (!p) { c->state = CONN_HELLO; relay_hello(c, RLY_ATTACH, n->room_id, c->rly_id, c->rly_cookie); return; }   // the host's link for one visitor
        if (p->relay) { c->state = CONN_RELAY; relay_hello(c, RLY_JOIN, n->room_id, 0, 0); return; }               // HELLO once the relay pairs it
        c->state = CONN_HELLO;
        uint8_t h[MP_HELLO3_LEN]; put32(h, MP_MAGIC); put16(h + 4, (uint32_t)n->proto); h[6] = n->cfg.platform; memcpy(h + 7, n->cfg.uid, 16);
        if (n->cfg.auth) { memcpy(c->hello, h + 6, 17); cplat_random(c->ns, CAUTH_NONCE); memcpy(h + MP_HELLO_LEN, c->ns, CAUTH_NONCE); }
        cmp_send_frame(n, c, M_HELLO, h, n->cfg.auth ? MP_HELLO3_LEN : MP_HELLO_LEN);
        return;
    }
    if (got & (CP_IN | CP_ERR)) conn_read(n, c, p);
    if (c->state != CONN_FREE && (got & CP_OUT)) conn_flush(c);
}
static void close_due(coop_node *n, conn *c, peer *p, int64_t now) {
    if (c->state == CONN_FREE || !c->closing) return;
    if (c->closing == 1 && !c->close_by) c->close_by = now + CLOSE_FLUSH_MS;
    if (c->closing >= 2 || !c->txn || now >= c->close_by) conn_close(n, c, p, c->closing == 3 ? "closed" : c->closing == 2 ? "error" : "done");
}
enum { PK_LISTEN = 1, PK_HUDP, PK_SUDP, PK_DISC, PK_HC, PK_PEER, PK_RELAY };
static void io_main(void *arg) {
    coop_node *n = arg;
    enum { NF = 5 + HOST_CONNS + PEERS };
    cpollfd f[NF]; int kind[NF], idx[NF];
    cnode_lock(n);
    for (;;) {
        int k = 0, stopping = n->io_stop, pending = 0;
        int64_t now = cplat_now_ms();
        if (stopping && !n->stop_until) n->stop_until = now + FLUSH_ON_STOP_MS;
#define ADD(K, I, S, W) do { f[k].s = (S); f[k].want = (W); kind[k] = (K); idx[k] = (I); k++; } while (0)
        if (!stopping) {
            if (n->lsock != CSOCK_BAD) ADD(PK_LISTEN, 0, n->lsock, CP_IN);
            if (n->husock != CSOCK_BAD) ADD(PK_HUDP, 0, n->husock, CP_IN);
            if (n->susock != CSOCK_BAD) ADD(PK_SUDP, 0, n->susock, CP_IN);
            csock d = cdisc_fd(n); if (d != CSOCK_BAD) ADD(PK_DISC, 0, d, CP_IN);
        }
        for (int i = 0; i < HOST_CONNS; i++) {
            conn *c = &n->hc[i];
            if (c->state == CONN_FREE || (stopping && !c->txn)) continue;
            ADD(PK_HC, i, c->s, (stopping ? 0 : CP_IN) | (c->state == CONN_CONNECTING || c->txn ? CP_OUT : 0)); pending |= c->txn != 0;
        }
        for (int i = 0; i < PEERS; i++) {
            conn *c = &n->pr[i].c;
            if (!n->pr[i].used || c->state == CONN_FREE || (stopping && !c->txn)) continue;
            ADD(PK_PEER, i, c->s, (stopping ? 0 : CP_IN) | (c->state == CONN_CONNECTING || c->txn ? CP_OUT : 0)); pending |= c->txn != 0;
        }
        if (n->rc.state != CONN_FREE && !stopping) ADD(PK_RELAY, 0, n->rc.s, CP_IN | (n->rc.state == CONN_CONNECTING || n->rc.txn ? CP_OUT : 0));
#undef ADD
        if (stopping && (!pending || now >= n->stop_until)) break;   // Finalize: queued goodbyes (KICK, LEAVE) had their moment
        cnode_unlock(n);
        cplat_poll(f, k, IO_TICK_MS);
        cnode_lock(n);
        for (int i = 0; i < k; i++) {
            if (!f[i].got) continue;
            switch (kind[i]) {
            case PK_LISTEN: if (f[i].s == n->lsock) host_accept(n); break;
            case PK_HUDP: if (f[i].s == n->husock) udp_read(n, 1); break;
            case PK_SUDP: if (f[i].s == n->susock) udp_read(n, 0); break;
            case PK_DISC: if (f[i].s == cdisc_fd(n)) cdisc_process(n); break;
            case PK_HC: conn_io(n, &n->hc[idx[i]], NULL, f[i].s, f[i].got); break;
            case PK_PEER: if (n->pr[idx[i]].used) conn_io(n, &n->pr[idx[i]].c, &n->pr[idx[i]], f[i].s, f[i].got); break;
            case PK_RELAY: rc_io(n, f[i].s, f[i].got); break;
            }
        }
        for (int i = 0; i < n->nzap; i++) cplat_close(n->zap[i]);   // sockets a game thread let go while the poll had them
        n->nzap = 0;
        if (stopping) continue;
        now = cplat_now_ms();
        for (int i = 0; i < HOST_CONNS; i++) close_due(n, &n->hc[i], NULL, now);
        for (int i = 0; i < PEERS; i++) if (n->pr[i].used) close_due(n, &n->pr[i].c, &n->pr[i], now);
        timers(n);
        cdisc_tick(n);
        if (n->notify) { cnode_unlock(n); cnode_lock(n); }   // state callbacks owed by what the links did
    }
    cdisc_stop(n);
    cnode_unlock(n);
}

int cmp_start(coop_node *n) {
    n->io_stop = 0; n->stop_until = 0; n->udp_ok = 0; n->token = 0; n->upi = -1; n->nzap = 0; n->browse = 0; n->scan_logged = -1; n->profile_said = 0;
    n->scan_busy = n->join_wait = n->join_done = 0; n->join_peer = -1;
    n->hosting = 0; n->lsock = n->husock = n->susock = CSOCK_BAD; n->bound_port = n->udp_port = 0;
    memset(&n->accepts, 0, sizeof n->accepts);
    n->disc.adv_want = n->disc.browse_want = 0;
    for (int i = 0; i < HOST_CONNS; i++) conn_init(&n->hc[i], CSOCK_BAD, CONN_FREE);
    for (int i = 0; i < PEERS; i++) peer_free(n, &n->pr[i]);
    conn_init(&n->rc, CSOCK_BAD, CONN_FREE); n->rc_open = 0; n->rc_next = 0; n->rc_fails = 0; n->rc_said = 0;
    char a[16]; ipstr(a, n->cfg.addr);
    if (n->cfg.role == COOP_ROLE_JOIN) { cmp_peer_cfg(n); coop_logf(n, "mp: joining %s:%u%s", a, n->cfg.port, n->cfg.auth ? " with the room code" : ""); }
    else if (n->cfg.role == COOP_ROLE_HOST) coop_logf(n, "mp: hosts on 127.0.0.1:%u while the game hosts", n->cfg.port);
    else if (n->cfg.relay_ip) {
        cmp_peer_cfg(n); ipstr(a, n->cfg.relay_ip);
        coop_logf(n, "mp: auto through the relay %s:%u: opens the room there while the game hosts, asks it for the room's host while the game scans", a, n->cfg.relay_port);
    } else coop_logf(n, "mp: auto: hosts on %s (port %u) while the game hosts%s", n->cfg.scope == COOP_SCOPE_INTERNET ? "every interface for internet play" : "the LAN",
                     n->cfg.port, n->cfg.discovery ? ", finds LAN hosts with DNS-SD while it scans" : "; DNS-SD off (DM_COOP_DISCOVERY=0)");
    if (cplat_thread_start(&n->io, io_main, n)) return -1;
    n->io_on = 1;
    return 0;
}
void cmp_stop(coop_node *n) {
    cnode_lock(n);
    if (!n->io_on) { cnode_unlock(n); return; }
    n->io_stop = 1;
    cnode_unlock(n);
    cplat_thread_join(n->io);
    cnode_lock(n);
    for (int i = 0; i < HOST_CONNS; i++) { cplat_close(n->hc[i].s); conn_init(&n->hc[i], CSOCK_BAD, CONN_FREE); }
    for (int i = 0; i < PEERS; i++) peer_free(n, &n->pr[i]);
    cplat_close(n->rc.s); conn_init(&n->rc, CSOCK_BAD, CONN_FREE); n->rc_open = 0;
    for (int i = 0; i < n->nzap; i++) cplat_close(n->zap[i]);
    n->nzap = 0;
    cplat_close(n->lsock); cplat_close(n->husock); cplat_close(n->susock); n->lsock = n->husock = n->susock = CSOCK_BAD;
    n->io_on = 0; n->hosting = 0; n->bound_port = n->udp_port = 0; n->udp_ok = 0; n->token = 0; n->upi = -1;
    cnode_unlock(n);
}
void coop_get_stats(coop_node *n, coop_stats *s) {
    memset(s, 0, sizeof *s);
    cnode_lock(n);
    for (int i = 0; i < HOST_CONNS; i++) if (n->hc[i].state == CONN_READY) { s->links++; s->udp_links += n->hc[i].node > 0 && n->hc[i].udp_port; }
    for (int i = 0; i < PEERS; i++) if (n->pr[i].used && n->pr[i].c.state == CONN_READY) { s->links++; s->udp_links += i == n->upi && n->udp_ok; }
    s->udp_tx = n->st_udp_tx; s->udp_rx = n->st_udp_rx; s->tcp_tx = n->st_tcp_tx; s->tcp_rx = n->st_tcp_rx;
    s->refused_src = n->refused_src; s->refused_join = n->refused_join; s->bad_input = n->bad_input + n->udp_junk; s->rate_drops = n->rate_drops;
    s->auth_ok = n->auth_ok; s->auth_fail = n->auth_fail; s->bad_tag = n->bad_tag; s->replays = n->replays; s->relay_up = n->rc_open;
    cplat_lock(&n->dlk);
    for (int i = 0; i < DISC_MAX; i++) s->discovered += n->disc.e[i].used;
    s->bad_input += n->disc.bad;
    cplat_unlock(&n->dlk);
    s->advertising = n->disc.adv_on;
    cnode_unlock(n);
}
