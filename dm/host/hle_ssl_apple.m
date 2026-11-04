// Public SecureTransport supplies the stream engine. SecTrust validates public PKI;
// an explicit private service CA additionally supports strict, SAN-only OpenSSL
// verification, with Apple's basic chain checks retained. No permissive callback.
// SecureTransport supports TLS through 1.2. TLS-1.3-only requests fail rather than silently downgrade.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include "nextendo_x509.h"
#import <Security/SecureTransport.h>
#include "ssl_backend.h"
#include "net_socket.h"
#include "ssl_trace.h"
#include <stdatomic.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// These APIs remain public on the deployment targets. The existing guest's connected BSD
// descriptor cannot be adopted by Network.framework through a public API.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);

@interface DmTlsContext : NSObject
@property(nonatomic) uint64_t identifier;
@property(nonatomic) unsigned version;
@property(nonatomic) NSLock *lock;
@property(nonatomic) NSMutableDictionary<NSNumber *, NSArray *> *anchors;
@property(nonatomic) NSArray *clientIdentity;
@property(nonatomic) uint64_t clientIdentifier;
@end
@implementation DmTlsContext
@end

@interface DmTlsConnection : NSObject {
@public
    _Atomic bool closed;
    _Atomic int descriptor;
    SSLContextRef tls;
    short wanted;
}
@property(nonatomic) uint64_t identifier;
@property(nonatomic) DmTlsContext *context;
@property(nonatomic) NSLock *lock;
@property(nonatomic) int guestDescriptor;
@property(nonatomic) uint64_t socketGeneration;
@property(nonatomic) NSString *hostname;
@property(nonatomic) NSString *peerName;
@property(nonatomic) NSData *policyAnchor;
@property(nonatomic) BOOL policyPinned;
@property(nonatomic) unsigned ioMode, verifyOptions, sessionCache, timeout;
@property(nonatomic) BOOL certificateChain, enableAlpn, handshaking, established, verified, rejected;
@property(nonatomic) BOOL tracedRead, tracedWrite;
@property(atomic) BOOL doNotClose;
@property(atomic) uint32_t lastError, verifyError;
@property(nonatomic) NSMutableData *lookahead;
@property(nonatomic) NSData *pendingWrite;
@property(nonatomic) NSArray<NSString *> *alpn;
@property(nonatomic) void *certificateBuffer;
@property(nonatomic) unsigned certificateCapacity;
@end
@implementation DmTlsConnection
- (void)dealloc {
    // No operation can still own this object at its final release. Send close_notify on the
    // still-owned descriptor, without retaining a connection already inside dealloc.
    if (tls) { atomic_store(&closed, false); SSLClose(tls); CFRelease(tls); }
    int fd = atomic_load(&descriptor); if (fd >= 0) close(fd);
}
@end

static NSMutableDictionary<NSValue *, DmTlsContext *> *contexts;
static NSMutableDictionary<NSValue *, DmTlsConnection *> *connections;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint64_t nextIdentifier;
static _Atomic(dm_ssl_peer_policy_provider) peerProvider;
static _Atomic(dm_ssl_peer_policy_provider) pinProvider;
static _Atomic(bool (*)(const char *)) peerGuard;
static void initialize_registry(void) { contexts = [NSMutableDictionary new]; connections = [NSMutableDictionary new]; }
static NSValue *key(void *object) { return [NSValue valueWithPointer:object]; }
static BOOL enabled(void) { return dm_net_enabled(); }
static uint64_t clock_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static DmTlsContext *get_context(void *object) {
    pthread_once(&once, initialize_registry); pthread_mutex_lock(&registry);
    DmTlsContext *c = contexts[key(object)]; pthread_mutex_unlock(&registry); return c;
}
static DmTlsConnection *get_connection(void *object) {
    pthread_once(&once, initialize_registry); pthread_mutex_lock(&registry);
    DmTlsConnection *c = connections[key(object)]; pthread_mutex_unlock(&registry); return c;
}
static uint32_t error(DmTlsConnection *c, uint32_t value) { c.lastError = value; return value; }
static void trace(DmTlsConnection *c, dm_tls_stage stage, uint32_t result, OSStatus native, uint64_t detail) {
    unsigned flags = (c.handshaking ? 1u : 0u) | (c.established ? 2u : 0u) |
        (c.verified ? 4u : 0u) | (c.rejected ? 8u : 0u) |
        (c.policyPinned ? 16u : 0u) | (c.policyAnchor ? 32u : 0u);
    dm_tls_trace(stage, c.identifier, result, native, detail, flags);
}
static uint32_t status_result(DmTlsConnection *c, OSStatus status) {
    if (!status) return error(c, 0);
    if (status == errSSLWouldBlock) return error(c, DM_SSL_WOULD_BLOCK);
    return error(c, DM_SSL_FAIL);
}
static uint32_t wait_socket(DmTlsConnection *c, short events, uint64_t deadline) {
    for (;;) {
        if (atomic_load(&c->closed)) return error(c, DM_SSL_BAD_SOCKET);
        uint64_t now = clock_ms(); if (now >= deadline) return error(c, DM_SSL_TIMEOUT);
        uint64_t left = deadline - now; int ms = left > 100 ? 100 : (int)left;
        struct pollfd p = {.fd = atomic_load(&c->descriptor), .events = events};
        dm_guest_host_wait_begin(); int n = poll(&p, 1, ms), e = errno; dm_guest_host_wait_end();
        if (n < 0 && e == EINTR) continue;
        if (n < 0 || atomic_load(&c->closed)) return error(c, DM_SSL_BAD_SOCKET);
        if (n) return 0;
    }
}
static OSStatus transport_io(SSLConnectionRef reference, void *bytes, size_t *size, bool reading) {
    __unsafe_unretained DmTlsConnection *c = (__bridge DmTlsConnection *)(void *)reference;
    size_t requested = *size, done = 0; *size = 0;
    while (done < requested) {
        if (atomic_load(&c->closed)) return errSSLClosedAbort;
        int fd = atomic_load(&c->descriptor);
        ssize_t n = reading ? recv(fd, (char *)bytes + done, requested - done, 0) : send(fd, (char *)bytes + done, requested - done, 0);
        if (n > 0) { done += (size_t)n; *size = done; continue; }
        if (!n && reading) return errSSLClosedAbort; // Only a TLS close_notify is orderly EOF; bare TCP EOF may truncate an authenticated record.
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { c->wanted = reading ? POLLIN : POLLOUT; return errSSLWouldBlock; }
        return errSSLClosedAbort;
    }
    return noErr;
}
static OSStatus transport_read(SSLConnectionRef c, void *p, size_t *n) { return transport_io(c, p, n, true); }
static OSStatus transport_write(SSLConnectionRef c, const void *p, size_t *n) { return transport_io(c, (void *)p, n, false); }

