// coop: Pokopia's Link Play "Local Connection" between our native builds (research/06 §5, docs/coop-step1.md, docs/coop-step1c.md). MIT, our own code.
// One portable C11 module every port's HLE layer calls: `ldn` (the nn::ldn state machine and the 0x480 NetworkInfo), `vnet` (the game's UDP
// sockets on a virtual 169.254.S.0/24), `mpnet` (the real transport: a star of TCP control links around the host plus UDP for game datagrams)
// and `disc` (DNS-SD on the LAN: Apple dns_sd, Windows DnsService*, nothing elsewhere).
// BSD sockets (macOS, iOS, Android, Linux) or Winsock (Windows). Every entry point is thread-safe; each node has one lock and one I/O thread.
// Peers are loopback or LAN addresses (step 1c), configured by DM_COOP / DM_COOP_TCP / DM_UID; internet play (step 1d, docs/coop-step1d.md) is an
// explicit opt-in (DM_COOP_INTERNET=1): every link proves the host's room code and every frame and datagram carries a tag (coop_auth.c), but
// nothing is encrypted. DM_COOP_RELAY names a relay players host themselves (tools/coop-relay).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coop_node coop_node;

// HOST: listens on 127.0.0.1 while the game hosts (step 1; an SSH tunnel's end). JOIN: dials one configured host. AUTO: hosts on the LAN while the
// game hosts and finds LAN hosts with DNS-SD while it scans
enum { COOP_ROLE_NONE, COOP_ROLE_HOST, COOP_ROLE_JOIN, COOP_ROLE_AUTO };
// Which addresses may connect or be dialed. INTERNET (DM_COOP_INTERNET=1 only) takes any address, and only links that prove the room code
enum { COOP_SCOPE_LOOPBACK, COOP_SCOPE_LAN, COOP_SCOPE_INTERNET };
#define COOP_PORT_DEFAULT 47600            // auto hosts listen here (TCP control, UDP datagrams), or on a port the system picks when it is taken; join's default
#define COOP_SERVICE "_pokopia-ldn._udp"   // the DNS-SD service type (iOS lists it in NSBonjourServices)
#define COOP_RELAY_PORT_DEFAULT 47610      // tools/coop-relay's port
#define COOP_ROOM_LEN 16                   // a room code's symbols (Crockford base32, 5 bits each: 80 bits)
#define COOP_ROOM_SHOWN (COOP_ROOM_LEN + 4)  // XXXX-XXXX-XXXX-XXXX and its NUL

typedef struct coop_hooks {
    void *user;
    void (*log)(void *user, const char *line);    // one line without a newline; NULL: silent
    void (*sleep_ns)(void *user, int64_t ns);     // waits inside game calls (Scan, Connect, Poll, blocking RecvFrom); NULL: a plain host sleep
    void (*on_state)(void *user, int state);      // every LDN state change and NetworkInfo update (nn::ldn's state-change event); called without the node lock held
    // What peers compare before a join (docs/coop-step1c.md), read at Initialize, CreateNetwork, Connect and each join. NULL: unknown, not compared
    const char *(*build)(void *user);                   // main's build ID in hex; the first 16 digits are compared
    uint64_t (*mods)(void *user, char *what, size_t n); // a fingerprint of the active gameplay mods or cheats, 0 = none; what: a short text for logs
} coop_hooks;

typedef struct coop_cfg {
    int role;            // COOP_ROLE_*
    int scope;           // COOP_SCOPE_*: HOST is loopback; JOIN is loopback for a 127/8 address, else LAN; AUTO is LAN
    uint32_t addr;       // join: the host's IPv4, host order
    uint16_t port;       // host / auto: TCP and UDP port (host: 0 lets the system pick; auto: falls back to one the system picks); join: the host's port
    int discovery;       // auto: advertise while hosting and browse while scanning (DNS-SD); 1 by default
    int bind_loopback;   // auto: listen on 127.0.0.1 instead of every interface (unit tests: no firewall prompt for a test binary)
    uint8_t uid[16];     // nn::account::Uid as it lies in memory (two little-endian u64); 0 = unset
    uint64_t lcid;       // LocalCommunicationId that replaces the SDK's -1 ("this application") in NetworkConfig, ScanFilter and Connect; 0: keep -1
    uint8_t platform;    // NodeInfo.Platform (1: Switch 2)
    int tcp_only;        // game datagrams over the control link instead of UDP
    // internet play (coop_cfg_parse_ex): every link proves the room code, frames and datagrams carry tags
    int auth;            // 1: internet play (coop protocol 3); 0: step 1c's LAN links, unchanged
    char room[COOP_ROOM_LEN + 1];   // the room code, normalized (coop_room_normalize)
    int room_new;        // the code was made here (the host shows it: coop_env_node logs it)
    uint32_t relay_ip;   // the relay, host order (0: none): auto hosts through it while the game hosts and joins its room while the game scans
    uint16_t relay_port;
    coop_hooks hooks;
} coop_cfg;

