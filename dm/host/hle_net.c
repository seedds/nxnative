// Host BSD sockets and opt-in local-network NIFM support. Internet services, friends and SSL remain offline.
#include "hle.h"
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <limits.h>
#include <unistd.h>

static int host_ipv4(uint32_t *ip, uint32_t *mask);
static uint32_t sk_address(uint32_t ip, int to_host);

static int lan_enabled(void) {
    const char *peers = getenv("DM_LAN_PEERS");
    const char *mode = getenv("DM_LAN");
    return (peers && *peers) || (mode && strcmp(mode, "0") != 0);
}
static _Atomic int g_lan_status_logged;
static _Atomic int g_socket_initialized;
typedef struct sk_icmp_fd { int fd, guest_type; struct sk_icmp_fd *next; } sk_icmp_fd;
static sk_icmp_fd *g_icmp_fds;
static pthread_mutex_t g_icmp_lock = PTHREAD_MUTEX_INITIALIZER;
static int sk_icmp_type(int fd) {
    pthread_mutex_lock(&g_icmp_lock); int found = 0;
    for (sk_icmp_fd *p = g_icmp_fds; p; p = p->next) if (p->fd == fd) { found = p->guest_type; break; }
    pthread_mutex_unlock(&g_icmp_lock); return found;
}
static int sk_is_icmp(int fd) { return sk_icmp_type(fd) != 0; }
Result dm_sk_initialize(void) { atomic_store(&g_socket_initialized, 1); return RES_OK; }

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "initializes host BSD sockets", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return dm_sk_initialize(); }
HLE_FORCE("_ZN2nn6socket8internal10InitializeEi", "initializes native BSD sockets instead of connecting to Horizon bsd services", Result, h_socket_InternalInit, int concurrency) { (void)concurrency; if (lan_enabled()) LOG("LAN socket runtime initialized (native BSD)"); return dm_sk_initialize(); }
HLE_FORCE("_ZN2nn6socket13IsInitializedEv", "native socket runtime initialization state", int, h_socket_IsInitialized, void) { return lan_enabled() && atomic_load(&g_socket_initialized); }
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
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "offline: the request is on hold for the first query, then settles as unavailable", void, h_nifm_Submit, void) { atomic_store(&g_nifm_hold, 1); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "offline: drops the pending request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "offline: on hold for the first query after the submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "opt-in host network availability for LAN play (DM_LAN)", int, h_nifm_Available, void) {
    int available = lan_enabled() && host_ipv4(NULL, NULL);
    if (available && !atomic_exchange(&g_lan_status_logged, 1)) {
        uint32_t ip = 0, mask = 0; host_ipv4(&ip, &mask);
        if (getenv("DM_LAN_PEERS") && *getenv("DM_LAN_PEERS")) { ip = sk_address(ip, 0); mask = htonl(0xffffff00); }
        struct in_addr ia = {.s_addr = ip}, ma = {.s_addr = mask}; char ipbuf[INET_ADDRSTRLEN], maskbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ia, ipbuf, sizeof(ipbuf)); inet_ntop(AF_INET, &ma, maskbuf, sizeof(maskbuf));
        LOG("LAN mode enabled: host IPv4 %s, reported mask %s, peer relay %s", ipbuf, maskbuf, getenv("DM_LAN_PEERS") ?: "(local network)");
    }
    return available;
}
// Returns bool (`and w0, w0, #1`, Metroid Dread's sdk+0x336210): whether the system's network screen resolved the error
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "offline: not resolved, shows no system network screen", unsigned, h_nifm_HandleErr, void) { return 0; }
static int host_ipv4(uint32_t *ip, uint32_t *mask) {
    if (ip) *ip = 0;
    if (mask) *mask = 0;
    struct ifaddrs *all = NULL;
    if (getifaddrs(&all) != 0) return 0;
    int found = 0;
    const char *selected = getenv("DM_LAN_IP");
    struct in_addr wanted = {0};
    if (selected && inet_pton(AF_INET, selected, &wanted) != 1) { freeifaddrs(all); return 0; }
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP) || (!selected && (p->ifa_flags & IFF_LOOPBACK))) continue;
        const struct sockaddr_in *a = (const struct sockaddr_in *)p->ifa_addr;
        if (selected && a->sin_addr.s_addr != wanted.s_addr) continue;
        const struct sockaddr_in *m = p->ifa_netmask ? (const struct sockaddr_in *)p->ifa_netmask : NULL;
        if (ip) *ip = a->sin_addr.s_addr;
        if (mask && m) *mask = m->sin_addr.s_addr;
        found = 1;
        break;
    }
    freeifaddrs(all);
    return found;
}
int dm_nifm_ipv4(uint32_t *ip, uint32_t *mask) {
    if (!lan_enabled()) { if (ip) *ip = 0; if (mask) *mask = 0; return 0; }
    int found = host_ipv4(ip, mask);
    if (found && getenv("DM_LAN_PEERS") && *getenv("DM_LAN_PEERS")) {
        if (ip) *ip = sk_address(*ip, 0);
        if (mask) *mask = htonl(0xffffff00);
    }
    return found;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "host primary IPv4 address for opt-in LAN play", Result, h_nifm_GetIp, uint32_t *a) { return dm_nifm_ipv4(a, NULL) ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "offline: no proxy setting", Result, h_nifm_GetProxy, void *p) { if (getenv("DM_LAN_TRACE")) LOG("LAN GetCurrentProxySetting requested"); (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "connects to the ssl service; the network library's global init needs it to succeed, offline creates no session (cut)", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "pairs with nnsslInitialize (cut)", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- socket: host BSD socket operations ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 100                 // Horizon BSD service uses Linux/newlib errno numbers
static __thread int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; if (getenv("DM_LAN_TRACE")) { LOG("LAN operation reached an unimplemented socket entry"); dm_backtrace((uint64_t)__builtin_frame_address(0)); } return -1; }
static int sk_errno(int error) {
    switch (error) {
    case EAGAIN: return 11; case EDEADLK: return 35;
    case EINPROGRESS: return 115; case EALREADY: return 114;
    case ENOTSOCK: return 88; case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90; case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92; case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94; case ENOTSUP: return 95;
    case EOPNOTSUPP: return 95; case EPFNOSUPPORT: return 96;
    case EAFNOSUPPORT: return 97; case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99; case ENETDOWN: return 100;
    case ENETUNREACH: return 101; case ENETRESET: return 102;
    case ECONNABORTED: return 103; case ECONNRESET: return 104;
    case ENOBUFS: return 105; case EISCONN: return 106;
    case ENOTCONN: return 107; case ESHUTDOWN: return 108;
    case ETOOMANYREFS: return 109; case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111; case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113; case ELOOP: return 40;
    case ENAMETOOLONG: return 36; case ENOTEMPTY: return 39;
    case ENOSYS: return 38; case EOVERFLOW: return 75;
    case ECANCELED: return 125;
    default: return error >= 0 && error <= 34 ? error : EIO;
    }
}
static int sk_result(int r) { if (r < 0) { t_skerr = sk_errno(errno); if (getenv("DM_LAN_TRACE") && errno != EAGAIN) LOG("LAN socket call failed: %d (%s)", errno, strerror(errno)); } return r; }
static long sk_lresult(long r) { if (r < 0) { t_skerr = sk_errno(errno); if (getenv("DM_LAN_TRACE") && errno != EAGAIN) LOG("LAN data call failed: %d (%s)",errno,strerror(errno)); } return r; }
int dm_sk_finish_int(int r) { return sk_result(r); }
long dm_sk_finish(long r) { return sk_lresult(r); }
// A routed peer list forms one virtual /24, like an emulator room. Every host
// sorts the same real endpoints, giving each endpoint the same virtual address.
// Only socket addresses are translated; the router and its routes are untouched.
static int sk_endpoints(uint32_t out[33]) {
    uint32_t local = 0; if (!host_ipv4(&local, NULL)) return 0;
    out[0] = local; int count = 1;
    const char *peers = getenv("DM_LAN_PEERS");
    if (!peers || !*peers) return 0;
    char list[512]; snprintf(list, sizeof(list), "%s", peers);
    char *save = NULL;
    for (char *s = strtok_r(list, ", \t", &save); s && count < 33; s = strtok_r(NULL, ", \t", &save)) {
        struct in_addr a; if (inet_pton(AF_INET, s, &a) != 1) continue;
        int seen = 0; for (int j = 0; j < count; j++) if (out[j] == a.s_addr) seen = 1;
        if (!seen) out[count++] = a.s_addr;
    }
    for (int i = 1; i < count; i++) for (int j = i; j && ntohl(out[j]) < ntohl(out[j-1]); j--) {
        uint32_t t = out[j]; out[j] = out[j-1]; out[j-1] = t;
    }
    return count;
}
static uint32_t sk_address(uint32_t ip, int to_host) {
    uint32_t endpoints[33]; int n = sk_endpoints(endpoints);
    for (int i = 0; i < n; i++) {
        uint32_t virtual_ip = htonl(0xac1ffe01u + i); // 172.31.254.1 onward
        if (ip == (to_host ? virtual_ip : endpoints[i])) return to_host ? endpoints[i] : virtual_ip;
    }
    return ip;
}
static void sk_translate(void *address, socklen_t length, int to_host) {
    if (address && length >= offsetof(struct sockaddr_in, sin_addr) + sizeof(struct in_addr)) {
        struct sockaddr_in *a = address;
        if (a->sin_family == AF_INET) a->sin_addr.s_addr = sk_address(a->sin_addr.s_addr, to_host);
    }
}
static void sk_icmp_header(int fd, void *buffer, size_t length) {
    if (length < 20 || !sk_is_icmp(fd)) return;
    uint8_t *b = buffer; size_t header = (b[0] & 15) * 4;
    if ((b[0] >> 4) != 4 || header < 20 || header > length) return;
    // Darwin's ping socket delivers the IPv4 header, just as the guest raw
    // socket does. Its addresses must agree with the virtual recvfrom sender.
    for (int off = 12; off <= 16; off += 4) {
        uint32_t ip; memcpy(&ip, b + off, 4); ip = sk_address(ip, 0); memcpy(b + off, &ip, 4);
    }
    b[10] = b[11] = 0; unsigned sum = 0;
    for (size_t i = 0; i < header; i += 2) sum += ((unsigned)b[i] << 8) | b[i+1];
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    sum = ~sum; b[10] = sum >> 8; b[11] = sum;
}
long dm_sk_recv(int fd, void *buf, size_t len, int flags) {
    long r = recv(fd, buf, len, flags); if (r > 0) sk_icmp_header(fd, buf, r);
    return sk_lresult(r);
}
int dm_sk_name(int fd, void *address, socklen_t *length, int peer) {
    if (!address || !length) { errno = EFAULT; return sk_result(-1); }
    socklen_t capacity = *length;
    int r = peer ? getpeername(fd, address, length) : getsockname(fd, address, length);
    if (r == 0 && length) sk_translate(address, capacity < *length ? capacity : *length, 0);
    return sk_result(r);
}
long dm_sk_recvfrom(int fd, void *buf, size_t len, int flags, void *address, socklen_t *length) {
    socklen_t capacity = length ? *length : 0;
    long r = recvfrom(fd, buf, len, flags, address, length);
    if (r > 0) sk_icmp_header(fd, buf, r);
    if (r >= 0 && length) sk_translate(address, capacity < *length ? capacity : *length, 0);
    return sk_lresult(r);
}
int dm_sk_errno(void) { return t_skerr; }
int dm_sk_socket(int family, int type, int protocol) {
    if (!lan_enabled()) return sk_fail();
    if (family != AF_INET) { errno = EAFNOSUPPORT; return sk_result(-1); }
    // Horizon permits application ICMP raw sockets. Darwin reserves raw sockets
    // for root, but exposes ICMP echo through unprivileged datagram sockets.
    int guest_type = type;
    if (family == AF_INET && type == SOCK_RAW && protocol == IPPROTO_ICMP) type = SOCK_DGRAM;
    int r = sk_result(socket(family, type, protocol));
    if (r >= 0 && family == AF_INET && protocol == IPPROTO_ICMP) {
        sk_icmp_fd *p = malloc(sizeof(*p));
        if (!p) { close(r); errno = ENOMEM; return sk_result(-1); }
        p->fd = r; p->guest_type = guest_type;
        pthread_mutex_lock(&g_icmp_lock); p->next = g_icmp_fds; g_icmp_fds = p; pthread_mutex_unlock(&g_icmp_lock);
    }
    if (getenv("DM_LAN_TRACE")) LOG("LAN socket(%d,%d,%d) = %d", family, type, protocol, r);
    return r;
}
int dm_sk_close(int fd) {
    pthread_mutex_lock(&g_icmp_lock);
    for (sk_icmp_fd **p = &g_icmp_fds; *p; p = &(*p)->next) if ((*p)->fd == fd) {
        sk_icmp_fd *old = *p; *p = old->next; free(old); break;
    }
    pthread_mutex_unlock(&g_icmp_lock);
    return sk_result(close(fd));
}
int dm_sk_setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
    int r = sk_result(setsockopt(fd, level, option, value, length));
    if (getenv("DM_LAN_TRACE")) LOG("LAN setsockopt(%d,%#x,%#x,%u) = %d", fd,level,option,length,r);
    return r;
}
int dm_sk_getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (!value || !length) { errno = EFAULT; return sk_result(-1); }
    socklen_t capacity = *length;
    int guest_type = sk_icmp_type(fd);
    if (level == SOL_SOCKET && option == SO_TYPE && guest_type && *length >= sizeof(int)) {
        *(int *)value = guest_type; *length = sizeof(int); return 0;
    }
    int r = sk_result(getsockopt(fd, level, option, value, length));
    if (r == 0 && level == SOL_SOCKET && option == SO_ERROR && capacity >= sizeof(int) && *length >= sizeof(int)) *(int *)value = sk_errno(*(int *)value);
    return r;
}
int dm_sk_fcntl(int fd, int cmd, long arg) {
    if (getenv("DM_LAN_TRACE")) LOG("LAN fcntl(%d,%d,%#lx)",fd,cmd,arg);
    if (cmd == F_GETFD) return sk_result(fcntl(fd, F_GETFD));
    if (cmd == F_SETFD) return sk_result(fcntl(fd, F_SETFD, (int)arg));
    // Horizon uses FreeBSD O_NONBLOCK (0x800); Darwin uses 0x4.
    if (cmd == F_GETFL) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0) return sk_result(flags);
        return (flags & O_NONBLOCK) ? 0x800 : 0;
    }
    if (cmd == F_SETFL) {
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0) return sk_result(flags);
        return sk_result(fcntl(fd, F_SETFL, (flags & ~O_NONBLOCK) | ((arg & 0x800) ? O_NONBLOCK : 0)));
    }
    t_skerr = EINVAL; return -1;
}
static int sk_peer_allowed(uint32_t ip) {
    const char *peers = getenv("DM_LAN_PEERS"); if (!peers || !*peers) return 0;
    char list[512]; size_t n = strlen(peers); if (n >= sizeof(list)) n = sizeof(list) - 1;
    memcpy(list, peers, n); list[n] = 0;
    for (char *p = list; p && *p;) {
        char *next = strchr(p, ','); if (next) *next++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p); while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        struct in_addr candidate;
        if (*p && inet_pton(AF_INET, p, &candidate) == 1 && candidate.s_addr == ip) return 1;
        p = next;
    }
    return 0;
}
static int sk_dest_allowed(const void *address, socklen_t length) {
    if (!lan_enabled() || !address || length < sizeof(struct sockaddr_in)) return 0;
    const struct sockaddr_in *a = address;
    if (a->sin_family != AF_INET) return 0;
    uint32_t ip, mask;
    uint32_t actual = sk_address(a->sin_addr.s_addr, 1);
    if (a->sin_addr.s_addr == htonl(INADDR_BROADCAST) || a->sin_addr.s_addr == htonl(0xac1ffeff) || sk_peer_allowed(actual)) return 1;
    if (!host_ipv4(&ip, &mask)) return 0;
    return actual == ip || (mask && (actual & mask) == (ip & mask));
}
int dm_sk_bind(int fd, const void *address, socklen_t length) {
    if (getenv("DM_LAN_TRACE") && address && length >= sizeof(struct sockaddr_in)) { const struct sockaddr_in *a = address; LOG("LAN bind(%d,%#x:%u)",fd,ntohl(a->sin_addr.s_addr),ntohs(a->sin_port)); }
    if (!lan_enabled()) { errno = ENETDOWN; return sk_result(-1); }
    if (!address) { errno = EFAULT; return sk_result(-1); }
    struct sockaddr_storage copy;
    if (address && length <= sizeof(copy)) { memcpy(&copy, address, length); sk_translate(&copy, length, 1); address = &copy; }
    if (address && length >= sizeof(struct sockaddr_in) && ((const struct sockaddr_in *)address)->sin_family == AF_INET) {
        const struct sockaddr_in *a = address; uint32_t ip = 0;
        if (a->sin_addr.s_addr != htonl(INADDR_ANY) && (!host_ipv4(&ip, NULL) || ip != a->sin_addr.s_addr)) { errno = EACCES; return sk_result(-1); }
    }
    return sk_result(bind(fd, address, length));
}
int dm_sk_listen(int fd, int backlog) {
    if (!lan_enabled()) { errno = ENETDOWN; return sk_result(-1); }
    return sk_result(listen(fd, backlog));
}
int dm_sk_connect(int fd, const void *address, socklen_t length) {
    if (!sk_dest_allowed(address, length)) { errno = EACCES; return sk_result(-1); }
    struct sockaddr_storage copy;
    if (address && length <= sizeof(copy)) { memcpy(&copy, address, length); sk_translate(&copy, length, 1); address = &copy; }
    return sk_result(connect(fd, address, length));
}
int dm_sk_accept(int fd, void *address, socklen_t *length) {
    socklen_t capacity = length ? *length : 0;
    int accepted = accept(fd, address, length);
    if (accepted < 0) return sk_result(accepted);
    struct sockaddr_storage remote; socklen_t remote_length = sizeof(remote);
    if (getpeername(accepted, (struct sockaddr *)&remote, &remote_length) || !sk_dest_allowed(&remote, remote_length)) { close(accepted); errno = EACCES; return sk_result(-1); }
    if (length) sk_translate(address, capacity < *length ? capacity : *length, 0);
    return accepted;
}
long dm_sk_sendto(int fd, const void *buf, size_t len, int flags, const void *address, socklen_t address_len) {
    const struct sockaddr_in *to = address;
    if (getenv("DM_LAN_TRACE") && to && address_len >= sizeof(*to)) LOG("LAN sendto(%d,%#x:%u,%zu bytes)",fd,ntohl(to->sin_addr.s_addr),ntohs(to->sin_port),len);
    if (!sk_dest_allowed(address, address_len)) { errno = EACCES; return sk_lresult(-1); }
    if (to && address_len >= sizeof(*to) && to->sin_family == AF_INET &&
        to->sin_addr.s_addr != htonl(INADDR_BROADCAST) && to->sin_addr.s_addr != htonl(0xac1ffeff)) {
        struct sockaddr_in mapped = *to; sk_translate(&mapped, sizeof(mapped), 1);
        return sk_lresult(sendto(fd, buf, len, flags, (struct sockaddr *)&mapped, sizeof(mapped)));
    }
    struct sockaddr_in broadcast = *to;
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    long sent = sendto(fd, buf, len, flags, (struct sockaddr *)&broadcast, sizeof(broadcast));
    const char *peers = getenv("DM_LAN_PEERS");
    if (!peers || !*peers) return sk_lresult(sent);
    char list[512];
    size_t ncopy = strlen(peers); if (ncopy >= sizeof(list)) ncopy = sizeof(list) - 1;
    memcpy(list, peers, ncopy); list[ncopy] = 0;
    char *p = list;
    int relayed = 0;
    while (p && *p) {
        char *next = strchr(p, ','); if (next) *next++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p); while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        struct sockaddr_in peer = *to;
        if (*p && inet_pton(AF_INET, p, &peer.sin_addr) == 1) {
            long r = sendto(fd, buf, len, flags, (struct sockaddr *)&peer, sizeof(peer));
            if (r >= 0) { relayed++; if (getenv("DM_LAN_TRACE")) LOG("LAN discovery relayed to %s:%u (%ld bytes)", p, ntohs(peer.sin_port), r); }
            else LOG("LAN discovery relay to %s:%u failed: %s", p, ntohs(peer.sin_port), strerror(errno));
        }
        p = next;
    }
    if (sent >= 0 || relayed) return (long)len;
    return sk_lresult(-1);
}
long dm_sk_sendmsg(int fd, const void *message, int flags) {
    const struct msghdr *m = message;
    if (m && m->msg_name && !sk_dest_allowed(m->msg_name, (socklen_t)m->msg_namelen)) { errno = EACCES; return sk_lresult(-1); }
    if (!m || !m->msg_name || m->msg_namelen != sizeof(struct sockaddr_in)) return sk_lresult(sendmsg(fd, m, flags));
    struct msghdr copy = *m;
    struct sockaddr_in to = *(const struct sockaddr_in *)m->msg_name;
    copy.msg_name = &to;
    int broadcast = to.sin_addr.s_addr == htonl(INADDR_BROADCAST) || to.sin_addr.s_addr == htonl(0xac1ffeff);
    if (broadcast) to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    else sk_translate(&to, sizeof(to), 1);
    long result = sendmsg(fd, &copy, flags);
    if (broadcast) {
        uint32_t endpoints[33], local = 0; int n = sk_endpoints(endpoints);
        host_ipv4(&local, NULL);
        for (int i = 0; i < n; i++) if (endpoints[i] != local) {
            to.sin_addr.s_addr = endpoints[i];
            long r = sendmsg(fd, &copy, flags); if (r >= 0) result = r;
        }
    }
    return sk_lresult(result);
}
long dm_sk_recvmsg(int fd, void *message, int flags) {
    struct msghdr *m = message;
    socklen_t capacity = m ? m->msg_namelen : 0;
    long r = recvmsg(fd, m, flags);
    if (r > 0 && m && m->msg_iovlen > 0) sk_icmp_header(fd, m->msg_iov[0].iov_base, (size_t)r < m->msg_iov[0].iov_len ? (size_t)r : m->msg_iov[0].iov_len);
    if (r >= 0 && m) sk_translate(m->msg_name, capacity < m->msg_namelen ? capacity : m->msg_namelen, 0);
    return sk_lresult(r);
}
#define SKF(m, cname, ...) HLE_FORCE(m, "native host BSD socket operation", int, cname, __VA_ARGS__)
// Host poll/select operate on the host socket descriptors returned by Socket.
void dm_guest_sleep_ns(int64_t ns);
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
int dm_sk_poll(void *fds, uint64_t n, int ms) {   // also hle_offline.c's plain pollfd form
    if (n > (uint64_t)INT_MAX) { t_skerr = EINVAL; return -1; }
    if (!lan_enabled()) {
        struct pollfd *p = fds;
        // Preserve upstream's offline SDK 21 behavior for unavailable descriptors.
        for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) {
            static int once; if (!once++) LOG("socket: Poll of %llu descriptors with one the host never made (fd %d, events %#x; %d ms): nothing ready at the timeout", (unsigned long long)n, p[i].fd, p[i].events, ms);
            break;
        }
        for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
        return sk_idle(ms);
    }
    int r = poll(fds, (nfds_t)n, ms);
    return sk_result(r);
}
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 seconds, i64 microseconds}
    if (!lan_enabled()) {
        const uint64_t *sets[3] = {r,w,e};
        for (int k = 0; k < 3; k++) for (int fd = 0; sets[k] && fd < nfds && fd < 1024; fd++) if (sets[k][fd >> 6] >> (fd & 63) & 1) return sk_fail();
        return sk_idle(tv ? tv[0] * 1000 + tv[1] / 1000 : -1);
    }
    struct timeval timeout, *timeoutp = NULL;
    if (tv) { timeout.tv_sec = (time_t)tv[0]; timeout.tv_usec = (suseconds_t)tv[1]; timeoutp = &timeout; }
    int result = select(nfds, (fd_set *)r, (fd_set *)w, (fd_set *)e, timeoutp);
    return sk_result(result);
}
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return dm_sk_socket(a, b, c); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { return dm_sk_listen(s, n); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { return dm_sk_accept(s, a, (socklen_t *)n); }
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return dm_sk_close(s); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, (socklen_t *)n, 1); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, (socklen_t *)n, 0); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getsockopt(s, l, o, v, (socklen_t *)n); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setsockopt(s, l, o, v, n); }
HLE_FORCE("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", "native host BSD socket descriptor flags", int, h_sk_Fcntl, int s, int c, long arg) {
    return dm_sk_fcntl(s, c, arg);
}
// SDK 21 overloads Star Fox imports: plain sockaddr, the ShutdownMethod enum, Read / Write and a name lookup without options; SetAllocator keeps nothing, as no session is created
SKF("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", h_sk_GetPeerName21, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", h_sk_Shutdown21, int s, int how) { (void)s; (void)how; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4ReadEiPvm", "bsd socket service (cut)", long, h_sk_Read, int s, void *b, unsigned long n) { (void)s; (void)b; (void)n; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket5WriteEiPKvm", "bsd socket service (cut)", long, h_sk_Write, int s, const void *b, unsigned long n) { (void)s; (void)b; (void)n; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKc", "name resolution (cut): NULL", void *, h_sk_GetHostEnt21, const char *n) { (void)n; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", "socket library allocator; no session is created, ignored", void, h_sk_SetAlloc, void *r) { (void)r; }
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
// The C library's poll (SDK 21's nnmusl, Star Fox): it needs the socket state of the initialization cut above and fails with EFAULT without it, which the game's loop takes as fatal, so it goes
// to the host's Poll too
SKF("poll", h_libc_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
SKF("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", h_sk_EventFd, unsigned long v, int f) { LOG("LAN EventFd requested: initial %lu, flags %#x (not implemented)", v, f); return sk_fail(); }
SKF("_ZN2nn6socket11EventFdReadEiPm", h_sk_EventFdRead, int s, unsigned long *v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket12EventFdWriteEim", h_sk_EventFdWrite, int s, unsigned long v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "native host BSD socket receive", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return dm_sk_recv(s, b, n, f); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "native host BSD socket receive", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_sk_recvfrom(s, b, n, f, a, (socklen_t *)al); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "native host BSD socket send", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return sk_lresult(send(s, b, n, f)); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    if (getenv("DM_LAN_TRACE")) LOG("LAN GetAddrInfo: %s:%s", h ?: "(null)", s ?: "(null)");
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4;
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAddrInfo, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution (cut): NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return dm_sk_socket(a, b, c); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
SKF("nnsocketClose", h_nsk_Close, int s) { return dm_sk_close(s); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, (socklen_t *)n, 1); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, (socklen_t *)n, 0); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getsockopt(s, l, o, v, (socklen_t *)n); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setsockopt(s, l, o, v, n); }
HLE_FORCE("nnsocketFcntl", "native host BSD socket descriptor flags", int, h_nsk_Fcntl, int s, int c, long arg) {
    return dm_sk_fcntl(s, c, arg);
}
SKF("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "native host BSD socket receive", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return dm_sk_recv(s, b, n, f); }
HLE_FORCE("nnsocketSend", "native host BSD socket send", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return sk_lresult(send(s, b, n, f)); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution (cut): NULL", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl: offline, Initialize and a context's Create / Destroy succeed, everything else that contacts the service fails ----------
// A context is a session of the local ssl service, so on Horizon Create works with no network and only a connection needs one; DRAGON QUEST BUILDERS 2 ends its startup when Create fails (main+0x861260).
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "same as nnsslInitialize: succeeds, creates no session (cut)", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "pairs with Initialize (cut)", Result, h_ssl_FinCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", "a session of the local ssl service: succeeds offline, connects to nothing (cut)", Result, h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context7DestroyEv", "pairs with Create (cut)", Result, h_ssl_CtxDestroy, void *self) { (void)self; return RES_OK; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) { (void)self; (void)s; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl service (cut): read returns -1", int, h_ssl_CnRead, void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl service (cut): write returns -1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
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
