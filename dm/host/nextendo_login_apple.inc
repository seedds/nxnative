// SPDX-License-Identifier: MIT
// Nextendo sign-in on the Mac and the iPhone. Users: the host's credential provider (hle_nextendo_login_apple.m), the Mac tool
// (dm/tools/nextendo_login.m) and the iPhone app (dm/ios/Sources/NextendoSignIn.m). Written from Nextendo's public OAuth interface:
//   - sign-in: OAuth 2 authorization code with PKCE (S256) at /oauth/authorize, redirected to a loopback address on this device;
//   - /api/oauth/token trades the code, later the stored refresh token, for an access token, and sends a new refresh token each time;
//   - /api/nex-token, with that access token, gives the game token: "nx2." base64url("<pid>.<username>.<expiry seconds>") "." signature.
// Only the refresh token is kept, in a private file (0600 in a 0700 folder) rather than the Keychain, which would ask again after every
// rebuild of dmhost; on the iPhone the folder is in the app's container. Tokens never go to the log: errors carry Nextendo's error code
// or the HTTP status.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Not every user of this file needs every definition, hence the unused attributes.
static NSString *const kNxScopes __attribute__((unused)) = @"identity friends game.matchmaking presence";
static NSString *const kNxAuthorize __attribute__((unused)) = @"https://nextendo.network/oauth/authorize";
static NSString *const kNxTokenEndpoint __attribute__((unused)) = @"https://nextendo.network/api/oauth/token";
static NSString *const kNxGameTokenEndpoint __attribute__((unused)) = @"https://nextendo.network/api/nex-token";
static NSString *const kNxOnlineStatusEndpoint __attribute__((unused)) = @"https://nextendo.network/api/online-status";
static NSString *const kNxUserInfoEndpoint __attribute__((unused)) = @"https://nextendo.network/api/oauth/userinfo";
// The redirect is http://127.0.0.1:<port>/callback. That client was registered with this port, so it is tried first; Nextendo also takes
// other loopback ports, which are used when it is busy.
static const uint16_t kNxCallbackPort __attribute__((unused)) = 47823;
static const NSTimeInterval kNxTimeout __attribute__((unused)) = 12;
enum { kNxRefreshMax = 8192 };

__attribute__((unused)) static void nxl_fail(NSString **error, NSString *text) { if (error) *error = text; }

