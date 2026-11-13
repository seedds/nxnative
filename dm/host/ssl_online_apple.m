// nn::ssl in online mode (DM_ONLINE, hle_online.c): the client TLS service over SecureTransport on the host socket behind the game's descriptor. FromSoftware's own
// client (The Duskbloods' phx-*.fromsoftware-game.net) does TLS through nnssl*, unlike gRPC, which links its own TLS. The engine and the SDK's semantics (results, I/O modes,
// the certificate buffer layout, Poll's events) follow #2589's hle_ssl_apple.m; the trust anchors are the server PKIs the game imports, DM_ONLINE_CA, and the system's roots.
// Guest objects are only registry keys: no SDK object fields are written. Offline the hooks in hle_net.c keep failing as before.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <Security/SecureTransport.h>
#include "dm.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // SecureTransport: the only public API that runs TLS over a descriptor the game already connected

enum { S_OK = 0, S_FAIL = 0x2a2, S_BAD_POINTER = 0xe27b, S_NO_SOCKET = 0xce7b, S_BAD_SOCKET = 0xd47b, S_WOULD_BLOCK = 0x1987b, S_TIMEOUT = 0x19a7b };

@interface OsslContext : NSObject
@property(nonatomic) uint64_t identifier;
@property(nonatomic) unsigned version;
@property(nonatomic) NSMutableDictionary<NSNumber *, NSArray *> *anchors;
@end
@implementation OsslContext
@end

@interface OsslConnection : NSObject {
@public
    _Atomic bool closed;
    int fd;          // a dup of the host socket
    SSLContextRef tls;
    short wanted;
}
@property(nonatomic) uint64_t identifier;
@property(nonatomic) OsslContext *context;
@property(nonatomic) NSLock *lock;
@property(nonatomic) int guest;
@property(nonatomic) NSString *host;
@property(nonatomic) unsigned ioMode, verifyOptions, sessionCache, timeout;
@property(nonatomic) BOOL chain, alpnOn, doNotClose, handshaking, established, verified;
@property(nonatomic) uint32_t lastError, verifyError;
@property(nonatomic) NSMutableData *lookahead;
@property(nonatomic) NSData *pendingWrite;
@property(nonatomic) NSArray<NSString *> *alpn;
@property(nonatomic) void *certBuffer;
@property(nonatomic) unsigned certCapacity;
@end
@implementation OsslConnection
- (void)dealloc { if (tls) { SSLClose(tls); CFRelease(tls); } if (fd >= 0) close(fd); }
@end

