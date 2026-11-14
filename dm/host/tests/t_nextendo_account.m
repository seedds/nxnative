// Exact account HLE hooks with synthetic proof metadata only. The test provider
// prevents every Keychain/document read; no fixture can authenticate to Nextendo.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include "../hle.h"
#include "t_gate_fixture.h"
#include "../nextendo_account.h"
#include "../net_socket.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#import "../nextendo_login_apple.inc"   // the sign-in (hle_nextendo_login_apple.m): its record must pass this decoder

static int checks, failures;
static _Atomic int entered, left, waits, providerCalls;
static dm_net_mode_t mode = DM_NET_NEXTENDO;
static NSData *fixture;
static pthread_mutex_t providerLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t providerCondition = PTHREAD_COND_INITIALIZER;
static bool blockProvider, providerWaiting;
static char contextA[64], contextB[64];
typedef struct { uint64_t id; bool signaled; } TestEvent;
static TestEvent *events[32]; static unsigned eventCount; static uint64_t nextEvent;
static unsigned deletes;
#define CHECK(value, message) do { checks++; if (!(value)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); } } while (0)
dm_net_mode_t dm_net_mode(void) { return mode; }
void dm_guest_host_wait_begin(void) { atomic_fetch_add(&waits, 1); }
void dm_guest_host_wait_end(void) {}
void thr_svc_enter(uint64_t *regs, uint64_t pc) { (void)regs; (void)pc; atomic_fetch_add(&entered, 1); }
void thr_svc_leave(void) { atomic_fetch_add(&left, 1); }
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
uint64_t gate_dispatch(uint64_t ret, uint64_t *regs) { (void)ret; (void)regs; abort(); }
uint64_t unimpl_c(unsigned index, uint64_t *regs) { (void)index; (void)regs; abort(); }
void dm_thread_returned(void) { abort(); }
static void guest_delete(void *pointer) { deletes++; free(pointer); }
void *guest_sym(const char *name) { return !strcmp(name, "_ZdlPv") ? guest_delete : NULL; }
void hle_bind_system_event(void *pointer, int clear) { (void)clear; TestEvent *event = pointer; if (!event->id) { event->id = ++nextEvent; event->signaled = false; events[eventCount++] = event; } }
uint64_t hle_system_event_binding(void *pointer) { return ((TestEvent *)pointer)->id; }
int hle_signal_system_event_binding(uint64_t binding) { for (unsigned i = 0; i < eventCount; i++) if (events[i] && events[i]->id == binding) { events[i]->signaled = true; return 1; } return 0; }
void hle_signal_system_event(void *pointer) { hle_bind_system_event(pointer, 0); hle_signal_system_event_binding(hle_system_event_binding(pointer)); }
static void close_event(TestEvent *event) { for (unsigned i = 0; i < eventCount; i++) if (events[i] == event) events[i] = NULL; event->id = 0; event->signaled = false; }
static void *hook(const char *name) {
    fixture_gate_prepare();
    unsigned long size; const hle_force_ent *table = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &size);
    for (unsigned long i = 0; table && i < size / sizeof *table; i++) if (!strcmp(table[i].name, name)) return table[i].fn;
    fprintf(stderr, "Missing account hook: %s\n", name); abort();
}
#define H(type, name) ((type)hook(name))
static bool provider(uint8_t *buffer, size_t capacity, size_t *size) {
    atomic_fetch_add(&providerCalls, 1); pthread_mutex_lock(&providerLock); providerWaiting = true; pthread_cond_broadcast(&providerCondition);
    while (blockProvider) pthread_cond_wait(&providerCondition, &providerLock);
    NSData *data = fixture; bool okay = data && data.length <= capacity; if (okay) { memcpy(buffer, data.bytes, data.length); *size = data.length; } pthread_mutex_unlock(&providerLock); return okay;
}
static NSString *b64(NSData *data) { NSString *s = [[data base64EncodedStringWithOptions:0] stringByReplacingOccurrencesOfString:@"+" withString:@"-"]; return [[s stringByReplacingOccurrencesOfString:@"/" withString:@"_"] stringByReplacingOccurrencesOfString:@"=" withString:@""]; }
static NSData *unb64(NSString *s) { NSString *n = [[s stringByReplacingOccurrencesOfString:@"-" withString:@"+"] stringByReplacingOccurrencesOfString:@"_" withString:@"/"]; n = [n stringByPaddingToLength:(n.length + 3) & ~(NSUInteger)3 withString:@"=" startingAtIndex:0]; return [[NSData alloc] initWithBase64EncodedString:n options:0]; }
static NSMutableDictionary *valid_record(void) {
    uint64_t expiry = (uint64_t)NSDate.date.timeIntervalSince1970 + 3600; NSString *payload = [NSString stringWithFormat:@"1800000042.synthetic_user.%llu", expiry];
    uint8_t invalidHmac[32] = {0}; NSString *token = [NSString stringWithFormat:@"nx2.%@.%@", b64([payload dataUsingEncoding:NSUTF8StringEncoding]), b64([NSData dataWithBytes:invalidHmac length:32])];
    return [@{@"schema_version":@1, @"token_kind":@"nex", @"token":token, @"identity":@{@"pid":@1800000042ull, @"username":@"synthetic_user", @"friend_code":@"test-only"}, @"scopes":@[@"identity"], @"created_at":@((uint64_t)NSDate.date.timeIntervalSince1970 - 30), @"expires_at":@(expiry)} mutableCopy];
}
static void set_record(NSDictionary *record) { pthread_mutex_lock(&providerLock); fixture = record ? [NSJSONSerialization dataWithJSONObject:record options:0 error:nil] : nil; pthread_mutex_unlock(&providerLock); }
static void identity_tests(void) {
    NSMutableDictionary *record = valid_record(); set_record(record); uint64_t pid = 0; uint8_t available = 0;
    CHECK(dm_nextendo_account_available(&available) == 0 && available && dm_nextendo_account_id(&pid) == 0 && pid == 1800000042ull, "real metadata PID is returned without a fabricated fallback");
    char name[33] = "x", nick[33];
    CHECK(dm_nextendo_account_username(name) == 0 && !strcmp(name, "synthetic_user"), "the username is the signed-in record's");
    unsetenv("DM_NICK"); CHECK(H(uint32_t (*)(char *, const void *), "_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE")(nick, NULL) == 0 && !strcmp(nick, "synthetic_user"), "Nextendo mode: the nickname is the account's username");
    int calls = atomic_load(&providerCalls);
    for (unsigned i = 0; i < 3; i++) { mode = (dm_net_mode_t)i; available = 9; pid = 9; CHECK(dm_nextendo_account_available(&available) == 0 && !available && dm_nextendo_account_id(&pid) != 0 && !pid && dm_nextendo_account_ensure() != 0, "offline/LAN/LDN never load an online credential"); }
    CHECK(atomic_load(&providerCalls) == calls, "disabled modes do not query credentials"); mode = DM_NET_AUTO;
    NSArray *bad = @[@{@"token_kind":@"oauth"}, @{@"schema_version":@1.5}, @{@"schema_version":@YES}, @{@"expires_at":@1}, @{@"expires_at":@1e20}, @{@"created_at":@1e20}, @{@"scopes":@"identity"}, @{@"identity":@{@"pid":@1800000043ull,@"username":@"synthetic_user"}}, @{@"identity":@{@"pid":@1800000042ull,@"username":@"someone_else"}}, @{@"token":@"nx2.invalid.invalid"}, @{@"token":@"oauth-access-token"}, @{@"token":@"nx2. .invalid"}];
    for (NSDictionary *changes in bad) { NSMutableDictionary *wrong = record.mutableCopy; [wrong addEntriesFromDictionary:changes]; set_record(wrong); available = 9; pid = 9; CHECK(dm_nextendo_account_available(&available) == 0 && !available && dm_nextendo_account_id(&pid) != 0 && !pid, "malformed/OAuth/expired/incoherent data never enables NSA"); }
    set_record(nil); CHECK(dm_nextendo_account_ensure() != 0, "missing credential fails honestly"); set_record(record);
}
static uint64_t user[3];
static void token_tests(void) {
    CHECK(H(uint32_t (*)(void *), "_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE")(user) == 0, "actual UserHandle issued by host");
    uint64_t pid = 0; CHECK(H(uint32_t (*)(uint64_t *, const void *), "_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE")(&pid, user) == 0 && pid == 1800000042ull, "SDK4/19 NSA output is exactly one UInt64");
    uint64_t wrong[3] = {user[0] ^ 1, user[1], user[2]}; pid = 9;
    CHECK(H(uint32_t (*)(uint64_t *, const void *), "_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE")(&pid, wrong) != 0 && !pid, "only the linked local user can obtain the online PID");
    dm_nextendo_set_game_version("3.0.5"); uint64_t size = 0; char bytes[8193]; memset(bytes, 0x6a, sizeof bytes);
    typedef uint32_t (*Load)(uint64_t *, char *, uint64_t, const void *);
    Load load = H(Load, "_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE");
    CHECK(load(&size, bytes, 16, user) != 0 && size > 16 && bytes[0] == 0x6a, "short cache buffer reports needed length without overwriting it");
    CHECK(load(&size, bytes, 8192, user) == 0 && size < 3072 && bytes[size] == 0 && bytes[8192] == 0x6a, "token cache is bounded and NUL-terminated; reported size excludes NUL");
    NSString *jwt = [[NSString alloc] initWithBytes:bytes length:(NSUInteger)size encoding:NSASCIIStringEncoding]; NSArray *segments = [jwt componentsSeparatedByString:@"."]; CHECK(segments.count == 3, "compact signed envelope contains three segments");
    if (segments.count != 3) return;
    NSData *headerBytes=unb64(segments[0]), *claimBytes=unb64(segments[1]);
    CHECK(headerBytes && claimBytes,"synthetic envelope JSON segments decode before parsing");
    if (!headerBytes || !claimBytes) return;
    NSDictionary *header = [NSJSONSerialization JSONObjectWithData:headerBytes options:0 error:nil], *claims = [NSJSONSerialization JSONObjectWithData:claimBytes options:0 error:nil];
    NSDictionary *original = [NSJSONSerialization JSONObjectWithData:fixture options:0 error:nil];
    CHECK([header[@"alg"] isEqual:@"RS256"] && [header[@"typ"] isEqual:@"id_token"] && [header[@"kid"] isEqual:@"nextendo-baas-key-1"] && [claims[@"nnex"] isEqual:original[@"token"]] && [claims[@"tv"] isEqual:@"3.0.5"], "Nextendo clients' BaaS header, untouched NEX proof and original game version");
    CHECK([claims[@"iss"] isEqual:@"https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"] && [claims[@"aud"] isEqual:@"ed9e2f05d286f7b8"] && [claims[@"hm"] isEqual:@YES] && [claims[@"sub"] length] == 32 && [claims[@"bs:did"] length] == 32 && !claims[@"country"] && !claims[@"app_id"],
          "the claims Ryujinx-Nextendo and Mush8 send (issuer, audience, membership, random IDs), no country and no app_id for this game");
    CHECK([claims[@"exp"] doubleValue] <= [original[@"expires_at"] doubleValue] && [claims[@"nbf"] isEqual:claims[@"iat"]], "expiry bounded by the proof");
    uint8_t publicKeyBytes[1024]; uint64_t keySize = 0; CHECK(dm_nextendo_copy_public_signing_key(publicKeyBytes, sizeof publicKeyBytes, &keySize) == 0 && keySize, "only the ephemeral public key is exported for verification");
    NSData *keyData = [NSData dataWithBytes:publicKeyBytes length:(NSUInteger)keySize]; SecKeyRef key = SecKeyCreateWithData((__bridge CFDataRef)keyData, (__bridge CFDictionaryRef)@{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeRSA, (__bridge id)kSecAttrKeyClass:(__bridge id)kSecAttrKeyClassPublic}, NULL);
    NSData *signedBytes = [[NSString stringWithFormat:@"%@.%@", segments[0], segments[1]] dataUsingEncoding:NSASCIIStringEncoding];
    CHECK(key && SecKeyVerifySignature(key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, (__bridge CFDataRef)signedBytes, (__bridge CFDataRef)unb64(segments[2]), NULL), "envelope signature verifies with the process's own public key"); if (key) CFRelease(key);
    char second[8192]; uint64_t again = 0; CHECK(load(&again, second, sizeof second, user) == 0 && again == size && !memcmp(second, bytes, (size_t)size), "unchanged proof/version uses the cache without reminting a token");
    dm_nextendo_set_game_version("3.0.6"); CHECK(load(&again, second, sizeof second, user) == 0 && memcmp(second, bytes, (size_t)MIN(again, size)), "actual game-version change invalidates token cache"); dm_nextendo_set_game_version("3.0.5");
    NSMutableDictionary *oauth = valid_record(); oauth[@"token_kind"] = @"oauth"; set_record(oauth); again = 99;
    CHECK(load(&again, second, sizeof second, user) != 0 && !again, "a cached game token cannot survive replacement by an OAuth social session"); set_record(valid_record());
}
typedef struct { void *context; uint32_t result; } Worker;
static void *ensure_worker(void *pointer) { Worker *worker = pointer; worker->result = H(uint32_t (*)(void *, const void *), "_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE")(worker->context, user); return NULL; }
static pthread_t pending(Worker *worker) {
    pthread_mutex_lock(&providerLock); blockProvider = true; providerWaiting = false; pthread_mutex_unlock(&providerLock);
    pthread_t thread; CHECK(pthread_create(&thread, NULL, ensure_worker, worker) == 0, "start real concurrent token request");
    pthread_mutex_lock(&providerLock); while (!providerWaiting) pthread_cond_wait(&providerCondition, &providerLock); pthread_mutex_unlock(&providerLock); return thread;
}
static void release_provider(pthread_t thread) { pthread_mutex_lock(&providerLock); blockProvider = false; pthread_cond_broadcast(&providerCondition); pthread_mutex_unlock(&providerLock); CHECK(pthread_join(thread, NULL) == 0, "credential operation returned"); }
static uint32_t service_operation(const void *value) { return *(const uint32_t *)value; }
// The SDK's own constructor builds an AsyncContext (hle_account.c leaves it alone): it writes guest memory the host never reads, so here it writes nothing either
static void sdk_ctor(void *self) { (void)self; }
static void async_tests(void) {
    typedef void (*Ctor)(void *); typedef uint32_t (*Object)(void *); typedef uint32_t (*Event)(void *, void *);
    Ctor ctor = sdk_ctor, dtor = H(Ctor, "_ZN2nn7account12AsyncContextD1Ev");
    Object result = H(Object, "_ZN2nn7account12AsyncContext9GetResultEv"); Event event = H(Event, "_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE");
    memset(contextA, 0xa5, sizeof contextA); memset(contextB, 0x5a, sizeof contextB); ctor(contextA); TestEvent completion = {0}; uint8_t done = 9;
    CHECK(result(contextA) != 0 && event(contextA, &completion) != 0 && !completion.id, "empty context has no invented completion request");
    Worker work = {contextA, 99}; pthread_t thread = pending(&work);
    CHECK(dm_nextendo_async_done(contextA, &done) == 0 && !done && event(contextA, &completion) == 0 && !completion.signaled, "pending operation exposes an unsignaled real event binding");
    release_provider(thread); CHECK(work.result == 0 && completion.signaled && result(contextA) == 0, "success and completion event publish only after actual token creation");
    typedef uint32_t (*Kind)(void *); Kind kind = H(Kind, "_ZN2nn7account37AsyncNetworkServiceLicenseInfoContext28GetNetworkServiceLicenseKindEv");
    CHECK(kind(contextA) == 1 && kind(contextB) == 0, "membership reads subscribed only for a completed successful request");
    CHECK(H(Object, "_ZN2nn7account12AsyncContext6CancelEv")(contextA) == 0 && result(contextA) == 0, "cancel after completion does not rewrite a completed success");
    CHECK(contextA[0] == (char)0xa5 && contextB[0] == 0x5a, "SDK4/19 private object memory remains untouched"); close_event(&completion); dtor(contextA);
    ctor(contextA); work = (Worker){contextA,99}; thread = pending(&work); CHECK(event(contextA, &completion) == 0, "attach cancel completion event");
    CHECK(H(Object, "_ZN2nn7account12AsyncContext6CancelEv")(contextA) == 0 && completion.signaled && result(contextA) != 0, "cancel publishes failure and signals completion");
    release_provider(thread); CHECK(result(contextA) != 0, "late success cannot overwrite cancellation"); close_event(&completion); dtor(contextA);
    ctor(contextA); ctor(contextB); work = (Worker){contextA,99}; thread = pending(&work); uint64_t lifetime = dm_nextendo_async_lifetime(contextA);
    CHECK(H(void *(*)(void *,void *), "_ZN2nn7account12AsyncContextaSEOS1_")(contextB,contextA) == contextB && !dm_nextendo_async_valid(contextA) && dm_nextendo_async_lifetime(contextB) == lifetime, "move preserves pending state and empties its source");
    CHECK(event(contextB, &completion) == 0, "attach event after moving a pending request"); release_provider(thread);
    CHECK(completion.signaled && H(Object, "_ZNK2nn7account12AsyncContext9GetResultEv")(contextB) == 0, "SDK19 const result observes completion after move"); close_event(&completion); dtor(contextA); dtor(contextB);
    ctor(contextA); CHECK(dm_nextendo_async_ensure(contextA) == 0 && result(contextA) == 0, "initial request before output-context reuse"); lifetime = dm_nextendo_async_lifetime(contextA);
    CHECK(dm_nextendo_async_ensure(contextA) == 0 && result(contextA) == 0 && dm_nextendo_async_lifetime(contextA) != lifetime, "completed output context can receive a fresh request lifetime");
    ctor(contextB); H(void (*)(void *, void *), "_ZN2nn7account12AsyncContext4SwapERS1_")(contextA, contextB);
    CHECK(result(contextA) != 0 && result(contextB) == 0, "swap exchanges completed state and an empty context"); dtor(contextA); dtor(contextB);
    NSMutableDictionary *oauth = valid_record(); oauth[@"token_kind"] = @"oauth"; set_record(oauth); ctor(contextA);
    CHECK(dm_nextendo_async_ensure(contextA) == 0 && result(contextA) != 0 && dm_nextendo_async_done(contextA, &done) == 0 && done, "queued OAuth-only cache request completes with failure, never game authentication"); dtor(contextA); set_record(valid_record());
    ctor(contextA); work = (Worker){contextA,99}; thread = pending(&work); CHECK(event(contextA, &completion) == 0, "attach event before destruction");
    dtor(contextA); close_event(&completion); hle_bind_system_event(&completion, 0); uint64_t fresh = completion.id; ctor(contextA); release_provider(thread);
    CHECK(completion.id == fresh && !completion.signaled && result(contextA) != 0, "late work cannot signal a reused event or publish into a reconstructed context"); close_event(&completion); dtor(contextA);
    void *heap_context = malloc(64); ctor(heap_context); H(Ctor, "_ZN2nn7account12AsyncContextD0Ev")(heap_context);
    CHECK(deletes == 1 && !dm_nextendo_async_valid(heap_context), "deleting destructor releases host state and calls the guest's allocator exactly once");
    ctor(contextA); uint32_t service_result=0x1234;
    CHECK(dm_nextendo_async_execute(contextA,service_operation,&service_result)==0 && result(contextA)==service_result && !dm_nextendo_async_done(contextA,&done) && done, "service operation preserves its real failure result and completed state");
    service_result=0; CHECK(dm_nextendo_async_execute(contextA,service_operation,&service_result)==0 && !result(contextA), "a later service request has its own real completion");
    mode=DM_NET_LAN; CHECK(dm_nextendo_async_execute(contextA,service_operation,&service_result)!=0, "local modes do not execute a provider service operation"); mode=DM_NET_NEXTENDO; dtor(contextA);
    // A move constructor builds the object, so it writes the SDK's vtable (taken from the source) and a null IPC pointer at +0x18, and moves the request state
    ctor(contextA); CHECK(dm_nextendo_async_ensure(contextA) == 0 && result(contextA) == 0, "request before move construction"); lifetime = dm_nextendo_async_lifetime(contextA);
    memset(contextA, 0x11, 8); memset(contextB, 0x5a, sizeof contextB); H(void (*)(void *, void *), "_ZN2nn7account12AsyncContextC1EOS1_")(contextB, contextA);
    void *ipc; memcpy(&ipc, contextB + 0x18, sizeof ipc);
    CHECK(!memcmp(contextB, contextA, 8) && !ipc && contextB[8] == 0x5a, "move construction writes the vtable and IPC pointer, and nothing else");
    CHECK(dm_nextendo_async_lifetime(contextB) == lifetime && !dm_nextendo_async_valid(contextA) && result(contextB) == 0, "move construction carries the completed request"); dtor(contextA); dtor(contextB);
    // Nextendo off, a context answers as the offline host always has, whatever was constructed
    mode=DM_NET_OFFLINE; ctor(contextA); TestEvent offline = {0}; done = 0;
    CHECK(dm_nextendo_async_ensure(contextA) != 0 && !dm_nextendo_async_valid(contextA), "offline: no request starts");
    CHECK(event(contextA, &offline) == 0 && offline.signaled, "offline: the completion event fires immediately");
    CHECK(H(Event, "_ZN2nn7account12AsyncContext7HasDoneEPb")(contextA, &done) == 0 && done, "offline: the context is done");
    CHECK(result(contextA) != 0 && H(Object, "_ZN2nn7account12AsyncContext6CancelEv")(contextA) == 0, "offline: the result is failure and cancel has nothing to cancel");
    close_event(&offline); dtor(contextA); mode=DM_NET_NEXTENDO;
}
// The Mac sign-in's pieces that need no network: the record it builds from an nx2 game token passes hle_nextendo_apple.m's own checks, and the stored sign-in
// is a private file that a loose or linked one cannot stand in for
static void login_tests(void) {
    NSString *token = valid_record()[@"token"]; NSString *error = nil; NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    NSDictionary *record = nxl_record(token, now, &error);
    CHECK(record && [record[@"identity"][@"pid"] unsignedLongLongValue] == 1800000042ull && [record[@"identity"][@"username"] isEqual:@"synthetic_user"], "the sign-in's record takes its identity from the token's payload");
    set_record(record); uint8_t available = 0; uint64_t pid = 0;
    CHECK(dm_nextendo_account_available(&available) == 0 && available && dm_nextendo_account_id(&pid) == 0 && pid == 1800000042ull, "the sign-in's record passes the account decoder");
    NSString *payload = [NSString stringWithFormat:@"1800000042.synthetic_user.%llu", (unsigned long long)now - 5]; uint8_t mac[32] = {0};
    NSString *stale = [NSString stringWithFormat:@"nx2.%@.%@", b64([payload dataUsingEncoding:NSUTF8StringEncoding]), b64([NSData dataWithBytes:mac length:32])];
    CHECK(!nxl_record(stale, now, &error) && error.length, "an expired game token gives no record");
    CHECK(!nxl_record(@"oauth.abc.def", now, &error) && !nxl_record(@"nx2.bm9uZQ.AA", now, &error), "a token that is not an nx2 one, or names no identity, gives no record");
    char dir[] = "/tmp/t_nextendo_login.XXXXXX"; CHECK(mkdtemp(dir) != NULL, "temporary sign-in folder");
    setenv("DM_NEXTENDO_LOGIN_DIR", dir, 1);
    CHECK(!nxl_read_refresh(), "no stored sign-in at first");
    CHECK(nxl_write_refresh(@"synthetic-refresh") && [nxl_read_refresh() isEqual:@"synthetic-refresh"], "the stored sign-in reads back");
    struct stat st; CHECK(!stat(nxl_refresh_path().fileSystemRepresentation, &st) && (st.st_mode & 0777) == 0600, "the stored sign-in is readable by its owner only");
    chmod(nxl_refresh_path().fileSystemRepresentation, 0644); CHECK(!nxl_read_refresh(), "a stored sign-in others can read is refused");
    NSString *link = [[NSString stringWithUTF8String:dir] stringByAppendingPathComponent:@"elsewhere"];
    CHECK(nxl_write_refresh(@"synthetic-refresh") && !rename(nxl_refresh_path().fileSystemRepresentation, link.fileSystemRepresentation) && !symlink(link.fileSystemRepresentation, nxl_refresh_path().fileSystemRepresentation) && !nxl_read_refresh(), "a linked stored sign-in is refused");
    unlink(nxl_refresh_path().fileSystemRepresentation); unlink(link.fileSystemRepresentation);
    CHECK(nxl_write_refresh(@"synthetic-refresh") && nxl_write_refresh(nil) && !nxl_read_refresh(), "sign-out removes the stored sign-in");
    unsetenv("DM_NEXTENDO_LOGIN_DIR"); rmdir(dir); set_record(valid_record());
}
int main(void) { @autoreleasepool {
    // No configured provider is a normal unavailable result, not an OS storage
    // read or a spawned helper. Offline never invokes even an installed provider.
    uint8_t absent=7; dm_nextendo_set_credential_provider(NULL);
    CHECK(!dm_nextendo_account_available(&absent) && !absent && dm_nextendo_account_ensure()!=0 && !atomic_load(&providerCalls), "unconfigured backend never queries an implicit credential source");
    dm_nextendo_set_credential_provider(provider); unsetenv("DM_NEXTENDO_ROUTES"); mode=DM_NET_AUTO;
    CHECK(!dm_nextendo_account_available(&absent) && !absent && !atomic_load(&providerCalls), "automatic local mode without a profile never invokes the credential provider");
    setenv("DM_NEXTENDO_ROUTES","/synthetic/public-policy.tsv",1); mode=DM_NET_NEXTENDO;
    dm_nextendo_set_game_version("3.0.5"); identity_tests(); login_tests(); token_tests(); async_tests();
    CHECK(atomic_load(&entered) > 0 && atomic_load(&entered) == atomic_load(&left) && atomic_load(&waits) > 0, "blocking account hooks publish balanced guest contexts and mark host security waits");
    printf("Nextendo account: %d checks, %d failures; synthetic provider only, no user credentials read\n", checks, failures); return failures ? 1 : 0;
} }
