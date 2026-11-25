// Network-mode nn::socket transport: the guest's BSD socket calls on real Darwin IPv4 TCP/UDP sockets (net_socket.h). Off by default; DM_NETWORK=lan turns it on.
// ABI references: Atmosphere-NX socket_types.hpp / socket_errno.hpp; switchbrew/libnx socket.c. Horizon uses the BSD sockaddr (u8 length, u8 family), Linux errno numbers
// (nn::socket::Errno: EAGAIN 11, as Mario Kart 8 Deluxe's receive loop compares it), O_NONBLOCK 0x800, SOCK_NONBLOCK 0x20000000 and FreeBSD option numbers, some of which Darwin
// numbers differently (SO_NOSIGPIPE is 0x800 on Horizon, 0x1022 here). Guest structures are always copied field by field, never cast to Darwin's.
// Name resolution, TLS, LDN and the online services are not part of this transport: they keep hle_net.c's / hle_offline.c's offline answers.
#include "hle.h"
#include "hle_net.h"
#include "net_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// A blocking call waits in slices of this length, so a Close on another thread ends it with EBADF within one slice even when the socket gives no wakeup (UDP)
enum { NET_SLOTS = DM_NET_FD_END - DM_NET_FD_BASE, NX_NONBLOCK = 0x800, NX_SOCK_NONBLOCK = 0x20000000, NX_SOCK_CLOEXEC = 0x10000000, NET_SLICE_MS = 100 };
enum { NX_MSG_OOB = 1, NX_MSG_PEEK = 2, NX_MSG_DONTROUTE = 4, NX_MSG_WAITALL = 0x40, NX_MSG_DONTWAIT = 0x80, NX_MSG_NOSIGNAL = 0x20000 };
typedef struct { uint8_t len, family; uint16_t port; uint32_t address; uint8_t zero[8]; } nx_addr;
typedef struct { int64_t sec, usec; } nx_timeval;
typedef struct { int32_t fd; int16_t events, revents; } nx_pollfd;
_Static_assert(sizeof(nx_addr) == 16, "Horizon sockaddr_in");
_Static_assert(sizeof(nx_pollfd) == 8, "Horizon pollfd");

// The native descriptor stays open while a guest slot is used. A call borrows a duplicate under the lock, so a Close on another thread never closes a descriptor that is in use,
// and the slot's generation tells a waiter that its socket was closed (or the slot reused) while it waited
typedef struct { int native, used, nonblock, type; uint64_t generation; int64_t send_ms, recv_ms; } net_slot;
typedef struct { int fd, slot, nonblock, type; uint64_t generation; int64_t send_ms, recv_ms; } net_ref;
static net_slot g_net[NET_SLOTS];
static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_net_on;
static pthread_once_t g_net_once = PTHREAD_ONCE_INIT;
static void net_read_mode(void) { const char *s = getenv("DM_NETWORK"); g_net_on = s && !strcmp(s, "lan"); }
int dm_net_enabled(void) { pthread_once(&g_net_once, net_read_mode); return g_net_on; }

static int net_error(int e) { dm_sk_set_errno(e); return -1; }
static int net_errno(int e) {   // Darwin's errno as Horizon's (Linux's) number
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
static uint64_t net_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }

