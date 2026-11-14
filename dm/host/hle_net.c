// Network services: offline by default. Offline, the BSD service supplies process-local UDP and EventFd transports, which offline engines use for their own threads; an
// explicit network mode translates the guest's sockets to Darwin's instead (net_socket.h).
#include "hle.h"
#include "hle_net.h"
#include "net_socket.h"
#include <pthread.h>
#include <stdatomic.h>
void dm_guest_sleep_ns(int64_t ns);

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
static _Atomic int g_nifm_pending;
static _Atomic int g_nifm_hold;   // queries the pending request still answers "on hold" (0 = none, or settled)
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "offline: the request is on hold for the first query, then settles as unavailable", void, h_nifm_Submit, void) { atomic_store(&g_nifm_pending, 1); atomic_store(&g_nifm_hold, 1); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "offline: drops the pending request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_pending, 0); atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "offline: on hold for the first query after the submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
int dm_nifm_available(void) { return dm_net_enabled() && atomic_load(&g_nifm_pending) && !atomic_load(&g_nifm_hold) && !dm_net_ipconfig(NULL, NULL, NULL, NULL, NULL); }
void dm_nifm_submit_wait(void) { if (dm_net_enabled()) { h_nifm_Submit(); while (h_nifm_OnHold()) dm_guest_sleep_ns(0); } }
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "offline by default; explicit LAN reports the actual IPv4 request state", int, h_nifm_Available, void) { return dm_nifm_available(); }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "whether the network request was resolved without a system UI", unsigned, h_nifm_HandleErr, void) { return dm_nifm_available() != 0; }
HLE_FORCE("_ZN2nn4nifm26HandleNetworkRequestResultEv", "SDK 4 network request result without a system UI", Result, h_nifm_HandleResult, void) { return dm_nifm_available() ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "offline by default; explicit LAN returns the actual interface IPv4 address", Result, h_nifm_GetIp, uint32_t *a) { if (!a) return RES_FAIL; *a = 0; return dm_net_ipconfig(a, NULL, NULL, NULL, NULL) ? RES_FAIL : RES_OK; }
// The proxy setting (nifm::ProxySetting, 0xaa bytes: enabled +0, port +2, server +4, auto-auth +0x68, user +0x69, password +0x89, as Super Mario Bros. Wonder 1.2.1
// reads it: libcurl's proxy code at main+0xf7aa40 clears 0xaa bytes first, and gRPC's proxy mapper at main+0xb31760 asks too). In a network mode the host uses no
// proxy, so it answers as a console without one: success, every field zero. Offline it still fails
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "network mode: no proxy (every field zero); offline: no proxy setting", Result, h_nifm_GetProxy, void *p) {
    if (!p || !dm_net_enabled()) return RES_FAIL;
    memset(p, 0, 0xaa); static int told; if (!told++) LOG("nifm: proxy setting asked: none (the host uses no proxy)"); return RES_OK;
}

