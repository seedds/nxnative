// Network-mode nn::socket transport (DM_NETWORK=lan, hle_net_backend.c) through the game's own entry points in hle_net.c, on real loopback sockets.
// Offline behaviour without DM_NETWORK is t_net_loopback's and t_net_eventfd's, unchanged.
#include "../hle_net.c"
__thread _Atomic int dm_host_work;
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void dm_guest_sleep_ns(int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; pthread_cond_timedwait_relative_np(c, m, &ts); }
uint64_t guest_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
int dm_detclock_enabled(void) { return 0; }
// The guest core is released around every host wait, and the calls balance
static _Atomic int g_wait_depth, g_wait_entries;
void dm_thread_native_wait(int entering) {
    if (entering) { atomic_fetch_add(&g_wait_depth, 1); atomic_fetch_add(&g_wait_entries, 1); }
    else assert(atomic_fetch_sub(&g_wait_depth, 1) > 0);
}
// Records the spans the transport prepares before the kernel writes guest memory
static _Atomic(void *) g_prep_addr; static _Atomic size_t g_prep_len; static _Atomic int g_prep_count;
void dm_guest_write_prepare(void *addr, size_t len) { atomic_store(&g_prep_addr, addr); atomic_store(&g_prep_len, len); atomic_fetch_add(&g_prep_count, 1); }

static int checks, failed;
#define CHECK(c, what) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); } } while (0)
static void address(uint8_t out[16], uint32_t ip, unsigned port) {   // Horizon's sockaddr_in: {u8 len, u8 family, u16 port, u32 address}, network order
    uint8_t b[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip}; memcpy(out, b, sizeof b);
}
static unsigned port_of(const uint8_t a[16]) { return (unsigned)a[2] << 8 | a[3]; }
static uint64_t ms_now(void) { return guest_ns() / 1000000; }
static int bound(int type, uint8_t a[16]) {
    int s = h_sk_Socket(2, type, 0); address(a, 0x7f000001, 0);
    unsigned n = 16;
    if (s < 0 || h_sk_Bind(s, a, 16) || h_sk_GetSockName(s, a, &n) || n != 16) return -1;
    return s;
}
typedef struct { int fd; long result; int error; uint64_t took; } wait_arg;
static void *blocked_recv(void *p) {
    wait_arg *a = p; char b[8]; uint64_t t0 = ms_now();
    a->result = h_sk_Recv(a->fd, b, sizeof b, 0); a->error = h_sk_GetLastError(); a->took = ms_now() - t0; return NULL;
}
static void *late_event(void *p) { dm_guest_sleep_ns(30000000); h_sk_EventFdWrite((int)(intptr_t)p, 1); return NULL; }

