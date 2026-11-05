// Networking (out of scope: online play/friends/store/cloud saves): all hooked up as "network unavailable", the game takes the offline branch.
// Cuts: nifm reports no network; socket only hooks up Initialize (returns success, creates no session), the rest of socket/ssl/friends are hooked up as failures when they come into use.
#include "hle.h"
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

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
static _Atomic int g_nifm_hold;   // queries the pending request still answers "on hold" (0 = none, or settled)
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "offline: the request is on hold for the first query, then settles as unavailable", void, h_nifm_Submit, void) { atomic_store(&g_nifm_hold, 1); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "offline: drops the pending request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "offline: on hold for the first query after the submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "offline: the network is always unavailable", int, h_nifm_Available, void) { return 0; }
// Returns bool (`and w0, w0, #1`, Metroid Dread's sdk+0x336210): whether the system's network screen resolved the error
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "offline: not resolved, shows no system network screen", unsigned, h_nifm_HandleErr, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "offline: no IP", Result, h_nifm_GetIp, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "offline: no proxy setting", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "connects to the ssl service; the network library's global init needs it to succeed, offline creates no session (cut)", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "pairs with nnsslInitialize (cut)", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- socket: offline, all calls that contact the service return -1 in BSD convention, error code ENETDOWN ----------
// Not cut: Inet* / nnsocketInet* byte order and address text conversion (pure computation).
#define SK_ENETDOWN 50                  // Horizon sockets use FreeBSD error codes
static __thread int t_skerr = SK_ENETDOWN;
static int sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
#define SKF(m, cname, ...) HLE_FORCE(m, "bsd socket service (cut): returns -1/ENETDOWN", int, cname, __VA_ARGS__)
// Poll / Select with nothing to wait on: the host creates no socket, so a game's descriptors are all -1, or it passes none. POSIX and Horizon's bsd service ignore negative
// descriptors and wait out the timeout, returning 0. Failing at once made Dark Souls Remastered's network thread ("NexusRevolution Socket", main+0x92c7c4) spin a core at 100%
// on Poll(fds, 0, 33 ms), 11,000 calls a second. An endless wait on nothing never returns on hardware; here it returns 0 after 100 ms: a thread in a host hook pauses for a garbage
// collector (IL2CPP's, hle_thread.c) only once the hook returns, and the caller can still see a shutdown
void dm_guest_sleep_ns(int64_t ns);
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }

// ---------- eventfd: a local counter, not networking, so it is served offline as well ----------
// nn::socket::EventFd is Linux's eventfd on the bsd service: a 64-bit counter that write adds to and read takes (all of it, or 1 with the semaphore flag), readable while
// it is not zero. On a console it works with no network. gRPC (in Nintendo's online library, NPLN) wakes its poller through one: The Legend of Zelda: Ocarina of Time
// (0400486024DEC000) starts that library at boot, its poll engine's initializer (main+0x21f1280) found no wakeup fd while every socket call failed, no engine started and
// gRPC aborted (main+0x21f0c7c). Descriptors 512..575: below select's 1024, and the host makes no other descriptor. Flags as Horizon numbers them (gRPC passes 6:
// nonblocking + close-on-exec); fcntl's O_NONBLOCK, F_GETFL and F_SETFL are FreeBSD's. A failed read or write also sets the C library's errno, which gRPC reads
// (Linux numbers: EAGAIN 11, EINTR 4), beside the socket error code
enum { EFD_BASE = 512, EFD_MAX = 64, EFD_SEMAPHORE = 1, EFD_NONBLOCK = 4, SK_O_NONBLOCK = 4, SK_F_GETFD = 1, SK_F_SETFD = 2, SK_F_GETFL = 3, SK_F_SETFL = 4 };
enum { SK_EBADF = 9, SK_EAGAIN = 35, SK_EINVAL = 22, LIBC_EBADF = 9, LIBC_EAGAIN = 11, LIBC_EINVAL = 22 };
enum { SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4 };
static struct { int used, nonblock, sem; uint64_t count; } g_efd[EFD_MAX];
static pthread_mutex_t g_efd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_cv = PTHREAD_COND_INITIALIZER;   // broadcast on every change of a counter
static int efd_slot(int fd) { int i = fd - EFD_BASE; return i >= 0 && i < EFD_MAX && g_efd[i].used ? i : -1; }   // caller holds g_efd_mu
static int efd_err(int sk, int libc) {   // -1 with the socket error code and the guest's errno
    static int *(*loc)(void); t_skerr = sk;
    if (!loc) loc = guest_sym("__errno_location");
    if (loc) *loc() = libc;
    return -1;
}
static int efd_wait(int64_t ms) {   // waits for a change of any counter, at most ms (< 0: 100 ms, as an endless wait, sk_idle); 0 when the time is up. Caller holds g_efd_mu
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ns = ts.tv_nsec + (ms < 0 || ms > 100 ? 100 : ms) * 1000000;
    ts.tv_sec += ns / 1000000000; ts.tv_nsec = ns % 1000000000;
    return pthread_cond_timedwait(&g_efd_cv, &g_efd_mu, &ts) != ETIMEDOUT;
}
static int efd_close(int fd) {   // 1 when fd was an eventfd (now closed)
    pthread_mutex_lock(&g_efd_mu); int i = efd_slot(fd); if (i >= 0) g_efd[i].used = 0; pthread_mutex_unlock(&g_efd_mu);
    return i >= 0;
}
static int efd_fcntl(int fd, int cmd, long arg, int *out) {   // 1 when fd was an eventfd (*out: the result)
    pthread_mutex_lock(&g_efd_mu); int i = efd_slot(fd);
    if (i >= 0) {
        if (cmd == SK_F_GETFL) *out = g_efd[i].nonblock ? SK_O_NONBLOCK : 0;
        else if (cmd == SK_F_SETFL) { g_efd[i].nonblock = (arg & SK_O_NONBLOCK) != 0; *out = 0; }
        else if (cmd == SK_F_GETFD || cmd == SK_F_SETFD) *out = 0;   // close-on-exec: no exec here
        else *out = efd_err(SK_EINVAL, LIBC_EINVAL);
    }
    pthread_mutex_unlock(&g_efd_mu); return i >= 0;
}
HLE_FORCE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", "eventfd: a local counter (no network needed)", int, h_sk_EventFd, uint64_t v, int f) {
    pthread_mutex_lock(&g_efd_mu); int i = 0;
    while (i < EFD_MAX && g_efd[i].used) i++;
    if (i < EFD_MAX) { g_efd[i].used = 1; g_efd[i].count = v; g_efd[i].nonblock = (f & EFD_NONBLOCK) != 0; g_efd[i].sem = (f & EFD_SEMAPHORE) != 0; }
    pthread_mutex_unlock(&g_efd_mu);
    if (i == EFD_MAX) { LOG("[RED] eventfd: more than %d open", EFD_MAX); return efd_err(24, 24); }   // EMFILE
    { static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("eventfd: served by the host (first: descriptor %d, flags %#x)", EFD_BASE + i, f); }
    return EFD_BASE + i;
}
HLE_FORCE("_ZN2nn6socket11EventFdReadEiPm", "eventfd: takes the counter (or 1 with the semaphore flag)", int, h_sk_EventFdRead, int fd, uint64_t *v) {
    pthread_mutex_lock(&g_efd_mu); int i;
    while ((i = efd_slot(fd)) >= 0 && !g_efd[i].count && !g_efd[i].nonblock) efd_wait(-1);
    int r = i < 0 ? -2 : !g_efd[i].count ? -1 : 0;
    if (!r) { uint64_t take = g_efd[i].sem ? 1 : g_efd[i].count; g_efd[i].count -= take; if (v) *v = take; pthread_cond_broadcast(&g_efd_cv); }
    pthread_mutex_unlock(&g_efd_mu);
    return r == -2 ? efd_err(SK_EBADF, LIBC_EBADF) : r ? efd_err(SK_EAGAIN, LIBC_EAGAIN) : 0;
}
HLE_FORCE("_ZN2nn6socket12EventFdWriteEim", "eventfd: adds to the counter", int, h_sk_EventFdWrite, int fd, uint64_t v) {
    pthread_mutex_lock(&g_efd_mu); int i;
    if (v == UINT64_MAX) { pthread_mutex_unlock(&g_efd_mu); return efd_err(SK_EINVAL, LIBC_EINVAL); }
    while ((i = efd_slot(fd)) >= 0 && g_efd[i].count > UINT64_MAX - 1 - v && !g_efd[i].nonblock) efd_wait(-1);
    int r = i < 0 ? -2 : g_efd[i].count > UINT64_MAX - 1 - v ? -1 : 0;
    if (!r) { g_efd[i].count += v; pthread_cond_broadcast(&g_efd_cv); }
    pthread_mutex_unlock(&g_efd_mu);
    return r == -2 ? efd_err(SK_EBADF, LIBC_EBADF) : r ? efd_err(SK_EAGAIN, LIBC_EAGAIN) : 0;
}

typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
static int efd_ready(int i, int ev) {   // the poll events an eventfd has now. Caller holds g_efd_mu
    return (ev & (SK_POLLIN | SK_POLLRDNORM) & (g_efd[i].count ? ~0 : 0)) | (ev & (SK_POLLOUT | SK_POLLWRNORM) & (g_efd[i].count < UINT64_MAX - 1 ? ~0 : 0));
}
// Poll: eventfds are waited on (in slices of at most 100 ms, as an endless wait, sk_idle). A descriptor the host never made (it creates no socket) reports no event: Star Fox's HTTP
// thread polls two with a zero timeout, its wake-up eventfd that failed (-1) and a garbage one, and takes any failure but EINTR as fatal (main+0xbe2efc4). On a console a socket with
// nothing to say reports no event at the timeout, so that is the answer for every such descriptor: nothing ready, the timeout waited out, 0. Failing with ENETDOWN here made the loop abort
int dm_sk_poll(void *fds, uint64_t n, int ms) {   // also hle_offline.c's plain pollfd form
    sk_pollfd *p = fds; int any = 0;
    pthread_mutex_lock(&g_efd_mu);
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) {
        if (efd_slot(p[i].fd) >= 0) { any = 1; continue; }
        static int once; if (!once++) LOG("socket: Poll of %llu descriptors with one the host never made (fd %d, events %#x; %d ms): nothing ready at the timeout", (unsigned long long)n, p[i].fd, p[i].events, ms);
    }
    if (!any) { pthread_mutex_unlock(&g_efd_mu); for (uint64_t i = 0; i < n; i++) p[i].revents = 0; return sk_idle(ms); }
    int ready;
    for (int64_t left = ms;;) {
        ready = 0;
        for (uint64_t i = 0; i < n; i++) { int k = p[i].fd >= 0 ? efd_slot(p[i].fd) : -1; p[i].revents = (int16_t)(k >= 0 ? efd_ready(k, p[i].events) : 0); ready += p[i].revents != 0; }
        if (ready || !left || !efd_wait(left)) break;   // ready, a zero timeout, or a slice passed with no change
        if (left > 0) left = left > 100 ? left - 100 : 0;
    }
    pthread_mutex_unlock(&g_efd_mu);
    return ready;
}
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 seconds, i64 microseconds}
    uint64_t *sets[3] = {(uint64_t *)r, (uint64_t *)w, (uint64_t *)e}, want[3][16] = {{0}}; int any = 0;
    if (nfds > 1024) nfds = 1024;
    pthread_mutex_lock(&g_efd_mu);
    for (int k = 0; k < 3; k++) for (int fd = 0; sets[k] && fd < nfds; fd++) if (sets[k][fd >> 6] >> (fd & 63) & 1) {
        if (efd_slot(fd) < 0) { pthread_mutex_unlock(&g_efd_mu); return sk_fail(); }
        want[k][fd >> 6] |= 1ull << (fd & 63); any = 1;
    }
    pthread_mutex_unlock(&g_efd_mu);
    int64_t ms = tv ? tv[0] * 1000 + tv[1] / 1000 : -1;
    if (!any) return sk_idle(ms);
    sk_pollfd p[EFD_MAX * 3]; int np = 0, ev[3] = {SK_POLLIN, SK_POLLOUT, 0};
    for (int k = 0; k < 3; k++) for (int fd = 0; fd < nfds; fd++) if (want[k][fd >> 6] >> (fd & 63) & 1 && np < EFD_MAX * 3) p[np++] = (sk_pollfd){fd, (int16_t)ev[k], 0};
    int got = dm_sk_poll(p, (uint64_t)np, (int)ms);
    if (got < 0) return got;
    for (int k = 0; k < 3; k++) if (sets[k]) for (int fd = 0; fd < nfds; fd++) if (want[k][fd >> 6] >> (fd & 63) & 1) sets[k][fd >> 6] &= ~(1ull << (fd & 63));
    for (int i = 0; i < np; i++) if (p[i].revents) { int k = p[i].events == SK_POLLIN ? 0 : 1; sets[k][p[i].fd >> 6] |= 1ull << (p[i].fd & 63); }
    return got;
}
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { (void)s; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return efd_close(s) ? 0 : sk_fail(); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, long a) { int r; return efd_fcntl(s, c, a, &r) ? r : sk_fail(); }
// SDK 21 overloads Star Fox imports: plain sockaddr, the ShutdownMethod enum, Read / Write and a name lookup without options; SetAllocator keeps nothing, as no session is created
SKF("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", h_sk_GetPeerName21, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", h_sk_Shutdown21, int s, int how) { (void)s; (void)how; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4ReadEiPvm", "bsd socket service (cut)", long, h_sk_Read, int s, void *b, unsigned long n) { (void)s; (void)b; (void)n; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket5WriteEiPKvm", "bsd socket service (cut)", long, h_sk_Write, int s, const void *b, unsigned long n) { (void)s; (void)b; (void)n; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKc", "name resolution (cut): NULL", void *, h_sk_GetHostEnt21, const char *n) { (void)n; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", "socket library allocator; no session is created, ignored", void, h_sk_SetAlloc, void *r) { (void)r; }
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
// The C library's poll (SDK 21's nnmusl, Star Fox): it needs the socket state of the initialization cut above and fails with EFAULT without it, which the game's loop takes as fatal, so it goes
// to the host's Poll too
SKF("poll", h_libc_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "bsd socket service (cut)", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "bsd socket service (cut)", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "bsd socket service (cut)", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    (void)h; (void)s; (void)hint; if (res) *res = NULL; return 4;   // EAI_FAIL
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAddrInfo, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution (cut): NULL", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { (void)a; (void)b; (void)c; return sk_fail(); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketClose", h_nsk_Close, int s) { return efd_close(s) ? 0 : sk_fail(); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return sk_fail(); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return sk_fail(); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, long a) { int r; return efd_fcntl(s, c, a, &r) ? r : sk_fail(); }
SKF("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "bsd socket service (cut)", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("nnsocketSend", "bsd socket service (cut)", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { (void)s; (void)b; (void)n; (void)f; return sk_fail(); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution (cut): NULL", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)n; (void)o; (void)c; t_skerr = SK_ENETDOWN; return NULL; }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- ssl: offline, Initialize and a context's Create / Destroy succeed, everything else that contacts the service fails ----------
// A context is a session of the local ssl service, so on Horizon Create works with no network and only a connection needs one; DRAGON QUEST BUILDERS 2 ends its startup when Create fails (main+0x861260).
// Not cut: Context / Connection construction and destruction, Context::GetContextId (only read and write game objects).
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "same as nnsslInitialize: succeeds, creates no session (cut)", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "pairs with Initialize (cut)", Result, h_ssl_FinCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", "a session of the local ssl service: succeeds offline, connects to nothing (cut)", Result, h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context7DestroyEv", "pairs with Create (cut)", Result, h_ssl_CtxDestroy, void *self) { (void)self; return RES_OK; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) { (void)self; (void)ctx; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) { (void)self; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) { (void)self; (void)h; (void)n; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) { (void)self; (void)m; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) { (void)self; (void)s; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) { (void)self; if (r) *r = RES_FAIL; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) { (void)self; (void)b; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) { (void)self; (void)o; (void)i; (void)t; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl service (cut): read returns -1", int, h_ssl_CnRead, void *self, char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl service (cut): write returns -1", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) { (void)self; (void)b; (void)n; return -1; }
// C interface: all return nnResult
#define SSC(m, cname) HLE_FORCE(m, "ssl service (cut): fails", Result, cname, void) { return RES_FAIL; }
SSC("nnsslConnectionCreate", h_nssl_Create) SSC("nnsslConnectionDestroy", h_nssl_Destroy)
SSC("nnsslConnectionDoHandshakeWithCertBuffer", h_nssl_Handshake) SSC("nnsslConnectionGetConnectionId", h_nssl_GetId)
SSC("nnsslConnectionGetNextAlpnProto", h_nssl_GetAlpn) SSC("nnsslConnectionGetVerifyCertError", h_nssl_VerifyErr)
SSC("nnsslConnectionPeek", h_nssl_Peek) SSC("nnsslConnectionPending", h_nssl_Pending) SSC("nnsslConnectionRead", h_nssl_Read)
SSC("nnsslConnectionSetHostName", h_nssl_SetHost) SSC("nnsslConnectionSetIoMode", h_nssl_SetIo)
SSC("nnsslConnectionSetNextAlpnProto", h_nssl_SetAlpn) SSC("nnsslConnectionSetOption", h_nssl_SetOpt)
SSC("nnsslConnectionSetSessionCacheMode", h_nssl_SetCache) SSC("nnsslConnectionSetSocketDescriptor", h_nssl_SetSock)
SSC("nnsslConnectionSetVerifyOption", h_nssl_SetVerify) SSC("nnsslConnectionWrite", h_nssl_Write)
SSC("nnsslContextDestroy", h_nssl_CtxDestroy) SSC("nnsslContextGetContextId", h_nssl_CtxGetId)
