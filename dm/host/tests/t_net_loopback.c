// Offline UDP startup self-test and datagram semantics, without opening any host socket.
#include "../hle_net.c"
__thread _Atomic int dm_host_work;
#include <assert.h>
#include <stdio.h>
#include <time.h>

void dm_guest_sleep_ns(int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; pthread_cond_timedwait_relative_np(c, m, &ts); }
uint64_t guest_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
int dm_detclock_enabled(void) { return 0; }

static void address(uint8_t out[16], uint32_t ip, unsigned port) {
    uint8_t b[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip}; memcpy(out, b, sizeof b);
}
static unsigned port_of(const uint8_t a[16]) { return (unsigned)a[2] << 8 | a[3]; }
static int bound(uint32_t ip, uint8_t a[16]) {
    int s = h_nsk_Socket(2, 2, 0); assert(s >= 0); address(a, ip, 0); assert(h_nsk_Bind(s, a, 16) == 0);
    unsigned n = 16; assert(h_nsk_GetSockName(s, a, &n) == 0 && n == 16 && port_of(a)); return s;
}
typedef struct { int fd; _Atomic int entered; long result; int error; } wait_arg;
static void *receiver(void *p) {
    wait_arg *a = p; uint32_t value; atomic_store(&a->entered, 1); a->result = h_sk_Recv(a->fd, &value, sizeof value, 0); a->error = h_sk_GetLastError(); return NULL;
}
static void *delayed_sender(void *p) {
    uint8_t *addr = p; dm_guest_sleep_ns(20000000); int s = h_nsk_Socket(2, 2, 17); assert(s >= 0);
    assert(dm_sk_sendto(s, "ping", 4, 0, addr, 16) == 4); assert(h_sk_Close(s) == 0); return NULL;
}