int main(void) {
    setenv("DM_NETWORK", "lan", 1);
    CHECK(dm_net_enabled(), "DM_NETWORK=lan turns the transport on");
    uint8_t a[16] = {0}, b[16] = {0}, from[16] = {0}; char data[64]; unsigned n;

    // EventFd stays the offline service's, below the transport's range
    int event = h_sk_EventFd(0, 6);
    CHECK(event >= 0 && event < DM_NET_FD_BASE, "EventFd keeps a small offline descriptor");

    // UDP: Horizon's address layout both ways, data and source, and Linux errno numbers
    int u = bound(2, a), v = bound(2, b);
    assert(dm_net_owns(u) && dm_net_owns(v));   // the rest indexes fd_sets with them
    CHECK(u >= DM_NET_FD_BASE && v >= DM_NET_FD_BASE && u != v, "network sockets start at DM_NET_FD_BASE");
    CHECK(a[0] == 16 && a[1] == 2 && a[4] == 127 && port_of(a), "GetSockName writes {16, AF_INET, port, 127.0.0.1}");
    CHECK(h_nsk_SendTo(u, "ping", 4, 0, b, 16) == 4, "SendTo another loopback socket");
    n = sizeof from; memset(data, 0, sizeof data); atomic_store(&g_prep_count, 0);
    long got = h_sk_RecvFrom(v, data, sizeof data, 0, from, &n);
    CHECK(got == 4 && !memcmp(data, "ping", 4), "RecvFrom gets the datagram");
    CHECK(n == 16 && from[0] == 16 && from[1] == 2 && port_of(from) == port_of(a), "RecvFrom reports the sender in Horizon's layout");
    CHECK(atomic_load(&g_prep_count) >= 1 && atomic_load(&g_prep_addr) == data && atomic_load(&g_prep_len) == sizeof data, "the receive buffer is prepared before the kernel writes it");
    uint8_t short_from[4]; n = sizeof short_from; h_nsk_SendTo(u, "x", 1, 0, b, 16);
    CHECK(h_sk_RecvFrom(v, data, sizeof data, 0, short_from, &n) == 1 && n == 16 && short_from[1] == 2, "a short address buffer gets its capacity and the full length");
    CHECK(h_sk_Recv(v, data, sizeof data, 0x80) == -1 && h_sk_GetLastError() == 11 && dm_sk_errno() == 11, "an empty MSG_DONTWAIT receive is EAGAIN 11 (Horizon's number, not Darwin's 35)");
    uint8_t bad[16]; address(bad, 0x7f000001, 1); bad[1] = 0;
    CHECK(h_nsk_SendTo(u, "x", 1, 0, bad, 16) == -1 && h_sk_GetLastError() == 97, "a family other than AF_INET is EAFNOSUPPORT 97");
    uint8_t zero_len[16]; memcpy(zero_len, b, 16); zero_len[0] = 0;
    CHECK(h_nsk_SendTo(u, "z", 1, 0, zero_len, 16) == 1, "sin_len 0 is accepted, as hand-written game code leaves it");
    CHECK(h_sk_Recv(v, data, sizeof data, 0) == 1, "and arrives");

    // Socket options: Horizon's numbers translated, SO_NOSIGPIPE kept on, SO_TYPE / SO_ERROR in Horizon's values, getsockopt output prepared
    int one = 1, zero = 0, value = -1; unsigned len = sizeof value;
    CHECK(h_sk_SetSockOpt(u, 0xffff, 0x20, &one, sizeof one) == 0, "SO_BROADCAST (0x20)");
    CHECK(h_sk_SetSockOpt(u, 0xffff, 0x800, &zero, sizeof zero) == 0, "SO_NOSIGPIPE (Horizon 0x800) is accepted");
    len = sizeof value; atomic_store(&g_prep_count, 0);
    CHECK(h_sk_GetSockOpt(u, 0xffff, 0x800, &value, &len) == 0 && value != 0, "SO_NOSIGPIPE stays on after the guest clears it");
    CHECK(atomic_load(&g_prep_count) == 1 && atomic_load(&g_prep_addr) == &value, "the option output is prepared before getsockopt writes it");
    len = sizeof value; CHECK(h_sk_GetSockOpt(u, 0xffff, 0x1008, &value, &len) == 0 && value == 2, "SO_TYPE is SOCK_DGRAM 2");
    len = sizeof value; CHECK(h_sk_GetSockOpt(u, 0xffff, 0x1007, &value, &len) == 0 && value == 0, "SO_ERROR 0");
    CHECK(h_sk_SetSockOpt(u, 0xffff, 0x9999, &one, sizeof one) == -1 && h_sk_GetLastError() == 92, "an unknown option is ENOPROTOOPT 92");
    CHECK(h_sk_SetSockOpt(u, 0xffff, 0x1007, &one, sizeof one) == -1 && h_sk_GetLastError() == 92, "SO_ERROR is read-only");

    // SO_RCVTIMEO bounds a blocking receive (u32 milliseconds, then the 16-byte TimeVal)
    uint32_t timeout = 60; uint64_t t0 = ms_now();
    CHECK(h_sk_SetSockOpt(v, 0xffff, 0x1006, &timeout, sizeof timeout) == 0, "SO_RCVTIMEO as u32 milliseconds");
    CHECK(h_sk_Recv(v, data, sizeof data, 0) == -1 && h_sk_GetLastError() == 11 && ms_now() - t0 >= 50, "a timed-out receive is EAGAIN 11 after the timeout");
    int64_t tv[2] = {0, 0}; len = sizeof tv;
    CHECK(h_sk_GetSockOpt(v, 0xffff, 0x1006, tv, &len) == 0 && len == 16 && tv[0] == 0 && tv[1] == 60000, "SO_RCVTIMEO reads back as a TimeVal");
    int64_t none[2] = {0, 0};
    CHECK(h_sk_SetSockOpt(v, 0xffff, 0x1006, none, sizeof none) == 0, "a zero TimeVal clears the timeout");

    // Fcntl: O_NONBLOCK is Horizon's 0x800
    CHECK(h_sk_Fcntl(v, 4, 0x802) == 0 && (h_sk_Fcntl(v, 3, 0) & 0x800), "F_SETFL O_RDWR | O_NONBLOCK, F_GETFL reports 0x800");
    CHECK(h_sk_Recv(v, data, sizeof data, 0) == -1 && h_sk_GetLastError() == 11, "a non-blocking socket's empty receive is EAGAIN");
    CHECK(h_sk_Fcntl(v, 4, 0) == 0 && !(h_sk_Fcntl(v, 3, 0) & 0x800), "F_SETFL 0 makes it blocking again");

    // Close on another thread ends a blocked receive with EBADF, without waiting for data
    wait_arg w = {.fd = v}; pthread_t th; atomic_store(&g_wait_entries, 0);
    pthread_create(&th, NULL, blocked_recv, &w); dm_guest_sleep_ns(50000000);
    CHECK(h_sk_Close(v) == 0, "Close while another thread receives");
    pthread_join(th, NULL);
    CHECK(w.result == -1 && w.error == 9 && w.took < 1000, "the blocked receive ends with EBADF 9 within a slice");
    CHECK(atomic_load(&g_wait_entries) > 0 && atomic_load(&g_wait_depth) == 0, "the wait released the guest core and balanced it");
    CHECK(h_sk_Recv(v, data, sizeof data, 0x80) == -1 && h_sk_GetLastError() == 9, "a closed descriptor is EBADF");

    // Poll and Select: network descriptors, POLLNVAL for a closed one, and a set mixing an EventFd with a socket wakes on either
    v = bound(2, b); assert(dm_net_owns(v)); h_nsk_SendTo(u, "p", 1, 0, b, 16);
    sk_pollfd p[3] = {{v, 1, 0}, {u, 1, 0}, {event, 1, 0}};
    CHECK(h_sk_Poll(p, 3, 1000) == 1 && (p[0].revents & 1) && !p[1].revents && !p[2].revents, "Poll: the socket with data is readable, nothing else");
    uint64_t rs[16] = {0}; rs[v >> 6] |= 1ull << (v & 63); rs[u >> 6] |= 1ull << (u & 63); int64_t stv[2] = {1, 0};
    CHECK(h_sk_Select(1024, rs, NULL, NULL, stv) == 1 && (rs[v >> 6] >> (v & 63) & 1) && !(rs[u >> 6] >> (u & 63) & 1), "Select: the same answer in an fd_set");
    h_sk_Recv(v, data, sizeof data, 0);
    pthread_create(&th, NULL, late_event, (void *)(intptr_t)event); t0 = ms_now();
    p[0] = (sk_pollfd){v, 1, 0}; p[1] = (sk_pollfd){event, 1, 0};
    int ready = h_sk_Poll(p, 2, 2000); pthread_join(th, NULL);
    CHECK(ready == 1 && (p[1].revents & 1) && !p[0].revents && ms_now() - t0 < 1000, "a mixed Poll wakes for the EventFd");
    unsigned long counter; h_sk_EventFdRead(event, &counter);
    int dead = h_sk_Socket(2, 2, 0); h_sk_Close(dead);
    p[0] = (sk_pollfd){dead, 1, 0};
    CHECK(h_sk_Poll(p, 1, 0) == 1 && p[0].revents == 0x20, "a closed descriptor polls POLLNVAL");
    t0 = ms_now(); p[0] = (sk_pollfd){v, 1, 0};
    CHECK(h_sk_Poll(p, 1, -1) == 0 && ms_now() - t0 < 1000, "an endless Poll returns periodically, as the offline service's does");

    // TCP: listen, a blocking connect, accept, data both ways, a peer that closed gives EPIPE 32 instead of a process-ending SIGPIPE
    uint8_t la[16]; int l = bound(1, la);
    CHECK(l >= DM_NET_FD_BASE && h_sk_Listen(l, 4) == 0, "TCP listen");
    int c = h_sk_Socket(2, 1, 0);
    CHECK(h_sk_Connect(c, la, 16) == 0, "a blocking connect completes");
    n = sizeof from; int s = h_sk_Accept(l, from, &n);
    CHECK(s >= DM_NET_FD_BASE && n == 16 && from[1] == 2 && from[4] == 127, "Accept reports the peer in Horizon's layout");
    len = sizeof value; CHECK(h_sk_GetSockOpt(s, 0xffff, 0x1008, &value, &len) == 0 && value == 1, "SO_TYPE is SOCK_STREAM 1");
    CHECK(h_sk_SetSockOpt(c, 6, 1, &one, sizeof one) == 0, "TCP_NODELAY at IPPROTO_TCP (6)");
    CHECK(h_sk_Send(c, "hello", 5, 0) == 5, "TCP send");
    CHECK(h_sk_Recv(s, data, 5, 0x40) == 5 && !memcmp(data, "hello", 5), "MSG_WAITALL gets the whole message");
    n = sizeof from; CHECK(h_sk_GetPeerName(c, from, &n) == 0 && port_of(from) == port_of(la), "GetPeerName");
    CHECK(h_sk_Close(s) == 0, "the server closes");
    dm_guest_sleep_ns(20000000);
    long r1 = h_sk_Send(c, "a", 1, 0); dm_guest_sleep_ns(20000000); long r2 = h_sk_Send(c, "b", 1, 0); int e2 = h_sk_GetLastError();
    CHECK(r1 == 1 || r1 == -1, "a send after the peer closed returns");
    CHECK(r2 == -1 && (e2 == 32 || e2 == 104), "the next is EPIPE 32 (or ECONNRESET 104), and the process lives");
    uint8_t refused[16]; memcpy(refused, la, 16); h_sk_Close(l); h_sk_Close(c);
    c = h_sk_Socket(2, 1, 0);
    CHECK(h_sk_Connect(c, refused, 16) == -1 && h_sk_GetLastError() == 111, "a refused connect is ECONNREFUSED 111");
    h_sk_Close(c);
    int nb = h_sk_Socket(2, 1 | 0x20000000, 0);
    CHECK(nb >= DM_NET_FD_BASE && (h_sk_Fcntl(nb, 3, 0) & 0x800), "SOCK_NONBLOCK makes a non-blocking socket");
    uint8_t na[16]; address(na, 0x7f000001, 0); n = 16;
    CHECK(!h_sk_Bind(nb, na, 16) && !h_sk_GetSockName(nb, na, &n) && !h_sk_Listen(nb, 1), "a non-blocking listener");
    c = h_sk_Socket(2, 1, 0); CHECK(h_sk_Connect(c, na, 16) == 0, "connect to it");
    sk_pollfd lp = {nb, 1, 0}; h_sk_Poll(&lp, 1, 1000);   // a non-blocking accept waits for the connection to reach the queue
    n = sizeof from; s = h_sk_Accept(nb, from, &n);
    CHECK(s >= DM_NET_FD_BASE && (h_sk_Fcntl(s, 3, 0) & 0x800), "the accepted socket inherits O_NONBLOCK, as FreeBSD gives");
    h_sk_Close(s); h_sk_Close(c);
    h_sk_Close(nb);
    CHECK(h_sk_Socket(2, 3, 0) == -1 && h_sk_GetLastError() == 94, "SOCK_RAW is ESOCKTNOSUPPORT 94");

    // nifm: the on-hold sequence is the offline one; the answer then follows the host interface
    h_nifm_Submit();
    CHECK(!h_nifm_Available(), "unavailable while the request is on hold");
    CHECK(h_nifm_OnHold(), "the request is on hold at the first query");
    CHECK(!h_nifm_OnHold(), "and settled at the next");
    uint32_t ip = 0, want = 0; int have = !dm_net_ipconfig(&want, NULL);
    CHECK(h_nifm_Available() == have, "available exactly when the host has an interface");
    CHECK((h_nifm_GetIp(&ip) == RES_OK) == have && ip == want, "GetCurrentPrimaryIpAddress is that interface's address");
    h_nifm_Cancel();
    CHECK(!h_nifm_Available(), "a cancelled request is unavailable");

    h_sk_Close(u); h_sk_Close(v); h_sk_Close(event);
    CHECK(atomic_load(&g_wait_depth) == 0, "no wait left the guest core released");
    printf("t_net: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