static int net_borrow(int guest, net_ref *r) {
    if (!dm_net_owns(guest)) return net_error(9);
    int slot = guest - DM_NET_FD_BASE;
    dm_host_mutex_lock(&g_net_lock);
    if (!g_net[slot].used) { pthread_mutex_unlock(&g_net_lock); return net_error(9); }
    net_slot s = g_net[slot];
    int fd = fcntl(s.native, F_DUPFD_CLOEXEC, 0), err = errno;
    pthread_mutex_unlock(&g_net_lock);
    if (fd < 0) return net_error(net_errno(err));
    *r = (net_ref){fd, slot, s.nonblock, s.type, s.generation, s.send_ms, s.recv_ms}; return 0;
}
static int net_live(const net_ref *r) {
    dm_host_mutex_lock(&g_net_lock);
    int live = g_net[r->slot].used && g_net[r->slot].generation == r->generation;
    pthread_mutex_unlock(&g_net_lock); return live;
}
// The native descriptor is always non-blocking: guest blocking calls wait here, in slices, with the guest core released (dm_thread_native_wait), as main's offline waits do.
// Every socket gets SO_NOSIGPIPE: Darwin has no MSG_NOSIGNAL, and a send to a peer that closed would otherwise raise SIGPIPE and end the process instead of returning EPIPE
static int net_register(int fd, int type, int nonblock) {
    int one = 1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) {
        int e = errno; close(fd); return net_error(net_errno(e));
    }
    dm_host_mutex_lock(&g_net_lock);
    int i = 0; while (i < NET_SLOTS && g_net[i].used) i++;
    if (i < NET_SLOTS) g_net[i] = (net_slot){fd, 1, nonblock, type, g_net[i].generation + 1, -1, -1};
    pthread_mutex_unlock(&g_net_lock);
    if (i == NET_SLOTS) { close(fd); return net_error(24); }   // EMFILE
    return DM_NET_FD_BASE + i;
}
static int net_wait(const net_ref *r, short events, int64_t timeout, uint64_t start) {
    for (;;) {
        if (!net_live(r)) return net_error(9);
        int slice = NET_SLICE_MS;
        if (timeout >= 0) {
            uint64_t elapsed = net_ms() - start;
            if (elapsed >= (uint64_t)timeout) return net_error(11);
            if ((uint64_t)slice > (uint64_t)timeout - elapsed) slice = (int)((uint64_t)timeout - elapsed);
        }
        struct pollfd p = {.fd = r->fd, .events = events};
        dm_thread_native_wait(1); int count = poll(&p, 1, slice), err = errno; dm_thread_native_wait(0);
        if (count < 0 && err == EINTR) continue;   // the host's pause and lifecycle signals are not the guest's interrupts
        if (count < 0) return net_error(net_errno(err));
        if (!net_live(r)) return net_error(9);
        if (count) return 0;
    }
}

static int net_addr_in(const void *p, unsigned n, struct sockaddr_in *out) {
    if (!p) return net_error(14);
    if (n < sizeof(nx_addr)) return net_error(22);
    nx_addr a; memcpy(&a, p, sizeof a);
    if (a.family != 2) return net_error(97);   // EAFNOSUPPORT
    if (a.len && a.len != sizeof a) return net_error(22);   // hand-written game code may leave sin_len zero
    memset(out, 0, sizeof *out); out->sin_len = sizeof *out; out->sin_family = AF_INET;
    out->sin_port = a.port; out->sin_addr.s_addr = a.address; return 0;
}
// Copies at most the caller's capacity and returns the full length, as BSD does for a short buffer
static void net_addr_out(void *p, unsigned *n, const struct sockaddr_in *in) {
    if (!p || !n) return;
    nx_addr a = {.len = sizeof a, .family = 2, .port = in->sin_port, .address = in->sin_addr.s_addr};
    unsigned count = *n < sizeof a ? *n : (unsigned)sizeof a; memcpy(p, &a, count); *n = sizeof a;
}

