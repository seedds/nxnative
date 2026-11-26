// coop internals shared by coop_ldn.c, coop_vnet.c, coop_mpnet.c, coop_disc.c, coop_plat.c and coop_auth.c (MIT, our own code). Not for the HLE: it
// includes the platform's socket headers.
#pragma once
#include "coop.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// ---------- platform layer (coop_plat.c): Winsock + Win32 threads, or BSD sockets + pthreads ----------
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET csock;
#define CSOCK_BAD INVALID_SOCKET
typedef SRWLOCK cmutex;
#define CMUTEX_INIT SRWLOCK_INIT
typedef HANDLE cthread;
#else
#include <pthread.h>
typedef int csock;
#define CSOCK_BAD (-1)
typedef pthread_mutex_t cmutex;
#define CMUTEX_INIT PTHREAD_MUTEX_INITIALIZER
typedef pthread_t cthread;
#endif

enum { CP_IN = 1, CP_OUT = 2, CP_ERR = 4 };
typedef struct { csock s; int want, got; } cpollfd;

int cplat_init(void);                                         // WSAStartup on Windows (counted); 0 on success
void cplat_fini(void);
void cplat_mutex_init(cmutex *m);
void cplat_mutex_destroy(cmutex *m);
void cplat_lock(cmutex *m);
void cplat_unlock(cmutex *m);
int cplat_thread_start(cthread *t, void (*fn)(void *), void *arg);
void cplat_thread_join(cthread t);
int64_t cplat_now_ms(void);                                   // monotonic
void cplat_sleep_ns(int64_t ns);
void cplat_random(void *p, size_t n);
csock cplat_udp_open(uint32_t ip, uint16_t port, uint16_t *bound);      // bound, non-blocking; CSOCK_BAD on failure
csock cplat_tcp_listen(uint32_t ip, uint16_t port, uint16_t *bound);
csock cplat_tcp_accept(csock l, uint32_t *ip, uint16_t *port);           // non-blocking, TCP_NODELAY, with the peer's IPv4 (0 when not IPv4); CSOCK_BAD when none
csock cplat_tcp_connect(uint32_t ip, uint16_t port);                     // non-blocking connect started; CSOCK_BAD on failure
int cplat_tcp_connected(csock s);                                        // after CP_OUT / CP_ERR: 1 connected, -1 failed
int64_t cplat_send(csock s, const void *p, size_t n);                    // bytes, or -1 would block, -2 error
int64_t cplat_recv(csock s, void *p, size_t n);                          // bytes, 0 closed, -1 would block, -2 error
int cplat_sendto(csock s, const void *p, size_t n, uint32_t ip, uint16_t port);   // 0 sent (or dropped by a full buffer), -2 error
int64_t cplat_recvfrom(csock s, void *p, size_t n, uint32_t *ip, uint16_t *port); // bytes, -1 none, -2 error, -3 dropped (too big; Windows)
void cplat_close(csock s);
int cplat_poll(cpollfd *f, int n, int ms);                               // fills got; < 0 on error
int cplat_route_ip(uint32_t *ip);   // the IPv4 this machine sends from towards the default route (or the mDNS group): nothing is sent; 0 on success
typedef struct caddr { uint32_t ip, mask; int is_public; } caddr;
// This machine's IPv4 addresses on interfaces that are up (loopback left out), with their subnet masks. is_public (only asked for with profiles, a
// COM call): Windows, the interface's network has the Public profile (its firewall keeps visitors out there); elsewhere 0. The count (at most
// max), -1 when the list cannot be read
int cplat_local_addrs(caddr *out, int max, int profiles);

// ---------- the node ----------
#define LDN_NETINFO 0x480
#define LDN_NODES 8
#define LDN_ADV_MAX 0x180
#define VIP_PREFIX 0xA9FE0000u    // 169.254.0.0
#define VQ COOP_QUEUE_MAX

