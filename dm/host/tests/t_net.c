// Exercise the actual guest-facing HLE_FORCE entries, with real Darwin sockets as the peer.
// Link hle_net.c, hle_offline.c, hle_net_backend.c, hle_bigstack.c and entry.S with a generated game_profile.h include path.
#include "../hle.h"
#include "../net_socket.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>

pthread_key_t g_keygate;
extern uint64_t dm_gate_top[1024];
extern const unsigned char dm_gate_stubs[];
void dm_gate_nested(uint64_t sp) { (void)sp; abort(); }
static _Atomic unsigned test_gate_next;
static void *test_gate_stack(void) {
    unsigned slot = atomic_fetch_add(&test_gate_next, 1); assert(slot < 1024);
    void *stack = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(stack != MAP_FAILED);
    dm_gate_top[slot] = (uintptr_t)stack + (1 << 20);
    pthread_setspecific(g_keygate, dm_gate_stubs + 16 * slot);
    return stack;
}
__attribute__((constructor)) static void test_gate_key(void) { pthread_key_create(&g_keygate, NULL); (void)test_gate_stack(); }
struct test_start { void *(*fn)(void *); void *arg; };
static void *test_thread_start(void *p) {
    struct test_start start = *(struct test_start *)p; free(p);
    void *stack = test_gate_stack(), *value = start.fn(start.arg);
    munmap(stack, 1 << 20); return value;
}
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    struct test_start *start = malloc(sizeof *start); assert(start); *start = (struct test_start){fn,arg};
    int status = pthread_create(thread, attr, test_thread_start, start); if (status) free(start); return status;
}
#define pthread_create test_pthread_create
static int checks, failures;
static _Atomic int waits, wait_balance;
static _Atomic int contexts_entered, contexts_left, invalid_contexts;
static __thread int context_depth;
static volatile sig_atomic_t sigpipe_count;
static int resolver_abi;
static void sigpipe(int signal) { (void)signal; sigpipe_count++; }
#define CHECK(c, message) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL line %d: %s (guest errno=%d host errno=%d)\n", __LINE__, message, dm_net_errno(), errno); abort(); } } while (0)
void dm_guest_host_wait_begin(void) { atomic_fetch_add(&waits, 1); atomic_fetch_add(&wait_balance, 1); }
void dm_guest_host_wait_end(void) { atomic_fetch_sub(&wait_balance, 1); }
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
void dm_guest_sleep_ns(int64_t ns) { struct timespec t = {ns / 1000000000, ns % 1000000000}; dm_guest_host_wait_begin(); nanosleep(&t, NULL); dm_guest_host_wait_end(); }
void hle_bind_system_event(void *p, int clear) { (void)p; (void)clear; }
void hle_signal_system_event(void *p) { (void)p; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
void thr_svc_enter(uint64_t *registers, uint64_t pc) { if (!registers || !pc || context_depth) atomic_fetch_add(&invalid_contexts, 1); context_depth++; atomic_fetch_add(&contexts_entered, 1); }
void thr_svc_leave(void) { if (context_depth != 1) atomic_fetch_add(&invalid_contexts, 1); context_depth--; atomic_fetch_add(&contexts_left, 1); }
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
uint64_t gate_dispatch(uint64_t ret, uint64_t *registers) { (void)ret; (void)registers; abort(); }
uint64_t unimpl_c(unsigned index, uint64_t *registers) { (void)index; (void)registers; abort(); }
void dm_thread_returned(void) { abort(); }
void *guest_sym(const char *name) {
    if (strstr(name, "GetAddrInfoEPKcS2_PK8addrinfoPPS3_")) return resolver_abi != 1 ? (void *)1 : NULL;
    if (strstr(name, "GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_")) return resolver_abi != 0 ? (void *)1 : NULL;
    return NULL;
}

static void *hook(const char *name) {
    unsigned long bytes; const hle_force_ent *p = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (unsigned long i = 0; p && i < bytes / sizeof *p; i++) if (!strcmp(p[i].name, name)) return p[i].fn;
    fprintf(stderr, "Missing hook: %s\n", name); abort();
}
typedef int (*fn_socket)(int, int, int);
typedef int (*fn_bind)(int, const void *, unsigned);
typedef int (*fn_name)(int, void *, unsigned *);
typedef int (*fn_int)(int);
typedef int (*fn_fcntl)(int, int, int);
typedef long (*fn_sendto)(int, const void *, unsigned long, int, const void *, unsigned);
typedef long (*fn_recvfrom)(int, void *, unsigned long, int, void *, unsigned *);
typedef int (*fn_option)(int, int, int, const void *, unsigned);
typedef int (*fn_getoption)(int, int, int, void *, unsigned *);
typedef int (*fn_poll)(void *, unsigned long, int);
typedef int (*fn_select)(int, void *, void *, void *, void *);
typedef int (*fn_void)(void);
typedef void (*fn_vvoid)(void);
typedef struct { uint8_t len, family; uint16_t port; uint32_t ip; uint8_t padding[8]; } address;
#define GET(type, symbol) ((type)hook(symbol))
#define SOCKET GET(fn_socket, "_ZN2nn6socket6SocketEiii")
#define BIND GET(fn_bind, "_ZN2nn6socket4BindEiPK8sockaddrj")
#define NAME GET(fn_name, "_ZN2nn6socket11GetSockNameEiP8sockaddrPj")
#define PEER GET(fn_name, "_ZN2nn6socket11GetPeerNameEiP8sockaddrPj")
#define CLOSE GET(fn_int, "_ZN2nn6socket5CloseEi")
#define FCNTL GET(fn_fcntl, "_ZN2nn6socket5FcntlEiiz")
#define SEND GET(fn_sendto, "_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj")
#define RECV GET(fn_recvfrom, "_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj")
#define SETOPT GET(fn_option, "_ZN2nn6socket10SetSockOptEiiiPKvj")
#define GETOPT GET(fn_getoption, "_ZN2nn6socket10GetSockOptEiiiPvPj")
#define POLL GET(fn_poll, "_ZN2nn6socket4PollEP6pollfdmi")
#define SELECT GET(fn_select, "_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval")
static address addr(uint32_t ip, uint16_t port) { return (address){16, 2, htons(port), ip, {0}}; }
static int native_udp(address *a) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in in = {.sin_len = sizeof in, .sin_family = AF_INET, .sin_addr.s_addr = a->ip, .sin_port = a->port};
    CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&in, sizeof in) == 0, "native UDP peer bound");
    socklen_t n = sizeof in; CHECK(getsockname(fd, (struct sockaddr *)&in, &n) == 0, "native peer port"); a->port = in.sin_port; return fd;
}
static void offline(void) {
    unsetenv("DM_NETWORK");
    CHECK(!dm_net_enabled() && SOCKET(2, 2, 17) == -1, "offline remains the default, no socket created");
    CHECK(GET(fn_void, "_ZN2nn6socket12GetLastErrnoEv")() == 100, "offline reports actual Horizon ENETDOWN");
    CHECK(GET(fn_void, "_ZN2nn4nifm18IsNetworkAvailableEv")() == 0, "offline NIFM unavailable");
    fn_void on_hold = GET(fn_void, "_ZN2nn4nifm22IsNetworkRequestOnHoldEv"); on_hold();
    GET(fn_vvoid, "_ZN2nn4nifm20SubmitNetworkRequestEv")();
    CHECK(on_hold() == 1, "offline retains asynchronous request hold");
    dm_guest_sleep_ns(60000000);
    CHECK(GET(fn_void, "_ZN2nn4nifm22IsNetworkRequestOnHoldEv")() == 0 && !dm_nifm_available(), "offline request settles as unavailable");
    CHECK(POLL(NULL, 0, 2) == 0, "offline idle poll remains a timed wait");
    pid_t child = fork();
    if (child == 0) { POLL(NULL, 0, -1); _exit(91); }
    CHECK(child > 0, "start infinite offline poll in isolated child");
    if (child > 0) { dm_guest_sleep_ns(150000000); int status = 0; CHECK(waitpid(child, &status, WNOHANG) == 0, "infinite offline poll never returns a fabricated100ms timeout"); kill(child, SIGTERM); CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status), "external process shutdown ends the infinite wait"); }
}
static void nifm(void) {
    setenv("DM_NETWORK", "auto", 1); setenv("DM_NETWORK_INTERFACE", "lo0", 1);
    GET(fn_vvoid, "_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv")();
    CHECK(dm_nifm_available(), "NIFM automatic request completes with real up interface");
    uint32_t ip = 0, mask = 0, router = 1, dns1 = 0, dns2 = 0;
    CHECK(dm_net_ipconfig(&ip, &mask, &router, &dns1, &dns2) == 0 && ip == htonl(0x7f000001) && mask == htonl(0xff000000) && router == 0, "actual loopback IP/mask and no invented router");
    setenv("DM_NETWORK_INTERFACE", "no-such-interface", 1);
    CHECK(dm_net_ipconfig(&ip, NULL, NULL, NULL, NULL) < 0 && ip == 0 && !dm_nifm_available(), "missing interface does not claim connectivity");
    setenv("DM_NETWORK_INTERFACE", "lo0", 1);
    GET(fn_vvoid, "_ZN2nn4nifm20CancelNetworkRequestEv")(); CHECK(!dm_nifm_available(), "cancel clears the request state");
}
static void udp(void) {
    int guest = SOCKET(2, 2, 17); address local = addr(htonl(0x7f000001), 0), peer = local;
    CHECK(guest >= 0 && BIND(guest, &local, sizeof local) == 0, "guest UDP socket binds");
    unsigned n = sizeof local; CHECK(NAME(guest, &local, &n) == 0 && n == 16 && local.port != 0, "guest UDP actual bound address");
    struct { uint8_t bytes[3], guard; } short_address = {{0}, 0xa5}; n = 3;
    CHECK(NAME(guest, &short_address, &n) == 0 && n == 16 && short_address.guard == 0xa5, "truncated sockaddr writes only the caller capacity and reports full length");
    int native = native_udp(&peer);
    CHECK(SEND(guest, "guest", 5, 0, &peer, 16) == 5, "guest UDP transmits to a native socket");
    char bytes[32] = {0}; struct sockaddr_in sender; socklen_t size = sizeof sender;
    CHECK(recvfrom(native, bytes, sizeof bytes, 0, (struct sockaddr *)&sender, &size) == 5 && !memcmp(bytes, "guest", 5), "native peer received exact guest bytes");
    CHECK(sendto(native, "host", 4, 0, (struct sockaddr *)&sender, size) == 4, "native peer replies");
    dm_net_pollfd p = {guest, 1, 0}; CHECK(POLL(&p, 1, 1000) == 1 && (p.revents & 1), "poll translates readable event");
    address from = {0}; n = sizeof from; CHECK(RECV(guest, bytes, sizeof bytes, 2, &from, &n) == 4 && from.port == peer.port, "peek returns source without consuming");
    CHECK(RECV(guest, bytes, sizeof bytes, 0, NULL, NULL) == 4 && !memcmp(bytes, "host", 4), "guest consumes native datagram");
    CHECK(FCNTL(guest, 4, 0x800) == 0 && FCNTL(guest, 3, 0) == 0x800, "nonblock flag translated from Horizon0x800");
    CHECK(FCNTL(guest, 4, 0x802) == 0 && FCNTL(guest, 3, 0) == 0x800, "NEX read/write access bits do not invalidate F_SETFL");
    CHECK(RECV(guest, bytes, sizeof bytes, 0, NULL, NULL) < 0 && dm_net_errno() == 11, "nonblock recv reports Horizon EAGAIN11");
    CHECK(FCNTL(guest, 4, 2) == 0 && FCNTL(guest, 3, 0) == 0, "read/write bits alone restore blocking mode without changing native ownership");
    CHECK(RECV(guest, bytes, sizeof bytes, 0x80, NULL, NULL) < 0 && dm_net_errno() == 11, "per-call DONTWAIT0x80 translated");
    uint32_t timeout = 25; CHECK(SETOPT(guest, 0xffff, 0x1006, &timeout, 4) == 0, "receive timeout milliseconds accepted");
    uint64_t start = now_ns(); CHECK(RECV(guest, bytes, sizeof bytes, 0, NULL, NULL) < 0 && dm_net_errno() == 11 && now_ns() - start >= 20000000, "blocking recv timeout waits and reports EAGAIN");
    timeout = 0; CHECK(SETOPT(guest, 0xffff, 0x1006, &timeout, 4) == 0, "timeout cleared");
    timeout = 123; n = 4; CHECK(GETOPT(guest, 0xffff, 0x1006, &timeout, &n) == 0 && timeout == 0, "zero timeout roundtrips as infinite, never uint32 minus one");
    int one = 1, value = 0; n = 4; CHECK(SETOPT(guest, 0xffff, 0x20, &one, 4) == 0 && GETOPT(guest, 0xffff, 0x20, &value, &n) == 0 && value, "broadcast option is real");
    uint64_t read[16] = {0}, write[16] = {0}; read[guest >> 6] |= 1ull << (guest & 63); write[guest >> 6] |= 1ull << (guest & 63); int64_t tv[2] = {0, 0};
    CHECK(SELECT(guest + 1, read, write, NULL, tv) == 1 && !read[guest >> 6] && write[guest >> 6], "select fd_set layout and output bits translated");
    int first_close = CLOSE(guest), repeated_close = CLOSE(guest);
    CHECK(first_close == 0 && repeated_close < 0 && dm_net_errno() == 9, "close owns guest socket only, second close EBADF");
    close(native);
}
typedef struct { int socket; int result; int error; } blocked_call;
static void *blocked_recv(void *opaque) { blocked_call *c = opaque; char byte; c->result = (int)RECV(c->socket, &byte, 1, 0, NULL, NULL); c->error = dm_net_errno(); return NULL; }
static void concurrency(void) {
    int guest = SOCKET(2, 2, 17); address a = addr(htonl(0x7f000001), 0); CHECK(BIND(guest, &a, 16) == 0, "blocking socket bound");
    blocked_call call = {.socket = guest}; pthread_t t; CHECK(pthread_create(&t, NULL, blocked_recv, &call) == 0, "start blocked guest recv");
    dm_guest_sleep_ns(30000000); CHECK(CLOSE(guest) == 0, "concurrent close removes the slot");
    int reused = SOCKET(2, 2, 17); CHECK(reused == guest, "guest slot reused");
    CHECK(pthread_join(t, NULL) == 0 && call.result < 0 && call.error == 9, "old waiter cancels with EBADF and never reads reused socket");
    CHECK(CLOSE(reused) == 0, "reused socket remains intact");
}
typedef struct { int fd; } writer_args;
static void *split_writer(void *opaque) { writer_args *a = opaque; send(a->fd, "12", 2, 0); dm_guest_sleep_ns(25000000); send(a->fd, "34", 2, 0); return NULL; }
static void tcp(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001)};
    CHECK(server >= 0 && bind(server, (struct sockaddr *)&a, sizeof a) == 0 && listen(server, 1) == 0, "native TCP listener");
    socklen_t size = sizeof a; getsockname(server, (struct sockaddr *)&a, &size); address destination = addr(a.sin_addr.s_addr, 0); destination.port = a.sin_port;
    int guest = SOCKET(2, 1, 6); CHECK(GET(fn_bind, "_ZN2nn6socket7ConnectEiPK8sockaddrj")(guest, &destination, 16) == 0, "guest blocking connect completes on readiness");
    int native = accept(server, NULL, NULL); CHECK(native >= 0, "native accepts guest connection");
    address remote; unsigned n = 16; CHECK(PEER(guest, &remote, &n) == 0 && remote.port == destination.port, "guest peer address translated");
    writer_args args = {native}; pthread_t t; pthread_create(&t, NULL, split_writer, &args); char bytes[8] = {0};
    CHECK(RECV(guest, bytes, 4, 0x40, NULL, NULL) == 4 && !memcmp(bytes, "1234", 4), "WAITALL gathers partial TCP arrivals"); pthread_join(t, NULL);
    CHECK(SEND(guest, "ok", 2, 0, NULL, 0) == 2 && recv(native, bytes, sizeof bytes, 0) == 2 && !memcmp(bytes, "ok", 2), "guest TCP send to native peer");
    close(native); CHECK(RECV(guest, bytes, 4, 0, NULL, NULL) == 0, "TCP orderly EOF");
    CHECK(GET(int (*)(int,int), "_ZN2nn6socket8ShutdownEii")(guest, 1) == 0, "guest shutdown translates write direction");
    CHECK(SEND(guest, "closed", 6, 0, NULL, 0) < 0 && dm_net_errno() == 32 && !sigpipe_count, "send after shutdown reports EPIPE without raising host SIGPIPE"); CLOSE(guest);
    guest = SOCKET(2, 1, 6); CHECK(FCNTL(guest, 4, 0x802) == 0, "TCP set nonblock with actual NEX F_SETFL flags");
    int connected = GET(fn_bind, "_ZN2nn6socket7ConnectEiPK8sockaddrj")(guest, &destination, 16);
    CHECK(connected == 0 || (connected < 0 && dm_net_errno() == 115), "nonblocking connect reports Horizon EINPROGRESS115");
    dm_net_pollfd pending = {guest, 4, 0}; CHECK(POLL(&pending, 1, 1000) == 1 && (pending.revents & 4), "nonblocking connect becomes writable");
    int error = -1; n = 4; CHECK(GETOPT(guest, 0xffff, 0x1007, &error, &n) == 0 && error == 0, "SO_ERROR completes nonblocking connect");
    native = accept(server, NULL, NULL); CHECK(native >= 0, "native accepts nonblocking client"); close(native); CLOSE(guest); close(server);
    int listener = SOCKET(2, 1, 6); address local = addr(htonl(0x7f000001), 0); n = 16;
    CHECK(BIND(listener, &local, 16) == 0 && NAME(listener, &local, &n) == 0 && GET(int (*)(int,int), "_ZN2nn6socket6ListenEii")(listener, 1) == 0, "guest TCP listener");
    int client = socket(AF_INET, SOCK_STREAM, 0); CHECK(client >= 0, "native client socket"); a.sin_port = local.port; CHECK(connect(client, (struct sockaddr *)&a, sizeof a) == 0, "native connects guest listener");
    n = 16; int accepted = GET(fn_name, "_ZN2nn6socket6AcceptEiP8sockaddrPj")(listener, &remote, &n); CHECK(accepted >= 0 && accepted != listener && remote.ip == htonl(0x7f000001), "guest accept allocates a separate owned descriptor");
    close(client); CLOSE(accepted); CLOSE(listener);
}
static void errors_and_resolver(void) {
    CHECK(SOCKET(28, 2, 17) < 0 && dm_net_errno() == 97, "IPv6 rejected with correct family error");
    CHECK(SOCKET(2, 3, 0) < 0 && dm_net_errno() == 94, "raw socket rejected");
    CHECK(SOCKET(2, 1, 17) < 0 && dm_net_errno() == 93, "protocol mismatch rejected");
    int guest = SOCKET(2, 2, 17); address a = addr(htonl(0x7f000001), 0);
    CHECK(BIND(guest, &a, 3) < 0 && dm_net_errno() == 22, "short sockaddr rejected");
    CHECK(BIND(guest, NULL, 16) < 0 && dm_net_errno() == 14, "null sockaddr rejected");
    CHECK(FCNTL(guest, 4, 4) < 0 && dm_net_errno() == 22, "Darwin nonblock numeric flag not accidentally accepted");
    int one = 1; CHECK(SETOPT(guest, 0xffff, 0x7777, &one, 4) < 0 && dm_net_errno() == 92, "unsupported option fails honestly");
    dm_net_pollfd bad = {999, 1, 0}; CHECK(POLL(&bad, 1, 0) == 1 && bad.revents == 32, "poll invalid guest descriptor returns NVAL");
    int saved = fcntl(0, F_GETFD); dm_net_pollfd malformed[3] = {{guest, 0x4000, 0}, {guest, 1, 0}, {guest, 1, 0}};
    CHECK(POLL(malformed, 3, 0) < 0 && fcntl(0, F_GETFD) == saved, "poll validation never closes unborrowed host descriptors");
    CHECK(POLL(NULL, 1, 0) < 0 && dm_net_errno() == 14 && POLL(NULL, UINT64_MAX, 0) < 0, "poll null/overflow rejected");
    int64_t badtime[2] = {0, 1000000}; CHECK(SELECT(1, NULL, NULL, NULL, badtime) < 0 && dm_net_errno() == 22, "select invalid timeval rejected");
    CLOSE(guest);
    typedef int (*fn_gai)(const char *, const char *, const void *, void **);
    dm_net_addrinfo hints = {.family = 2, .type = 2, .flags = 4 | 8}; void *out = NULL;
    CHECK(GET(fn_gai, "_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_")("127.0.0.1", "12345", &hints, &out) == 0 && out, "numeric getaddrinfo through guest hook");
    if (out) { dm_net_addrinfo_legacy *p = out; CHECK(p->family == 2 && p->type == 2 && p->addrlen == 16 && ((address *)p->addr)->port == htons(12345), "SDK4 addrinfo address pointer at32, wire port and address correct"); p->next = (void *)0xdeadbeef; dm_net_freeaddrinfo(out); dm_net_freeaddrinfo(out); CHECK(dm_net_errno() == 22, "resolver free uses owned allocations, ignores corrupted guest next and double free"); }
    CHECK(GET(fn_gai, "_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_")("127.0.0.1", "12345", &hints, &out) == 0 && out, "modern named AddrInfo uses its separate hook");
    if (out) { dm_net_addrinfo *p = out; CHECK(p->family == 2 && p->addrlen == 16 && ((address *)p->addr)->port == htons(12345), "modern AddrInfo address pointer at24 translated separately"); dm_net_freeaddrinfo(out); }
    resolver_abi = 0;
    CHECK(GET(fn_gai, "nnsocketGetAddrInfo")("127.0.0.1", "1", &hints, &out) == 0 && out && ((dm_net_addrinfo_legacy *)out)->addr, "C resolver selects the exported legacy SDK ABI"); dm_net_freeaddrinfo(out);
    resolver_abi = 1;
    CHECK(GET(fn_gai, "nnsocketGetAddrInfo")("127.0.0.1", "1", &hints, &out) == 0 && out && ((dm_net_addrinfo *)out)->addr, "C resolver selects the exported modern SDK ABI"); dm_net_freeaddrinfo(out);
    resolver_abi = 2;
    CHECK(GET(fn_gai, "nnsocketGetAddrInfo")("127.0.0.1", "1", &hints, &out) == 4 && !out, "C resolver refuses ambiguous exported ABIs"); resolver_abi = 0;
    CHECK(dm_net_getaddrinfo("127.0.0.1", "1", &(dm_net_addrinfo){.family = 28}, &out) == 5 && !out, "unsupported resolver family returns EAI_FAMILY");
    CHECK(GET(void *(*)(const char *), "_ZN2nn6socket13GetHostByNameEPKc")("127.0.0.1") != NULL, "automatic mode resolves a real numeric LAN peer through legacy hostent");
    CHECK(GET(void *(*)(const char *), "_ZN2nn6socket13GetHostByNameEPKc")("localhost") != NULL, "local mode resolves the host loopback name without an online provider");
}
static void broadcast(void) {
    unsetenv("DM_NETWORK_INTERFACE"); uint32_t ip, mask;
    if (dm_net_ipconfig(&ip, &mask, NULL, NULL, NULL)) { CHECK(0, "broadcast requires an up real network interface"); return; }
    address destination = addr(0, 0); int native = native_udp(&destination); destination.ip = htonl(0xffffffff);
    int guest = SOCKET(2, 2, 17), one = 1; CHECK(SETOPT(guest, 0xffff, 0x20, &one, 4) == 0, "broadcast enabled");
    address source = addr(ip, 0); CHECK(BIND(guest, &source, sizeof source) == 0, "broadcast sender uses the actual NIFM interface");
    const char message[] = "nxnative LAN transport test";
    CHECK(SEND(guest, message, sizeof message, 0, &destination, sizeof destination) == sizeof message, "guest sends real limited broadcast");
    struct pollfd p = {native, POLLIN, 0}; CHECK(poll(&p, 1, 1000) == 1, "native peer receives guest limited broadcast");
    if (p.revents & POLLIN) { char bytes[64]; CHECK(recv(native, bytes, sizeof bytes, 0) == sizeof message && !memcmp(bytes, message, sizeof message), "broadcast payload preserved"); }
    CLOSE(guest); close(native); setenv("DM_NETWORK_INTERFACE", "lo0", 1);
}
static void discovery_peer(void) {
    address peer = addr(htonl(0x7f000001), 0);
    int native = native_udp(&peer), guest = SOCKET(2, 2, 17);
    char bytes[16];
    dm_net_set_multicast_allowed(0);
    address discovery = peer; discovery.ip = htonl(UINT32_MAX);
    unsetenv("DM_NET_DISCOVERY_PEER");
    CHECK(SEND(guest, "blocked", 7, 0, &discovery, 16) == -1 && dm_net_errno() == 99,
          "a loopback-only adapter cannot trigger an automatic LAN sweep");
    setenv("DM_NET_DISCOVERY_PEER", "127.0.0.1", 1);
    CHECK(SEND(guest, "discovery", 9, 0, &discovery, 16) == 9,
          "explicit unicast discovery works without multicast entitlement");
    struct pollfd ready = {native, POLLIN, 0};
    CHECK(poll(&ready, 1, 1000) == 1 && recv(native, bytes, sizeof bytes, 0) == 9 && !memcmp(bytes, "discovery", 9),
          "actual unicast peer receives unchanged discovery bytes");
    setenv("DM_NET_DISCOVERY_PEER", "invalid", 1);
    CHECK(SEND(guest, "invalid", 7, 0, &discovery, 16) == -1 && dm_net_errno() == 22,
          "malformed discovery peer fails explicitly");
    CHECK(SEND(guest, "ordinary", 8, 0, &peer, 16) == 8,
          "ordinary unicast ignores discovery override");
    CHECK(poll(&ready, 1, 1000) == 1 && recv(native, bytes, sizeof bytes, 0) == 8 && !memcmp(bytes, "ordinary", 8),
          "ordinary unicast keeps original destination and bytes");
    setenv("DM_NET_DISCOVERY_PEER", "224.0.0.1", 1);
    CHECK(SEND(guest, "invalid", 7, 0, &discovery, 16) == -1 && dm_net_errno() == 22,
          "multicast address cannot masquerade as explicit unicast peer");
    uint32_t target;
    unsetenv("DM_NET_DISCOVERY_PEER");
    dm_net_set_multicast_allowed(1);
    CHECK(dm_net_discovery_target(discovery.ip, &target) == 0 && target == discovery.ip,
          "capable host retains native discovery destination");
    CLOSE(guest); close(native);
}
int main(int argc, char **argv) {
    struct sigaction sa = {.sa_handler = sigpipe}; sigemptyset(&sa.sa_mask); sigaction(SIGPIPE, &sa, NULL);
    offline(); nifm(); udp(); concurrency(); tcp(); errors_and_resolver(); discovery_peer();
    if (argc == 2 && !strcmp(argv[1], "--no-broadcast")) puts("broadcast test skipped explicitly (requires local-network permission for this test executable)"); else broadcast();
    GET(fn_vvoid, "_ZN2nn6socket8FinalizeEv")();
    CHECK(atomic_load(&waits) > 0 && atomic_load(&wait_balance) == 0, "all host waits yield and restore guest execution");
    CHECK(atomic_load(&contexts_entered) > 0 && atomic_load(&contexts_entered) == atomic_load(&contexts_left) && !atomic_load(&invalid_contexts), "blocking guest hooks publish and leave balanced register contexts");
    printf("network hooks: %d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
