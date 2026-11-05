// Opt-in, host-process player assignment. Objects, never names or enumeration indices, identify sources.
#pragma once
#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#include "dm_input.h"

typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; } DmPlayerState;
typedef NS_ENUM(NSInteger, DmPlayerRole) { DmPlayerOwn, DmPlayerShare };
@interface DmPlayerSource : NSObject
@property(nonatomic, strong) GCController *controller;
@property(nonatomic) BOOL split;
@property(nonatomic) NSInteger player, rightPlayer; // -1 disabled; otherwise zero-based
@property(nonatomic) DmPlayerRole role, rightRole;
@property(nonatomic) NSUInteger number; // session label, never a physical identity
@end
typedef NS_ENUM(NSInteger, DmPlayerAssignment) {
    DmPlayerAssignmentKeyboard, DmPlayerAssignmentWhole, DmPlayerAssignmentRight, DmPlayerAssignmentSplit,
    DmPlayerAssignmentKeyboardMode, DmPlayerAssignmentWholeMode, DmPlayerAssignmentRightMode
};
// Immutable-in-use logical route snapshot; source identity stays bound through hotplug.
@interface DmPlayerRoute : NSObject
@property(nonatomic, strong) DmPlayerSource *source; // nil is keyboard
@property(nonatomic) DmPlayerAssignment field;
@property(nonatomic, copy) NSString *name;
@property(nonatomic) NSInteger player;
@property(nonatomic) DmPlayerRole role;
@property(nonatomic) BOOL supported;
@end
@interface DmMacPlayers : NSObject
@property(nonatomic, readonly) NSArray<DmPlayerSource *> *sources;
@property(nonatomic) NSInteger keyboardPlayer; // -1 disabled, otherwise zero-based
@property(nonatomic) DmPlayerRole keyboardRole;
@property(nonatomic, readonly) NSString *assignmentError;
@property(nonatomic, copy) void (^routeLog)(NSString *line);
- (NSArray<NSString *> *)playerSummaries;
- (NSArray<NSString *> *)routeDescriptions;
// Atomic mode/target change; rejects conflicts without altering the previous route.
// UI target choice: -2 creates a separate player; selecting an occupied player consents to combining input.
- (BOOL)selectPlayer:(NSInteger)player source:(DmPlayerSource *)source field:(DmPlayerAssignment)field;
- (BOOL)assignRoute:(DmPlayerSource *)source field:(DmPlayerAssignment)field player:(NSInteger)player role:(DmPlayerRole)role;
@property(nonatomic) BOOL active;
@property(nonatomic, copy) void (^output)(int, DmPlayerState);
@property(nonatomic, copy) void (^players)(uint32_t);
@property(nonatomic, copy) void (^routesChanged)(void);
// Profile opt-in: physical SL/SR replace ordinary shoulders on sideways Joy-Cons.
// Provider returns neutral raw bits 0x20=SL, 0x10=SR, only for an unambiguous side.
@property(nonatomic) BOOL nativeSingleJoyCons;
@property(nonatomic, copy) BOOL (^joyConRails)(int side, uint8_t *bits);
@property(nonatomic, copy) void (^styles)(int player, uint32_t style);
- (uint32_t)controllerStyleForPlayer:(int)player;
- (void)sync:(NSArray<GCController *> *)controllers;
- (void)read;
// Recompute and deliver every state (including zeros) and the mask to newly attached callbacks.
- (void)publish;
- (void)keyboard:(DmPlayerState)state;
- (GCController *)wholeControllerForPlayer:(int)player;
// Embed in the existing launch window. No window or modal session is created.
- (NSView *)configurationView;
- (NSArray<DmPlayerRoute *> *)assignedRoutes;
- (NSArray<DmPlayerRoute *> *)availableRoutes;
// Call before syncing controllers on opening; end before launching or hiding setup.
- (BOOL)beginConfiguration;
- (void)endConfiguration; // cleanup only, including cancellation; never persists choices
// Call only on confirmation/launch, not cancellation; honors the view's Remember checkbox
// and the defaults supplied to loadPreferences:. Do not save to standard defaults directly.
- (void)commitPreferences;
// The embedded view uses this same windowless action seam. Device actions require a live source identity.
- (void)assignSource:(DmPlayerSource *)source field:(DmPlayerAssignment)field value:(NSInteger)value;
// Saved controller descriptions are preferences, never physical-device identities. Only a
// unique saved/current match restores automatically; ambiguous devices require a button press.
- (NSArray<NSNumber *> *)pendingLayoutIndexes;
- (NSString *)savedLayoutDescription:(NSUInteger)index;
- (BOOL)armSavedLayout:(NSUInteger)index;
- (BOOL)forgetSavedLayout:(NSUInteger)index; // preview only; persisted only on Play
- (void)loadPreferences:(NSUserDefaults *)defaults;
- (void)savePreferences:(NSUserDefaults *)defaults;
@end
