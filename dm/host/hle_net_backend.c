// Opt-in Apple IPv4 transport. The default offline hooks retain their previous answers.
// ABI references: Atmosphere-NX socket_types.hpp / socket_errno.hpp; switchbrew/libnx socket.c.
// Horizon uses BSD sockaddr (length + u8 family), Linux errno numbers, O_NONBLOCK=0x800,
// Modern nn::socket::AddrInfo has addr before canonname; SDK 4's plain addrinfo has
// the opposite order (verified in MK8 3.0.5's DNSSerializer at sdk+0xafa94). Never cast guest structs to Darwin structs.
#include "net_socket.h"
#include "discovery_protocol.h"
#include <TargetConditionals.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#if TARGET_OS_OSX
#include <net/route.h>
#include <sys/sysctl.h>
#endif
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <time.h>
#include <unistd.h>

void dm_guest_write_prepare(void *addr, size_t len);
void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);

enum { NET_MAX = 1024, NX_NONBLOCK = 0x800, NET_SLICE_MS = 100 };
typedef struct { uint8_t len, family; uint16_t port; uint32_t address; uint8_t zero[8]; } nx_addr;
typedef struct { int64_t sec, usec; } nx_timeval;
_Static_assert(sizeof(nx_addr) == 16, "Horizon sockaddr_in");
_Static_assert(sizeof(dm_net_addrinfo) == 48 && offsetof(dm_net_addrinfo, addr) == 24, "Horizon addrinfo");
_Static_assert(sizeof(dm_net_addrinfo_legacy) == 48 && offsetof(dm_net_addrinfo_legacy, addr) == 32, "SDK 4 addrinfo");
_Static_assert(sizeof(dm_net_pollfd) == 8, "Horizon pollfd");
typedef struct { int native, used, nonblock, type; uint64_t generation; int64_t send_ms, recv_ms; } net_slot;
typedef struct { int fd, guest, nonblock, type; uint64_t generation; int64_t send_ms, recv_ms; } net_ref;
static net_slot g_sockets[NET_MAX];
static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_net_multicast_allowed = TARGET_OS_OSX;
static _Atomic uint32_t g_net_broadcast;
static _Atomic int g_net_broadcast_known;
typedef struct { uint32_t address, mask, gateway; unsigned index, flags; int local_link; } net_interface;
static int net_interface_get(net_interface *out);
#ifdef DM_NET_TEST
static net_interface g_test_interface;
static int g_test_interface_set;
void dm_net_test_set_interface(const dm_net_test_interface *p) {
    g_test_interface_set = p != NULL;
    if (p) g_test_interface = (net_interface){p->address, p->mask, 0, p->index, p->flags, p->local_link};
    atomic_store(&g_net_broadcast_known, 0);
}
#endif
static __thread int t_net_errno = 100; // Horizon's BSD service and nn::socket expose Linux errno numbers (not Darwin's).

