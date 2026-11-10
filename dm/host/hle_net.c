// Opt-in local IPv4 sockets and NIFM state. Online account, friends and TLS services retain their offline behavior.
#include "hle.h"
#include "net_socket.h"
#include <pthread.h>
#include <stdatomic.h>
void dm_guest_sleep_ns(int64_t ns);

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
// SDK 21 (Star Fox) inlines nn::socket::Initialize(const Config &) into the game, and the service connection is this call
HLE_FORCE("_ZN2nn6socket8internal10InitializeEi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_InitializeInternal, int n) { (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "connects to the nifm network management service; offline (cut)", Result, h_nifm_Initialize, void) { return RES_OK; }
// The request is asynchronous on Horizon: SubmitNetworkRequest only hands it to the nifm service, and IsNetworkRequestOnHold stays true until the service has evaluated it, so a query
// right after the submission finds it on hold. Pikmin 4 (Unreal Engine 4.26, main+0x203f3b0) relies on that: it starts its BCAT thread, whose Run (main+0x203f510) sets its status
// to 2, submits, waits while on hold and sets 6 (done, offline), and only then stores status 1 itself; answered at once, a thread that ran first finished before that store, the 1 stayed,
// and the boot state machine (main+0x1f2f764, state 7) waited for the status forever at the black loading screen (about 1 boot in 6, 3 in 4 with four at once). Pikmin 4 sleeps 10 ms
// between its queries, so a request that is on hold at the first query is all it needs.
// Each query is an IPC to the nifm service on Horizon, which evaluates the request as it serves them (offline, a rejection), so the request is on hold for the first query after
// the submission and settled at the next. The hold used to be 50 ms of host time: Hades' NetworkPollThread (main+0x27aaa0) polls with SleepThread(0) and cancels after 2,000
// queries, 2,000 IPCs on the Switch but 0.1 ms here, so it cancelled and submitted again forever (1.17 billion queries a minute, a host core). Its stamp, now_ns() | 1, also read
// as settled at once whenever the next query came within the same 42 ns clock tick (30% of Hades' submissions)
static _Atomic int g_nifm_pending;
static _Atomic int g_nifm_hold;   // queries the pending request still answers "on hold" (0 = none, or settled)
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "offline: the request is on hold for the first query, then settles as unavailable", void, h_nifm_Submit, void) { atomic_store(&g_nifm_pending, 1); atomic_store(&g_nifm_hold, 1); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "offline: drops the pending request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_pending, 0); atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "offline: on hold for the first query after the submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
int dm_nifm_available(void) { return dm_net_enabled() && atomic_load(&g_nifm_pending) && !atomic_load(&g_nifm_hold) && !dm_net_ipconfig(NULL, NULL, NULL, NULL, NULL); }
void dm_nifm_submit_wait(void) { if (dm_net_enabled()) { h_nifm_Submit(); while (h_nifm_OnHold()) dm_guest_sleep_ns(0); } }
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "offline by default; explicit LAN reports the actual IPv4 request state", int, h_nifm_Available, void) { return dm_nifm_available(); }
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "whether the network request was resolved without a system UI", unsigned, h_nifm_HandleErr, void) { return dm_nifm_available() != 0; }
HLE_FORCE("_ZN2nn4nifm26HandleNetworkRequestResultEv", "SDK 4 network request result without a system UI", Result, h_nifm_HandleResult, void) { return dm_nifm_available() ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "offline by default; explicit LAN returns the actual interface IPv4 address", Result, h_nifm_GetIp, uint32_t *a) { if (!a) return RES_FAIL; *a = 0; return dm_net_ipconfig(a, NULL, NULL, NULL, NULL) ? RES_FAIL : RES_OK; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "offline: no proxy setting", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }

// ---------- socket: offline by default; explicit LAN translates the guest ABI to Darwin ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 100                 // Horizon BSD IPC errno is returned unchanged by nn::socket (MK8 SDK 4 detail::Socket / GetLastErrno).
// The game's errno. On Horizon nn::socket's errno is the C library's: detail::SetLastErrno and GetLastErrno write and read *__errno_location(), and code such
// as gRPC's reads errno itself. Super Mario Bros. Wonder's gRPC sets 97 when it leaves IPv6 (main+0xb61480) and reads errno after its non-blocking connect
// (main+0xb61e38, tcp_client_posix.cc:299): with only the host's copy set, it took that connect's EINPROGRESS for the stale 97 and gave the address up.
// A failed hook writes it on the game's thread, at the hook boundary; a game without __errno_location keeps only the host's copy
static int *(*_Atomic g_guest_errno)(void); static _Atomic int g_guest_errno_looked;
static int *guest_errno_location(void) {
    if (!atomic_load(&g_guest_errno_looked)) { g_guest_errno = (int *(*)(void))guest_sym("__errno_location"); atomic_store(&g_guest_errno_looked, 1); }
    int *(*loc)(void) = g_guest_errno; return loc ? loc() : NULL;
}
long dm_net_guest_errno(long r) { if (r < 0) { int *e = guest_errno_location(); if (e) *e = dm_net_errno(); } return r; }
int dm_net_guest_errno_get(void) { int *e = guest_errno_location(); return e ? *e : dm_net_errno(); }
static int sk_fail(void) { dm_net_set_errno(dm_net_enabled() ? 95 : SK_ENETDOWN); return (int)dm_net_guest_errno(-1); }
#define SKF(m, cname, ...) HLE_FORCE(m, "socket transport: unavailable by default, translated Darwin sockets in explicit Mac LAN mode", int, cname, __VA_ARGS__)
#define SKW(m, cname, ...) HLE_FORCE_WAIT(m, "blocking socket transport with a pausable guest context", int, cname, __VA_ARGS__)
// Poll / Select with nothing to wait on: the host creates no socket, so a game's descriptors are all -1, or it passes none. POSIX and Horizon's bsd service ignore negative
// descriptors and wait out the timeout, returning 0. Failing at once made Dark Souls Remastered's network thread ("NexusRevolution Socket", main+0x92c7c4) spin a core at 100%
// on Poll(fds, 0, 33 ms), 11,000 calls a second. An endless wait on nothing never returns;
// HLE_FORCE_WAIT publishes the guest context so a collector can pause the thread while it waits.
void dm_guest_sleep_ns(int64_t ns);
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns);   // hle_thread.c
static int sk_idle(int64_t ms) { if (ms < 0) return dm_net_idle(); if (ms > INT64_MAX / 1000000) { dm_net_set_errno(22); return -1; } dm_guest_sleep_ns(ms * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
// EventFd is a local descriptor of the bsd service and works with no network, so it is real here: gRPC (nn::npln, Ocarina of Time at startup) wakes its pollers with one, and with no
// wakeup descriptor its poll engine's factory (main+0x21f1280) returns null and grpc_init calls abort (main+0x21f0c7c). The descriptors are the host's only ones: Socket still fails.
// The flags are Horizon's: gRPC creates its descriptor with 6 and consumes after a poll says it is ready. A descriptor with bit 0 reads one at a time; bits 1 and 2 never block.
// The numbers are the bsd service's: the lowest free one, from 0, as for its sockets (the host makes no other descriptor). Games size their tables for those small numbers: Minecraft's
// NetherNet threads put theirs in a 1024-bit fd_set on the stack for Select (FD_SET at main+0x18b2ae8 in 1.21.120), and when the host numbered them from 0x4000 the bit landed 2 KB
// past the set and the thread crashed (SIGBUS)
enum { EFD_MAX = 64, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4, SK_POLLNVAL = 0x20, SK_EBADF = 9, SK_EAGAIN = 35 };
static struct { uint64_t count; int used, flags; } g_efd[EFD_MAX];
static pthread_mutex_t g_efd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_c = PTHREAD_COND_INITIALIZER;
static int efd_used(int fd) { return fd >= 0 && fd < EFD_MAX && g_efd[fd].used; }   // with g_efd_m held
static int efd_create(uint64_t v, int flags) {
    pthread_mutex_lock(&g_efd_m);
    for (int i = 0; i < EFD_MAX; i++) if (!g_efd[i].used) { g_efd[i].used = 1; g_efd[i].count = v; g_efd[i].flags = flags; pthread_mutex_unlock(&g_efd_m); return i; }
    pthread_mutex_unlock(&g_efd_m);
    LOG("[RED] net: more than %d event descriptors", EFD_MAX);
    dm_net_set_errno(24); return -1;   // EMFILE
}
static int efd_close(int fd) {
    pthread_mutex_lock(&g_efd_m);
    int ok = efd_used(fd);
    if (ok) { g_efd[fd].used = 0; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m);
    if (!ok) return sk_fail();
    return 0;
}
static int efd_read(int fd, uint64_t *out) {
    pthread_mutex_lock(&g_efd_m);
    int r = -1;
    for (;;) {
        if (!efd_used(fd)) { dm_net_set_errno(SK_EBADF); break; }
        uint64_t *c = &g_efd[fd].count; int f = g_efd[fd].flags;
        if (*c) { if (f & 1) { *out = 1; *c -= 1; } else { *out = *c; *c = 0; } r = 0; break; }
        if (f & 6) { dm_net_set_errno(SK_EAGAIN); break; }
        dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, dm_detclock_enabled() ? 1000000 : 100000000);
    }
    pthread_mutex_unlock(&g_efd_m);
    return r;
}
static int efd_write(int fd, uint64_t v) {
    pthread_mutex_lock(&g_efd_m);
    int ok = efd_used(fd);
    if (ok) { g_efd[fd].count += v; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m);
    if (!ok) { dm_net_set_errno(SK_EBADF); return -1; }
    return 0;
}
// With g_efd_m held: fills revents, returns how many descriptors have any. open holds the descriptors that were open when the wait began: one of those closed since is POLLNVAL, any other
// is one the host never made, with nothing to say (see dm_sk_poll). The numbers start at 0, where a game's stale or never-valid descriptors fall too, so the range alone cannot tell them apart
static int efd_ready(sk_pollfd *p, uint64_t n, uint64_t open) {
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        p[i].revents = 0;
        if (p[i].fd < 0 || p[i].fd >= EFD_MAX || !(open >> p[i].fd & 1)) continue;
        if (!efd_used(p[i].fd)) { p[i].revents = SK_POLLNVAL; ready++; continue; }   // closed while waiting
        if (g_efd[p[i].fd].count) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
        p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
        if (p[i].revents) ready++;
    }
    return ready;
}
// A descriptor the host never made (it creates no socket): Star Fox's HTTP thread polls two with a zero timeout, its wake-up eventfd that failed (-1) and a garbage one, and takes any failure
// but EINTR as fatal (main+0xbe2efc4). On a console a socket with nothing to say reports no event at the timeout, so that is the answer for every descriptor: nothing ready, the timeout
// waited out, 0. Failing with ENETDOWN here made the loop abort
int dm_sk_poll(void *fds, uint64_t n, int ms) {   // also hle_offline.c's plain pollfd form
    if (dm_net_enabled()) {
        int rv = dm_net_poll(fds, n, ms);
        return rv;
    }
    if (n > 1024 || (!fds && n)) { dm_net_set_errno(!fds && n ? 14 : 22); return -1; }
    sk_pollfd *p = fds;
    uint64_t open = 0;   // EFD_MAX is 64: one bit per descriptor
    pthread_mutex_lock(&g_efd_m);
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) {
        if (efd_used(p[i].fd)) { open |= 1ull << p[i].fd; continue; }
        static int once; if (!once++) LOG("socket: Poll of %llu descriptors with one the host never made (fd %d, events %#x; %d ms): nothing ready at the timeout", (unsigned long long)n, p[i].fd, p[i].events, ms);
    }
    pthread_mutex_unlock(&g_efd_m);
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    if (!open) {
        int rv = sk_idle(ms);
        return rv;
    }
    // As sk_idle: an endless wait comes back with 0 after 100 ms, so the hook returns to a garbage collector's pause and the caller can see a shutdown
    int64_t limit = (ms < 0 ? 100 : (int64_t)ms) * 1000000;
    uint64_t start = guest_ns();
    pthread_mutex_lock(&g_efd_m);
    int ready;
    while (!(ready = efd_ready(p, n, open))) {
        int64_t left = limit - (int64_t)(guest_ns() - start);
        if (left <= 0) break;
        dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, dm_detclock_enabled() && left > 1000000 ? 1000000 : left);
    }
    pthread_mutex_unlock(&g_efd_m);
    return ready;
}
// Select is Poll over the descriptors in its sets, so the same rules hold (Minecraft's NetherNet threads wait on their event descriptors this way): the read set asks POLLIN, the write
// set POLLOUT, and the sets come back with the ready ones. A descriptor closed during the wait fails with EBADF, as select does. Only the host's numbers (below EFD_MAX) are passed on:
// Poll has nothing to say about any other, and the array stays small on the guest thread's stack. An explicit network mode goes to the transport's Select instead
int dm_sk_select(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 seconds, i64 microseconds}
    if (dm_net_enabled()) { int rv = dm_net_select(nfds, r, w, e, tv); return rv; }
    if (nfds < 0 || nfds > 1024 || (tv && (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000 || tv[0] > INT64_MAX / 1000))) { dm_net_set_errno(22); return -1; }
    uint64_t *sets[3] = {r, w, e};
    static const int16_t ask[3] = {SK_POLLIN, SK_POLLOUT, 0};
    sk_pollfd p[EFD_MAX]; int np = 0;
    for (int fd = 0; fd < nfds && fd < EFD_MAX; fd++) {
        int in = 0; int16_t ev = 0;
        for (int k = 0; k < 3; k++) if (sets[k] && sets[k][fd >> 6] >> (fd & 63) & 1) { in = 1; ev |= ask[k]; }
        if (in) p[np++] = (sk_pollfd){fd, ev, 0};
    }
    int64_t ms = tv ? tv[0] * 1000 + tv[1] / 1000 : -1;
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    dm_sk_poll(p, (uint64_t)np, (int)ms);
    for (int k = 0; k < 3; k++) if (sets[k]) for (int fd = 0; fd < nfds; fd++) sets[k][fd >> 6] &= ~(1ull << (fd & 63));
    int n = 0;
    for (int i = 0; i < np; i++) {
        if (p[i].revents & SK_POLLNVAL) { dm_net_set_errno(SK_EBADF); return -1; }
        if (p[i].revents & (SK_POLLIN | SK_POLLRDNORM) && r) { r[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
        if (p[i].revents & SK_POLLOUT && w) { w[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
    }
    return n;
}
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return dm_net_guest_errno(dm_net_socket(a, b, c)); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return dm_net_guest_errno(dm_net_bind(s, a, n)); }
SKW("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { return dm_net_guest_errno(dm_net_connect(s, a, n)); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { return dm_net_guest_errno(dm_net_listen(s, n)); }
SKW("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_accept(s, a, n)); }
SKW("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { if (efd_used(s)) return efd_close(s); return dm_net_guest_errno(dm_net_close(s)); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 1)); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 0)); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_net_guest_errno(dm_net_sockopt(s, l, o, v, n, 0)); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_net_guest_errno(dm_net_sockopt(s, l, o, (void *)v, &n, 1)); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, int flags) { return dm_net_guest_errno(dm_net_fcntl(s, c, flags)); }
SKW("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_net_guest_errno(dm_sk_poll(f, n, t)); }
SKW("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_net_guest_errno(dm_sk_select(n, r, w, e, t)); }
// SDK 21 overloads Star Fox imports: plain sockaddr, the ShutdownMethod enum, Read / Write and a name lookup without options; SetAllocator keeps nothing, as no session is created. All go through the transport as their SockAddr forms do (the overloads only differ in the C type they spell)
SKF("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", h_sk_GetPeerName21, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 1)); }
SKF("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", h_sk_Shutdown21, int s, int how) { return dm_net_guest_errno(dm_net_shutdown(s, how)); }
HLE_FORCE_WAIT("_ZN2nn6socket4ReadEiPvm", "socket transport, offline by default", long, h_sk_Read, int s, void *b, unsigned long n) { return dm_net_guest_errno(dm_net_recv(s, b, n, 0, NULL, NULL)); }
HLE_FORCE_WAIT("_ZN2nn6socket5WriteEiPKvm", "socket transport, offline by default", long, h_sk_Write, int s, const void *b, unsigned long n) { return dm_net_guest_errno(dm_net_send(s, b, n, 0, NULL, 0)); }
HLE_FORCE_WAIT("_ZN2nn6socket16GetHostEntByNameEPKc", "name resolution, offline by default", void *, h_sk_GetHostEnt21, const char *n) { return dm_net_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", "socket library allocator; no session is created, ignored", void, h_sk_SetAlloc, void *r) { (void)r; }
// The C library's poll (SDK 21's nnmusl, Star Fox): it needs the socket state of the initialization cut above and fails with EFAULT without it, which the game's loop takes as fatal, so it goes
// to the host's Poll too
SKF("poll", h_libc_Poll, void *f, unsigned long n, int t) { return dm_net_guest_errno(dm_sk_poll(f, n, t)); }
// EventFd works in every mode. Offline it is the counter table above (a garbage collector of the game's own wakes its pollers, Ocarina of Time at startup). In a network mode it is a
// descriptor of the transport's socket table (dm_net_eventfd, hle_net_backend.c), numbered with the sockets, so the transport's Poll and Select see both: gRPC polls its sockets and
// its wakeup descriptor together (nn::npln). A blocking read waits as the host's other waits do: dm_guest_cond_wait_ns offline, the transport's bracketed host waits otherwise
HLE_FORCE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", "a counter descriptor: the host's table offline, the transport's socket table in a network mode", int, h_sk_EventFd, unsigned long v, int f) { int rv = dm_net_enabled() ? dm_net_eventfd(v, f) : efd_create(v, f); return (int)dm_net_guest_errno(rv); }
HLE_FORCE("_ZN2nn6socket11EventFdReadEiPm", "reads the counter; blocks unless the descriptor never blocks", int, h_sk_EventFdRead, int s, unsigned long *v) { uint64_t c = 0; int rv = dm_net_enabled() ? dm_net_eventfd_read(s, &c) : efd_read(s, &c); if (!rv && v) *v = c; return (int)dm_net_guest_errno(rv); }
HLE_FORCE("_ZN2nn6socket12EventFdWriteEim", "adds to the counter and wakes its pollers", int, h_sk_EventFdWrite, int s, unsigned long v) { int rv = dm_net_enabled() ? dm_net_eventfd_write(s, v) : efd_write(s, v); return (int)dm_net_guest_errno(rv); }
// Resolver options (nn::socket::ResolverOption: its key is the first word). The host resolver has none to apply: setting one is accepted and not applied, and reading one fails,
// as there is no value to give. Each key is logged once, so a game that depends on one shows it
static int sk_res_option(const void *o, int key, int set) {
    if (set) key = o ? *(const int32_t *)o : -1;
    static _Atomic uint64_t seen[2]; uint64_t bit = key >= 0 && key < 64 ? 1ull << key : 1ull << 63;
    if (!(atomic_fetch_or(&seen[set], bit) & bit)) LOG("%ssocket: resolver option %d %s", set ? "" : "[RED] ", key, set ? "set: accepted, the host resolver does not apply it" : "read: the host resolver has none, fails");
    if (set && dm_net_enabled() && o) return 0;
    return sk_fail();
}
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; return sk_res_option(NULL, k, 0); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { return sk_res_option(o, 0, 1); }
HLE_FORCE_WAIT("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "socket transport, offline by default", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return dm_net_guest_errno(dm_net_recv(s, b, n, f, NULL, NULL)); }
HLE_FORCE_WAIT("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "socket transport, offline by default", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_net_guest_errno(dm_net_recv(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "socket transport, offline by default", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return dm_net_guest_errno(dm_net_send(s, b, n, f, NULL, 0)); }
HLE_FORCE_WAIT("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    return dm_net_getaddrinfo(h, s, hint, res);
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "release only resolver allocations owned by the host", void, h_sk_FreeAddrInfo, void *ai) { dm_net_freeaddrinfo(ai); }
HLE_FORCE_WAIT("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution, offline by default", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_net_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return dm_net_errno(); }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return dm_net_guest_errno(dm_net_socket(a, b, c)); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return dm_net_guest_errno(dm_net_bind(s, a, n)); }
SKW("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { return dm_net_guest_errno(dm_net_connect(s, a, n)); }
SKW("nnsocketClose", h_nsk_Close, int s) { if (efd_used(s)) return efd_close(s); return dm_net_guest_errno(dm_net_close(s)); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 1)); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 0)); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_net_guest_errno(dm_net_sockopt(s, l, o, v, n, 0)); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_net_guest_errno(dm_net_sockopt(s, l, o, (void *)v, &n, 1)); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, int flags) { return dm_net_guest_errno(dm_net_fcntl(s, c, flags)); }
SKW("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_net_guest_errno(dm_sk_poll(f, n, t)); }
HLE_FORCE_WAIT("nnsocketGetHostByNameCancel", "real IPv4 resolution with an independent cancellation ID", void *, h_nsk_GetHostCancel, const char *name, uint32_t id) { return dm_net_gethostbyname_cancel(name, id); }
HLE_FORCE_WAIT("_ZN2nn6socket13GetHostByNameEPKci", "real IPv4 resolution with an independent cancellation ID", void *, h_sk_GetHostCancel, const char *name, uint32_t id) { return dm_net_gethostbyname_cancel(name, id); }
HLE_FORCE("nnsocketRequestCancelHandle", "allocate a nonzero resolver cancellation identity, not a socket", uint32_t, h_nsk_RequestCancel, void) { return dm_net_request_cancel_handle(); }
HLE_FORCE("_ZN2nn6socket19RequestCancelHandleEv", "allocate a nonzero resolver cancellation identity, not a socket", uint32_t, h_sk_RequestCancel, void) { return dm_net_request_cancel_handle(); }
SKW("nnsocketCancel", h_nsk_Cancel, uint32_t id) { return dm_net_guest_errno(dm_net_cancel(id)); }
SKW("_ZN2nn6socket6CancelEi", h_sk_Cancel, uint32_t id) { return dm_net_guest_errno(dm_net_cancel(id)); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; return sk_res_option(NULL, k, 0); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { return sk_res_option(o, 0, 1); }
HLE_FORCE_WAIT("nnsocketRecv", "socket transport, offline by default", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return dm_net_guest_errno(dm_net_recv(s, b, n, f, NULL, NULL)); }
HLE_FORCE_WAIT("nnsocketSend", "socket transport, offline by default", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return dm_net_guest_errno(dm_net_send(s, b, n, f, NULL, 0)); }
HLE_FORCE_WAIT("nnsocketGetHostByNameWithOptions", "name resolution, offline by default", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_net_gethostbyname(n); }
HLE_FORCE("nnsocketGetLastErrno", "the game's errno, as nn::socket::GetLastErrno reads it", int, h_nsk_GetErrno, void) { return dm_net_guest_errno_get(); }
HLE_FORCE("nnsocketSetLastErrno", "sets the host's copy and the game's errno", void, h_nsk_SetErrno, int e) { dm_net_set_errno(e); dm_net_guest_errno(-1); }
// ---------- ssl: hle_ssl.c (the client TLS service, offline and online) ----------