int dm_net_socket(int family, int type, int protocol) {
    if (family != 2) return net_error(97);
    unsigned flags = (unsigned)type & ~0xfu; int base = type & 0xf;
    if (flags & ~(unsigned)(NX_SOCK_NONBLOCK | NX_SOCK_CLOEXEC)) return net_error(22);
    if (base != 1 && base != 2) return net_error(94);   // ESOCKTNOSUPPORT: SOCK_STREAM and SOCK_DGRAM only
    if (protocol && protocol != (base == 1 ? 6 : 17)) return net_error(93);
    int fd = socket(AF_INET, base == 1 ? SOCK_STREAM : SOCK_DGRAM, protocol), err = errno;
    return fd < 0 ? net_error(net_errno(err)) : net_register(fd, base, !!(flags & NX_SOCK_NONBLOCK));
}
int dm_net_close(int guest) {
    if (!dm_net_owns(guest)) return net_error(9);
    int slot = guest - DM_NET_FD_BASE;
    dm_host_mutex_lock(&g_net_lock);
    if (!g_net[slot].used) { pthread_mutex_unlock(&g_net_lock); return net_error(9); }
    int fd = g_net[slot].native; g_net[slot].used = 0;
    pthread_mutex_unlock(&g_net_lock);
    // shutdown reaches the socket under every duplicate: a TCP waiter on another thread wakes now rather than at its next slice
    shutdown(fd, SHUT_RDWR); int result = close(fd), err = errno;
    return net_host_result(result, err);
}
int dm_net_bind(int guest, const void *addr, unsigned len) {
    struct sockaddr_in a; if (net_addr_in(addr, len, &a)) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    int result = bind(r.fd, (struct sockaddr *)&a, sizeof a), err = errno; close(r.fd);
    return net_host_result(result, err);
}
int dm_net_connect(int guest, const void *addr, unsigned len) {
    struct sockaddr_in a; if (net_addr_in(addr, len, &a)) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    uint64_t start = net_ms(); int result = connect(r.fd, (struct sockaddr *)&a, sizeof a), err = errno;
    if (result < 0 && err == EINPROGRESS && !r.nonblock) {   // a blocking connect: wait for the handshake, then report its own result
        result = net_wait(&r, POLLOUT, r.send_ms, start);
        if (!result) {
            socklen_t n = sizeof err; if (getsockopt(r.fd, SOL_SOCKET, SO_ERROR, &err, &n) < 0) err = errno;
            if (err) result = net_error(net_errno(err));
        }
    } else result = net_host_result(result, err);
    close(r.fd); return result;
}
int dm_net_listen(int guest, int backlog) {
    net_ref r; if (net_borrow(guest, &r)) return -1;
    int result = listen(r.fd, backlog), err = errno; close(r.fd); return net_host_result(result, err);
}
int dm_net_accept(int guest, void *addr, unsigned *len) {
    if (addr && !len) return net_error(14);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    uint64_t start = net_ms(); int fd; struct sockaddr_in a;
    for (;;) {
        socklen_t n = sizeof a; fd = accept(r.fd, (struct sockaddr *)&a, &n); int err = errno;
        if (fd >= 0) break;
        if (err != EAGAIN || r.nonblock) { net_error(net_errno(err)); break; }
        if (net_wait(&r, POLLIN, r.recv_ms, start)) break;
    }
    close(r.fd);
    if (fd < 0) return -1;
    int result = net_register(fd, 1, r.nonblock);   // FreeBSD's accept (Horizon's bsd service): the new socket inherits the listener's O_NONBLOCK
    if (result >= 0) net_addr_out(addr, len, &a);
    return result;
}
int dm_net_name(int guest, void *addr, unsigned *len, int peer) {
    if (!addr || !len) return net_error(14);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    struct sockaddr_in a; socklen_t n = sizeof a;
    int result = peer ? getpeername(r.fd, (struct sockaddr *)&a, &n) : getsockname(r.fd, (struct sockaddr *)&a, &n), err = errno; close(r.fd);
    if (result >= 0) net_addr_out(addr, len, &a);
    return net_host_result(result, err);
}
int dm_net_shutdown(int guest, int how) {
    if (how < 0 || how > 2) return net_error(22);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    static const int methods[] = {SHUT_RD, SHUT_WR, SHUT_RDWR};
    int result = shutdown(r.fd, methods[how]), err = errno; close(r.fd); return net_host_result(result, err);
}
// Commands as in main's offline service: 1 F_GETFD, 2 F_SETFD, 3 F_GETFL, 4 F_SETFL. Only O_NONBLOCK is a status flag here; the access-mode bits a caller carries over
// from F_GETFL (NEX passes O_RDWR | O_NONBLOCK, 0x802) are accepted and ignored
int dm_net_fcntl(int guest, int cmd, int value) {
    net_ref r; if (net_borrow(guest, &r)) return -1;
    close(r.fd);
    if (cmd == 1) return 1;   // FD_CLOEXEC
    if (cmd == 2) return 0;
    if (cmd == 3) return r.nonblock ? NX_NONBLOCK | 2 : 2;   // O_RDWR
    if (cmd != 4) return net_error(22);
    if ((unsigned)value & ~(unsigned)(NX_NONBLOCK | 3)) return net_error(22);
    dm_host_mutex_lock(&g_net_lock);
    int live = g_net[r.slot].used && g_net[r.slot].generation == r.generation;
    if (live) g_net[r.slot].nonblock = !!(value & NX_NONBLOCK);
    pthread_mutex_unlock(&g_net_lock); return live ? 0 : net_error(9);
}
static int net_message_flags(int flags, int sending) {
    if ((unsigned)flags & ~(unsigned)(NX_MSG_OOB | NX_MSG_PEEK | NX_MSG_DONTROUTE | NX_MSG_WAITALL | NX_MSG_DONTWAIT | (sending ? NX_MSG_NOSIGNAL : 0))) return net_error(95);
    if (sending && (flags & (NX_MSG_PEEK | NX_MSG_WAITALL))) return net_error(95);
    if ((flags & NX_MSG_PEEK) && (flags & NX_MSG_WAITALL)) return net_error(95);
    int out = 0;
    if (flags & NX_MSG_OOB) out |= MSG_OOB;
    if (flags & NX_MSG_PEEK) out |= MSG_PEEK;
    if (flags & NX_MSG_DONTROUTE) out |= MSG_DONTROUTE;
    return out;   // DONTWAIT, WAITALL and blocking are handled around the non-blocking native call; NOSIGNAL always holds (SO_NOSIGPIPE)
}
long dm_net_recv(int guest, void *buf, unsigned long len, int flags, void *addr, unsigned *addrlen) {
    if ((!buf && len) || (addr && !addrlen)) return net_error(14);
    if (len > SSIZE_MAX) return net_error(22);
    int hostflags = net_message_flags(flags, 0); if (hostflags < 0) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    uint64_t start = net_ms(); long result = -1; size_t total = 0; struct sockaddr_in a;
    for (;;) {
        // The kernel's copyout into a write-protected guest page fails with EFAULT instead of reaching the dirty-page fault hook (crash.c), so prepare the span first,
        // again after every wait: the renderer may protect it again meanwhile
        dm_guest_write_prepare(buf ? (char *)buf + total : NULL, len - total);
        socklen_t n = sizeof a;
        ssize_t got = recvfrom(r.fd, buf ? (char *)buf + total : NULL, len - total, hostflags, addr ? (struct sockaddr *)&a : NULL, addr ? &n : NULL); int err = errno;
        if (got >= 0) {
            total += (size_t)got; result = (long)total;
            if (addr) net_addr_out(addr, addrlen, &a);
            if (!got || r.type != 1 || !(flags & NX_MSG_WAITALL) || r.nonblock || (flags & NX_MSG_DONTWAIT) || total == len) break;
            continue;
        }
        if (err != EAGAIN || r.nonblock || (flags & NX_MSG_DONTWAIT)) { if (!total) result = net_error(net_errno(err)); break; }
        if (net_wait(&r, POLLIN, r.recv_ms, start)) { if (!total) result = -1; break; }
    }
    close(r.fd); return result;
}
long dm_net_send(int guest, const void *buf, unsigned long len, int flags, const void *addr, unsigned addrlen) {
    if (!buf && len) return net_error(14);
    if (len > SSIZE_MAX) return net_error(22);
    struct sockaddr_in a; if (addr && net_addr_in(addr, addrlen, &a)) return -1;
    int hostflags = net_message_flags(flags, 1); if (hostflags < 0) return -1;
    net_ref r; if (net_borrow(guest, &r)) return -1;
    uint64_t start = net_ms(); long result;
    for (;;) {
        result = addr ? sendto(r.fd, buf, len, hostflags, (struct sockaddr *)&a, sizeof a) : send(r.fd, buf, len, hostflags); int err = errno;
        if (result >= 0) break;
        if (err != EAGAIN || r.nonblock || (flags & NX_MSG_DONTWAIT)) { result = net_error(net_errno(err)); break; }
        if (net_wait(&r, POLLOUT, r.send_ms, start)) { result = -1; break; }
    }
    close(r.fd); return result;
}