int main(void) {
    uint8_t a[16], b[16], from[16]; char data[64] = {0}; unsigned n;
    int event = h_sk_EventFd(0, 6); assert(event == 0);
    int self = bound(0, a); assert(self == 1); unsigned port = port_of(a); address(a, 0x7f000001, port);
    // RakNet's offline startup: two UDP descriptors, ANY/loopback bind, four bytes sent to the sender itself.
    int other = bound(0x7f000001, b); assert(other == 2);
    assert(dm_sk_sendto(self, "test", 4, 0, a, 16) == 4);
    n = sizeof from; assert(h_sk_RecvFrom(self, data, sizeof data, SK_MSG_DONTWAIT, from, &n) == 4 && !memcmp(data, "test", 4));
    assert(n == 16 && from[4] == 127 && port_of(from) == port);
    assert(h_sk_Recv(self, data, sizeof data, SK_MSG_DONTWAIT) == -1 && h_sk_GetLastError() == SK_EAGAIN);

    // Both local endpoints can exchange datagrams. Peek leaves the entire packet queued; a short receive consumes the remainder.
    assert(dm_sk_sendto(self, "abcdef", 6, 0, b, 16) == 6);
    assert(h_sk_Recv(other, data, 2, SK_MSG_PEEK) == 2 && !memcmp(data, "ab", 2));
    assert(h_sk_Recv(other, data, 3, 0) == 3 && !memcmp(data, "abc", 3));
    assert(h_sk_Recv(other, data, sizeof data, SK_MSG_DONTWAIT) == -1 && h_sk_GetLastError() == SK_EAGAIN);
    assert(dm_sk_sendto(self, NULL, 0, 0, b, 16) == 0);
    sk_pollfd poll = {other, SK_POLLIN, 0}; assert(h_sk_Poll(&poll, 1, 0) == 1); assert(h_sk_Recv(other, data, sizeof data, 0) == 0);

    // Connect filters the incoming peer, and Send/Recv use the same queue. Fcntl flags use the guest ABI, not Darwin's O_NONBLOCK.
    assert(h_nsk_Connect(self, b, 16) == 0 && h_nsk_Connect(other, a, 16) == 0);
    assert(h_nsk_Send(self, "peer", 4, 0) == 4 && h_nsk_Recv(other, data, sizeof data, 0) == 4);
    assert(h_nsk_Fcntl(other, 4, SK_NONBLOCK) == 0 && h_nsk_Fcntl(other, 3, 0) == SK_NONBLOCK);
    assert(h_nsk_Recv(other, data, sizeof data, 0) == -1 && h_sk_GetLastError() == SK_EAGAIN);
    n = 16; assert(h_nsk_GetPeerName(other, from, &n) == 0 && !memcmp(from, a, 16));
    int stranger = h_nsk_Socket(2, 2, 0); assert(stranger >= 0 && dm_sk_sendto(stranger, "drop", 4, 0, b, 16) == 4);
    assert(h_nsk_Recv(other, data, sizeof data, 0) == -1 && h_sk_GetLastError() == SK_EAGAIN); assert(h_sk_Close(stranger) == 0);

    // Remote traffic and TCP never become host network activity.
    uint8_t remote[16]; address(remote, 0x08080808, 53);
    assert(dm_sk_sendto(self, "dns", 3, 0, remote, 16) == -1 && h_sk_GetLastError() == SK_ENETDOWN);
    assert(h_nsk_Connect(other, remote, 16) == -1 && h_sk_GetLastError() == SK_ENETDOWN);
    assert(h_nsk_Socket(2, 1, 0) == -1 && h_nsk_Socket(10, 2, 0) == -1);
    assert(dm_sk_sendto(self, data, SK_UDP_MAX + 1, 0, a, 16) == -1 && h_sk_GetLastError() == SK_EMSGSIZE);

    // Poll and Select observe UDP and EventFd together, including an arrival after the wait starts.
    assert(h_nsk_Fcntl(other, 4, 0) == 0); pthread_t thread;
    assert(pthread_create(&thread, NULL, delayed_sender, b) == 0);
    // Disconnect the receiver so a packet from a newly created endpoint is eligible.
    uint8_t unspec[16] = {0}; assert(h_nsk_Connect(other, unspec, 16) == 0);
    assert(h_sk_Poll(&poll, 1, 1000) == 1 && poll.revents == SK_POLLIN); pthread_join(thread, NULL);
    assert(h_sk_EventFdWrite(event, 1) == 0);
    uint64_t rs[16] = {0}; rs[0] = (1ull << event) | (1ull << other); int64_t zero[2] = {0, 0};
    assert(h_sk_Select(other + 1, rs, NULL, NULL, zero) == 2 && rs[0] == ((1ull << event) | (1ull << other)));
    assert(h_sk_Recv(other, data, sizeof data, 0) == 4 && !memcmp(data, "ping", 4));
    unsigned long count; assert(h_sk_EventFdRead(event, &count) == 0 && count == 1);
    count = 7; assert(h_nsk_Write(event, &count, sizeof count) == sizeof count);
    count = 0; assert(h_nsk_Read(event, &count, sizeof count) == sizeof count && count == 7);

    // Excess datagrams are dropped at the receiver; bounded queue pressure never exposes an unbounded allocation or blocks a sender.
    for (unsigned i = 0; i < SK_PACKET_MAX + 10; i++) assert(h_nsk_Send(self, "q", 1, 0) == 1);
    unsigned received = 0;
    while (h_sk_Recv(other, data, sizeof data, SK_MSG_DONTWAIT) >= 0) received++;
    assert(received == SK_PACKET_MAX && h_sk_GetLastError() == SK_EAGAIN);

    // A closed receiver wakes with EBADF even when its descriptor number is immediately reused.
    wait_arg arg = {.fd = other}; assert(pthread_create(&thread, NULL, receiver, &arg) == 0);
    while (!atomic_load(&arg.entered)) dm_guest_sleep_ns(1000000);
    dm_guest_sleep_ns(20000000); assert(h_sk_Close(other) == 0); int reused = h_nsk_Socket(2, 2, 0); assert(reused == other);
    pthread_join(thread, NULL); assert(arg.result == -1 && arg.error == SK_EBADF);

    // Receive timeouts terminate a blocked wait; unsupported options are errors, not fictitious successful changes.
    int64_t timeout[2] = {0, 20000}; assert(h_nsk_SetSockOpt(reused, 0xffff, 0x1006, timeout, sizeof timeout) == 0);
    uint64_t start = guest_ns(); assert(h_sk_Recv(reused, data, sizeof data, 0) == -1 && h_sk_GetLastError() == SK_EAGAIN && guest_ns() - start >= 15000000);
    int v = 1; assert(h_nsk_SetSockOpt(reused, 0xffff, 0x12345, &v, sizeof v) == -1);
    assert(h_sk_Close(reused) == 0 && h_sk_Close(self) == 0 && h_sk_Close(event) == 0);

    // SO_REUSEADDR permits a wildcard/specific-address overlap. Delivery chooses the more specific local binding, not whichever fd was allocated first.
    int wildcard = bound(0, a), specific = h_nsk_Socket(2, 2, 0); assert(specific >= 0);
    v = 1; assert(h_nsk_SetSockOpt(wildcard, 0xffff, 4, &v, sizeof v) == 0 && h_nsk_SetSockOpt(specific, 0xffff, 4, &v, sizeof v) == 0);
    address(b, 0x7f000001, port_of(a)); assert(h_nsk_Bind(specific, b, 16) == 0);
    assert(dm_sk_sendto(wildcard, "specific", 8, 0, b, 16) == 8);
    assert(h_sk_Recv(specific, data, sizeof data, SK_MSG_DONTWAIT) == 8 && !memcmp(data, "specific", 8));
    assert(h_sk_Recv(wildcard, data, sizeof data, SK_MSG_DONTWAIT) == -1 && h_sk_GetLastError() == SK_EAGAIN);
    int clash = h_nsk_Socket(2, 2, 0); assert(clash >= 0 && h_nsk_Bind(clash, b, 16) == -1 && h_sk_GetLastError() == SK_EADDRINUSE);
    assert(h_sk_Close(clash) == 0 && h_sk_Close(specific) == 0 && h_sk_Close(wildcard) == 0);
    puts("ok t_net_loopback"); return 0;
}
