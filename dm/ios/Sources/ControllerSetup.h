#pragma once
#import <Foundation/Foundation.h>

// In-process launch setup bridge. Call on the main thread.
NS_ASSUME_NONNULL_BEGIN
NSArray<NSDictionary<NSString *, id> *> *dm_ios_controller_snapshot(void);
NSString * _Nullable dm_ios_controller_configure(NSArray<NSDictionary<NSString *, id> *> *choices);
BOOL dm_ios_controller_identify(NSString *deviceID);
// Status includes requiredStyle, empty unless one external style is the only accepted style.
NSDictionary<NSString *, id> *dm_ios_controller_status(NSArray<NSDictionary<NSString *, id> *> *choices, BOOL inGame);
NSString * _Nullable dm_ios_controller_apply(NSArray<NSDictionary<NSString *, id> *> *choices);
// Updated when another game request joins an open sheet.
BOOL dm_ios_controller_can_join(void);
void dm_ios_controller_open_join(void (^completion)(BOOL));
void dm_gamepad_cancel_join(void);
void dm_ios_controller_limits(int *minimum, int *maximum);
NS_ASSUME_NONNULL_END