// Horizon's (FreeBSD's) level and option numbers to Darwin's. Unlisted options fail with ENOPROTOOPT rather than reach Darwin under a number that means something else there
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
int dm_net_sockopt(int guest, int level, int option, void *value, unsigned *len, int set) {
    if (!value || !len) return net_error(14);
    net_ref r; if (net_borrow(guest, &r)) return -1;
    if (level == 0xffff && (option == 0x1005 || option == 0x1006)) {
        // SO_SNDTIMEO / SO_RCVTIMEO bound this transport's own waits (the native socket never blocks). Horizon's service takes a u32 of milliseconds; the SDK also passes
        // its 16-byte TimeVal. Zero means no timeout
        int64_t ms;
        if (set) {
            if (*len == 4) { uint32_t v; memcpy(&v, value, 4); ms = v ? (int64_t)v : -1; }
            else if (*len == sizeof(nx_timeval)) {
                nx_timeval v; memcpy(&v, value, sizeof v);
                if (v.sec < 0 || v.usec < 0 || v.usec >= 1000000 || v.sec > INT_MAX / 1000) { close(r.fd); return net_error(22); }
                ms = v.sec * 1000 + (v.usec + 999) / 1000; if (!ms) ms = -1;
            } else { close(r.fd); return net_error(22); }
            dm_host_mutex_lock(&g_net_lock);
            int live = g_net[r.slot].used && g_net[r.slot].generation == r.generation;
            if (live) { if (option == 0x1005) g_net[r.slot].send_ms = ms; else g_net[r.slot].recv_ms = ms; }
            pthread_mutex_unlock(&g_net_lock); close(r.fd); return live ? 0 : net_error(9);
        }
        ms = option == 0x1005 ? r.send_ms : r.recv_ms; if (ms < 0) ms = 0;
        if (*len >= sizeof(nx_timeval)) { nx_timeval v = {ms / 1000, ms % 1000 * 1000}; memcpy(value, &v, sizeof v); *len = sizeof v; }
        else if (*len >= 4) { uint32_t v = (uint32_t)ms; memcpy(value, &v, 4); *len = 4; }
        else { close(r.fd); return net_error(22); }
        close(r.fd); return 0;
    }
    int hl, ho; unsigned size;
    if (net_option(level, option, &hl, &ho, &size)) { close(r.fd); return -1; }
    if (set && level == 0xffff && (option == 2 || option == 0x1007 || option == 0x1008)) { close(r.fd); return net_error(92); }   // read-only
    if (*len < size || (set && *len != size)) { close(r.fd); return net_error(22); }
    // The transport keeps SO_NOSIGPIPE on: a guest that clears it gets EPIPE as Horizon gives, not a host signal that ends the process
    int one = 1; void *hostvalue = level == 0xffff && option == 0x800 ? &one : value;
    socklen_t n = size;
    if (!set) dm_guest_write_prepare(value, n);   // getsockopt's copyout is the kernel's too
    int result = set ? setsockopt(r.fd, hl, ho, hostvalue, n) : getsockopt(r.fd, hl, ho, value, &n), err = errno;
    if (!set && result >= 0) {
        *len = n;
        if (level == 0xffff && option == 0x1007 && n == 4) { int e; memcpy(&e, value, 4); e = net_errno(e); memcpy(value, &e, 4); }   // SO_ERROR is an errno too
        if (level == 0xffff && option == 0x1008 && n == 4) { int t; memcpy(&t, value, 4); t = t == SOCK_STREAM ? 1 : t == SOCK_DGRAM ? 2 : t; memcpy(value, &t, 4); }
    }
    close(r.fd); return net_host_result(result, err);
}