dm_net_mode_t dm_net_mode(void) {
#if TARGET_OS_OSX || TARGET_OS_IOS
    const char *s = getenv("DM_NETWORK");
    if (s && !strcmp(s, "lan")) return DM_NET_LAN;
    if (s && !strcmp(s, "ldn")) return DM_NET_LDN;
    if (s && !strcmp(s, "auto")) return DM_NET_AUTO;
    return DM_NET_OFFLINE;
#else
    return DM_NET_OFFLINE;
#endif
}
int dm_net_enabled(void) { return dm_net_mode() != DM_NET_OFFLINE; }
void dm_net_set_multicast_allowed(int allowed) { atomic_store(&g_net_multicast_allowed, !!allowed); }
int dm_net_multicast_allowed(void) { return atomic_load(&g_net_multicast_allowed); }
static int net_discovery_unicast_plan(dm_net_discovery_plan *p) {
    net_interface adapter;
    if (net_interface_get(&adapter) || !adapter.index || !(adapter.flags & IFF_UP) ||
        !(adapter.flags & IFF_BROADCAST) || (adapter.flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) {
        dm_net_set_errno(99); return -1;
    }
#if TARGET_OS_IOS
    if (!adapter.local_link) { dm_net_set_errno(99); return -1; }
#endif
    if (dm_net_discovery_subnet(p, adapter.address, adapter.mask)) { dm_net_set_errno(95); return -1; }
    return 0;
}
int dm_net_discovery_prepare(uint32_t destination, dm_net_discovery_plan *p) {
    if (!p) { dm_net_set_errno(14); return -1; }
    memset(p, 0, sizeof *p);
    const char *peer = getenv("DM_NET_DISCOVERY_PEER");
    if (peer && *peer) {
        struct in_addr address;
        if (inet_pton(AF_INET, peer, &address) != 1 || !address.s_addr ||
            (ntohl(address.s_addr) & 0xf0000000u) == 0xe0000000u ||
            ntohl(address.s_addr) >= 0xf0000000u) { dm_net_set_errno(22); return -1; }
        p->target = address.s_addr; p->count = 1; return 0;
    }
    if (dm_net_multicast_allowed()) { p->target = destination; p->count = 1; return 0; }
    return net_discovery_unicast_plan(p);
}
int dm_net_discovery_target(uint32_t destination, uint32_t *target) {
    if (!target) { dm_net_set_errno(14); return -1; }
    dm_net_discovery_plan p;
    if (dm_net_discovery_prepare(destination, &p)) return -1;
    // A connected UDP endpoint cannot represent multiple peers. SendTo and LDN
    // use the full plan; never silently connect to the first scanned address.
    if (p.automatic) { dm_net_set_errno(95); return -1; }
    *target = p.target; return 0;
}
// Only discovery destinations may be redirected. Ordinary peer traffic keeps its
// original address, including after a lobby supplies the actual peer endpoint.
static int net_is_discovery(uint32_t address, int type) {
    if (type != 2) return 0;
    uint32_t host = ntohl(address);
    int discovery = host == UINT32_MAX || (host & 0xf0000000u) == 0xe0000000u;
    if (!discovery) {
        if (!atomic_load(&g_net_broadcast_known)) {
            dm_net_ipconfig(NULL, NULL, NULL, NULL, NULL);
            atomic_store(&g_net_broadcast_known, 1);
        }
        uint32_t broadcast = atomic_load(&g_net_broadcast);
        discovery = broadcast && address == broadcast;
    }
    return discovery;
}
int dm_net_discovery_supplement(uint32_t destination,dm_net_discovery_plan *plan) {
    if(!plan)return 0;memset(plan,0,sizeof *plan);
    const char *peer=getenv("DM_NET_DISCOVERY_PEER");dm_net_mode_t mode=dm_net_mode();
    if(!dm_net_multicast_allowed()||(peer&&*peer)||(mode!=DM_NET_LAN&&mode!=DM_NET_LDN&&mode!=DM_NET_AUTO)||!net_is_discovery(destination,2))return 0;
    int saved=t_net_errno,result=!net_discovery_unicast_plan(plan);t_net_errno=saved;return result;
}
int dm_net_errno(void) { return t_net_errno; }
void dm_net_set_errno(int e) { t_net_errno = e; }
static int net_error(int e) { t_net_errno = e; return -1; }
static int net_gate(void) { return dm_net_enabled() ? 1 : (net_error(100), 0); }
static int net_errno(int e) {
    switch (e) {
        case 0: return 0;
        case EPERM: return 1; case ENOENT: return 2; case EINTR: return 4; case EIO: return 5;
        case EBADF: return 9; case EAGAIN: return 11; case ENOMEM: return 12; case EACCES: return 13;
        case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17; case ENODEV: return 19;
        case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case EPIPE: return 32;
        case ENAMETOOLONG: return 36; case ENOSYS: return 38; case EOVERFLOW: return 75;
        case ENOTSOCK: return 88; case EDESTADDRREQ: return 89; case EMSGSIZE: return 90;
        case EPROTOTYPE: return 91; case ENOPROTOOPT: return 92; case EPROTONOSUPPORT: return 93;
        case ESOCKTNOSUPPORT: return 94; case EOPNOTSUPP: return 95; case EPFNOSUPPORT: return 96;
        case EAFNOSUPPORT: return 97; case EADDRINUSE: return 98; case EADDRNOTAVAIL: return 99;
        case ENETDOWN: return 100; case ENETUNREACH: return 101; case ENETRESET: return 102;
        case ECONNABORTED: return 103; case ECONNRESET: return 104; case ENOBUFS: return 105;
        case EISCONN: return 106; case ENOTCONN: return 107; case ESHUTDOWN: return 108;
        case ETIMEDOUT: return 110; case ECONNREFUSED: return 111; case EHOSTDOWN: return 112;
        case EHOSTUNREACH: return 113; case EALREADY: return 114; case EINPROGRESS: return 115;
        case ECANCELED: return 125; default: return 5;
    }
}
static int net_host_result(int result, int err) { return result < 0 ? net_error(net_errno(err)) : result; }
static uint64_t net_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void net_trace(const char *op, int fd, long result) {
    const char *s = getenv("DM_NET_TRACE");
    if (s && atoi(s)) fprintf(stderr, "net: %s guest_fd=%d result=%ld errno=%d\n", op, fd, result, result < 0 ? t_net_errno : 0);
}
// Each operation borrows a duplicate while under the lock. Close removes the guest slot first;
// a generation check in every bounded wait prevents an old waiter touching a subsequently reused slot.
static int net_borrow(int guest, net_ref *r) {
    if (!net_gate()) return -1;
    pthread_mutex_lock(&g_net_lock);
    if (guest < 0 || guest >= NET_MAX || !g_sockets[guest].used) { pthread_mutex_unlock(&g_net_lock); return net_error(9); }
    net_slot s = g_sockets[guest];
    int fd = fcntl(s.native, F_DUPFD_CLOEXEC, 0), err = errno;
    pthread_mutex_unlock(&g_net_lock);
    if (fd < 0) return net_error(net_errno(err));
    *r = (net_ref){fd, guest, s.nonblock, s.type, s.generation, s.send_ms, s.recv_ms}; return 0;
}
int dm_net_dup_native(int guest) {
    return dm_net_dup_native_generation(guest, NULL);
}
int dm_net_dup_native_generation(int guest, uint64_t *generation) {
    net_ref r;
    if (net_borrow(guest, &r)) return -1;
    if (generation) *generation = r.generation;
    return r.fd;
}
static int net_live(const net_ref *r) {
    pthread_mutex_lock(&g_net_lock);
    int live = g_sockets[r->guest].used && g_sockets[r->guest].generation == r->generation;
    pthread_mutex_unlock(&g_net_lock); return live;
}
#include "net_discovery_queue.inc"
static int net_register(int fd, int type, int nonblock) {
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { int e = errno; close(fd); return net_error(net_errno(e)); }
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) { int e = errno; close(fd); return net_error(net_errno(e)); }
    pthread_mutex_lock(&g_net_lock);
    int i; for (i = 0; i < NET_MAX && g_sockets[i].used; i++) {}
    if (i < NET_MAX) { uint64_t gen = g_sockets[i].generation + 1; g_sockets[i] = (net_slot){fd, 1, nonblock, type, gen, -1, -1}; }
    pthread_mutex_unlock(&g_net_lock);
    if (i == NET_MAX) { close(fd); return net_error(24); }
    return i;
}
static int net_addr_in(const void *p, unsigned n, struct sockaddr_in *out) {
    if (!p) return net_error(14);
    if (n < sizeof(nx_addr)) return net_error(22);
    nx_addr a; memcpy(&a, p, sizeof a);
    if (a.family != 2) return net_error(97);
    if (a.len && a.len != sizeof a) return net_error(22);
    memset(out, 0, sizeof *out); out->sin_len = sizeof *out; out->sin_family = AF_INET;
    out->sin_port = a.port; out->sin_addr.s_addr = a.address; return 0;
}
static int net_addr_output_ok(void *p, unsigned *n) { return p && !n ? net_error(14) : 0; }
static void net_addr_out(void *p, unsigned *n, const struct sockaddr_in *in) {
    if (!p || !n) return;
    nx_addr a = {.len = sizeof a, .family = 2, .port = in->sin_port, .address = in->sin_addr.s_addr};
    unsigned count = *n < sizeof a ? *n : sizeof a; memcpy(p, &a, count); *n = sizeof a;
}
static int net_wait(net_ref *r, short events, int64_t timeout, uint64_t start) {
    for (;;) {
        if (!net_live(r)) return net_error(9);
        int slice = NET_SLICE_MS;
        if (timeout >= 0) { uint64_t elapsed = net_ms() - start; if (elapsed >= (uint64_t)timeout) return net_error(11); if ((uint64_t)slice > (uint64_t)timeout - elapsed) slice = (int)((uint64_t)timeout - elapsed); }
        struct pollfd p = {.fd = r->fd, .events = events};
        dm_guest_host_wait_begin(); int count = poll(&p, 1, slice), err = errno; dm_guest_host_wait_end();
        if (count < 0 && err == EINTR) continue;
        if (count < 0) return net_error(net_errno(err));
        if (!net_live(r)) return net_error(9);
        if (count) return 0;
    }
}
int dm_net_socket(int family, int type, int protocol) {
    if (!net_gate()) return -1;
    if (family != 2) return net_error(97);
    unsigned flags = (unsigned)type & ~0xfu; int base = type & 0xf;
    if (flags & ~0x20000000u) return net_error(22);
    if (base != 1 && base != 2) return net_error(94);
    if (protocol && protocol != (base == 1 ? 6 : 17)) return net_error(93);
    int fd = socket(AF_INET, base == 1 ? SOCK_STREAM : SOCK_DGRAM, protocol), err = errno;
    int result = fd < 0 ? net_error(net_errno(err)) : net_register(fd, base, !!flags);
    net_trace("socket", result, result); return result;
}
static int net_close_owned(int guest, int check_generation, uint64_t generation) {
    pthread_mutex_lock(&g_net_lock);
    if (guest < 0 || guest >= NET_MAX || !g_sockets[guest].used ||
        (check_generation && g_sockets[guest].generation != generation)) {
        pthread_mutex_unlock(&g_net_lock); return net_error(9);
    }
    int fd = g_sockets[guest].native; uint64_t old_generation = g_sockets[guest].generation; g_sockets[guest].used = 0;
    pthread_mutex_unlock(&g_net_lock);
    net_discovery_cancel(guest, old_generation);
    shutdown(fd, SHUT_RDWR); int result = close(fd), err = errno;
    return net_host_result(result, err);
}
int dm_net_close(int guest) { return net_gate() ? net_close_owned(guest, 0, 0) : -1; }
int dm_net_close_generation(int guest, uint64_t generation) { return net_close_owned(guest, 1, generation); }
void dm_net_finalize(void) { for (int i = 0; i < NET_MAX; i++) { pthread_mutex_lock(&g_net_lock); int used = g_sockets[i].used; pthread_mutex_unlock(&g_net_lock); if (used) net_close_owned(i, 0, 0); } }
int dm_net_bind(int guest, const void *addr, unsigned len) {
    if (!net_gate()) return -1;
    struct sockaddr_in a; if (net_addr_in(addr, len, &a)) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    int result = bind(r.fd, (struct sockaddr *)&a, sizeof a), err = errno; close(r.fd);
    result = net_host_result(result, err); net_trace("bind", guest, result); return result;
}
int dm_net_connect(int guest, const void *addr, unsigned len) {
    if (!net_gate()) return -1;
    struct sockaddr_in a; if (net_addr_in(addr, len, &a)) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    if (net_is_discovery(a.sin_addr.s_addr, r.type) && dm_net_discovery_target(a.sin_addr.s_addr, &a.sin_addr.s_addr)) { close(r.fd); return -1; }
    uint64_t start = net_ms(); int result = connect(r.fd, (struct sockaddr *)&a, sizeof a), err = errno;
    if (result < 0 && err == EINPROGRESS && !r.nonblock) {
        result = net_wait(&r, POLLOUT, r.send_ms, start);
        if (!result) { socklen_t n = sizeof err; if (getsockopt(r.fd, SOL_SOCKET, SO_ERROR, &err, &n) < 0) err = errno; result = err ? -1 : 0; if (result) net_error(net_errno(err)); }
    } else result = net_host_result(result, err);
    close(r.fd); net_trace("connect", guest, result); return result;
}
int dm_net_listen(int guest, int backlog) { net_ref r; if (net_borrow(guest, &r)) return -1; int result = listen(r.fd, backlog), err = errno; close(r.fd); return net_host_result(result, err); }
int dm_net_accept(int guest, void *addr, unsigned *len) {
    if (!net_gate() || net_addr_output_ok(addr, len)) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    uint64_t start = net_ms(); int fd;
    struct sockaddr_in a;
    for (;;) {
        socklen_t n = sizeof a; fd = accept(r.fd, (struct sockaddr *)&a, &n); int err = errno;
        if (fd >= 0) break;
        if (err != EAGAIN || r.nonblock) { net_error(net_errno(err)); break; }
        if (net_wait(&r, POLLIN, r.recv_ms, start)) break;
    }
    close(r.fd);
    if (fd < 0) return -1;
    int result = net_register(fd, 1, r.nonblock);
    if (result >= 0) net_addr_out(addr, len, &a);
    return result;
}
static int net_name_impl(int guest, void *addr, unsigned *len, int peer) {
    if (!net_gate()) return -1;
    if (!addr || !len) return net_error(14);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    struct sockaddr_in a; socklen_t n = sizeof a;
    int result = peer ? getpeername(r.fd, (struct sockaddr *)&a, &n) : getsockname(r.fd, (struct sockaddr *)&a, &n), err = errno; close(r.fd);
    if (result >= 0) net_addr_out(addr, len, &a);
    return net_host_result(result, err);
}
int dm_net_shutdown(int guest, int how) {
    if (!net_gate()) return -1;
    if (how < 0 || how > 2) return net_error(22);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    int methods[] = {SHUT_RD, SHUT_WR, SHUT_RDWR}; int result = shutdown(r.fd, methods[how]), err = errno; close(r.fd); return net_host_result(result, err);
}
static int net_fcntl_impl(int guest, int cmd, int flags) {
    net_ref r; if (net_borrow(guest, &r)) return -1;
    close(r.fd);
    if (cmd == 3) return r.nonblock ? NX_NONBLOCK : 0;
    if (cmd != 4) return net_error(95);
    // F_SETFL does not change the descriptor's access mode. NEX supplies
    // O_RDWR | O_NONBLOCK (0x802); reject unsupported status flags, not the
    // read/write bits carried from a caller's file-status flags.
    if ((unsigned)flags & ~(NX_NONBLOCK | 3u)) return net_error(22);
    pthread_mutex_lock(&g_net_lock);
    int live = g_sockets[guest].used && g_sockets[guest].generation == r.generation;
    if (live) g_sockets[guest].nonblock = !!(flags & NX_NONBLOCK);
    pthread_mutex_unlock(&g_net_lock); return live ? 0 : net_error(9);
}
static int net_message_flags(int flags, int sending) {
    if ((unsigned)flags & ~(1u | 2u | 4u | 0x40u | 0x80u)) return net_error(95);
    if (sending && (flags & (2 | 0x40))) return net_error(95);
    int out = 0;
    if (flags & 1) out |= MSG_OOB; if (flags & 2) out |= MSG_PEEK; if (flags & 4) out |= MSG_DONTROUTE;
    // Native sockets always remain nonblocking. Guest blocking and WAITALL are implemented around readiness waits.
    return out;
}
long dm_net_recv(int guest, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen) {
    if (!net_gate()) return -1;
    if ((!buf && len) || net_addr_output_ok(addr, addrlen)) return net_error(14);
    if (len > SSIZE_MAX) return net_error(22);
    int hostflags = net_message_flags(flags, 0); if (hostflags < 0) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    if ((flags & 0x40) && (flags & 2)) { close(r.fd); return net_error(95); } // PEEK+WAITALL would need a separate buffered transport.
    uint64_t start = net_ms(); long result = -1; size_t total = 0;
    struct sockaddr_in a;
    for (;;) {
        // Kernel copyout cannot fault into the dirty-page signal hook. A wait or
        // partial stream read may allow the renderer to protect this span again.
        dm_guest_write_prepare(buf ? (char *)buf + total : NULL, len - total);
        socklen_t n = sizeof a; ssize_t got = recvfrom(r.fd, buf ? (char *)buf + total : NULL, len - total, hostflags, addr ? (struct sockaddr *)&a : NULL, addr ? &n : NULL); int err = errno;
        if (got >= 0) {
            total += (size_t)got; result = (long)total;
            if (addr) net_addr_out(addr, addrlen, &a);
            if (!got || r.type != 1 || !(flags & 0x40) || r.nonblock || (flags & 0x80) || total == len) break;
            continue;
        }
        if (err != EAGAIN || r.nonblock || (flags & 0x80)) { if (!total) result = net_error(net_errno(err)); break; }
        if (net_wait(&r, POLLIN, r.recv_ms, start)) { if (!total) result = -1; break; }
    }
    close(r.fd); net_trace("recv", guest, result); return result;
}
long dm_net_send(int guest, const void *buf, unsigned long len, int flags, const void *addr, unsigned addrlen) {
    if (!net_gate()) return -1;
    if (!buf && len) return net_error(14);
    if (len > SSIZE_MAX) return net_error(22);
    struct sockaddr_in a; if (addr && net_addr_in(addr, addrlen, &a)) return -1;
    int hostflags = net_message_flags(flags, 1); if (hostflags < 0) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    int discovery=addr&&net_is_discovery(a.sin_addr.s_addr,r.type);
    if (discovery) {
        dm_net_discovery_plan plan;
        if (dm_net_discovery_prepare(a.sin_addr.s_addr, &plan)) { close(r.fd); return -1; }
        if (plan.automatic) {
            long result = net_discovery_enqueue(&r, plan, a, buf, len, hostflags);
            close(r.fd); net_trace("send-discovery", guest, result); return result;
        }
        a.sin_addr.s_addr = plan.target;
    }
    uint64_t start = net_ms(); long result;
    for (;;) {
        result = addr ? net_discovery_packet(r.fd,buf,len,hostflags,&a) : sendto(r.fd,buf,len,hostflags,NULL,0); int err = errno;
        if (result >= 0) break;
        if (err != EAGAIN || r.nonblock || (flags & 0x80)) { result = net_error(net_errno(err)); break; }
        if (net_wait(&r, POLLOUT, r.send_ms, start)) { result = -1; break; }
    }
    if(result>=0&&discovery&&dm_discovery_query_protocol(ntohs(a.sin_port),buf,len)) {
        dm_net_discovery_plan extra;int saved=t_net_errno;
        if(dm_net_discovery_supplement(a.sin_addr.s_addr,&extra)) {
            long queued=net_discovery_enqueue(&r,extra,a,buf,len,hostflags);
            net_trace("send-discovery-extra",guest,queued);
        }
        // The original native send owns the result. Queue pressure or an
        // unavailable bounded subnet must not turn its success into failure.
        t_net_errno=saved;
    }
    close(r.fd); net_trace("send", guest, result); return result;
}

