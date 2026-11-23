// Network services stay offline by default. When DM_NEXTENDO=1, nifm, BSD TCP/UDP sockets, DNS redirection, and TLS connections route through the host and Nextendo.
#include "hle.h"
#include "hle_net.h"
#include "nextendo_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
// SDK 21 (Star Fox) inlines nn::socket::Initialize(const Config &) into the game, and the service connection is this call
HLE_FORCE("_ZN2nn6socket8internal10InitializeEi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_InitializeInternal, int n) { (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "connects to the nifm network management service; offline (cut)", Result, h_nifm_Initialize, void) { return RES_OK; }
// The request is asynchronous on Horizon: SubmitNetworkRequest only hands it to the nifm service, and IsNetworkRequestOnHold stays true until the service has evaluated it, so a query
// right after the submission finds it on hold. Pikmin 4 (Unreal Engine 4.26, main+0x203f3b0) relies on that: it starts its BCAT thread, whose Run (main+0x203f510) sets its status
// to 2, submits, waits while on hold and sets 6 (done, offline), and only then stores status 1 itself; answered at once, a thread that ran first finished before that store, the 1 stayed,
// and the boot state machine (main+0x1f2f764, state 7) waited for the status forever at the black loading screen (about 1 boot in 6, 3 in 4 with four at once). Pikmin 4 sleeps 10 ms
// between its queries, so a request that is on hold at the first query is all it needs.
// Each query is an IPC to the nifm service on Horizon, which evaluates the request as it serves them (offline, a rejection), so the request is on hold for the first query after
// the submission and settled at the next. The hold used to be 50 ms of host time: Hades' NetworkPollThread (main+0x27aaa0) polls with SleepThread(0) and cancels after 2,000
// queries, 2,000 IPCs on the Switch but 0.1 ms here, so it cancelled and submitted again forever (1.17 billion queries a minute, a host core). Its stamp, now_ns() | 1, also read
// as settled at once whenever the next query came within the same 42 ns clock tick (30% of Hades' submissions)
static _Atomic int g_nifm_hold;   // queries the pending request still answers "on hold" (0 = none, or settled)
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "submits a network request: on hold for the first query, then settles", void, h_nifm_Submit, void) {
    atomic_store(&g_nifm_hold, 1);
    if (dm_nextendo_enabled()) LOG("nextendo: nifm::SubmitNetworkRequest");
}
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "drops the pending network request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "on hold for the first query after the submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "offline: always unavailable; online when DM_NEXTENDO=1", int, h_nifm_Available, void) {
    return dm_nextendo_enabled() ? 1 : 0;
}
// Returns bool (`and w0, w0, #1`, Metroid Dread's sdk+0x336210): whether the system's network screen resolved the error
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "offline: not resolved, shows no system network screen", unsigned, h_nifm_HandleErr, void) { return 0; }

int dm_nifm_get_ip_config(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns1, uint32_t *dns2) {
    uint32_t *p[5] = {ip, mask, gw, dns1, dns2};
    for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0;
    if (!dm_nextendo_enabled()) return -1;
    uint32_t found_ip = 0, found_mask = 0;
    // Prefer the adapter actually carrying the default Internet route (works on multi-homed Macs & iOS).
    {
        int probe = socket(AF_INET, SOCK_DGRAM, 0);
        if (probe >= 0) {
            struct sockaddr_in dst = {0}, loc = {0};
            dst.sin_len = sizeof dst;
            dst.sin_family = AF_INET;
            dst.sin_port = htons(53);
            dst.sin_addr.s_addr = htonl(0x08080808u);
            if (connect(probe, (struct sockaddr *)&dst, sizeof dst) == 0) {
                socklen_t llen = sizeof loc;
                if (getsockname(probe, (struct sockaddr *)&loc, &llen) == 0 && loc.sin_family == AF_INET) {
                    if (loc.sin_addr.s_addr && (ntohl(loc.sin_addr.s_addr) >> 24) != 127)
                        found_ip = loc.sin_addr.s_addr;
                }
            }
            close(probe);
        }
    }
    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) == 0) {
        for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
            const struct sockaddr_in *sm = (const struct sockaddr_in *)ifa->ifa_netmask;
            uint32_t a = sin->sin_addr.s_addr;
            uint32_t m = sm ? sm->sin_addr.s_addr : htonl(0xffffff00u);
            if (found_ip && a == found_ip) { found_mask = m; break; }
            if (!found_ip) { found_ip = a; found_mask = m; }
        }
        freeifaddrs(ifas);
    }
    if (!found_ip) {
        found_ip = htonl(INADDR_LOOPBACK);
        found_mask = htonl(0xff000000u);
    }
    if (ip) *ip = found_ip;
    if (mask) *mask = found_mask;
    if (gw) *gw = (found_ip & found_mask) | htonl(1u);
    if (dns1) *dns1 = htonl(0x01010101u);
    if (dns2) *dns2 = htonl(0x08080808u);
    return 0;
}

int dm_nifm_get_ip(uint32_t *ip_net_order) {
    return dm_nifm_get_ip_config(ip_net_order, NULL, NULL, NULL, NULL);
}

HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "primary IP: fails offline; host IPv4 when DM_NEXTENDO=1", Result, h_nifm_GetIp, uint32_t *a) {
    return dm_nifm_get_ip(a) == 0 ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "proxy setting: fails offline; disabled proxy when DM_NEXTENDO=1", Result, h_nifm_GetProxy, void *p) {
    if (dm_nextendo_enabled()) {
        if (p) memset(p, 0, 0xaa);
        LOG("nextendo: nifm::GetCurrentProxySetting -> disabled (RES_OK)");
        return RES_OK;
    }
    return RES_FAIL;
}
HLE_FORCE("nnsslInitialize", "connects to the ssl service; the network library's global init needs it to succeed, offline creates no session (cut)", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "pairs with nnsslInitialize (cut)", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- BSD service: local UDP/EventFd work offline; remote networking stays unavailable ----------
// Local transports are used even by offline games: RakNet sends a startup datagram to itself in Divinity: Original Sin 2. No host socket is opened. Descriptors belong to
// this service, not Darwin's fd table, and stay small enough for games' fd_set arrays. Address/flag/error constants below are the Horizon/FreeBSD ABI (libnx's BSD service passes
// these structures/flags unchanged; FreeBSD11 sys/socket.h and netinet/in.h). IPv4 payloads retain packet boundaries; the per-endpoint queue holds at most256 packets/1MiB.
#define SK_ENETDOWN 50
static __thread int t_skerr = SK_ENETDOWN;
static int *(*_Atomic g_guest_errno_fn)(void);
static _Atomic int g_guest_errno_looked;
static int *sk_guest_errno_ptr(void) {
    if (!atomic_load(&g_guest_errno_looked)) {
        void *sym = guest_sym("__errno_location");
        atomic_store(&g_guest_errno_fn, (int *(*)(void))sym);
        atomic_store(&g_guest_errno_looked, 1);
    }
    int *(*fn)(void) = atomic_load(&g_guest_errno_fn);
    return fn ? fn() : NULL;
}
static int sk_darwin_to_linux_errno(int e) {
    switch (e) {
    case EAGAIN: return 11;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93;
    case EOPNOTSUPP: return 95;
    case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ENETRESET: return 102;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
    default: return e;
    }
}
static int sk_error(int error) {
    t_skerr = error;
    if (dm_nextendo_enabled()) {
        int *ge = sk_guest_errno_ptr();
        if (ge) *ge = error;
    }
    return -1;
}
static int sk_host_error(int darwin_err) {
    int le = sk_darwin_to_linux_errno(darwin_err);
    t_skerr = le;
    int *ge = sk_guest_errno_ptr();
    if (ge) *ge = le;
    return -1;
}
static int sk_fail(void) { return sk_error(SK_ENETDOWN); }
int dm_sk_errno(void) {
    if (dm_nextendo_enabled()) {
        int *ge = sk_guest_errno_ptr();
        if (ge && *ge) return *ge;
    }
    return t_skerr;
}
#define SKF(m, cname, ...) HLE_FORCE(m, "local BSD descriptor service; remote networking unavailable", int, cname, __VA_ARGS__)
void dm_guest_sleep_ns(int64_t ns);
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns);
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
enum { EFD_MAX = 64, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLERR = 8, SK_POLLHUP = 0x10, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4, SK_POLLNVAL = 0x20,
       SK_EBADF = 9, SK_EAGAIN = 35, SK_EINVAL = 22, SK_EMSGSIZE = 40, SK_EOPNOTSUPP = 45, SK_EADDRINUSE = 48, SK_ENOTCONN = 57,
       SK_NONBLOCK = 0x800, SK_MSG_PEEK = 2, SK_MSG_DONTWAIT = 0x80, SK_EFD = 1, SK_UDP = 2, SK_HOST = 3,
       SK_UDP_MAX = 65507, SK_QUEUE_MAX = 1024 * 1024, SK_PACKET_MAX = 256, SK_DEFAULT_BUF = 256 * 1024, SK_PARK_MAX = 8, SK_PARK_TTL = 5 };