// Horizon's poll bits: POLLIN 1, PRI 2, OUT 4, ERR 8, HUP 0x10, NVAL 0x20, RDNORM 0x40, RDBAND 0x80, WRBAND 0x100 (WRNORM = OUT)
static short net_poll_events(uint16_t e) {
    short h = 0;
    if (e & 1) h |= POLLIN; if (e & 2) h |= POLLPRI; if (e & 4) h |= POLLOUT;
    if (e & 0x40) h |= POLLRDNORM; if (e & 0x80) h |= POLLRDBAND; if (e & 0x100) h |= POLLWRBAND;
    return h;
}
static int16_t net_poll_revents(short h) {
    int16_t e = 0;
    if (h & POLLIN) e |= 1; if (h & POLLPRI) e |= 2; if (h & POLLOUT) e |= 4;
    if (h & POLLERR) e |= 8; if (h & POLLHUP) e |= 0x10; if (h & POLLNVAL) e |= 0x20;
    if (h & POLLRDNORM) e |= 0x40; if (h & POLLRDBAND) e |= 0x80; if (h & POLLWRBAND) e |= 0x100;
    return e;
}
int dm_net_poll(void *fds, uint64_t count, int timeout_ms) {
    if (count > DM_NET_FD_END) return net_error(22);
    if (count && !fds) return net_error(14);
    nx_pollfd *guest = fds; size_t cap = count ? (size_t)count : 1;
    struct pollfd *host = calloc(cap, sizeof *host); net_ref *refs = calloc(cap, sizeof *refs);
    if (!host || !refs) { free(host); free(refs); return net_error(12); }
    int result = 0, invalid = 0;
    for (uint64_t i = 0; i < count; i++) {
        // The game's own array: snapshot fd and events once, so a concurrent edit cannot change which descriptor a borrowed duplicate stands for
        refs[i].fd = host[i].fd = -1;
        int fd = guest[i].fd; uint16_t events = (uint16_t)guest[i].events;
        if (!dm_net_owns(fd)) continue;
        guest[i].revents = 0;
        if (net_borrow(fd, &refs[i])) { refs[i].fd = -1; guest[i].revents = 0x20; invalid++; continue; }   // POLLNVAL
        host[i].fd = refs[i].fd; host[i].events = net_poll_events(events);
    }
    uint64_t start = net_ms();
    for (;;) {
        int wait = invalid ? 0 : NET_SLICE_MS;
        if (timeout_ms >= 0) { uint64_t elapsed = net_ms() - start, left = elapsed < (uint64_t)timeout_ms ? (uint64_t)timeout_ms - elapsed : 0; if ((uint64_t)wait > left) wait = (int)left; }
        dm_thread_native_wait(1); int ready = poll(host, (nfds_t)count, wait), err = errno; dm_thread_native_wait(0);
        if (ready < 0 && err == EINTR) continue;
        if (ready < 0) { result = net_error(net_errno(err)); break; }
        result = invalid;
        for (uint64_t i = 0; i < count; i++) if (refs[i].fd >= 0) {
            int16_t revents = net_live(&refs[i]) ? net_poll_revents(host[i].revents) : 0x20;   // closed during the wait
            guest[i].revents = revents; if (revents) result++;
        }
        if (result || timeout_ms == 0 || (timeout_ms > 0 && net_ms() - start >= (uint64_t)timeout_ms)) break;
    }
    for (uint64_t i = 0; i < count; i++) if (refs[i].fd >= 0) close(refs[i].fd);
    free(host); free(refs); return result;
}