// The OAuth client this build signs in as: a "public application (PKCE)" its builder registered at nextendo.network/developers, whose
// name Nextendo's sign-in page shows. DM_NEXTENDO_CLIENT_ID in the environment (the Mac), else DMNextendoClientID in the app's Info.plist
// (build_ipa.sh writes it from DM_NEXTENDO_CLIENT_ID at build time). nil when the build has none: then nobody can sign in.
__attribute__((unused)) static NSString *nxl_client_id(void) {
    const char *env = getenv("DM_NEXTENDO_CLIENT_ID");
    id value = env && *env ? @(env) : [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMNextendoClientID"];
    if (![value isKindOfClass:NSString.class] || [value length] == 0 || [value length] > 128) return nil;
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_."];
    return [value rangeOfCharacterFromSet:allowed.invertedSet].location == NSNotFound ? value : nil;
}
static NSString *const kNxNoClient __attribute__((unused)) =
    @"this build has no Nextendo client ID: register a public application at nextendo.network/developers and build with DM_NEXTENDO_CLIENT_ID set to its ID";

// ---- The stored refresh token ----

__attribute__((unused)) static NSString *nxl_dir(void) {
    const char *dir = getenv("DM_NEXTENDO_LOGIN_DIR");   // tests point it at a scratch folder
    return dir && *dir ? @(dir) : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/nxnative/nextendo"];
}
__attribute__((unused)) static NSString *nxl_refresh_path(void) { return [nxl_dir() stringByAppendingPathComponent:@"refresh_token"]; }

// nil unless the file is a regular file of this user's that no one else can read, and not a link
__attribute__((unused)) static NSString *nxl_read_refresh(void) {
    int fd = open(nxl_refresh_path().fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return nil;
    struct stat info;
    bool private_file = !fstat(fd, &info) && S_ISREG(info.st_mode) && !(info.st_mode & (S_IRWXG | S_IRWXO)) && info.st_uid == geteuid()
                        && info.st_size > 0 && info.st_size <= kNxRefreshMax;
    char text[kNxRefreshMax + 1]; ssize_t length = 0;
    if (private_file) {
        for (ssize_t n; length < info.st_size && (n = read(fd, text + length, (size_t)(info.st_size - length))) > 0;) length += n;
        private_file = length == info.st_size;
    }
    close(fd);
    if (!private_file) return nil;
    NSString *token = [[NSString alloc] initWithBytes:text length:(NSUInteger)length encoding:NSUTF8StringEncoding];
    token = [token stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return token.length ? token : nil;
}

// Saves the token through a temporary file and a rename, so a reader sees the old one or the new one; nil deletes it
__attribute__((unused)) static BOOL nxl_write_refresh(NSString *token) {
    const char *path = nxl_refresh_path().fileSystemRepresentation;
    if (!token) return unlink(path) == 0 || errno == ENOENT;
    NSString *dir = nxl_dir();
    NSError *problem = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:&problem]
        || chmod(dir.fileSystemRepresentation, 0700) != 0) return NO;
    NSString *staging = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@".refresh_token.%d", getpid()]];
    unlink(staging.fileSystemRepresentation);
    int fd = open(staging.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return NO;
    const char *bytes = token.UTF8String; size_t size = strlen(bytes), written = 0;
    for (ssize_t n; written < size && (n = write(fd, bytes + written, size - written)) > 0;) written += (size_t)n;
    bool saved = written == size && fsync(fd) == 0;
    saved = close(fd) == 0 && saved && rename(staging.fileSystemRepresentation, path) == 0;
    if (!saved) unlink(staging.fileSystemRepresentation);
    return saved;
}

// ---- Encoding ----

__attribute__((unused)) static NSString *nxl_b64url(NSData *data) {
    NSMutableString *text = [[data base64EncodedStringWithOptions:0] mutableCopy];
    [text replaceOccurrencesOfString:@"+" withString:@"-" options:0 range:NSMakeRange(0, text.length)];
    [text replaceOccurrencesOfString:@"/" withString:@"_" options:0 range:NSMakeRange(0, text.length)];
    [text replaceOccurrencesOfString:@"=" withString:@"" options:0 range:NSMakeRange(0, text.length)];
    return text;
}
__attribute__((unused)) static NSData *nxl_unb64url(NSString *text) {
    NSMutableString *padded = [text mutableCopy];
    [padded replaceOccurrencesOfString:@"-" withString:@"+" options:0 range:NSMakeRange(0, padded.length)];
    [padded replaceOccurrencesOfString:@"_" withString:@"/" options:0 range:NSMakeRange(0, padded.length)];
    while (padded.length % 4) [padded appendString:@"="];
    return [[NSData alloc] initWithBase64EncodedString:padded options:0];
}
// application/x-www-form-urlencoded, with only RFC 3986's unreserved characters left as they are
__attribute__((unused)) static NSData *nxl_form_body(NSDictionary<NSString *, NSString *> *fields) {
    NSCharacterSet *unreserved = [NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"];
    NSMutableArray<NSString *> *pairs = [NSMutableArray arrayWithCapacity:fields.count];
    [fields enumerateKeysAndObjectsUsingBlock:^(NSString *name, NSString *value, BOOL *stop) {
        (void)stop; [pairs addObject:[NSString stringWithFormat:@"%@=%@", name, [value stringByAddingPercentEncodingWithAllowedCharacters:unreserved]]];
    }];
    return [[pairs componentsJoinedByString:@"&"] dataUsingEncoding:NSUTF8StringEncoding];
}

// ---- Nextendo's HTTPS endpoints ----

// Sends one request and waits for it (the request's timeout bounds the wait). The JSON object of a 200 answer; otherwise nil, with
// Nextendo's "error" code, the HTTP status or the transport's failure in *error
__attribute__((unused)) static NSDictionary *nxl_send(NSURLRequest *request, NSString **error) {
    __block NSData *body; __block NSHTTPURLResponse *response; __block NSError *transport;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    NSURLSession *session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    [[session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *answer, NSError *failure) {
        body = data; transport = failure; response = [answer isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)answer : nil;
        dispatch_semaphore_signal(finished);
    }] resume];
    dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];
    id parsed = body.length ? [NSJSONSerialization JSONObjectWithData:body options:0 error:nil] : nil;
    NSDictionary *json = [parsed isKindOfClass:NSDictionary.class] ? parsed : nil;
    if (response.statusCode == 200) {
        if (!json) nxl_fail(error, @"the answer from nextendo.network was not a JSON object");
        return json;
    }
    id code = json[@"error"];
    if ([code isKindOfClass:NSString.class]) nxl_fail(error, code);
    else if (!response) nxl_fail(error, [NSString stringWithFormat:@"no answer from Nextendo (%@)", transport.localizedDescription ?: @"no response"]);
    else nxl_fail(error, [NSString stringWithFormat:@"Nextendo answered HTTP %ld", (long)response.statusCode]);
    return nil;
}

