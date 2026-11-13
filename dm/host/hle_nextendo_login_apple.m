// SPDX-License-Identifier: MIT
// Supplies the Nextendo credential (dm_nextendo_set_credential_provider, hle_nextendo_apple.m) from this device's saved sign-in
// (nextendo_login_apple.inc). The Mac signs in with dm/tools/nextendo_login.m, the iPhone on the game's settings screen
// (dm/ios/Sources/NextendoSignIn.m). A record is reused until two minutes before its game token expires. After a failed fetch
// the next attempt waits 30 seconds; meanwhile the current record serves until its own expiry, and with none the game sees no account
// rather than a made-up one.
#include <TargetConditionals.h>
#if TARGET_OS_OSX || TARGET_OS_IOS
#import "nextendo_login_apple.inc"
#include "nextendo_account.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);

static struct {
    pthread_mutex_t lock;
    NSData *json;               // the current record, encoded
    NSTimeInterval expires;     // its game token's expiry
    NSTimeInterval next_try;    // no fetch before this after a failure
} g_cache = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void refresh_locked(NSTimeInterval now) {
    NSString *why = nil;
    NSDictionary *record = nxl_game_record(&why);
    NSData *json = record ? [NSJSONSerialization dataWithJSONObject:record options:0 error:nil] : nil;
    if (!json) {   // the current record, if any, serves until its own expiry (provide_credential checks it)
        g_cache.next_try = now + 30;
        fprintf(stderr, "[dm] [RED] nextendo: no %sgame credential: %s\n", g_cache.json && g_cache.expires > now ? "new " : "", (why ?: @"the record could not be encoded").UTF8String);
        return;
    }
    g_cache.json = json; g_cache.expires = [record[@"expires_at"] doubleValue]; g_cache.next_try = 0;
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
    return usable;
} }

__attribute__((constructor)) static void install_provider(void) { dm_nextendo_set_credential_provider(provide_credential); }
#endif
