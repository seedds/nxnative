// coop vnet (MIT, our own code): the game's UDP sockets on the virtual 169.254.S.0/24 (research/06 §5.3). Descriptors 0x100 and up, per node.
// No host port is used: Bind only records the port, so instances on one machine all bind 49530. A datagram to a station's address, the host's,
// 169.254.S.255 or 255.255.255.255 enters the star (coop_mpnet.c); broadcasts reach every other node and never the sender. Receivers see the
// sender's virtual address and port. Anything outside the /24 is unreachable, so the game can reach only session members.
#include "coop_int.h"
#include <stdio.h>

// nn::socket's values (FreeBSD)
#define V_SOCK_NONBLOCK 0x20000000
#define V_SOL_SOCKET 0xffff
#define V_SO_BROADCAST 0x20
#define V_SO_ERROR 0x1007
#define V_SO_TYPE 0x1008
#define V_MSG_PEEK 0x2
#define V_MSG_DONTWAIT 0x80
#define V_F_GETFL 3
#define V_F_SETFL 4
#define V_O_NONBLOCK 4
#define V_EDESTADDRREQ 39
#define V_LOG_FIRST 8      // datagrams logged per socket and direction, then every V_LOG_EVERY-th
#define V_LOG_EVERY 500

static vsock *get(coop_node *n, int fd) {
    if (fd < COOP_FD_BASE || fd >= COOP_FD_BASE + COOP_FD_MAX) return NULL;
    vsock *s = &n->sk[fd - COOP_FD_BASE];
    return s->used ? s : NULL;
}
static int want_log(uint64_t k) { return k < V_LOG_FIRST || k % V_LOG_EVERY == 0; }
static void ipstr(char *o, uint32_t ip) { snprintf(o, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF); }
static const char *hexstr(coop_node *n, const void *p, size_t len) {   // the first bytes of a logged datagram, in n->scratch (free while the lock is held and nothing is being sent)
    static const char hx[] = "0123456789abcdef";
    char *o = (char *)n->scratch; const uint8_t *b = p; size_t k = len < 192 ? len : 192;
    for (size_t i = 0; i < k; i++) { o[2 * i] = hx[b[i] >> 4]; o[2 * i + 1] = hx[b[i] & 15]; }
    o[2 * k] = 0;
    return o;
}
static void sa_put(void *sa, uint32_t *salen, uint32_t ip, uint16_t port) {   // BSD sockaddr_in: len, family, port and address big-endian
    if (!sa || !salen) return;
    uint8_t b[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip};
    memcpy(sa, b, *salen < 16 ? *salen : 16); *salen = 16;
}
static int port_used(coop_node *n, uint16_t port) {
    for (int i = 0; i < COOP_FD_MAX; i++) if (n->sk[i].used && n->sk[i].bound && n->sk[i].port == port) return 1;
    return 0;
}
static void autobind(coop_node *n, vsock *s) {
    do { s->port = n->next_eph++; if (n->next_eph < 49152) n->next_eph = 49152; } while (port_used(n, s->port));
    s->bound = 1; s->bind_ip = 0;
}
static void q_free(vsock *s) { while (s->qn) { free(s->q[s->qh]); s->qh = (s->qh + 1) % VQ; s->qn--; } s->qh = 0; }