typedef struct sk_packet { struct sk_packet *next; uint32_t addr; uint16_t port; size_t size; uint8_t data[]; } sk_packet;
static struct {
    uint64_t count, generation; int used, flags, fdflags, host_fd, sock_type;
    uint32_t addr, peer_addr; uint16_t port, peer_port;
    int connected, shut_read, shut_write, reuse, broadcast, rcvbuf, sndbuf;
    int64_t recv_timeout, send_timeout; sk_packet *head, *tail; size_t queued; unsigned packets;
} g_efd[EFD_MAX];
static struct { int fd; uint16_t port; int64_t expiry; } g_sk_park[SK_PARK_MAX];
static uint64_t g_sk_generation;
static uint16_t g_sk_ephemeral = 49152;
static pthread_mutex_t g_efd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_c = PTHREAD_COND_INITIALIZER;
static int sk_used(int fd) { return fd >= 0 && fd < EFD_MAX && g_efd[fd].used; }   // with g_efd_m held
static int efd_used(int fd) { return sk_used(fd) && g_efd[fd].used == SK_EFD; }
static int sk_host_locked(int fd) { return sk_used(fd) && g_efd[fd].used == SK_HOST && g_efd[fd].host_fd >= 0; }
static int sk_new_locked(int kind) {
    for (int i = 0; i < EFD_MAX; i++) if (!g_efd[i].used) {
        memset(&g_efd[i], 0, sizeof g_efd[i]); g_efd[i].used = kind; g_efd[i].generation = ++g_sk_generation;
        g_efd[i].host_fd = -1; g_efd[i].rcvbuf = g_efd[i].sndbuf = SK_DEFAULT_BUF; return i;
    }
    return sk_error(24);   // EMFILE
}
static int sk_udp_locked(int fd) { return sk_used(fd) && g_efd[fd].used == SK_UDP; }
static void sk_park_sweep_locked(int64_t now) {
    for (int i = 0; i < SK_PARK_MAX; i++) if (g_sk_park[i].fd >= 0 && g_sk_park[i].port && now >= g_sk_park[i].expiry) {
        close(g_sk_park[i].fd); g_sk_park[i].fd = -1; g_sk_park[i].port = 0;
    }
}
static int sk_park_take_locked(uint16_t port) {
    int64_t now = (int64_t)time(NULL);
    sk_park_sweep_locked(now);
    if (!port) return -1;
    for (int i = 0; i < SK_PARK_MAX; i++) if (g_sk_park[i].port == port && g_sk_park[i].fd >= 0) {
        int hfd = g_sk_park[i].fd; g_sk_park[i].fd = -1; g_sk_park[i].port = 0; return hfd;
    }
    return -1;
}
static int sk_park_put_locked(int hfd, uint16_t port) {
    if (hfd < 0 || !port) return 0;
    int64_t now = (int64_t)time(NULL);
    sk_park_sweep_locked(now);
    for (int i = 0; i < SK_PARK_MAX; i++) if (!g_sk_park[i].port) {
        g_sk_park[i].fd = hfd; g_sk_park[i].port = port; g_sk_park[i].expiry = now + SK_PARK_TTL; return 1;
    }
    return 0;
}
static int sk_host_addr_in(const void *a, unsigned n, struct sockaddr_in *out) {
    if (!a || n < 8) {
        LOG("nextendo: sk_host_addr_in FAIL a=%p n=%u", a, n);
        return sk_host_error(EINVAL);
    }
    const uint8_t *b = a;
    int is_bsd = (b[1] == AF_INET && (!b[0] || b[0] == 16));
    int is_linux = (b[0] == AF_INET && b[1] == 0);
    if (!is_bsd && !is_linux) {
        LOG("nextendo: sk_host_addr_in FAIL n=%u b=%02x %02x %02x %02x", n, b[0], b[1], b[2], b[3]);
        return sk_host_error(EINVAL);
    }
    memset(out, 0, sizeof *out);
    out->sin_len = sizeof *out;
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, b + 2, 2);
    memcpy(&out->sin_addr.s_addr, b + 4, 4);
    return 0;
}
static int sk_host_addr_out(void *a, unsigned *n, const struct sockaddr_in *in) {
    if (!a) return 0;
    if (!n) return sk_error(SK_EINVAL);
    uint8_t b[16] = {16, AF_INET};
    memcpy(b + 2, &in->sin_port, 2);
    memcpy(b + 4, &in->sin_addr.s_addr, 4);
    memcpy(a, b, *n < sizeof b ? *n : sizeof b);
    *n = sizeof b;
    return 0;
}
int dm_sk_host_fd(int fd) {
    dm_host_mutex_lock(&g_efd_m);
    int hfd = sk_host_locked(fd) ? g_efd[fd].host_fd : -1;
    pthread_mutex_unlock(&g_efd_m);
    return hfd;
}
static int efd_create(uint64_t v, int flags) {
    dm_host_mutex_lock(&g_efd_m); int fd = sk_new_locked(SK_EFD);
    if (fd >= 0) { g_efd[fd].count = v; g_efd[fd].flags = flags; }
    pthread_mutex_unlock(&g_efd_m); return fd;
}
int dm_sk_socket(int family, int type, int protocol) {
    int base_type = type & 0xFF;
    int want_nb = !!(type & 0x20000000);   // Horizon/FreeBSD SOCK_NONBLOCK
    if (dm_nextendo_enabled() && family == AF_INET && (base_type == SOCK_STREAM || base_type == SOCK_DGRAM)) {
        int hfd = socket(AF_INET, base_type, protocol);
        if (hfd < 0) return sk_host_error(errno);
        int one = 1;
        setsockopt(hfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        if (want_nb) fcntl(hfd, F_SETFL, fcntl(hfd, F_GETFL, 0) | O_NONBLOCK);
        dm_host_mutex_lock(&g_efd_m);
        int fd = sk_new_locked(SK_HOST);
        if (fd >= 0) { g_efd[fd].host_fd = hfd; g_efd[fd].sock_type = base_type; if (want_nb) g_efd[fd].flags |= SK_NONBLOCK; }
        pthread_mutex_unlock(&g_efd_m);
        if (fd < 0) close(hfd);
        LOG("nextendo: socket(fam=%d, type=%#x, proto=%d) -> fd=%d hfd=%d", family, type, protocol, fd, hfd);
        return fd;
    }
    if (dm_nextendo_enabled()) LOG("nextendo: socket(fam=%d, type=%#x, proto=%d) -> fallback/fail", family, type, protocol);
    if (family != 2 || base_type != 2 || (protocol && protocol != 17)) return sk_fail();
    dm_host_mutex_lock(&g_efd_m); int fd = sk_new_locked(SK_UDP); pthread_mutex_unlock(&g_efd_m); return fd;
}
int dm_sk_close(int fd) {
    dm_host_mutex_lock(&g_efd_m);
    if (!sk_used(fd)) { pthread_mutex_unlock(&g_efd_m); return sk_fail(); }
    int hfd = g_efd[fd].used == SK_HOST ? g_efd[fd].host_fd : -1;
    int park = (g_efd[fd].used == SK_HOST && g_efd[fd].sock_type == SOCK_DGRAM && !g_efd[fd].connected && g_efd[fd].port);
    uint16_t port = g_efd[fd].port;
    sk_packet *p = g_efd[fd].head;
    while (p) { sk_packet *next = p->next; free(p); p = next; }
    g_efd[fd].head = g_efd[fd].tail = NULL; g_efd[fd].used = 0; g_efd[fd].host_fd = -1;
    if (park && sk_park_put_locked(hfd, port)) hfd = -1;
    pthread_cond_broadcast(&g_efd_c);
    pthread_mutex_unlock(&g_efd_m);
    if (hfd >= 0) { shutdown(hfd, SHUT_RDWR); close(hfd); }
    return 0;
}
static int efd_read(int fd, uint64_t *out) {
    dm_host_mutex_lock(&g_efd_m); int r = -1;
    uint64_t gen = efd_used(fd) ? g_efd[fd].generation : 0;
    for (;;) {
        if (!efd_used(fd) || g_efd[fd].generation != gen) { t_skerr = SK_EBADF; break; }
        uint64_t *c = &g_efd[fd].count; int f = g_efd[fd].flags;
        if (*c) { if (f & 1) { *out = 1; *c -= 1; } else { *out = *c; *c = 0; } r = 0; break; }
        if (f & 6) { t_skerr = SK_EAGAIN; break; }
        dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, dm_detclock_enabled() ? 1000000 : 100000000);
    }
    pthread_mutex_unlock(&g_efd_m); return r;
}
static int efd_write(int fd, uint64_t v) {
    dm_host_mutex_lock(&g_efd_m); int ok = efd_used(fd);
    if (ok) { g_efd[fd].count += v; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m); return ok ? 0 : sk_error(SK_EBADF);
}
// sockaddr_in is {u8 length, u8 family, u16 port, u32 address, padding}, port/address in network byte order. sin_len may be zero in hand-written game code.
static int sk_addr_read(const void *a, unsigned n, uint32_t *addr, uint16_t *port) {
    if (!a || n < 16) return sk_error(SK_EINVAL);
    const uint8_t *b = a;
    if (b[1] != 2 || (b[0] && b[0] != 16)) return sk_error(SK_EINVAL);
    *port = (uint16_t)((unsigned)b[2] << 8 | b[3]);
    *addr = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    if (*addr && (*addr >> 24) != 127) return sk_fail();
    return 0;
}
static int sk_addr_write(void *a, unsigned *n, uint32_t addr, uint16_t port) {
    if (!a) return 0;
    if (!n) return sk_error(SK_EINVAL);
    uint8_t b[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    memcpy(a, b, *n < sizeof b ? *n : sizeof b); *n = sizeof b; return 0;
}
static int sk_port_free(int fd, uint32_t addr, uint16_t port) {
    for (int i = 0; i < EFD_MAX; i++) if (i != fd && sk_udp_locked(i) && g_efd[i].port == port && (!addr || !g_efd[i].addr || addr == g_efd[i].addr)) {
        int both = g_efd[i].reuse & g_efd[fd].reuse;
        if (!(both & 0x200) && !((both & 4) && addr != g_efd[i].addr)) return 0;
    }
    return 1;
}
static int sk_bind_locked(int fd, uint32_t addr, uint16_t port) {
    if (g_efd[fd].port) return sk_error(SK_EINVAL);
    if (!port) {
        for (unsigned i = 0; i < 16384; i++) {
            uint16_t candidate = g_sk_ephemeral; g_sk_ephemeral = candidate == 65535 ? 49152 : (uint16_t)(candidate + 1);
            if (sk_port_free(fd, addr, candidate)) { port = candidate; break; }
        }
        if (!port) return sk_error(SK_EADDRINUSE);
    } else if (!sk_port_free(fd, addr, port)) return sk_error(SK_EADDRINUSE);
    g_efd[fd].addr = addr; g_efd[fd].port = port; return 0;
}
int dm_sk_bind(int fd, const void *a, unsigned n) {
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd, stype = g_efd[fd].sock_type;
        struct sockaddr_in sa;
        if (sk_host_addr_in(a, n, &sa)) { pthread_mutex_unlock(&g_efd_m); return -1; }
        uint16_t port = ntohs(sa.sin_port);
        if (stype == SOCK_DGRAM && port) {
            int parked = sk_park_take_locked(port);
            if (parked >= 0) {
                close(hfd);
                g_efd[fd].host_fd = parked;
                g_efd[fd].port = port;
                pthread_mutex_unlock(&g_efd_m);
                return 0;
            }
        }
        pthread_mutex_unlock(&g_efd_m);
        if (bind(hfd, (const struct sockaddr *)&sa, sizeof sa) < 0) return sk_host_error(errno);
        struct sockaddr_in bound = {0};
        socklen_t blen = sizeof bound;
        if (getsockname(hfd, (struct sockaddr *)&bound, &blen) == 0) {
            dm_host_mutex_lock(&g_efd_m);
            if (sk_host_locked(fd)) g_efd[fd].port = ntohs(bound.sin_port);
            pthread_mutex_unlock(&g_efd_m);
        }
        return 0;
    }
    pthread_mutex_unlock(&g_efd_m);
    uint32_t addr; uint16_t port; if (sk_addr_read(a, n, &addr, &port)) return -1;
    dm_host_mutex_lock(&g_efd_m); int r = sk_udp_locked(fd) ? sk_bind_locked(fd, addr, port) : sk_error(SK_EBADF); pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_connect(int fd, const void *a, unsigned n) {
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd;
        pthread_mutex_unlock(&g_efd_m);
        struct sockaddr_in sa;
        if (sk_host_addr_in(a, n, &sa)) return -1;
        dm_host_work++;
        int rc = connect(hfd, (const struct sockaddr *)&sa, sizeof sa);
        int err = errno;
        dm_host_work--;
        LOG("nextendo: connect(fd=%d, %s:%u) -> rc=%d err=%d (guest_err=%d)", fd, inet_ntoa(sa.sin_addr), (unsigned)ntohs(sa.sin_port), rc, rc < 0 ? err : 0, rc < 0 ? sk_darwin_to_linux_errno(err) : 0);
        if (rc < 0) return sk_host_error(err);
        dm_host_mutex_lock(&g_efd_m);
        if (sk_host_locked(fd)) g_efd[fd].connected = 1;
        pthread_mutex_unlock(&g_efd_m);
        return 0;
    }
    pthread_mutex_unlock(&g_efd_m);
    uint32_t addr = 0; uint16_t port = 0; int disconnect = a && n >= 2 && ((const uint8_t *)a)[1] == 0;
    if (!disconnect && sk_addr_read(a, n, &addr, &port)) return -1;
    if (!disconnect && !port) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m); int r = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (!g_efd[fd].port && sk_bind_locked(fd, 0, 0)) r = -1;
    else { g_efd[fd].connected = !disconnect; g_efd[fd].peer_addr = addr ? addr : 0x7f000001; g_efd[fd].peer_port = port; }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_listen(int fd, int backlog) {
    dm_host_mutex_lock(&g_efd_m);
    int hfd = sk_host_locked(fd) ? g_efd[fd].host_fd : -1;
    pthread_mutex_unlock(&g_efd_m);
    if (hfd < 0) return sk_fail();
    return listen(hfd, backlog) < 0 ? sk_host_error(errno) : 0;
}
int dm_sk_accept(int fd, void *a, unsigned *n) {
    dm_host_mutex_lock(&g_efd_m);
    int hfd = sk_host_locked(fd) ? g_efd[fd].host_fd : -1;
    pthread_mutex_unlock(&g_efd_m);
    if (hfd < 0) return sk_fail();
    struct sockaddr_in sa = {0};
    socklen_t slen = sizeof sa;
    dm_host_work++;
    int chfd = accept(hfd, (struct sockaddr *)&sa, &slen);
    int err = errno;
    dm_host_work--;
    if (chfd < 0) return sk_host_error(err);
    int one = 1;
    setsockopt(chfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    dm_host_mutex_lock(&g_efd_m);
    int cfd = sk_new_locked(SK_HOST);
    if (cfd >= 0) { g_efd[cfd].host_fd = chfd; g_efd[cfd].sock_type = SOCK_STREAM; g_efd[cfd].connected = 1; }
    pthread_mutex_unlock(&g_efd_m);
    if (cfd < 0) { close(chfd); return -1; }
    if (a && n) sk_host_addr_out(a, n, &sa);
    return cfd;
}
int dm_sk_name(int fd, void *a, unsigned *n, int peer) {
    if (!a || !n) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd;
        pthread_mutex_unlock(&g_efd_m);
        struct sockaddr_in sa = {0};
        socklen_t slen = sizeof sa;
        int rc = peer ? getpeername(hfd, (struct sockaddr *)&sa, &slen) : getsockname(hfd, (struct sockaddr *)&sa, &slen);
        return rc < 0 ? sk_host_error(errno) : sk_host_addr_out(a, n, &sa);
    }
    int r;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (peer && !g_efd[fd].connected) r = sk_error(SK_ENOTCONN);
    else r = sk_addr_write(a, n, peer ? g_efd[fd].peer_addr : g_efd[fd].addr, peer ? g_efd[fd].peer_port : g_efd[fd].port);
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_shutdown(int fd, int how) {
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd;
        pthread_mutex_unlock(&g_efd_m);
        return shutdown(hfd, how) < 0 ? sk_host_error(errno) : 0;
    }
    int r = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (how < 0 || how > 2) r = sk_error(SK_EINVAL);
    else if (!g_efd[fd].connected) r = sk_error(SK_ENOTCONN);
    else { if (how != 1) g_efd[fd].shut_read = 1; if (how != 0) g_efd[fd].shut_write = 1; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_fcntl(int fd, int cmd, int value) {
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd;
        int r = 0;
        if (cmd == 1) r = g_efd[fd].fdflags;
        else if (cmd == 2) g_efd[fd].fdflags = value & 1;
        else if (cmd == 3) {
            int fl = fcntl(hfd, F_GETFL, 0);
            r = (fl >= 0 && (fl & O_NONBLOCK)) ? SK_NONBLOCK : 0;
        } else if (cmd == 4) {
            int fl = fcntl(hfd, F_GETFL, 0);
            if (fl < 0) r = sk_host_error(errno);
            else {
                int nfl = (value & SK_NONBLOCK) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
                r = fcntl(hfd, F_SETFL, nfl) < 0 ? sk_host_error(errno) : 0;
                if (!r) g_efd[fd].flags = value & SK_NONBLOCK;
            }
        } else r = sk_error(SK_EINVAL);
        pthread_mutex_unlock(&g_efd_m);
        return r;
    }
    int r = 0;
    if (!sk_used(fd)) r = sk_error(SK_EBADF);
    else if (cmd == 1) r = g_efd[fd].fdflags;
    else if (cmd == 2) g_efd[fd].fdflags = value & 1;
    else if (cmd == 3) r = g_efd[fd].used == SK_EFD ? (g_efd[fd].flags & 6 ? SK_NONBLOCK : 0) : g_efd[fd].flags;
    else if (cmd == 4) { if (g_efd[fd].used == SK_EFD) g_efd[fd].flags = (g_efd[fd].flags & ~6) | (value & SK_NONBLOCK ? 2 : 0); else g_efd[fd].flags = value & SK_NONBLOCK; }
    else r = sk_error(SK_EINVAL);
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_setopt(int fd, int level, int option, const void *value, unsigned size) {
    if (!value) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd;
        pthread_mutex_unlock(&g_efd_m);
        int hlevel = level == 0xffff ? SOL_SOCKET : level;
        if (hlevel == SOL_SOCKET && (option == 0x1005 || option == 0x1006)) {
            struct timeval tv = {0};
            if (size >= 16) {
                int64_t gtv[2]; memcpy(gtv, value, sizeof gtv);
                tv.tv_sec = (time_t)gtv[0]; tv.tv_usec = (suseconds_t)gtv[1];
            } else if (size == 4) {
                uint32_t ms = 0; memcpy(&ms, value, 4);
                tv.tv_sec = (time_t)(ms / 1000u); tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
            } else return sk_host_error(EINVAL);
            int hopt = option == 0x1006 ? SO_RCVTIMEO : SO_SNDTIMEO;
            return setsockopt(hfd, SOL_SOCKET, hopt, &tv, sizeof tv) < 0 ? sk_host_error(errno) : 0;
        }
        if (hlevel == SOL_SOCKET && option == 0x800) {
            int one = 1;
            return setsockopt(hfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0 ? sk_host_error(errno) : 0;
        }
        return setsockopt(hfd, hlevel, option, value, size) < 0 ? sk_host_error(errno) : 0;
    }
    int r = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (level == 0xffff && (option == 0x1006 || option == 0x1005) && size >= 16) {
        int64_t tv[2]; memcpy(tv, value, sizeof tv);
        if (tv[0] < 0 || tv[0] > INT64_MAX / 1000000000 - 1 || tv[1] < 0 || tv[1] >= 1000000) r = sk_error(SK_EINVAL);
        else if (option == 0x1006) g_efd[fd].recv_timeout = tv[0] * 1000000000 + tv[1] * 1000;
        else g_efd[fd].send_timeout = tv[0] * 1000000000 + tv[1] * 1000;
    } else if (level == 0xffff && size >= sizeof(int)) {
        int v; memcpy(&v, value, sizeof v);
        if (option == 4 || option == 0x200) { g_efd[fd].reuse &= ~option; if (v) g_efd[fd].reuse |= option; }
        else if (option == 0x20) g_efd[fd].broadcast = !!v;
        else if (option == 0x1001 || option == 0x1002) {
            if (v <= 0) r = sk_error(SK_EINVAL);
            else { if (v > SK_QUEUE_MAX) v = SK_QUEUE_MAX; if (option == 0x1001) g_efd[fd].sndbuf = v; else g_efd[fd].rcvbuf = v; }
        } else r = sk_error(42);   // ENOPROTOOPT
    } else r = sk_error(42);
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_getopt(int fd, int level, int option, void *value, unsigned *size) {
    if (!value || !size) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd;
        pthread_mutex_unlock(&g_efd_m);
        int hlevel = level == 0xffff ? SOL_SOCKET : level;
        if (hlevel == SOL_SOCKET && (option == 0x1005 || option == 0x1006)) {
            struct timeval tv = {0}; socklen_t tlen = sizeof tv;
            int hopt = option == 0x1006 ? SO_RCVTIMEO : SO_SNDTIMEO;
            if (getsockopt(hfd, SOL_SOCKET, hopt, &tv, &tlen) < 0) return sk_host_error(errno);
            if (*size >= 16) {
                int64_t gtv[2] = {(int64_t)tv.tv_sec, (int64_t)tv.tv_usec};
                memcpy(value, gtv, sizeof gtv); *size = sizeof gtv;
            } else if (*size >= 4) {
                uint32_t ms = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
                memcpy(value, &ms, 4); *size = 4;
            } else return sk_host_error(EINVAL);
            return 0;
        }
        socklen_t slen = *size;
        int rc = getsockopt(hfd, hlevel, option, value, &slen);
        int err = errno;
        if (rc == 0 && hlevel == SOL_SOCKET && option == SO_ERROR && slen >= 4) {
            int *ev = (int *)value;
            *ev = sk_darwin_to_linux_errno(*ev);
        }
        if (rc < 0) return sk_host_error(err);
        *size = (unsigned)slen;
        return 0;
    }
    int r = 0, v = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (level != 0xffff) r = sk_error(42);
    else if ((option == 0x1006 || option == 0x1005) && *size >= 16) {
        int64_t ns = option == 0x1006 ? g_efd[fd].recv_timeout : g_efd[fd].send_timeout;
        int64_t tv[2] = {ns / 1000000000, ns % 1000000000 / 1000}; memcpy(value, tv, sizeof tv); *size = sizeof tv;
    } else if (*size < sizeof v) r = sk_error(SK_EINVAL);
    else {
        if (option == 0x1008) v = 2; else if (option == 0x1007) v = 0; else if (option == 0x1016) v = 17;
        else if (option == 4 || option == 0x200) v = !!(g_efd[fd].reuse & option);
        else if (option == 0x20) v = g_efd[fd].broadcast;
        else if (option == 0x1001) v = g_efd[fd].sndbuf; else if (option == 0x1002) v = g_efd[fd].rcvbuf;
        else r = sk_error(42);
        if (!r) { memcpy(value, &v, sizeof v); *size = sizeof v; }
    }
    pthread_mutex_unlock(&g_efd_m); return r;
}
static _Atomic uint32_t g_nncs_public_ip;

static int net_trace_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NET_TRACE"); v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0; }
    return v;
}

long dm_sk_sendto(int fd, const void *data, unsigned long n, int flags, const void *a, unsigned al) {
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd, stype = g_efd[fd].sock_type;
        pthread_mutex_unlock(&g_efd_m);
        int hflags = (flags & SK_MSG_DONTWAIT) ? MSG_DONTWAIT : 0;
        struct sockaddr_in sa;
        const struct sockaddr *sap = NULL; socklen_t salen = 0;
        int hairpinned = 0;
        if (a) {
            if (sk_host_addr_in(a, al, &sa)) return -1;
            uint32_t pub = atomic_load(&g_nncs_public_ip);
            if (stype == SOCK_DGRAM && pub && sa.sin_addr.s_addr == pub) {
                sa.sin_addr.s_addr = htonl(0x7F000001u); hairpinned = 1;
            }
            sap = (const struct sockaddr *)&sa; salen = sizeof sa;
        }
        dm_host_work++;
        ssize_t r = sap ? sendto(hfd, data, n, hflags, sap, salen) : send(hfd, data, n, hflags);
        int err = errno;
        dm_host_work--;
        if (net_trace_on() && stype == SOCK_DGRAM && sap && ntohs(sa.sin_port) != 10025) {
            char ip[32] = ""; inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
            LOG("net_trace: udp_tx fd=%d -> %s:%u len=%lu rc=%zd err=%d hp=%d", fd, ip, (unsigned)ntohs(sa.sin_port), n, r, err, hairpinned);
        }
        return r < 0 ? sk_host_error(err) : (long)r;
    }
    pthread_mutex_unlock(&g_efd_m);
    if ((!data && n) || n > SK_UDP_MAX) return sk_error(n > SK_UDP_MAX ? SK_EMSGSIZE : SK_EINVAL);
    if (flags & ~(SK_MSG_DONTWAIT | 4 | 0x20000)) return sk_error(SK_EOPNOTSUPP);
    uint32_t addr = 0; uint16_t port = 0; if (a && sk_addr_read(a, al, &addr, &port)) return -1;
    dm_host_mutex_lock(&g_efd_m); long r = -1;
    if (!sk_udp_locked(fd)) { t_skerr = SK_EBADF; goto done; }
    if (g_efd[fd].shut_write) { t_skerr = 32; goto done; }
    if (!a) { if (!g_efd[fd].connected) { t_skerr = 39; goto done; } addr = g_efd[fd].peer_addr; port = g_efd[fd].peer_port; }
    if (!port) { t_skerr = SK_EINVAL; goto done; }
    if (!addr) addr = 0x7f000001;
    if (!g_efd[fd].port && sk_bind_locked(fd, 0, 0)) goto done;
    uint32_t src = g_efd[fd].addr ? g_efd[fd].addr : 0x7f000001;
    int target = -1, specificity = -1;
    for (int i = 0; i < EFD_MAX; i++) if (sk_udp_locked(i) && !g_efd[i].shut_read && g_efd[i].port == port && (!g_efd[i].addr || g_efd[i].addr == addr)) {
        if (g_efd[i].connected && (g_efd[i].peer_addr != src || g_efd[i].peer_port != g_efd[fd].port)) continue;
        int score = (g_efd[i].addr != 0) + 2 * g_efd[i].connected;
        if (score > specificity) { target = i; specificity = score; }
    }
    if (target >= 0) { int i = target;
        // UDP drops a datagram when the receive queue is full; the sender still succeeds, as it has no delivery acknowledgement.
        if (g_efd[i].packets < SK_PACKET_MAX && n <= (size_t)g_efd[i].rcvbuf && g_efd[i].queued <= (size_t)g_efd[i].rcvbuf - n) {
            sk_packet *p = malloc(sizeof *p + n);
            if (!p) { t_skerr = 55; goto done; }   // ENOBUFS
            p->next = NULL; p->addr = src; p->port = g_efd[fd].port; p->size = n; if (n) memcpy(p->data, data, n);
            if (g_efd[i].tail) g_efd[i].tail->next = p; else g_efd[i].head = p;
            g_efd[i].tail = p; g_efd[i].queued += n; g_efd[i].packets++; pthread_cond_broadcast(&g_efd_c);
        }
    }
    r = (long)n;
done:
    pthread_mutex_unlock(&g_efd_m); return r;
}
long dm_sk_sendmsg(int fd, const void *gmsg, int flags) {
    if (!gmsg) return sk_error(SK_EINVAL);
    const struct msghdr *gm = gmsg;
    if (!dm_nextendo_enabled()) return sk_fail();
    int hfd = -1, nb = 0;
    dm_host_mutex_lock(&g_efd_m);
    if (fd >= 0 && fd < EFD_MAX && g_efd[fd].used == SK_HOST && g_efd[fd].host_fd >= 0) {
        hfd = g_efd[fd].host_fd; nb = !!(g_efd[fd].flags & SK_NONBLOCK);
    }
    pthread_mutex_unlock(&g_efd_m);
    if (hfd < 0) {
        size_t total = 0;
        for (int i = 0; i < gm->msg_iovlen && gm->msg_iov; i++) total += gm->msg_iov[i].iov_len;
        uint8_t *tmp = total ? malloc(total) : NULL;
        if (total && !tmp) return sk_error(12);   // ENOMEM
        size_t off = 0;
        for (int i = 0; i < gm->msg_iovlen && gm->msg_iov; i++) {
            if (gm->msg_iov[i].iov_len) { memcpy(tmp + off, gm->msg_iov[i].iov_base, gm->msg_iov[i].iov_len); off += gm->msg_iov[i].iov_len; }
        }
        long r = dm_sk_sendto(fd, tmp, total, flags, gm->msg_name, gm->msg_namelen);
        free(tmp);
        return r;
    }
    struct msghdr hm = {0};
    struct sockaddr_in sin = {0};
    if (gm->msg_name && gm->msg_namelen >= 8) {
        if (sk_host_addr_in(gm->msg_name, gm->msg_namelen, &sin) == 0) {
            uint32_t pub = atomic_load(&g_nncs_public_ip);
            if (pub && sin.sin_addr.s_addr == pub) sin.sin_addr.s_addr = htonl(0x7F000001u);
            hm.msg_name = &sin; hm.msg_namelen = sizeof sin;
        }
    }
    hm.msg_iov = gm->msg_iov;
    hm.msg_iovlen = gm->msg_iovlen;
    int hf = ((flags & SK_MSG_DONTWAIT) || nb) ? MSG_DONTWAIT : 0;
    dm_host_work++;
    ssize_t r = sendmsg(hfd, &hm, hf);
    int err = errno;
    dm_host_work--;
    if (r < 0) return sk_host_error(err);
    return (long)r;
}

