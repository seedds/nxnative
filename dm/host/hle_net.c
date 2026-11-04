// Opt-in local IPv4 sockets and NIFM state. Online account, friends and TLS services retain their offline behavior.
#include "hle.h"
#include "net_socket.h"
#include <stdatomic.h>
void dm_guest_sleep_ns(int64_t ns);

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
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
static int sk_fail(void) { dm_net_set_errno(dm_net_enabled() ? 95 : SK_ENETDOWN); return -1; }
#define SKF(m, cname, ...) HLE_FORCE(m, "socket transport: unavailable by default, translated Darwin sockets in explicit Mac LAN mode", int, cname, __VA_ARGS__)
#define SKW(m, cname, ...) HLE_FORCE_WAIT(m, "blocking socket transport with a pausable guest context", int, cname, __VA_ARGS__)
// Poll / Select with nothing to wait on: the host creates no socket, so a game's descriptors are all -1, or it passes none. POSIX and Horizon's bsd service ignore negative
// descriptors and wait out the timeout, returning 0. Failing at once made Dark Souls Remastered's network thread ("NexusRevolution Socket", main+0x92c7c4) spin a core at 100%
// on Poll(fds, 0, 33 ms), 11,000 calls a second. An endless wait on nothing never returns;
// HLE_FORCE_WAIT publishes the guest context so a collector can pause the thread while it waits.
void dm_guest_sleep_ns(int64_t ns);
static int sk_idle(int64_t ms) { if (ms < 0) return dm_net_idle(); if (ms > INT64_MAX / 1000000) { dm_net_set_errno(22); return -1; } dm_guest_sleep_ns(ms * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
int dm_sk_poll(void *fds, uint64_t n, int ms) {   // also hle_offline.c's plain pollfd form
    if (dm_net_enabled()) return dm_net_poll(fds, n, ms);
    if (n > 1024 || (!fds && n)) { dm_net_set_errno(!fds && n ? 14 : 22); return -1; }
    sk_pollfd *p = fds;
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0) return sk_fail();
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    return sk_idle(ms);
}
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv) {   // fd_set: 1024 bits; timeval: {i64 seconds, i64 microseconds}
    if (dm_net_enabled()) return dm_net_select(nfds, (void *)r, (void *)w, (void *)e, tv);
    if (nfds < 0 || nfds > 1024 || (tv && (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000 || tv[0] > INT64_MAX / 1000))) { dm_net_set_errno(22); return -1; }
    const uint64_t *sets[3] = {r, w, e};
    for (int k = 0; k < 3; k++) for (int fd = 0; sets[k] && fd < nfds && fd < 1024; fd++) if (sets[k][fd >> 6] >> (fd & 63) & 1) return sk_fail();
    return sk_idle(tv ? tv[0] * 1000 + tv[1] / 1000 : -1);
}
SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return dm_net_socket(a, b, c); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return dm_net_bind(s, a, n); }
SKW("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { return dm_net_connect(s, a, n); }
SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { return dm_net_listen(s, n); }
SKW("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { return dm_net_accept(s, a, n); }
SKW("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return dm_net_close(s); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { return dm_net_name(s, a, n, 1); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return dm_net_name(s, a, n, 0); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_net_sockopt(s, l, o, v, n, 0); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_net_sockopt(s, l, o, (void *)v, &n, 1); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, int flags) { return dm_net_fcntl(s, c, flags); }
SKW("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKW("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
SKF("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", h_sk_EventFd, unsigned long v, int f) { (void)v; (void)f; return sk_fail(); }
SKF("_ZN2nn6socket11EventFdReadEiPm", h_sk_EventFdRead, int s, unsigned long *v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket12EventFdWriteEim", h_sk_EventFdWrite, int s, unsigned long v) { (void)s; (void)v; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE_WAIT("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "socket transport, offline by default", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return dm_net_recv(s, b, n, f, NULL, NULL); }
HLE_FORCE_WAIT("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "socket transport, offline by default", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_net_recv(s, b, n, f, a, al); }
HLE_FORCE_WAIT("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "socket transport, offline by default", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return dm_net_send(s, b, n, f, NULL, 0); }
HLE_FORCE_WAIT("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    return dm_net_getaddrinfo(h, s, hint, res);
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "release only resolver allocations owned by the host", void, h_sk_FreeAddrInfo, void *ai) { dm_net_freeaddrinfo(ai); }
HLE_FORCE_WAIT("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution, offline by default", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_net_gethostbyname(n); }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return dm_net_errno(); }
// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return dm_net_socket(a, b, c); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return dm_net_bind(s, a, n); }
SKW("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { return dm_net_connect(s, a, n); }
SKW("nnsocketClose", h_nsk_Close, int s) { return dm_net_close(s); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { return dm_net_name(s, a, n, 1); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return dm_net_name(s, a, n, 0); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_net_sockopt(s, l, o, v, n, 0); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_net_sockopt(s, l, o, (void *)v, &n, 1); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, int flags) { return dm_net_fcntl(s, c, flags); }
SKW("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE_WAIT("nnsocketRecv", "socket transport, offline by default", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return dm_net_recv(s, b, n, f, NULL, NULL); }
HLE_FORCE_WAIT("nnsocketSend", "socket transport, offline by default", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return dm_net_send(s, b, n, f, NULL, 0); }
HLE_FORCE_WAIT("nnsocketGetHostByNameWithOptions", "name resolution, offline by default", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return dm_net_gethostbyname(n); }
HLE_FORCE("nnsocketGetLastErrno", "socket error code kept by the host", int, h_nsk_GetErrno, void) { return dm_net_errno(); }
HLE_FORCE("nnsocketSetLastErrno", "socket error code kept by the host", void, h_nsk_SetErrno, int e) { dm_net_set_errno(e); }