static int net_option(int level, int option, int *hostlevel, int *hostoption, unsigned *size) {
    *size = sizeof(int);
    if (level == 0xffff) {
        *hostlevel = SOL_SOCKET;
        switch (option) {
            case 1: *hostoption = SO_DEBUG; break; case 2: *hostoption = SO_ACCEPTCONN; break;
            case 4: *hostoption = SO_REUSEADDR; break; case 8: *hostoption = SO_KEEPALIVE; break;
            case 16: *hostoption = SO_DONTROUTE; break; case 32: *hostoption = SO_BROADCAST; break;
            case 128: *hostoption = SO_LINGER; *size = sizeof(struct linger); break;
            case 256: *hostoption = SO_OOBINLINE; break; case 512: *hostoption = SO_REUSEPORT; break;
            case 0x800: *hostoption = SO_NOSIGPIPE; break;
            case 0x1001: *hostoption = SO_SNDBUF; break; case 0x1002: *hostoption = SO_RCVBUF; break;
            case 0x1003: *hostoption = SO_SNDLOWAT; break; case 0x1004: *hostoption = SO_RCVLOWAT; break;
            case 0x1007: *hostoption = SO_ERROR; break; case 0x1008: *hostoption = SO_TYPE; break;
            default: return net_error(92);
        }
    } else if (level == 6) {
        *hostlevel = IPPROTO_TCP;
        if (option == 1) *hostoption = TCP_NODELAY;
        else if (option == 2) *hostoption = TCP_MAXSEG;
        else return net_error(92);
    } else if (level == 0) {
        *hostlevel = IPPROTO_IP;
        switch (option) {
            case 3: *hostoption = IP_TOS; break; case 4: *hostoption = IP_TTL; break;
            case 9: *hostoption = IP_MULTICAST_IF; *size = 4; break;
            case 10: *hostoption = IP_MULTICAST_TTL; *size = 1; break;
            case 11: *hostoption = IP_MULTICAST_LOOP; *size = 1; break;
            case 12: *hostoption = IP_ADD_MEMBERSHIP; *size = 8; break;
            case 13: *hostoption = IP_DROP_MEMBERSHIP; *size = 8; break;
            default: return net_error(92);
        }
    } else return net_error(92);
    return 0;
}
static int net_sockopt_impl(int guest, int level, int option, void *value, unsigned *len, int set) {
    if (!net_gate()) return -1;
    if (!value || !len) return net_error(14);
    if (set && level == 0 && option == 12 && !dm_net_multicast_allowed()) return net_error(13);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    if (level == 0xffff && (option == 0x1005 || option == 0x1006)) {
        // Horizon's service uses a u32 millisecond value. Also accept the SDK's 64-bit TimeVal form, translating rather than passing it to Darwin.
        int64_t ms;
        if (set) {
            if (*len == 4) { uint32_t v; memcpy(&v, value, 4); ms = v ? (int64_t)v : -1; }
            else if (*len == sizeof(nx_timeval)) { nx_timeval v; memcpy(&v, value, sizeof v); if (v.sec < 0 || v.usec < 0 || v.usec >= 1000000 || v.sec > INT_MAX / 1000) { close(r.fd); return net_error(22); } ms = v.sec * 1000 + (v.usec + 999) / 1000; if (!ms) ms = -1; }
            else { close(r.fd); return net_error(22); }
            pthread_mutex_lock(&g_net_lock);
            int live = g_sockets[guest].used && g_sockets[guest].generation == r.generation;
            if (live) { if (option == 0x1005) g_sockets[guest].send_ms = ms; else g_sockets[guest].recv_ms = ms; }
            pthread_mutex_unlock(&g_net_lock); close(r.fd); return live ? 0 : net_error(9);
        }
        ms = option == 0x1005 ? r.send_ms : r.recv_ms; if (ms < 0) ms = 0;
        if (*len == sizeof(nx_timeval)) { nx_timeval v = {ms / 1000, ms % 1000 * 1000}; memcpy(value, &v, sizeof v); }
        else if (*len >= 4) { uint32_t v = (uint32_t)ms; memcpy(value, &v, 4); *len = 4; }
        else { close(r.fd); return net_error(22); }
        close(r.fd); return 0;
    }
    int hl, ho; unsigned size;
    if (net_option(level, option, &hl, &ho, &size)) { close(r.fd); return -1; }
    if (set && level == 0xffff && (option == 2 || option == 0x1007 || option == 0x1008)) { close(r.fd); return net_error(92); }
    // SO_NOSIGPIPE is required internally to preserve the guest's EPIPE convention. Guest setting it to zero cannot re-enable a process-killing host signal.
    int one = 1; void *hostvalue = level == 0xffff && option == 0x800 ? &one : value;
    if (*len < size || (set && *len != size)) { close(r.fd); return net_error(22); }
    if (set && level == 0xffff && option == 128) {
        int v[2]; memcpy(v, value, sizeof v);
        if ((v[0] != 0 && v[0] != 1) || v[1] < 0) { close(r.fd); return net_error(22); }
    }
    socklen_t n = size;
    // Kernel copyout does not invoke the tracked guest-page write-fault hook.
    if (!set) dm_guest_write_prepare(value, n);
    int result = set ? setsockopt(r.fd, hl, ho, hostvalue, n) : getsockopt(r.fd, hl, ho, value, &n), err = errno;
    if (!set && result >= 0) {
        *len = n;
        if (level == 0xffff && option == 0x1007 && n == 4) { int e; memcpy(&e, value, 4); e = net_errno(e); memcpy(value, &e, 4); }
    }
    close(r.fd); return net_host_result(result, err);
}
static short net_poll_events(uint16_t events) {
    short host = 0;
    if (events & 1) host |= POLLIN; if (events & 2) host |= POLLPRI; if (events & 4) host |= POLLOUT;
    if (events & 64) host |= POLLRDNORM; if (events & 128) host |= POLLRDBAND; if (events & 256) host |= POLLWRBAND;
    return host;
}
static int16_t net_poll_revents(short events) {
    int16_t out = 0;
    if (events & POLLIN) out |= 1; if (events & POLLPRI) out |= 2; if (events & POLLOUT) out |= 4;
    if (events & POLLERR) out |= 8; if (events & POLLHUP) out |= 16; if (events & POLLNVAL) out |= 32;
    if (events & POLLRDNORM) out |= 64; if (events & POLLRDBAND) out |= 128; if (events & POLLWRBAND) out |= 256;
    return out;
}
int dm_net_idle(void) {
    for (;;) {
        dm_guest_host_wait_begin(); int result = poll(NULL, 0, -1), err = errno; dm_guest_host_wait_end();
        if (result < 0 && err == EINTR) continue; // Host pause/lifecycle signals are not guest-visible interrupts.
        return net_host_result(result, err);
    }
}
int dm_net_poll(void *fds, uint64_t count, int timeout_ms) {
    if (!net_gate()) return -1;
    if (count > NET_MAX) return net_error(22);
    if (count && !fds) return net_error(14);
    struct pollfd *host = calloc(count ? (size_t)count : 1, sizeof *host);
    net_ref *refs = calloc(count ? (size_t)count : 1, sizeof *refs);
    dm_net_pollfd *guest = fds;
    if (!host || !refs) { free(host); free(refs); return net_error(12); }
    int result = -1, invalid = 0;
    for (uint64_t i = 0; i < count; i++) refs[i].fd = -1;
    for (uint64_t i = 0; i < count; i++) {
        refs[i].fd = host[i].fd = -1; guest[i].revents = 0;
        if (guest[i].fd < 0) continue;
        if ((uint16_t)guest[i].events & ~0x1ffu) { net_error(22); goto done; }
        if (net_borrow(guest[i].fd, &refs[i])) { if (t_net_errno != 9) goto done; guest[i].revents = 32; invalid = 1; }
        else { host[i].fd = refs[i].fd; host[i].events = net_poll_events((uint16_t)guest[i].events); }
    }
    uint64_t start = net_ms();
    for (;;) {
        int wait = invalid ? 0 : NET_SLICE_MS;
        if (timeout_ms >= 0) { uint64_t elapsed = net_ms() - start; uint64_t left = elapsed < (uint64_t)timeout_ms ? (uint64_t)timeout_ms - elapsed : 0; if ((uint64_t)wait > left) wait = (int)left; }
        dm_guest_host_wait_begin(); int ready = poll(host, (nfds_t)count, wait), err = errno; dm_guest_host_wait_end();
        if (ready < 0 && err == EINTR) continue;
        if (ready < 0) { net_error(net_errno(err)); break; }
        result = 0;
        for (uint64_t i = 0; i < count; i++) {
            if (refs[i].fd >= 0) guest[i].revents = net_live(&refs[i]) ? net_poll_revents(host[i].revents) : 32;
            if (guest[i].revents) result++;
        }
        if (result || (timeout_ms >= 0 && net_ms() - start >= (uint64_t)timeout_ms)) break;
    }
done:
    for (uint64_t i = 0; i < count; i++) if (refs[i].fd >= 0) close(refs[i].fd);
    free(host); free(refs); return result;
}
int dm_net_select(int nfds, void *read, void *write, void *except, const int64_t *timeout) {
    if (!net_gate()) return -1;
    if (nfds < 0 || nfds > NET_MAX) return net_error(22);
    int ms = -1;
    if (timeout) {
        int64_t sec = timeout[0], us = timeout[1];
        if (sec < 0 || us < 0 || us >= 1000000 || sec > INT_MAX / 1000) return net_error(22);
        int64_t total = sec * 1000 + (us + 999) / 1000; if (total > INT_MAX) return net_error(22); ms = (int)total;
    }
    dm_net_pollfd *fds = calloc(nfds ? (size_t)nfds : 1, sizeof *fds);
    if (!fds) return net_error(12);
    uint64_t *sets[] = {read, write, except};
    int count = 0;
    for (int i = 0; i < nfds; i++) {
        uint16_t ev = 0;
        for (int k = 0; k < 3; k++) if (sets[k] && ((sets[k][i >> 6] >> (i & 63)) & 1)) ev |= (uint16_t[]){1, 4, 2}[k];
        if (ev) fds[count++] = (dm_net_pollfd){i, (int16_t)ev, 0};
    }
    int result = dm_net_poll(fds, (uint64_t)count, ms);
    if (result >= 0) for (int i = 0; i < count; i++) if (fds[i].revents & 32) { result = net_error(9); break; }
    if (result >= 0) {
        for (int k = 0; k < 3; k++) if (sets[k]) memset(sets[k], 0, NET_MAX / 8);
        result = 0;
        for (int i = 0; i < count; i++) for (int k = 0; k < 3; k++) if (sets[k] && (fds[i].events & (int[]){1, 4, 2}[k]) && (fds[i].revents & (int[]){1 | 8 | 16, 4 | 8 | 16, 2}[k])) { sets[k][fds[i].fd >> 6] |= 1ull << (fds[i].fd & 63); result++; }
    }
    free(fds); return result;
}

