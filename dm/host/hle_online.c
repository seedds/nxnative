// Online mode (DM_ONLINE=1, off unless set): the bsd socket service on the host's sockets, name resolution with a redirect list, the network reported available, a network
// service account with an ID token, and ssl's built-in CA certificates replaced by one CA. It exists for games whose online services run on stand-in servers on this Mac
// (The Duskbloods' network test with playDB-CNT's npln_stub.py and frontdoor.py: the game needs its online login even to play alone). Without DM_ONLINE every hook in
// hle_net.c, hle_offline.c, hle_account.c and hle_misc.c keeps its offline answer; with it they hand over to the functions here.
//   DM_ONLINE_HOSTS   names that resolve to 127.0.0.1 (comma-separated; default The Duskbloods' NPLN and FromSoftware names)
//   DM_ONLINE_SERVER  host:port that a connection to 127.0.0.1:443 goes to instead (default 127.0.0.1:18443, playDB-CNT's front door)
//   DM_ONLINE_CA      the CA certificate (DER or PEM) every built-in certificate id answers with (playDB-CNT's pki/ca.der)
//   DM_ONLINE_TOKEN   the network service account's ID token (default nxnative-online)
//   DM_ONLINE_PUBLIC  1 = names off the list resolve and connect for real; unset, only this Mac is reachable (as playDB-CNT's loopback-only mode)
// Horizon's sockets are FreeBSD's: the structures are Darwin's (sockaddr with its length byte, pollfd, addrinfo, fd_set), the constants mostly too; AF_INET6, SOCK_NONBLOCK /
// SOCK_CLOEXEC, MSG_NOSIGNAL and the TCP keep-alive options differ and are converted. Guest descriptors are small numbers from 0, as the bsd service gives them, mapped to host ones
#include "hle.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// A host call that can block gives up the guest core while it waits (hle_thread.c), as on Horizon a thread blocked in the bsd service lets the others of its core run.
// gRPC's threads (nn::npln) share one core: a poll or event read holding it starved the thread that had to finish the TLS handshake (as #2589's transport brackets its waits)
#define HOST_WAIT(expr) ({ dm_thread_native_wait(1); __typeof__(expr) hw_r = (expr); int hw_e = errno; dm_thread_native_wait(0); errno = hw_e; hw_r; })
int dm_online(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_ONLINE"); v = e && *e && strcmp(e, "0"); if (v) LOG("online mode (DM_ONLINE): the game's sockets are the host's, see hle_online.c"); }
    return v;
}