typedef struct vdgram { uint32_t src_ip; uint16_t src_port; uint16_t len; uint8_t data[]; } vdgram;
typedef struct vsock {
    int used, nonblock, bcast, bound;
    uint32_t bind_ip; uint16_t port;
    vdgram *q[VQ]; int qh, qn;
    uint64_t sends, recvs, drops, bsends;
} vsock;

// A token bucket in thousandths of a token: rate per second, burst in whole tokens
typedef struct { int64_t milli, last; } bucket;
static inline int bucket_take(bucket *b, int64_t now, int64_t rate, int64_t burst, int64_t n) {
    if (!b->last) { b->last = now; b->milli = burst * 1000; }
    b->milli += (now - b->last) * rate; b->last = now;
    if (b->milli > burst * 1000) b->milli = burst * 1000;
    if (b->milli < n * 1000) return 0;
    b->milli -= n * 1000; return 1;
}

// ---------- internet play's message authentication (coop_auth.c): SHA-256, HMAC-SHA256, HKDF-SHA256 ----------
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } csha;
typedef struct { csha in, out; } chmac;   // a key's hashed inner and outer pads
void csha_init(csha *s);
void csha_update(csha *s, const void *p, size_t n);
void csha_final(csha *s, uint8_t out[32]);
void csha256(const void *p, size_t n, uint8_t out[32]);
void chmac_key(chmac *k, const void *key, size_t n);
void chmac_start(const chmac *k, csha *s);              // then csha_update the message
void chmac_end(const chmac *k, csha *s, uint8_t out[32]);
void chmac_once(const void *key, size_t kn, const void *msg, size_t mn, uint8_t out[32]);   // one message
void chkdf_extract(const void *salt, size_t sn, const void *ikm, size_t in, uint8_t prk[32]);
int chkdf_expand(const uint8_t prk[32], const void *info, size_t infon, uint8_t *out, size_t outn);   // -1 past 255 blocks
int cct_eq(const void *a, const void *b, size_t n);      // constant time
void cwipe(void *p, size_t n);
void croom_keys(const char *norm, uint8_t k_auth[32], uint8_t room_id[16]);
#define CAUTH_TAG 16   // tag bytes on every frame and datagram (HMAC-SHA256 cut to 128 bits, as RFC 4868)
#define CAUTH_NONCE 16

// One TCP control link: on the host, a station's (a scanner until it joins); on a station, its link to one host. RELAY: a station's link to a relay,
// waiting for it to pair the visitor with the host. AUTH: internet play's room-code handshake (host: waiting for the proof; station: for the answer)
enum { CONN_FREE, CONN_CONNECTING, CONN_RELAY, CONN_HELLO, CONN_AUTH, CONN_READY };
typedef struct conn {
    int state;
    csock s;
    uint32_t peer_ip; uint16_t peer_port;   // the other end (host side: checked against the station's UDP endpoint)
    uint8_t *rx, *tx; size_t rxn, rxcap, txn, txcap;
    int64_t last_rx, last_tx, opened;
    int closing;          // 1: flush tx, then close; 2: close now (an error); 3: closed by the peer (the I/O thread does all three)
    int64_t close_by;     // closing 1: closed by then even with bytes unsent
    bucket frames, dgrams, bytes;           // what the other end may send (coop_mpnet.c RATE_*)
    bucket scans, joins;                    // SCAN_REQ and JOIN have their own, tighter limits
    uint64_t drops; int64_t drop_log;
    // host side, once joined
    int node;             // node id 1..7, -1 before JOIN
    uint8_t uid[16], platform, user[0x30];
    uint16_t lcv;
    uint64_t token;
    uint32_t udp_ip; uint16_t udp_port;   // the station's UDP endpoint, learned from its UHELLO; 0 until then
    int accept_pending;   // joined, ACCEPT held until the host's game has read a NetworkInfo listing it (or ACCEPT_WAIT_MS)
    uint64_t accept_gen; int64_t accept_due;
    // internet play: the handshake's nonces, then the link's keys (coop_auth.c cauth_link_keys) and sequence numbers
    uint8_t ns[CAUTH_NONCE], nh[CAUTH_NONCE];   // the station's and the host's nonce
    uint8_t hello[17];    // the station's HELLO platform and Uid, which its proof binds
    int keyed;            // frames and datagrams carry tags from here on
    chmac ktx, krx, kutx, kurx;                  // TCP and UDP, each direction
    uint64_t seq_tx, seq_rx;                     // frames sent and received since keyed (never sent: part of each tag)
    uint64_t useq_tx, useq_max, uwin;            // datagrams: the last sent, the highest received and the replay window below it
    // the relay: host side, a link the host dialled for one visitor (rly_id, rly_cookie from the relay); station side, its link through the relay
    int relay; uint32_t rly_id, rly_cookie;
} conn;
void cauth_proof(const uint8_t k_auth[32], int host, const uint8_t ns[16], const uint8_t nh[16], const uint8_t *hello, size_t hn, uint8_t out[32]);
void cauth_link_keys(conn *c, const uint8_t k_auth[32], const uint8_t ns[16], const uint8_t nh[16], int host);
void cauth_frame_tag(const chmac *k, uint64_t seq, const uint8_t *hdr5, const uint8_t *body, size_t n, uint8_t tag[CAUTH_TAG]);
void cauth_udp_tag(const chmac *k, const uint8_t *pkt, size_t n, uint8_t tag[CAUTH_TAG]);
int cauth_replay_ok(uint64_t *max, uint64_t *win, uint64_t seq);