// POSTs a grant to the token endpoint. Returns the access token and saves the refresh token that comes with it. A refused refresh
// token (invalid_grant) is deleted, since it will never work again
__attribute__((unused)) static NSString *nxl_token(NSDictionary<NSString *, NSString *> *grant, NSString **error) {
    NSMutableURLRequest *post = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:kNxTokenEndpoint]
                                                        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:kNxTimeout];
    post.HTTPMethod = @"POST";
    post.HTTPBody = nxl_form_body(grant);
    [post setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
    NSString *why = nil;
    NSDictionary *answer = nxl_send(post, &why);
    if (!answer) {
        bool refreshing = [grant[@"grant_type"] isEqual:@"refresh_token"];
        if (refreshing && [why isEqual:@"invalid_grant"]) { nxl_write_refresh(nil); why = @"the saved sign-in is no longer accepted: sign in to Nextendo again"; }
        nxl_fail(error, why);
        return nil;
    }
    NSString *access = answer[@"access_token"], *refresh = answer[@"refresh_token"];
    if (![access isKindOfClass:NSString.class] || access.length == 0) { nxl_fail(error, @"the token answer had no access_token"); return nil; }
    if ([refresh isKindOfClass:NSString.class] && refresh.length && !nxl_write_refresh(refresh)) {
        nxl_fail(error, @"the renewed refresh token could not be written");
        return nil;
    }
    return access;
}

// The record hle_nextendo_apple.m's decode_credential accepts for an nx2 game token. Its identity is read from the token itself
// ("<pid>.<username>.<expiry>"; a username may contain dots), so the two can never disagree
__attribute__((unused)) static NSDictionary *nxl_record(NSString *token, NSTimeInterval now, NSString **error) {
    NSArray<NSString *> *segments = [token componentsSeparatedByString:@"."];
    if (segments.count != 3 || ![segments[0] isEqual:@"nx2"]) { nxl_fail(error, @"the game token is not in nx2 form"); return nil; }
    NSData *payload = nxl_unb64url(segments[1]);
    NSString *claims = payload ? [[NSString alloc] initWithData:payload encoding:NSUTF8StringEncoding] : nil;
    NSRange first = [claims rangeOfString:@"."], last = [claims rangeOfString:@"." options:NSBackwardsSearch];
    if (!claims || first.location == NSNotFound || last.location <= first.location + 1) { nxl_fail(error, @"the game token carries no player ID and name"); return nil; }
    NSString *pidText = [claims substringToIndex:first.location], *expiryText = [claims substringFromIndex:last.location + 1];
    NSString *username = [claims substringWithRange:NSMakeRange(first.location + 1, last.location - first.location - 1)];
    NSCharacterSet *notDigit = NSCharacterSet.decimalDigitCharacterSet.invertedSet;
    unsigned long long pid = strtoull(pidText.UTF8String, NULL, 10), expiry = strtoull(expiryText.UTF8String, NULL, 10);
    bool numeric = pidText.length && expiryText.length && pidText.length <= 20 && expiryText.length <= 20
                   && [pidText rangeOfCharacterFromSet:notDigit].location == NSNotFound && [expiryText rangeOfCharacterFromSet:notDigit].location == NSNotFound;
    if (!numeric || pid == 0) { nxl_fail(error, @"the game token carries no player ID and name"); return nil; }
    if ((double)expiry <= now) { nxl_fail(error, @"the game token is past its expiry"); return nil; }
    return @{@"schema_version": @1, @"token_kind": @"nex", @"token": token,
             @"identity": @{@"pid": @(pid), @"username": username},
             @"created_at": @(floor(now)), @"expires_at": @(expiry),
             @"scopes": [kNxScopes componentsSeparatedByString:@" "]};
}