long dm_sk_recvmsg(int fd, void *gmsg, int flags) {
    if (!gmsg) return sk_error(SK_EINVAL);
    struct msghdr *gm = gmsg;
    if (!dm_nextendo_enabled()) return sk_fail();
    int hfd = -1, nb = 0, stype = 0;
    dm_host_mutex_lock(&g_efd_m);
    if (fd >= 0 && fd < EFD_MAX && g_efd[fd].used == SK_HOST && g_efd[fd].host_fd >= 0) {
        hfd = g_efd[fd].host_fd; nb = !!(g_efd[fd].flags & SK_NONBLOCK); stype = g_efd[fd].sock_type;
    }
    pthread_mutex_unlock(&g_efd_m);
    if (hfd < 0) {
        if (!gm->msg_iov || gm->msg_iovlen <= 0) return 0;
        unsigned alen = gm->msg_namelen;
        long r = dm_sk_recvfrom(fd, gm->msg_iov[0].iov_base, gm->msg_iov[0].iov_len, flags, gm->msg_name, gm->msg_name ? &alen : NULL);
        if (r >= 0) { gm->msg_namelen = alen; gm->msg_flags = 0; }
        return r;
    }
    struct msghdr hm = {0};
    struct sockaddr_in sin = {0};
    if (gm->msg_name && gm->msg_namelen >= 16) { hm.msg_name = &sin; hm.msg_namelen = sizeof sin; }
    hm.msg_iov = gm->msg_iov;
    hm.msg_iovlen = gm->msg_iovlen;
    int hf = ((flags & SK_MSG_DONTWAIT) || nb) ? MSG_DONTWAIT : 0;
    if (flags & SK_MSG_PEEK) hf |= MSG_PEEK;
    ssize_t r;
    int err = 0;
    for (;;) {
        for (int i = 0; i < (int)hm.msg_iovlen && hm.msg_iov; i++) {
            if (hm.msg_iov[i].iov_base && hm.msg_iov[i].iov_len)
                dm_guest_write_prepare(hm.msg_iov[i].iov_base, hm.msg_iov[i].iov_len);
        }
        dm_host_work++;
        r = recvmsg(hfd, &hm, hf);
        err = errno;
        dm_host_work--;
        if (r >= 0) break;
        if (err == EINTR) continue;
        if (stype == SOCK_DGRAM && (err == ECONNREFUSED || err == ECONNRESET || err == ENETRESET)) {
            if (nb || (flags & SK_MSG_DONTWAIT)) return sk_host_error(EAGAIN);
            continue;
        }
        return sk_host_error(err);
    }
    if (gm->msg_name && hm.msg_namelen >= 8 && sin.sin_family == AF_INET) {
        unsigned alen = gm->msg_namelen;
        sk_addr_write(gm->msg_name, &alen, ntohl(sin.sin_addr.s_addr), ntohs(sin.sin_port));
        gm->msg_namelen = alen;
    }
    gm->msg_controllen = 0;
    gm->msg_flags = 0;
    return (long)r;
}