// A host a station knows: the configured one (JOIN) or one DNS-SD found (AUTO). Its link carries scans, the join and, once joined, the session
#define PEERS 8
#define DISC_ADDRS 4              // a DNS-SD host's addresses kept (a PC on two networks, or a phone's Wi-Fi and its USB link)
enum { PEER_CFG = 1, PEER_DISC };
typedef struct peer {
    int used, src;
    uint32_t ip; uint16_t port;
    conn c;
    int64_t next_dial, dead_until; int fails;
    int compat;                       // from WELCOME and SCAN_RESP: 1 compatible; -1 the build or mods differ (asked again at each scan, not listed);
                                      // -2 another coop protocol (not dialled again); 0 not known yet
    char build[17]; uint64_t mods;    // the host's
    uint16_t udp_port;                // the host's UDP port (WELCOME; 0: none)
    int scan_wait, scan_done, scan_has;
    uint8_t scan_info[LDN_NETINFO];   // the host's NetworkInfo from its last SCAN_RESP, checked sane
    char name[64];                    // the DNS-SD instance (logs)
    int relay;                        // the relay (DM_COOP_RELAY): every dial asks it for the room's host
    uint32_t alt[DISC_ADDRS]; int nalt, alti;   // DNS-SD: the host's addresses, the one on this machine's subnet first; a failed dial tries the next
    int said;                         // the last relay or room-code problem logged (a line when it changes)
} peer;

// DNS-SD (coop_disc.c). The I/O thread starts and stops the backend as adv_want / browse_want say; found hosts land in e[] under the node's dlk
#define DISC_MAX 16
typedef struct cdisc_entry {
    int used;
    char name[64]; uint32_t ifindex;
    uint32_t ip; uint16_t port;
    uint32_t addrs[DISC_ADDRS]; int naddrs;   // every LAN address to try, ip (the one on this machine's subnet, else the first) first
    int proto; char build[17]; uint64_t mods; int has_txt;   // from the TXT record (p=, b=, m=)
    uint32_t txt_ip;      // the TXT's a=: the host's best LAN address (used when no A record answers)
    uint32_t txt_x[DISC_ADDRS - 1]; int txt_nx;   // the TXT's x=: its other LAN addresses, best first
    int64_t seen;         // Windows: last reported (stale entries expire); Apple: 0, gone on its remove event
} cdisc_entry;
typedef struct cdisc {
    int adv_want, browse_want;   // under the node lock
    int adv_on, browse_on;       // the backend's state (I/O thread)
    uint16_t adv_port;
    cdisc_entry e[DISC_MAX];     // under dlk
    uint64_t bad;                // records refused (bad name, TXT, port or address class)
    void *impl;                  // the backend's own state
} cdisc;