int coop_vnet_is(coop_node *n, int fd) { cnode_lock(n); int r = get(n, fd) != NULL; cnode_unlock(n); return r; }
int coop_vnet_socket(coop_node *n, int family, int type, int proto) {
    if (family != 2 || (type & 0xffff) != 2 || (proto != 0 && proto != 17)) return -COOP_ENETDOWN;
    cnode_lock(n);
    if (n->ls == COOP_LS_NONE) { cnode_unlock(n); return -COOP_ENETDOWN; }
    for (int i = 0; i < COOP_FD_MAX; i++) {
        vsock *s = &n->sk[i];
        if (s->used) continue;
        memset(s, 0, sizeof *s); s->used = 1; s->nonblock = !!(type & V_SOCK_NONBLOCK);
        coop_logf(n, "sock: Socket(%d, %#x, %d) -> virtual %#x%s", family, type, proto, COOP_FD_BASE + i, s->nonblock ? " (non-blocking)" : "");
        cnode_unlock(n);
        return COOP_FD_BASE + i;
    }
    cnode_unlock(n);
    return -COOP_EMFILE;
}
int coop_vnet_bind(coop_node *n, int fd, const void *sa, uint32_t len) {
    const uint8_t *a = sa;
    if (!a || len < 8) return -COOP_EINVAL;
    uint16_t port = (uint16_t)(a[2] << 8 | a[3]);
    uint32_t ip = (uint32_t)a[4] << 24 | (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
    cnode_lock(n);
    vsock *s = get(n, fd); int r = 0;
    if (!s) r = -COOP_EBADF;
    else if (s->bound) r = -COOP_EINVAL;
    else if (ip && !(n->subnet && ip == vip_of(n, n->self))) r = -COOP_EADDRNOTAVAIL;
    else if (port && port_used(n, port)) r = -COOP_EADDRINUSE;
    else if (!port) { autobind(n, s); s->bind_ip = ip; }
    else { s->port = port; s->bind_ip = ip; s->bound = 1; }
    if (s) { char b[16]; ipstr(b, ip); coop_logf(n, "sock: Bind(%#x, %s:%u) -> %d", fd, b, s->port, r); }
    cnode_unlock(n);
    return r;
}
int coop_vnet_close(coop_node *n, int fd) {
    cnode_lock(n);
    vsock *s = get(n, fd);
    if (!s) { cnode_unlock(n); return -COOP_EBADF; }
    coop_logf(n, "sock: Close(%#x): %llu sent (%llu broadcast), %llu received, %llu dropped by a full queue", fd, (unsigned long long)s->sends,
              (unsigned long long)s->bsends, (unsigned long long)s->recvs, (unsigned long long)s->drops);
    q_free(s); memset(s, 0, sizeof *s);
    cnode_unlock(n);
    return 0;
}
int coop_vnet_setsockopt(coop_node *n, int fd, int level, int opt, const void *v, uint32_t len) {
    uint32_t x = 0; if (v) memcpy(&x, v, len < 4 ? len : 4);
    cnode_lock(n);
    vsock *s = get(n, fd);
    if (s && level == V_SOL_SOCKET && opt == V_SO_BROADCAST) s->bcast = x != 0;
    if (s) coop_logf(n, "sock: SetSockOpt(%#x, level %#x, option %#x, %u bytes, %#x)%s", fd, level, opt, len, x, level == V_SOL_SOCKET && opt == V_SO_BROADCAST ? ": SO_BROADCAST" : "");
    cnode_unlock(n);
    return s ? 0 : -COOP_EBADF;
}
int coop_vnet_getsockopt(coop_node *n, int fd, int level, int opt, void *v, uint32_t *len) {
    cnode_lock(n);
    vsock *s = get(n, fd);
    if (s && v && len) {
        uint32_t x = 0;
        if (level == V_SOL_SOCKET && opt == V_SO_BROADCAST) x = (uint32_t)s->bcast;
        else if (level == V_SOL_SOCKET && opt == V_SO_TYPE) x = 2;
        else if (level == V_SOL_SOCKET && opt == V_SO_ERROR) x = 0;
        memset(v, 0, *len); memcpy(v, &x, *len < 4 ? *len : 4); if (*len > 4) *len = 4;
    }
    cnode_unlock(n);
    return s ? 0 : -COOP_EBADF;
}
int coop_vnet_getsockname(coop_node *n, int fd, void *sa, uint32_t *len) {
    cnode_lock(n);
    vsock *s = get(n, fd);
    if (s) sa_put(sa, len, s->bind_ip, s->bound ? s->port : 0);
    cnode_unlock(n);
    return s ? 0 : -COOP_EBADF;
}
int coop_vnet_fcntl(coop_node *n, int fd, int cmd, int v) {
    cnode_lock(n);
    vsock *s = get(n, fd); int r = s ? 0 : -COOP_EBADF;
    if (s && cmd == V_F_GETFL) r = 2 | (s->nonblock ? V_O_NONBLOCK : 0);   // O_RDWR
    if (s && cmd == V_F_SETFL) s->nonblock = !!(v & V_O_NONBLOCK);
    cnode_unlock(n);
    return r;
}
int64_t coop_vnet_sendto(coop_node *n, int fd, const void *buf, uint64_t len, int flags, const void *sa, uint32_t salen) {
    (void)flags;
    const uint8_t *a = sa;
    if (len > COOP_DGRAM_MAX) return -COOP_EMSGSIZE;
    if (!a || salen < 8) return -V_EDESTADDRREQ;
    uint16_t dport = (uint16_t)(a[2] << 8 | a[3]);
    uint32_t dst = (uint32_t)a[4] << 24 | (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
    cnode_lock(n);
    vsock *s = get(n, fd); int64_t r = (int64_t)len;
    if (!s) r = -COOP_EBADF;
    else if ((n->ls != COOP_LS_AP_CREATED && n->ls != COOP_LS_STATION_CONNECTED) || !n->subnet) r = -COOP_ENETUNREACH;
    else if (!vip_is_bcast(n, dst) && (dst & 0xFFFFFF00u) != n->subnet) r = -COOP_ENETUNREACH;
    if (r < 0) {
        if (s && want_log(s->sends)) { char d[16]; ipstr(d, dst); coop_logf(n, "sock: SendTo(%#x, %llu bytes, %s:%u) -> -%lld", fd, (unsigned long long)len, d, dport, (long long)-r); }
        cnode_unlock(n); return r;
    }
    if (!s->bound) autobind(n, s);
    int bc = vip_is_bcast(n, dst);
    uint64_t k = s->sends++;
    if (bc) {
        uint64_t b = s->bsends++;
        if (want_log(b)) {   // in KTGL's broadcast-relay mode the first 4 bytes name the destination node (research/06 §2.4)
            char d[16]; ipstr(d, dst);
            coop_logf(n, "sock: broadcast #%llu on %#x, %llu bytes to %s:%u%s: %s", (unsigned long long)b, fd, (unsigned long long)len, d, dport,
                      s->bcast ? "" : " (SO_BROADCAST not set)", hexstr(n, buf, (size_t)(b < V_LOG_FIRST ? len : len < 4 ? len : 4)));
        }
    } else if (want_log(k)) { char d[16]; ipstr(d, dst); coop_logf(n, "sock: send #%llu on %#x, %llu bytes to %s:%u%s%s", (unsigned long long)k, fd, (unsigned long long)len, d, dport, k < V_LOG_FIRST ? ": " : "", k < V_LOG_FIRST ? hexstr(n, buf, (size_t)len) : ""); }
    cmp_route(n, n->self, vip_of(n, n->self), s->port, dst, dport, buf, (size_t)len);
    cnode_unlock(n);
    return r;
}
int64_t coop_vnet_recvfrom(coop_node *n, int fd, void *buf, uint64_t len, int flags, void *sa, uint32_t *salen) {
    cnode_lock(n);
    for (;;) {
        vsock *s = get(n, fd);
        if (!s) { cnode_unlock(n); return -COOP_EBADF; }
        if (s->qn) {
            vdgram *d = s->q[s->qh];
            uint64_t c = d->len < len ? d->len : len;
            if (c) memcpy(buf, d->data, (size_t)c);
            sa_put(sa, salen, d->src_ip, d->src_port);
            if (!(flags & V_MSG_PEEK)) {
                if (want_log(s->recvs)) { char a[16]; ipstr(a, d->src_ip); coop_logf(n, "sock: received #%llu on %#x: %u bytes from %s:%u", (unsigned long long)s->recvs, fd, d->len, a, d->src_port); }
                s->q[s->qh] = NULL; s->qh = (s->qh + 1) % VQ; s->qn--; free(d); s->recvs++;
            }
            cnode_unlock(n);
            return (int64_t)c;
        }
        if (s->nonblock || (flags & V_MSG_DONTWAIT) || !n->io_on) { cnode_unlock(n); return -COOP_EAGAIN; }
        cldn_wait(n, 1000000);   // a blocking socket: wait for data (the game's own socket is non-blocking)
    }
}
static int events(vsock *s, int want) {
    int r = 0;
    if (s->qn) r |= want & (COOP_POLLIN | COOP_POLLRDNORM);
    r |= want & COOP_POLLOUT;
    return r;
}
int coop_vnet_poll(coop_node *n, coop_pollfd *fds, uint64_t nfds, int timeout_ms) {
    int64_t until = cplat_now_ms() + (timeout_ms < 0 ? 100 : timeout_ms);
    cnode_lock(n);
    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            vsock *s = get(n, fds[i].fd);
            fds[i].revents = (int16_t)(s ? events(s, fds[i].events) : COOP_POLLNVAL);
            ready += fds[i].revents != 0;
        }
        if (ready || cplat_now_ms() >= until) { cnode_unlock(n); return ready; }
        cldn_wait(n, 1000000);
    }
}
int coop_vnet_select(coop_node *n, int nfds, uint64_t *rd, uint64_t *wr, uint64_t *ex, int timeout_ms) {
    if (nfds < 0) return -COOP_EINVAL;
    if (nfds > 1024) nfds = 1024;
    int words = (nfds + 63) / 64;
    uint64_t in[3][16] = {{0}}, out[3][16];
    uint64_t *sets[3] = {rd, wr, ex};
    for (int k = 0; k < 3; k++) if (sets[k]) memcpy(in[k], sets[k], (size_t)words * 8);
    int64_t until = cplat_now_ms() + (timeout_ms < 0 ? 100 : timeout_ms);
    cnode_lock(n);
    for (;;) {
        int ready = 0;
        memset(out, 0, sizeof out);
        for (int fd = 0; fd < nfds; fd++) for (int k = 0; k < 3; k++) {
            if (!(in[k][fd >> 6] >> (fd & 63) & 1)) continue;
            vsock *s = get(n, fd);
            if (!s) { cnode_unlock(n); return -COOP_EBADF; }
            int ok = k == 0 ? s->qn > 0 : k == 1;
            if (ok) { out[k][fd >> 6] |= 1ull << (fd & 63); ready++; }
        }
        if (ready || cplat_now_ms() >= until) {
            for (int k = 0; k < 3; k++) if (sets[k]) memcpy(sets[k], out[k], (size_t)words * 8);
            cnode_unlock(n);
            return ready;
        }
        cldn_wait(n, 1000000);
    }
}