void dm_ssl_set_peer_policy(dm_ssl_peer_policy_provider provider) { atomic_store(&peerProvider, provider); }
void dm_ssl_set_peer_pin_policy(dm_ssl_peer_policy_provider provider) { atomic_store(&pinProvider, provider); }
void dm_ssl_set_peer_guard(bool (*guard)(const char *)) { atomic_store(&peerGuard, guard); }
uint32_t dm_ssl_initialize(unsigned concurrency) { return concurrency >= 1 && concurrency <= 4 ? 0 : DM_SSL_FAIL; }
uint32_t dm_ssl_finalize(void) { @autoreleasepool {
    pthread_once(&once, initialize_registry); pthread_mutex_lock(&registry);
    NSArray *objects = connections.allValues; [connections removeAllObjects]; [contexts removeAllObjects]; pthread_mutex_unlock(&registry);
    for (DmTlsConnection *c in objects) { atomic_store(&c->closed, true); if (atomic_load(&c->descriptor) >= 0 && !c.doNotClose && c.guestDescriptor >= 0) dm_net_close_generation(c.guestDescriptor, c.socketGeneration); }
    return 0;
} }
uint32_t dm_ssl_context_create(void *object, unsigned version) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; if (!enabled()) return DM_SSL_FAIL;
    unsigned protocols = version & 0xff; if (!(protocols & (1 | 8 | 16 | 32)) || (protocols & ~(1u | 8u | 16u | 32u | 64u))) return DM_SSL_FAIL;
    DmTlsContext *c = [DmTlsContext new]; if (!c) return DM_SSL_FAIL; c.version = version; c.lock = [NSLock new]; c.anchors = [NSMutableDictionary new];
    pthread_once(&once, initialize_registry); pthread_mutex_lock(&registry);
    if (contexts[key(object)] || contexts.count >= 64) { pthread_mutex_unlock(&registry); return DM_SSL_FAIL; }
    c.identifier = ++nextIdentifier; contexts[key(object)] = c; pthread_mutex_unlock(&registry); return 0;
} }
uint32_t dm_ssl_context_destroy(void *object) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER;
    pthread_once(&once, initialize_registry); pthread_mutex_lock(&registry); DmTlsContext *c = contexts[key(object)];
    if (!c) { pthread_mutex_unlock(&registry); return DM_SSL_FAIL; }
    for (DmTlsConnection *connection in connections.allValues) if (connection.context == c) { pthread_mutex_unlock(&registry); return DM_SSL_FAIL; }
    [contexts removeObjectForKey:key(object)]; pthread_mutex_unlock(&registry); return 0;
} }
uint32_t dm_ssl_context_id(void *object, uint64_t *out) { @autoreleasepool {
    if (!out || !object) return DM_SSL_BAD_POINTER;
    // The SDK getter also succeeds before Create and after Destroy, with ID 0.
    // This query must not allocate a context or imply a usable TLS session.
    DmTlsContext *c = get_context(object); *out = c ? c.identifier : 0; return 0;
} }
static NSArray *parse_certificates(const void *bytes, unsigned size, int format) {
    if (!bytes || !size || size > 4 * 1024 * 1024 || (format != 1 && format != 2)) return nil;
    NSMutableArray *certificates = [NSMutableArray new];
    if (format == 2) {
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, size); SecCertificateRef certificate = SecCertificateCreateWithData(kCFAllocatorDefault, data); CFRelease(data);
        if (!certificate) return nil; [certificates addObject:CFBridgingRelease(certificate)];
    } else {
        NSString *text = [[NSString alloc] initWithBytes:bytes length:size encoding:NSASCIIStringEncoding]; if (!text) return nil;
        NSRange remainder = NSMakeRange(0, text.length);
        while (remainder.length) {
            NSRange start = [text rangeOfString:@"-----BEGIN CERTIFICATE-----" options:0 range:remainder]; if (start.location == NSNotFound) break;
            NSUInteger p = NSMaxRange(start); NSRange end = [text rangeOfString:@"-----END CERTIFICATE-----" options:0 range:NSMakeRange(p, text.length - p)]; if (end.location == NSNotFound) return nil;
            NSData *data = [[NSData alloc] initWithBase64EncodedString:[text substringWithRange:NSMakeRange(p, end.location - p)] options:NSDataBase64DecodingIgnoreUnknownCharacters];
            SecCertificateRef certificate = data ? SecCertificateCreateWithData(kCFAllocatorDefault, (__bridge CFDataRef)data) : NULL;
            if (!certificate) return nil; [certificates addObject:CFBridgingRelease(certificate)];
            remainder = NSMakeRange(NSMaxRange(end), text.length - NSMaxRange(end));
        }
    }
    return certificates.count ? certificates : nil;
}
// CertStore BDF entries are file records, not the SDK's pointer-bearing 24-byte
// BuiltInCertificateInfo. Offsets are relative to byte 8 of the original file.
static NSArray<NSDictionary *> *builtin_certificates(void) {
    const char *root = getenv("DM_SYSTEM_DATA_DIR"); if (!root || !root[0]) return nil;
    NSString *directory = [NSString stringWithUTF8String:root]; if (!directory) return nil;
    NSString *path = [directory stringByAppendingPathComponent:@"0100000000000800/files/ssl_TrustedCerts.bdf"];
    NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    if (!attributes || [attributes fileSize] > 4 * 1024 * 1024) return nil;
    NSData *file = [NSData dataWithContentsOfFile:path]; if (file.length < 8 || file.length > 4 * 1024 * 1024 || memcmp(file.bytes, "sslT", 4)) return nil;
    uint32_t count; memcpy(&count, (const char *)file.bytes + 4, 4);
    if (!count || count > 4096 || 8u + 16u * count > file.length) return nil;
    NSMutableArray *entries = [NSMutableArray new]; NSMutableSet *seen = [NSMutableSet new];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t row[4]; memcpy(row, (const char *)file.bytes + 8 + 16 * i, sizeof row);
        uint64_t offset = 8ull + row[3];
        if (!row[0] || row[0] == UINT32_MAX || [seen containsObject:@(row[0])] || row[1] > 3 || !row[2] || offset < 8u + 16u * count || offset > file.length || row[2] > file.length - offset) return nil;
        NSData *der = [file subdataWithRange:NSMakeRange((NSUInteger)offset, row[2])];
        if (!parse_certificates(der.bytes, (unsigned)der.length, 2)) return nil;
        [seen addObject:@(row[0])]; [entries addObject:@{@"id":@(row[0]), @"status":@(row[1]), @"data":der}];
    }
    return entries;
}
static NSArray<NSDictionary *> *select_builtin(const uint32_t *ids, unsigned count, bool *all, unsigned *size) {
    *size = 0; *all = false; if (!ids || !count || count > 4096 || !enabled()) return nil;
    NSArray *store = builtin_certificates(); if (!store) return nil;
    for (unsigned i = 0; i < count; i++) if (ids[i] == UINT32_MAX) *all = true;
    NSMutableArray *selected = [NSMutableArray new];
    if (*all) [selected addObjectsFromArray:store];
    else for (unsigned i = 0; i < count; i++) {
        NSDictionary *found = nil; for (NSDictionary *entry in store) if ([entry[@"id"] unsignedIntValue] == ids[i]) { found = entry; break; }
        if (!found) return nil; [selected addObject:found];
    }
    uint64_t needed = 24 * (selected.count + (*all ? 1 : 0));
    for (NSDictionary *entry in selected) needed += ([entry[@"data"] length] + 3u) & ~(uint64_t)3;
    if (needed > UINT32_MAX) return nil; *size = (unsigned)needed; return selected;
}
uint32_t dm_ssl_builtin_size(unsigned *out, const uint32_t *ids, unsigned count) { @autoreleasepool {
    if (!out || !ids) return DM_SSL_BAD_POINTER; *out = 0; bool all;
    return select_builtin(ids, count, &all, out) ? 0 : DM_SSL_FAIL;
} }
uint32_t dm_ssl_builtin_certificates(void **out, uint8_t *buffer, unsigned capacity, const uint32_t *ids, unsigned count) { @autoreleasepool {
    if (!out || !buffer || !ids) return DM_SSL_BAD_POINTER; *out = NULL; bool all; unsigned size;
    NSArray *entries = select_builtin(ids, count, &all, &size); if (!entries || capacity < size) return DM_SSL_FAIL;
    memset(buffer, 0, size); size_t position = 24 * (entries.count + (all ? 1 : 0));
    for (NSUInteger i = 0; i < entries.count; i++) {
        NSDictionary *entry = entries[i]; NSData *der = entry[@"data"];
        struct { uint32_t id, status; uint64_t size; const uint8_t *data; } info = {[entry[@"id"] unsignedIntValue], [entry[@"status"] unsignedIntValue], der.length, buffer + position};
        memcpy(buffer + 24 * i, &info, sizeof info); memcpy(buffer + position, der.bytes, der.length); position += (der.length + 3u) & ~(size_t)3;
    }
    if (all) { uint32_t sentinel[2] = {UINT32_MAX, UINT32_MAX}; memcpy(buffer + 24 * entries.count, sentinel, sizeof sentinel); }
    *out = buffer; return 0;
} }
uint32_t dm_ssl_import_server(void *object, uint64_t *id, const void *bytes, unsigned size, int format) { @autoreleasepool {
    if (!object || !id || !bytes) return DM_SSL_BAD_POINTER; *id = 0; DmTlsContext *c = get_context(object); if (!c) return DM_SSL_FAIL;
    NSArray *certificates = parse_certificates(bytes, size, format); if (!certificates) return DM_SSL_FAIL;
    pthread_mutex_lock(&registry); uint64_t identifier = ++nextIdentifier; pthread_mutex_unlock(&registry);
    [c.lock lock]; if (c.anchors.count >= 71) { [c.lock unlock]; return DM_SSL_FAIL; } c.anchors[@(identifier)] = certificates; [c.lock unlock]; if (id) *id = identifier; return 0;
} }
uint32_t dm_ssl_import_client(void *object, uint64_t *out_id, const void *bytes, const char *password, unsigned size, unsigned password_size) { @autoreleasepool {
    if (!object || !out_id || !bytes || (!password && password_size)) return DM_SSL_BAD_POINTER; *out_id = 0;
    DmTlsContext *c = get_context(object); if (!c || !size || size > 4 * 1024 * 1024 || password_size > 1024) return DM_SSL_FAIL;
    if (@available(macOS 15.0, iOS 18.0, *)) {
        NSData *data = [NSData dataWithBytes:bytes length:size]; NSString *pass = [[NSString alloc] initWithBytes:password ?: "" length:password_size encoding:NSUTF8StringEncoding]; if (!pass) return DM_SSL_FAIL;
        CFArrayRef items = NULL; OSStatus status = SecPKCS12Import((__bridge CFDataRef)data, (__bridge CFDictionaryRef)@{(__bridge id)kSecImportExportPassphrase:pass, (__bridge id)kSecImportToMemoryOnly:@YES}, &items);
        if (status || !items || !CFArrayGetCount(items)) { if (items) CFRelease(items); return DM_SSL_FAIL; }
        NSDictionary *first = (__bridge NSDictionary *)CFArrayGetValueAtIndex(items, 0); id identity = first[(__bridge id)kSecImportItemIdentity]; NSArray *chain = first[(__bridge id)kSecImportItemCertChain];
        if (!identity) { CFRelease(items); return DM_SSL_FAIL; }
        NSMutableArray *identityChain = [NSMutableArray arrayWithObject:identity]; if (chain.count > 1) [identityChain addObjectsFromArray:[chain subarrayWithRange:NSMakeRange(1, chain.count - 1)]];
        pthread_mutex_lock(&registry); uint64_t identifier = ++nextIdentifier; pthread_mutex_unlock(&registry);
        [c.lock lock]; if (c.clientIdentity) { [c.lock unlock]; CFRelease(items); return DM_SSL_FAIL; } c.clientIdentity = identityChain; c.clientIdentifier = identifier; [c.lock unlock]; CFRelease(items); if (out_id) *out_id = identifier; return 0;
    }
    return DM_SSL_FAIL; // Never import a guest private key into the user's persistent Keychain on older macOS.
} }
uint32_t dm_ssl_remove_pki(void *object, uint64_t identifier) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; DmTlsContext *c = get_context(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; BOOL found = c.anchors[@(identifier)] != nil || c.clientIdentifier == identifier;
    [c.anchors removeObjectForKey:@(identifier)]; if (c.clientIdentifier == identifier) { c.clientIdentity = nil; c.clientIdentifier = 0; } [c.lock unlock]; return found ? 0 : DM_SSL_FAIL;
} }
uint32_t dm_ssl_connection_create(void *object, void *context) { @autoreleasepool {
    if (!object || !context) return DM_SSL_BAD_POINTER; if (!enabled()) return DM_SSL_FAIL;
    DmTlsContext *parent = get_context(context); if (!parent) return DM_SSL_FAIL;
    DmTlsConnection *c = [DmTlsConnection new]; if (!c) return DM_SSL_FAIL; c.context = parent; c.lock = [NSLock new]; c.lookahead = [NSMutableData new]; c.guestDescriptor = -1;
    c.ioMode = 1; c.verifyOptions = 3; c.timeout = 300000; atomic_init(&c->closed, false); atomic_init(&c->descriptor, -1);
    pthread_mutex_lock(&registry);
    if (contexts[key(context)] != parent || connections[key(object)] || connections.count >= 512) { pthread_mutex_unlock(&registry); return DM_SSL_FAIL; }
    unsigned count = 0; for (DmTlsConnection *other in connections.allValues) if (other.context == parent) count++;
    if (count >= 8) { pthread_mutex_unlock(&registry); return DM_SSL_FAIL; }
    c.identifier = ++nextIdentifier; connections[key(object)] = c; pthread_mutex_unlock(&registry); return 0;
} }
uint32_t dm_ssl_connection_destroy(void *object) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; pthread_once(&once, initialize_registry); pthread_mutex_lock(&registry);
    DmTlsConnection *c = connections[key(object)]; if (c) { atomic_store(&c->closed, true); [connections removeObjectForKey:key(object)]; } pthread_mutex_unlock(&registry);
    if (!c) return DM_SSL_FAIL;
    if (atomic_load(&c->descriptor) >= 0 && !c.doNotClose && c.guestDescriptor >= 0) dm_net_close_generation(c.guestDescriptor, c.socketGeneration);
    return 0; // A running operation retains c and its owned dup until it observes closed at its next readiness boundary.
} }
uint32_t dm_ssl_connection_id(void *object, uint64_t *out, bool context) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER;
    DmTlsConnection *c = get_connection(object);
    *out = c ? (context ? c.context.identifier : c.identifier) : 0; return 0;
} }
uint32_t dm_ssl_set_socket(void *object, int guest) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; if (atomic_load(&c->closed) || atomic_load(&c->descriptor) >= 0) { [c.lock unlock]; return error(c, DM_SSL_FAIL); }
    uint64_t generation; int fd = dm_net_dup_native_generation(guest, &generation);
    struct sockaddr_storage address; socklen_t size = sizeof address; int type = 0; socklen_t type_size = sizeof type;
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) || type != SOCK_STREAM || getpeername(fd, (struct sockaddr *)&address, &size)) { if (fd >= 0) close(fd); trace(c, DM_TLS_SOCKET, DM_SSL_BAD_SOCKET, 0, 0); [c.lock unlock]; return error(c, DM_SSL_BAD_SOCKET); }
    c.guestDescriptor = guest; c.socketGeneration = generation; atomic_store(&c->descriptor, fd);
    if (atomic_load(&c->closed)) { if (!c.doNotClose) dm_net_close_generation(guest, generation); [c.lock unlock]; return error(c, DM_SSL_BAD_SOCKET); }
    trace(c, DM_TLS_SOCKET, 0, 0, 0); [c.lock unlock]; return 0;
} }
uint32_t dm_ssl_get_socket(void *object, int *out) { @autoreleasepool { if (!object || !out) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; if (atomic_load(&c->descriptor) < 0) return error(c, DM_SSL_NO_SOCKET); *out = c.guestDescriptor; return 0; } }
uint32_t dm_ssl_set_hostname(void *object, const char *name, unsigned size) { @autoreleasepool {
    if (!object || !name) return DM_SSL_BAD_POINTER; if (!size || size > 255) return DM_SSL_FAIL;
    if (!name[size - 1]) size--; if (!size || memchr(name, 0, size)) return DM_SSL_FAIL;
    NSString *host = [[NSString alloc] initWithBytes:name length:size encoding:NSASCIIStringEncoding]; if (!host) return DM_SSL_FAIL;
    struct in6_addr literal;
    unsigned name_kind = (inet_pton(AF_INET, host.UTF8String, &literal) == 1 ||
                          inet_pton(AF_INET6, host.UTF8String, &literal) == 1) ? 2u : 0u;
    bool (*guard)(const char *) = atomic_load(&peerGuard); if (guard && !guard(host.UTF8String)) { dm_tls_trace(DM_TLS_HOSTNAME, 0, DM_SSL_FAIL, 0, name_kind, 0); return DM_SSL_FAIL; }
    DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; if (c.handshaking) { [c.lock unlock]; return error(c, DM_SSL_FAIL); }
    NSString *peer = host; NSData *anchor = nil; BOOL pinned = NO;
    dm_ssl_peer_policy_provider provider = atomic_load(&peerProvider), pin = atomic_load(&pinProvider);
    if (provider || pin) {
        char mapped[256] = {0}; const uint8_t *der = NULL; size_t length = 0;
        pinned = pin && pin(host.UTF8String, mapped, sizeof mapped, &der, &length);
        if (pinned || (provider && provider(host.UTF8String, mapped, sizeof mapped, &der, &length))) {
            if (!memchr(mapped, 0, sizeof mapped) || !mapped[0] || (!der && length) || length > 4 * 1024 * 1024) { [c.lock unlock]; return error(c, DM_SSL_FAIL); }
            if (pinned && (!length || !dm_nextendo_x509_available())) { [c.lock unlock]; return error(c, DM_SSL_FAIL); }
            peer = [NSString stringWithUTF8String:mapped]; if (!peer) { [c.lock unlock]; return error(c, DM_SSL_FAIL); }
            if (length) { anchor = [NSData dataWithBytes:der length:length]; if (!parse_certificates(anchor.bytes, (unsigned)anchor.length, 2)) { [c.lock unlock]; return error(c, DM_SSL_FAIL); } }
        }
    }
    c.hostname = host; c.peerName = peer; c.policyAnchor = anchor; c.policyPinned = pinned;
    trace(c, DM_TLS_HOSTNAME, 0, 0, 1u | name_kind); [c.lock unlock]; return 0;
} }
uint32_t dm_ssl_get_hostname(void *object, char *out, unsigned *size, unsigned capacity) { @autoreleasepool {
    if (!object || !out || !size) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; const char *name = c.hostname.UTF8String ?: ""; size_t length = strlen(name); *size = (unsigned)length;
    if (capacity <= length) { [c.lock unlock]; return DM_SSL_FAIL; } memcpy(out, name, length + 1); [c.lock unlock]; return 0;
} }
uint32_t dm_ssl_set_value(void *object, int property, unsigned value) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; uint32_t result = 0;
    if (property == DM_SSL_IO_MODE && (value == 1 || value == 2)) c.ioMode = value;
    else if (property == DM_SSL_VERIFY_OPTIONS && !c.handshaking && (value & 3) == 3 && !(value & ~7u)) c.verifyOptions = value;
    else if (property == DM_SSL_SESSION_CACHE && !c.handshaking && value <= 2) c.sessionCache = value;
    else if (property == DM_SSL_IO_TIMEOUT && value) c.timeout = value;
    else result = error(c, DM_SSL_FAIL);
    trace(c, DM_TLS_VALUE, result, 0, ((uint64_t)(uint32_t)property << 32) | value);
    [c.lock unlock]; return result;
} }
uint32_t dm_ssl_get_value(void *object, int property, unsigned *out) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; uint32_t result = 0;
    switch (property) { case DM_SSL_IO_MODE: *out = c.ioMode; break; case DM_SSL_VERIFY_OPTIONS: *out = c.verifyOptions; break; case DM_SSL_SESSION_CACHE: *out = c.sessionCache; break; case DM_SSL_IO_TIMEOUT: *out = c.timeout; break; default: result = DM_SSL_FAIL; }
    [c.lock unlock]; return result;
} }
uint32_t dm_ssl_set_option(void *object, int option, unsigned value) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; value &= 1;
    [c.lock lock]; uint32_t result = 0;
    if (option == 0 && atomic_load(&c->descriptor) < 0) c.doNotClose = value;
    else if (option == 1) c.certificateChain = value;
    else if (option == 2 && !value) { /* Verification stays enabled. */ }
    else if (option == 3 && !c.handshaking) c.enableAlpn = value;
    else result = DM_SSL_FAIL;
    trace(c, DM_TLS_OPTION, result, 0, ((uint64_t)(uint32_t)option << 32) | value);
    [c.lock unlock]; return result;
} }
uint32_t dm_ssl_get_option(void *object, void *out, int option) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; uint8_t value;
    switch (option) { case 0: value = c.doNotClose; break; case 1: value = c.certificateChain; break; case 2: value = 0; break; case 3: value = c.enableAlpn; break; default: [c.lock unlock]; return DM_SSL_FAIL; }
    memcpy(out, &value, 1); [c.lock unlock]; return 0;
} }