static NSMutableDictionary<NSValue *, OsslContext *> *g_ctx;
static NSMutableDictionary<NSValue *, OsslConnection *> *g_conn;
static pthread_mutex_t g_reg = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_next;
static void reg_init(void) { if (!g_ctx) { g_ctx = [NSMutableDictionary new]; g_conn = [NSMutableDictionary new]; } }
static NSValue *key(void *o) { return [NSValue valueWithPointer:o]; }
static OsslContext *get_ctx(void *o) {
    pthread_mutex_lock(&g_reg); reg_init(); OsslContext *c = g_ctx[key(o)];
    if (!c && g_ctx.count == 1) c = g_ctx.allValues.firstObject;   // the C interface's context may be another view of the C++ Context the game created
    pthread_mutex_unlock(&g_reg); return c;
}
static OsslConnection *get_conn(void *o) { pthread_mutex_lock(&g_reg); reg_init(); OsslConnection *c = g_conn[key(o)]; pthread_mutex_unlock(&g_reg); return c; }
static uint64_t clock_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }
static uint32_t err(OsslConnection *c, uint32_t v) { c.lastError = v; return v; }
static uint32_t status_result(OsslConnection *c, OSStatus s) { return err(c, !s ? 0 : s == errSSLWouldBlock ? S_WOULD_BLOCK : S_FAIL); }
static int host_poll(struct pollfd *p, int ms) { dm_thread_native_wait(1); int n = poll(p, 1, ms), e = errno; dm_thread_native_wait(0); errno = e; return n; }
static uint32_t wait_socket(OsslConnection *c, short events, uint64_t deadline) {
    for (;;) {
        if (atomic_load(&c->closed)) return err(c, S_BAD_SOCKET);
        uint64_t now = clock_ms(); if (now >= deadline) return err(c, S_TIMEOUT);
        uint64_t left = deadline - now; struct pollfd p = {.fd = c->fd, .events = events};
        int n = host_poll(&p, left > 100 ? 100 : (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return err(c, S_BAD_SOCKET);
        if (n) return 0;
    }
}
static OSStatus transport_io(SSLConnectionRef ref, void *bytes, size_t *size, bool reading) {
    __unsafe_unretained OsslConnection *c = (__bridge OsslConnection *)(void *)ref;
    size_t want = *size, done = 0; *size = 0;
    while (done < want) {
        if (atomic_load(&c->closed)) return errSSLClosedAbort;
        ssize_t n = reading ? recv(c->fd, (char *)bytes + done, want - done, 0) : send(c->fd, (char *)bytes + done, want - done, 0);
        if (n > 0) { done += (size_t)n; *size = done; continue; }
        if (!n && reading) return errSSLClosedAbort;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { c->wanted = reading ? POLLIN : POLLOUT; return errSSLWouldBlock; }
        return errSSLClosedAbort;
    }
    return noErr;
}
static OSStatus t_read(SSLConnectionRef c, void *p, size_t *n) { return transport_io(c, p, n, true); }
static OSStatus t_write(SSLConnectionRef c, const void *p, size_t *n) { return transport_io(c, (void *)p, n, false); }

static NSArray *parse_certs(const void *bytes, unsigned size, int format) {   // format 1 PEM, 2 DER
    if (!bytes || !size || size > 4 * 1024 * 1024) return nil;
    NSMutableArray *out = [NSMutableArray new];
    if (format == 2) {
        SecCertificateRef cert = SecCertificateCreateWithData(NULL, (__bridge CFDataRef)[NSData dataWithBytes:bytes length:size]);
        if (cert) [out addObject:CFBridgingRelease(cert)];
    } else {
        NSString *text = [[NSString alloc] initWithBytes:bytes length:size encoding:NSASCIIStringEncoding]; NSRange rest = NSMakeRange(0, text.length);
        while (text && rest.length) {
            NSRange a = [text rangeOfString:@"-----BEGIN CERTIFICATE-----" options:0 range:rest]; if (a.location == NSNotFound) break;
            NSUInteger p = NSMaxRange(a); NSRange b = [text rangeOfString:@"-----END CERTIFICATE-----" options:0 range:NSMakeRange(p, text.length - p)]; if (b.location == NSNotFound) break;
            NSData *der = [[NSData alloc] initWithBase64EncodedString:[text substringWithRange:NSMakeRange(p, b.location - p)] options:NSDataBase64DecodingIgnoreUnknownCharacters];
            SecCertificateRef cert = der ? SecCertificateCreateWithData(NULL, (__bridge CFDataRef)der) : NULL; if (cert) [out addObject:CFBridgingRelease(cert)];
            rest = NSMakeRange(NSMaxRange(b), text.length - NSMaxRange(b));
        }
    }
    return out.count ? out : nil;
}
static NSArray *online_ca(void) {   // DM_ONLINE_CA, DER or PEM
    static NSArray *ca; static dispatch_once_t once;
    dispatch_once(&once, ^{ size_t n = 0; const uint8_t *d = dm_online_ca_der(&n); ca = d ? parse_certs(d, (unsigned)n, 2) : @[]; });
    return ca;
}

uint32_t dm_ossl_context_create(void *o, unsigned version) { @autoreleasepool {
    if (!o) return S_BAD_POINTER;
    OsslContext *c = [OsslContext new]; c.version = version; c.anchors = [NSMutableDictionary new];
    pthread_mutex_lock(&g_reg); reg_init(); c.identifier = ++g_next; g_ctx[key(o)] = c; pthread_mutex_unlock(&g_reg);
    LOG("online: ssl context %llu (version %#x)", (unsigned long long)c.identifier, version); return 0;
} }
uint32_t dm_ossl_context_destroy(void *o) { @autoreleasepool { pthread_mutex_lock(&g_reg); reg_init(); [g_ctx removeObjectForKey:key(o)]; pthread_mutex_unlock(&g_reg); return 0; } }
uint32_t dm_ossl_context_id(void *o, uint64_t *out) { @autoreleasepool { if (!out) return S_BAD_POINTER; OsslContext *c = get_ctx(o); *out = c ? c.identifier : 0; return 0; } }
uint32_t dm_ossl_import_server(void *o, uint64_t *id, const void *bytes, unsigned size, int format) { @autoreleasepool {
    if (!id || !bytes) return S_BAD_POINTER; *id = 0; OsslContext *c = get_ctx(o); if (!c) return S_FAIL;
    NSArray *certs = parse_certs(bytes, size, format); if (!certs) return S_FAIL;
    pthread_mutex_lock(&g_reg); uint64_t n = ++g_next; c.anchors[@(n)] = certs; pthread_mutex_unlock(&g_reg); *id = n; return 0;
} }
uint32_t dm_ossl_connection_create(void *o, void *ctx) { @autoreleasepool {
    if (!o) return S_BAD_POINTER; OsslContext *parent = get_ctx(ctx); if (!parent) { LOG("[RED] online: ssl connection on an unknown context"); return S_FAIL; }
    OsslConnection *c = [OsslConnection new]; c.context = parent; c.lock = [NSLock new]; c.lookahead = [NSMutableData new]; c.guest = -1; c->fd = -1;
    c.ioMode = 1; c.verifyOptions = 3; c.timeout = 300000; atomic_init(&c->closed, false);
    pthread_mutex_lock(&g_reg); reg_init(); c.identifier = ++g_next; g_conn[key(o)] = c; pthread_mutex_unlock(&g_reg); return 0;
} }
uint32_t dm_ossl_connection_destroy(void *o) { @autoreleasepool {
    pthread_mutex_lock(&g_reg); reg_init(); OsslConnection *c = g_conn[key(o)]; if (c) { atomic_store(&c->closed, true); [g_conn removeObjectForKey:key(o)]; } pthread_mutex_unlock(&g_reg);
    if (!c) return S_FAIL;
    if (c.guest >= 0 && !c.doNotClose) dm_online_close(c.guest);   // the service owns the descriptor it was given unless DoNotCloseSocket
    return 0;
} }
uint32_t dm_ossl_connection_id(void *o, uint64_t *out, bool context) { @autoreleasepool { if (!out) return S_BAD_POINTER; OsslConnection *c = get_conn(o); *out = c ? (context ? c.context.identifier : c.identifier) : 0; return 0; } }
uint32_t dm_ossl_set_socket(void *o, int guest) { @autoreleasepool {
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    [c.lock lock]; int fd = dm_online_dup_host(guest);
    if (fd < 0 || c->fd >= 0) { if (fd >= 0) close(fd); [c.lock unlock]; return err(c, S_BAD_SOCKET); }
    c->fd = fd; c.guest = guest; [c.lock unlock]; return 0;
} }
uint32_t dm_ossl_set_hostname(void *o, const char *name, unsigned size) { @autoreleasepool {
    if (!name) return S_BAD_POINTER; if (!size || size > 255) return S_FAIL;
    if (!name[size - 1]) size--;
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    NSString *h = [[NSString alloc] initWithBytes:name length:size encoding:NSASCIIStringEncoding]; if (!h) return S_FAIL;
    [c.lock lock]; c.host = h; [c.lock unlock]; return 0;
} }
uint32_t dm_ossl_set_value(void *o, int what, unsigned v) { @autoreleasepool {   // 0 I/O mode (1 blocking, 2 non-blocking), 1 verify options, 2 session cache, 3 I/O timeout
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    [c.lock lock]; uint32_t r = 0;
    if (what == 0 && (v == 1 || v == 2)) c.ioMode = v; else if (what == 1) c.verifyOptions = v; else if (what == 2 && v <= 2) c.sessionCache = v; else if (what == 3 && v) c.timeout = v; else r = S_FAIL;
    [c.lock unlock]; return r;
} }
uint32_t dm_ossl_set_option(void *o, int option, unsigned v) { @autoreleasepool {   // 0 DoNotCloseSocket, 1 GetServerCertChain, 2 SkipDefaultVerify (verification stays on), 3 EnableAlpn
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL; v &= 1;
    [c.lock lock]; uint32_t r = 0;
    if (option == 0) c.doNotClose = v; else if (option == 1) c.chain = v; else if (option == 2) {} else if (option == 3) c.alpnOn = v; else r = S_FAIL;
    [c.lock unlock]; return r;
} }
uint32_t dm_ossl_set_alpn(void *o, const uint8_t *d, unsigned size) { @autoreleasepool {
    if (!d) return S_BAD_POINTER; OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    NSMutableArray *list = [NSMutableArray new];
    for (unsigned p = 0; p < size;) { unsigned n = d[p++]; if (!n || n > size - p) return S_FAIL; NSString *s = [[NSString alloc] initWithBytes:d + p length:n encoding:NSASCIIStringEncoding]; if (!s) return S_FAIL; [list addObject:s]; p += n; }
    [c.lock lock]; c.alpn = list; [c.lock unlock]; return 0;
} }
uint32_t dm_ossl_get_alpn(void *o, unsigned *state, uint8_t *buf, unsigned *size, unsigned cap) { @autoreleasepool {
    if (!state || !size) return S_BAD_POINTER; *state = *size = 0; OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    [c.lock lock]; CFArrayRef list = NULL; OSStatus s = c->tls ? SSLCopyALPNProtocols(c->tls, &list) : noErr;
    if (!s && list && CFArrayGetCount(list)) {
        NSData *d = [(__bridge NSString *)CFArrayGetValueAtIndex(list, 0) dataUsingEncoding:NSASCIIStringEncoding]; *size = (unsigned)d.length; *state = 1;
        if (cap < d.length || !buf) s = errSecParam; else memcpy(buf, d.bytes, d.length);
    }
    if (list) CFRelease(list); [c.lock unlock]; return status_result(c, s);
} }

static uint32_t prepare(OsslConnection *c) {
    if (c->fd < 0) return err(c, S_NO_SOCKET);
    if (!c.host.length) return err(c, S_FAIL);
    if (c->tls) return 0;
    c->tls = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType); if (!c->tls) return err(c, S_FAIL);
    OSStatus s = SSLSetProtocolVersionMin(c->tls, kTLSProtocol1);
    if (!s) s = SSLSetProtocolVersionMax(c->tls, kTLSProtocol12);
    if (!s) s = SSLSetIOFuncs(c->tls, t_read, t_write);
    if (!s) s = SSLSetConnection(c->tls, (__bridge SSLConnectionRef)c);
    if (!s) s = SSLSetPeerDomainName(c->tls, c.host.UTF8String, strlen(c.host.UTF8String));
    if (!s) s = SSLSetSessionOption(c->tls, kSSLSessionOptionBreakOnServerAuth, true);
    if (!s && c.alpnOn && c.alpn.count) s = SSLSetALPNProtocols(c->tls, (__bridge CFArrayRef)c.alpn);
    c.handshaking = YES; return status_result(c, s);
}
static BOOL verify_peer(OsslConnection *c) {
    SecTrustRef trust = NULL; if (SSLCopyPeerTrust(c->tls, &trust) || !trust) return NO;
    NSMutableArray *anchors = [NSMutableArray arrayWithArray:online_ca()];
    pthread_mutex_lock(&g_reg); for (NSArray *a in c.context.anchors.allValues) [anchors addObjectsFromArray:a]; pthread_mutex_unlock(&g_reg);
    SecPolicyRef policy = SecPolicyCreateSSL(true, (__bridge CFStringRef)c.host);
    OSStatus s = SecTrustSetPolicies(trust, policy); CFRelease(policy);
    if (!s) s = SecTrustSetNetworkFetchAllowed(trust, false);
    if (!s && anchors.count) { s = SecTrustSetAnchorCertificates(trust, (__bridge CFArrayRef)anchors); if (!s) s = SecTrustSetAnchorCertificatesOnly(trust, false); }
    CFErrorRef failure = NULL; dm_thread_native_wait(1); BOOL ok = !s && SecTrustEvaluateWithError(trust, &failure); dm_thread_native_wait(0);
    if (!ok) LOG("[RED] online: ssl %s: the server's certificate is not trusted (%s)", c.host.UTF8String, failure ? [[(__bridge NSError *)failure localizedDescription] UTF8String] : "?");
    if (failure) CFRelease(failure); CFRelease(trust); return ok;
}
static uint32_t handshake_locked(OsslConnection *c) {
    if (c.established) return 0;
    uint32_t r = prepare(c); if (r) return r;
    uint64_t deadline = clock_ms() + c.timeout;
    for (;;) {
        if (atomic_load(&c->closed)) return err(c, S_BAD_SOCKET);
        c->wanted = POLLIN; OSStatus s = SSLHandshake(c->tls);
        if (s == errSSLServerAuthCompleted || (!s && !c.verified)) {
            if (!verify_peer(c)) { c.verifyError = S_FAIL; SSLSetError(c->tls, errSSLXCertChainInvalid); return err(c, S_FAIL); }
            c.verified = YES; if (s) continue;
        }
        if (!s) { c.established = YES; LOG("online: ssl %s established", c.host.UTF8String); return err(c, 0); }
        if (s != errSSLWouldBlock || c.ioMode == 2) { if (s != errSSLWouldBlock) LOG("[RED] online: ssl %s handshake failed (%d)", c.host.UTF8String, (int)s); return status_result(c, s); }
        r = wait_socket(c, c->wanted, deadline); if (r) return r;
    }
}
static NSArray<NSData *> *peer_certs(OsslConnection *c) {
    SecTrustRef trust = NULL; if (!c->tls || SSLCopyPeerTrust(c->tls, &trust) || !trust) return nil;
    NSMutableArray *out = [NSMutableArray new]; CFIndex n = SecTrustGetCertificateCount(trust); if (!c.chain && n > 1) n = 1;
    for (CFIndex i = 0; i < n; i++) { SecCertificateRef cert = SecTrustGetCertificateAtIndex(trust, i); CFDataRef d = cert ? SecCertificateCopyData(cert) : NULL; if (d) [out addObject:CFBridgingRelease(d)]; }
    CFRelease(trust); return out;
}
// The server certificate buffer: one DER without GetServerCertChain, else {u64 magic, u32 count, pad, count x {u32 size, u32 offset}} then the DERs (#2589)
static uint32_t copy_certs(OsslConnection *c, unsigned *size, unsigned *count, void *buf, unsigned cap) {
    NSArray<NSData *> *certs = peer_certs(c); if (!certs.count) return S_FAIL;
    size_t need = c.chain ? 16 + 8 * certs.count : 0; for (NSData *d in certs) need += d.length;
    *size = (unsigned)need; *count = (unsigned)certs.count;
    if (!buf || cap < need) return S_FAIL;
    if (!c.chain) memcpy(buf, certs[0].bytes, need);
    else {
        uint64_t magic = 0x4e4d684374726543ull; uint32_t total = *count; memset(buf, 0, 16); memcpy(buf, &magic, 8); memcpy((char *)buf + 8, &total, 4);
        size_t off = 16 + 8 * certs.count;
        for (NSUInteger i = 0; i < certs.count; i++) { uint32_t e[2] = {(uint32_t)certs[i].length, (uint32_t)off}; memcpy((char *)buf + 16 + i * 8, e, 8); memcpy((char *)buf + off, certs[i].bytes, certs[i].length); off += certs[i].length; }
    }
    return 0;
}
uint32_t dm_ossl_handshake_certs(void *o, unsigned *size, unsigned *count) { @autoreleasepool {
    unsigned ns = 0, nc = 0; if (!size) size = &ns; if (!count) count = &nc; *size = *count = 0;
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    [c.lock lock]; uint32_t r = handshake_locked(c);
    if (!r && c.certBuffer && c.certCapacity) { dm_guest_write_prepare(c.certBuffer, c.certCapacity); r = copy_certs(c, size, count, c.certBuffer, c.certCapacity); }
    [c.lock unlock]; return err(c, r);
} }
uint32_t dm_ossl_set_cert_buffer(void *o, void *buf, unsigned cap) { @autoreleasepool { OsslConnection *c = get_conn(o); if (!c) return S_FAIL; [c.lock lock]; c.certBuffer = buf; c.certCapacity = cap; [c.lock unlock]; return 0; } }
uint32_t dm_ossl_cert_detail(void *out, const void *buf, unsigned index) {
    if (!out || !buf) return S_BAD_POINTER;
    uint64_t magic; uint32_t count; memcpy(&magic, buf, 8); memcpy(&count, (const char *)buf + 8, 4);
    if (magic != 0x4e4d684374726543ull || index >= count || count > 64) return S_FAIL;
    uint32_t e[2]; memcpy(e, (const char *)buf + 16 + index * 8, 8);
    struct { uint32_t size, pad; const void *data; } d = {e[0], 0, (const char *)buf + e[1]}; memcpy(out, &d, sizeof d); return 0;
}
uint32_t dm_ossl_read(void *o, void *buf, int *out, unsigned cap, bool peek) { @autoreleasepool {
    if (!out || (!buf && cap)) return S_BAD_POINTER; *out = 0;
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL; [c.lock lock];
    uint32_t r = c.established ? 0 : S_FAIL;
    if (!r && c.lookahead.length) {
        NSUInteger n = MIN(c.lookahead.length, cap); if (n) memcpy(buf, c.lookahead.bytes, n); *out = (int)n;
        if (!peek && n) [c.lookahead replaceBytesInRange:NSMakeRange(0, n) withBytes:NULL length:0];
    } else if (!r && cap) {
        unsigned want = peek ? MIN(cap, 16384u) : cap; NSMutableData *tmp = peek ? [NSMutableData dataWithLength:want] : nil; void *dst = peek ? tmp.mutableBytes : buf;
        if (!peek) dm_guest_write_prepare(buf, cap);
        uint64_t deadline = clock_ms() + c.timeout;
        for (;;) {
            if (atomic_load(&c->closed)) { r = S_BAD_SOCKET; break; }
            c->wanted = POLLIN; size_t n = 0; OSStatus s = SSLRead(c->tls, dst, want, &n);
            if (n || !s || s == errSSLClosedGraceful) { *out = (int)n; if (peek && n) { [c.lookahead appendBytes:dst length:n]; memcpy(buf, dst, n); } break; }
            if (s != errSSLWouldBlock || c.ioMode == 2) { r = status_result(c, s); break; }
            r = wait_socket(c, c->wanted, deadline); if (r) break;
        }
    }
    err(c, r); [c.lock unlock]; return r;
} }
uint32_t dm_ossl_write(void *o, const void *buf, int *out, unsigned size) { @autoreleasepool {
    if (!out || (!buf && size)) return S_BAD_POINTER; *out = 0;
    OsslConnection *c = get_conn(o); if (!c) return S_FAIL; [c.lock lock];
    uint32_t r = c.established ? 0 : S_FAIL; uint64_t deadline = clock_ms() + c.timeout;
    while (!r && size) {
        if (atomic_load(&c->closed)) { r = S_BAD_SOCKET; break; }
        bool flushing = c.pendingWrite != nil; unsigned want = flushing ? (unsigned)c.pendingWrite.length : MIN(size, 16384u);
        c->wanted = POLLOUT; size_t n = 0; OSStatus s = SSLWrite(c->tls, flushing ? NULL : buf, flushing ? 0 : want, &n);
        if (!s) { *out = flushing ? (int)want : (int)n; c.pendingWrite = nil; break; }
        if (s != errSSLWouldBlock) { r = status_result(c, s); break; }
        if (!flushing) c.pendingWrite = [NSData dataWithBytes:buf length:want];   // SSLWrite took the record into its cache: a retry only flushes it
        if (c.ioMode == 2) { r = S_WOULD_BLOCK; break; }
        r = wait_socket(c, c->wanted, deadline);
    }
    err(c, r); [c.lock unlock]; return r;
} }
uint32_t dm_ossl_pending(void *o, int *out) { @autoreleasepool {
    if (!out) return S_BAD_POINTER; *out = 0; OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    [c.lock lock]; size_t n = 0; OSStatus s = c->tls ? SSLGetBufferedReadSize(c->tls, &n) : noErr;
    if (!s) *out = (int)MIN((size_t)INT_MAX, n + c.lookahead.length); [c.lock unlock]; return status_result(c, s);
} }
uint32_t dm_ossl_last_error(void *o, uint32_t *out, bool verify) { @autoreleasepool {
    if (!out) return S_BAD_POINTER; OsslConnection *c = get_conn(o); if (!c) return S_FAIL;
    [c.lock lock]; *out = verify ? c.verifyError : c.lastError; if (verify) c.verifyError = 0; [c.lock unlock]; return 0;
} }
#pragma clang diagnostic pop