// /api/oauth/userinfo names the account the sign-in belongs to. The game token must be that account's (NX Port Studio checks the same):
// a mismatch, or no answer, gives no record
__attribute__((unused)) static BOOL nxl_same_account(NSString *access, NSDictionary *record, NSString **error) {
    NSMutableURLRequest *get = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:kNxUserInfoEndpoint]
                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:kNxTimeout];
    [get setValue:[NSString stringWithFormat:@"Bearer %@", access] forHTTPHeaderField:@"Authorization"];
    NSDictionary *answer = nxl_send(get, error);
    if (!answer) return NO;
    NSNumber *pid = record[@"identity"][@"pid"]; NSString *username = record[@"identity"][@"username"];
    bool named = false, same = true;   // the account may sit at the top level, under "account" or under "identity"; each one present must agree
    for (id place in @[answer, answer[@"account"] ?: NSNull.null, answer[@"identity"] ?: NSNull.null]) {
        if (![place isKindOfClass:NSDictionary.class] || !place[@"pid"]) continue;
        named = true;
        same = same && [place[@"pid"] isKindOfClass:NSNumber.class] && [place[@"pid"] isEqual:pid] && [place[@"username"] isEqual:username];
    }
    if (!named) { nxl_fail(error, @"nextendo.network did not say which account the sign-in belongs to"); return NO; }
    if (!same) { nxl_fail(error, @"the game token belongs to another account than the sign-in; sign out and sign in again"); return NO; }
    return YES;
}

// /api/online-status says whether this account may play online now: {"allow": true}, or {"allow": false, "reason": ...}. When it refuses,
// the reason in words for the player; nil when it allows, cannot be asked, or gives no answer
__attribute__((unused)) static NSString *nxl_online_refusal(NSString *access) {
    NSMutableURLRequest *get = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:kNxOnlineStatusEndpoint]
                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:8];
    [get setValue:[NSString stringWithFormat:@"Bearer %@", access] forHTTPHeaderField:@"Authorization"];
    NSDictionary *answer = nxl_send(get, NULL);
    if (![answer[@"allow"] isEqual:@NO]) return nil;
    NSString *reason = [answer[@"reason"] isKindOfClass:NSString.class] ? answer[@"reason"] : @"";
    NSDictionary<NSString *, NSString *> *words = @{
        @"discord_unlinked": @"link a Discord account on your nextendo.network account page",
        @"unverified": @"verify the account's e-mail address with the link Nextendo sent you",
        @"elsewhere": @"this account is online on another device; leave online play there first",
        @"disabled": @"this account is disabled",
        @"unknown": @"Nextendo does not know this account; sign out and sign in again"};
    NSString *what = words[reason] ?: [NSString stringWithFormat:@"Nextendo gave the reason \"%@\"", reason.length ? reason : @"none"];
    return [@"Nextendo does not allow online play yet: " stringByAppendingString:what];
}

// Each refresh replaces the token the next one needs, so one runs at a time, across threads and processes (the Mac tool beside dmhost,
// the iPhone launcher's check beside the game): an exclusive flock on a file in the sign-in's folder. The descriptor (close releases it),
// or -1 when the folder does not exist, which means no sign-in is saved either
__attribute__((unused)) static int nxl_lock(void) {
    int fd = open([nxl_dir() stringByAppendingPathComponent:@".lock"].fileSystemRepresentation, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX) != 0) { close(fd); fd = -1; }
    return fd;
}