// ---------- descriptors ----------
enum { ON_MAX = 1024, K_FREE = 0, K_SOCK, K_EFD };
typedef struct { int kind, host, wr; uint64_t count; int efd_flags; } on_fd;   // an event descriptor: a pipe (host = read end, wr = write end) and its counter
static on_fd g_fd[ON_MAX];
static pthread_mutex_t g_fd_m = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_err;
static int *guest_errno(void);
int dm_online_errno(void) { int *g = guest_errno(); return g ? *g : t_err; }
void dm_online_set_errno(int e) { t_err = e; int *g = guest_errno(); if (g) *g = e; }
// The game's errno is its C library's (nn::socket::detail::SetLastErrno writes *__errno_location()), and gRPC reads it directly: after a non-blocking connect it must see
// EINPROGRESS there, not a value it set itself earlier. Every failure writes both copies, on the game's thread
static int *guest_errno(void) {
    static int *(*_Atomic loc)(void); static _Atomic int looked;
    if (!atomic_load(&looked)) { loc = (int *(*)(void))guest_sym("__errno_location"); atomic_store(&looked, 1); }
    int *(*f)(void) = atomic_load(&loc); return f ? f() : NULL;
}
// Horizon's socket errno numbers are Linux's (EAGAIN 11, EINPROGRESS 115, EAFNOSUPPORT 97), not Darwin's: gRPC took Darwin's EAGAIN (35) after its first read for a real error
// and the TLS handshake stalled. The table is #2589's net_errno; anything else is EIO
static int hz_errno(int e) {
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
static int fail(int e) { e = hz_errno(e); t_err = e; int *g = guest_errno(); if (g) *g = e; return -1; }   // e is a Darwin errno
static int host_err(void) { return fail(errno); }
static int fd_new(int kind, int host) {
    pthread_mutex_lock(&g_fd_m);
    for (int i = 0; i < ON_MAX; i++) if (g_fd[i].kind == K_FREE) { g_fd[i] = (on_fd){kind, host, -1, 0, 0}; pthread_mutex_unlock(&g_fd_m); return i; }
    pthread_mutex_unlock(&g_fd_m);
    return fail(EMFILE);
}
static int host_of(int s) {   // the host descriptor of a socket, or -1 with EBADF
    pthread_mutex_lock(&g_fd_m);
    int h = s >= 0 && s < ON_MAX && g_fd[s].kind != K_FREE ? g_fd[s].host : -1;
    pthread_mutex_unlock(&g_fd_m);
    if (h < 0) t_err = EBADF;
    return h;
}


// ---------- addresses: Horizon's sockaddr is FreeBSD's, whose AF_INET6 is 28 (Darwin's 30) ----------
enum { H_AF_INET6 = 28 };
static int fam_in(int f) { return f == H_AF_INET6 ? AF_INET6 : f; }
static int fam_out(int f) { return f == AF_INET6 ? H_AF_INET6 : f; }
static socklen_t sa_in(const void *g, socklen_t n, struct sockaddr_storage *h) {
    if (!g || n > sizeof *h) return 0;
    memcpy(h, g, n); ((struct sockaddr *)h)->sa_family = (sa_family_t)fam_in(((const uint8_t *)g)[1]);
    return n;
}
static void sa_out(void *g, unsigned *gn, const struct sockaddr_storage *h, socklen_t n) {
    if (!g || !gn) return;
    unsigned c = *gn < n ? *gn : n; memcpy(g, h, c);
    if (c >= 2) ((uint8_t *)g)[1] = (uint8_t)fam_out(h->ss_family);
    *gn = n;
}

// The redirect list and the server a local port-443 connection goes to
static int name_listed(const char *h) {
    static char list[2048]; static int init;
    if (!init) { init = 1; const char *e = getenv("DM_ONLINE_HOSTS");
                 snprintf(list, sizeof list, ",%s,", e ? e : "npln.srv.nintendo.net,npln.nintendo.net,n.n.srv.nintendo.net,p.srv.nintendo.net,fromsoftware-game.net"); }
    if (!h || !*h) return 0;
    char k[300]; snprintf(k, sizeof k, ",%s,", h);
    if (strstr(list, k)) return 1;
    for (const char *p = list; (p = strchr(p, ',')) && p[1]; p++) {   // a subdomain of a listed name
        const char *e = strchr(p + 1, ','); size_t n = e - p - 1, hl = strlen(h);
        if (hl > n && h[hl - n - 1] == '.' && !strncmp(h + hl - n, p + 1, n)) return 1;
    }
    return 0;
}
static int public_ok(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_ONLINE_PUBLIC"); v = e && *e && strcmp(e, "0"); } return v; }
static void server_addr(struct sockaddr_in *a) {
    const char *e = getenv("DM_ONLINE_SERVER"); char h[64] = "127.0.0.1"; int port = 18443;
    if (e && *e) { const char *c = strrchr(e, ':'); if (c) { snprintf(h, sizeof h, "%.*s", (int)(c - e), e); port = atoi(c + 1); } }
    memset(a, 0, sizeof *a); a->sin_len = sizeof *a; a->sin_family = AF_INET; a->sin_port = htons((uint16_t)port); inet_pton(AF_INET, h, &a->sin_addr);
}

// ---------- the bsd service ----------
static const int H_SOCK_NONBLOCK = 0x20000000, H_SOCK_CLOEXEC = 0x10000000, H_MSG_NOSIGNAL = 0x20000;
int dm_online_socket(int fam, int type, int proto) {
    int nb = type & H_SOCK_NONBLOCK; type &= ~(H_SOCK_NONBLOCK | H_SOCK_CLOEXEC);
    int h = socket(fam_in(fam), type, proto);
    if (h < 0) return host_err();
    int one = 1; setsockopt(h, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   // a write to a closed peer is EPIPE, never SIGPIPE (MSG_NOSIGNAL)
    if (nb) fcntl(h, F_SETFL, fcntl(h, F_GETFL) | O_NONBLOCK);
    int g = fd_new(K_SOCK, h);
    if (g < 0) close(h);
    else { static int n; if (n++ < 8) LOG("online: socket %d (family %d, type %d, protocol %d)", g, fam, type, proto); }
    return g;
}
int dm_online_close(int s) {
    pthread_mutex_lock(&g_fd_m);
    if (s < 0 || s >= ON_MAX || g_fd[s].kind == K_FREE) { pthread_mutex_unlock(&g_fd_m); return fail(EBADF); }
    on_fd f = g_fd[s]; g_fd[s].kind = K_FREE;
    pthread_mutex_unlock(&g_fd_m);
    close(f.host); if (f.wr >= 0) close(f.wr);
    return 0;
}
int dm_online_bind(int s, const void *a, unsigned n) {
    int h = host_of(s); struct sockaddr_storage sa; socklen_t l = sa_in(a, n, &sa);
    if (h < 0) return -1;
    return bind(h, (struct sockaddr *)&sa, l) ? host_err() : 0;
}
int dm_online_connect(int s, const void *a, unsigned n) {
    int h = host_of(s); struct sockaddr_storage sa; socklen_t l = sa_in(a, n, &sa);
    if (h < 0) return -1;
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&sa;
        int local = (ntohl(in->sin_addr.s_addr) >> 24) == 127;
        if (local && ntohs(in->sin_port) == 443) { server_addr(in); l = sizeof *in; }
        else if (!local && !public_ok()) { static int c; if (c++ < 8) { char b[32]; LOG("online: connection to %s:%d refused (only this Mac is reachable; DM_ONLINE_PUBLIC=1 allows it)", inet_ntop(AF_INET, &in->sin_addr, b, sizeof b), ntohs(in->sin_port)); } return fail(ENETUNREACH); }
        static int c; if (c++ < 16) { char b[32]; LOG("online: connect %d → %s:%d", s, inet_ntop(AF_INET, &in->sin_addr, b, sizeof b), ntohs(in->sin_port)); }
    } else if (sa.ss_family == AF_INET6) {   // a dual-stack socket reaches IPv4 through ::ffff:a.b.c.d (gRPC connects this way); ::1 is this Mac too
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&sa; char b[64];
        int mapped = IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr), loop6 = IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
        int local = loop6 || (mapped && in6->sin6_addr.s6_addr[12] == 127);
        if (local && ntohs(in6->sin6_port) == 443) {
            struct sockaddr_in v4; server_addr(&v4);
            memset(&in6->sin6_addr, 0, sizeof in6->sin6_addr); in6->sin6_addr.s6_addr[10] = in6->sin6_addr.s6_addr[11] = 0xff;
            memcpy(&in6->sin6_addr.s6_addr[12], &v4.sin_addr, 4); in6->sin6_port = v4.sin_port;
            if (loop6) { int off = 0; setsockopt(h, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); }
        } else if (!local && !public_ok()) { static int c; if (c++ < 8) LOG("online: connection to [%s]:%d refused (only this Mac is reachable; DM_ONLINE_PUBLIC=1 allows it)", inet_ntop(AF_INET6, &in6->sin6_addr, b, sizeof b), ntohs(in6->sin6_port)); return fail(ENETUNREACH); }
        static int c; if (c++ < 16) LOG("online: connect %d → [%s]:%d", s, inet_ntop(AF_INET6, &in6->sin6_addr, b, sizeof b), ntohs(in6->sin6_port));
    } else if (!public_ok()) return fail(ENETUNREACH);
    return HOST_WAIT(connect(h, (struct sockaddr *)&sa, l)) ? host_err() : 0;
}
int dm_online_listen(int s, int n) { int h = host_of(s); if (h < 0) return -1; return listen(h, n) ? host_err() : 0; }
int dm_online_accept(int s, void *a, unsigned *n) {
    int h = host_of(s); if (h < 0) return -1;
    struct sockaddr_storage sa; socklen_t l = sizeof sa;
    int c = HOST_WAIT(accept(h, (struct sockaddr *)&sa, &l));
    if (c < 0) return host_err();
    sa_out(a, n, &sa, l);
    int g = fd_new(K_SOCK, c); if (g < 0) close(c);
    return g;
}
static int name_of(int s, void *a, unsigned *n, int peer) {
    int h = host_of(s); if (h < 0) return -1;
    struct sockaddr_storage sa; socklen_t l = sizeof sa;
    if ((peer ? getpeername : getsockname)(h, (struct sockaddr *)&sa, &l)) return host_err();
    sa_out(a, n, &sa, l); return 0;
}
int dm_online_getpeername(int s, void *a, unsigned *n) { return name_of(s, a, n, 1); }
int dm_online_getsockname(int s, void *a, unsigned *n) { return name_of(s, a, n, 0); }
// TCP keep-alive options: FreeBSD's TCP_KEEPIDLE 0x100, TCP_KEEPINTVL 0x200, TCP_KEEPCNT 0x400 are Darwin's 0x10, 0x101, 0x102
static int opt_in(int level, int opt) {
    if (level == IPPROTO_TCP) switch (opt) { case 0x100: return TCP_KEEPALIVE; case 0x200: return TCP_KEEPINTVL; case 0x400: return TCP_KEEPCNT; }
    return opt;
}
int dm_online_setsockopt(int s, int level, int opt, const void *v, unsigned n) {
    int h = host_of(s); if (h < 0) return -1;
    if (setsockopt(h, level, opt_in(level, opt), v, n)) {
        static int c; if (c++ < 8) LOG("online: setsockopt %d level %#x option %#x (%u bytes): %s, reported done", s, level, opt, n, strerror(errno));
    }
    return 0;   // an option the Mac does not have changes nothing the game can see
}
int dm_online_getsockopt(int s, int level, int opt, void *v, unsigned *n) {
    int h = host_of(s); if (h < 0) return -1;
    socklen_t l = n ? *n : 0;
    dm_guest_write_prepare(v, l);   // the kernel's copy-out does not take the write-protect fault that tracks guest pages
    if (getsockopt(h, level, opt_in(level, opt), v, &l)) return host_err();
    if (n) *n = l;
    if (level == 0xffff && opt == 0x1007 && l == 4) { int e; memcpy(&e, v, 4); e = hz_errno(e); memcpy(v, &e, 4); }   // SO_ERROR: gRPC reads it after a non-blocking connect
    return 0;
}
int dm_online_fcntl(int s, int cmd, int arg) {   // F_GETFL (3) and F_SETFL (4); Horizon's O_NONBLOCK is Linux's 0x800 (Darwin's is 4)
    int h = host_of(s); if (h < 0) return -1;
    if (cmd == 3) { int r = fcntl(h, F_GETFL); return r < 0 ? host_err() : (r & O_NONBLOCK ? 0x802 : 2); }   // O_RDWR as well
    if (cmd == 4) return fcntl(h, F_SETFL, (fcntl(h, F_GETFL) & ~O_NONBLOCK) | (arg & 0x800 ? O_NONBLOCK : 0)) < 0 ? host_err() : 0;
    return fail(EINVAL);
}
int dm_online_shutdown(int s, int how) { int h = host_of(s); if (h < 0) return -1; return shutdown(h, how) ? host_err() : 0; }
long dm_online_recvfrom(int s, void *b, unsigned long n, int f, void *a, unsigned *al) {
    int h = host_of(s); if (h < 0) return -1;
    struct sockaddr_storage sa; socklen_t l = sizeof sa;
    dm_guest_write_prepare(b, n);
    ssize_t r = HOST_WAIT(recvfrom(h, b, n, f & ~H_MSG_NOSIGNAL, a ? (struct sockaddr *)&sa : NULL, a ? &l : NULL));
    if (r < 0) return host_err();
    if (a) sa_out(a, al, &sa, l);
    return r;
}
long dm_online_sendto(int s, const void *b, unsigned long n, int f, const void *a, unsigned al) {
    int h = host_of(s); if (h < 0) return -1;
    struct sockaddr_storage sa; socklen_t l = a ? sa_in(a, al, &sa) : 0;
    if (a && sa.ss_family == AF_INET && !public_ok() && (ntohl(((struct sockaddr_in *)&sa)->sin_addr.s_addr) >> 24) != 127) return fail(ENETUNREACH);
    ssize_t r = HOST_WAIT(sendto(h, b, n, f & ~H_MSG_NOSIGNAL, a ? (struct sockaddr *)&sa : NULL, l));
    return r < 0 ? host_err() : r;
}
long dm_online_recv(int s, void *b, unsigned long n, int f) { return dm_online_recvfrom(s, b, n, f, NULL, NULL); }
// sendmsg / recvmsg (gRPC writes and reads with them): FreeBSD's msghdr and iovec are Darwin's on 64-bit; the address goes through the family conversion, ancillary data is not passed
typedef struct { void *name; uint32_t namelen; void *iov; int iovlen; void *control; uint32_t controllen; int flags; } g_msghdr;
long dm_online_sendmsg(int s, const void *m, int f) {
    int h = host_of(s); if (h < 0) return -1;
    const g_msghdr *g = m; struct sockaddr_storage sa;
    if (g->name && sa_in(g->name, g->namelen, &sa) && sa.ss_family == AF_INET && !public_ok() && (ntohl(((struct sockaddr_in *)&sa)->sin_addr.s_addr) >> 24) != 127) return fail(ENETUNREACH);
    struct msghdr hm = {g->name ? &sa : NULL, g->name ? g->namelen : 0, g->iov, g->iovlen, NULL, 0, 0};
    ssize_t r = HOST_WAIT(sendmsg(h, &hm, f & ~H_MSG_NOSIGNAL));
    return r < 0 ? host_err() : r;
}
long dm_online_recvmsg(int s, void *m, int f) {
    int h = host_of(s); if (h < 0) return -1;
    g_msghdr *g = m; struct sockaddr_storage sa;
    struct msghdr hm = {g->name ? &sa : NULL, g->name ? (socklen_t)sizeof sa : 0, g->iov, g->iovlen, NULL, 0, 0};
    { struct iovec *v = g->iov; for (int i = 0; v && i < g->iovlen; i++) dm_guest_write_prepare(v[i].iov_base, v[i].iov_len); }
    ssize_t r = HOST_WAIT(recvmsg(h, &hm, f & ~H_MSG_NOSIGNAL));
    if (r < 0) return host_err();
    if (g->name) sa_out(g->name, &g->namelen, &sa, hm.msg_namelen);
    g->controllen = 0; g->flags = hm.msg_flags;
    return r;
}
long dm_online_send(int s, const void *b, unsigned long n, int f) { return dm_online_sendto(s, b, n, f, NULL, 0); }
static long efd_rw(int s, void *b, unsigned long n, int wr);
long dm_online_read(int s, void *b, unsigned long n) {
    pthread_mutex_lock(&g_fd_m); int k = s >= 0 && s < ON_MAX ? g_fd[s].kind : K_FREE; pthread_mutex_unlock(&g_fd_m);
    if (k == K_EFD) return efd_rw(s, b, n, 0);
    int h = host_of(s); if (h < 0) return -1;
    dm_guest_write_prepare(b, n);
    ssize_t r = HOST_WAIT(read(h, b, n)); return r < 0 ? host_err() : r;
}
long dm_online_write(int s, const void *b, unsigned long n) {
    pthread_mutex_lock(&g_fd_m); int k = s >= 0 && s < ON_MAX ? g_fd[s].kind : K_FREE; pthread_mutex_unlock(&g_fd_m);
    if (k == K_EFD) return efd_rw(s, (void *)b, n, 1);
    int h = host_of(s); if (h < 0) return -1;
    ssize_t r = HOST_WAIT(write(h, b, n)); return r < 0 ? host_err() : r;
}

// ---------- event descriptors: a counter with a pipe that is readable while the counter is not 0, so one poll covers them and sockets ----------
int dm_online_eventfd(uint64_t v, int flags) {
    int p[2]; if (pipe(p)) return host_err();
    fcntl(p[0], F_SETFL, O_NONBLOCK); fcntl(p[1], F_SETFL, O_NONBLOCK);
    int g = fd_new(K_EFD, p[0]);
    if (g < 0) { close(p[0]); close(p[1]); return -1; }
    pthread_mutex_lock(&g_fd_m); g_fd[g].wr = p[1]; g_fd[g].count = v; g_fd[g].efd_flags = flags; pthread_mutex_unlock(&g_fd_m);
    if (v) { char c = 1; (void)!write(p[1], &c, 1); }
    return g;
}
// Read: the counter (or 1 of it with the semaphore flag, bit 0), waiting while it is 0 unless the descriptor is non-blocking (bits 1, 2: EAGAIN). Write: adds to it
int dm_online_eventfd_read(int s, uint64_t *out) {
    for (;;) {
        pthread_mutex_lock(&g_fd_m);
        if (s < 0 || s >= ON_MAX || g_fd[s].kind != K_EFD) { pthread_mutex_unlock(&g_fd_m); return fail(EBADF); }
        on_fd *f = &g_fd[s];
        if (f->count) {
            uint64_t v = f->efd_flags & 1 ? 1 : f->count; f->count -= v; *out = v;
            if (!f->count) { char d[64]; while (read(f->host, d, sizeof d) > 0) {} }
            pthread_mutex_unlock(&g_fd_m); return 0;
        }
        int nb = f->efd_flags & 6, h = f->host;
        pthread_mutex_unlock(&g_fd_m);
        if (nb) return fail(EAGAIN);
        struct pollfd p = {h, POLLIN, 0}; (void)HOST_WAIT(poll(&p, 1, 100));
    }
}
int dm_online_eventfd_write(int s, uint64_t v) {
    pthread_mutex_lock(&g_fd_m);
    if (s < 0 || s >= ON_MAX || g_fd[s].kind != K_EFD) { pthread_mutex_unlock(&g_fd_m); return fail(EBADF); }
    on_fd *f = &g_fd[s]; int was = f->count != 0; f->count += v;
    if (!was && f->count) { char c = 1; (void)!write(f->wr, &c, 1); }
    pthread_mutex_unlock(&g_fd_m);
    return 0;
}
static long efd_rw(int s, void *b, unsigned long n, int wr) {   // read / write of 8 bytes on an event descriptor, as on Linux
    if (n < 8) return fail(EINVAL);
    if (wr) { uint64_t v; memcpy(&v, b, 8); return dm_online_eventfd_write(s, v) ? -1 : 8; }
    uint64_t v; if (dm_online_eventfd_read(s, &v)) return -1;
    memcpy(b, &v, 8); return 8;
}

// ---------- poll and select: the guest's descriptors mapped to the host's, one host poll; an endless wait comes back every 100 ms with 0, as the offline Poll ----------
typedef struct { int32_t fd; int16_t events, revents; } g_pollfd;
int dm_online_poll(void *fds, uint64_t n, int ms) {
    g_pollfd *g = fds;
    struct pollfd stack[64], *p = n <= 64 ? stack : calloc(n, sizeof *p);
    if (!p) return fail(ENOMEM);
    pthread_mutex_lock(&g_fd_m);
    for (uint64_t i = 0; i < n; i++) {
        int s = g[i].fd; p[i] = (struct pollfd){-1, g[i].events, 0};
        if (s >= 0 && s < ON_MAX && g_fd[s].kind != K_FREE) p[i].fd = g_fd[s].host;
        else if (s >= 0) p[i].revents = POLLNVAL;
    }
    pthread_mutex_unlock(&g_fd_m);
    int r = 0;
    for (uint64_t i = 0; i < n; i++) if (p[i].revents) r++;
    // Wait in slices of 100 ms (the core is given up during each) until something is ready or the game's timeout passes; an endless wait stays endless, as on Horizon
    // At least one host poll: a zero timeout asks once and returns (a curl-style client polls its connecting socket that way)
    for (uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), first = 1; !r; first = 0) {
        int wait = 100;
        if (ms >= 0) { uint64_t el = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000000; if (!first && el >= (uint64_t)ms) break; wait = el >= (uint64_t)ms ? 0 : (uint64_t)ms - el < 100 ? (int)((uint64_t)ms - el) : 100; }
        r = HOST_WAIT(poll(p, (nfds_t)n, wait));
        if (r < 0 && errno == EINTR) { r = 0; continue; }
        if (r < 0) { if (p != stack) free(p); return host_err(); }
    }
    for (uint64_t i = 0; i < n; i++) g[i].revents = (int16_t)p[i].revents;
    if (p != stack) free(p);
    return r;
}
int dm_online_select(int nfds, uint64_t *rs, uint64_t *ws, uint64_t *es, const int64_t *tv) {
    if (nfds > ON_MAX) nfds = ON_MAX;
    g_pollfd p[ON_MAX]; int np = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int16_t ev = 0;
        if (rs && rs[fd >> 6] >> (fd & 63) & 1) ev |= POLLIN;
        if (ws && ws[fd >> 6] >> (fd & 63) & 1) ev |= POLLOUT;
        if (es && es[fd >> 6] >> (fd & 63) & 1) ev |= POLLPRI;
        if (ev) p[np++] = (g_pollfd){fd, ev, 0};
    }
    int64_t ms = tv ? tv[0] * 1000 + tv[1] / 1000 : -1;
    if (dm_online_poll(p, (uint64_t)np, ms > 0x7fffffff ? 0x7fffffff : (int)ms) < 0) return -1;
    for (uint64_t *set[3] = {rs, ws, es}, k = 0; k < 3; k++) if (set[k]) memset(set[k], 0, ((size_t)(nfds + 63) / 64) * 8);
    int n = 0;
    for (int i = 0; i < np; i++) {
        if (p[i].revents & POLLNVAL) return fail(EBADF);
        if (rs && p[i].revents & (POLLIN | POLLHUP | POLLERR)) { rs[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
        if (ws && p[i].revents & (POLLOUT | POLLERR)) { ws[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
        if (es && p[i].revents & POLLPRI) { es[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
    }
    return n;
}

// ---------- name resolution: a listed name is 127.0.0.1; another goes to the Mac's resolver with DM_ONLINE_PUBLIC, else EAI_NONAME. The result is FreeBSD's addrinfo (Darwin's
// layout), one host allocation each, freed by dm_online_freeaddrinfo ----------
typedef struct g_ai { int flags, family, socktype, protocol; uint32_t addrlen; char *canonname; void *addr; struct g_ai *next; } g_ai;
static g_ai *ai_make(const struct sockaddr *sa, socklen_t l, int socktype, int protocol) {
    g_ai *a = calloc(1, sizeof *a + sizeof(struct sockaddr_storage));
    if (!a) return NULL;
    a->family = fam_out(sa->sa_family); a->socktype = socktype; a->protocol = protocol; a->addrlen = l;
    a->addr = a + 1; memcpy(a->addr, sa, l); ((uint8_t *)a->addr)[1] = (uint8_t)fam_out(sa->sa_family);
    return a;
}
int dm_online_getaddrinfo(const char *host, const char *serv, const void *hints, void **res) {
    const g_ai *gh = hints; int socktype = gh ? gh->socktype : 0, protocol = gh ? gh->protocol : 0;
    *res = NULL;
    if (host && name_listed(host)) {
        struct sockaddr_in in = {sizeof in, AF_INET, htons(serv ? (uint16_t)atoi(serv) : 0), {htonl(INADDR_LOOPBACK)}, {0}};
        static int c; if (c++ < 16) LOG("online: %s → 127.0.0.1 (DM_ONLINE_HOSTS)", host);
        *res = ai_make((struct sockaddr *)&in, sizeof in, socktype ? socktype : SOCK_STREAM, protocol);
        return *res ? 0 : EAI_MEMORY;
    }
    struct addrinfo hh = {0}, *r = NULL;
    if (gh) { hh.ai_flags = gh->flags; hh.ai_family = fam_in(gh->family); hh.ai_socktype = gh->socktype; hh.ai_protocol = gh->protocol; }
    int numeric = host && (inet_addr(host) != INADDR_NONE || strchr(host, ':'));
    if (host && !numeric && !public_ok()) { static int c; if (c++ < 16) LOG("online: %s not resolved (not in DM_ONLINE_HOSTS; DM_ONLINE_PUBLIC=1 resolves it)", host); return EAI_NONAME; }
    int e = HOST_WAIT(getaddrinfo(host, serv, gh ? &hh : NULL, &r));
    if (e) return e;
    g_ai **tail = (g_ai **)res;
    for (struct addrinfo *a = r; a; a = a->ai_next) { g_ai *x = ai_make(a->ai_addr, a->ai_addrlen, a->ai_socktype, a->ai_protocol); if (!x) break; *tail = x; tail = &x->next; }
    freeaddrinfo(r);
    return *res ? 0 : EAI_NONAME;
}
// nn::socket::AddrInfo (the C++ overload on that type) keeps the address at +0x18 and the canonical name at +0x20, the other way round from addrinfo; next is at +0x28 in both
int dm_online_getaddrinfo_nn(const char *host, const char *serv, const void *hints, void **res) {
    int e = dm_online_getaddrinfo(host, serv, hints, res);
    for (g_ai *x = e ? NULL : *res; x; x = x->next) { void *t = x->addr; x->addr = x->canonname; x->canonname = t; }
    return e;
}
void dm_online_freeaddrinfo(void *a) { for (g_ai *x = a, *n; x; x = n) { n = x->next; free(x); } }

// ---------- ssl: every built-in CA certificate id answers with DM_ONLINE_CA. The game's own TLS (The Duskbloods' NPLN gRPC, its bundled OpenSSL) takes its trusted CAs from
// here: entries of 0x18 bytes {u32 id, u32 status (1 trusted), u64 DER size, DER pointer}, the list ended by an entry with id -1 (main+0x677b08 walks it so); asking for id -1 is all ----------
static const uint8_t *ca_der(size_t *n) {
    static uint8_t *d; static size_t dn; static int init;
    if (!init) {
        init = 1; const char *p = getenv("DM_ONLINE_CA");
        FILE *f = p ? fopen(p, "rb") : NULL;
        if (!f) { LOG("[RED] online: no CA certificate (DM_ONLINE_CA=%s): the game's TLS will trust no server", p ? p : "unset"); return NULL; }
        static uint8_t buf[16384]; size_t r = fread(buf, 1, sizeof buf, f); fclose(f);
        if (r > 10 && !memcmp(buf, "-----BEGIN", 10)) {   // PEM: the base64 between the lines
            static const char *B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            char *s = strchr((char *)buf, '\n'), *e = strstr((char *)buf, "-----END"); d = malloc(r); uint32_t acc = 0; int bits = 0;
            for (char *q = s; q && q < e; q++) { const char *k = strchr(B, *q); if (!k || !*q) continue; acc = acc << 6 | (uint32_t)(k - B); bits += 6; if (bits >= 8) { bits -= 8; d[dn++] = (uint8_t)(acc >> bits); } }
        } else { d = malloc(r); memcpy(d, buf, r); dn = r; }
        LOG("online: the built-in CA certificates are %s (%zu bytes)", p, dn);
    }
    *n = dn; return d;
}
static const uint32_t CA_ALL_IDS[] = {1, 2, 3, 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019, 1020,
                                      1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043,
                                      1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 65536};   // as playDB-CNT's certificate archive
static uint32_t ca_count(const uint32_t *ids, unsigned n) { return n == 1 && ids && ids[0] == 0xffffffffu ? sizeof CA_ALL_IDS / 4 : n; }
int dm_online_ca_bufsize(uint32_t *out, const uint32_t *ids, unsigned n) {
    size_t dn; if (!ca_der(&dn)) return -1;
    *out = (uint32_t)((ca_count(ids, n) + 1) * 0x18 + dn);
    return 0;
}
int dm_online_ca_certs(void **out, uint8_t *buf, unsigned size, const uint32_t *ids, unsigned n) {
    size_t dn; const uint8_t *d = ca_der(&dn); if (!d) return -1;
    uint32_t c = ca_count(ids, n), all = c != n || !ids;
    size_t need = (c + 1) * 0x18 + dn;
    if (size < need) return -1;
    uint8_t *der = buf + (c + 1) * 0x18; memcpy(der, d, dn);
    for (uint32_t i = 0; i <= c; i++) {
        uint8_t *e = buf + i * 0x18; uint32_t id = i == c ? 0xffffffffu : all ? CA_ALL_IDS[i] : ids[i], st = i == c ? 0 : 1; uint64_t sz = i == c ? 0 : dn; uint64_t p = i == c ? 0 : (uint64_t)der;
        memcpy(e, &id, 4); memcpy(e + 4, &st, 4); memcpy(e + 8, &sz, 8); memcpy(e + 16, &p, 8);
    }
    *out = buf;
    static int once; if (!once++) LOG("online: built-in certificates: %u ids answered with the CA", c);
    return 0;
}

// ---------- account: a network service account with an ID token ----------
const char *dm_online_token(void) { const char *e = getenv("DM_ONLINE_TOKEN"); return e && *e ? e : "nxnative-online"; }
int dm_online_load_token(uint64_t *n, char *buf, uint64_t size) {   // LoadNetworkServiceAccountIdTokenCache: the token and its length (no terminator counted)
    const char *t = dm_online_token(); size_t l = strlen(t);
    if (!buf || size < l + 1) return RES_FAIL;
    memcpy(buf, t, l + 1); if (n) *n = l;
    return RES_OK;
}
// nsd::ResolveEx: Horizon's name settings replace '%' in a server name with the environment ("lp1", as nsd::GetEnvironmentIdentifier answers); the name is otherwise as given
int dm_online_resolve_fqdn(void *out, const void *in) {
    const char *s = in; char *o = out; size_t k = 0;
    for (size_t i = 0; s[i] && i < 0xff && k < 0xfc; i++) { if (s[i] == '%') { memcpy(o + k, "lp1", 3); k += 3; } else o[k++] = s[i]; }
    o[k] = 0;
    static int c; if (c++ < 16) LOG("online: nsd %s → %s", s, o);
    return RES_OK;
}

// ---------- bcat: the delivery cache is a host folder (DM_ONLINE_BCAT) laid out as <directory>/<file>, as the cache storage. The Duskbloods reads its regulation from
// Regulation/regulation_nt_1_0.bin (the romfs regulation_nt.bin) and stops at "failed to retrieve latest calibrations" without it. The host owns the progress and file
// objects whole (their constructors are hooked too): the progress keeps status and result in its first 8 bytes, a file its host descriptor + 1 in its first 8 ----------
static const char *bcat_dir(void) { const char *d = getenv("DM_ONLINE_BCAT"); return d && *d ? d : NULL; }
int dm_online_bcat_mount(void) {
    const char *d = bcat_dir();
    static int once; if (!once++) LOG("online: bcat delivery cache %s", d ? d : "(none: DM_ONLINE_BCAT is unset, mounting fails)");
    return d ? RES_OK : RES_FAIL;
}
void dm_online_bcat_progress(void *p, int done, int r) { uint32_t v[2] = {done ? 9u : 0u, (uint32_t)r}; memcpy(p, v, 8); }   // status Done = 9
int dm_online_bcat_status(const void *p) { uint32_t v; memcpy(&v, p, 4); return (int)v; }
int dm_online_bcat_result(const void *p) { uint32_t v; memcpy(&v, (const uint8_t *)p + 4, 4); return (int)v; }
int dm_online_bcat_sync(void *p) { int r = bcat_dir() ? RES_OK : RES_FAIL; if (p) dm_online_bcat_progress(p, 1, r); return r; }
static int bcat_fd(const void *f) { int64_t v; memcpy(&v, f, 8); return (int)v - 1; }
void dm_online_bcat_file_init(void *f) { int64_t z = 0; memcpy(f, &z, 8); }
void dm_online_bcat_file_close(void *f) { int h = bcat_fd(f); if (h >= 0) close(h); dm_online_bcat_file_init(f); }
int dm_online_bcat_file_open(void *f, const char *path) {
    const char *d = bcat_dir(); char p[1024];
    if (!d || !path || strstr(path, "..") || path[0] == '/') return RES_FAIL;
    dm_online_bcat_file_close(f);
    snprintf(p, sizeof p, "%s/%s", d, path);
    int h = open(p, O_RDONLY | O_CLOEXEC);
    LOG("online: bcat open %s%s", path, h < 0 ? ": not in the cache" : "");
    if (h < 0) return RES_FAIL;
    int64_t v = (int64_t)h + 1; memcpy(f, &v, 8);
    return RES_OK;
}
int dm_online_bcat_file_read(void *f, uint64_t *out, int64_t off, void *buf, uint64_t n) {
    int h = bcat_fd(f); uint64_t k = 0;
    if (h < 0 || off < 0) return RES_FAIL;
    while (k < n) { ssize_t r = pread(h, (uint8_t *)buf + k, n - k, off + (off_t)k); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; k += (uint64_t)r; }
    if (out) *out = k;
    return RES_OK;
}
int64_t dm_online_bcat_file_size(const void *f) { int h = bcat_fd(f); struct stat st; return h >= 0 && !fstat(h, &st) ? (int64_t)st.st_size : 0; }

// For ssl_online_apple.m: the host socket behind a game descriptor (a dup the TLS engine owns), and DM_ONLINE_CA's DER
int dm_online_dup_host(int s) { int h = host_of(s); return h < 0 ? -1 : dup(h); }
const uint8_t *dm_online_ca_der(size_t *n) { return ca_der(n); }
