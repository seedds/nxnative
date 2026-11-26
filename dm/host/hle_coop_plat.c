// coop platform layer (MIT, our own code): sockets, threads, locks, time and randomness. Winsock + Win32 on Windows, BSD sockets + pthreads
// elsewhere (macOS, iOS, Android, Linux). Every socket the transport opens is non-blocking.
#ifdef _WIN32
#define _CRT_RAND_S   // rand_s, before stdlib.h
#endif
#include "coop_int.h"

#ifdef _WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
static void set_nonblock(csock s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static int would_block(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR; }
int cplat_init(void) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) ? -1 : 0; }
void cplat_fini(void) { WSACleanup(); }
void cplat_mutex_init(cmutex *m) { InitializeSRWLock(m); }
void cplat_mutex_destroy(cmutex *m) { (void)m; }
void cplat_lock(cmutex *m) { AcquireSRWLockExclusive(m); }
void cplat_unlock(cmutex *m) { ReleaseSRWLockExclusive(m); }
typedef struct { void (*fn)(void *); void *arg; } tramp;
static DWORD WINAPI thread_tramp(LPVOID p) { tramp t = *(tramp *)p; free(p); t.fn(t.arg); return 0; }
int cplat_thread_start(cthread *t, void (*fn)(void *), void *arg) {
    tramp *p = malloc(sizeof *p); if (!p) return -1;
    p->fn = fn; p->arg = arg;
    *t = CreateThread(NULL, 0, thread_tramp, p, 0, NULL);
    if (!*t) { free(p); return -1; }
    return 0;
}
void cplat_thread_join(cthread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
int64_t cplat_now_ms(void) { return (int64_t)GetTickCount64(); }
void cplat_sleep_ns(int64_t ns) { Sleep(ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000)); }
void cplat_random(void *p, size_t n) {
    uint8_t *b = p;
    for (size_t i = 0; i < n; i += 4) { unsigned v = 0; rand_s(&v); for (size_t k = 0; k < 4 && i + k < n; k++) b[i + k] = (uint8_t)(v >> 8 * k); }
}
void cplat_close(csock s) { if (s != CSOCK_BAD) closesocket(s); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static void set_nonblock(csock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
int cplat_init(void) { return 0; }
void cplat_fini(void) {}
void cplat_mutex_init(cmutex *m) { pthread_mutex_init(m, NULL); }
void cplat_mutex_destroy(cmutex *m) { pthread_mutex_destroy(m); }
void cplat_lock(cmutex *m) { pthread_mutex_lock(m); }
void cplat_unlock(cmutex *m) { pthread_mutex_unlock(m); }
typedef struct { void (*fn)(void *); void *arg; } tramp;
static void *thread_tramp(void *p) { tramp t = *(tramp *)p; free(p); t.fn(t.arg); return NULL; }
int cplat_thread_start(cthread *t, void (*fn)(void *), void *arg) {
    tramp *p = malloc(sizeof *p); if (!p) return -1;
    p->fn = fn; p->arg = arg;
    if (pthread_create(t, NULL, thread_tramp, p)) { free(p); return -1; }
    return 0;
}
void cplat_thread_join(cthread t) { pthread_join(t, NULL); }
int64_t cplat_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
void cplat_sleep_ns(int64_t ns) { if (ns <= 0) return; struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)}; while (nanosleep(&ts, &ts) && errno == EINTR) {} }
void cplat_random(void *p, size_t n) {
#if defined(__APPLE__) || defined(__ANDROID__) || defined(__FreeBSD__)
    arc4random_buf(p, n);
#else
    int fd = open("/dev/urandom", O_RDONLY); size_t got = 0;
    if (fd >= 0) { while (got < n) { ssize_t k = read(fd, (uint8_t *)p + got, n - got); if (k <= 0) break; got += (size_t)k; } close(fd); }
    uint64_t x = (uint64_t)cplat_now_ms() ^ (uint64_t)(uintptr_t)p;   // only if /dev/urandom failed
    for (; got < n; got++) { x += 0x9E3779B97F4A7C15ull; uint64_t z = x; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; ((uint8_t *)p)[got] = (uint8_t)(z ^ (z >> 31)); }
#endif
}
void cplat_close(csock s) { if (s != CSOCK_BAD) close(s); }
#endif

static void sin_set(struct sockaddr_in *a, uint32_t ip, uint16_t port) {
    memset(a, 0, sizeof *a); a->sin_family = AF_INET; a->sin_port = htons(port); a->sin_addr.s_addr = htonl(ip);
}
static void tcp_opts(csock s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    set_nonblock(s);
}
static uint16_t bound_port(csock s) { struct sockaddr_in a; socklen_t l = sizeof a; if (getsockname(s, (struct sockaddr *)&a, &l)) return 0; return ntohs(a.sin_port); }

csock cplat_udp_open(uint32_t ip, uint16_t port, uint16_t *bound) {
    csock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == CSOCK_BAD) return CSOCK_BAD;
    int buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&buf, sizeof buf);