// DM_COOP=auto[:<port>] | join:<IPv4>[:<port>] | host:<port> | off, DM_UID=<32 hex digits> (first 16 digits are Uid word 0). join takes loopback, RFC 1918
// and 169.254/16 addresses only. Returns 1 configured, 0 when coop is NULL, empty, "off" or "0", -1 invalid (err says why). Defaults: platform 1,
// discovery on, the constant nxnative Uid when uid is NULL or empty.
int coop_cfg_parse(coop_cfg *c, const char *coop, const char *uid, char *err, size_t errn);
// The same with internet play: internet "1" turns it on (NULL, empty or "0": off, and then room and relay are ignored and join stays LAN-only).
// On: join takes public addresses; room is the host's code (a visitor must give it; a host without one gets a new one, room_new); relay is
// <IPv4>[:<port>] and needs DM_COOP=auto. Returns as coop_cfg_parse.
int coop_cfg_parse_ex(coop_cfg *c, const char *coop, const char *uid, const char *internet, const char *room, const char *relay, char *err, size_t errn);
// Room codes: normalize accepts upper or lower case, '-' and spaces between symbols, O for 0 and I / L for 1; 1 when it is 16 symbols
int coop_room_normalize(const char *in, char out[COOP_ROOM_LEN + 1]);
void coop_room_new(char out[COOP_ROOM_LEN + 1]);   // 80 bits from the system's random source
void coop_room_format(const char *norm, char out[COOP_ROOM_SHOWN]);
// The process's node from DM_COOP / DM_UID, created on first call (thread-safe); NULL when DM_COOP is unset, off or invalid (logged once).
// DM_COOP_TCP=1 sets tcp_only (game datagrams over the control link: a TCP-only path such as an SSH tunnel; a station's choice, the host follows it).
// DM_COOP_INTERNET=1, DM_COOP_ROOM=<code>, DM_COOP_RELAY=<IPv4>[:<port>]: internet play (coop_cfg_parse_ex); a host's new room code is logged.
coop_node *coop_env_node(const coop_hooks *hooks);
// The Uid DM_UID names when DM_COOP is set: 1 and out filled, else 0 (the host keeps its constant).
int coop_env_uid(uint8_t out[16]);

coop_node *coop_create(const coop_cfg *c);
void coop_destroy(coop_node *n);
uint16_t coop_port(coop_node *n);   // host / auto: the port bound while the game hosts (0 otherwise)
int coop_role(coop_node *n);
typedef struct coop_stats {
    int links;              // host: station links up; station: links to hosts up
    int udp_links;          // links whose game datagrams travel over UDP
    uint64_t udp_tx, udp_rx, tcp_tx, tcp_rx;   // game datagrams sent and received per path
    uint64_t refused_src;   // connections and UDP packets from addresses the scope refuses
    uint64_t refused_join;  // joins refused (network, passphrase, version, build, mods, room)
    uint64_t bad_input;     // malformed frames, packets and records (each closes its link or is dropped)
    uint64_t rate_drops;    // frames and datagrams dropped by the rate limits, connections refused by them
    int discovered;         // DNS-SD: hosts found and resolved
    int advertising;        // DNS-SD: this host's service is registered
    // internet play
    uint64_t auth_ok;       // links that proved the room code (both ends)
    uint64_t auth_fail;     // links that failed it (a wrong code, a host that did not prove it) or were refused by the failure limit
    uint64_t bad_tag;       // frames and datagrams whose tag did not verify (changed, replayed in a stream, another key): link closed or dropped
    uint64_t replays;       // datagrams refused as repeats or older than the window
    int relay_up;           // host: its room is open at the relay
} coop_stats;
void coop_get_stats(coop_node *n, coop_stats *s);