typedef struct net_ai_allocation { struct net_ai_allocation *next; dm_net_addrinfo *root; void **owned; size_t count; } net_ai_allocation;
static net_ai_allocation *g_ai;
static int net_gai_error(int e) {
    switch (e) {
        case 0: return 0; case EAI_AGAIN: return 2; case EAI_BADFLAGS: return 3; case EAI_FAIL: return 4;
        case EAI_FAMILY: return 5; case EAI_MEMORY: return 6; case EAI_NODATA: return 7; case EAI_NONAME: return 8;
        case EAI_SERVICE: return 9; case EAI_SOCKTYPE: return 10; case EAI_SYSTEM: return 11; case EAI_OVERFLOW: return 14;
        default: return 4;
    }
}
static void net_ai_destroy(net_ai_allocation *p) { if (p) { for (size_t i = 0; i < p->count; i++) free(p->owned[i]); free(p->owned); free(p); } }
static int net_getaddrinfo(const char *host, const char *service, const void *hints, void **result, int legacy) {
    if (!result) { net_error(14); return 11; }
    *result = NULL;
    if (!net_gate()) return 4;
    struct addrinfo h = {.ai_family = AF_INET}, *native = NULL;
    dm_net_addrinfo input = {0}; if (hints) memcpy(&input, hints, sizeof input);
    if (input.family != 0 && input.family != 2) return 5;
    if (input.type && input.type != 1 && input.type != 2) return 10;
    if (input.protocol && input.protocol != 6 && input.protocol != 17) return 13;
    if ((unsigned)input.flags & ~(1u | 2u | 4u | 8u | 1024u)) return 3;
    h.ai_socktype = input.type == 1 ? SOCK_STREAM : input.type == 2 ? SOCK_DGRAM : 0; h.ai_protocol = input.protocol;
    if (input.flags & 1) h.ai_flags |= AI_PASSIVE; if (input.flags & 2) h.ai_flags |= AI_CANONNAME;
    if (input.flags & 4) h.ai_flags |= AI_NUMERICHOST; if (input.flags & 8) h.ai_flags |= AI_NUMERICSERV;
    if (input.flags & 1024) h.ai_flags |= AI_ADDRCONFIG;
    dm_guest_host_wait_begin(); int code = getaddrinfo(host, service, &h, &native), err = errno; dm_guest_host_wait_end();
    if (code) { if (code == EAI_SYSTEM) net_error(net_errno(err)); return net_gai_error(code); }
    net_ai_allocation *allocation = calloc(1, sizeof *allocation);
    if (!allocation) { freeaddrinfo(native); return 6; }
    size_t capacity = 0;
    for (struct addrinfo *p = native; p; p = p->ai_next) if (++capacity > NET_MAX) { net_ai_destroy(allocation); freeaddrinfo(native); return 6; }
    allocation->owned = calloc(capacity ? capacity : 1, sizeof(void *));
    if (!allocation->owned) { net_ai_destroy(allocation); freeaddrinfo(native); return 6; }
    dm_net_addrinfo *first = NULL, **tail = &first;
    for (struct addrinfo *p = native; p; p = p->ai_next) {
        if (p->ai_family != AF_INET || p->ai_addrlen < sizeof(struct sockaddr_in)) continue;
        const char *canonical = p->ai_canonname;
        size_t name_size = canonical ? strlen(canonical) + 1 : 0;
        dm_net_addrinfo *node = calloc(1, sizeof *node + sizeof(nx_addr) + name_size);
        if (!node) { net_ai_destroy(allocation); freeaddrinfo(native); return 6; }
        allocation->owned[allocation->count++] = node;
        nx_addr *address = (nx_addr *)(node + 1);
        node->flags = input.flags; node->family = 2; node->type = p->ai_socktype == SOCK_STREAM ? 1 : 2; node->protocol = p->ai_protocol;
        node->addrlen = sizeof *address; node->addr = address;
        struct sockaddr_in *a = (struct sockaddr_in *)p->ai_addr; unsigned n = sizeof *address; net_addr_out(address, &n, a);
        if (name_size) { node->canonname = (char *)(address + 1); memcpy(node->canonname, canonical, name_size); }
        if (legacy) { void *swap = node->addr; node->addr = node->canonname; node->canonname = swap; }
        *tail = node; tail = (dm_net_addrinfo **)&node->next;
    }
    freeaddrinfo(native);
    if (!first) { net_ai_destroy(allocation); return 7; }
    allocation->root = first; pthread_mutex_lock(&g_net_lock); allocation->next = g_ai; g_ai = allocation; pthread_mutex_unlock(&g_net_lock);
    *result = first; return 0;
}
int dm_net_getaddrinfo(const char *host, const char *service, const void *hints, void **result) { return net_getaddrinfo(host, service, hints, result, 0); }
int dm_net_getaddrinfo_legacy(const char *host, const char *service, const void *hints, void **result) { return net_getaddrinfo(host, service, hints, result, 1); }
void dm_net_freeaddrinfo(void *result) {
    if (!result) return;
    pthread_mutex_lock(&g_net_lock); net_ai_allocation **link = &g_ai; while (*link && (*link)->root != result) link = &(*link)->next;
    net_ai_allocation *found = *link; if (found) *link = found->next; pthread_mutex_unlock(&g_net_lock);
    if (found) net_ai_destroy(found); else if (dm_net_enabled()) net_error(22);
}
void *dm_net_gethostbyname(const char *name) {
    typedef struct { char *name; char **aliases; int32_t family, length; char **addresses; } nx_hostent;
    static __thread struct { nx_hostent entry; char name[NI_MAXHOST]; char *aliases[1], *addresses[65]; uint32_t data[64]; } data;
    if (!name) { net_error(14); return NULL; }
    void *out = NULL; dm_net_addrinfo hints = {.family = 2, .type = 1};
    if (dm_net_getaddrinfo(name, NULL, &hints, &out)) return NULL;
    memset(&data, 0, sizeof data); snprintf(data.name, sizeof data.name, "%s", name);
    int n = 0;
    for (dm_net_addrinfo *p = out; p && n < 64; p = p->next) { data.data[n] = ((nx_addr *)p->addr)->address; data.addresses[n] = (char *)&data.data[n]; n++; }
    data.entry = (nx_hostent){data.name, data.aliases, 2, 4, data.addresses}; dm_net_freeaddrinfo(out); return &data.entry;
}