#ifdef _WIN32
    BOOL off = FALSE; DWORD got = 0;   // an ICMP port-unreachable would otherwise fail the next recvfrom with WSAECONNRESET
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
#endif
    struct sockaddr_in a; sin_set(&a, ip, port);
    if (bind(s, (struct sockaddr *)&a, sizeof a)) { cplat_close(s); return CSOCK_BAD; }
    set_nonblock(s);
    if (bound) *bound = bound_port(s);
    return s;
}
csock cplat_tcp_listen(uint32_t ip, uint16_t port, uint16_t *bound) {
    csock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == CSOCK_BAD) return CSOCK_BAD;
    int one = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   // a restarted host rebinds while old links sit in TIME_WAIT
#endif
    struct sockaddr_in a; sin_set(&a, ip, port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { cplat_close(s); return CSOCK_BAD; }
    set_nonblock(s);
    if (bound) *bound = bound_port(s);
    return s;
}
csock cplat_tcp_accept(csock l, uint32_t *ip, uint16_t *port) {
    struct sockaddr_storage a; socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    csock s = accept(l, (struct sockaddr *)&a, &al);
    if (s == CSOCK_BAD) return CSOCK_BAD;
    tcp_opts(s);
    *ip = 0; *port = 0;
    if (a.ss_family == AF_INET) { struct sockaddr_in *v = (struct sockaddr_in *)&a; *ip = ntohl(v->sin_addr.s_addr); *port = ntohs(v->sin_port); }
    return s;
}
csock cplat_tcp_connect(uint32_t ip, uint16_t port) {
    csock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == CSOCK_BAD) return CSOCK_BAD;
    tcp_opts(s);
    struct sockaddr_in a; sin_set(&a, ip, port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) && !would_block()) { cplat_close(s); return CSOCK_BAD; }
    return s;
}
int cplat_tcp_connected(csock s) {
    int e = 0; socklen_t l = sizeof e;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&e, &l) || e) return -1;
    return 1;
}
int64_t cplat_send(csock s, const void *p, size_t n) {
#ifdef _WIN32
    int k = send(s, (const char *)p, n > 0x7fffffff ? 0x7fffffff : (int)n, 0);
#else
    int fl = 0;
#ifdef MSG_NOSIGNAL
    fl = MSG_NOSIGNAL;
#endif
    ssize_t k = send(s, p, n, fl);
#endif
    if (k >= 0) return (int64_t)k;
    return would_block() ? -1 : -2;
}
int64_t cplat_recv(csock s, void *p, size_t n) {
#ifdef _WIN32
    int k = recv(s, (char *)p, n > 0x7fffffff ? 0x7fffffff : (int)n, 0);
#else
    ssize_t k = recv(s, p, n, 0);
#endif
    if (k >= 0) return (int64_t)k;
    return would_block() ? -1 : -2;
}
int cplat_sendto(csock s, const void *p, size_t n, uint32_t ip, uint16_t port) {
    struct sockaddr_in a; sin_set(&a, ip, port);
#ifdef _WIN32
    int k = sendto(s, (const char *)p, (int)n, 0, (struct sockaddr *)&a, sizeof a);
#else
    ssize_t k = sendto(s, p, n, 0, (struct sockaddr *)&a, sizeof a);
#endif
    return k >= 0 || would_block() ? 0 : -2;
}
int64_t cplat_recvfrom(csock s, void *p, size_t n, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in a; socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
#ifdef _WIN32
    int k = recvfrom(s, (char *)p, (int)n, 0, (struct sockaddr *)&a, &l);
    if (k < 0) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK ? -1 : e == WSAECONNRESET || e == WSAEMSGSIZE ? -3 : -2; }   // both consumed what they report
#else
    ssize_t k = recvfrom(s, p, n, 0, (struct sockaddr *)&a, &l);
    if (k < 0) return would_block() ? -1 : errno == ECONNREFUSED ? -3 : -2;
