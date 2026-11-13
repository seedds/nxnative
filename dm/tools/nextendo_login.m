// SPDX-License-Identifier: MIT
// nextendo_login: the Mac's Nextendo sign-in for nxnative's online play (dm/host/nextendo_login_apple.inc).
//   nextendo_login sign-in    shows Nextendo's sign-in page in the browser and saves the sign-in on this Mac
//   nextendo_login status     fetches a game token as a game would and names the account
//   nextendo_login sign-out   deletes the saved sign-in
// DM_NEXTENDO_CLIENT_ID names the Nextendo application it signs in as (nextendo_login_apple.inc).
// Build: make -C dm/host nextendo_login. It prints the account's name and PID, never a token. The iPhone app signs in on its
// settings screen instead (dm/ios/Sources/NextendoSignIn.m).
#import <AppKit/AppKit.h>
#import "../host/nextendo_login_apple.inc"
#include <stdio.h>

static int print_result(NSDictionary *record, NSString *why) {
    if (!record) { fprintf(stderr, "nextendo_login: %s\n", why.UTF8String); return 1; }
    NSDate *expiry = [NSDate dateWithTimeIntervalSince1970:[record[@"expires_at"] doubleValue]];
    NSString *when = [NSDateFormatter localizedStringFromDate:expiry dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterShortStyle];
    printf("Signed in to Nextendo as %s (PID %s). A game token is available, valid until %s.\n",
           [record[@"identity"][@"username"] UTF8String], [[record[@"identity"][@"pid"] stringValue] UTF8String], when.UTF8String);
    return 0;
}

static int sign_in(void) {
    if (!nxl_client_id()) { fprintf(stderr, "nextendo_login: %s\n", kNxNoClient.UTF8String); return 1; }
    NSString *verifier = nxl_random_urlsafe(32), *state = nxl_random_urlsafe(16);
    if (!verifier || !state) { fputs("nextendo_login: no random bytes\n", stderr); return 1; }
    uint16_t port = 0;
    int listener = nxl_listen_loopback(&port);
    if (listener < 0) { perror("nextendo_login: loopback listener"); return 1; }
    NSString *redirect = [NSString stringWithFormat:@"http://127.0.0.1:%u/callback", port];
    NSURL *page = nxl_authorize_url(redirect, state, verifier);
    printf("Opening Nextendo's sign-in page in your browser (waiting up to 5 minutes). If it does not open, go to:\n%s\n", page.absoluteString.UTF8String);
    [NSWorkspace.sharedWorkspace openURL:page];
    NSDictionary *query = nxl_wait_callback(listener, state, 300, NULL);
    close(listener);
    if (!query) return print_result(nil, @"no answer from the sign-in page in 5 minutes");
    NSString *why = nil;
    NSDictionary *record = nxl_finish_sign_in(query, verifier, redirect, &why);
    return print_result(record, why);
}

int main(int argc, char **argv) { @autoreleasepool {
    const char *command = argc == 2 ? argv[1] : "";
    if (!strcmp(command, "sign-in")) return sign_in();
    if (!strcmp(command, "status")) { NSString *why = nil; NSDictionary *record = nxl_game_record(&why); return print_result(record, why); }
    if (!strcmp(command, "sign-out")) {
        if (!nxl_write_refresh(nil)) { perror("nextendo_login: sign-out"); return 1; }
        puts("Signed out of Nextendo on this Mac.");
        return 0;
    }
    fputs("usage: nextendo_login sign-in | status | sign-out\n", stderr);
    return 2;
} }
