// The socket layer with DM_NET=1 (hle_net.c): a UDP socket driven through the game's own nn::socket calls — Socket, Bind,
// GetSockName, SendTo, Poll, RecvFrom, GetLastError, the Fcntl non-blocking round trip, Select, Close — and the broadcast
// replication to DM_NET_PEERS (the Tailscale path: a tailnet carries no L2 broadcast). The host functions are looked up in
// the swforce registrations, as the game calls them.
#include "../hle_net.c"
#include <mach-o/getsect.h>
#include <string.h>
#include <time.h>
#include <signal.h>

void dm_guest_sleep_ns(int64_t ns) { struct timespec t = {0, ns > 0 && ns < 1000000000 ? (long)ns : 100000000}; nanosleep(&t, NULL); }   // hle_thread.c is not linked
void hle_bind_system_event(void *ev, int on) { (void)ev; (void)on; }
void dm_applet_enter(const char *name) { (void)name; }   // hle_offline.c: the error stand-ins hold the game out of focus
void dm_applet_leave(void) { }

static int checks, fails;
static volatile sig_atomic_t poll_signal_seen;
static void poll_interrupted(int sig) { (void)sig; poll_signal_seen = 1; }
struct poll_waker { pthread_t target; _Atomic int done, error; };
static void *interrupt_poll(void *arg) {
    struct poll_waker *w = arg;
    while (!atomic_load(&w->done)) {
        usleep(10000);
        if (!atomic_load(&w->done)) { int err = pthread_kill(w->target, SIGUSR1); atomic_store(&w->error, err); if (err) break; }
    }
    return NULL;
}
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) if (!strcmp(e[i].name, name)) { count++; fn = (void *)e[i].fn; }
    CHECK(count == 1 && fn, name);
    return fn;
}
static void guest_addr(void *g, uint32_t ip, uint16_t port) {   // Horizon SockAddrIn: family 2, port big-endian, address
    memset(g, 0, 16);
    ((uint8_t *)g)[0] = 2;
    uint16_t p = htons(port); memcpy((char *)g + 2, &p, 2);
    uint32_t a = htonl(ip); memcpy((char *)g + 4, &a, 4);
}
int main(void) {
    unsigned (*handle_error)(void) = hook("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv");
    if (!handle_error) return 1;
    setenv("DM_NET", "0", 1);
    CHECK(handle_error() == 0, "offline nifm network screen did not resolve the error");
    setenv("DM_NET", "1", 1);
    setenv("DM_NET_PEERS", getenv("T_NET_PEER") ? getenv("T_NET_PEER") : "127.0.0.1", 1);   // T_NET_PEER: test the replication over a real interface (e.g. the tailnet's)
    int (*sock)(int, int, int) = hook("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE");
    int (*bind_)(int, const void *, unsigned) = hook("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj");
    int (*gname)(int, void *, unsigned *) = hook("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj");
    long (*sendto_)(int, const void *, unsigned long, int, const void *, unsigned) = hook("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj");
    int (*poll_)(void *, unsigned long, int) = hook("_ZN2nn6socket4PollEPNS0_6PollFdEmi");
    long (*recvfrom_)(int, void *, unsigned long, int, void *, unsigned *) = hook("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj");
    int (*fcntl_)(int, int, long) = hook("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz");
    int (*close_)(int) = hook("_ZN2nn6socket5CloseEi");
    int (*lasterr)(void) = hook("_ZN2nn6socket12GetLastErrorEv");
    int (*select_)(int, void *, void *, void *, void *) = hook("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE");
    int (*netavail)(void) = hook("_ZN2nn4nifm18IsNetworkAvailableEv");
    Result (*ipcfg)(uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *) = hook("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_");
    if (fails) { printf("net: %d checks, %d failed\n", checks, fails); return 1; }

    int a = sock(2, 2, 0);   // Inet / Dgram / IpProtoUdp (0 = the default here)
    CHECK(a >= 0, "Socket");
    uint8_t any[16]; guest_addr(any, 0, 0);
    CHECK(bind_(a, any, 16) == 0, "Bind");
    uint8_t name[16]; unsigned nl = 16;
    CHECK(gname(a, name, &nl) == 0 && name[0] == 2 && ((name[2] << 8) | name[3]) != 0, "GetSockName");
    uint16_t port = (uint16_t)((name[2] << 8) | name[3]);

    uint8_t dst[16]; guest_addr(dst, 0x7f000001, port);   // 127.0.0.1
    CHECK(sendto_(a, "hello", 5, 0, dst, 16) == 5, "SendTo loopback");
    struct { int32_t fd; int16_t events, revents; } pf = {a, 1, 0};   // POLLIN
    CHECK(poll_(&pf, 1, 500) == 1 && (pf.revents & 1), "Poll readable");
    uint8_t buf[64]; uint8_t from[16]; unsigned fl = 16;
    long r = recvfrom_(a, buf, sizeof buf, 0, from, &fl);
    CHECK(r == 5 && !memcmp(buf, "hello", 5) && from[0] == 2, "RecvFrom");
    CHECK(lasterr() == 0, "GetLastError");

    uint8_t short_from[16]; memset(short_from, 0xa5, sizeof short_from);
    unsigned short_len = 4;
    CHECK(sendto_(a, "short", 5, 0, dst, 16) == 5, "SendTo for truncated address");
    CHECK(recvfrom_(a, buf, sizeof buf, 0, short_from, &short_len) == 5 && short_len == 16,
          "RecvFrom reports the full source address length");
    CHECK(short_from[0] == 2 && short_from[4] == 0xa5 && short_from[15] == 0xa5,
          "RecvFrom respects the source address capacity");
    memset(short_from, 0xa5, sizeof short_from); short_len = 4;
    CHECK(gname(a, short_from, &short_len) == 0 && short_len == 16 && short_from[0] == 2 && short_from[4] == 0xa5,
          "GetSockName truncates to the capacity and reports the full length");

    int fl0 = fcntl_(a, 3, 0);   // F_GETFL
    CHECK(fl0 >= 0, "Fcntl GETFL");
    CHECK(fcntl_(a, 4, (long)(fl0 | 0x800)) == 0, "Fcntl SETFL non-blocking");   // Horizon's O_NONBLOCK = 0x800
    CHECK((fcntl_(a, 3, 0) & 0x800) != 0, "Fcntl GETFL has O_NONBLOCK");
    CHECK(fcntl_(a, 4, (long)(fl0 & ~0x800)) == 0, "Fcntl SETFL back");

    uint8_t bc[16]; guest_addr(bc, 0xffffffff, port);   // 255.255.255.255 → replicated to the peer (127.0.0.1 here)
    CHECK(sendto_(a, "bc", 2, 0, bc, 16) == 2, "SendTo broadcast (replicated)");
    CHECK(poll_(&pf, 1, 500) == 1, "Poll readable (broadcast)");
    r = recvfrom_(a, buf, sizeof buf, 0, from, &fl);
    CHECK(r == 2 && !memcmp(buf, "bc", 2), "RecvFrom broadcast");

    static char too_large[65536];   // beyond IPv4 UDP's maximum payload: neither the broadcast nor any peer copy can succeed
    CHECK(sendto_(a, too_large, sizeof too_large, 0, bc, 16) == -1, "SendTo broadcast preserves total failure");
    CHECK(lasterr() == EMSGSIZE, "GetLastError after failed broadcast");

    int b = sock(2, 2, 0);
    CHECK(b >= 0, "Socket 2");
    uint8_t any2[16]; guest_addr(any2, 0, 0);
    CHECK(bind_(b, any2, 16) == 0, "Bind 2");
    uint8_t name2[16]; unsigned nl2 = 16;
    CHECK(gname(b, name2, &nl2) == 0, "GetSockName 2");
    uint16_t port2 = (uint16_t)((name2[2] << 8) | name2[3]);
    struct sigaction action = {0}, old_action;
    action.sa_handler = poll_interrupted; sigemptyset(&action.sa_mask);
    int handler_ok = sigaction(SIGUSR1, &action, &old_action) == 0;
    CHECK(handler_ok, "install the polling signal handler");
    if (!handler_ok) return 1;
    sigset_t signal_set, old_mask; sigemptyset(&signal_set); sigaddset(&signal_set, SIGUSR1);
    int unblocked = pthread_sigmask(SIG_UNBLOCK, &signal_set, &old_mask) == 0;
    CHECK(unblocked, "unblock the polling signal on this thread");
    if (!unblocked) { sigaction(SIGUSR1, &old_action, NULL); return 1; }
    poll_signal_seen = 0;
    struct poll_waker waker = {.target = pthread_self()}; pthread_t waking;
    int thread_ok = pthread_create(&waking, NULL, interrupt_poll, &waker) == 0;
    CHECK(thread_ok, "create the polling thread's signal sender");
    struct { int32_t fd; int16_t events, revents; } idle = {b, 1, 0};
    int interrupted = poll_(&idle, 1, 1000);   // an actual socket wait, also on UIKit's worker threads
    int host_error = errno, poll_error = lasterr();
    atomic_store(&waker.done, 1); if (thread_ok) pthread_join(waking, NULL);
    CHECK(atomic_load(&waker.error) == 0 && poll_signal_seen, "the polling thread received its signal");
    printf("Poll signal: received=%d sender_error=%d return=%d host_errno=%d guest_errno=%d\n", (int)poll_signal_seen, atomic_load(&waker.error), interrupted, host_error, poll_error);
    CHECK(interrupted == -1, "Poll interrupted by a signal on its own thread");
    CHECK(poll_error == EINTR, "GetLastError after interrupted Poll");
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL); sigaction(SIGUSR1, &old_action, NULL);
    CHECK(poll_(NULL, 0, 0) == 0 && lasterr() == 0, "successful Poll clears the previous error");

    uint8_t dst2[16]; guest_addr(dst2, 0x7f000001, port2);
    CHECK(sendto_(a, "sel", 3, 0, dst2, 16) == 3, "SendTo for Select");
    uint64_t rset[16] = {0}; rset[b >> 6] |= (uint64_t)1 << (b & 63);
    int64_t tv[2] = {0, 500000};   // 0.5 s
    CHECK(select_(b + 1, rset, NULL, NULL, tv) == 1, "Select readable");
    CHECK(close_(a) == 0 && close_(b) == 0, "Close");

    int listener = dm_sk_socket(2, 1, 0), client = dm_sk_socket(2, 1, 0);
    CHECK(listener >= 0 && client >= 0, "TCP sockets");
    CHECK(dm_sk_bind(listener, any, 16) == 0 && dm_sk_listen(listener, 1) == 0, "TCP listener");
    nl = sizeof name;
    CHECK(dm_sk_name(listener, name, &nl, 0) == 0, "TCP listener address");
    guest_addr(dst, 0x7f000001, (uint16_t)((name[2] << 8) | name[3]));
    CHECK(dm_sk_connect(client, dst, sizeof dst) == 0, "TCP connect");
    memset(short_from, 0xa5, sizeof short_from); short_len = 4;
    int accepted = dm_sk_accept(listener, short_from, &short_len);
    CHECK(accepted >= 0 && short_len == 16 && short_from[0] == 2 && short_from[4] == 0xa5,
          "Accept truncates to the capacity and reports the full length");
    CHECK(dm_sk_close(accepted) == 0 && dm_sk_close(client) == 0 && dm_sk_close(listener) == 0, "TCP close");


    CHECK(handle_error() == 0, "online nifm network screen did not resolve the error");
    CHECK(netavail() == 1, "nifm IsNetworkAvailable");
    uint32_t ip = 0, mask = 0, gw = 0, d0 = 0, d1 = 0;
    CHECK(ipcfg(&ip, &mask, &gw, &d0, &d1) == 0 && ip != 0 && mask != 0, "nifm IpConfigInfo");

    printf("net: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