void cvnet_deliver(coop_node *n, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *p, size_t len) {
    if (len > COOP_DGRAM_MAX) return;
    for (int i = 0; i < COOP_FD_MAX; i++) {
        vsock *s = &n->sk[i];
        if (!s->used || !s->bound || s->port != dst_port || (s->bind_ip && s->bind_ip != dst_ip)) continue;
        vdgram *d = malloc(sizeof *d + (len ? len : 1));
        if (!d) return;
        d->src_ip = src_ip; d->src_port = src_port; d->len = (uint16_t)len;
        if (len) memcpy(d->data, p, len);
        if (s->qn == VQ) { free(s->q[s->qh]); s->q[s->qh] = NULL; s->qh = (s->qh + 1) % VQ; s->qn--; s->drops++; }
        s->q[(s->qh + s->qn) % VQ] = d; s->qn++;
        uint64_t k = n->log_dgrams++;
        if (want_log(k)) { char a[16], b[16]; ipstr(a, src_ip); ipstr(b, dst_ip); coop_logf(n, "sock: delivered #%llu to %#x: %zu bytes from %s:%u to %s:%u%s%s", (unsigned long long)k, COOP_FD_BASE + i, len, a, src_port, b, dst_port, k < V_LOG_FIRST ? ": " : "", k < V_LOG_FIRST ? hexstr(n, p, len) : ""); }
        return;
    }
}
void cvnet_reset(coop_node *n) {
    cnode_lock(n);
    for (int i = 0; i < COOP_FD_MAX; i++) { q_free(&n->sk[i]); memset(&n->sk[i], 0, sizeof n->sk[i]); }
    cnode_unlock(n);
}