// Query the actual IPv4 default route, then find that interface's address. DM_NETWORK_INTERFACE
// can choose another up IPv4 interface (including lo0 for deterministic transport tests).
static unsigned net_route(unsigned wanted, uint32_t *gateway) {
#if TARGET_OS_OSX
    int mib[] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0}; size_t size = 0;
    if (sysctl(mib, 6, NULL, &size, NULL, 0) || !size) return 0;
    char *buf = malloc(size); if (!buf) return 0;
    if (sysctl(mib, 6, buf, &size, NULL, 0)) { free(buf); return 0; }
    unsigned found = 0;
    for (size_t pos = 0; pos + sizeof(struct rt_msghdr) <= size;) {
        struct rt_msghdr *r = (struct rt_msghdr *)(buf + pos); size_t end = pos + r->rtm_msglen;
        if (r->rtm_msglen < sizeof *r || end > size) break;
        if ((r->rtm_flags & (RTF_UP | RTF_GATEWAY)) == (RTF_UP | RTF_GATEWAY) && !(r->rtm_flags & RTF_IFSCOPE) && (!wanted || wanted == r->rtm_index)) {
            struct sockaddr *a[RTAX_MAX] = {0}; size_t next = pos + sizeof *r;
            for (int i = 0; i < RTAX_MAX; i++) if (r->rtm_addrs & (1 << i)) {
                if (next + 2 > end) break; struct sockaddr *s = (struct sockaddr *)(buf + next);
                size_t n = s->sa_len ? (s->sa_len + sizeof(long) - 1) & ~(sizeof(long) - 1) : sizeof(long);
                if (n > end - next) break; a[i] = s; next += n;
            }
            if (a[RTAX_DST] && a[RTAX_DST]->sa_family == AF_INET && a[RTAX_DST]->sa_len >= sizeof(struct sockaddr_in) &&
                ((struct sockaddr_in *)a[RTAX_DST])->sin_addr.s_addr == 0 && a[RTAX_GATEWAY] && a[RTAX_GATEWAY]->sa_family == AF_INET && a[RTAX_GATEWAY]->sa_len >= sizeof(struct sockaddr_in)) {
                found = r->rtm_index; *gateway = ((struct sockaddr_in *)a[RTAX_GATEWAY])->sin_addr.s_addr; break;
            }
        }
        pos = end;
    }
    free(buf); return found;
