// Native LDN discovery/session bridge. The service owns control-plane sockets;
// game packets use nn::socket and the actual adapter IPv4 addresses in NodeInfo.
// ldn_mitm authenticates neither its control TCP connection nor the passphrase:
// this is a local-network transport, not a replacement for Nintendo security.
#include "hle_ldn.h"
#include "net_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifndef DM_LDN_TEST
#include "hle.h"
#endif

enum { LDN_NONE, LDN_INITIALIZED, LDN_AP, LDN_CREATED, LDN_STATION, LDN_CONNECTED };
#define LDN_ERROR(n) (((uint32_t)(n) << 9) | 203u)
enum { LDN_INVALID_STATE = LDN_ERROR(32), LDN_NODE_NOT_FOUND = LDN_ERROR(48), LDN_CONNECT_FAILED = LDN_ERROR(64),
       LDN_NOT_FOUND = LDN_ERROR(65), LDN_TIMEOUT = LDN_ERROR(66), LDN_REJECTED = LDN_ERROR(67), LDN_INVALID = LDN_ERROR(96),
       LDN_VERSION_LOW = LDN_ERROR(113), LDN_VERSION_HIGH = LDN_ERROR(114), LDN_FULL = LDN_ERROR(144) };
enum { LDN_ROOM_MAX = 24, LDN_TX_MAX = 8192 };
typedef struct {
    int fd, node, active; uint64_t generation, deadline;
    uint8_t rx[DM_LDN_PACKET_MAX], tx[LDN_TX_MAX]; size_t received, queued, sent;
} ldn_peer;
typedef struct { dm_ldn_network network; struct sockaddr_in endpoint; } ldn_room;
struct dm_ldn_context {
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t worker;
    dm_ldn_transport transport; dm_ldn_notify notify; void *opaque;
    int udp, listener, wake[2], stopping, state, reason, scanning;
    dm_net_discovery_plan scan_plan,scan_supplement; uint64_t scan_next; int scan_tx_failed;
    uint64_t generation; dm_ldn_network network; dm_ldn_network_config config;
    dm_ldn_update updates[8]; ldn_peer peers[8]; ldn_room rooms[LDN_ROOM_MAX]; int room_count;
    uint8_t filters[8][6]; int filter_count; dm_ldn_address private_addresses[8]; int private_count;
};
// Opt-in, bounded control-plane diagnostics. Only operation names and numeric
// sizes/states/results are recorded: never addresses, names, IDs or payloads.
static int ldn_trace_enabled(void) {
    const char *flag = getenv("DM_LDN_TRACE"), *net = getenv("DM_NET_TRACE");
    return (flag && !strcmp(flag, "1")) || (net && !strcmp(net, "1"));
}
static void ldn_trace(const char *format, ...) {
    int saved = errno;
    static unsigned records;
    if (!ldn_trace_enabled()) { errno = saved; return; }
    unsigned record = __atomic_fetch_add(&records, 1, __ATOMIC_RELAXED);
    if (record < 2048) {
        char message[384]; va_list args; va_start(args, format);
        vsnprintf(message, sizeof message, format, args); va_end(args);
        fprintf(stderr, "ldn: %s\n", message);
    }
    errno = saved;
}
static uint64_t ldn_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static struct timespec ldn_deadline(unsigned ms) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += ms / 1000; t.tv_nsec += (ms % 1000) * 1000000L; if (t.tv_nsec >= 1000000000L) { ++t.tv_sec; t.tv_nsec -= 1000000000L; } return t; }
static struct sockaddr_in ldn_endpoint(uint32_t address, unsigned port) { struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = address; a.sin_port = htons((uint16_t)port); return a; }
static void ldn_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int yes = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
}
static void ldn_wake(dm_ldn_context *c) { uint8_t b = 1; (void)write(c->wake[1], &b, 1); }
static void ldn_changed(dm_ldn_context *c) { pthread_cond_broadcast(&c->changed); if (c->notify) c->notify(c->opaque); }
static void ldn_state_set(dm_ldn_context *c, int state) { if (c->state != state) { ldn_trace("state from=%d to=%d reason=%d", c->state, state, c->reason); c->state = state; ldn_changed(c); } }
static void ldn_network_set(dm_ldn_context *c, const dm_ldn_network *n) {
    if (!memcmp(&c->network, n, sizeof *n)) return;
    for (int i = 0; i < 8; ++i) {
        if (c->network.nodes[i].connected && (!n->nodes[i].connected || memcmp(c->network.nodes[i].mac, n->nodes[i].mac, 6))) c->updates[i].change |= 2;
        if (n->nodes[i].connected && (!c->network.nodes[i].connected || memcmp(c->network.nodes[i].mac, n->nodes[i].mac, 6))) c->updates[i].change |= 1;
    }
    c->network = *n; ldn_changed(c);
}
static void ldn_mac(uint8_t out[6], uint32_t address) { out[0] = 2; out[1] = 0; memcpy(out + 2, &address, 4); }
static void ldn_local_mac(dm_ldn_context *c, uint8_t out[6]) { static const uint8_t zero[6]; if (memcmp(c->transport.mac, zero, 6)) memcpy(out, c->transport.mac, 6); else ldn_mac(out, c->transport.address); }
static dm_ldn_node ldn_node(dm_ldn_context *c, const dm_ldn_user *user, int version, int id) {
    dm_ldn_node n = {0}; n.ip = ntohl(c->transport.address); ldn_local_mac(c, n.mac); n.id = id; n.connected = 1;
    memcpy(n.name, user->name, 33); n.version = (int16_t)version; return n;
}
static void ldn_peer_close(dm_ldn_context *c, int i) { ldn_peer *p = &c->peers[i]; if (p->fd >= 0) close(p->fd); p->fd = -1; p->active = 0; p->generation = ++c->generation; p->received = p->queued = p->sent = 0; }
static int ldn_queue(dm_ldn_context *c, int i, unsigned type, const void *data, size_t size) {
    ldn_peer *p = &c->peers[i]; uint8_t packet[DM_LDN_PACKET_MAX]; int n = dm_ldn_packet_encode(packet, sizeof packet, type, data, size, 1);
    if (n < 0 || p->fd < 0) return -1;
    if (p->sent) { memmove(p->tx, p->tx + p->sent, p->queued - p->sent); p->queued -= p->sent; p->sent = 0; }
    if ((size_t)n > sizeof p->tx - p->queued) return -1;
    memcpy(p->tx + p->queued, packet, (size_t)n); p->queued += n; return 0;
}
static void ldn_sync(dm_ldn_context *c) {
    // An update that cannot be delivered means the member has left. Rebuild
    // the roster before queuing a new snapshot to all surviving members.
    for (;;) {
        dm_ldn_network n = c->network; n.node_count = 1;
        for (int i = 1; i < 8; i++) { if (c->peers[i].active) ++n.node_count; else memset(&n.nodes[i], 0, sizeof n.nodes[i]); }
        ldn_network_set(c, &n); int removed = 0;
        for (int i = 1; i < 8; ++i) if (c->peers[i].active && ldn_queue(c, i, DM_LDN_SYNC, &c->network, sizeof c->network)) { ldn_peer_close(c, i); removed = 1; }
        if (!removed) break;
    }
    ldn_wake(c);
}
static void ldn_drop(dm_ldn_context *c, int i) {
    int active = c->peers[i].active; ldn_peer_close(c, i);
    if (c->state == LDN_CREATED && active) ldn_sync(c);
    else if ((c->state == LDN_CONNECTED || c->state == LDN_STATION) && i == 0) {
        c->reason = 6; dm_ldn_network empty = {0}; ldn_network_set(c, &empty); ldn_state_set(c, LDN_STATION); ldn_changed(c);
    }
}
static int ldn_mac_in_list(const uint8_t mac[6], uint8_t list[8][6], int count) { for (int i = 0; i < count; ++i) if (!memcmp(mac, list[i], 6)) return 1; return 0; }
static int ldn_accept_node(dm_ldn_context *c, int slot, dm_ldn_node *n) {
    static const uint8_t zero_mac[6];
    if (!n->ip || !memcmp(n->mac, zero_mac, 6) || (n->mac[0] & 1) || !memchr(n->name, 0, 33) || n->version != c->network.nodes[0].version || n->platform > 1) return 0;
    unsigned policy = c->network.accept_policy; int listed = ldn_mac_in_list(n->mac, c->filters, c->filter_count);
    if (policy == 1 || (policy == 2 && listed) || (policy == 3 && !listed)) return 0;
    if (c->private_count) { int allowed = 0; for (int i = 0; i < c->private_count; ++i) if (c->private_addresses[i].ip == n->ip && !memcmp(c->private_addresses[i].mac, n->mac, 6)) allowed = 1; if (!allowed) return 0; }
    for (int i = 0; i < 8; ++i) if (c->network.nodes[i].connected && (c->network.nodes[i].ip == n->ip || !memcmp(c->network.nodes[i].mac, n->mac, 6))) {
        // Production peers have different adapter addresses. Loopback fixtures
        // deliberately share an address and still use distinct locally assigned MACs.
        if (!memcmp(c->network.nodes[i].mac, n->mac, 6)) return 0;
    }
    n->id = slot; n->connected = 1; dm_ldn_network next = c->network; next.nodes[slot] = *n; ++next.node_count; c->peers[slot].active = 1; ldn_network_set(c, &next); ldn_sync(c); return 1;
}
static int ldn_frame(dm_ldn_context *c, int i, const uint8_t *data, size_t size) {
    dm_ldn_network payload; unsigned type; int n = dm_ldn_packet_decode(data, size, &type, &payload, sizeof payload);
    if (n < 0) { ldn_trace("tcp-frame invalid size=%zu", size); return -1; }
    ldn_trace("tcp-frame type=%u expanded=%d state=%d slot=%d", type, n, c->state, i);
    if (c->state == LDN_CREATED && i > 0 && type == DM_LDN_CONNECT && !c->peers[i].active) {
        dm_ldn_node node; memcpy(&node, &payload, sizeof node);
        struct sockaddr_in endpoint; socklen_t len = sizeof endpoint;
        if (getpeername(c->peers[i].fd, (struct sockaddr *)&endpoint, &len) || node.ip != ntohl(endpoint.sin_addr.s_addr)) return -1;
        return ldn_accept_node(c, i, &node) ? 0 : -1;
    }
    if ((c->state == LDN_STATION || c->state == LDN_CONNECTED) && i == 0 && type == DM_LDN_SYNC && dm_ldn_network_valid(&payload) &&
        !memcmp(&payload.id, &c->network.id, sizeof payload.id) && payload.nodes[0].ip == c->network.nodes[0].ip) {
        uint8_t mac[6]; ldn_local_mac(c, mac); int found = 0;
        for (int j = 1; j < 8; ++j) if (payload.nodes[j].connected && !memcmp(payload.nodes[j].mac, mac, 6) && payload.nodes[j].ip == ntohl(c->transport.address)) found = 1;
        if (!found) return -1;
        ldn_network_set(c, &payload); c->peers[0].active = 1; ldn_state_set(c, LDN_CONNECTED); return 0;
    }
    return -1;
}
static int ldn_read_peer(dm_ldn_context *c, int i) {
    ldn_peer *p = &c->peers[i]; uint8_t data[2048]; ssize_t n = recv(p->fd, data, sizeof data, 0);
    if (n <= 0) { if (!n || (errno != EAGAIN && errno != EINTR)) ldn_trace("tcp-read slot=%d result=%ld errno=%d active=%d", i, (long)n, n < 0 ? errno : 0, p->active); return n < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : -1; }
    for (size_t at = 0; at < (size_t)n;) {
        size_t wanted = 12; if (p->received >= 12) { int total = dm_ldn_packet_size(p->rx); if (total < 0) return -1; wanted = (size_t)total; }
        size_t copy = wanted - p->received; if (copy > (size_t)n - at) copy = (size_t)n - at;
        memcpy(p->rx + p->received, data + at, copy); p->received += copy; at += copy;
        if (p->received >= 12) { int total = dm_ldn_packet_size(p->rx); if (total < 0) return -1; if (p->received == (size_t)total) { if (ldn_frame(c, i, p->rx, p->received)) return -1; p->received = 0; } }
    }
    return 0;
}
static void ldn_read_udp(dm_ldn_context *c) {
    uint8_t data[DM_LDN_PACKET_MAX + 1], payload[sizeof(dm_ldn_network)]; struct sockaddr_in from; socklen_t len = sizeof from;
    ssize_t size = recvfrom(c->udp, data, sizeof data, 0, (struct sockaddr *)&from, &len); unsigned type;
    if (size < 12 || size > DM_LDN_PACKET_MAX || from.sin_family != AF_INET) return;
    int n = dm_ldn_packet_decode(data, (size_t)size, &type, payload, sizeof payload); if (n < 0) return;
    if (type == DM_LDN_SCAN && c->state == LDN_CREATED && !(from.sin_addr.s_addr == c->transport.address && ntohs(from.sin_port) == c->transport.port)) {
        int count = dm_ldn_packet_encode(data, sizeof data, DM_LDN_SCAN_RESPONSE, &c->network, sizeof c->network, 1);
        if (count > 0) (void)sendto(c->udp, data, count, 0, (struct sockaddr *)&from, len);
    } else if (type == DM_LDN_SCAN_RESPONSE && c->scanning) {
        dm_ldn_network room; memcpy(&room, payload, sizeof room);
        if (!dm_ldn_network_valid(&room) || room.nodes[0].ip != ntohl(from.sin_addr.s_addr)) return;
        for (int i = 0; i < c->room_count; ++i) if (!memcmp(&room.id, &c->rooms[i].network.id, sizeof room.id)) { c->rooms[i].network = room; c->rooms[i].endpoint = from; return; }
        if (c->room_count < LDN_ROOM_MAX) c->rooms[c->room_count++] = (ldn_room){room, from};
    }
}
static void *ldn_worker(void *opaque) {
    dm_ldn_context *c = opaque;
    for (;;) {
        struct pollfd fds[11]; uint64_t generations[8];
        pthread_mutex_lock(&c->lock); if (c->stopping) { pthread_mutex_unlock(&c->lock); break; }
        fds[0] = (struct pollfd){c->wake[0], POLLIN, 0}; fds[1] = (struct pollfd){c->udp, POLLIN, 0}; fds[2] = (struct pollfd){c->listener, POLLIN, 0};
        for (int i = 0; i < 8; ++i) { ldn_peer *p = &c->peers[i]; fds[3+i] = (struct pollfd){p->fd, POLLIN | (p->queued > p->sent ? POLLOUT : 0), 0}; generations[i] = p->generation; }
        int timeout = 100;
        if (c->scanning && (c->scan_plan.count||c->scan_supplement.count)) {
            uint64_t now = ldn_ms(); timeout = c->scan_next > now ? (int)(c->scan_next - now) : 0;
            if (timeout > 100) timeout = 100;
        }
        pthread_mutex_unlock(&c->lock); int ready = poll(fds, 11, timeout);
        if (ready < 0 && errno != EINTR) break;
        pthread_mutex_lock(&c->lock);
        if (fds[0].revents & POLLIN) { uint8_t drain[64]; while (read(c->wake[0], drain, sizeof drain) > 0) {} }
        if (c->scanning && (c->scan_plan.count||c->scan_supplement.count) && ldn_ms() >= c->scan_next) {
            uint8_t packet[12]; dm_ldn_packet_encode(packet, sizeof packet, DM_LDN_SCAN, NULL, 0, 0);
            for (unsigned i = 0; i < DM_DISCOVERY_BATCH; ++i) {
                dm_net_discovery_plan *plan=c->scan_plan.count?&c->scan_plan:&c->scan_supplement;
                if(!plan->count)break;
                dm_net_discovery_plan saved = *plan; uint32_t address;
                if (!dm_net_discovery_next(plan, &address)) { plan->count=0;continue; }
                ssize_t result=dm_net_discovery_send_native(c->udp,packet,sizeof packet,0,address,htons(c->transport.discovery_port));
                if (result < 0 && (errno == EAGAIN || errno == ENOBUFS || errno == EINTR)) { *plan=saved;break; }
                if (result < 0 && (!plan->automatic||!dm_net_discovery_unreachable(errno))) {
                    // A supplemental failure must not replace an accepted
                    // original broadcast's result; native errors stay visible.
                    if(plan==&c->scan_supplement) { plan->count=0;break; }
                    c->scan_tx_failed=1;c->scan_plan.count=c->scan_supplement.count=0;pthread_cond_broadcast(&c->changed);break;
                }
            }
            c->scan_next = ldn_ms() + DM_DISCOVERY_TICK_MS;
        }
        if (fds[1].revents & POLLIN) ldn_read_udp(c);
        if (fds[2].revents & POLLIN && c->listener == fds[2].fd) {
            int fd = accept(c->listener, NULL, NULL);
            if (fd >= 0) { int slot = -1; for (int i = 1; i < c->network.max_nodes; ++i) if (c->peers[i].fd < 0) { slot = i; break; }
                ldn_trace("tcp-accept slot=%d state=%d policy=%u", slot, c->state, c->network.accept_policy);
                if (slot < 0 || c->state != LDN_CREATED || c->network.accept_policy == 1) close(fd);
                else { ldn_nonblock(fd); c->peers[slot].fd = fd; c->peers[slot].node = slot; c->peers[slot].generation = ++c->generation; c->peers[slot].deadline = ldn_ms() + c->transport.connect_ms; }
            }
        }
        for (int i = 0; i < 8; ++i) {
            ldn_peer *p = &c->peers[i]; if (p->fd < 0 || p->generation != generations[i] || p->fd != fds[i+3].fd) continue;
            int drop = fds[i+3].revents & (POLLERR | POLLHUP | POLLNVAL);
            if (!drop && fds[i+3].revents & POLLIN) drop = ldn_read_peer(c, i) != 0;
            if (!drop && fds[i+3].revents & POLLOUT && p->queued > p->sent) {
                ssize_t sent = send(p->fd, p->tx + p->sent, p->queued - p->sent, 0);
                if (sent > 0) { p->sent += sent; if (p->sent == p->queued) p->sent = p->queued = 0; }
                else if (sent == 0 || (errno != EAGAIN && errno != EINTR)) drop = 1;
            }
            if (!p->active && ldn_ms() >= p->deadline) { ldn_trace("tcp-handshake-timeout slot=%d state=%d", i, c->state); drop = 1; }
            if (drop) ldn_drop(c, i);
        }
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}
dm_ldn_context *dm_ldn_start(const dm_ldn_transport *t, dm_ldn_notify notify, void *opaque) {
    if (!t || !t->address || !t->mask || !t->port || !t->discovery_port || !t->scan_ms || !t->connect_ms) return NULL;
    ldn_trace("start port=%u discovery-port=%u scan-ms=%u connect-ms=%u multicast=%d", t->port, t->discovery_port, t->scan_ms, t->connect_ms, dm_net_multicast_allowed());
    dm_ldn_context *c = calloc(1, sizeof *c); if (!c) return NULL;
    c->transport = *t; c->notify = notify; c->opaque = opaque; c->udp = c->listener = c->wake[0] = c->wake[1] = -1;
    for (int i = 0; i < 8; ++i) c->peers[i].fd = -1;
    pthread_mutex_init(&c->lock, NULL); pthread_cond_init(&c->changed, NULL);
    c->udp = socket(AF_INET, SOCK_DGRAM, 0); if (c->udp < 0 || pipe(c->wake)) goto fail;
    ldn_nonblock(c->udp); ldn_nonblock(c->wake[0]); ldn_nonblock(c->wake[1]); int yes = 1;
    if (dm_net_multicast_allowed()) setsockopt(c->udp, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    struct sockaddr_in a = ldn_endpoint(t->bind_address, t->port);
    if (bind(c->udp, (struct sockaddr *)&a, sizeof a)) goto fail;
    c->state = LDN_INITIALIZED;
    if (pthread_create(&c->worker, NULL, ldn_worker, c)) goto fail;
    return c;
fail:
    ldn_trace("start failed errno=%d", errno);
    if (c->udp >= 0) close(c->udp); if (c->wake[0] >= 0) close(c->wake[0]); if (c->wake[1] >= 0) close(c->wake[1]);
    pthread_cond_destroy(&c->changed); pthread_mutex_destroy(&c->lock); free(c); return NULL;
}
void dm_ldn_stop(dm_ldn_context *c) {
    if (!c) return; pthread_mutex_lock(&c->lock); c->stopping = 1; ldn_wake(c); pthread_cond_broadcast(&c->changed); pthread_mutex_unlock(&c->lock);
    pthread_join(c->worker, NULL); for (int i = 0; i < 8; ++i) ldn_peer_close(c, i);
    if (c->listener >= 0) close(c->listener); close(c->udp); close(c->wake[0]); close(c->wake[1]); pthread_cond_destroy(&c->changed); pthread_mutex_destroy(&c->lock); free(c);
}
int dm_ldn_state(dm_ldn_context *c) { if (!c) return 0; pthread_mutex_lock(&c->lock); int n = c->state; pthread_mutex_unlock(&c->lock); return n; }
int dm_ldn_reason(dm_ldn_context *c) { if (!c) return 0; pthread_mutex_lock(&c->lock); int n = c->reason; pthread_mutex_unlock(&c->lock); return n; }
uint32_t dm_ldn_open(dm_ldn_context *c, int ap) {
    if (!c) return LDN_INVALID_STATE; pthread_mutex_lock(&c->lock); uint32_t r = LDN_INVALID_STATE;
    if (c->state == LDN_INITIALIZED) { ldn_state_set(c, ap ? LDN_AP : LDN_STATION); r = 0; }
    pthread_mutex_unlock(&c->lock); return r;
}
static uint32_t ldn_disconnect_locked(dm_ldn_context *c, int host, int reason) {
    if (c->state != (host ? LDN_CREATED : LDN_CONNECTED)) return LDN_INVALID_STATE;
    if (c->listener >= 0) { close(c->listener); c->listener = -1; }
    for (int i = 0; i < 8; ++i) ldn_peer_close(c, i);
    dm_ldn_network empty = {0}; c->reason = reason; ldn_network_set(c, &empty); ldn_state_set(c, host ? LDN_AP : LDN_STATION); ldn_wake(c); return 0;
}
uint32_t dm_ldn_disconnect(dm_ldn_context *c, int host) { if (!c) return LDN_INVALID_STATE; pthread_mutex_lock(&c->lock); uint32_t r = ldn_disconnect_locked(c, host, host ? 3 : 1); pthread_mutex_unlock(&c->lock); return r; }
uint32_t dm_ldn_close(dm_ldn_context *c, int ap) {
    if (!c) return LDN_INVALID_STATE; pthread_mutex_lock(&c->lock); uint32_t r = LDN_INVALID_STATE;
    if (c->state == (ap ? LDN_CREATED : LDN_CONNECTED)) ldn_disconnect_locked(c, ap, ap ? 3 : 1);
    if (c->state == (ap ? LDN_AP : LDN_STATION)) { ldn_state_set(c, LDN_INITIALIZED); r = 0; }
    pthread_mutex_unlock(&c->lock); return r;
}
uint32_t dm_ldn_scan(dm_ldn_context *c, dm_ldn_network *out, int *count, int max, const dm_ldn_filter *f, int channel) {
    if (count) *count = 0;
    if (!c || !out || !count || !f || max < 1 || f->flags & ~63u || ((f->flags & 16) && f->ssid.len > 32) || ((f->flags & 4) && f->type > 3) || channel < 0 || channel > 255) return LDN_INVALID;
    if (max > 24) max = 24; pthread_mutex_lock(&c->lock);
    if ((c->state != LDN_STATION && c->state != LDN_CONNECTED && c->state != LDN_CREATED) || c->scanning) { pthread_mutex_unlock(&c->lock); return LDN_INVALID_STATE; }
    dm_net_discovery_plan plan = {.target = c->transport.discovery, .count = 1};
    uint32_t host = ntohl(c->transport.discovery);
    if ((host == UINT32_MAX || (host & 0xf0000000u) == 0xe0000000u ||
         c->transport.discovery == (c->transport.address | ~c->transport.mask)) &&
        dm_net_discovery_prepare(c->transport.discovery, &plan)) { pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    dm_net_discovery_plan extra={0};
    if(!plan.automatic)dm_net_discovery_supplement(c->transport.discovery,&extra);
    c->scanning=1;c->room_count=0;c->scan_plan=plan;c->scan_supplement=extra;c->scan_next=0;c->scan_tx_failed=0;ldn_wake(c);
    // Keep the ordinary response window after the paced sweep, rather than
    // penalizing a console near the end of a larger (at most /20) subnet.
    struct timespec until=ldn_deadline(c->transport.scan_ms+dm_net_discovery_duration(&plan)+dm_net_discovery_duration(&extra));
    while (!c->stopping && !c->scan_tx_failed && (c->state == LDN_STATION || c->state == LDN_CONNECTED || c->state == LDN_CREATED))
        if (pthread_cond_timedwait(&c->changed, &c->lock, &until) == ETIMEDOUT) break;
    c->scan_plan.count=c->scan_supplement.count=0;
    if (c->scan_tx_failed) { c->scanning = 0; pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    if (c->stopping || (c->state != LDN_STATION && c->state != LDN_CONNECTED && c->state != LDN_CREATED)) { c->scanning = 0; pthread_mutex_unlock(&c->lock); return LDN_INVALID_STATE; }
    for (int i = 0; i < c->room_count && *count < max; ++i) if (dm_ldn_filter_match(&c->rooms[i].network, f, channel)) out[(*count)++] = c->rooms[i].network;
    ldn_trace("scan complete rooms=%d matched=%d", c->room_count, *count);
    c->scanning = 0; pthread_mutex_unlock(&c->lock); return 0;
}
uint32_t dm_ldn_create(dm_ldn_context *c, const dm_ldn_network_config *cfg, const dm_ldn_security_config *sec, const dm_ldn_user *user, const dm_ldn_security_parameter *parameter, int count, const dm_ldn_address *addresses) {
    if (cfg && sec) ldn_trace("create-config channel=%d max-nodes=%d version=%d security-mode=%u passphrase-size=%u private-count=%d", cfg->channel, cfg->max_nodes, cfg->version, sec->mode, sec->size, count);
    if (!c || !cfg || !sec || !user || cfg->max_nodes < 1 || cfg->max_nodes > 8 || cfg->version < 0 || cfg->channel < 0 || cfg->channel > 255 || sec->size > 64 || count < 0 || count > 8 || (count && !addresses) || !memchr(user->name, 0, 33)) return LDN_INVALID;
    pthread_mutex_lock(&c->lock); if (c->state != LDN_AP) { pthread_mutex_unlock(&c->lock); return LDN_INVALID_STATE; }
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    ldn_nonblock(fd); int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes); struct sockaddr_in a = ldn_endpoint(c->transport.address, c->transport.port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 8)) { ldn_trace("create-listener failed errno=%d", errno); close(fd); pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    dm_ldn_network n = {0}; n.id.intent = cfg->intent; n.max_nodes = cfg->max_nodes; n.security_mode = sec->mode;
    n.common.channel = cfg->channel ? cfg->channel : 6; n.common.link = 3; n.common.type = 2; n.common.ssid.len = 32;
    ldn_local_mac(c, n.common.mac); n.nodes[0] = ldn_node(c, user, cfg->version, 0); n.node_count = 1;
    arc4random_buf(n.id.session, 16); arc4random_buf(n.security, 16);
    if (parameter) { memcpy(n.id.session, parameter->session, 16); memcpy(n.security, parameter->server_random, 16); }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { n.common.ssid.text[i*2] = hex[n.id.session[i] >> 4]; n.common.ssid.text[i*2+1] = hex[n.id.session[i] & 15]; }
    n.accept_policy = c->network.accept_policy; n.advertise_size = c->network.advertise_size; memcpy(n.advertise, c->network.advertise, sizeof n.advertise);
    c->private_count = count; if (count) memcpy(c->private_addresses, addresses, count * sizeof *addresses);
    c->config = *cfg; c->listener = fd; c->reason = 0; memset(c->updates, 0, sizeof c->updates); ldn_network_set(c, &n); ldn_state_set(c, LDN_CREATED); ldn_wake(c);
    pthread_mutex_unlock(&c->lock); return 0;
}
uint32_t dm_ldn_connect(dm_ldn_context *c, const dm_ldn_network *network, const dm_ldn_security_config *security, const dm_ldn_user *user, int version, int option) {
    ldn_trace("connect-config version=%d option=%d network-valid=%d remote-version=%d", version, option, dm_ldn_network_valid(network), network ? network->nodes[0].version : -1);
    if (!c || !network || !security || !user || !dm_ldn_network_valid(network) || security->size > 64 || !memchr(user->name, 0, 33) || option < 0 || option > 1 || version < 0 || version > 32767) return LDN_INVALID;
    if (version < network->nodes[0].version) return LDN_VERSION_LOW; if (version > network->nodes[0].version) return LDN_VERSION_HIGH;
    // Node count can change between Scan and Connect. Admission uses the
    // listener's current roster, rather than rejecting a stale full snapshot.
    pthread_mutex_lock(&c->lock); if (c->state != LDN_STATION || c->peers[0].fd >= 0) { pthread_mutex_unlock(&c->lock); return LDN_INVALID_STATE; }
    struct sockaddr_in dest = ldn_endpoint(htonl(network->nodes[0].ip), 11452);
    for (int i = 0; i < c->room_count; ++i) if (!memcmp(&network->id, &c->rooms[i].network.id, sizeof network->id)) dest = c->rooms[i].endpoint;
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    ldn_nonblock(fd); struct sockaddr_in local = ldn_endpoint(c->transport.address, 0);
    if (bind(fd, (struct sockaddr *)&local, sizeof local)) { close(fd); pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    int connected = connect(fd, (struct sockaddr *)&dest, sizeof dest); int error = errno;
    if (connected && error != EINPROGRESS) { ldn_trace("connect-native failed errno=%d", error); close(fd); pthread_mutex_unlock(&c->lock); return LDN_NOT_FOUND; }
    // Complete connect outside the context lock. The descriptor has not been
    // published to the worker yet, so it cannot be reused underneath this wait.
    pthread_mutex_unlock(&c->lock); struct pollfd pollfd = {fd, POLLOUT, 0}; int ready; do { ready = poll(&pollfd, 1, (int)c->transport.connect_ms); } while (ready < 0 && errno == EINTR);
    socklen_t error_len = sizeof error; if (ready <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) || error) { ldn_trace("connect-complete failed ready=%d errno=%d socket-error=%d", ready, errno, error); close(fd); return ready == 0 ? LDN_TIMEOUT : LDN_CONNECT_FAILED; }
    pthread_mutex_lock(&c->lock); if (c->state != LDN_STATION || c->peers[0].fd >= 0) { close(fd); pthread_mutex_unlock(&c->lock); return LDN_INVALID_STATE; }
    c->network = *network; memset(c->updates, 0, sizeof c->updates); c->reason = 0;
    ldn_peer *p = &c->peers[0]; p->fd = fd; p->generation = ++c->generation; p->deadline = ldn_ms() + c->transport.connect_ms;
    dm_ldn_node node = ldn_node(c, user, version, 0);
    if (ldn_queue(c, 0, DM_LDN_CONNECT, &node, sizeof node)) { ldn_peer_close(c, 0); pthread_mutex_unlock(&c->lock); return LDN_CONNECT_FAILED; }
    ldn_wake(c); struct timespec until = ldn_deadline(c->transport.connect_ms); int timedout = 0;
    while (!c->stopping && c->state == LDN_STATION && p->fd >= 0) if (pthread_cond_timedwait(&c->changed, &c->lock, &until) == ETIMEDOUT) { timedout = 1; break; }
    uint32_t result = c->state == LDN_CONNECTED ? 0 : timedout ? LDN_TIMEOUT : LDN_REJECTED;
    if (result) ldn_peer_close(c, 0); pthread_mutex_unlock(&c->lock); return result;
}
uint32_t dm_ldn_get_network(dm_ldn_context *c, dm_ldn_network *network, dm_ldn_update *updates, int count) {
    if (!c || !network || count < 0 || count > 8 || (count && !updates)) return LDN_INVALID;
    pthread_mutex_lock(&c->lock); uint32_t result = LDN_INVALID_STATE;
    if (c->state == LDN_CREATED || c->state == LDN_CONNECTED) { *network = c->network; if (count) { memcpy(updates, c->updates, count * sizeof *updates); memset(c->updates, 0, count * sizeof *updates); } result = 0; }
    pthread_mutex_unlock(&c->lock); return result;
}
uint32_t dm_ldn_advertise(dm_ldn_context *c, const void *data, size_t size) {
    if (!c || size > 384 || (size && !data)) return LDN_INVALID; pthread_mutex_lock(&c->lock); uint32_t result = LDN_INVALID_STATE;
    if (c->state == LDN_AP || c->state == LDN_CREATED) { memset(c->network.advertise, 0, 384); if (size) memcpy(c->network.advertise, data, size); c->network.advertise_size = size; if (c->state == LDN_CREATED) { ldn_changed(c); ldn_sync(c); } result = 0; }
    pthread_mutex_unlock(&c->lock); return result;
}
uint32_t dm_ldn_policy(dm_ldn_context *c, int policy) {
    if (!c || policy < 0 || policy > 3) return LDN_INVALID; pthread_mutex_lock(&c->lock); uint32_t result = LDN_INVALID_STATE;
    if (c->state == LDN_AP || c->state == LDN_CREATED) { c->network.accept_policy = policy; if (c->state == LDN_CREATED) { ldn_changed(c); ldn_sync(c); } result = 0; }
    pthread_mutex_unlock(&c->lock); return result;
}
uint32_t dm_ldn_filter_add(dm_ldn_context *c, const uint8_t mac[6]) {
    if (!c || !mac) return LDN_INVALID; pthread_mutex_lock(&c->lock); uint32_t result = LDN_INVALID_STATE;
    if (c->state == LDN_AP || c->state == LDN_CREATED) { result = 0; if (!ldn_mac_in_list(mac, c->filters, c->filter_count)) { if (c->filter_count == 8) result = LDN_FULL; else memcpy(c->filters[c->filter_count++], mac, 6); } }
    pthread_mutex_unlock(&c->lock); return result;
}
uint32_t dm_ldn_filter_clear(dm_ldn_context *c) { if (!c) return LDN_INVALID_STATE; pthread_mutex_lock(&c->lock); uint32_t r = LDN_INVALID_STATE; if (c->state == LDN_AP || c->state == LDN_CREATED) { c->filter_count = 0; r = 0; } pthread_mutex_unlock(&c->lock); return r; }
uint32_t dm_ldn_reject(dm_ldn_context *c, uint32_t ip) {
    if (!c) return LDN_INVALID_STATE; pthread_mutex_lock(&c->lock); uint32_t result = c->state == LDN_CREATED ? LDN_NODE_NOT_FOUND : LDN_INVALID_STATE;
    if (c->state == LDN_CREATED) for (int i = 1; i < 8; ++i) if (c->peers[i].active && c->network.nodes[i].ip == ip) { ldn_drop(c, i); result = 0; break; }
    pthread_mutex_unlock(&c->lock); return result;
}

#ifndef DM_LDN_TEST
// Every operation that takes these locks uses WAIT, including brief getters:
// a getter arriving behind a Finalize writer can wait for an in-flight scan.
// Attach only creates a guest event and publishes an atomic lifetime ID; it
// takes no context lock and must retain the SDK callback-capable entry path.
// Readers retain the context for the complete operation. Finalize joins the
// worker only after in-flight scans/connects release their read lock. The
// worker notification never acquires this lock or invokes guest code.
static dm_ldn_context *g_ldn;
static pthread_rwlock_t g_ldn_lock = PTHREAD_RWLOCK_INITIALIZER;
static void ldn_unlock(int *held) { if (*held) pthread_rwlock_unlock(&g_ldn_lock); }
static int ldn_lock(int write) { dm_guest_host_wait_begin(); if (write) pthread_rwlock_wrlock(&g_ldn_lock); else pthread_rwlock_rdlock(&g_ldn_lock); dm_guest_host_wait_end(); return 1; }
#define LDN_USE() int ldn_lock_held __attribute__((cleanup(ldn_unlock))) = ldn_lock(0)
#define LDN_WRITE() int ldn_lock_held __attribute__((cleanup(ldn_unlock))) = ldn_lock(1)
static uint64_t g_ldn_events[64];
static void ldn_event(void *opaque) {
    (void)opaque;
    for (int i = 0; i < 64; ++i) {
        uint64_t id = __atomic_load_n(&g_ldn_events[i], __ATOMIC_ACQUIRE);
        if (id && !hle_signal_system_event_binding(id)) __atomic_compare_exchange_n(&g_ldn_events[i], &id, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}
static Result ldn_hook_result(const char *operation, Result result) {
    if (!ldn_trace_enabled() || (!result && (!strcmp(operation,"Info") || !strcmp(operation,"InfoUpdates")))) return result;
    ldn_trace("op=%s result=0x%08x state=%d reason=%d", operation, result,
              dm_ldn_state(g_ldn), dm_ldn_reason(g_ldn));
    return result;
}
static Result ldn_hook_info(dm_ldn_network *out, dm_ldn_update *updates, int count) {
    Result result = dm_ldn_get_network(g_ldn, out, updates, count);
    if (!result && ldn_trace_enabled()) {
        unsigned connected = 0, own = 0;
        for (int i = 0; i < 8; ++i) {
            if (out->nodes[i].connected) connected |= 1u << i;
            if (out->nodes[i].connected && out->nodes[i].ip == ntohl(g_ldn->transport.address)) own |= 1u << i;
        }
        ldn_trace("info nodes=%u max=%d connected-mask=0x%x own-mask=0x%x updates=%d host-version=%d", out->node_count, out->max_nodes, connected, own, count, out->nodes[0].version);
    }
    return ldn_hook_result("Info", result);
}
static int ldn_enabled(void) { const char *m = getenv("DM_NETWORK"); return m && (!strcmp(m, "ldn") || !strcmp(m, "auto")); }
HLE_FORCE_WAIT("_ZN2nn3ldn10InitializeEv", "native ldn_mitm control transport", Result, h_ldn_Init, void) { LDN_WRITE();
    if (!ldn_enabled()) return ldn_hook_result("InitDisabled", LDN_ERROR(22)); if (g_ldn) return ldn_hook_result("InitExisting",0);
    dm_ldn_transport t = {.port=11452, .discovery_port=11452, .scan_ms=1000, .connect_ms=4000};
    if (dm_net_ipconfig(&t.address, &t.mask, NULL, NULL, NULL)) return LDN_ERROR(16);
    t.discovery = t.address | ~t.mask; dm_net_discovery_plan plan;
    if (dm_net_discovery_prepare(t.discovery, &plan)) {
        LOG("[RED] LDN discovery unavailable (errno %d): no eligible bounded IPv4 LAN or valid explicit peer", dm_net_errno());
        return LDN_ERROR(22);
    }
    g_ldn = dm_ldn_start(&t, ldn_event, NULL); if (g_ldn) ldn_event(NULL); return ldn_hook_result("Init",g_ldn ? 0 : LDN_ERROR(16));
}
HLE_FORCE_WAIT("_ZN2nn3ldn8FinalizeEv", "stop native LDN workers and peers", void, h_ldn_Fini, void) { LDN_WRITE(); dm_ldn_context *c = g_ldn; g_ldn = NULL; for (int i = 0; i < 64; ++i) __atomic_store_n(&g_ldn_events[i], 0, __ATOMIC_RELEASE); dm_guest_host_wait_begin(); dm_ldn_stop(c); dm_guest_host_wait_end(); }
HLE_FORCE_WAIT("_ZN2nn3ldn8GetStateEv", "native LDN service state", int, h_ldn_State, void) { LDN_USE(); return dm_ldn_state(g_ldn); }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "real native LDN state event", void, h_ldn_Attach, void *ev) {
    if (!ev) return;
    hle_bind_system_event(ev, 1); uint64_t id = hle_system_event_binding(ev);
    for (int i = 0; i < 64; ++i) if (__atomic_load_n(&g_ldn_events[i], __ATOMIC_ACQUIRE) == id) return;
    for (int i = 0; i < 64; ++i) { uint64_t empty = 0; if (__atomic_compare_exchange_n(&g_ldn_events[i], &empty, id, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED) || empty == id) return; }
    die("LDN state event binding table full");
}
HLE_FORCE_WAIT("_ZN2nn3ldn11OpenStationEv", "native LDN station lifecycle", Result, h_ldn_OpenSt, void) { LDN_USE(); return ldn_hook_result("OpenSt", dm_ldn_open(g_ldn, 0)); }
HLE_FORCE_WAIT("_ZN2nn3ldn12CloseStationEv", "native LDN station lifecycle", Result, h_ldn_CloseSt, void) { LDN_USE(); return ldn_hook_result("CloseSt", dm_ldn_close(g_ldn, 0)); }
HLE_FORCE_WAIT("_ZN2nn3ldn15OpenAccessPointEv", "native LDN access point lifecycle", Result, h_ldn_OpenAp, void) { LDN_USE(); return ldn_hook_result("OpenAp", dm_ldn_open(g_ldn, 1)); }
HLE_FORCE_WAIT("_ZN2nn3ldn16CloseAccessPointEv", "native LDN access point lifecycle", Result, h_ldn_CloseAp, void) { LDN_USE(); return ldn_hook_result("CloseAp", dm_ldn_close(g_ldn, 1)); }
HLE_FORCE_WAIT("_ZN2nn3ldn10DisconnectEv", "close native LDN station session", Result, h_ldn_Disconnect, void) { LDN_USE(); return ldn_hook_result("Disconnect", dm_ldn_disconnect(g_ldn, 0)); }
HLE_FORCE_WAIT("_ZN2nn3ldn14DestroyNetworkEv", "close native LDN access point sessions", Result, h_ldn_Destroy, void) { LDN_USE(); return ldn_hook_result("Destroy", dm_ldn_disconnect(g_ldn, 1)); }
HLE_FORCE_WAIT("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "real UDP LDN discovery", Result, h_ldn_Scan, dm_ldn_network *o, int *n, int max, const dm_ldn_filter *f, int channel) { LDN_USE(); if (!f) return LDN_INVALID; dm_ldn_filter public_filter = *f; public_filter.flags &= 0x37; dm_guest_host_wait_begin(); Result r = dm_ldn_scan(g_ldn, o, n, max, &public_filter, channel); dm_guest_host_wait_end(); return ldn_hook_result("Scan", r); }
HLE_FORCE_WAIT("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "real filtered UDP LDN discovery", Result, h_ldn_ScanPrivate, dm_ldn_network *o, int *n, int max, const dm_ldn_filter *f, int channel) { LDN_USE(); dm_guest_host_wait_begin(); Result r = dm_ldn_scan(g_ldn, o, n, max, f, channel); dm_guest_host_wait_end(); return ldn_hook_result("ScanPrivate", r); }
HLE_FORCE_WAIT("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "native LDN access point creation", Result, h_ldn_Create, const dm_ldn_network_config *n, const dm_ldn_security_config *s, const dm_ldn_user *u) { LDN_USE(); return ldn_hook_result("Create", dm_ldn_create(g_ldn, n, s, u, NULL, 0, NULL)); }
HLE_FORCE_WAIT("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "native LDN private session and accept list", Result, h_ldn_CreatePrivate, const dm_ldn_network_config *n, const dm_ldn_security_config *s, const dm_ldn_security_parameter *p, const dm_ldn_user *u, int count, const dm_ldn_address *addresses) { LDN_USE(); if (!p) return LDN_INVALID; return ldn_hook_result("CreatePrivate", dm_ldn_create(g_ldn, n, s, u, p, count, addresses)); }
HLE_FORCE_WAIT("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "real TCP LDN station handshake", Result, h_ldn_Connect, const dm_ldn_network *n, const dm_ldn_security_config *s, const dm_ldn_user *u, int version, int option) { LDN_USE(); dm_guest_host_wait_begin(); Result r = dm_ldn_connect(g_ldn, n, s, u, version, option); dm_guest_host_wait_end(); return ldn_hook_result("Connect", r); }
HLE_FORCE_WAIT("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi", "SDK legacy LDN connect overload", Result, h_ldn_ConnectLegacy, const dm_ldn_network *n, const dm_ldn_security_config *s, const dm_ldn_user *u, int version) { LDN_USE(); dm_guest_host_wait_begin(); Result r = dm_ldn_connect(g_ldn, n, s, u, version, 1); dm_guest_host_wait_end(); return ldn_hook_result("ConnectLegacy", r); }
static Result ldn_connect_private(const dm_ldn_network_config *n, const dm_ldn_security_config *s, const dm_ldn_security_parameter *p, const dm_ldn_user *u, int version, int option) {
    if (!n || !p) return LDN_INVALID;
    dm_ldn_filter f = {.flags=35}; f.id.intent = n->intent; memcpy(f.id.session, p->session, 16); dm_ldn_network rooms[24]; int count;
    dm_guest_host_wait_begin(); Result r = dm_ldn_scan(g_ldn, rooms, &count, 24, &f, n->channel);
    if (!r) { r = LDN_NOT_FOUND; for (int i = 0; i < count; ++i) if (!memcmp(rooms[i].security, p->server_random, 16)) { r = dm_ldn_connect(g_ldn, &rooms[i], s, u, version, option); break; } }
    dm_guest_host_wait_end(); return r;
}
HLE_FORCE_WAIT("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "real private-session LDN discovery and handshake", Result, h_ldn_ConnectPrivate, const dm_ldn_network_config *n, const dm_ldn_security_config *s, const dm_ldn_security_parameter *p, const dm_ldn_user *u, int version, int option) { LDN_USE(); return ldn_hook_result("ConnectPrivate",ldn_connect_private(n, s, p, u, version, option)); }
HLE_FORCE_WAIT("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEi", "SDK legacy private LDN connect overload", Result, h_ldn_ConnectPrivateLegacy, const dm_ldn_network_config *n, const dm_ldn_security_config *s, const dm_ldn_security_parameter *p, const dm_ldn_user *u, int version) { LDN_USE(); return ldn_hook_result("ConnectPrivateLegacy",ldn_connect_private(n, s, p, u, version, 1)); }
HLE_FORCE_WAIT("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "read real LDN network snapshot", Result, h_ldn_Info, dm_ldn_network *o) { LDN_USE(); return ldn_hook_info(o, NULL, 0); }
HLE_FORCE_WAIT("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoEPNS0_16NodeLatestUpdateEi", "read and consume real LDN node changes", Result, h_ldn_InfoUpdates, dm_ldn_network *o, dm_ldn_update *u, int count) { LDN_USE(); return ldn_hook_info(o, u, count); }
HLE_FORCE_WAIT("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "actual adapter addresses for LDN game sockets", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) { LDN_USE(); if (!a || !m) return LDN_INVALID; dm_ldn_network n; Result r = dm_ldn_get_network(g_ldn, &n, NULL, 0); if (r) return ldn_hook_result("Ip",r); *a = ntohl(g_ldn->transport.address); *m = ntohl(g_ldn->transport.mask); return ldn_hook_result("Ip",0); }
HLE_FORCE_WAIT("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "read native LDN session identifiers", Result, h_ldn_Security, dm_ldn_security_parameter *o) { LDN_USE(); if (!o) return LDN_INVALID; dm_ldn_network n; Result r = dm_ldn_get_network(g_ldn, &n, NULL, 0); if (!r) { memcpy(o->server_random, n.security, 16); memcpy(o->session, n.id.session, 16); } return ldn_hook_result("Security", r); }
HLE_FORCE_WAIT("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "publish actual LDN advertise bytes", Result, h_ldn_Advert, const void *d, uint64_t size) { LDN_USE(); return ldn_hook_result("Advert", dm_ldn_advertise(g_ldn, d, size)); }
HLE_FORCE_WAIT("_ZN2nn3ldn19GetDisconnectReasonEv", "native LDN disconnect reason", int, h_ldn_Reason, void) { LDN_USE(); return dm_ldn_reason(g_ldn); }
HLE_FORCE_WAIT("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "native LDN MAC accept filter", Result, h_ldn_Add, uint64_t mac) { LDN_USE(); return ldn_hook_result("Add", dm_ldn_filter_add(g_ldn, (uint8_t *)&mac)); }
HLE_FORCE_WAIT("_ZN2nn3ldn20AddAcceptFilterEntryERKNS0_8NodeInfoE", "native LDN node accept filter", Result, h_ldn_AddNode, const dm_ldn_node *node) { LDN_USE(); return node ? dm_ldn_filter_add(g_ldn, node->mac) : LDN_INVALID; }
HLE_FORCE_WAIT("_ZN2nn3ldn17ClearAcceptFilterEv", "clear native LDN accept filter", Result, h_ldn_Clear, void) { LDN_USE(); return ldn_hook_result("Clear", dm_ldn_filter_clear(g_ldn)); }
HLE_FORCE_WAIT("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "enforce native LDN accept policy", Result, h_ldn_Policy, int policy) { LDN_USE(); return ldn_hook_result("Policy", dm_ldn_policy(g_ldn, policy)); }
HLE_FORCE_WAIT("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "disconnect actual native LDN member", Result, h_ldn_Reject, uint32_t ip) { LDN_USE(); return ldn_hook_result("Reject", dm_ldn_reject(g_ldn, ip)); }
HLE_FORCE_WAIT("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", "read native LDN network configuration", Result, h_ldn_Config, dm_ldn_network_config *o) { LDN_USE(); if (!o) return LDN_INVALID; dm_ldn_network n; Result r = dm_ldn_get_network(g_ldn, &n, NULL, 0); if (!r) { memset(o, 0, sizeof *o); o->intent = n.id.intent; o->channel = n.common.channel; o->max_nodes = n.max_nodes; o->version = n.nodes[0].version; } return ldn_hook_result("Config", r); }
#endif