static uint32_t prepare(DmTlsConnection *c) {
    if (atomic_load(&c->closed)) return error(c, DM_SSL_BAD_SOCKET);
    if (c.rejected) return error(c, DM_SSL_FAIL);
    if (atomic_load(&c->descriptor) < 0) return error(c, DM_SSL_NO_SOCKET);
    if (!c.peerName.length) return error(c, DM_SSL_FAIL);
    if (c->tls) return 0;
    c->tls = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType); if (!c->tls) return error(c, DM_SSL_FAIL);
    SSLProtocol min = kTLSProtocol1, max = kTLSProtocol12; unsigned v = c.context.version & 0xff;
    if (!(v & 1)) { min = v & 8 ? kTLSProtocol1 : v & 16 ? kTLSProtocol11 : kTLSProtocol12; max = v & (32 | 64) ? kTLSProtocol12 : v & 16 ? kTLSProtocol11 : kTLSProtocol1; }
    OSStatus s = SSLSetProtocolVersionMin(c->tls, min); if (!s) s = SSLSetProtocolVersionMax(c->tls, max);
    if (!s) s = SSLSetIOFuncs(c->tls, transport_read, transport_write);
    if (!s) s = SSLSetConnection(c->tls, (__bridge SSLConnectionRef)c);
    if (!s) s = SSLSetPeerDomainName(c->tls, c.peerName.UTF8String, strlen(c.peerName.UTF8String));
    if (!s) s = SSLSetSessionOption(c->tls, kSSLSessionOptionBreakOnServerAuth, true);
    if (!s) s = SSLSetSessionOption(c->tls, kSSLSessionOptionEnableSessionTickets, c.sessionCache == 2);
    NSString *peerID = [NSString stringWithFormat:@"nxnative:%llu:%@:%llu", c.context.identifier, c.peerName, c.sessionCache ? 0ull : c.identifier];
    if (!s) s = SSLSetPeerID(c->tls, peerID.UTF8String, strlen(peerID.UTF8String));
    if (!s && c.enableAlpn && c.alpn.count) s = SSLSetALPNProtocols(c->tls, (__bridge CFArrayRef)c.alpn);
    [c.context.lock lock]; NSArray *identity = c.context.clientIdentity; [c.context.lock unlock];
    if (!s && identity.count) s = SSLSetCertificate(c->tls, (__bridge CFArrayRef)identity);
    c.handshaking = YES; uint32_t result = status_result(c, s);
    trace(c, DM_TLS_PREPARE, result, s, v); return result;
}
static BOOL verify_peer(DmTlsConnection *c) {
    SecTrustRef trust = NULL; if (SSLCopyPeerTrust(c->tls, &trust) || !trust) return NO;
    if (c.policyPinned) {
        SecCertificateRef leaf = SecTrustGetCertificateAtIndex(trust, 0);
        NSData *der = leaf ? CFBridgingRelease(SecCertificateCopyData(leaf)) : nil;
        BOOL valid = der.length && dm_nextendo_x509_verify_pin(c.peerName.UTF8String,
            c.policyAnchor.bytes, c.policyAnchor.length, der.bytes, der.length);
        CFRelease(trust); return valid;
    }
    if (c.policyAnchor && dm_nextendo_x509_available()) {
        // The route's explicit private CA is the sole authority for this path.
        // BasicX509 retains Apple's chain/time/trust checks; OpenSSL adds strict
        // server purpose, SAN-only hostname, constraints and strength checks.
        // This does not adopt a root sent by the server or alter system trust.
        NSArray *scoped = parse_certificates(c.policyAnchor.bytes, (unsigned)c.policyAnchor.length, 2);
        SecPolicyRef basic = SecPolicyCreateBasicX509();
        OSStatus result = basic ? SecTrustSetPolicies(trust, basic) : errSecParam;
        if (basic) CFRelease(basic);
        if (!result) result = SecTrustSetNetworkFetchAllowed(trust, false);
        if (!result && scoped.count == 1) result = SecTrustSetAnchorCertificates(trust, (__bridge CFArrayRef)scoped);
        else if (!result) result = errSecParam;
        if (!result) result = SecTrustSetAnchorCertificatesOnly(trust, true);
        CFErrorRef failure = NULL;
        dm_guest_host_wait_begin();
        BOOL valid = !result && SecTrustEvaluateWithError(trust, &failure);
        if (valid) {
            CFIndex count = SecTrustGetCertificateCount(trust);
            NSMutableArray<NSData *> *data = [NSMutableArray new];
            const uint8_t *chain[16]; size_t sizes[16];
            if (count < 1 || count > 16) valid = NO;
            for (CFIndex i = 0; valid && i < count; i++) {
                SecCertificateRef certificate = SecTrustGetCertificateAtIndex(trust, i);
                NSData *der = certificate ? CFBridgingRelease(SecCertificateCopyData(certificate)) : nil;
                if (!der.length || der.length > 65536) { valid = NO; break; }
                [data addObject:der]; chain[i] = der.bytes; sizes[i] = der.length;
            }
            if (valid) valid = dm_nextendo_x509_verify(c.peerName.UTF8String, c.policyAnchor.bytes,
                c.policyAnchor.length, chain, sizes, (size_t)count);
        }
        dm_guest_host_wait_end();
        if (failure) CFRelease(failure); CFRelease(trust); return valid;
    }
    SecPolicyRef policy = SecPolicyCreateSSL(true, (__bridge CFStringRef)c.peerName);
    OSStatus status = policy ? SecTrustSetPolicies(trust, policy) : errSecParam; if (policy) CFRelease(policy);
    if (!status) status = SecTrustSetNetworkFetchAllowed(trust, false); // Nonblocking TLS must not start hidden AIA/OCSP network work.
    NSMutableArray *anchors = [NSMutableArray new]; [c.context.lock lock]; for (NSArray *list in c.context.anchors.allValues) [anchors addObjectsFromArray:list]; [c.context.lock unlock];
    // Horizon's default CertStore is context-local too. Preserve the dumped trust status;
    // removed, untrusted and revoked records remain retrievable, never default anchors.
    for (NSDictionary *entry in builtin_certificates()) if ([entry[@"status"] unsignedIntValue] == 1) {
        NSData *der = entry[@"data"]; [anchors addObjectsFromArray:parse_certificates(der.bytes, (unsigned)der.length, 2)];
    }
    if (c.policyAnchor) { NSArray *scoped = parse_certificates(c.policyAnchor.bytes, (unsigned)c.policyAnchor.length, 2); if (!scoped) { CFRelease(trust); return NO; } [anchors addObjectsFromArray:scoped]; }
    if (!status && anchors.count) { status = SecTrustSetAnchorCertificates(trust, (__bridge CFArrayRef)anchors); if (!status) status = SecTrustSetAnchorCertificatesOnly(trust, false); }
    CFErrorRef failure = NULL; dm_guest_host_wait_begin(); BOOL valid = !status && SecTrustEvaluateWithError(trust, &failure); dm_guest_host_wait_end();
    if (failure) CFRelease(failure); CFRelease(trust); return valid;
}
static uint32_t handshake_locked(DmTlsConnection *c) {
    if (c.established) return 0;
    uint32_t result = prepare(c); if (result) return result;
    uint64_t deadline = clock_ms() + c.timeout;
    for (;;) {
        if (atomic_load(&c->closed)) return error(c, DM_SSL_BAD_SOCKET);
        c->wanted = POLLIN; OSStatus status = SSLHandshake(c->tls);
        if (status == errSSLServerAuthCompleted) {
            BOOL valid = verify_peer(c); trace(c, DM_TLS_VERIFY, valid ? 0 : DM_SSL_FAIL, status, 0);
            if (!valid) { c.rejected = YES; c.verifyError = DM_SSL_FAIL; SSLSetError(c->tls, errSSLXCertChainInvalid); return error(c, DM_SSL_FAIL); }
            c.verified = YES;
            continue;
        }
        if (!status) {
            if (!c.verified) {
                BOOL valid = verify_peer(c); trace(c, DM_TLS_VERIFY, valid ? 0 : DM_SSL_FAIL, status, 0);
                if (!valid) { c.rejected = YES; c.verifyError = DM_SSL_FAIL; SSLSetError(c->tls, errSSLXCertChainInvalid); return error(c, DM_SSL_FAIL); }
            }
            c.verified = YES; c.established = YES; trace(c, DM_TLS_HANDSHAKE, 0, 0, 0); return error(c, 0);
        }
        if (status != errSSLWouldBlock || c.ioMode == 2) { uint32_t r = status_result(c, status); trace(c, DM_TLS_HANDSHAKE, r, status, 0); return r; }
        result = wait_socket(c, c->wanted, deadline); if (result) return result;
    }
}
uint32_t dm_ssl_handshake(void *object) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; uint32_t result = handshake_locked(c); [c.lock unlock]; return result;
} }
static NSArray<NSData *> *peer_certificates(DmTlsConnection *c) {
    SecTrustRef trust = NULL; if (!c->tls || SSLCopyPeerTrust(c->tls, &trust) || !trust) return nil;
    NSMutableArray *out = [NSMutableArray new]; CFIndex count = SecTrustGetCertificateCount(trust);
    if (!c.certificateChain && count > 1) count = 1;
    for (CFIndex i = 0; i < count; i++) { SecCertificateRef certificate = SecTrustGetCertificateAtIndex(trust, i); CFDataRef data = certificate ? SecCertificateCopyData(certificate) : NULL; if (data) [out addObject:CFBridgingRelease(data)]; }
    CFRelease(trust); return out;
}
static uint32_t copy_certificates(DmTlsConnection *c, unsigned *size, unsigned *count, void *buffer, unsigned capacity) {
    NSArray<NSData *> *certificates = peer_certificates(c); if (!certificates.count) return DM_SSL_FAIL;
    size_t needed = c.certificateChain ? 16 + 8 * certificates.count : 0; for (NSData *data in certificates) needed += data.length;
    if (needed > UINT32_MAX) return DM_SSL_FAIL; *size = (unsigned)needed; *count = (unsigned)certificates.count;
    if (!buffer || capacity < needed) return DM_SSL_FAIL;
    if (!c.certificateChain) memcpy(buffer, certificates[0].bytes, needed);
    else {
        uint64_t magic = 0x4e4d684374726543ull; uint32_t total = *count; memset(buffer, 0, 16); memcpy(buffer, &magic, 8); memcpy((char *)buffer + 8, &total, 4);
        size_t offset = 16 + 8 * certificates.count;
        for (NSUInteger i = 0; i < certificates.count; i++) { NSData *data = certificates[i]; uint32_t entry[2] = {(uint32_t)data.length, (uint32_t)offset}; memcpy((char *)buffer + 16 + i * 8, entry, 8); memcpy((char *)buffer + offset, data.bytes, data.length); offset += data.length; }
    }
    return 0;
}
uint32_t dm_ssl_handshake_certs(void *object, unsigned *size, unsigned *count, void *buffer, unsigned capacity, bool registered_buffer) { @autoreleasepool {
    if (!object) return DM_SSL_BAD_POINTER;
    unsigned ignored_size = 0, ignored_count = 0;
    if (!size) size = &ignored_size;
    if (!count) count = &ignored_count;
    *size = *count = 0;
    DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; uint32_t result = handshake_locked(c);
    void *output = registered_buffer ? c.certificateBuffer : buffer;
    unsigned available = registered_buffer ? c.certificateCapacity : capacity;
    // The SDK overload delegates to ordinary DoHandshake when either the
    // certificate buffer or its capacity is zero. Export is optional; peer
    // verification in handshake_locked remains mandatory in both branches.
    if (!result && output && available) result = copy_certificates(c, size, count, output, available);
    trace(c, DM_TLS_CERTIFICATES, result, 0, output && available ? 1 : 0);
    [c.lock unlock]; return error(c, result);
} }
uint32_t dm_ssl_set_cert_buffer(void *object, void *buffer, unsigned capacity) { @autoreleasepool {
    if (!object || (!buffer && capacity)) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; c.certificateBuffer = buffer; c.certificateCapacity = capacity; [c.lock unlock]; return 0;
} }
uint32_t dm_ssl_needed_cert_size(void *object, unsigned *out) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER; *out = 0; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    unsigned count = 0; [c.lock lock]; copy_certificates(c, out, &count, NULL, 0); [c.lock unlock]; return *out ? 0 : DM_SSL_FAIL;
} }
uint32_t dm_ssl_cipher_info(void *object, void *out) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; SSLCipherSuite cipher = 0; SSLProtocol version = kSSLProtocolUnknown;
    OSStatus status = !c.established ? errSSLBadConfiguration : SSLGetNegotiatedCipher(c->tls, &cipher); if (!status) status = SSLGetNegotiatedProtocolVersion(c->tls, &version);
    if (!status) { struct { char cipher[64], protocol[8]; } info = {0}; snprintf(info.cipher, sizeof info.cipher, "0x%04x", cipher); snprintf(info.protocol, sizeof info.protocol, "%s", version == kTLSProtocol12 ? "TLSv1.2" : version == kTLSProtocol11 ? "TLSv1.1" : "TLSv1.0"); memcpy(out, &info, sizeof info); }
    [c.lock unlock]; return status_result(c, status);
} }
uint32_t dm_ssl_read(void *object, void *buffer, int *out, unsigned capacity, bool peek) { @autoreleasepool {
    if (!object || !out || (!buffer && capacity)) return DM_SSL_BAD_POINTER; *out = 0; if (capacity > INT_MAX) return DM_SSL_FAIL;
    DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; [c.lock lock];
    uint32_t result = !c.established ? DM_SSL_FAIL : 0;
    if (!result && c.lookahead.length) {
        NSUInteger n = MIN(c.lookahead.length, capacity); if (n) memcpy(buffer, c.lookahead.bytes, n); *out = (int)n;
        if (!peek && n) [c.lookahead replaceBytesInRange:NSMakeRange(0, n) withBytes:NULL length:0];
    } else if (!result && capacity) {
        unsigned request = peek ? MIN(capacity, 16384u) : capacity; // A peek may return one record; never allocate the caller's arbitrarily large capacity.
        NSMutableData *temporary = peek ? [NSMutableData dataWithLength:request] : nil; void *destination = peek ? temporary.mutableBytes : buffer;
        if (peek && !temporary) { [c.lock unlock]; return error(c, DM_SSL_FAIL); }
        uint64_t deadline = clock_ms() + c.timeout;
        for (;;) {
            if (atomic_load(&c->closed)) { result = DM_SSL_BAD_SOCKET; break; }
            c->wanted = POLLIN; size_t n = 0; OSStatus status = SSLRead(c->tls, destination, request, &n);
            if (n || !status || status == errSSLClosedGraceful) { *out = (int)n; if (peek && n) { [c.lookahead appendBytes:destination length:n]; memcpy(buffer, destination, n); } break; }
            if (status != errSSLWouldBlock || c.ioMode == 2) { result = status_result(c, status); break; }
            result = wait_socket(c, c->wanted, deadline); if (result) break;
        }
    }
    if ((!result && *out > 0 && !c.tracedRead) || (result && result != DM_SSL_WOULD_BLOCK)) {
        trace(c, DM_TLS_READ, result, 0, (unsigned)*out); if (!result) c.tracedRead = YES;
    }
    error(c, result); [c.lock unlock]; return result;
} }
uint32_t dm_ssl_write(void *object, const void *buffer, int *out, unsigned size) { @autoreleasepool {
    if (!object || !out || (!buffer && size)) return DM_SSL_BAD_POINTER; *out = 0; if (size > INT_MAX) return DM_SSL_FAIL;
    DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; [c.lock lock];
    uint32_t result = !c.established ? DM_SSL_FAIL : 0; uint64_t deadline = clock_ms() + c.timeout;
    NSData *pending = c.pendingWrite; NSUInteger pendingSize = pending.length;
    if (!result && pendingSize && (size < pendingSize || !buffer || memcmp(buffer, pending.bytes, pendingSize))) result = DM_SSL_FAIL;
    while (!result && size) {
        if (atomic_load(&c->closed)) { result = DM_SSL_BAD_SOCKET; break; }
        // SSLWrite's processed count can describe its encrypted cache rather than the
        // native socket. Flush that cache with no new data before acknowledging this
        // bounded record; retrying the plaintext would duplicate it on a blocking path.
        bool flushing = c.pendingWrite != nil; unsigned request = flushing ? (unsigned)c.pendingWrite.length : MIN(size, 16384u);
        c->wanted = POLLOUT; size_t n = 0; OSStatus status = SSLWrite(c->tls, flushing ? NULL : buffer, flushing ? 0 : request, &n);
        if (!status) { *out = flushing ? (int)request : (int)n; c.pendingWrite = nil; break; }
        if (status != errSSLWouldBlock) { result = status_result(c, status); break; }
        if (!flushing) { c.pendingWrite = [NSData dataWithBytes:buffer length:request]; if (!c.pendingWrite) { result = DM_SSL_FAIL; break; } }
        if (c.ioMode == 2) { result = DM_SSL_WOULD_BLOCK; break; }
        result = wait_socket(c, c->wanted, deadline);
    }
    if ((!result && *out > 0 && !c.tracedWrite) || (result && result != DM_SSL_WOULD_BLOCK)) {
        trace(c, DM_TLS_WRITE, result, 0, (unsigned)*out); if (!result) c.tracedWrite = YES;
    }
    error(c, result); [c.lock unlock]; return result;
} }
uint32_t dm_ssl_pending(void *object, int *out) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER; *out = 0; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; size_t pending = 0; OSStatus status = c->tls ? SSLGetBufferedReadSize(c->tls, &pending) : noErr;
    if (!status) *out = (int)MIN((size_t)INT_MAX, pending + c.lookahead.length); [c.lock unlock]; return status_result(c, status);
} }
uint32_t dm_ssl_poll(void *object, unsigned *out, const unsigned *events, unsigned timeout_ms) { @autoreleasepool {
    if (!object || !out || !events) return DM_SSL_BAD_POINTER; *out = 0; unsigned requested = *events;
    if (requested & ~7u) return DM_SSL_FAIL; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; [c.lock lock];
    if (atomic_load(&c->descriptor) < 0) { [c.lock unlock]; return error(c, DM_SSL_NO_SOCKET); }
    size_t pending = 0; if (c->tls) SSLGetBufferedReadSize(c->tls, &pending);
    if ((requested & 1) && (pending || c.lookahead.length)) *out |= 1;
    uint64_t deadline = clock_ms() + timeout_ms; uint32_t result = 0;
    for (;;) {
        if (atomic_load(&c->closed)) { result = DM_SSL_BAD_SOCKET; break; }
        uint64_t now = clock_ms(), remaining = deadline > now ? deadline - now : 0; int ms = *out ? 0 : (int)MIN(remaining, 100);
        struct pollfd p = {.fd = atomic_load(&c->descriptor), .events = ((requested & 1) ? POLLIN : 0) | ((requested & 2) ? POLLOUT : 0) | ((requested & 4) ? POLLPRI : 0)};
        dm_guest_host_wait_begin(); int n = poll(&p, 1, ms), e = errno; dm_guest_host_wait_end();
        if (n < 0 && e == EINTR) continue; if (n < 0) { result = DM_SSL_BAD_SOCKET; break; }
        if ((requested & 1) && (p.revents & (POLLIN | POLLHUP))) *out |= 1;
        if ((requested & 2) && (p.revents & POLLOUT)) *out |= 2;
        if (p.revents & (POLLERR | POLLNVAL | POLLPRI)) *out |= 4;
        if (*out || clock_ms() >= deadline) break;
    }
    [c.lock unlock]; return error(c, result);
} }
uint32_t dm_ssl_last_error(void *object, uint32_t *out, bool verify) { @autoreleasepool {
    if (!object || !out) return DM_SSL_BAD_POINTER; DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL;
    [c.lock lock]; *out = verify ? c.verifyError : c.lastError; if (verify) c.verifyError = 0; [c.lock unlock]; return 0;
} }
uint32_t dm_ssl_set_alpn(void *object, const uint8_t *data, unsigned size) { @autoreleasepool {
    if (!object || !data) return DM_SSL_BAD_POINTER; if (size < 2 || size > 4096) return DM_SSL_FAIL;
    DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; NSMutableArray *protocols = [NSMutableArray new];
    for (unsigned pos = 0; pos < size;) { unsigned n = data[pos++]; if (!n || n > size - pos) return DM_SSL_FAIL; NSString *p = [[NSString alloc] initWithBytes:data + pos length:n encoding:NSASCIIStringEncoding]; if (!p) return DM_SSL_FAIL; [protocols addObject:p]; pos += n; }
    [c.lock lock]; if (c.handshaking || atomic_load(&c->descriptor) < 0) { [c.lock unlock]; return DM_SSL_FAIL; } c.alpn = protocols; [c.lock unlock]; return 0;
} }
uint32_t dm_ssl_get_alpn(void *object, unsigned *state, uint8_t *buffer, unsigned *size, unsigned capacity) { @autoreleasepool {
    if (!object || !state || !size || (!buffer && capacity)) return DM_SSL_BAD_POINTER; *state = *size = 0;
    DmTlsConnection *c = get_connection(object); if (!c) return DM_SSL_FAIL; [c.lock lock];
    CFArrayRef protocols = NULL; OSStatus status = c->tls ? SSLCopyALPNProtocols(c->tls, &protocols) : noErr;
    if (!status && protocols && CFArrayGetCount(protocols)) {
        NSString *selected = (__bridge NSString *)CFArrayGetValueAtIndex(protocols, 0); NSData *data = [selected dataUsingEncoding:NSASCIIStringEncoding]; *size = (unsigned)data.length; *state = 1;
        NSUInteger length = data.length;
        if (capacity < length || (length && !buffer)) status = errSecParam; else if (length) memcpy(buffer, data.bytes, length);
    }
    if (protocols) CFRelease(protocols); [c.lock unlock]; return status_result(c, status);
} }
uint32_t dm_ssl_cert_detail(void *out, const void *buffer, unsigned index) {
    if (!out || !buffer) return DM_SSL_BAD_POINTER;
    uint64_t magic; uint32_t count; memcpy(&magic, buffer, 8); memcpy(&count, (const char *)buffer + 8, 4);
    if (magic != 0x4e4d684374726543ull || index >= count || count > 64) return DM_SSL_FAIL;
    uint32_t entry[2]; memcpy(entry, (const char *)buffer + 16 + index * 8, 8);
    if (entry[1] < 16 + count * 8 || entry[1] > UINT32_MAX - entry[0]) return DM_SSL_FAIL;
    struct { uint32_t size, pad; const void *data; } detail = {entry[0], 0, (const char *)buffer + entry[1]}; memcpy(out, &detail, sizeof detail); return 0;
}
#pragma clang diagnostic pop