#define HOST_CONNS 16
#define MP_UDP_HDR 16   // u32 magic, u8 type, 3 reserved, u64 token
#define MP_DG_HDR 12    // u32 source address, u32 destination address, u16 source port, u16 destination port (virtual, host order)
#define MP_UDP_HDR3 24  // internet play: u32 magic, u8 type, 3 reserved, u64 token, u64 sequence number; the tag follows the body

struct coop_node {
    coop_cfg cfg;
    cmutex lk, life, dlk; // lk: all state below; life: Initialize / Finalize against each other; dlk: disc.e (DNS-SD callbacks on other threads)
    int notify;           // state-change callbacks owed (made after the lock is released)
    // identity compared before a join (hooks.build / hooks.mods)
    char build[17]; uint64_t mods; char mods_what[64];
    // ldn
    int ls, reason;
    uint8_t netcfg[0x20], user[0x30];
    uint64_t sec_hash, challenge;
    uint16_t sec_mode;
    uint8_t adv[LDN_ADV_MAX]; uint16_t advn;
    int policy;
    uint8_t info[LDN_NETINFO]; int have_info;   // host: the network it builds; station: the host's, as last synced
    uint64_t info_gen, info_seen;               // info changes, and the last change the game read (GetNetworkInfo)
    int seen_count;                             // NodeCount the game last read
    uint8_t session[16], server_random[16];
    uint32_t subnet;      // 169.254.S.0, host order; 0 when no network
    int self;             // own node id
    uint16_t self_lcv;
    // transport
    int io_on, io_stop;
    int64_t stop_until;
    cthread io;
    uint8_t scratch[MP_UDP_HDR3 + MP_DG_HDR + COOP_DGRAM_MAX + CAUTH_TAG];   // packet assembly under the lock: game threads' stacks can be small
    // internet play (cfg.auth): the coop protocol spoken (MP_PROTO, or MP_PROTO_INET with the handshake and tags) and the room's keys
    int proto;
    size_t uhdr;          // a datagram's header: MP_UDP_HDR, or MP_UDP_HDR3 with its sequence number
    uint8_t k_auth[32], room_id[16];
    bucket fail_all;      // failed proofs, every link (a budget per minute)
    struct { uint32_t ip; bucket b; } fail_ip[16];   // and per address (links through the relay all come from it: only the first limit)
    uint64_t auth_ok, auth_fail, bad_tag, replays;
    int64_t log_auth_at, log_roam_at;
    // host through a relay (cfg.relay_ip): the control link that keeps its room open while the game hosts
    conn rc;
    int rc_open;          // the relay said the room is open
    int64_t rc_next, rc_said_at; int rc_fails, rc_said;
    // host: listening only while the game hosts (CreateNetwork to DestroyNetwork / CloseAccessPoint / Finalize)
    int hosting;
    csock lsock, husock;  // the listener and the host's UDP socket
    uint16_t bound_port, udp_port;
    conn hc[HOST_CONNS];  // station links
    bucket accepts;
    // station
    peer pr[PEERS];
    int upi;              // the peer joined, -1 none
    csock susock;         // the station's UDP socket (opened at the first join)
    uint64_t token;       // its token from ACCEPT
    int udp_ok;           // UACK seen
    int64_t next_uhello, uhello_until;
    int scan_busy, join_wait, join_done, join_peer; uint32_t join_result;
    int browse;           // auto: DNS-SD browsing wanted (OpenStation to OpenAccessPoint / Finalize)
    int64_t browse_t0;
    int scan_logged;      // hosts the last Scan reported (a line when it changes)
    int profile_said;     // the Public-network hint was logged (once per Initialize)
    // sockets the I/O thread may be polling: it closes them after its poll returns
    csock zap[8]; int nzap;
    uint64_t st_udp_tx, st_udp_rx, st_tcp_tx, st_tcp_rx;
    uint64_t refused_src, refused_join, bad_input, rate_drops, udp_junk;
    int64_t log_src_at, log_junk_at;
    cdisc disc;
    // vnet
    vsock sk[COOP_FD_MAX];
    uint16_t next_eph;
    uint64_t log_dgrams;
};