#else
    (void)wanted; (void)gateway; return 0;
#endif
}
static int net_interface_get(net_interface *out) {
    memset(out, 0, sizeof *out);
#ifdef DM_NET_TEST
    if (g_test_interface_set) { *out = g_test_interface; return out->address ? 0 : -1; }
#endif
    const char *name = getenv("DM_NETWORK_INTERFACE"); unsigned wanted = name && *name ? if_nametoindex(name) : 0;
    if (name && *name && !wanted) return -1;
    unsigned primary = net_route(wanted, &out->gateway); if (wanted) primary = wanted;
    struct ifaddrs *list = NULL; if (getifaddrs(&list)) return -1;
    int control = socket(AF_INET, SOCK_DGRAM, 0);
#if TARGET_OS_IOS
    // getifaddrs ordering is not the preferred route on iOS. Automatic mode
    // must not select a cellular/VPN entry before an available LAN adapter,
    // then reject that same entry when preparing local discovery. Preserve a
    // non-local fallback for online use when no eligible LAN is available.
    dm_net_mode_t mode = dm_net_mode();
    for (int pass = 0; pass < (mode == DM_NET_AUTO ? 2 : 1) && !out->address; pass++) {
#endif
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP) || (!wanted && (p->ifa_flags & IFF_LOOPBACK))) continue;
        unsigned index = if_nametoindex(p->ifa_name);
        if (!index || (primary && index != primary)) continue;
        struct ifreq request = {0}; strlcpy(request.ifr_name, p->ifa_name, sizeof request.ifr_name);
        unsigned type = control >= 0 && !ioctl(control, SIOCGIFFUNCTIONALTYPE, &request) ?
                        request.ifr_ifru.ifru_functional_type : IFRTYPE_FUNCTIONAL_UNKNOWN;
        int local_link = type == IFRTYPE_FUNCTIONAL_WIFI_INFRA || type == IFRTYPE_FUNCTIONAL_WIRED;