// Address classes (step 1c's protection): a scope accepts loopback (both), private and link-local (LAN). IPv4 in host order; IPv6 as 16 bytes
enum { COOP_ADDR_PUBLIC, COOP_ADDR_LOOPBACK, COOP_ADDR_PRIVATE, COOP_ADDR_LINKLOCAL, COOP_ADDR_INVALID };
int coop_addr4_class(uint32_t ip);
int coop_addr6_class(const uint8_t ip[16]);
int coop_addr_allowed(int scope, int cls);
// " (Radmin VPN's range)" and the like for the public ranges VPNs hand their players (25/8, 26/8, 100.64/10), else "": named where such an address is refused
const char *coop_addr4_vpn(uint32_t ip);

// ---------- nn::ldn. Results are nn::Result values (module 203), 0 on success. Structs are the SDK's: NetworkConfig 0x20, SecurityConfig 0x44,
// UserConfig 0x30, NetworkInfo 0x480, ScanFilter 0x60, SecurityParameter 0x20. IPv4 addresses are host-order u32, as nn::ldn keeps them ----------
#define COOP_LDN_INVALID_STATE    0x40CBu
#define COOP_LDN_DEVICE_DISABLED  0x2CCBu
#define COOP_LDN_INVALID_ARGUMENT 0xC0CBu
#define COOP_LDN_CONNECT_NOT_FOUND 0x82CBu
#define COOP_LDN_CONNECT_TIMEOUT  0x84CBu
#define COOP_LDN_CONNECT_REJECTED 0x86CBu
#define COOP_LDN_VERSION_TOO_LOW  0xE2CBu
#define COOP_LDN_VERSION_TOO_HIGH 0xE4CBu
#define COOP_LDN_TOO_MANY_PLAYERS 0x120CBu
enum { COOP_LS_NONE, COOP_LS_INITIALIZED, COOP_LS_AP, COOP_LS_AP_CREATED, COOP_LS_STATION, COOP_LS_STATION_CONNECTED, COOP_LS_ERROR };
enum { COOP_DR_NONE, COOP_DR_BY_USER, COOP_DR_BY_SYSTEM, COOP_DR_DESTROYED_BY_USER, COOP_DR_DESTROYED_BY_SYSTEM, COOP_DR_REJECTED, COOP_DR_SIGNAL_LOST };

uint32_t coop_ldn_initialize(coop_node *n);
void coop_ldn_finalize(coop_node *n);
int coop_ldn_state(coop_node *n);
uint32_t coop_ldn_open_ap(coop_node *n);
uint32_t coop_ldn_close_ap(coop_node *n);
uint32_t coop_ldn_create_network(coop_node *n, const void *netcfg, const void *sec, const void *user);
uint32_t coop_ldn_destroy_network(coop_node *n);
uint32_t coop_ldn_set_advertise(coop_node *n, const void *data, uint64_t size);
uint32_t coop_ldn_open_station(coop_node *n);
uint32_t coop_ldn_close_station(coop_node *n);
uint32_t coop_ldn_scan(coop_node *n, void *out, int *count, int max, const void *filter, int channel);
uint32_t coop_ldn_connect(coop_node *n, const void *info, const void *sec, const void *user, int version, int option);
uint32_t coop_ldn_disconnect(coop_node *n);
uint32_t coop_ldn_network_info(coop_node *n, void *out);
uint32_t coop_ldn_ipv4(coop_node *n, uint32_t *ip, uint32_t *mask);
uint32_t coop_ldn_network_config(coop_node *n, void *out);
uint32_t coop_ldn_security_parameter(coop_node *n, void *out);
int coop_ldn_disconnect_reason(coop_node *n);
uint32_t coop_ldn_set_accept_policy(coop_node *n, int policy);
uint32_t coop_ldn_reject(coop_node *n, uint32_t ip);