// coop_ldn.c
void coop_logf(coop_node *n, const char *fmt, ...);
void cnode_lock(coop_node *n);
void cnode_unlock(coop_node *n);              // makes the callbacks owed
void cldn_set_state(coop_node *n, int st);    // with the lock held; owes a callback
void cldn_notify(coop_node *n);
void cldn_host_rebuild(coop_node *n);         // host: rebuild info from its own state and the joined stations
uint32_t cldn_host_join(coop_node *n, conn *c, const uint8_t *netid, const uint8_t *user, uint16_t lcv, uint64_t sec_hash, const char *build, uint64_t mods);
void cldn_station_lost(coop_node *n, int reason);
void cldn_wait(coop_node *n, int64_t ns);     // releases the lock around one sleep slice
void cldn_ident(coop_node *n);                // refresh build and mods from the hooks (lock held)
int cldn_info_sane(uint8_t *info, char *why, size_t n);   // a NetworkInfo from the network: 1 when the game may read it; unused node slots and advertise bytes are zeroed
void cldn_name(char *out, const uint8_t *p, size_t n);         // printable copy of an untrusted name for logs
static inline int vip_is_bcast(const coop_node *n, uint32_t ip) { return ip == 0xFFFFFFFFu || (n->subnet && ip == (n->subnet | 0xFF)); }
static inline uint32_t vip_of(const coop_node *n, int node) { return n->subnet | (uint32_t)(node + 1); }

// coop_vnet.c
void cvnet_deliver(coop_node *n, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, size_t len);
void cvnet_reset(coop_node *n);

// coop_mpnet.c
int cmp_start(coop_node *n);                  // with the lock held: the I/O thread (Initialize)
void cmp_stop(coop_node *n);                  // without the lock: says goodbye, joins the thread, closes everything
int cmp_host_open(coop_node *n);              // with the lock held: listen while the game hosts (CreateNetwork); 0 or -1
void cmp_host_close(coop_node *n);            // with the lock held: stop listening, links closed after their goodbyes (teardown)
void cmp_send_frame(coop_node *n, conn *c, int type, const void *p, size_t len);
void cmp_route(coop_node *n, int from_node, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, size_t len);
void cmp_host_sync(coop_node *n);             // host: SYNC the current info to every joined station
void cmp_host_kick(coop_node *n, conn *c, int reason);
void cmp_host_drop(coop_node *n, conn *c);    // host: a station left; rebuild, sync, notify
conn *cmp_host_conn_by_ip(coop_node *n, uint32_t ip);
void cmp_station_leave(coop_node *n);         // station: LEAVE
void cmp_host_accepts(coop_node *n);          // host: send the ACCEPTs that were waiting for the game to read the NetworkInfo
int cmp_src_ok(coop_node *n, uint32_t ip);    // the scope accepts this peer address (counts and logs a refusal)
int cmp_peer_cfg(coop_node *n);               // station: the configured host as peer 0 (JOIN)
peer *cmp_up(coop_node *n);                   // station: the joined peer, or NULL