#if TARGET_OS_IOS
        // TN3179: BSD names such as en0 are not API. Ask the functional type.
        if ((mode == DM_NET_LAN || mode == DM_NET_LDN || (mode == DM_NET_AUTO && pass == 0)) &&
            (!local_link || !(p->ifa_flags & IFF_BROADCAST) || (p->ifa_flags & IFF_POINTOPOINT))) continue;
#endif
        out->address = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
        if (p->ifa_netmask && p->ifa_netmask->sa_family == AF_INET)
            out->mask = ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr;
        out->index = index; out->flags = p->ifa_flags; out->local_link = local_link; break;
    }
#if TARGET_OS_IOS
    }
#endif
    if (control >= 0) close(control);
    freeifaddrs(list);
    return out->address ? 0 : -1;
}
int dm_net_ipconfig(uint32_t *address, uint32_t *mask, uint32_t *gateway, uint32_t *dns1, uint32_t *dns2) {
    uint32_t values[5] = {0}; uint32_t *outputs[] = {address, mask, gateway, dns1, dns2};
    for (int i = 0; i < 5; i++) if (outputs[i]) *outputs[i] = 0;
    if (!dm_net_enabled()) return -1;
    net_interface adapter;
    if (net_interface_get(&adapter)) return -1;
    values[0] = adapter.address; values[1] = adapter.mask; values[2] = adapter.gateway;
    atomic_store(&g_net_broadcast, adapter.mask && (adapter.flags & IFF_BROADCAST) ? adapter.address | ~adapter.mask : 0);
    atomic_store(&g_net_broadcast_known, 1);
    if (!values[0]) return -1;
    // DNS is advisory for LAN and never invented. macOS publishes the default resolver here;
    // getaddrinfo itself continues to use the system resolver (including scoped DNS).
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) { char line[512], ip[64]; int n = 3; while (n < 5 && fgets(line, sizeof line, f)) if (sscanf(line, "nameserver %63s", ip) == 1 && inet_pton(AF_INET, ip, &values[n]) == 1) n++; fclose(f); }
    for (int i = 0; i < 5; i++) if (outputs[i]) *outputs[i] = values[i]; return 0;
}

// Error-only diagnostics for control operations. Record numeric ABI metadata,
// never option contents, socket addresses or application data.
static void net_control_trace(const char *operation, int guest, int result, int a, int b) {
    int saved = errno; const char *flag = getenv("DM_NET_TRACE");
    if (result < 0 && flag && atoi(flag))
        fprintf(stderr,"net: %s guest_fd=%d result=%d errno=%d arg0=%d arg1=%d\n", operation, guest, result, t_net_errno, a, b);
    errno = saved;
}
int dm_net_name(int guest, void *addr, unsigned *len, int peer) {
    int result = net_name_impl(guest, addr, len, peer);
    net_control_trace(peer ? "getpeername" : "getsockname", guest, result, 0, 0);
    return result;
}
int dm_net_fcntl(int guest, int command, int flags) {
    int result = net_fcntl_impl(guest, command, flags);
    net_control_trace("fcntl", guest, result, command, flags);
    return result;
}
int dm_net_sockopt(int guest, int level, int option, void *value, unsigned *len, int set) {
    int result = net_sockopt_impl(guest, level, option, value, len, set);
    net_control_trace(set ? "setsockopt" : "getsockopt", guest, result, level, option);
    return result;
}
