// SPDX-License-Identifier: MIT
// Nextendo sign-in on the iPhone (NextendoSignIn.h). Nextendo redirects only to a loopback address, which neither a custom URL scheme
// nor an associated domain can stand for. So the web authentication session is started without a callback scheme, a listener in the
// app takes the redirect, and the session is then closed by the app.
#import "NextendoSignIn.h"
#import "../../host/nextendo_login_apple.inc"
#import <AuthenticationServices/AuthenticationServices.h>
#import <UIKit/UIKit.h>

typedef void (^DmNextendoDone)(NSString *_Nullable account, NSString *_Nullable error);

static void report(DmNextendoDone done, NSDictionary *record, NSString *why) {
    NSString *account = record ? [NSString stringWithFormat:@"%@ (PID %@)", record[@"identity"][@"username"], record[@"identity"][@"pid"]] : nil;
    NSString *error = record ? nil : why ?: @"the sign-in failed";
    dispatch_async(dispatch_get_main_queue(), ^{ done(account, error); });
}

BOOL dm_nextendo_built(void) {
    NSString *profile = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm/nextendo-services/routes.tsv"];
    return [NSFileManager.defaultManager fileExistsAtPath:profile];
}
BOOL dm_nextendo_signed_in(void) { return nxl_read_refresh() != nil; }
void dm_nextendo_sign_out(void) { nxl_write_refresh(nil); }
void dm_nextendo_check(DmNextendoDone done) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *why = nil;
        NSDictionary *record = nxl_game_record(&why);
        report(done, record, why);
    });
}

// The window the sign-in sheet is shown over: the key window of a connected scene, else any window of one
@interface DmNextendoPresenter : NSObject <ASWebAuthenticationPresentationContextProviding>
@end
@implementation DmNextendoPresenter
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:(ASWebAuthenticationSession *)session {
    (void)session;
    UIWindow *fallback = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            if (window.isKeyWindow) return window;
            if (!fallback) fallback = window;
        }
    }
    return fallback ?: [[UIWindow alloc] init];   // the launcher always has a window; this keeps the type's contract
}
@end

void dm_nextendo_sign_in(DmNextendoDone done) {
    static ASWebAuthenticationSession *current;
    static DmNextendoPresenter *presenter;
    static _Atomic int closed_by_player;
    if (!nxl_client_id()) { report(done, nil, kNxNoClient); return; }
    NSString *verifier = nxl_random_urlsafe(32), *state = nxl_random_urlsafe(16);
    uint16_t port = 0;
    int listener = verifier && state ? nxl_listen_loopback(&port) : -1;
    if (listener < 0) { report(done, nil, @"the sign-in could not start (no loopback listener)"); return; }
    NSString *redirect = [NSString stringWithFormat:@"http://127.0.0.1:%u/callback", port];

    [current cancel];
    atomic_store(&closed_by_player, 0);
    presenter = [DmNextendoPresenter new];
    // With no callback scheme, the completion handler runs only when the player closes the sheet
    current = [[ASWebAuthenticationSession alloc] initWithURL:nxl_authorize_url(redirect, state, verifier) callbackURLScheme:nil
                                            completionHandler:^(NSURL *url, NSError *error) { (void)url; (void)error; atomic_store(&closed_by_player, 1); }];
    current.presentationContextProvider = presenter;
    current.prefersEphemeralWebBrowserSession = NO;
    if (![current start]) {
        close(listener); current = nil;
        report(done, nil, @"the sign-in page could not be shown");
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSDictionary *query = nxl_wait_callback(listener, state, 300, &closed_by_player);
        close(listener);
        bool cancelled = atomic_load(&closed_by_player);
        dispatch_async(dispatch_get_main_queue(), ^{ [current cancel]; current = nil; });
        if (!query) { report(done, nil, cancelled ? @"the sign-in was cancelled" : @"no answer from the sign-in page in 5 minutes"); return; }
        NSString *why = nil;
        NSDictionary *record = nxl_finish_sign_in(query, verifier, redirect, &why);
        report(done, record, why);
    });
}