// coop_disc.c
void cdisc_init(coop_node *n);                // at create
void cdisc_fini(coop_node *n);                // at destroy: stopped, Windows callbacks drained
void cdisc_tick(coop_node *n);                // I/O thread, lock held: start / stop advertising and browsing as wanted; expire stale entries
void cdisc_stop(coop_node *n);                // I/O thread, lock held, on its way out
csock cdisc_fd(coop_node *n);                 // Apple: the daemon connection for the I/O thread's poll; else CSOCK_BAD
void cdisc_process(coop_node *n);             // Apple: that socket is readable (lock held)
int cdisc_txt_parse(const uint8_t *txt, size_t len, cdisc_entry *e);   // a DNS-SD TXT record (length-prefixed key=value strings): 1 when p, b and m are valid
size_t cdisc_txt_make(uint8_t *out, size_t cap, int proto, const char *build, uint64_t mods, const uint32_t *ip, int nip);   // a= ip[0], x= the rest
// A host's LAN addresses for its record, best first (at most max): those on networks visitors can reach before those on Public ones (Windows), private
// before link-local, the route's own first among equals. Loopback and non-LAN addresses are left out
int cdisc_host_addrs(const caddr *mine, int n, uint32_t route, uint32_t *out, int max);
// A visitor's choice among a host's addresses (in the host's order): the index of the first on one of this machine's subnets, else 0
int cdisc_pick(const uint32_t *cand, int n, const caddr *mine, int nm);
int cdisc_name_ok(const char *s, size_t max);  // an instance name from the network: printable, terminated, short enough

// TCP frames: u32 length of (type + body), u8 type, body. UDP packets: u32 magic, u8 type, 3 reserved, u64 token, body
enum { M_HELLO = 1, M_WELCOME, M_SCAN_REQ, M_SCAN_RESP, M_JOIN, M_ACCEPT, M_REFUSE, M_SYNC, M_LEAVE, M_KICK, M_DGRAM, M_PING, M_PONG };
enum { U_HELLO = 1, U_ACK, U_DGRAM };
#define MP_MAGIC 0x31504350u     // "PCP1"
#define MP_UMAGIC 0x31554350u    // "PCU1"
#define MP_PROTO 2               // 2 (step 1c): WELCOME, SCAN_RESP and JOIN carry the build and mods fingerprint
#define MP_FRAME_MAX 0x4000
#define MP_HELLO_LEN 23          // magic, protocol, platform, Uid
#define MP_WELCOME_LEN 32        // magic, protocol, UDP port, build (16 hex digits), mods
#define MP_IDENT_LEN 24          // build (16 hex digits) + mods (u64)
#define MP_JOIN_LEN (0x5A + MP_IDENT_LEN)   // NetworkId 0x20, UserConfig 0x30, version u16, passphrase hash u64, identity
// Protocol 3 (internet play, docs/coop-step1d.md): HELLO adds the station's nonce; the host answers AUTH_CHAL (its nonce), the station AUTH_PROOF
// (HMAC over both nonces and its HELLO), the host AUTH_OK (its own proof) or AUTH_FAIL. From AUTH_OK on, every frame ends in a tag and every
// datagram has a sequence number and a tag (MP_UMAGIC3). The rest of protocol 2 is unchanged inside the tagged frames
#define MP_PROTO_INET 3
#define MP_HELLO3_LEN (MP_HELLO_LEN + CAUTH_NONCE)
#define MP_CHAL_LEN (6 + CAUTH_NONCE)     // magic, protocol, the host's nonce
#define MP_UMAGIC3 0x33554350u   // "PCU3"
enum { M_AUTH_CHAL = 0x20, M_AUTH_PROOF, M_AUTH_OK, M_AUTH_FAIL };

// The relay (tools/coop-relay, the same values as its coop_relay.h): a link's first 32 bytes say what it is (magic, kind, version 1, 2 reserved,
// room id, visitor id, cookie); the relay answers in 16-byte messages (magic, type, code, 2 reserved, arg, cookie), and once it pairs a visitor's
// link with one its host dialled, it copies bytes between them without reading them
#define RLY_MAGIC 0x31524B50u    // "PKR1"
#define RLY_HELLO_LEN 32
#define RLY_MSG_LEN 16
enum { RLY_HOST = 1, RLY_JOIN, RLY_ATTACH };
enum { RLY_OK = 1, RLY_VISITOR, RLY_GONE, RLY_ERR, RLY_PONG, RLY_PING };
enum { RLY_E_NOROOM = 1, RLY_E_TAKEN, RLY_E_FULL, RLY_E_LIMIT, RLY_E_TIMEOUT, RLY_E_BAD, RLY_E_EXPIRED };

static inline void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static inline void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static inline uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