long dm_sk_read(int fd, void *data, unsigned long size) {
    dm_host_mutex_lock(&g_efd_m); int event = efd_used(fd); pthread_mutex_unlock(&g_efd_m);
    if (!event) return dm_sk_recvfrom(fd, data, size, 0, NULL, NULL);
    if (!data || size < sizeof(uint64_t)) return sk_error(SK_EINVAL);
    uint64_t v; if (efd_read(fd, &v)) return -1; memcpy(data, &v, sizeof v); return sizeof v;
}
long dm_sk_write(int fd, const void *data, unsigned long size) {
    dm_host_mutex_lock(&g_efd_m); int event = efd_used(fd); pthread_mutex_unlock(&g_efd_m);
    if (!event) return dm_sk_sendto(fd, data, size, 0, NULL, 0);
    if (!data || size < sizeof(uint64_t)) return sk_error(SK_EINVAL);
    uint64_t v; memcpy(&v, data, sizeof v); return efd_write(fd, v) ? -1 : (long)sizeof v;
}
long dm_sk_recvfrom(int fd, void *data, unsigned long n, int flags, void *a, unsigned *al) {
    dm_host_mutex_lock(&g_efd_m);
    if (sk_host_locked(fd)) {
        int hfd = g_efd[fd].host_fd, stype = g_efd[fd].sock_type, nb = (g_efd[fd].flags & SK_NONBLOCK) || (flags & SK_MSG_DONTWAIT);
        pthread_mutex_unlock(&g_efd_m);
        int hflags = ((flags & SK_MSG_DONTWAIT) ? MSG_DONTWAIT : 0) | ((flags & SK_MSG_PEEK) ? MSG_PEEK : 0);
        for (;;) {
            struct sockaddr_in sa = {0};
            socklen_t slen = sizeof sa;
            if (data && n) dm_guest_write_prepare(data, n);
            dm_host_work++;
            ssize_t r = a ? recvfrom(hfd, data, n, hflags, (struct sockaddr *)&sa, &slen) : recv(hfd, data, n, hflags);
            int err = errno;
            dm_host_work--;
            if (r >= 0) {
                if (stype == SOCK_DGRAM && a) {
                    if (r == 16 && ntohs(sa.sin_port) == 10025 && data) {
                        uint32_t pub = 0; memcpy(&pub, (const uint8_t *)data + 8, 4);
                        if (pub && pub != htonl(0x7F000001u)) {
                            atomic_store(&g_nncs_public_ip, pub);
                            if (net_trace_on()) {
                                char ip[32] = ""; inet_ntop(AF_INET, &pub, ip, sizeof ip);
                                LOG("net_trace: nncs_public_ip=%s", ip);
                            }
                        }
                    } else if (sa.sin_addr.s_addr == htonl(0x7F000001u)) {
                        uint32_t pub = atomic_load(&g_nncs_public_ip);
                        if (pub) sa.sin_addr.s_addr = pub;
                    }
                    if (net_trace_on() && ntohs(sa.sin_port) != 10025) {
                        char ip[32] = ""; inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
                        LOG("net_trace: udp_rx fd=%d <- %s:%u len=%zd", fd, ip, (unsigned)ntohs(sa.sin_port), r);
                    }
                }
                if (a && al) sk_host_addr_out(a, al, &sa);
                return (long)r;
            }
            if (err == EINTR) continue;
            // Suppress spurious ICMP port-unreachable resets on UDP sockets (Nextendo P2P / NAT traversal).
            if (stype == SOCK_DGRAM && (err == ECONNREFUSED || err == ECONNRESET || err == ENETRESET)) {
                if (nb) return sk_host_error(EAGAIN);
                continue;
            }
            return sk_host_error(err);
        }
    }
    pthread_mutex_unlock(&g_efd_m);
    if ((!data && n) || (a && !al)) return sk_error(SK_EINVAL);
    if (flags & ~(SK_MSG_DONTWAIT | SK_MSG_PEEK)) return sk_error(SK_EOPNOTSUPP);
    dm_host_mutex_lock(&g_efd_m); long r = -1;
    if (!sk_udp_locked(fd)) { t_skerr = SK_EBADF; goto done; }
    uint64_t gen = g_efd[fd].generation, start = guest_ns();
    for (;;) {
        if (!sk_udp_locked(fd) || g_efd[fd].generation != gen) { t_skerr = SK_EBADF; break; }
        sk_packet *p = g_efd[fd].head;
        if (p) {
            size_t copied = n < p->size ? n : p->size; if (copied) memcpy(data, p->data, copied); sk_addr_write(a, al, p->addr, p->port); r = (long)copied;
            if (!(flags & SK_MSG_PEEK)) { g_efd[fd].head = p->next; if (!p->next) g_efd[fd].tail = NULL; g_efd[fd].queued -= p->size; g_efd[fd].packets--; free(p); }
            break;
        }
        if (g_efd[fd].shut_read) { r = 0; break; }
        if (flags & SK_MSG_DONTWAIT || g_efd[fd].flags & SK_NONBLOCK) { t_skerr = SK_EAGAIN; break; }
        int64_t left = g_efd[fd].recv_timeout ? g_efd[fd].recv_timeout - (int64_t)(guest_ns() - start) : 100000000;
        if (left <= 0) { t_skerr = SK_EAGAIN; break; }
        if (dm_detclock_enabled() && left > 1000000) left = 1000000;
        dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, left);
    }
