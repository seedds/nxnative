// SPDX-License-Identifier: MIT
// Supplies the Nextendo credential (dm_nextendo_set_credential_provider, hle_nextendo_apple.m) from this device's saved sign-in
// (nextendo_login_apple.inc). The Mac signs in with dm/tools/nextendo_login.m, the iPhone on the game's settings screen
// (dm/ios/Sources/NextendoSignIn.m). A record is reused until two minutes before its game token expires. After a failed fetch
// the next attempt waits 30 seconds; meanwhile the current record serves until its own expiry, and with none the game sees no account
// rather than a made-up one.
// The refresh's OAuth access token is kept beside it, in memory only and never logged: /api/me, /api/friends and /api/presence take it
// (dm_nextendo_login_access; /api/me refuses the nx2 game token), and /api/online-status says with it why Nextendo refuses online play
// (dm_nextendo_login_online_check), which a game's own login cannot show: the NEX protocol carries no reason.
#include <TargetConditionals.h>
#if TARGET_OS_OSX || TARGET_OS_IOS
#import "nextendo_login_apple.inc"
#include "nextendo_account.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);
void dm_nextendo_login_online_check(const char *when);

enum { ACCESS_LIFE = 50 * 60 };   // Nextendo's access tokens live an hour; one older than this is renewed first

static struct {
    pthread_mutex_t lock;
    NSData *json;               // the current record, encoded
    NSTimeInterval expires;     // its game token's expiry
    NSTimeInterval next_try;    // no fetch before this after a failure
    NSString *access;           // the refresh's access token, and when it was issued
    NSTimeInterval access_at;
} g_cache = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void refresh_locked(NSTimeInterval now) {
    NSString *why = nil, *access = nil;
    NSDictionary *record = nxl_game_record_access(&why, &access);
    NSData *json = record ? [NSJSONSerialization dataWithJSONObject:record options:0 error:nil] : nil;
    if (!json) {   // the current record, if any, serves until its own expiry (provide_credential checks it)
        g_cache.next_try = now + 30;
        fprintf(stderr, "[dm] [RED] nextendo: no %sgame credential: %s\n", g_cache.json && g_cache.expires > now ? "new " : "", (why ?: @"the record could not be encoded").UTF8String);
        return;
    }
    g_cache.json = json; g_cache.expires = [record[@"expires_at"] doubleValue]; g_cache.next_try = 0;
    g_cache.access = access; g_cache.access_at = now;
    fprintf(stderr, "[dm] nextendo: signed in as %s\n", [record[@"identity"][@"username"] UTF8String]);
}

static bool provide_credential(uint8_t *buffer, size_t capacity, size_t *size) { @autoreleasepool {
    dm_guest_host_wait_begin();   // a refresh is network round trips, and other callers wait on the lock meanwhile: their guest cores are free
    pthread_mutex_lock(&g_cache.lock);
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    bool stale = !g_cache.json || g_cache.expires - now < 120;
    if (stale && now >= g_cache.next_try) refresh_locked(now);
    bool usable = g_cache.json && g_cache.expires > now && g_cache.json.length <= capacity;
    if (usable) { memcpy(buffer, g_cache.json.bytes, g_cache.json.length); *size = g_cache.json.length; }
    pthread_mutex_unlock(&g_cache.lock);
    dm_guest_host_wait_end();
    static atomic_bool asked;   // the first game credential also asks once whether Nextendo will let this account play (on a queue: the game does not wait)
    if (usable && !atomic_exchange(&asked, true))
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ dm_nextendo_login_online_check("at the game's first sign-in"); });
    return usable;
} }

// The access token for Nextendo's account API, renewed when older than ACCESS_LIFE; nil without a sign-in or while a refresh fails
NSString *dm_nextendo_login_access(void) { @autoreleasepool {
    dm_guest_host_wait_begin();
    pthread_mutex_lock(&g_cache.lock);
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    if ((!g_cache.access || now - g_cache.access_at >= ACCESS_LIFE) && now >= g_cache.next_try) refresh_locked(now);
    NSString *access = g_cache.access && now - g_cache.access_at < ACCESS_LIFE + 600 ? g_cache.access : nil;
    pthread_mutex_unlock(&g_cache.lock);
    dm_guest_host_wait_end();
    return access;
} }

// Asks /api/online-status whether this account may play online now and logs the answer when it changes: "allowed", the refusal in words
// (the gates Nextendo's game servers apply at a game's login: e-mail, Discord link, another device online, a disabled account), or that it
// could not be asked. when says what prompted it ("at the start", "after the game's error 2306-0807")
void dm_nextendo_login_online_check(const char *when) { @autoreleasepool {
    NSString *access = dm_nextendo_login_access();
    NSString *line;
    if (!access) line = @"[RED] nextendo: online status not asked: no access token (no sign-in, or its refresh failed)";
    else {
        NSMutableURLRequest *get = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:kNxOnlineStatusEndpoint]
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:8];
        [get setValue:[NSString stringWithFormat:@"Bearer %@", access] forHTTPHeaderField:@"Authorization"];
        NSString *why = nil;
        dm_guest_host_wait_begin(); NSDictionary *answer = nxl_send(get, &why); dm_guest_host_wait_end();
        if (!answer) line = [NSString stringWithFormat:@"[RED] nextendo: online status could not be asked: %@", why ?: @"no answer"];
        else if ([answer[@"allow"] isEqual:@YES]) line = @"nextendo: Nextendo allows online play for this account";
        else line = [@"[RED] nextendo: " stringByAppendingString:nxl_refusal_words(answer[@"reason"])];
    }
    static pthread_mutex_t told_lock = PTHREAD_MUTEX_INITIALIZER; static NSString *told;
    pthread_mutex_lock(&told_lock); bool fresh = ![told isEqual:line]; if (fresh) told = line; pthread_mutex_unlock(&told_lock);
    if (fresh) fprintf(stderr, "[dm] %s (%s)\n", line.UTF8String, when ? when : "asked");
} }

__attribute__((constructor)) static void install_provider(void) { dm_nextendo_set_credential_provider(provide_credential); }
#endif