// ---------- nn::socket on the virtual network. FreeBSD values throughout (Horizon's); results are >= 0 or -errno. Sockaddrs are BSD sockaddr_in
// (len, family, port and address in network order). Only AF_INET datagram sockets made while the LDN is initialized are virtual: coop_vnet_socket
// answers -ENETDOWN for anything else, and the HLE keeps its offline behaviour for descriptors coop_vnet_is rejects ----------
#define COOP_FD_BASE 0x100
#define COOP_FD_MAX 32
#define COOP_EBADF 9
#define COOP_EACCES 13
#define COOP_EINVAL 22
#define COOP_EMFILE 24
#define COOP_EAGAIN 35
#define COOP_EMSGSIZE 40
#define COOP_ENOPROTOOPT 42
#define COOP_EADDRINUSE 48
#define COOP_EADDRNOTAVAIL 49
#define COOP_ENETDOWN 50
#define COOP_ENETUNREACH 51
#define COOP_POLLIN 0x1
#define COOP_POLLOUT 0x4
#define COOP_POLLNVAL 0x20
#define COOP_POLLRDNORM 0x40
#define COOP_DGRAM_MAX 4096    // payload cap (the game sends < 4,093 bytes)
#define COOP_QUEUE_MAX 256     // datagrams queued per socket; the oldest is dropped beyond
typedef struct { int32_t fd; int16_t events, revents; } coop_pollfd;   // nn::socket::PollFd and plain pollfd

int coop_vnet_is(coop_node *n, int fd);
int coop_vnet_socket(coop_node *n, int family, int type, int proto);
int coop_vnet_bind(coop_node *n, int fd, const void *sa, uint32_t len);
int coop_vnet_close(coop_node *n, int fd);
int coop_vnet_setsockopt(coop_node *n, int fd, int level, int opt, const void *v, uint32_t len);
int coop_vnet_getsockopt(coop_node *n, int fd, int level, int opt, void *v, uint32_t *len);
int coop_vnet_getsockname(coop_node *n, int fd, void *sa, uint32_t *len);
int coop_vnet_fcntl(coop_node *n, int fd, int cmd, int v);
int64_t coop_vnet_sendto(coop_node *n, int fd, const void *buf, uint64_t len, int flags, const void *sa, uint32_t salen);
int64_t coop_vnet_recvfrom(coop_node *n, int fd, void *buf, uint64_t len, int flags, void *sa, uint32_t *salen);
// Poll over virtual descriptors only (negative fds ignored): POLLIN when a datagram is queued, POLLOUT always. Waits up to timeout_ms
// (negative: 100 ms, as the HLE's idle wait) through hooks.sleep_ns. Returns the ready count or -errno.
int coop_vnet_poll(coop_node *n, coop_pollfd *fds, uint64_t nfds, int timeout_ms);
// Select over Horizon's fd_set (1024 bits as u64 words); every set descriptor must be virtual. Rewrites the sets to the ready ones.
int coop_vnet_select(coop_node *n, int nfds, uint64_t *rd, uint64_t *wr, uint64_t *ex, int timeout_ms);

// ---------- "Interact everywhere" (coop_interact.c, docs/coop-interact.md): Link Play in story areas without Spectator Mode's restrictions.
// DM_COOP_INTERACT=1 turns it on. A port calls coop_interact_site(id, x0..x30) right before the instruction at a site runs; a register the
// callback writes takes effect. Every player in a session must have the same setting: a port adds COOP_INTERACT_MOD to its mods fingerprint.
// "Story co-op (experimental)" (docs/story-coop.md), DM_COOP_STORY=1, implies Interact everywhere and adds the story sites; a port that attaches them
// adds COOP_STORY_MOD after COOP_INTERACT_MOD ----------
enum { COOP_SITE_MASK = 1, COOP_SITE_TALK = 2 };
typedef struct coop_site {
    const char *build;   // main's build ID, its first 32 hex digits (the rewriter's key)
    uint32_t fn, off;    // the function holding the site and the site, main offsets
    uint32_t word;       // the instruction there, checked before attaching
    int id;              // COOP_SITE_*
    const char *what;
} coop_site;
#define COOP_INTERACT_MOD "Interact everywhere"
#define COOP_STORY_MOD "Story co-op (experimental)"
int coop_interact_on(void);   // DM_COOP_INTERACT=1, or Story co-op on
int coop_story_on(void);
int coop_interact_sites(const char *main_build, const coop_site **out);   // the sites of this main build (a run of them) and their count; 0: unsupported
int coop_story_sites(const char *main_build, const coop_site **out);      // the same for Story co-op's sites
void coop_interact_site(int id, uint64_t *x);
uint64_t coop_mods_add(uint64_t h, const char *name);   // a mods fingerprint (0: none so far) with one more named mod

#ifdef __cplusplus
}
#endif
