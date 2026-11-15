// Which transport the dm_sk_ entry points take. Run twice (make net-dispatch): "off" with DM_NET unset, where a loopback datagram
// goes through the BSD service's own local transport and no host socket exists; "on" with DM_NET=1, where the same calls are the
// host's sockets, a loopback datagram crosses the host's stack, and the error numbers are Horizon's (Linux's).
#include "../hle_net.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

__thread _Atomic int dm_host_work;
void dm_guest_sleep_ns(int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; pthread_cond_timedwait_relative_np(c, m, &ts); }
uint64_t guest_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec; }
int dm_detclock_enabled(void) { return 0; }

static void address(uint8_t out[16], uint32_t ip, unsigned port) {   // a game's SockAddrIn: {sin_len 16, family 2, port, address}
    uint8_t b[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip}; memcpy(out, b, sizeof b);
}
static unsigned port_of(const uint8_t a[16]) { return (unsigned)a[2] << 8 | a[3]; }
// Whether the host's own stack has something bound to this UDP port on loopback
static int host_port_in_use(unsigned port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0); assert(s >= 0);
    struct sockaddr_in a = {0}; a.sin_len = sizeof a; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(0x7f000001);
    int busy = bind(s, (struct sockaddr *)&a, sizeof a) != 0; close(s); return busy;
}

int main(int argc, char **argv) {
    int on = argc > 1 && !strcmp(argv[1], "on");
    assert(argc > 1 && (on || !strcmp(argv[1], "off")));
    assert(dm_net_online() == on);   // the environment the Makefile sets agrees with the mode asked for

    uint8_t a[16], b[16], from[16]; char data[64]; unsigned n;
    int s = dm_sk_socket(2, 2, 0), r = dm_sk_socket(2, 2, 0); assert(s >= 0 && r >= 0);
    address(a, 0x7f000001, 0); assert(dm_sk_bind(r, a, 16) == 0);
    n = 16; assert(dm_sk_name(r, b, &n, 0) == 0 && n == 16 && port_of(b));
    address(b, 0x7f000001, port_of(b));
    // The round trip both modes owe the game
    assert(dm_sk_sendto(s, "ping", 4, 0, b, 16) == 4);
    sk_pollfd p = {r, SK_POLLIN, 0}; assert(dm_sk_poll(&p, 1, 1000) == 1 && (p.revents & SK_POLLIN));
    n = 16; assert(dm_sk_recvfrom(r, data, sizeof data, 0, from, &n) == 4 && !memcmp(data, "ping", 4) && n == 16 && from[1] == 2);
    // Where it went: the local transport has no host socket behind its descriptor, the host path is one. Which table numbers the error is the other tell.
    assert(host_port_in_use(port_of(b)) == on);
    assert(dm_sk_fcntl(r, 4, SK_NONBLOCK) == 0);
    n = 16; assert(dm_sk_recvfrom(r, data, sizeof data, 0, from, &n) == -1);
    assert(dm_sk_errno() == (on ? 11 : SK_EAGAIN));   // Linux's EAGAIN under DM_NET=1; the service's own FreeBSD number (35) without it
    assert(dm_sk_close(s) == 0 && dm_sk_close(r) == 0);
    assert(host_port_in_use(port_of(b)) == 0);
    // A remote address is unreachable offline; under DM_NET=1 it is the host's own route, which this test does not use (no packet leaves the machine)
    if (!on) { uint8_t remote[16]; address(remote, 0x08080808, 53); s = dm_sk_socket(2, 2, 0); assert(dm_sk_sendto(s, "dns", 3, 0, remote, 16) == -1 && dm_sk_errno() == SK_ENETDOWN); assert(dm_sk_close(s) == 0); }
    printf("ok t_net_dispatch %s\n", argv[1]);
    return 0;
}