// ---------- BSD service: local UDP/EventFd offline (remote networking unavailable); an explicit network mode translates the guest ABI to Darwin ----------
// Local transports are used even by offline games: RakNet sends a startup datagram to itself in Divinity: Original Sin 2. No host socket is opened. Descriptors belong to
// this service, not Darwin's fd table, and stay small enough for games' fd_set arrays. Address/flag/error constants below are the Horizon/FreeBSD ABI (libnx's BSD service passes
// these structures/flags unchanged; FreeBSD11 sys/socket.h and netinet/in.h). IPv4 payloads retain packet boundaries; the per-endpoint queue holds at most256 packets/1MiB.
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
// Both keep one errno (dm_net_errno). The offline service sets the FreeBSD numbers below; the network transport sets the Linux numbers hle_net_backend.c documents.
#define SK_ENETDOWN 50
// The game's errno. On Horizon nn::socket's errno is the C library's: detail::SetLastErrno and GetLastErrno write and read *__errno_location(), and code such
// as gRPC's reads errno itself. Super Mario Bros. Wonder's gRPC sets 97 when it leaves IPv6 (main+0xb61480) and reads errno after its non-blocking connect
// (main+0xb61e38, tcp_client_posix.cc:299): with only the host's copy set, it took that connect's EINPROGRESS for the stale 97 and gave the address up.
// A failed hook writes it on the game's thread, at the hook boundary; a game without __errno_location keeps only the host's copy
static int *(*_Atomic g_guest_errno)(void); static _Atomic int g_guest_errno_looked;
static int *guest_errno_location(void) {
    if (!atomic_load(&g_guest_errno_looked)) { g_guest_errno = (int *(*)(void))guest_sym("__errno_location"); atomic_store(&g_guest_errno_looked, 1); }
    int *(*loc)(void) = g_guest_errno; return loc ? loc() : NULL;
}
long dm_net_guest_errno(long r) { if (r < 0) { int *e = guest_errno_location(); if (e) *e = dm_net_errno(); } return r; }
int dm_net_guest_errno_get(void) { int *e = guest_errno_location(); return e ? *e : dm_net_errno(); }
static int sk_error(int error) { dm_net_set_errno(error); return -1; }
static int sk_fail(void) { return sk_error(dm_net_enabled() ? 95 : SK_ENETDOWN); }
int dm_sk_unavailable(void) { return sk_fail(); }
int dm_sk_errno(void) { return dm_net_errno(); }
#define SKF(m, cname, ...) HLE_FORCE(m, "socket call: the local BSD descriptor service offline, translated Darwin sockets in an explicit network mode", int, cname, __VA_ARGS__)
#define SKW(m, cname, ...) HLE_FORCE_WAIT(m, "blocking socket call with a pausable guest context: the local service offline, Darwin sockets in a network mode", int, cname, __VA_ARGS__)
// A call with both forms: the network transport in an explicit network mode, the local service otherwise; a failure also sets the game's errno
#define SKD(net, local) dm_net_guest_errno(dm_net_enabled() ? (long)(net) : (long)(local))
void dm_guest_sleep_ns(int64_t ns);
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns);   // hle_thread.c
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
enum { EFD_MAX = 64, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4, SK_POLLNVAL = 0x20,
       SK_EBADF = 9, SK_EAGAIN = 35, SK_EINVAL = 22, SK_EMSGSIZE = 40, SK_EOPNOTSUPP = 45, SK_EADDRINUSE = 48, SK_ENOTCONN = 57,
       SK_NONBLOCK = 0x800, SK_MSG_PEEK = 2, SK_MSG_DONTWAIT = 0x80, SK_UDP = 2, SK_EFD = 1,
       SK_UDP_MAX = 65507, SK_QUEUE_MAX = 1024 * 1024, SK_PACKET_MAX = 256, SK_DEFAULT_BUF = 256 * 1024 };