// Read again at most once a second: games ask nifm in loops (Hades queries about once a millisecond), and getifaddrs walks every interface
static pthread_mutex_t g_ip_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_ip_at; static int g_ip_found = -1; static uint32_t g_ip_address, g_ip_mask;
static int net_ip_scan(uint32_t *address, uint32_t *mask) {
    struct ifaddrs *list = NULL; if (getifaddrs(&list)) return -1;
    int found = -1;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        unsigned need = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & need) != need || (p->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) continue;
        *address = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
        *mask = p->ifa_netmask && p->ifa_netmask->sa_family == AF_INET ? ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr : 0;
        found = 0; break;
    }
    freeifaddrs(list); return found;
}
int dm_net_ipconfig(uint32_t *address, uint32_t *mask) {
    if (address) *address = 0;
    if (mask) *mask = 0;
    if (!dm_net_enabled()) return -1;
    dm_host_mutex_lock(&g_ip_lock);
    uint64_t now = net_ms();
    if (!g_ip_at || now - g_ip_at >= 1000) { g_ip_address = g_ip_mask = 0; g_ip_found = net_ip_scan(&g_ip_address, &g_ip_mask); g_ip_at = now ? now : 1; }
    int found = g_ip_found;
    if (!found && address) *address = g_ip_address;
    if (!found && mask) *mask = g_ip_mask;
    pthread_mutex_unlock(&g_ip_lock); return found;
}