done:
    pthread_mutex_unlock(&g_efd_m); return r;
}
// Closed/reused descriptors from this wait report POLLNVAL. Descriptors the host never made retain the prior offline behavior: nothing ready at the timeout.
static int efd_ready(sk_pollfd *p, uint64_t n, uint64_t open, const uint64_t generation[EFD_MAX]) {
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        p[i].revents = 0; int fd = p[i].fd;
        if (fd < 0 || fd >= EFD_MAX || !(open >> fd & 1)) continue;
        if (!sk_used(fd) || g_efd[fd].generation != generation[fd]) { p[i].revents = SK_POLLNVAL; ready++; continue; }
        if (g_efd[fd].used == SK_HOST) continue;
        if (g_efd[fd].used == SK_EFD ? g_efd[fd].count != 0 : (g_efd[fd].head != NULL || g_efd[fd].shut_read)) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
        if (g_efd[fd].used == SK_EFD || !g_efd[fd].shut_write) p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
        if (p[i].revents) ready++;
    }
    return ready;
}
static int sk_poll_mixed(sk_pollfd *p, uint64_t n, int ms) {
    struct pollfd hfds[EFD_MAX];
    uint64_t hidx[EFD_MAX];
    int nh = 0;
    uint64_t open = 0, generation[EFD_MAX] = {0};
    dm_host_mutex_lock(&g_efd_m);
    for (uint64_t i = 0; i < n; i++) {
        p[i].revents = 0;
        int fd = p[i].fd;
        if (fd < 0 || fd >= EFD_MAX || !sk_used(fd)) {
            if (fd >= 0) p[i].revents = SK_POLLNVAL;
            continue;
        }
        open |= 1ull << fd;
        generation[fd] = g_efd[fd].generation;
        if (g_efd[fd].used == SK_HOST && nh < EFD_MAX) {
            short ev = 0;
            if (p[i].events & (SK_POLLIN | SK_POLLRDNORM)) ev |= POLLIN;
            if (p[i].events & (SK_POLLOUT | SK_POLLWRNORM)) ev |= POLLOUT;
            hfds[nh] = (struct pollfd){.fd = g_efd[fd].host_fd, .events = ev, .revents = 0};
            hidx[nh] = i;
            nh++;
        }
    }
    pthread_mutex_unlock(&g_efd_m);

    int64_t limit = ms < 0 ? -1 : (int64_t)ms * 1000000;
    uint64_t start = guest_ns();
    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < n; i++) if (p[i].revents == SK_POLLNVAL) ready++;
        dm_host_mutex_lock(&g_efd_m);
        for (uint64_t i = 0; i < n; i++) {
            int fd = p[i].fd;
            if (fd < 0 || fd >= EFD_MAX || !(open >> fd & 1)) continue;
            if (!sk_used(fd) || g_efd[fd].generation != generation[fd]) { p[i].revents = SK_POLLNVAL; ready++; continue; }
            if (g_efd[fd].used == SK_HOST) continue;
            p[i].revents = 0;
            if (g_efd[fd].used == SK_EFD ? g_efd[fd].count != 0 : (g_efd[fd].head != NULL || g_efd[fd].shut_read)) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
            if (g_efd[fd].used == SK_EFD || !g_efd[fd].shut_write) p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
            if (p[i].revents) ready++;
        }
        pthread_mutex_unlock(&g_efd_m);

        int64_t left_ms = limit < 0 ? 10 : (limit - (int64_t)(guest_ns() - start)) / 1000000;
        if (left_ms < 0) left_ms = 0;
        int slice_ms = ready ? 0 : (left_ms > 10 ? 10 : (int)left_ms);
        if (nh > 0) {
            for (int k = 0; k < nh; k++) hfds[k].revents = 0;
            dm_host_work++;
            int hr = poll(hfds, (nfds_t)nh, slice_ms);
            dm_host_work--;
            if (hr > 0) {
                for (int k = 0; k < nh; k++) {
                    int16_t rev = 0;
                    if (hfds[k].revents & POLLIN) rev |= (p[hidx[k]].events & (SK_POLLIN | SK_POLLRDNORM));
                    if (hfds[k].revents & POLLOUT) rev |= (p[hidx[k]].events & (SK_POLLOUT | SK_POLLWRNORM));
                    if (hfds[k].revents & POLLERR) rev |= SK_POLLERR;
                    if (hfds[k].revents & POLLHUP) rev |= SK_POLLHUP;
                    if (hfds[k].revents & POLLNVAL) rev |= SK_POLLNVAL;
                    if (rev) { p[hidx[k]].revents = rev; ready++; }
                }
            }
        }
        if (ready || (limit >= 0 && left_ms <= 0)) return ready;
    }
}
int dm_sk_poll(void *fds, uint64_t n, int ms) {
    sk_pollfd *p = fds; uint64_t open = 0, generation[EFD_MAX] = {0};
    if (!p && n) return sk_error(SK_EINVAL);
    int has_host = 0;
    dm_host_mutex_lock(&g_efd_m);
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0 && sk_used(p[i].fd)) {
        open |= 1ull << p[i].fd; generation[p[i].fd] = g_efd[p[i].fd].generation;
        if (g_efd[p[i].fd].used == SK_HOST) has_host = 1;
    }
    if (has_host) { pthread_mutex_unlock(&g_efd_m); return sk_poll_mixed(p, n, ms); }
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    if (!open) { pthread_mutex_unlock(&g_efd_m); return sk_idle(ms); }
    // Periodically return from an endless wait, as before, so guest garbage collection/shutdown can observe a thread outside the host hook.
    int64_t limit = (ms < 0 ? 100 : (int64_t)ms) * 1000000; uint64_t start = guest_ns(); int ready;
    while (!(ready = efd_ready(p, n, open, generation))) {
        int64_t left = limit - (int64_t)(guest_ns() - start); if (left <= 0) break;
        dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, dm_detclock_enabled() && left > 1000000 ? 1000000 : left);
    }
    pthread_mutex_unlock(&g_efd_m); return ready;
}
// fd_set is1024 bits; timeval is {i64 seconds, i64 microseconds}. Only this service's small descriptors can become ready.
int dm_sk_select(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, const int64_t *tv) {
    uint64_t *sets[3] = {r, w, e}; static const int16_t ask[3] = {SK_POLLIN, SK_POLLOUT, 0}; sk_pollfd p[EFD_MAX]; int np = 0;
    if (nfds < 0 || (tv && (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000))) return sk_error(SK_EINVAL);
    if (nfds > 1024) nfds = 1024;
    for (int fd = 0; fd < nfds && fd < EFD_MAX; fd++) { int in = 0; int16_t ev = 0;
        for (int k = 0; k < 3; k++) if (sets[k] && sets[k][fd >> 6] >> (fd & 63) & 1) { in = 1; ev |= ask[k]; }
        if (in) p[np++] = (sk_pollfd){fd, ev, 0};
    }
    int64_t ms = tv ? (tv[0] > 0x7fffffff / 1000 ? 0x7fffffff : tv[0] * 1000 + tv[1] / 1000) : -1;
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    dm_sk_poll(p, (uint64_t)np, (int)ms);
    for (int k = 0; k < 3; k++) if (sets[k]) for (int fd = 0; fd < nfds; fd++) sets[k][fd >> 6] &= ~(1ull << (fd & 63));
    int n = 0;
    for (int i = 0; i < np; i++) {
        if (p[i].revents & SK_POLLNVAL) return sk_error(SK_EBADF);
        if (p[i].revents & (SK_POLLIN | SK_POLLRDNORM) && r) { r[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
        if (p[i].revents & SK_POLLOUT && w) { w[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
    }
    return n;
}
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return dm_sk_socket(a, b, c); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { return dm_sk_listen(s, n); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { return dm_sk_accept(s, a, n); }
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return dm_sk_close(s); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 1); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 0); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getopt(s, l, o, v, n); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setopt(s, l, o, v, n); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, int v) { return dm_sk_fcntl(s, c, v); }
// SDK 21 overloads Star Fox imports: plain sockaddr, the ShutdownMethod enum, Read / Write and a name lookup without options; SetAllocator keeps nothing, as no session is created
SKF("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", h_sk_GetPeerName21, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 1); }
SKF("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", h_sk_Shutdown21, int s, int how) { return dm_sk_shutdown(s, how); }
HLE_FORCE("_ZN2nn6socket4ReadEiPvm", "local UDP/EventFd (offline BSD service)", long, h_sk_Read, int s, void *b, unsigned long n) { return dm_sk_read(s, b, n); }
HLE_FORCE("_ZN2nn6socket5WriteEiPKvm", "local UDP/EventFd (offline BSD service)", long, h_sk_Write, int s, const void *b, unsigned long n) { return dm_sk_write(s, b, n); }
// Horizon has two 48-byte addrinfo ABIs:
//   - nn::socket::AddrInfo (FreeBSD/Darwin): offset 24 = ai_canonname, offset 32 = ai_addr
//   - POSIX/nnmusl struct addrinfo (Linux):  offset 24 = ai_addr,      offset 32 = ai_canonname
typedef struct dm_guest_addrinfo {
    int32_t ai_flags, ai_family, ai_socktype, ai_protocol;
    uint32_t ai_addrlen, _pad;
    void *ptr24;
    void *ptr32;
    struct dm_guest_addrinfo *ai_next;
    uint8_t addr_buf[16];
    char canon_buf[256];
} dm_guest_addrinfo;
_Static_assert(offsetof(dm_guest_addrinfo, ai_addrlen) == 16 && offsetof(dm_guest_addrinfo, ptr24) == 24 && offsetof(dm_guest_addrinfo, ptr32) == 32 && offsetof(dm_guest_addrinfo, ai_next) == 40, "guest addrinfo ABI");

static uint16_t parse_service_port(const char *service) {
    if (!service || !*service) return 0;
    if (!strcmp(service, "http")) return 80;
    if (!strcmp(service, "https")) return 443;
    char *end = NULL;
    unsigned long p = strtoul(service, &end, 10);
    return (end && *end == '\0' && p <= 65535) ? (uint16_t)p : 0;
}

int dm_sk_getaddrinfo_ex(const char *host, const char *service, const void *hint, void **res, int posix_layout) {
    if (res) *res = NULL;
    if (!dm_nextendo_enabled() || !res) return 4;   // EAI_FAIL offline
    if (!host || !*host) return 8;                  // EAI_NONAME
    uint32_t ip = 0;
    char sni[256] = "";
    int r = dm_nextendo_resolve_host(host, &ip, sni, sizeof sni);
    LOG("nextendo: GetAddrInfo('%s', serv='%s', posix=%d) -> r=%d ip=%u.%u.%u.%u sni='%s'",
        host, service ? service : "", posix_layout, r, (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, sni);
    if (r != 0) return 8;
    dm_guest_addrinfo *ai = calloc(1, sizeof *ai);
    if (!ai) return 6;   // EAI_MEMORY
    int socktype = SOCK_STREAM, proto = IPPROTO_TCP;
    if (hint) {
        const dm_guest_addrinfo *h = hint;
        if (h->ai_socktype) socktype = h->ai_socktype;
        if (h->ai_protocol) proto = h->ai_protocol;
        else if (socktype == SOCK_DGRAM) proto = IPPROTO_UDP;
    }
    uint16_t port = parse_service_port(service);
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = proto;
    ai->ai_addrlen = 16;
    ai->addr_buf[0] = 16;
    ai->addr_buf[1] = AF_INET;
    ai->addr_buf[2] = (uint8_t)(port >> 8);
    ai->addr_buf[3] = (uint8_t)port;
    ai->addr_buf[4] = (uint8_t)(ip >> 24);
    ai->addr_buf[5] = (uint8_t)(ip >> 16);
    ai->addr_buf[6] = (uint8_t)(ip >> 8);
    ai->addr_buf[7] = (uint8_t)ip;
    strlcpy(ai->canon_buf, sni[0] ? sni : host, sizeof ai->canon_buf);
    if (posix_layout) {
        ai->ptr24 = ai->addr_buf;
        ai->ptr32 = ai->canon_buf;
    } else {
        ai->ptr24 = ai->canon_buf;
        ai->ptr32 = ai->addr_buf;
    }
    *res = ai;
    return 0;
}

int dm_sk_getaddrinfo(const char *host, const char *service, const void *hint, void **res) {
    return dm_sk_getaddrinfo_ex(host, service, hint, res, 0);
}

void dm_sk_freeaddrinfo(void *ai) {
    dm_guest_addrinfo *cur = ai;
    while (cur) {
        dm_guest_addrinfo *next = cur->ai_next;
        free(cur);
        cur = next;
    }
}

// Thread-local hostent for legacy GetHostEntByName callers.
typedef struct {
    char *h_name;
    char **h_aliases;
    int32_t h_addrtype, h_length;
    char **h_addr_list;
} dm_guest_hostent;
static __thread struct {
    dm_guest_hostent ent;
    char name[256];
    char *aliases[1];
    char *addrs[2];
    uint32_t ipv4_be;
} t_hostent;

void *dm_sk_gethostbyname(const char *name) {
    if (!dm_nextendo_enabled() || !name || !*name) { t_skerr = SK_ENETDOWN; return NULL; }
    uint32_t ip = 0;
    char sni[256] = "";
    int r = dm_nextendo_resolve_host(name, &ip, sni, sizeof sni);
    LOG("nextendo: GetHostByName('%s') -> r=%d ip=%u.%u.%u.%u", name, r, (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    if (r != 0) { t_skerr = SK_ENETDOWN; return NULL; }
    memset(&t_hostent, 0, sizeof t_hostent);
    strlcpy(t_hostent.name, sni[0] ? sni : name, sizeof t_hostent.name);
    t_hostent.ipv4_be = htonl(ip);
    t_hostent.addrs[0] = (char *)&t_hostent.ipv4_be;
    t_hostent.addrs[1] = NULL;
    t_hostent.ent.h_name = t_hostent.name;
    t_hostent.ent.h_aliases = t_hostent.aliases;
    t_hostent.ent.h_addrtype = AF_INET;
    t_hostent.ent.h_length = 4;
    t_hostent.ent.h_addr_list = t_hostent.addrs;
    return &t_hostent.ent;
}

HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKc", "name resolution: NULL offline, Nextendo DNS redirect when DM_NEXTENDO=1", void *, h_sk_GetHostEnt21, const char *n) { return dm_sk_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", "socket library allocator; no session is created, ignored", void, h_sk_SetAlloc, void *r) { (void)r; }
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
// The C library's poll (SDK 21's nnmusl, Star Fox): it needs the socket state of the initialization cut above and fails with EFAULT without it, which the game's loop takes as fatal, so it goes
// to the host's Poll too
SKF("poll", h_libc_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", "a descriptor of the bsd service that works offline: a host counter", int, h_sk_EventFd, unsigned long v, int f) { return efd_create(v, f); }
HLE_FORCE("_ZN2nn6socket11EventFdReadEiPm", "reads the host counter", int, h_sk_EventFdRead, int s, unsigned long *v) { uint64_t c = 0; int r = efd_read(s, &c); if (!r && v) *v = c; return r; }
HLE_FORCE("_ZN2nn6socket12EventFdWriteEim", "adds to the host counter and wakes its pollers", int, h_sk_EventFdWrite, int s, unsigned long v) { return efd_write(s, v); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "local UDP (offline BSD service)", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return dm_sk_recvfrom(s, b, n, f, NULL, NULL); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "local UDP (offline BSD service)", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_sk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "local UDP (offline BSD service)", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return dm_sk_sendto(s, b, n, f, NULL, 0); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution: EAI_FAIL offline, Nextendo DNS redirect when DM_NEXTENDO=1", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    return dm_sk_getaddrinfo_ex(h, s, hint, res, 1);
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo", void, h_sk_FreeAddrInfo, void *ai) { dm_sk_freeaddrinfo(ai); }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution: NULL offline, Nextendo DNS redirect when DM_NEXTENDO=1", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_sk_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return dm_sk_socket(a, b, c); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
SKF("nnsocketClose", h_nsk_Close, int s) { return dm_sk_close(s); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 1); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 0); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getopt(s, l, o, v, n); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setopt(s, l, o, v, n); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, int v) { return dm_sk_fcntl(s, c, v); }
SKF("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "local UDP (offline BSD service)", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return dm_sk_recvfrom(s, b, n, f, NULL, NULL); }
HLE_FORCE("nnsocketSend", "local UDP (offline BSD service)", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return dm_sk_sendto(s, b, n, f, NULL, 0); }
HLE_FORCE("nnsocketSendTo", "local UDP (offline BSD service)", long, h_nsk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return dm_sk_sendto(s, b, n, f, a, al); }
HLE_FORCE("nnsocketRecvFrom", "local UDP (offline BSD service)", long, h_nsk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_sk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("nnsocketRead", "local UDP/EventFd (offline BSD service)", long, h_nsk_Read, int s, void *b, unsigned long n) { return dm_sk_read(s, b, n); }
HLE_FORCE("nnsocketWrite", "local UDP/EventFd (offline BSD service)", long, h_nsk_Write, int s, const void *b, unsigned long n) { return dm_sk_write(s, b, n); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution: NULL offline, Nextendo DNS redirect when DM_NEXTENDO=1", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_sk_gethostbyname(n); }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl: offline fails connections; when DM_NEXTENDO=1, wraps host TCP sockets with SecureTransport ----------
enum { SSL_CONN_MAX = 32 };
static pthread_mutex_t g_ssl_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { void *guest; dm_ssl_conn *conn; } g_ssl_tab[SSL_CONN_MAX];

static dm_ssl_conn *ssl_get(void *guest) {
    if (!guest) return NULL;
    pthread_mutex_lock(&g_ssl_mu);
    dm_ssl_conn *c = NULL;
    for (int i = 0; i < SSL_CONN_MAX; i++) if (g_ssl_tab[i].guest == guest) { c = g_ssl_tab[i].conn; break; }
    pthread_mutex_unlock(&g_ssl_mu);
    return c;
}
static int ssl_put(void *guest, dm_ssl_conn *c) {
    if (!guest || !c) return -1;
    pthread_mutex_lock(&g_ssl_mu);
    int slot = -1;
    for (int i = 0; i < SSL_CONN_MAX; i++) {
        if (g_ssl_tab[i].guest == guest) { dm_nextendo_ssl_destroy(g_ssl_tab[i].conn); g_ssl_tab[i].conn = c; pthread_mutex_unlock(&g_ssl_mu); return 0; }
        if (!g_ssl_tab[i].guest && slot < 0) slot = i;
    }
    if (slot >= 0) { g_ssl_tab[slot].guest = guest; g_ssl_tab[slot].conn = c; }
    pthread_mutex_unlock(&g_ssl_mu);
    return slot >= 0 ? 0 : -1;
}
static dm_ssl_conn *ssl_del(void *guest) {
    if (!guest) return NULL;
    pthread_mutex_lock(&g_ssl_mu);
    dm_ssl_conn *c = NULL;
    for (int i = 0; i < SSL_CONN_MAX; i++) if (g_ssl_tab[i].guest == guest) { c = g_ssl_tab[i].conn; g_ssl_tab[i].guest = NULL; g_ssl_tab[i].conn = NULL; break; }
    pthread_mutex_unlock(&g_ssl_mu);
    return c;
}

#define SSF(m, cname, ...) HLE_FORCE(m, "ssl service: fails offline, TLS via Nextendo when DM_NEXTENDO=1", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "same as nnsslInitialize: succeeds, creates no session (cut)", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "pairs with Initialize (cut)", Result, h_ssl_FinCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", "a session of the local ssl service: succeeds offline, connects to nothing (cut)", Result, h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context7DestroyEv", "pairs with Create (cut)", Result, h_ssl_CtxDestroy, void *self) { (void)self; return RES_OK; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)p; if (id) *id = 1; return dm_nextendo_enabled() ? RES_OK : RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return dm_nextendo_enabled() ? RES_OK : RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) {
    (void)ids;
    if (dm_nextendo_enabled()) {
        uint32_t sz = dm_nextendo_builtin_certs_bufsize(n);
        LOG("nextendo: ssl::GetBuiltInCertificateBufSize(n=%u) -> %u", n, sz);
        if (o) *o = sz;
        return sz ? RES_OK : RES_FAIL;
    }
    if (o) *o = 0;
    return RES_FAIL;
}
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) {
    (void)ids;
    if (dm_nextendo_enabled()) {
        uint32_t cnt = 0;
        int ok = dm_nextendo_builtin_certs_fill(o, b, bs, n, &cnt) == 0;
        LOG("nextendo: ssl::GetBuiltInCertificates(bs=%u, n=%u) -> ok=%d count=%u", bs, n, ok, cnt);
        return ok ? RES_OK : RES_FAIL;
    }
    return RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) {
    (void)ctx;
    if (!dm_nextendo_enabled()) return RES_FAIL;
    dm_ssl_conn *c = dm_nextendo_ssl_create();
    if (!c) return RES_FAIL;
    if (ssl_put(self, c) != 0) { dm_nextendo_ssl_destroy(c); return RES_FAIL; }
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) {
    if (!dm_nextendo_enabled()) return RES_FAIL;
    dm_ssl_conn *c = ssl_del(self);
    if (c) dm_nextendo_ssl_destroy(c);
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c) return RES_FAIL;
    dm_host_work++;
    int r = dm_nextendo_ssl_handshake(c);
    dm_host_work--;
    if (r == 0) return RES_OK;
    return r == -2 ? 0x19901u : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c || !h) return RES_FAIL;
    char buf[256];
    size_t cp = n < sizeof buf - 1 ? n : sizeof buf - 1;
    memcpy(buf, h, cp); buf[cp] = '\0';
    return dm_nextendo_ssl_set_hostname(c, buf) == 0 ? RES_OK : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c) return RES_FAIL;
    return dm_nextendo_ssl_set_io_mode(c, m == 2) == 0 ? RES_OK : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return dm_nextendo_enabled() ? RES_OK : RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    int hfd = dm_sk_host_fd(s);
    if (!c || hfd < 0) return RES_FAIL;
    return dm_nextendo_ssl_set_fd(c, hfd) == 0 ? RES_OK : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c) { if (r) *r = RES_FAIL; return RES_FAIL; }
    if (r) *r = dm_nextendo_ssl_last_error(c);
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) {
    if (!dm_nextendo_enabled() || !ssl_get(self)) { if (r) *r = RES_FAIL; return RES_FAIL; }
    if (r) *r = RES_OK;
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c) { if (o) *o = 0; return RES_FAIL; }
    dm_host_work++;
    int r = dm_nextendo_ssl_read(c, b, n, 1);
    dm_host_work--;
    if (r >= 0) { if (o) *o = r; return RES_OK; }
    if (o) *o = 0;
    return r == -2 ? 0x19901u : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c || !o || !i) return RES_FAIL;
    uint32_t in_ev = 0; memcpy(&in_ev, i, sizeof in_ev);
    dm_host_work++;
    int rev = dm_nextendo_ssl_poll(c, (in_ev & 1) != 0, (in_ev & 2) != 0, (int)t);
    dm_host_work--;
    if (rev < 0) return RES_FAIL;
    uint32_t out_ev = (uint32_t)rev; memcpy(o, &out_ev, sizeof out_ev);
    return RES_OK;
}
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl service: read returns -1 offline, TLS read when DM_NEXTENDO=1", int, h_ssl_CnRead, void *self, char *b, unsigned n) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c) return -1;
    dm_host_work++;
    int r = dm_nextendo_ssl_read(c, b, n, 0);
    dm_host_work--;
    return r >= 0 ? r : -1;
}
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl service: write returns -1 offline, TLS write when DM_NEXTENDO=1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) {
    dm_ssl_conn *c = dm_nextendo_enabled() ? ssl_get(self) : NULL;
    if (!c) return -1;
    dm_host_work++;
    int r = dm_nextendo_ssl_write(c, b, n);
    dm_host_work--;
    return r >= 0 ? r : -1;
}
// C interface: all return nnResult
#define SSC(m, cname) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, void) { return RES_FAIL; }
SSC("nnsslConnectionCreate", h_nssl_Create) SSC("nnsslConnectionDestroy", h_nssl_Destroy)
SSC("nnsslConnectionDoHandshakeWithCertBuffer", h_nssl_Handshake) SSC("nnsslConnectionGetConnectionId", h_nssl_GetId)
SSC("nnsslConnectionGetNextAlpnProto", h_nssl_GetAlpn) SSC("nnsslConnectionGetVerifyCertError", h_nssl_VerifyErr)
SSC("nnsslConnectionPeek", h_nssl_Peek) SSC("nnsslConnectionPending", h_nssl_Pending) SSC("nnsslConnectionRead", h_nssl_Read)
SSC("nnsslConnectionSetHostName", h_nssl_SetHost) SSC("nnsslConnectionSetIoMode", h_nssl_SetIo)
SSC("nnsslConnectionSetNextAlpnProto", h_nssl_SetAlpn) SSC("nnsslConnectionSetOption", h_nssl_SetOpt)
SSC("nnsslConnectionSetSessionCacheMode", h_nssl_SetCache) SSC("nnsslConnectionSetSocketDescriptor", h_nssl_SetSock)
SSC("nnsslConnectionSetVerifyOption", h_nssl_SetVerify) SSC("nnsslConnectionWrite", h_nssl_Write)
SSC("nnsslContextDestroy", h_nssl_CtxDestroy) SSC("nnsslContextGetContextId", h_nssl_CtxGetId)
