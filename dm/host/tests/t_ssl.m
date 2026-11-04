// Real loopback TLS through the guest's actual C / C++ HLE entries and production socket ownership API.
// test_ssl_transport.py creates an ephemeral test CA/server and invokes this executable; no user credentials or trust-store mutations.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include "../hle.h"
#include "t_gate_fixture.h"
#include "../ssl_backend.h"
#include "../net_socket.h"
#include <arpa/inet.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
static _Atomic int entered, left, waits;
static NSData *caPem, *caDer, *clientPki;
static NSString *systemRoot;
static unsigned port;
static char context_key[32], connection_key[32];
#define CHECK(condition, message) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); } } while (0)
static uint64_t milliseconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
void dm_guest_host_wait_begin(void) { atomic_fetch_add(&waits, 1); }
void dm_guest_host_wait_end(void) {}
void thr_svc_enter(uint64_t *registers, uint64_t pc) { (void)registers; (void)pc; atomic_fetch_add(&entered, 1); }
void thr_svc_leave(void) { atomic_fetch_add(&left, 1); }
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
uint64_t gate_dispatch(uint64_t ret, uint64_t *registers) { (void)ret; (void)registers; abort(); }
uint64_t unimpl_c(unsigned index, uint64_t *registers) { (void)index; (void)registers; abort(); }
void dm_thread_returned(void) { abort(); }
static void *hook(const char *name) {
    fixture_gate_prepare();
    unsigned long bytes; const hle_force_ent *p = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (unsigned long i = 0; p && i < bytes / sizeof *p; i++) if (!strcmp(p[i].name, name)) return p[i].fn;
    fprintf(stderr, "Missing TLS hook: %s\n", name); abort();
}
#define H(type, name) ((type)hook(name))
typedef uint32_t (*ContextCreate)(void *, unsigned);
typedef uint32_t (*ConnectionCreate)(void *, void *);
typedef uint32_t (*ObjectCall)(void *);
typedef uint32_t (*SetInt)(void *, unsigned);
typedef uint32_t (*Read)(void *, void *, int *, unsigned);
typedef uint32_t (*Write)(void *, const void *, int *, unsigned);
typedef uint32_t (*Poll)(void *, unsigned *, const unsigned *, unsigned);
typedef struct { uint8_t len, family; uint16_t port; uint32_t ip; uint8_t padding[8]; } Address;
static void identifier_lifecycle(void) {
    typedef uint32_t (*GetId)(void *, uint64_t *);
    uint64_t id = UINT64_MAX, context_id = UINT64_MAX, connection_id = UINT64_MAX;
    CHECK(H(GetId, "nnsslContextGetContextId")(context_key, &id) == 0 && id == 0,
          "empty C context query succeeds with identifier zero before creation");
    CHECK(H(GetId, "_ZN2nn3ssl7Context12GetContextIdEPm")(context_key, &id) == 0 && id == 0,
          "empty C++ context query has the same SDK semantics");
    CHECK(H(GetId, "nnsslConnectionGetConnectionId")(connection_key, &id) == 0 && id == 0,
          "empty connection query succeeds with identifier zero");
    CHECK(H(GetId, "_ZN2nn3ssl10Connection12GetContextIdEPm")(connection_key, &id) == 0 && id == 0,
          "empty connection has no associated context identifier");
    CHECK(H(ObjectCall, "nnsslConnectionDoHandshake")(connection_key) != 0 &&
          H(ConnectionCreate, "nnsslConnectionCreate")(connection_key, context_key) != 0,
          "identifier queries do not create a context or grant a TLS session");
    CHECK(H(ContextCreate, "_ZN2nn3ssl7Context6CreateENS1_10SslVersionE")(context_key, 1) == 0 &&
          H(GetId, "nnsslContextGetContextId")(context_key, &context_id) == 0 && context_id,
          "C++ context creation after the empty C query produces a real identifier");
    CHECK(H(ConnectionCreate, "nnsslConnectionCreate")(connection_key, context_key) == 0 &&
          H(GetId, "nnsslConnectionGetConnectionId")(connection_key, &connection_id) == 0 && connection_id && connection_id != context_id,
          "connection creation retains a distinct live identifier");
    CHECK(H(GetId, "_ZN2nn3ssl10Connection12GetContextIdEPm")(connection_key, &id) == 0 && id == context_id,
          "live connection reports its actual parent context identifier");
    CHECK(H(ObjectCall, "nnsslConnectionDestroy")(connection_key) == 0 &&
          H(GetId, "nnsslConnectionGetConnectionId")(connection_key, &id) == 0 && id == 0,
          "destroyed connection returns to the empty identifier state");
    CHECK(H(ObjectCall, "nnsslContextDestroy")(context_key) == 0 &&
          H(GetId, "nnsslContextGetContextId")(context_key, &id) == 0 && id == 0,
          "destroyed context returns to the empty identifier state");
    CHECK(H(GetId, "nnsslContextGetContextId")(NULL, &id) == DM_SSL_BAD_POINTER &&
          H(GetId, "nnsslContextGetContextId")(context_key, NULL) == DM_SSL_BAD_POINTER &&
          H(GetId, "nnsslConnectionGetConnectionId")(NULL, &id) == DM_SSL_BAD_POINTER &&
          H(GetId, "nnsslConnectionGetConnectionId")(connection_key, NULL) == DM_SSL_BAD_POINTER,
          "identifier query null-pointer errors remain failures");
}
static int create(bool import) {
    memset(context_key, 0xa5, sizeof context_key); memset(connection_key, 0x5a, sizeof connection_key);
    CHECK(H(ContextCreate, "nnsslContextCreate")(context_key, 32) == 0, "C context create uses caller object pointer");
    if (import) {
        uint64_t id = 0;
        CHECK(H(uint32_t (*)(void *, uint64_t *, const void *, unsigned, int), "_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE")(context_key, &id, caPem.bytes, (unsigned)caPem.length, 1) == 0 && id, "actual C++ ImportServerPki imports only a test CA into this context");
    }
    CHECK(H(ConnectionCreate, "nnsslConnectionCreate")(connection_key, context_key) == 0, "C connection create links actual context");
    CHECK(context_key[0] == (char)0xa5 && connection_key[0] == 0x5a, "no guessed SDK private fields are overwritten");
    CHECK(H(ObjectCall, "nnsslContextDestroy")(context_key) != 0, "context cannot be destroyed with an active connection");
    int fd = dm_net_socket(2, 1, 6); Address address = {16, 2, htons(port), htonl(0x7f000001), {0}};
    CHECK(fd >= 0 && dm_net_connect(fd, &address, sizeof address) == 0, "production guest socket connects to local TLS server");
    CHECK(H(uint32_t (*)(void *, int), "nnsslConnectionSetSocketDescriptor")(connection_key, fd) == 0, "TLS borrows an owned native duplicate with its generation");
    return fd;
}
static void hostname(const char *name) { CHECK(H(uint32_t (*)(void *, const char *, unsigned), "nnsslConnectionSetHostName")(connection_key, name, (unsigned)strlen(name)) == 0, "set actual TLS peer hostname"); }
static uint32_t handshake(bool nonblock) {
    CHECK(H(SetInt, "nnsslConnectionSetIoMode")(connection_key, nonblock ? 2 : 1) == 0, "select TLS blocking mode");
    uint64_t deadline = milliseconds() + 5000; unsigned wouldblock = 0;
    for (;;) {
        uint32_t r = H(ObjectCall, "nnsslConnectionDoHandshake")(connection_key);
        if (r != DM_SSL_WOULD_BLOCK) { if (nonblock) CHECK(wouldblock > 0, "nonblocking handshake actually yielded WouldBlock"); return r; }
        wouldblock++; if (milliseconds() > deadline) return DM_SSL_TIMEOUT;
        unsigned wanted = 1, ready = 0; H(Poll, "nnsslConnectionPoll")(connection_key, &ready, &wanted, 100);
    }
}
static uint32_t read_some(void *bytes, int *n, unsigned capacity, bool peek) {
    uint64_t deadline = milliseconds() + 5000;
    for (;;) {
        uint32_t result = H(Read, peek ? "nnsslConnectionPeek" : "nnsslConnectionRead")(connection_key, bytes, n, capacity);
        if (result != DM_SSL_WOULD_BLOCK) return result;
        if (milliseconds() >= deadline) return DM_SSL_TIMEOUT;
        unsigned wanted = 1, ready = 0; H(Poll, "nnsslConnectionPoll")(connection_key, &ready, &wanted, 100);
    }
}
static void destroy(int fd, bool already_closed) {
    CHECK(H(ObjectCall, "nnsslConnectionDestroy")(connection_key) == 0, "TLS connection destroy");
    if (!already_closed) { Address a; unsigned n = sizeof a; CHECK(dm_net_name(fd, &a, &n, 0) < 0 && dm_net_errno() == 9, "default TLS ownership closes the original generation"); }
    CHECK(H(ObjectCall, "nnsslContextDestroy")(context_key) == 0, "TLS context released");
}
static void trusted(void) {
    int fd = create(true); hostname("localhost");
    CHECK(H(uint32_t (*)(void *, int, unsigned), "nnsslConnectionSetOption")(connection_key, 3, 1) == 0, "enable ALPN");
    uint8_t alpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    CHECK(H(uint32_t (*)(void *, const uint8_t *, unsigned), "nnsslConnectionSetNextAlpnProto")(connection_key, alpn, sizeof alpn) == 0, "SDK19 C ALPN input buffer signature");
    CHECK(handshake(true) == 0, "trusted loopback TLS handshake validates chain and hostname");
    unsigned omitted_size = 99, omitted_count = 99;
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *), "nnsslConnectionDoHandshakeWithCertBuffer")(connection_key, &omitted_size, &omitted_count) == 0 && !omitted_size && !omitted_count,
          "an unregistered certificate buffer performs a verified handshake without exporting certificates");
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *), "_ZN2nn3ssl10Connection11DoHandshakeEPjS2_")(connection_key, NULL, NULL) == 0,
          "the SDK permits omitted certificate output pointers");
    uint8_t untouched = 0x7a; omitted_size = omitted_count = 99;
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *, void *, unsigned), "nnsslConnectionDoHandshakeWithBuffer")(connection_key, &omitted_size, &omitted_count, &untouched, 0) == 0 && !omitted_size && !omitted_count && untouched == 0x7a,
          "a zero-capacity certificate buffer follows the SDK's handshake-only branch without writing it");
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *, void *, unsigned), "_ZN2nn3ssl10Connection11DoHandshakeEPjS2_Pcj")(connection_key, &omitted_size, &omitted_count, NULL, 8192) == 0 && !omitted_size && !omitted_count,
          "a null explicit certificate buffer also selects handshake-only semantics");
    unsigned state = 0, size = 0; uint8_t selected[16] = {0};
    CHECK(H(uint32_t (*)(void *, unsigned *, uint8_t *, unsigned *, unsigned), "nnsslConnectionGetNextAlpnProto")(connection_key, &state, selected, &size, sizeof selected) == 0 && state == 1 && size == 8 && !memcmp(selected, "http/1.1", 8), "native ALPN reports the server selection rather than the client's first choice");
    char cipher[72]; CHECK(H(uint32_t (*)(void *, void *), "_ZN2nn3ssl10Connection13GetCipherInfoEPNS1_10CipherInfoE")(connection_key, cipher) == 0 && !strcmp(cipher + 64, "TLSv1.2"), "actual negotiated TLS version reported");
    unsigned wanted = 1, ready = 99; uint64_t start = milliseconds();
    CHECK(H(Poll, "nnsslConnectionPoll")(connection_key, &ready, &wanted, 10) == 0 && ready == 0 && milliseconds() - start >= 8, "TLS poll honors the full finite timeout");
    char bytes[64] = {0}; int n = 99; uint32_t result = H(Read, "nnsslConnectionRead")(connection_key, bytes, &n, sizeof bytes);
    CHECK(result == DM_SSL_WOULD_BLOCK && n == 0, "nonblocking TLS read exposes proper WouldBlock result");
    CHECK(read_some(bytes, &n, 5, true) == 0 && n == 5 && !memcmp(bytes, "HELLO", 5), "TLS peek decrypts without consuming");
    int pending = 0; CHECK(H(uint32_t (*)(void *, int *), "nnsslConnectionPending")(connection_key, &pending) == 0 && pending >= 5, "peek data included in pending");
    CHECK(read_some(bytes, &n, 5, false) == 0 && n == 5 && !memcmp(bytes, "HELLO", 5), "TLS read consumes peeked data exactly once");
    n = 0; CHECK(H(Write, "nnsslConnectionWrite")(connection_key, "ECHO", &n, 4) == 0 && n == 4, "actual TLS write");
    unsigned total = 0;
    while (total < 4) { n = 0; result = read_some(bytes + total, &n, 4 - total, false); if (result || n <= 0) break; total += (unsigned)n; }
    CHECK(total == 4 && !memcmp(bytes, "ABCD", 4), "partial native TLS records preserve bytes");
    uint8_t certificates[8193]; memset(certificates, 0x7a, sizeof certificates); unsigned count = 0; size = 0;
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *, void *, unsigned), "nnsslConnectionDoHandshakeWithBuffer")(connection_key, &size, &count, certificates, 8192) == 0 && size > 128 && count == 1 && certificates[8192] == 0x7a, "verified explicit-buffer C handshake signature exports leaf DER");
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *, void *, unsigned), "nnsslConnectionDoHandshakeWithBuffer")(connection_key, NULL, NULL, certificates, 8192) == 0 && certificates[8192] == 0x7a,
          "a real certificate buffer is exported even when its metadata output pointers are omitted");
    certificates[0] = 0x7a;
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *, void *, unsigned), "nnsslConnectionDoHandshakeWithBuffer")(connection_key, &size, &count, certificates, 1) != 0 && certificates[0] == 0x7a,
          "a nonempty undersized certificate buffer still fails without a partial certificate write");
    CHECK(H(uint32_t (*)(void *, void *, unsigned), "nnsslConnectionSetServerCertBuffer")(connection_key, certificates, 8192) == 0, "register server certificate buffer");
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *), "nnsslConnectionDoHandshakeWithCertBuffer")(connection_key, &size, &count) == 0 && size > 128 && count == 1, "verified three-argument WithCertBuffer uses the registered buffer");
    CHECK(H(uint32_t (*)(void *, int, unsigned), "nnsslConnectionSetOption")(connection_key, 1, 1) == 0, "request actual server certificate chain");
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *, void *, unsigned), "nnsslConnectionDoHandshakeWithBuffer")(connection_key, &size, &count, certificates, 8192) == 0 && count >= 1 && !memcmp(certificates, "CertChMN", 8), "actual chain envelope produced");
    struct { uint32_t size, pad; const uint8_t *data; } detail = {0};
    CHECK(H(uint32_t (*)(void *, void *, const void *, unsigned), "nnsslConnectionGetServerCertDetail")(connection_key, &detail, certificates, 0) == 0 && detail.size > 128 && detail.data >= certificates && detail.data + detail.size <= certificates + size, "chain detail layout and offset correct");
    CHECK(read_some(bytes, &n, sizeof bytes, false) == 0 && n == 0, "close_notify is orderly TLS EOF");
    destroy(fd, false);
}
static void rejected(bool import, const char *name) {
    int fd = create(import); hostname(name);
    CHECK(handshake(false) != 0, import ? "wrong hostname rejected with trusted CA" : "untrusted test CA rejected");
    uint32_t verify = 0; CHECK(H(uint32_t (*)(void *, uint32_t *), "nnsslConnectionGetVerifyCertError")(connection_key, &verify) == 0 && verify != 0, "certificate verification failure retained");
    CHECK(H(ObjectCall, "nnsslConnectionDoHandshake")(connection_key) != 0, "failed trust cannot be bypassed by retrying after reading its error");
    unsigned size = 99, count = 99;
    CHECK(H(uint32_t (*)(void *, unsigned *, unsigned *), "nnsslConnectionDoHandshakeWithCertBuffer")(connection_key, &size, &count) != 0 && !size && !count,
          "omitting certificate export never bypasses failed chain or hostname verification");
    destroy(fd, false);
}
static bool scoped_policy(const char *guest, char *host, size_t capacity, const uint8_t **der, size_t *size) {
    if (strcmp(guest, "api.console.invalid") || capacity < 10) return false;
    memcpy(host, "localhost", 10); *der = caDer.bytes; *size = caDer.length; return true;
}
static void scoped(void) {
    dm_ssl_set_peer_policy(scoped_policy); int fd = create(false); hostname("api.console.invalid");
    CHECK(handshake(false) == 0, "explicit host policy rewrites SNI and scopes a copied DER CA"); destroy(fd, false);
    rejected(false, "outside.invalid"); dm_ssl_set_peer_policy(NULL);
}
static void ownership(void) {
    int fd = create(false); CHECK(dm_net_close(fd) == 0, "application closes its original while TLS retains a duplicate");
    int reused = dm_net_socket(2, 2, 17); CHECK(reused == fd, "guest descriptor slot reused"); destroy(fd, true);
    Address a; unsigned n = sizeof a; CHECK(dm_net_name(reused, &a, &n, 0) == 0, "TLS destroy generation guard preserves the reused guest descriptor"); dm_net_close(reused);
    CHECK(H(ContextCreate, "nnsslContextCreate")(context_key, 32) == 0 && H(ConnectionCreate, "nnsslConnectionCreate")(connection_key, context_key) == 0, "second ownership context");
    CHECK(H(uint32_t (*)(void *, int, unsigned), "nnsslConnectionSetOption")(connection_key, 0, 1) == 0, "DoNotCloseSocket configured before attachment");
    fd = dm_net_socket(2, 1, 6); Address address = {16, 2, htons(port), htonl(0x7f000001), {0}};
    CHECK(dm_net_connect(fd, &address, sizeof address) == 0 && H(uint32_t (*)(void *, int), "nnsslConnectionSetSocketDescriptor")(connection_key, fd) == 0, "attach independently owned connection");
    CHECK(H(ObjectCall, "nnsslConnectionDestroy")(connection_key) == 0, "destroy DoNotCloseSocket connection"); n = sizeof a;
    CHECK(dm_net_name(fd, &a, &n, 0) == 0, "explicit DoNotCloseSocket preserves the original"); dm_net_close(fd); H(ObjectCall, "nnsslContextDestroy")(context_key);
    CHECK(H(ContextCreate, "nnsslContextCreate")(context_key, 32) == 0 && H(ConnectionCreate, "nnsslConnectionCreate")(connection_key, context_key) == 0, "stream-only adapter context");
    fd = dm_net_socket(2, 2, 17); CHECK(fd >= 0 && dm_net_connect(fd, &address, sizeof address) == 0, "native UDP socket is actually connected");
    CHECK(H(uint32_t (*)(void *, int), "nnsslConnectionSetSocketDescriptor")(connection_key, fd) == DM_SSL_BAD_SOCKET, "stream TLS rejects connected UDP before taking ownership");
    CHECK(H(ObjectCall, "nnsslConnectionDestroy")(connection_key) == 0, "rejected datagram connection destroys safely"); n = sizeof a;
    CHECK(dm_net_name(fd, &a, &n, 0) == 0, "failed socket attachment leaves the caller's datagram open"); dm_net_close(fd); H(ObjectCall, "nnsslContextDestroy")(context_key);
}
static void abrupt(void) {
    int fd = create(true); hostname("localhost"); CHECK(handshake(false) == 0, "abrupt-close test handshake");
    char bytes[16]; int n = 0; CHECK(read_some(bytes, &n, 5, false) == 0 && n == 5, "abrupt-close greeting");
    CHECK(H(Write, "nnsslConnectionWrite")(connection_key, "ABRT", &n, 4) == 0 && n == 4, "request abrupt TCP close from the test server");
    CHECK(read_some(bytes, &n, sizeof bytes, false) != 0 && n == 0, "raw TCP EOF without close_notify is a TLS truncation error"); destroy(fd, false);
}
typedef struct { int result; } BlockedRead;
static void *blocked_reader(void *opaque) {
    BlockedRead *call = opaque; char byte;
    call->result = H(int (*)(void *, char *, unsigned), "_ZN2nn3ssl10Connection4ReadEPcj")(connection_key, &byte, 1);
    return NULL;
}
static void concurrent_destroy(void) {
    int fd = create(true); hostname("localhost"); CHECK(handshake(false) == 0, "concurrent close handshake");
    char bytes[8]; int n = 0; CHECK(read_some(bytes, &n, 5, false) == 0 && n == 5, "concurrent close greeting");
    CHECK(H(int (*)(void *, const char *, unsigned), "_ZN2nn3ssl10Connection5WriteEPKcj")(connection_key, "WAIT", 4) == 4, "scalar C++ Write returns the real byte count");
    BlockedRead call = {0}; pthread_t thread; CHECK(pthread_create(&thread, NULL, blocked_reader, &call) == 0, "start actual blocking TLS read");
    struct timespec pause = {0, 30000000}; nanosleep(&pause, NULL); uint64_t start = milliseconds();
    CHECK(H(ObjectCall, "nnsslConnectionDestroy")(connection_key) == 0, "destroy can cancel a connection while its TLS read owns it");
    int reused = dm_net_socket(2, 2, 17); CHECK(reused == fd, "original slot reused while old read exits");
    CHECK(pthread_join(thread, NULL) == 0 && call.result == -1 && milliseconds() - start < 1000, "scalar C++ Read exits on close without using the reused slot");
    Address a; unsigned length = sizeof a; CHECK(dm_net_name(reused, &a, &length, 0) == 0, "old TLS operation leaves the reused descriptor intact"); dm_net_close(reused);
    CHECK(H(ObjectCall, "nnsslContextDestroy")(context_key) == 0, "context released after canceled operation");
}
static void mutual_authentication(void) {
    if (@available(macOS 15.0, iOS 18.0, *)) {
        int fd = create(true); uint64_t identifier = 0;
        typedef uint32_t (*ImportClient)(void *, uint64_t *, const char *, const char *, unsigned, unsigned);
        ImportClient import = H(ImportClient, "_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj");
        CHECK(import(context_key, NULL, clientPki.bytes, "test-only", (unsigned)clientPki.length, 9) == DM_SSL_BAD_POINTER, "PKCS12 import requires its public output ID pointer");
        CHECK(import(context_key, &identifier, clientPki.bytes, "incorrect", (unsigned)clientPki.length, 9) != 0, "PKCS12 rejects a wrong test password");
        CHECK(import(context_key, &identifier, clientPki.bytes, "test-only", (unsigned)clientPki.length, 9) == 0 && identifier, "test client identity imported only into memory");
        hostname("localhost"); CHECK(handshake(false) == 0, "mutual TLS client identity handshake");
        char bytes[8]; int n = 0;
        CHECK(read_some(bytes, &n, 5, false) == 0 && n == 5, "mutual TLS greeting");
        CHECK(H(Write, "nnsslConnectionWrite")(connection_key, "MTLS", &n, 4) == 0 && n == 4, "request peer-observed certificate evidence");
        CHECK(read_some(bytes, &n, 4, false) == 0 && n == 4 && !memcmp(bytes, "CERT", 4), "server verified the real client certificate and private-key proof");
        CHECK(H(uint32_t (*)(void *, uint64_t), "_ZN2nn3ssl7Context9RemovePkiEm")(context_key, identifier) == 0, "imported client identity can be removed");
        destroy(fd, false);
    }
}
static void backpressure(bool nonblock) {
    int fd = create(true); hostname("localhost"); CHECK(handshake(nonblock) == 0, "backpressure handshake");
    int small = 4096; unsigned option_size = sizeof small; CHECK(dm_net_sockopt(fd, 0xffff, 0x1001, &small, &option_size, 1) == 0, "constrain the actual TCP send buffer");
    char greeting[8]; int n = 0; CHECK(read_some(greeting, &n, 5, false) == 0 && n == 5, "backpressure greeting");
    CHECK(H(Write, "nnsslConnectionWrite")(connection_key, "BULK", &n, 4) == 0 && n == 4, "begin delayed native server read");
    unsigned size = 2 * 1024 * 1024, position = 0, blocked = 0; uint8_t *data = malloc(size); for (unsigned i = 0; i < size; i++) data[i] = i % 251;
    uint64_t deadline = milliseconds() + 8000;
    while (position < size && milliseconds() < deadline) {
        n = 0; uint32_t result = H(Write, "nnsslConnectionWrite")(connection_key, data + position, &n, size - position);
        if (result == DM_SSL_WOULD_BLOCK) { blocked++; if (n) break; unsigned wanted = 2, ready = 0; H(Poll, "nnsslConnectionPoll")(connection_key, &ready, &wanted, 100); }
        else if (result || n <= 0 || (unsigned)n > size - position) break;
        else position += (unsigned)n;
    }
    CHECK(position == size && (!nonblock || blocked > 0), "TLS writes resume under native backpressure in the selected I/O mode");
    CHECK(read_some(greeting, &n, 4, false) == 0 && n == 4 && !memcmp(greeting, "GOOD", 4), "server receives every byte once, including the last cached TLS record");
    free(data); destroy(fd, false);
}
static void read_timeout(void) {
    int fd = create(true); hostname("localhost"); CHECK(handshake(false) == 0, "timeout test handshake");
    char bytes[8]; int n = 0; CHECK(read_some(bytes, &n, 5, false) == 0 && n == 5, "timeout test greeting");
    CHECK(H(Write, "nnsslConnectionWrite")(connection_key, "WAIT", &n, 4) == 0 && n == 4, "server deliberately waits without TLS plaintext");
    CHECK(H(SetInt, "_ZN2nn3ssl17ConnectionPrivate12SetIoTimeoutEj")(connection_key, 30) == 0, "set the actual SDK I/O timeout");
    uint64_t start = milliseconds(); uint32_t result = H(Read, "nnsslConnectionRead")(connection_key, bytes, &n, sizeof bytes);
    CHECK(result == DM_SSL_TIMEOUT && n == 0 && milliseconds() - start >= 25 && milliseconds() - start < 1000, "blocking TLS read observes its real deadline and returns Horizon Timeout");
    destroy(fd, false);
}
typedef struct { uint32_t id, status; uint64_t size; const uint8_t *data; } BuiltinInfo;
static NSString *store_path(void) { return [systemRoot stringByAppendingPathComponent:@"0100000000000800/files/ssl_TrustedCerts.bdf"]; }
static NSMutableData *store_fixture(uint32_t status) {
    NSMutableData *data = [NSMutableData dataWithLength:24 + caDer.length]; uint32_t header[] = {0x546c7373, 1, 1, status, (uint32_t)caDer.length, 16};
    memcpy(data.mutableBytes, header, sizeof header); memcpy((char *)data.mutableBytes + 24, caDer.bytes, caDer.length); return data;
}
static void builtin_store(void) {
    typedef uint32_t (*GetSize)(unsigned *, const uint32_t *, unsigned);
    typedef uint32_t (*GetCerts)(void **, uint8_t *, unsigned, const uint32_t *, unsigned);
    GetSize get_size = H(GetSize, "_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj");
    GetCerts get_certs = H(GetCerts, "_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj");
    unsetenv("DM_SYSTEM_DATA_DIR"); unsigned size = 99; uint32_t id = 1;
    CHECK(dm_ssl_import_server(context_key, NULL, caDer.bytes, (unsigned)caDer.length, 2) == DM_SSL_BAD_POINTER, "server certificate import requires its public output ID pointer");
    CHECK(get_size(&size, &id, 1) != 0 && !size, "missing firmware never fabricates a built-in CA");
    [[NSFileManager defaultManager] createDirectoryAtPath:[store_path() stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("DM_SYSTEM_DATA_DIR", systemRoot.fileSystemRepresentation, 1); NSMutableData *fixture = store_fixture(1);
    CHECK([fixture writeToFile:store_path() atomically:YES], "write synthetic public test CA store outside the repository");
    CHECK(get_size(&size, &id, 1) == 0 && size == 24 + ((caDer.length + 3) & ~(NSUInteger)3), "BDF size uses the SDK's 24-byte pointer record and aligned DER");
    uint8_t *buffer = malloc(size + 25); memset(buffer, 0x6b, size + 25); void *output = (void *)1;
    CHECK(get_certs(&output, buffer, size - 1, &id, 1) != 0 && output == NULL && buffer[0] == 0x6b, "undersized certificate output writes no partial metadata");
    CHECK(get_certs(&output, buffer, size, &id, 1) == 0 && output == buffer && buffer[size] == 0x6b, "built-in output respects caller capacity");
    BuiltinInfo info; memcpy(&info, buffer, sizeof info);
    CHECK(sizeof info == 24 && info.id == 1 && info.status == 1 && info.size == caDer.length && info.data == buffer + 24 && !memcmp(info.data, caDer.bytes, caDer.length), "actual built-in ID, trust status, size, pointer and DER preserved");
    id = UINT32_MAX; unsigned all_size = 0; CHECK(get_size(&all_size, &id, 1) == 0 && all_size == size + 24, "All query reserves its real end sentinel");
    CHECK(get_certs(&output, buffer, all_size, &id, 1) == 0, "All query exports original entries");
    BuiltinInfo sentinel; memcpy(&sentinel, buffer + 24, sizeof sentinel);
    CHECK(sentinel.id == UINT32_MAX && sentinel.status == UINT32_MAX && sentinel.size == 0 && sentinel.data == NULL, "All query terminates without a fake certificate");
    id = 999; CHECK(get_size(&size, &id, 1) != 0 && !size, "unknown firmware certificate ID fails");
    id = 1; uint32_t bad_offset = UINT32_MAX; memcpy((char *)fixture.mutableBytes + 20, &bad_offset, 4); [fixture writeToFile:store_path() atomically:YES];
    CHECK(get_size(&size, &id, 1) != 0 && !size, "BDF entry outside file bounds rejected");
    fixture = store_fixture(1); ((char *)fixture.mutableBytes)[24] = 0; [fixture writeToFile:store_path() atomically:YES];
    CHECK(get_size(&size, &id, 1) != 0 && !size, "malformed DER rejected before exposing an entry");
    fixture = store_fixture(1); ((char *)fixture.mutableBytes)[0] = 0; [fixture writeToFile:store_path() atomically:YES];
    CHECK(get_size(&size, &id, 1) != 0 && !size, "wrong BDF magic rejected");
    fixture = store_fixture(2); [fixture writeToFile:store_path() atomically:YES]; rejected(false, "localhost");
    fixture = store_fixture(1); [fixture writeToFile:store_path() atomically:YES];
    int fd = create(false); hostname("localhost"); CHECK(handshake(false) == 0, "original trusted firmware anchor applies only to the TLS context"); destroy(fd, false);
    rejected(false, "wrong.invalid");
    free(buffer); [[NSFileManager defaultManager] removeItemAtPath:store_path() error:nil]; unsetenv("DM_SYSTEM_DATA_DIR");
}
static int check_real_store(const char *directory) {
    setenv("DM_NETWORK", "lan", 1); setenv("DM_SYSTEM_DATA_DIR", directory, 1); uint32_t all = UINT32_MAX; unsigned size = 0;
    CHECK(dm_ssl_builtin_size(&size, &all, 1) == 0 && size > 24, "real firmware certificate table parsed");
    if (!size) return 1; uint8_t *bytes = malloc(size + 1); bytes[size] = 0xa5; void *out = NULL;
    CHECK(dm_ssl_builtin_certificates(&out, bytes, size, &all, 1) == 0 && out == bytes && bytes[size] == 0xa5, "real firmware output fits exact advertised size");
    unsigned count = 0, trusted_count = 0;
    for (unsigned i = 0; i < size / 24; i++) {
        BuiltinInfo info; memcpy(&info, bytes + i * 24, sizeof info); if (info.id == UINT32_MAX) break;
        CHECK(info.data >= bytes && info.data <= bytes + size && info.size <= (uint64_t)(bytes + size - info.data), "real certificate pointer stays within the output allocation");
        NSData *der = [NSData dataWithBytes:info.data length:(NSUInteger)info.size]; SecCertificateRef certificate = SecCertificateCreateWithData(NULL, (__bridge CFDataRef)der);
        CHECK(certificate != NULL && info.status <= 3, "real firmware certificate DER and status valid"); if (certificate) CFRelease(certificate);
        count++; trusted_count += info.status == 1;
    }
    printf("real firmware certificate store: %u entries, %u trusted; %d checks, %d failures\n", count, trusted_count, checks, failures); free(bytes); return failures ? 1 : 0;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 3 && !strcmp(argv[1], "--certstore")) return check_real_store(argv[2]);
    if (argc != 6) return 2;
    NSString *caPemPath=[NSString stringWithUTF8String:argv[1]], *caDerPath=[NSString stringWithUTF8String:argv[2]], *pkiPath=[NSString stringWithUTF8String:argv[4]];
    if (!caPemPath || !caDerPath || !pkiPath) return 2;
    caPem = [NSData dataWithContentsOfFile:caPemPath]; caDer = [NSData dataWithContentsOfFile:caDerPath]; port = (unsigned)strtoul(argv[3], NULL, 10);
    clientPki = [NSData dataWithContentsOfFile:pkiPath];
    systemRoot = [NSString stringWithUTF8String:argv[5]];
    CHECK(caPem.length && caDer.length && clientPki.length && port, "ephemeral TLS test fixtures loaded");
    setenv("DM_NETWORK", "offline", 1);
    CHECK(H(uint32_t (*)(void), "nnsslInitialize")() == 0 && H(ContextCreate, "nnsslContextCreate")(context_key, 32) != 0, "offline SSL initialization compatibility without a fake TLS session");
    setenv("DM_NETWORK", "lan", 1);
    CHECK(H(ContextCreate, "nnsslContextCreate")(context_key, 64) != 0, "TLS1.3-only request is not silently downgraded");
    identifier_lifecycle();
    trusted(); rejected(false, "localhost"); rejected(true, "wrong.invalid"); scoped(); ownership(); abrupt(); concurrent_destroy(); mutual_authentication(); backpressure(true); backpressure(false); read_timeout(); builtin_store();
    CHECK(H(uint32_t (*)(void), "nnsslFinalize")() == 0, "SSL finalization");
    CHECK(atomic_load(&entered) > 0 && atomic_load(&entered) == atomic_load(&left) && atomic_load(&waits) > 0, "TLS blocking operations publish balanced guest contexts and yield while waiting");
    printf("TLS hooks: %d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
} }