#endif
    if (a.sin_family != AF_INET) return -3;
    *ip = ntohl(a.sin_addr.s_addr); *port = ntohs(a.sin_port);
    return (int64_t)k;
}
#ifdef _WIN32
#define COBJMACROS
#include <objbase.h>
#include <netlistmgr.h>
#include <iphlpapi.h>
#include <stdio.h>
static const GUID k_clsid_nlm = {0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};   // CLSID_NetworkListManager
static const GUID k_iid_nlm = {0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};    // IID_INetworkListManager
static int public_adapters(char (*out)[40], int max) {   // read only: the adapters whose network has the Public profile, as "{GUID}" (GetAdaptersAddresses' name)
    HRESULT hi = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    INetworkListManager *m = NULL; IEnumNetworkConnections *e = NULL; int k = 0;
    if (SUCCEEDED(CoCreateInstance(&k_clsid_nlm, NULL, CLSCTX_ALL, &k_iid_nlm, (void **)&m))) {
        if (SUCCEEDED(INetworkListManager_GetNetworkConnections(m, &e))) {
            INetworkConnection *c = NULL; ULONG got = 0;
            while (IEnumNetworkConnections_Next(e, 1, &c, &got) == S_OK && got) {
                INetwork *net = NULL; NLM_NETWORK_CATEGORY cat; GUID g;
                if (SUCCEEDED(INetworkConnection_GetNetwork(c, &net))) {
                    if (SUCCEEDED(INetwork_GetCategory(net, &cat)) && cat == NLM_NETWORK_CATEGORY_PUBLIC && k < max && SUCCEEDED(INetworkConnection_GetAdapterId(c, &g)))
                        snprintf(out[k++], 40, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                                 g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
                    INetwork_Release(net);
                }
                INetworkConnection_Release(c);
            }
            IEnumNetworkConnections_Release(e);
        }
        INetworkListManager_Release(m);
    }
    if (SUCCEEDED(hi)) CoUninitialize();
    return k;
}
int cplat_local_addrs(caddr *out, int max, int profiles) {
    ULONG len = 16384, r = ERROR_BUFFER_OVERFLOW; IP_ADAPTER_ADDRESSES *aa = NULL;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(aa);
        if (!(aa = malloc(len))) return -1;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, aa, &len);
    }
    if (r != NO_ERROR) { free(aa); return -1; }
    char pub[16][40]; int np = profiles ? public_adapters(pub, 16) : 0, k = 0;
    for (IP_ADAPTER_ADDRESSES *a = aa; a && k < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        int is_pub = 0;
        for (int i = 0; i < np; i++) if (a->AdapterName && !_stricmp(pub[i], a->AdapterName)) is_pub = 1;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && k < max; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            uint32_t ip = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
            unsigned pl = u->OnLinkPrefixLength;
            if ((ip >> 24) == 127) continue;
            out[k].ip = ip; out[k].mask = !pl ? 0 : pl >= 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu << (32 - pl); out[k].is_public = is_pub; k++;
        }
    }
    free(aa);
    return k;
}
#else
#include <ifaddrs.h>
#include <net/if.h>
int cplat_local_addrs(caddr *out, int max, int profiles) {   // no network profiles: the platform asks for local network access itself (iOS, macOS)
    struct ifaddrs *ifs = NULL; int k = 0;
    (void)profiles;
    if (getifaddrs(&ifs)) return -1;
    for (struct ifaddrs *i = ifs; i && k < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        out[k].ip = ntohl(((const struct sockaddr_in *)(const void *)i->ifa_addr)->sin_addr.s_addr);
        out[k].mask = i->ifa_netmask ? ntohl(((const struct sockaddr_in *)(const void *)i->ifa_netmask)->sin_addr.s_addr) : 0;
        out[k].is_public = 0; k++;
    }
    freeifaddrs(ifs);
    return k;
}
#endif
int cplat_route_ip(uint32_t *ip) {   // connect() on a UDP socket picks the source address and sends nothing
    static const uint32_t to[2] = {0x08080808u, 0xE00000FBu};   // a public address (the default route), else the mDNS group 224.0.0.251
    for (int i = 0; i < 2; i++) {
        csock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == CSOCK_BAD) return -1;
        struct sockaddr_in a, b; socklen_t l = sizeof b; sin_set(&a, to[i], i ? 5353 : 53);
        int ok = !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&b, &l) && b.sin_addr.s_addr != 0;
        cplat_close(s);
        if (ok) { *ip = ntohl(b.sin_addr.s_addr); return 0; }
    }
    return -1;
}
int cplat_poll(cpollfd *f, int n, int ms) {
#ifdef _WIN32
    fd_set r, w, e; int any = 0;
    FD_ZERO(&r); FD_ZERO(&w); FD_ZERO(&e);
    for (int i = 0; i < n; i++) {
        f[i].got = 0;
        if (f[i].s == CSOCK_BAD) continue;
        if (f[i].want & CP_IN) { FD_SET(f[i].s, &r); any = 1; }
        if (f[i].want & CP_OUT) { FD_SET(f[i].s, &w); FD_SET(f[i].s, &e); any = 1; }   // a failed connect shows in the except set
    }
    if (!any) { Sleep(ms > 0 ? (DWORD)ms : 0); return 0; }   // select refuses three empty sets
    struct timeval tv = {ms / 1000, ms % 1000 * 1000};
    int k = select(0, &r, &w, &e, &tv);
    if (k < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (f[i].s == CSOCK_BAD) continue;
        if (FD_ISSET(f[i].s, &r)) f[i].got |= CP_IN;
        if (FD_ISSET(f[i].s, &w)) f[i].got |= CP_OUT;
        if (FD_ISSET(f[i].s, &e)) f[i].got |= CP_ERR;
    }
    return k;
#else
    struct pollfd p[64]; if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        p[i].fd = f[i].s; p[i].revents = 0; f[i].got = 0;
        p[i].events = (short)((f[i].want & CP_IN ? POLLIN : 0) | (f[i].want & CP_OUT ? POLLOUT : 0));
    }
    int k = poll(p, (nfds_t)n, ms);
    if (k < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        if (p[i].revents & (POLLIN | POLLHUP)) f[i].got |= CP_IN;   // a hang-up reads as end of stream
        if (p[i].revents & POLLOUT) f[i].got |= CP_OUT;
        if (p[i].revents & (POLLERR | POLLNVAL)) f[i].got |= CP_ERR;
    }
    return k;
#endif
}
