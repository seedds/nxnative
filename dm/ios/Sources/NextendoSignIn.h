// SPDX-License-Identifier: MIT
// The launcher's Nextendo section (Launcher.swift) calls these. They sign in through dm/host/nextendo_login_apple.inc, showing
// Nextendo's page in an ASWebAuthenticationSession; the host turns the saved sign-in into a game token when a game asks
// (hle_nextendo_login_apple.m).

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// YES when the app was built with --nextendo, so its bundle holds the public service profile (dm/nextendo-services, build_ipa.sh).
BOOL dm_nextendo_built(void);

/// YES when a sign-in is saved on this device. No network; dm_nextendo_check tests the account itself.
BOOL dm_nextendo_signed_in(void);

/// Fetches a game token as a game would, off the main thread. done runs on the main thread with "name (PID n)", or nil and the reason.
void dm_nextendo_check(void (^done)(NSString *_Nullable account, NSString *_Nullable error));

/// Shows Nextendo's sign-in page over the app. done runs on the main thread when it is over, as for dm_nextendo_check.
void dm_nextendo_sign_in(void (^done)(NSString *_Nullable account, NSString *_Nullable error));

/// Deletes the saved sign-in.
void dm_nextendo_sign_out(void);

NS_ASSUME_NONNULL_END