typedef struct sk_packet { struct sk_packet *next; uint32_t addr; uint16_t port; size_t size; uint8_t data[]; } sk_packet;
static struct {
    uint64_t count, generation; int used, flags, fdflags;
    uint32_t addr, peer_addr; uint16_t port, peer_port;
    int connected, shut_read, shut_write, reuse, broadcast, rcvbuf, sndbuf;
    int64_t recv_timeout, send_timeout; sk_packet *head, *tail; size_t queued; unsigned packets;
} g_efd[EFD_MAX];
static uint64_t g_sk_generation;
static uint16_t g_sk_ephemeral = 49152;
static pthread_mutex_t g_efd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_c = PTHREAD_COND_INITIALIZER;
static int sk_used(int fd) { return fd >= 0 && fd < EFD_MAX && g_efd[fd].used; }   // with g_efd_m held
static int efd_used(int fd) { return sk_used(fd) && g_efd[fd].used == SK_EFD; }
static int sk_new_locked(int kind) {
    for (int i = 0; i < EFD_MAX; i++) if (!g_efd[i].used) {
        memset(&g_efd[i], 0, sizeof g_efd[i]); g_efd[i].used = kind; g_efd[i].generation = ++g_sk_generation;
        g_efd[i].rcvbuf = g_efd[i].sndbuf = SK_DEFAULT_BUF; return i;
    }
    return sk_error(24);   // EMFILE
}
static int sk_udp_locked(int fd) { return sk_used(fd) && g_efd[fd].used == SK_UDP; }
static int efd_create(uint64_t v, int flags) {
    dm_host_mutex_lock(&g_efd_m); int fd = sk_new_locked(SK_EFD);
    if (fd >= 0) { g_efd[fd].count = v; g_efd[fd].flags = flags; }
    pthread_mutex_unlock(&g_efd_m); return fd;
}
int dm_sk_socket(int family, int type, int protocol) {
    if (family != 2 || type != 2 || (protocol && protocol != 17)) return sk_fail();
    dm_host_mutex_lock(&g_efd_m); int fd = sk_new_locked(SK_UDP); pthread_mutex_unlock(&g_efd_m); return fd;
}
int dm_sk_close(int fd) {
    dm_host_mutex_lock(&g_efd_m);
    if (!sk_used(fd)) { pthread_mutex_unlock(&g_efd_m); return sk_fail(); }
    sk_packet *p = g_efd[fd].head;
    while (p) { sk_packet *next = p->next; free(p); p = next; }
    g_efd[fd].head = g_efd[fd].tail = NULL; g_efd[fd].used = 0; pthread_cond_broadcast(&g_efd_c);
    pthread_mutex_unlock(&g_efd_m); return 0;
}
static int efd_read(int fd, uint64_t *out) {
    dm_host_mutex_lock(&g_efd_m); int r = -1;
    uint64_t gen = efd_used(fd) ? g_efd[fd].generation : 0;
    for (;;) {
        if (!efd_used(fd) || g_efd[fd].generation != gen) { dm_net_set_errno(SK_EBADF); break; }
        uint64_t *c = &g_efd[fd].count; int f = g_efd[fd].flags;
        if (*c) { if (f & 1) { *out = 1; *c -= 1; } else { *out = *c; *c = 0; } r = 0; break; }
        if (f & 6) { dm_net_set_errno(SK_EAGAIN); break; }
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
    uint32_t addr; uint16_t port; if (sk_addr_read(a, n, &addr, &port)) return -1;
    dm_host_mutex_lock(&g_efd_m); int r = sk_udp_locked(fd) ? sk_bind_locked(fd, addr, port) : sk_error(SK_EBADF); pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_connect(int fd, const void *a, unsigned n) {
    uint32_t addr = 0; uint16_t port = 0; int disconnect = a && n >= 2 && ((const uint8_t *)a)[1] == 0;
    if (!disconnect && sk_addr_read(a, n, &addr, &port)) return -1;
    if (!disconnect && !port) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m); int r = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (!g_efd[fd].port && sk_bind_locked(fd, 0, 0)) r = -1;
    else { g_efd[fd].connected = !disconnect; g_efd[fd].peer_addr = addr ? addr : 0x7f000001; g_efd[fd].peer_port = port; }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_name(int fd, void *a, unsigned *n, int peer) {
    if (!a || !n) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m); int r;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (peer && !g_efd[fd].connected) r = sk_error(SK_ENOTCONN);
    else r = sk_addr_write(a, n, peer ? g_efd[fd].peer_addr : g_efd[fd].addr, peer ? g_efd[fd].peer_port : g_efd[fd].port);
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_shutdown(int fd, int how) {
    dm_host_mutex_lock(&g_efd_m); int r = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (how < 0 || how > 2) r = sk_error(SK_EINVAL);
    else if (!g_efd[fd].connected) r = sk_error(SK_ENOTCONN);
    else { if (how != 1) g_efd[fd].shut_read = 1; if (how != 0) g_efd[fd].shut_write = 1; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_fcntl(int fd, int cmd, int value) {
    dm_host_mutex_lock(&g_efd_m); int r = 0;
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
    dm_host_mutex_lock(&g_efd_m); int r = 0;
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
    dm_host_mutex_lock(&g_efd_m); int r = 0, v = 0;
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
long dm_sk_sendto(int fd, const void *data, unsigned long n, int flags, const void *a, unsigned al) {
    if ((!data && n) || n > SK_UDP_MAX) return sk_error(n > SK_UDP_MAX ? SK_EMSGSIZE : SK_EINVAL);
    if (flags & ~(SK_MSG_DONTWAIT | 4 | 0x20000)) return sk_error(SK_EOPNOTSUPP);
    uint32_t addr = 0; uint16_t port = 0; if (a && sk_addr_read(a, al, &addr, &port)) return -1;
    dm_host_mutex_lock(&g_efd_m); long r = -1;
    if (!sk_udp_locked(fd)) { dm_net_set_errno(SK_EBADF); goto done; }
    if (g_efd[fd].shut_write) { dm_net_set_errno(32); goto done; }
    if (!a) { if (!g_efd[fd].connected) { dm_net_set_errno(39); goto done; } addr = g_efd[fd].peer_addr; port = g_efd[fd].peer_port; }
    if (!port) { dm_net_set_errno(SK_EINVAL); goto done; }
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
            if (!p) { dm_net_set_errno(55); goto done; }   // ENOBUFS
            p->next = NULL; p->addr = src; p->port = g_efd[fd].port; p->size = n; if (n) memcpy(p->data, data, n);
            if (g_efd[i].tail) g_efd[i].tail->next = p; else g_efd[i].head = p;
            g_efd[i].tail = p; g_efd[i].queued += n; g_efd[i].packets++; pthread_cond_broadcast(&g_efd_c);
        }
    }
    r = (long)n;
done:
    pthread_mutex_unlock(&g_efd_m); return r;
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
    if ((!data && n) || (a && !al)) return sk_error(SK_EINVAL);
    if (flags & ~(SK_MSG_DONTWAIT | SK_MSG_PEEK)) return sk_error(SK_EOPNOTSUPP);
    dm_host_mutex_lock(&g_efd_m); long r = -1;
    if (!sk_udp_locked(fd)) { dm_net_set_errno(SK_EBADF); goto done; }
    uint64_t gen = g_efd[fd].generation, start = guest_ns();
    for (;;) {
        if (!sk_udp_locked(fd) || g_efd[fd].generation != gen) { dm_net_set_errno(SK_EBADF); break; }
        sk_packet *p = g_efd[fd].head;
        if (p) {
            size_t copied = n < p->size ? n : p->size; if (copied) memcpy(data, p->data, copied); sk_addr_write(a, al, p->addr, p->port); r = (long)copied;
            if (!(flags & SK_MSG_PEEK)) { g_efd[fd].head = p->next; if (!p->next) g_efd[fd].tail = NULL; g_efd[fd].queued -= p->size; g_efd[fd].packets--; free(p); }
            break;
        }
        if (g_efd[fd].shut_read) { r = 0; break; }
        if (flags & SK_MSG_DONTWAIT || g_efd[fd].flags & SK_NONBLOCK) { dm_net_set_errno(SK_EAGAIN); break; }
        int64_t left = g_efd[fd].recv_timeout ? g_efd[fd].recv_timeout - (int64_t)(guest_ns() - start) : 100000000;
        if (left <= 0) { dm_net_set_errno(SK_EAGAIN); break; }
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
        if (g_efd[fd].used == SK_EFD ? g_efd[fd].count != 0 : (g_efd[fd].head != NULL || g_efd[fd].shut_read)) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
        if (g_efd[fd].used == SK_EFD || !g_efd[fd].shut_write) p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
        if (p[i].revents) ready++;
    }
    return ready;
}
int dm_sk_poll(void *fds, uint64_t n, int ms) {   // also hle_offline.c's plain pollfd form
    if (dm_net_enabled()) return dm_net_poll(fds, n, ms);   // the transport's sockets and event descriptors
    sk_pollfd *p = fds; uint64_t open = 0, generation[EFD_MAX] = {0};
    if (!p && n) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m);
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0 && sk_used(p[i].fd)) { open |= 1ull << p[i].fd; generation[p[i].fd] = g_efd[p[i].fd].generation; }
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
// fd_set is1024 bits; timeval is {i64 seconds, i64 microseconds}. Only this service's small descriptors can become ready. An explicit network mode goes to the transport's Select instead
int dm_sk_select(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, const int64_t *tv) {
    if (dm_net_enabled()) return dm_net_select(nfds, r, w, e, tv);
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
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return SKD(dm_net_socket(a, b, c), dm_sk_socket(a, b, c)); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return SKD(dm_net_bind(s, a, n), dm_sk_bind(s, a, n)); }
SKW("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { return SKD(dm_net_connect(s, a, n), dm_sk_connect(s, a, n)); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { return SKD(dm_net_listen(s, n), sk_fail()); }
SKW("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { return SKD(dm_net_accept(s, a, n), sk_fail()); }
SKW("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return SKD(dm_net_close(s), dm_sk_close(s)); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { return SKD(dm_net_name(s, a, n, 1), dm_sk_name(s, a, n, 1)); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return SKD(dm_net_name(s, a, n, 0), dm_sk_name(s, a, n, 0)); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return SKD(dm_net_sockopt(s, l, o, v, n, 0), dm_sk_getopt(s, l, o, v, n)); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return SKD(dm_net_sockopt(s, l, o, (void *)v, &n, 1), dm_sk_setopt(s, l, o, v, n)); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, int v) { return SKD(dm_net_fcntl(s, c, v), dm_sk_fcntl(s, c, v)); }
SKW("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_net_guest_errno(dm_sk_poll(f, n, t)); }
SKW("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_net_guest_errno(dm_sk_select(n, r, w, e, t)); }
// SDK 21 overloads Star Fox imports: plain sockaddr, the ShutdownMethod enum, Read / Write and a name lookup without options; SetAllocator keeps nothing, as no session is created. All go where their SockAddr forms do (the overloads only differ in the C type they spell)
SKF("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", h_sk_GetPeerName21, int s, void *a, unsigned *n) { return SKD(dm_net_name(s, a, n, 1), dm_sk_name(s, a, n, 1)); }
SKF("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", h_sk_Shutdown21, int s, int how) { return SKD(dm_net_shutdown(s, how), dm_sk_shutdown(s, how)); }
HLE_FORCE_WAIT("_ZN2nn6socket4ReadEiPvm", "socket read: local UDP/EventFd offline, the transport in a network mode", long, h_sk_Read, int s, void *b, unsigned long n) { return SKD(dm_net_recv(s, b, n, 0, NULL, NULL), dm_sk_read(s, b, n)); }
HLE_FORCE_WAIT("_ZN2nn6socket5WriteEiPKvm", "socket write: local UDP/EventFd offline, the transport in a network mode", long, h_sk_Write, int s, const void *b, unsigned long n) { return SKD(dm_net_send(s, b, n, 0, NULL, 0), dm_sk_write(s, b, n)); }
HLE_FORCE_WAIT("_ZN2nn6socket16GetHostEntByNameEPKc", "name resolution, offline by default", void *, h_sk_GetHostEnt21, const char *n) { return dm_net_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", "socket library allocator; no session is created, ignored", void, h_sk_SetAlloc, void *r) { (void)r; }
// The C library's poll (SDK 21's nnmusl, Star Fox): it needs the socket state of the initialization cut above and fails with EFAULT without it, which the game's loop takes as fatal, so it goes
// to the host's Poll too
SKF("poll", h_libc_Poll, void *f, unsigned long n, int t) { return dm_net_guest_errno(dm_sk_poll(f, n, t)); }
// EventFd works in every mode. Offline it is the counter table above (a garbage collector of the game's own wakes its pollers, Ocarina of Time at startup). In a network mode it is a
// descriptor of the transport's socket table (dm_net_eventfd, hle_net_backend.c), numbered with the sockets, so the transport's Poll and Select see both: gRPC polls its sockets and
// its wakeup descriptor together (nn::npln). A blocking read waits as the host's other waits do: dm_guest_cond_wait_ns offline, the transport's bracketed host waits otherwise
HLE_FORCE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", "a counter descriptor: the host's table offline, the transport's socket table in a network mode", int, h_sk_EventFd, unsigned long v, int f) { int rv = dm_net_enabled() ? dm_net_eventfd(v, f) : efd_create(v, f); return (int)dm_net_guest_errno(rv); }
HLE_FORCE("_ZN2nn6socket11EventFdReadEiPm", "reads the counter; blocks unless the descriptor never blocks", int, h_sk_EventFdRead, int s, unsigned long *v) { uint64_t c = 0; int rv = dm_net_enabled() ? dm_net_eventfd_read(s, &c) : efd_read(s, &c); if (!rv && v) *v = c; return (int)dm_net_guest_errno(rv); }
HLE_FORCE("_ZN2nn6socket12EventFdWriteEim", "adds to the counter and wakes its pollers", int, h_sk_EventFdWrite, int s, unsigned long v) { int rv = dm_net_enabled() ? dm_net_eventfd_write(s, v) : efd_write(s, v); return (int)dm_net_guest_errno(rv); }
// Resolver options (nn::socket::ResolverOption: its key is the first word). The host resolver has none to apply: setting one is accepted and not applied, and reading one fails,
// as there is no value to give. Each key is logged once, so a game that depends on one shows it
static int sk_res_option(const void *o, int key, int set) {
    if (set) key = o ? *(const int32_t *)o : -1;
    static _Atomic uint64_t seen[2]; uint64_t bit = key >= 0 && key < 64 ? 1ull << key : 1ull << 63;
    if (!(atomic_fetch_or(&seen[set], bit) & bit)) LOG("%ssocket: resolver option %d %s", set ? "" : "[RED] ", key, set ? "set: accepted, the host resolver does not apply it" : "read: the host resolver has none, fails");
    if (set && dm_net_enabled() && o) return 0;
    return sk_fail();
}
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; return sk_res_option(NULL, k, 0); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { return sk_res_option(o, 0, 1); }
HLE_FORCE_WAIT("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "socket call: local UDP offline, the transport in a network mode", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return SKD(dm_net_recv(s, b, n, f, NULL, NULL), dm_sk_recvfrom(s, b, n, f, NULL, NULL)); }
HLE_FORCE_WAIT("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "socket call: local UDP offline, the transport in a network mode", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return SKD(dm_net_recv(s, b, n, f, a, al), dm_sk_recvfrom(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "socket call: local UDP offline, the transport in a network mode", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return SKD(dm_net_send(s, b, n, f, NULL, 0), dm_sk_sendto(s, b, n, f, NULL, 0)); }
HLE_FORCE_WAIT("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    return dm_net_getaddrinfo(h, s, hint, res);
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "release only resolver allocations owned by the host", void, h_sk_FreeAddrInfo, void *ai) { dm_net_freeaddrinfo(ai); }
HLE_FORCE_WAIT("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution, offline by default", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_net_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return dm_net_errno(); }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return SKD(dm_net_socket(a, b, c), dm_sk_socket(a, b, c)); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return SKD(dm_net_bind(s, a, n), dm_sk_bind(s, a, n)); }
SKW("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { return SKD(dm_net_connect(s, a, n), dm_sk_connect(s, a, n)); }
SKW("nnsocketClose", h_nsk_Close, int s) { return SKD(dm_net_close(s), dm_sk_close(s)); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { return SKD(dm_net_name(s, a, n, 1), dm_sk_name(s, a, n, 1)); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return SKD(dm_net_name(s, a, n, 0), dm_sk_name(s, a, n, 0)); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return SKD(dm_net_sockopt(s, l, o, v, n, 0), dm_sk_getopt(s, l, o, v, n)); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return SKD(dm_net_sockopt(s, l, o, (void *)v, &n, 1), dm_sk_setopt(s, l, o, v, n)); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, int v) { return SKD(dm_net_fcntl(s, c, v), dm_sk_fcntl(s, c, v)); }
SKW("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_net_guest_errno(dm_sk_poll(f, n, t)); }
HLE_FORCE_WAIT("nnsocketGetHostByNameCancel", "real IPv4 resolution with an independent cancellation ID", void *, h_nsk_GetHostCancel, const char *name, uint32_t id) { return dm_net_gethostbyname_cancel(name, id); }
HLE_FORCE_WAIT("_ZN2nn6socket13GetHostByNameEPKci", "real IPv4 resolution with an independent cancellation ID", void *, h_sk_GetHostCancel, const char *name, uint32_t id) { return dm_net_gethostbyname_cancel(name, id); }
HLE_FORCE("nnsocketRequestCancelHandle", "allocate a nonzero resolver cancellation identity, not a socket", uint32_t, h_nsk_RequestCancel, void) { return dm_net_request_cancel_handle(); }
HLE_FORCE("_ZN2nn6socket19RequestCancelHandleEv", "allocate a nonzero resolver cancellation identity, not a socket", uint32_t, h_sk_RequestCancel, void) { return dm_net_request_cancel_handle(); }
SKW("nnsocketCancel", h_nsk_Cancel, uint32_t id) { return dm_net_guest_errno(dm_net_cancel(id)); }
SKW("_ZN2nn6socket6CancelEi", h_sk_Cancel, uint32_t id) { return dm_net_guest_errno(dm_net_cancel(id)); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; return sk_res_option(NULL, k, 0); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { return sk_res_option(o, 0, 1); }
HLE_FORCE_WAIT("nnsocketRecv", "socket receive: local UDP offline, the transport in a network mode", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return SKD(dm_net_recv(s, b, n, f, NULL, NULL), dm_sk_recvfrom(s, b, n, f, NULL, NULL)); }
HLE_FORCE_WAIT("nnsocketSend", "socket send: local UDP offline, the transport in a network mode", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return SKD(dm_net_send(s, b, n, f, NULL, 0), dm_sk_sendto(s, b, n, f, NULL, 0)); }
HLE_FORCE_WAIT("nnsocketSendTo", "socket send: local UDP offline, the transport in a network mode", long, h_nsk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return SKD(dm_net_send(s, b, n, f, a, al), dm_sk_sendto(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("nnsocketRecvFrom", "socket receive: local UDP offline, the transport in a network mode", long, h_nsk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return SKD(dm_net_recv(s, b, n, f, a, al), dm_sk_recvfrom(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("nnsocketRead", "socket read: local UDP/EventFd offline, the transport in a network mode", long, h_nsk_Read, int s, void *b, unsigned long n) { return SKD(dm_net_recv(s, b, n, 0, NULL, NULL), dm_sk_read(s, b, n)); }
HLE_FORCE_WAIT("nnsocketWrite", "socket write: local UDP/EventFd offline, the transport in a network mode", long, h_nsk_Write, int s, const void *b, unsigned long n) { return SKD(dm_net_send(s, b, n, 0, NULL, 0), dm_sk_write(s, b, n)); }
HLE_FORCE_WAIT("nnsocketGetHostByNameWithOptions", "name resolution, offline by default", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_net_gethostbyname(n); }
HLE_FORCE("nnsocketGetLastErrno", "the game's errno, as nn::socket::GetLastErrno reads it", int, h_nsk_GetErrno, void) { return dm_net_guest_errno_get(); }
HLE_FORCE("nnsocketSetLastErrno", "sets the host's copy and the game's errno", void, h_nsk_SetErrno, int e) { dm_net_set_errno(e); dm_net_guest_errno(-1); }
// ---------- ssl: hle_ssl.c (the client TLS service, offline and online) ----------