// From an access token to a game token's record: /api/nex-token, then the account check. nil and the reason when a step fails
__attribute__((unused)) static NSDictionary *nxl_record_for(NSString *access, NSString **error) {
    NSMutableURLRequest *get = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:kNxGameTokenEndpoint]
                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:kNxTimeout];
    [get setValue:[NSString stringWithFormat:@"Bearer %@", access] forHTTPHeaderField:@"Authorization"];
    NSDictionary *answer = nxl_send(get, error);
    NSString *token = answer[@"nex_token"];
    if (![token isKindOfClass:NSString.class] || token.length == 0) {
        NSString *reason = nxl_online_refusal(access);
        if (reason) nxl_fail(error, reason);
        else if (answer) nxl_fail(error, @"Nextendo issued no game token for this account (Nextendo asks for a verified e-mail address and a Discord link before online play)");
        return nil;
    }
    NSDictionary *record = nxl_record(token, NSDate.date.timeIntervalSince1970, error);
    return record && nxl_same_account(access, record, error) ? record : nil;
}

// From the saved sign-in to a game token's record: refresh grant (under nxl_lock), then nxl_record_for. nil and the reason when a step fails
__attribute__((unused)) static NSDictionary *nxl_game_record(NSString **error) {
    NSString *client = nxl_client_id();
    if (!client) { nxl_fail(error, kNxNoClient); return nil; }
    int lock = nxl_lock();
    NSString *refresh = nxl_read_refresh();
    NSString *access = refresh ? nxl_token(@{@"grant_type": @"refresh_token", @"refresh_token": refresh, @"client_id": client}, error) : nil;
    if (lock >= 0) close(lock);
    if (!refresh) {
#if TARGET_OS_IPHONE
        nxl_fail(error, @"this iPhone is not signed in to Nextendo: sign in on the game's settings screen");
#else
        nxl_fail(error, @"this Mac is not signed in to Nextendo: run nextendo_login sign-in");
#endif
        return nil;
    }
    return access ? nxl_record_for(access, error) : nil;
}

// ---- Signing in (the browser part belongs to the caller: the Mac opens the default browser, the iPhone a web authentication session) ----

__attribute__((unused)) static NSString *nxl_random_urlsafe(size_t bytes) {
    NSMutableData *random = [NSMutableData dataWithLength:bytes];
    if (bytes == 0 || bytes > 64 || SecRandomCopyBytes(kSecRandomDefault, bytes, random.mutableBytes) != errSecSuccess) return nil;
    return nxl_b64url(random);
}

// A TCP listener on 127.0.0.1 for the redirect: kNxCallbackPort when free, else a port the system picks. The descriptor, or -1
__attribute__((unused)) static int nxl_listen_loopback(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    const uint16_t tries[2] = {kNxCallbackPort, 0};
    bool bound = false;
    for (int i = 0; i < 2 && !bound; i++) { address.sin_port = htons(tries[i]); bound = bind(fd, (struct sockaddr *)&address, sizeof address) == 0; }
    socklen_t size = sizeof address;
    if (!bound || listen(fd, 4) != 0 || getsockname(fd, (struct sockaddr *)&address, &size) != 0) { close(fd); return -1; }
    *port = ntohs(address.sin_port);
    return fd;
}

__attribute__((unused)) static void nxl_answer(int client, int status, const char *html) {
    char header[200];
    int length = snprintf(header, sizeof header, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          status, status == 200 ? "OK" : "Not Found", strlen(html));
    (void)!write(client, header, (size_t)length);
    (void)!write(client, html, strlen(html));
}

