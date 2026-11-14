// Native Nextendo credentials and account requests. This bridge never mints an nx2
// proof: its HMAC is checked by Nextendo, while the host checks identity and expiry.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>
#include "nextendo_account.h"
#import "nextendo_api_apple.h"
#include "net_socket.h"
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>

void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);
int hle_signal_system_event_binding(uint64_t binding);

static _Atomic(dm_nextendo_credential_provider) credentialProvider;
static pthread_mutex_t accountLock = PTHREAD_MUTEX_INITIALIZER;
static NSString *explicitVersion;
static NSData *cachedFingerprint;
static NSString *cachedVersion;
static NSData *cachedToken;
static NSTimeInterval cachedExpiry;
static SecKeyRef signingKey;
static NSData *publicSigningKey;
static pthread_once_t signingOnce = PTHREAD_ONCE_INIT;
static BOOL enabled(void) { return dm_net_nextendo_enabled(); }
@interface DmNextendoCredential : NSObject
@property(nonatomic) uint64_t pid;
@property(nonatomic) NSString *username;
@property(nonatomic) NSString *token;
@property(nonatomic) NSTimeInterval expiry;
@end
@implementation DmNextendoCredential
@end

static NSString *base64url(NSData *data) {
    NSString *value = [[data base64EncodedStringWithOptions:0] stringByReplacingOccurrencesOfString:@"+" withString:@"-"];
    return [[value stringByReplacingOccurrencesOfString:@"/" withString:@"_"] stringByReplacingOccurrencesOfString:@"=" withString:@""];
}
static NSData *decode_base64url(NSString *value) {
    if (![value isKindOfClass:NSString.class] || !value.length) return nil;
    for (NSUInteger i = 0; i < value.length; i++) { unichar c = [value characterAtIndex:i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return nil; }
    NSString *normal = [[value stringByReplacingOccurrencesOfString:@"-" withString:@"+"] stringByReplacingOccurrencesOfString:@"_" withString:@"/"];
    normal = [normal stringByPaddingToLength:(normal.length + 3) & ~(NSUInteger)3 withString:@"=" startingAtIndex:0];
    NSData *data = [[NSData alloc] initWithBase64EncodedString:normal options:0]; return data && [base64url(data) isEqualToString:value] ? data : nil;
}
static BOOL unsigned_decimal(NSString *text, uint64_t *out) {
    if (![text isKindOfClass:NSString.class] || !text.length || text.length > 20) return NO;
    uint64_t value = 0;
    for (NSUInteger i = 0; i < text.length; i++) { unichar c = [text characterAtIndex:i]; if (c < '0' || c > '9' || value > (UINT64_MAX - (c - '0')) / 10) return NO; value = value * 10 + c - '0'; }
    if (!value) return NO; *out = value; return YES;
}
static BOOL number(id value) { return [value isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID(); }
// The embedding host owns local storage/authorization. No fallback reads occur.
static NSData *read_credentials(void) {
    dm_nextendo_credential_provider provider = atomic_load(&credentialProvider);
    if (!provider) return nil;
    NSMutableData *data = [NSMutableData dataWithLength:65536]; size_t size = 0;
    if (!data || !provider(data.mutableBytes, data.length, &size) || !size || size > data.length) return nil;
    data.length = size; return data;
}
static DmNextendoCredential *decode_credential(NSData *data) {
    if (!data.length || data.length > 65536) return nil;
    id decoded = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil]; if (![decoded isKindOfClass:NSDictionary.class]) return nil; NSDictionary *json = decoded;
    if (!number(json[@"schema_version"]) || [json[@"schema_version"] doubleValue] != 1 || ![json[@"token_kind"] isEqual:@"nex"]) return nil;
    NSString *token = json[@"token"]; if (![token isKindOfClass:NSString.class] || !token.length || [token lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 16384) return nil;
    for (NSUInteger i = 0; i < token.length; i++) { unichar c = [token characterAtIndex:i]; if (c <= 32 || c >= 127) return nil; }
    NSArray *parts = [token componentsSeparatedByString:@"."]; if (parts.count != 3 || ![parts[0] isEqual:@"nx2"]) return nil;
    NSData *payload = decode_base64url(parts[1]), *signature = decode_base64url(parts[2]); if (!payload || signature.length != 32) return nil;
    NSString *text = [[NSString alloc] initWithData:payload encoding:NSUTF8StringEncoding]; NSArray *claims = [text componentsSeparatedByString:@"."]; if (claims.count < 3) return nil;
    uint64_t pid, expiry; if (!unsigned_decimal(claims.firstObject, &pid) || !unsigned_decimal(claims.lastObject, &expiry) || expiry > INT64_MAX) return nil;
    NSString *username = [[claims subarrayWithRange:NSMakeRange(1, claims.count - 2)] componentsJoinedByString:@"."]; if (!username.length || username.length > 128) return nil;
    id identity = json[@"identity"]; if (![identity isKindOfClass:NSDictionary.class] || !number(identity[@"pid"])) return nil;
    uint64_t storedPid; if (!unsigned_decimal([identity[@"pid"] stringValue], &storedPid) || storedPid != pid || ![identity[@"username"] isEqual:username]) return nil;
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    if (!number(json[@"created_at"]) || !number(json[@"expires_at"])) return nil;
    double created = [json[@"created_at"] doubleValue], until = [json[@"expires_at"] doubleValue];
    if (!isfinite(created) || !isfinite(until) || created <= 0 || created > now + 60 || until <= now || until <= created || until > (double)expiry) return nil;
    id scopes = json[@"scopes"]; if (![scopes isKindOfClass:NSArray.class] || [scopes count] > 128) return nil; for (id scope in scopes) if (![scope isKindOfClass:NSString.class] || [scope length] > 256) return nil;
    DmNextendoCredential *c = [DmNextendoCredential new]; c.pid = pid; c.username = username; c.token = token; c.expiry = until; return c;
}
static DmNextendoCredential *credential(void) {
    return enabled() ? decode_credential(read_credentials()) : nil;
}
void dm_nextendo_set_credential_provider(dm_nextendo_credential_provider provider) {
    atomic_store(&credentialProvider, provider);
    pthread_mutex_lock(&accountLock); cachedFingerprint = nil; cachedToken = nil; cachedVersion = nil; cachedExpiry = 0; pthread_mutex_unlock(&accountLock);
}
void dm_nextendo_set_game_version(const char *version) {
    NSString *value = version ? [NSString stringWithUTF8String:version] : nil;
    pthread_mutex_lock(&accountLock); explicitVersion = value; cachedToken = nil; cachedVersion = nil; pthread_mutex_unlock(&accountLock);
}
uint32_t dm_nextendo_account_available(uint8_t *out) { @autoreleasepool {
    if (!out) return DM_ACCOUNT_FAIL; *out = 0;
    if (enabled()) *out = credential() != nil;
    return 0;
} }
uint32_t dm_nextendo_account_id(uint64_t *out) { @autoreleasepool {
    if (!out) return DM_ACCOUNT_FAIL; *out = 0; DmNextendoCredential *c = credential(); if (!c) return DM_ACCOUNT_FAIL;
    if (dm_nextendo_api_baas_namespace("account")) return dm_nextendo_api_baas_id(out);
    *out = c.pid; return 0;
} }
uint32_t dm_nextendo_account_ensure(void) { @autoreleasepool { return credential() ? 0 : DM_ACCOUNT_FAIL; } }
uint32_t dm_nextendo_account_username(char out[33]) { @autoreleasepool {
    if (!out) return DM_ACCOUNT_FAIL; memset(out, 0, 33); DmNextendoCredential *c = credential(); if (!c) return DM_ACCOUNT_FAIL;
    const char *name = c.username.UTF8String; size_t n = name ? strlen(name) : 0;
    if (n > 32) { n = 32; while (n && ((unsigned char)name[n] & 0xc0) == 0x80) n--; }   // whole UTF-8 characters only
    if (!n) return DM_ACCOUNT_FAIL; memcpy(out, name, n); return 0;
} }
// Host-only API authorization. Never serialized into a game bundle or HLE output. Nextendo's /api/me takes only a session or OAuth access
// token (friends and presence take the nx2 game token too), so the sign-in's access token goes first and the nx2 token serves without one.
// hle_nextendo_login_apple.m has the strong dm_nextendo_login_access; a host without it has no access token
__attribute__((weak)) NSString *dm_nextendo_login_access(void) { return nil; }
NSString *dm_nextendo_api_bearer(void) {
    DmNextendoCredential *c = credential(); if (!c) return nil;
    NSString *access = dm_nextendo_login_access(); return access.length ? access : c.token;
}
NSData *dm_nextendo_api_identity(void) {
    DmNextendoCredential *c = credential(); if (!c) return nil;
    NSData *bytes = [c.token dataUsingEncoding:NSUTF8StringEncoding]; uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest); return [NSData dataWithBytes:digest length:sizeof digest];
}

// Token construction is deliberately separate from credential validation. The nx2
// proof is never substituted with a local signature or an OAuth access token.
static void initialize_signing_key(void) {
    NSDictionary *attributes = @{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeRSA,
        (__bridge id)kSecAttrKeySizeInBits:@2048, (__bridge id)kSecPrivateKeyAttrs:@{(__bridge id)kSecAttrIsPermanent:@NO}};
    CFErrorRef error = NULL; dm_guest_host_wait_begin(); signingKey = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes, &error); dm_guest_host_wait_end(); if (error) CFRelease(error);
    if (!signingKey) return; SecKeyRef publicKey = SecKeyCopyPublicKey(signingKey);
    if (publicKey) { publicSigningKey = CFBridgingRelease(SecKeyCopyExternalRepresentation(publicKey, NULL)); CFRelease(publicKey); }
}
uint32_t dm_nextendo_copy_public_signing_key(void *buffer, uint64_t capacity, uint64_t *size) { @autoreleasepool {
    if (!size) return DM_ACCOUNT_FAIL; *size = 0; pthread_once(&signingOnce, initialize_signing_key);
    if (!publicSigningKey) return DM_ACCOUNT_FAIL; *size = publicSigningKey.length; if (!buffer || capacity < publicSigningKey.length) return DM_ACCOUNT_FAIL;
    memcpy(buffer, publicSigningKey.bytes, publicSigningKey.length); return 0;
} }
static NSString *tokenSub, *tokenDevice, *tokenDeviceAccount;   // under accountLock
static NSString *random_hex(size_t count) {
    uint8_t bytes[32]; if (count > sizeof bytes || SecRandomCopyBytes(kSecRandomDefault, count, bytes) != errSecSuccess) return nil;
    NSMutableString *hex = [NSMutableString stringWithCapacity:count * 2]; for (size_t i = 0; i < count; i++) [hex appendFormat:@"%02x", bytes[i]]; return hex;
}
static NSData *id_token(DmNextendoCredential *c) {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970; NSData *proof = [c.token dataUsingEncoding:NSASCIIStringEncoding]; unsigned char hash[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(proof.bytes, (CC_LONG)proof.length, hash);
    NSData *fingerprint = [NSData dataWithBytes:hash length:sizeof hash]; pthread_mutex_lock(&accountLock);
    NSString *version = explicitVersion ?: [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    if (![version isKindOfClass:NSString.class] || !version.length || version.length > 64 || ![version canBeConvertedToEncoding:NSASCIIStringEncoding]) { pthread_mutex_unlock(&accountLock); return nil; }
    if (cachedToken && [cachedFingerprint isEqual:fingerprint] && [cachedVersion isEqual:version] && cachedExpiry > now && cachedExpiry <= c.expiry) { NSData *token = cachedToken; pthread_mutex_unlock(&accountLock); return token; }
    pthread_once(&signingOnce, initialize_signing_key); if (!signingKey || !publicSigningKey) { pthread_mutex_unlock(&accountLock); return nil; }
    uint64_t expiry = (uint64_t)floor(MIN(c.expiry, now + 3 * 3600)); if ((double)expiry <= now) { pthread_mutex_unlock(&accountLock); return nil; }
    // The BaaS id_token shape Nextendo's own clients send (Ryujinx-Nextendo ManagerServer.GenerateIdToken, Mush8 nextendo.rs id_token): the
    // header's kid and typ, Nintendo BaaS's issuer, audience and certificate URL, random user / device / device-account IDs (fixed for the
    // process) and "hm", the online-membership claim (Ryujinx-Nextendo: Splatoon 2 reads it locally before it goes online). The server authenticates the nnex proof, not
    // this signature (Mush8 sends random bytes there); Wonder's token has no app_id (Ryujinx sends one only for the NSO Classics, Scarlet and MH Rise JP/HK)
    if (!tokenSub) { tokenSub = random_hex(16); tokenDevice = random_hex(16); tokenDeviceAccount = random_hex(16); }
    if (!tokenSub || !tokenDevice || !tokenDeviceAccount) { tokenSub = nil; pthread_mutex_unlock(&accountLock); return nil; }
    NSString *baas = @"https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"; NSNumber *issued = @((uint64_t)floor(now));
    NSDictionary *header = @{@"alg":@"RS256", @"typ":@"id_token", @"kid":@"nextendo-baas-key-1"};
    NSDictionary *claims = @{@"sub":tokenSub, @"aud":@"ed9e2f05d286f7b8", @"iss":baas, @"iat":issued, @"nbf":issued, @"exp":@(expiry),
        @"jku":[baas stringByAppendingString:@"/1.0.0/certificates"], @"jti":NSUUID.UUID.UUIDString.lowercaseString, @"di":tokenDevice, @"sn":@"XAW10000000000",
        @"bs:did":tokenDeviceAccount, @"hm":@YES, @"nnex":c.token, @"tv":version};
    NSJSONWritingOptions json = NSJSONWritingSortedKeys | NSJSONWritingWithoutEscapingSlashes;
    NSData *headerData = [NSJSONSerialization dataWithJSONObject:header options:json error:nil], *claimData = [NSJSONSerialization dataWithJSONObject:claims options:json error:nil];
    if (!headerData || !claimData) { pthread_mutex_unlock(&accountLock); return nil; }
    NSString *message = [NSString stringWithFormat:@"%@.%@", base64url(headerData), base64url(claimData)]; NSData *bytes = [message dataUsingEncoding:NSASCIIStringEncoding];
    CFErrorRef error = NULL; dm_guest_host_wait_begin(); NSData *signature = CFBridgingRelease(SecKeyCreateSignature(signingKey, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, (__bridge CFDataRef)bytes, &error)); dm_guest_host_wait_end(); if (error) CFRelease(error);
    if (!signature) { pthread_mutex_unlock(&accountLock); return nil; }
    NSData *token = [[NSString stringWithFormat:@"%@.%@", message, base64url(signature)] dataUsingEncoding:NSASCIIStringEncoding];
    cachedFingerprint = fingerprint; cachedVersion = version; cachedToken = token; cachedExpiry = (NSTimeInterval)expiry; pthread_mutex_unlock(&accountLock); return token;
}
uint32_t dm_nextendo_account_load_token(uint64_t *size, char *buffer, uint64_t capacity) { @autoreleasepool {
    if (!size) return DM_ACCOUNT_FAIL; *size = 0; if (!buffer && capacity) return DM_ACCOUNT_FAIL;
    DmNextendoCredential *c = credential(); if (!c) return DM_ACCOUNT_FAIL; NSData *token = id_token(c); if (!token) return DM_ACCOUNT_FAIL;
    *size = token.length; if (!buffer || capacity <= token.length) return DM_ACCOUNT_FAIL; memcpy(buffer, token.bytes, token.length); buffer[token.length] = 0; return 0;
} }

@interface DmNextendoAsync : NSObject
@property(nonatomic) NSLock *lock;
@property(nonatomic) uint64_t lifetime, generation;
@property(nonatomic) BOOL started, done, destroyed;
@property(nonatomic) uint32_t result;
@property(nonatomic) NSMutableArray<NSNumber *> *events;
@end
@implementation DmNextendoAsync
@end
static NSMutableDictionary<NSValue *, DmNextendoAsync *> *requests;
static pthread_once_t requestsOnce = PTHREAD_ONCE_INIT;
static pthread_mutex_t requestsLock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t nextLifetime;
static void requests_init(void) { requests = [NSMutableDictionary new]; }
static NSValue *request_key(void *object) { return [NSValue valueWithPointer:object]; }
static DmNextendoAsync *request(void *object) { pthread_once(&requestsOnce, requests_init); pthread_mutex_lock(&requestsLock); DmNextendoAsync *state = requests[request_key(object)]; pthread_mutex_unlock(&requestsLock); return state; }
static DmNextendoAsync *new_request(void) { DmNextendoAsync *state = [DmNextendoAsync new]; state.lock = [NSLock new]; state.events = [NSMutableArray new]; state.result = DM_ACCOUNT_FAIL; return state; }
static void finish_events(NSArray<NSNumber *> *events) { for (NSNumber *binding in events) hle_signal_system_event_binding(binding.unsignedLongLongValue); }
static void destroy_request(DmNextendoAsync *state) { if (!state) return; [state.lock lock]; state.destroyed = YES; state.generation++; state.done = YES; state.result = DM_ACCOUNT_FAIL; NSArray *events = state.events.copy; [state.lock unlock]; finish_events(events); }
void dm_nextendo_async_construct(void *object) { @autoreleasepool {
    if (!object) return; DmNextendoAsync *state = new_request(); if (!state) return; pthread_once(&requestsOnce, requests_init); pthread_mutex_lock(&requestsLock);
    DmNextendoAsync *old = requests[request_key(object)]; if (requests.count < 512 || old) { state.lifetime = ++nextLifetime; requests[request_key(object)] = state; } pthread_mutex_unlock(&requestsLock); destroy_request(old);
} }
void dm_nextendo_async_destroy(void *object) { @autoreleasepool { if (!object) return; pthread_once(&requestsOnce, requests_init); pthread_mutex_lock(&requestsLock); DmNextendoAsync *state = requests[request_key(object)]; [requests removeObjectForKey:request_key(object)]; pthread_mutex_unlock(&requestsLock); destroy_request(state); } }
void *dm_nextendo_async_move(void *object, void *source) { @autoreleasepool {
    if (!object || !source || object == source) return object; pthread_once(&requestsOnce, requests_init); pthread_mutex_lock(&requestsLock);
    DmNextendoAsync *old = requests[request_key(object)], *state = requests[request_key(source)]; [requests removeObjectForKey:request_key(source)]; requests[request_key(object)] = state; pthread_mutex_unlock(&requestsLock); destroy_request(old); return object;
} }
void dm_nextendo_async_swap(void *object, void *other) { @autoreleasepool {
    if (!object || !other || object == other) return; pthread_once(&requestsOnce, requests_init); pthread_mutex_lock(&requestsLock); DmNextendoAsync *first = requests[request_key(object)], *second = requests[request_key(other)]; requests[request_key(object)] = second; requests[request_key(other)] = first; pthread_mutex_unlock(&requestsLock);
} }
bool dm_nextendo_async_valid(void *object) { @autoreleasepool { DmNextendoAsync *state = request(object); if (!state) return false; [state.lock lock]; bool valid = state.started && !state.destroyed; [state.lock unlock]; return valid; } }
uint64_t dm_nextendo_async_lifetime(void *object) { @autoreleasepool { DmNextendoAsync *state = request(object); if (!state) return 0; [state.lock lock]; uint64_t id = state.started && !state.destroyed ? state.lifetime : 0; [state.lock unlock]; return id; } }
uint32_t dm_nextendo_async_attach(void *object, uint64_t lifetime, uint64_t binding) { @autoreleasepool {
    if (!object || !lifetime || !binding) return DM_ACCOUNT_FAIL; DmNextendoAsync *state = request(object); if (!state) return DM_ACCOUNT_FAIL; [state.lock lock];
    if (state.lifetime != lifetime || state.destroyed || !state.started || state.events.count >= 64) { [state.lock unlock]; return DM_ACCOUNT_FAIL; }
    if (![state.events containsObject:@(binding)]) [state.events addObject:@(binding)]; BOOL done = state.done; [state.lock unlock]; if (done) hle_signal_system_event_binding(binding); return 0;
} }
uint32_t dm_nextendo_async_execute(void *object, dm_nextendo_async_operation operation, const void *context) { @autoreleasepool {
    if (!object || !operation || !enabled()) return DM_ACCOUNT_FAIL;
    DmNextendoAsync *state = request(object); if (!state) { dm_nextendo_async_construct(object); state = request(object); } if (!state) return DM_ACCOUNT_FAIL;
    [state.lock lock]; BOOL replacing = state.started && state.done && !state.destroyed; [state.lock unlock];
    if (replacing) {
        DmNextendoAsync *fresh = new_request(); if (!fresh) return DM_ACCOUNT_FAIL;
        pthread_mutex_lock(&requestsLock); if (requests[request_key(object)] != state) { pthread_mutex_unlock(&requestsLock); return DM_ACCOUNT_FAIL; }
        fresh.lifetime = ++nextLifetime; requests[request_key(object)] = fresh; pthread_mutex_unlock(&requestsLock); destroy_request(state); state = fresh;
    }
    [state.lock lock]; if (state.started || state.destroyed) { [state.lock unlock]; return DM_ACCOUNT_FAIL; } state.started = YES; uint64_t generation = ++state.generation; [state.lock unlock];
    uint32_t result = operation(context);
    [state.lock lock]; NSArray *events = nil; if (!state.destroyed && state.generation == generation) { state.result = result; state.done = YES; events = state.events.copy; } [state.lock unlock]; finish_events(events);
    return enabled() ? 0 : DM_ACCOUNT_FAIL;
} }
static uint32_t ensure_token_operation(const void *context) {
    (void)context; DmNextendoCredential *c = credential(); return c && id_token(c) ? 0 : DM_ACCOUNT_FAIL;
}
uint32_t dm_nextendo_async_ensure(void *object) { return dm_nextendo_async_execute(object, ensure_token_operation, NULL); }
uint32_t dm_nextendo_async_cancel(void *object) { @autoreleasepool {
    DmNextendoAsync *state = request(object); if (!state) return DM_ACCOUNT_FAIL; [state.lock lock]; if (!state.started || state.destroyed) { [state.lock unlock]; return DM_ACCOUNT_FAIL; }
    NSArray *events = nil; if (!state.done) { state.generation++; state.done = YES; state.result = DM_ACCOUNT_FAIL; events = state.events.copy; } [state.lock unlock]; finish_events(events); return 0;
} }
uint32_t dm_nextendo_async_done(void *object, uint8_t *out) { @autoreleasepool {
    if (!out) return DM_ACCOUNT_FAIL; *out = 0; DmNextendoAsync *state = request(object); if (!state) return DM_ACCOUNT_FAIL; [state.lock lock]; uint32_t result = !state.started || state.destroyed ? DM_ACCOUNT_FAIL : 0; if (!result) *out = state.done; [state.lock unlock]; return result;
} }
uint32_t dm_nextendo_async_result(void *object) { @autoreleasepool {
    DmNextendoAsync *state = request(object); if (!state) return DM_ACCOUNT_FAIL; [state.lock lock]; uint32_t result = state.started && state.done && !state.destroyed ? state.result : DM_ACCOUNT_FAIL; [state.lock unlock]; return result;
} }