// Serves the listener until a GET /callback with our state arrives (anything else gets a 404) and returns its query parameters.
// nil once `seconds` pass or *cancel (may be NULL) becomes nonzero; the poll wakes twice a second to notice that
__attribute__((unused)) static NSDictionary<NSString *, NSString *> *nxl_wait_callback(int fd, NSString *state, int seconds, _Atomic int *cancel) {
    const time_t deadline = time(NULL) + seconds;
    while (!(cancel && atomic_load(cancel)) && time(NULL) < deadline) {
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        if (poll(&ready, 1, 500) <= 0) continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0) continue;
        int on = 1;   // a browser's preconnect may close before the answer; a write to it must fail, not raise SIGPIPE
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        struct timeval limit = {.tv_sec = 5};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
        char request[16384]; size_t have = 0;   // only the request line is needed
        while (have < sizeof request - 1 && !memchr(request, '\n', have)) {
            ssize_t n = read(client, request + have, sizeof request - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
        }
        request[have] = 0;
        char target[16384] = "";
        NSDictionary<NSString *, NSString *> *query = nil;
        NSString *path = sscanf(request, "GET %16383s HTTP/", target) == 1 ? [NSString stringWithUTF8String:target] : nil;   // nil when not UTF-8
        if (path) {
            NSURLComponents *url = [NSURLComponents componentsWithString:[@"http://127.0.0.1" stringByAppendingString:path]];
            NSMutableDictionary *items = [NSMutableDictionary dictionary];
            for (NSURLQueryItem *item in url.queryItems) items[item.name] = item.value ?: @"";
            if ([url.path isEqual:@"/callback"] && [items[@"state"] isEqual:state]) query = items;
        }
        if (query) nxl_answer(client, 200, "<!doctype html><title>nxnative</title><p>Nextendo sign-in received. You can close this page.</p>");
        else nxl_answer(client, 404, "");
        close(client);
        if (query) return query;
    }
    return nil;
}

// Nextendo's sign-in page for this redirect, with `state` and the S256 challenge of `verifier`
__attribute__((unused)) static NSURL *nxl_authorize_url(NSString *redirect, NSString *state, NSString *verifier) {
    NSData *plain = [verifier dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableData *challenge = [NSMutableData dataWithLength:CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(plain.bytes, (CC_LONG)plain.length, challenge.mutableBytes);
    NSURLComponents *page = [NSURLComponents componentsWithString:kNxAuthorize];
    NSString *client = nxl_client_id();
    if (!client) return nil;   // callers check nxl_client_id() first
    NSDictionary<NSString *, NSString *> *parameters = @{@"response_type": @"code", @"client_id": client, @"redirect_uri": redirect, @"scope": kNxScopes,
                                                         @"state": state, @"code_challenge": nxl_b64url(challenge), @"code_challenge_method": @"S256"};
    NSMutableArray<NSURLQueryItem *> *items = [NSMutableArray array];
    for (NSString *name in [parameters.allKeys sortedArrayUsingSelector:@selector(compare:)]) [items addObject:[NSURLQueryItem queryItemWithName:name value:parameters[name]]];
    page.queryItems = items;
    return page.URL;
}

// After the redirect: the code becomes the saved sign-in, and a game token is fetched as a game would, to show the account works.
// nil and the reason when a step fails
__attribute__((unused)) static NSDictionary *nxl_finish_sign_in(NSDictionary<NSString *, NSString *> *query, NSString *verifier, NSString *redirect, NSString **error) {
    if (!query) { nxl_fail(error, @"no answer from the sign-in page"); return nil; }
    if (query[@"error"]) { nxl_fail(error, [NSString stringWithFormat:@"the sign-in page answered with an error: %@", query[@"error"]]); return nil; }
    NSString *code = query[@"code"];
    if (code.length == 0) { nxl_fail(error, @"the sign-in page returned without an authorization code"); return nil; }
    NSString *client = nxl_client_id();
    if (!client) { nxl_fail(error, kNxNoClient); return nil; }
    NSDictionary *grant = @{@"grant_type": @"authorization_code", @"code": code, @"code_verifier": verifier, @"redirect_uri": redirect, @"client_id": client};
    int lock = nxl_lock();
    nxl_write_refresh(nil);
    NSString *access = nxl_token(grant, error), *saved = nxl_read_refresh();
    if (lock >= 0) close(lock);
    if (!access) return nil;
    if (!saved) { nxl_fail(error, @"the token answer had no refresh_token, so there is nothing to keep for later"); return nil; }
    return nxl_record_for(access, error);   // the code's own access token: no second refresh
}
