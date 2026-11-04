#import "players_mac.h"
#import "pad_apple.h"
#include <math.h>

@interface DmPlayerSource ()
@property(nonatomic) NSInteger savedLayoutIndex;
@property(nonatomic) BOOL manuallyAssigned, automaticallyRestored;
@end
@implementation DmPlayerSource
@end
@implementation DmPlayerRoute
@end

@protocol DmPlayerControl
@property(nonatomic, strong) DmPlayerSource *source;
@end
@interface DmPlayerRows : NSView
@end
@implementation DmPlayerRows
- (BOOL)isFlipped { return YES; }
@end
@interface DmPlayerPicker : NSPopUpButton <DmPlayerControl>
@property(nonatomic, strong) DmPlayerSource *source;
@end
@implementation DmPlayerPicker
@end
@interface DmPlayerButton : NSButton <DmPlayerControl>
@property(nonatomic, strong) DmPlayerSource *source;
@property(nonatomic) NSInteger value;
@end
@implementation DmPlayerButton
@end

static int32_t axis(float v) { return (int32_t)(fmaxf(-1, fminf(1, v)) * DM_STICK_MAX); }
static DmPlayerState whole(GCExtendedGamepad *g) {
    DmPlayerState s = {.buttons = dm_pad_face(g)};
#define K(btn, bit) if (btn.isPressed) s.buttons |= bit
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R); K(g.leftTrigger, DM_BTN_ZL); K(g.rightTrigger, DM_BTN_ZR);
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
#undef K
    s.lx = axis(g.leftThumbstick.xAxis.value); s.ly = axis(g.leftThumbstick.yAxis.value);
    s.rx = axis(g.rightThumbstick.xAxis.value); s.ry = axis(g.rightThumbstick.yAxis.value);
    return s;
}
// App-local split INPUT, not Bluetooth unpairing. Sideways: L rotates CCW, R rotates CW.
// The profile-opted rail path below replaces this legacy shoulder mapping with passive HID SL/SR.
static DmPlayerState half(GCExtendedGamepad *g, BOOL right) {
    DmPlayerState s = {0};
    if (right) {
        uint64_t face = dm_pad_face(g);
        if (face & DM_BTN_X) s.buttons |= DM_BTN_A;
        if (face & DM_BTN_A) s.buttons |= DM_BTN_B;
        if (face & DM_BTN_Y) s.buttons |= DM_BTN_X;
        if (face & DM_BTN_B) s.buttons |= DM_BTN_Y;
    } else {
        if (g.dpad.down.isPressed) s.buttons |= DM_BTN_A;
        if (g.dpad.left.isPressed) s.buttons |= DM_BTN_B;
        if (g.dpad.right.isPressed) s.buttons |= DM_BTN_X;
        if (g.dpad.up.isPressed) s.buttons |= DM_BTN_Y;
    }
    if ((right ? g.rightShoulder : g.leftShoulder).isPressed) s.buttons |= DM_BTN_L;
    if ((right ? g.rightTrigger : g.leftTrigger).isPressed) s.buttons |= DM_BTN_R;
    if ((right ? g.buttonMenu : g.buttonOptions).isPressed) s.buttons |= DM_BTN_PLUS;
    if ((right ? g.rightThumbstickButton : g.leftThumbstickButton).isPressed) s.buttons |= DM_BTN_LS;
    GCControllerDirectionPad *stick = right ? g.rightThumbstick : g.leftThumbstick;
    s.lx = axis(right ? stick.yAxis.value : -stick.yAxis.value);
    s.ly = axis(right ? -stick.xAxis.value : stick.xAxis.value);
    return s;
}
static BOOL combined(GCController *c) { return c.extendedGamepad && [c.productCategory isEqualToString:@"Nintendo Switch Joy-Con (L/R)"]; }
static int single_side(DmPlayerSource *s, int side) {
    if (s.split && combined(s.controller)) return side;
    NSString *category = s.controller.productCategory;
    if ([category isEqualToString:@"Nintendo Switch Joy-Con (L)"]) return 0;
    if ([category isEqualToString:@"Nintendo Switch Joy-Con (R)"]) return 1;
    return -1;
}
static void merge(DmPlayerState *a, DmPlayerState b) {
    a->buttons |= b.buttons;
    if (llabs(b.lx) + llabs(b.ly) > llabs(a->lx) + llabs(a->ly)) { a->lx = b.lx; a->ly = b.ly; }
    if (llabs(b.rx) + llabs(b.ry) > llabs(a->rx) + llabs(a->ry)) { a->rx = b.rx; a->ry = b.ry; }
}
static BOOL valid(NSInteger p) { return p >= 0 && p < DM_PLAYERS; }

// Individual Joy-Cons expose the standard GCGamepad shape, not an extended profile.
// Use its supported physical-input interface instead of deprecated GCGamepad accessors.
static GCPhysicalInputProfile *standard_profile(GCController *c) {
    if (c.extendedGamepad) return nil;
    GCPhysicalInputProfile *p = c.physicalInputProfile;
    return p.dpads[GCInputDirectionPad] && p.buttons[GCInputButtonA] && p.buttons[GCInputButtonB] &&
        p.buttons[GCInputButtonX] && p.buttons[GCInputButtonY] && p.buttons[GCInputLeftShoulder] &&
        p.buttons[GCInputRightShoulder] ? p : nil;
}
static BOOL controller_supported(GCController *c) { return c.extendedGamepad || standard_profile(c); }
static NSArray<NSString *> *standard_button_keys(void) {
    return @[GCInputButtonA, GCInputButtonB, GCInputButtonX, GCInputButtonY,
        GCInputLeftShoulder, GCInputRightShoulder, GCInputButtonMenu, GCInputButtonHome];
}
static DmPlayerState standard_state(GCPhysicalInputProfile *p) {
    DmPlayerState s = {0};
    // GCGamepad's documented diamond is A bottom, B right, X left, Y top. The
    // platform already maps an individual sideways Joy-Con into this logical layout.
#define K(key, bit) if (p.buttons[key].isPressed) s.buttons |= bit
    K(GCInputButtonB, DM_BTN_A); K(GCInputButtonA, DM_BTN_B); K(GCInputButtonY, DM_BTN_X); K(GCInputButtonX, DM_BTN_Y);
    K(GCInputLeftShoulder, DM_BTN_L); K(GCInputRightShoulder, DM_BTN_R); K(GCInputButtonMenu, DM_BTN_PLUS);
#undef K
    GCControllerDirectionPad *stick = p.dpads[GCInputDirectionPad];
    s.lx = axis(stick.xAxis.value); s.ly = axis(stick.yAxis.value);
    return s;
}

@interface DmMacPlayers ()
@property(nonatomic) NSMutableArray<DmPlayerSource *> *entries;
@property(nonatomic) NSView *rows;
@property(nonatomic) NSTextField *activity;
@property(nonatomic) NSButton *remember;
@property(nonatomic) NSView *configuration;
@property(nonatomic) BOOL choosing, rememberSetup;
@property(nonatomic) id keyMonitor;
@property(nonatomic) NSMutableArray *connectionObservers;
@property(nonatomic) NSTextField *summary, *errorLabel;
@property(nonatomic) NSLayoutConstraint *rowsHeight;
@property(nonatomic, copy) NSString *assignmentError, *identifiedName;
@property(nonatomic) NSUInteger nextNumber;
@property(nonatomic) BOOL configuring;
@property(nonatomic) NSUserDefaults *preferences;
@property(nonatomic) NSArray<NSDictionary *> *savedLayouts;
@property(nonatomic) NSInteger armedLayout;
@property(nonatomic) NSMutableIndexSet *confirmationRequired, *discardedLayouts;
@end
@implementation DmMacPlayers {
    DmPlayerState _keyboard;
    DmPlayerState _last[DM_PLAYERS];
    uint32_t _mask, _lastStyles[DM_PLAYERS];
    BOOL _hasConfigured;
}
- (instancetype)init {
    if ((self = [super init])) { _entries = [NSMutableArray new]; _active = YES; _keyboardPlayer = 0; _keyboardRole = DmPlayerShare; _armedLayout = -1; _confirmationRequired = [NSMutableIndexSet new]; _discardedLayouts = [NSMutableIndexSet new]; }
    return self;
}
- (NSArray *)sources { return [_entries copy]; }
- (void)loadPreferences:(NSUserDefaults *)defaults {
    self.preferences = defaults;
    id saved = [defaults objectForKey:@"players.controllers.v1"];
    _savedLayouts = nil; _armedLayout = -1;
    [_confirmationRequired removeAllIndexes]; [_discardedLayouts removeAllIndexes];
    for (DmPlayerSource *s in _entries) { s.savedLayoutIndex = -1; s.automaticallyRestored = NO; }
    if ([saved isKindOfClass:NSArray.class] && [saved count] <= 32) {
        BOOL ok = YES;
        for (id item in saved) if (![self validSavedLayout:item]) { ok = NO; break; }
        if (ok) _savedLayouts = [saved copy];
    }
    _rememberSetup = _savedLayouts != nil;
    id p = [defaults objectForKey:@"players.keyboard.v1"];
    id mode = [defaults objectForKey:@"players.keyboard.mode.v1"];
    // Legacy slot-only choices used shared routing. Validate mode independently and never
    // coerce strings, fractions, or unknown future enum values into an assignment.
    DmPlayerRole role = DmPlayerShare;
    if (mode) {
        if (![mode isKindOfClass:NSNumber.class] || [mode doubleValue] != [mode integerValue] ||
            ([mode integerValue] != DmPlayerOwn && [mode integerValue] != DmPlayerShare)) return;
        role = [mode integerValue];
    }
    if ([p isKindOfClass:NSNumber.class] && [p doubleValue] == [p integerValue] && [p integerValue] >= -1 && [p integerValue] < DM_PLAYERS) {
        if ([self assignRoute:nil field:DmPlayerAssignmentKeyboard player:[p integerValue] role:role]) _rememberSetup = YES;
    }
}
- (void)savePreferences:(NSUserDefaults *)defaults {
    if (_keyboardPlayer >= -1 && _keyboardPlayer < DM_PLAYERS && (_keyboardRole == DmPlayerOwn || _keyboardRole == DmPlayerShare)) {
        [defaults setInteger:_keyboardPlayer forKey:@"players.keyboard.v1"];
        [defaults setInteger:_keyboardRole forKey:@"players.keyboard.mode.v1"];
    }
    NSMutableArray *layouts = [NSMutableArray new];
    NSArray<NSNumber *> *pending = [self pendingLayoutIndexes];
    for (DmPlayerSource *s in _entries) if (controller_supported(s.controller)) {
        BOOL unresolved = NO;
        for (NSNumber *index in pending) if ([self source:s matchesLayout:_savedLayouts[index.unsignedIntegerValue]]) unresolved = YES;
        // An unconfirmed device must not replace its saved routes with the disabled preview.
        if (unresolved && s.savedLayoutIndex < 0 && !s.manuallyAssigned) continue;
        NSDictionary *layout = @{@"vendor": s.controller.vendorName ?: @"", @"category": s.controller.productCategory ?: @"",
            @"split": @(s.split), @"player": @(s.player), @"role": @(s.role),
            @"rightPlayer": @(s.rightPlayer), @"rightRole": @(s.rightRole)};
        if ([self validSavedLayout:layout]) [layouts addObject:layout];
    }
    // Keep temporarily absent/unconfirmed devices rather than forgetting them on Play.
    // A manual edit cannot tell us which identical/absent device it replaces. Keep
    // unresolved profiles until the user identifies one or explicitly chooses Forget.
    for (NSNumber *index in pending) [layouts addObject:_savedLayouts[index.unsignedIntegerValue]];
    if (layouts.count <= 32) [defaults setObject:layouts forKey:@"players.controllers.v1"];
}
- (BOOL)validSavedLayout:(id)item {
    if (![item isKindOfClass:NSDictionary.class]) return NO;
    for (NSString *key in @[@"vendor", @"category"])
        if (![item[key] isKindOfClass:NSString.class]) return NO;
    for (NSString *key in @[@"split", @"player", @"role", @"rightPlayer", @"rightRole"]) {
        id v = item[key];
        if (![v isKindOfClass:NSNumber.class] || [v doubleValue] != [v integerValue]) return NO;
        NSInteger n = [v integerValue];
        if ([key isEqualToString:@"player"] || [key isEqualToString:@"rightPlayer"]) { if (n < -1 || n >= DM_PLAYERS) return NO; }
        else if (n != 0 && n != 1) return NO;
    }
    if ([item[@"split"] boolValue] && ![item[@"category"] isEqualToString:@"Nintendo Switch Joy-Con (L/R)"]) return NO;
    return YES;
}
- (BOOL)source:(DmPlayerSource *)s matchesLayout:(NSDictionary *)layout {
    return controller_supported(s.controller) &&
        [(s.controller.vendorName ?: @"") isEqualToString:layout[@"vendor"]] &&
        [(s.controller.productCategory ?: @"") isEqualToString:layout[@"category"]];
}
- (NSArray<NSNumber *> *)pendingLayoutIndexes {
    NSMutableArray *pending = [NSMutableArray new];
    for (NSUInteger i = 0; i < _savedLayouts.count; i++) {
        if ([_discardedLayouts containsIndex:i]) continue;
        BOOL used = NO;
        for (DmPlayerSource *s in _entries) if (s.savedLayoutIndex == (NSInteger)i) { used = YES; break; }
        if (!used) [pending addObject:@(i)];
    }
    return pending;
}
- (NSString *)savedLayoutDescription:(NSUInteger)i {
    if (i >= _savedLayouts.count) return @"";
    NSDictionary *layout = _savedLayouts[i];
    NSString *name = [layout[@"vendor"] length] ? layout[@"vendor"] : layout[@"category"];
    NSInteger p = [layout[@"player"] integerValue], right = [layout[@"rightPlayer"] integerValue];
    NSString *(^player)(NSInteger, NSString *) = ^(NSInteger n, NSString *role) {
        return valid(n) ? [NSString stringWithFormat:@"Player %ld (%@)", (long)n + 1,
            [layout[role] integerValue] == DmPlayerOwn ? @"main input" : @"combined input"] : @"Not used";
    };
    NSString *targets = [layout[@"split"] boolValue] ? [NSString stringWithFormat:@"Left: %@, Right: %@", player(p, @"role"), player(right, @"rightRole")] : player(p, @"role");
    return [NSString stringWithFormat:@"%@ → %@", name.length ? name : @"Controller", targets];
}
- (BOOL)restoreLayout:(NSUInteger)i source:(DmPlayerSource *)s {
    if (!_configuring || i >= _savedLayouts.count || ![[self pendingLayoutIndexes] containsObject:@(i)] ||
        ![self source:s matchesLayout:_savedLayouts[i]] || s.savedLayoutIndex >= 0) return NO;
    NSDictionary *layout = _savedLayouts[i];
    NSInteger p = [layout[@"player"] integerValue], right = [layout[@"rightPlayer"] integerValue];
    DmPlayerRole role = [layout[@"role"] integerValue], rightRole = [layout[@"rightRole"] integerValue];
    BOOL split = [layout[@"split"] boolValue];
    if (split && !combined(s.controller)) return NO;
    // Check the complete pair before changing either half. Never take another source's slot.
    for (int slot = 0; slot < DM_PLAYERS; slot++) {
        NSUInteger owners = _keyboardPlayer == slot && _keyboardRole == DmPlayerOwn;
        for (DmPlayerSource *other in _entries) if (other != s && controller_supported(other.controller)) {
            owners += other.player == slot && other.role == DmPlayerOwn;
            owners += other.split && other.rightPlayer == slot && other.rightRole == DmPlayerOwn;
        }
        owners += p == slot && role == DmPlayerOwn;
        owners += split && right == slot && rightRole == DmPlayerOwn;
        if (owners > 1) { _assignmentError = @"Saved players conflict with current inputs. Change Controls first, then identify this controller again."; return NO; }
    }
    s.split = split; s.player = p; s.role = role; s.rightPlayer = right; s.rightRole = rightRole; s.savedLayoutIndex = i;
    s.automaticallyRestored = NO;
    if (_armedLayout == (NSInteger)i) _armedLayout = -1;
    _assignmentError = nil;
    [self logRoutes:@"restored" source:s];
    return YES;
}
- (void)restoreUniqueLayouts {
    if (!_configuring || _hasConfigured) return;
    // Re-evaluate automatic bindings when another identical model arrives. Arrival
    // order is not identity. Once ambiguous, this saved route requires confirmation
    // for the rest of setup even if one matching device subsequently disconnects.
    for (NSUInteger i = 0; i < _savedLayouts.count; i++) {
        NSUInteger count = 0;
        for (DmPlayerSource *s in _entries) if ([self source:s matchesLayout:_savedLayouts[i]]) count++;
        if (count > 1) [_confirmationRequired addIndex:i];
    }
    for (DmPlayerSource *s in _entries) if (s.automaticallyRestored && !s.manuallyAssigned &&
        s.savedLayoutIndex >= 0 && [_confirmationRequired containsIndex:(NSUInteger)s.savedLayoutIndex]) {
        s.player = s.rightPlayer = -1; s.split = NO; s.savedLayoutIndex = -1; s.automaticallyRestored = NO;
        [self logRoutes:@"ambiguous model; confirm saved setup" source:s];
    }
    for (NSNumber *index in [self pendingLayoutIndexes]) {
        NSUInteger i = index.unsignedIntegerValue, savedCount = 0, connectedCount = 0;
        NSDictionary *layout = _savedLayouts[i]; DmPlayerSource *candidate = nil;
        for (NSDictionary *other in _savedLayouts)
            if ([other[@"vendor"] isEqual:layout[@"vendor"]] && [other[@"category"] isEqual:layout[@"category"]]) savedCount++;
        for (DmPlayerSource *s in _entries) if ([self source:s matchesLayout:layout]) { connectedCount++; candidate = s; }
        // Incomplete descriptors are never sufficient for automatic identification.
        if (savedCount == 1 && connectedCount == 1 && ![_confirmationRequired containsIndex:i] &&
            [layout[@"vendor"] length] && [layout[@"category"] length] && !candidate.manuallyAssigned &&
            [self restoreLayout:i source:candidate]) candidate.automaticallyRestored = YES;
    }
}
- (BOOL)armSavedLayout:(NSUInteger)i {
    if (!_configuring || ![[self pendingLayoutIndexes] containsObject:@(i)]) return NO;
    _armedLayout = _armedLayout == (NSInteger)i ? -1 : (NSInteger)i;
    _assignmentError = nil; [self rebuild]; return YES;
}
- (BOOL)forgetSavedLayout:(NSUInteger)i {
    if (!_configuring || ![[self pendingLayoutIndexes] containsObject:@(i)]) return NO;
    [_discardedLayouts addIndex:i];
    if (_armedLayout == (NSInteger)i) _armedLayout = -1;
    _assignmentError = nil; [self rebuild]; return YES;
}
- (void)restorePressed:(NSButton *)sender { [self armSavedLayout:(NSUInteger)sender.tag]; }
- (void)forgetPressed:(NSButton *)sender { [self forgetSavedLayout:(NSUInteger)sender.tag]; }
- (void)setKeyboardPlayer:(NSInteger)p { [self assignRoute:nil field:DmPlayerAssignmentKeyboard player:p role:_keyboardRole]; }
- (void)setKeyboardRole:(DmPlayerRole)role { [self assignRoute:nil field:DmPlayerAssignmentKeyboard player:_keyboardPlayer role:role]; }
- (void)setActive:(BOOL)active { _active = active; if (!active) _keyboard = (DmPlayerState){0}; [self read]; }
- (void)keyboard:(DmPlayerState)state { if (_active && !_configuring) _keyboard = state; [self read]; }
- (void)sync:(NSArray<GCController *> *)controllers {
    for (DmPlayerSource *s in [_entries copy]) if ([controllers indexOfObjectIdenticalTo:s.controller] == NSNotFound) {
        [self logRoutes:@"disconnect" source:s];
        s.controller.extendedGamepad.valueChangedHandler = nil;
        GCPhysicalInputProfile *p = standard_profile(s.controller);
        for (NSString *key in standard_button_keys()) p.buttons[key].pressedChangedHandler = nil;
        p.dpads[GCInputDirectionPad].valueChangedHandler = nil;
        [_entries removeObjectIdenticalTo:s];
    }
    for (GCController *c in controllers) {
        BOOL found = NO;
        for (DmPlayerSource *s in _entries) if (s.controller == c) { found = YES; break; }
        if (found) continue;
        DmPlayerSource *s = [DmPlayerSource new]; s.controller = c; s.player = s.rightPlayer = -1; s.number = ++_nextNumber; s.savedLayoutIndex = -1;
        // New devices during play stay disabled, rather than silently stealing another player's slot.
        if (controller_supported(c) && _configuring && !_hasConfigured && !_savedLayouts.count) {
            for (int p = 0; p < DM_PLAYERS; p++) {
                if (![self ownCount:p]) { s.player = p; break; }
            }
        }
        [_entries addObject:s];
        [self logRoutes:@"connect" source:s];
        __weak DmMacPlayers *weakSelf = self;
        // GCDevice.h documents main as the default handler queue. Make it explicit and read
        // synchronously: another async hop would lose a quick press/release before HID latches it.
        c.handlerQueue = dispatch_get_main_queue();
        if (c.extendedGamepad) {
            c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) {
                [weakSelf inputChanged:g.controller element:el];
            };
        } else {
            __weak GCController *weakController = c;
            GCPhysicalInputProfile *p = standard_profile(c);
            // Standard profiles do not reliably forward element changes through the
            // aggregate profile callback. Subscribe to the actual native controls.
            for (NSString *key in standard_button_keys())
                p.buttons[key].pressedChangedHandler = ^(GCControllerButtonInput *b, float value, BOOL down) {
                    (void)value; (void)down; [weakSelf inputChanged:weakController element:b];
                };
            p.dpads[GCInputDirectionPad].valueChangedHandler = ^(GCControllerDirectionPad *d, float x, float y) {
                (void)x; (void)y; [weakSelf inputChanged:weakController element:d];
            };
        }
    }
    [self restoreUniqueLayouts];
    if (_routesChanged) _routesChanged();
    [self read];
    if (_configuring) [self rebuild];
}
- (void)inputChanged:(GCController *)c element:(GCControllerElement *)el {
    // Axis movement, including stick jitter and releases, must not replace identification.
    if (_configuring && [el respondsToSelector:@selector(isPressed)] && [(GCControllerButtonInput *)el isPressed]) {
        for (DmPlayerSource *entry in _entries) if (entry.controller == c) {
            GCExtendedGamepad *g = c.extendedGamepad;
            BOOL right = g && (el == g.buttonA || el == g.buttonB || el == g.buttonX || el == g.buttonY ||
                el == g.rightShoulder || el == g.rightTrigger || el == g.buttonMenu || el == g.rightThumbstickButton);
            if (_armedLayout >= 0) {
                if (![self restoreLayout:(NSUInteger)_armedLayout source:entry])
                    _assignmentError = _assignmentError ?: @"That controller does not match the saved model. Press a button on the matching controller.";
                if (_routesChanged) _routesChanged();
                [self rebuild];
            }
            [self identify:[self name:entry side:entry.split ? (right ? 1 : 0) : -1]];
        }
    }
    [self read];
}
- (GCController *)wholeControllerForPlayer:(int)p {
    if (!valid(p) || [self ownCount:p] > 1) return nil;
    GCController *found = nil;
    for (DmPlayerSource *s in _entries) if (s.controller.extendedGamepad && !s.split && s.player == p && (s.role == DmPlayerOwn || s.role == DmPlayerShare)) { if (found) return nil; found = s.controller; }
    return found;
}
- (BOOL)railsForSide:(int)side bits:(uint8_t *)bits {
    *bits = 0;
    // GCDevice has no public physical HID identity. Do not bind the one raw side
    // to a guessed GC object when multiple pairs/individual devices are present.
    NSUInteger count = 0;
    for (DmPlayerSource *s in _entries)
        if (combined(s.controller) || single_side(s, 0) == side) count++;
    return count == 1 && _joyConRails && _joyConRails(side, bits);
}
- (uint32_t)controllerStyleForPlayer:(int)p {
    if (!_nativeSingleJoyCons || !valid(p) ||
        (_keyboardPlayer == p && (_keyboardRole == DmPlayerOwn || _keyboardRole == DmPlayerShare))) return 1;
    NSUInteger count = 0; int foundSide = -1;
    for (DmPlayerSource *s in _entries) if (controller_supported(s.controller)) {
        for (int side = 0; side < (s.split ? 2 : 1); side++) {
            NSInteger player = side ? s.rightPlayer : s.player;
            DmPlayerRole role = side ? s.rightRole : s.role;
            if (player != p || (role != DmPlayerOwn && role != DmPlayerShare) || [self ownCount:p] > 1) continue;
            count++; foundSide = single_side(s, side);
        }
    }
    uint8_t bits;
    return count == 1 && foundSide >= 0 && [self railsForSide:foundSide bits:&bits] ? (foundSide ? 16 : 8) : 1;
}
- (void)read { [self readWithForce:NO]; }
- (void)publish { [self readWithForce:YES]; }
- (void)readWithForce:(BOOL)force {
    DmPlayerState states[DM_PLAYERS] = {0}; uint32_t mask = 0;
    if (valid(_keyboardPlayer) && (_keyboardRole == DmPlayerOwn || _keyboardRole == DmPlayerShare) && [self ownCount:_keyboardPlayer] <= 1) { mask |= 1u << _keyboardPlayer; if (_active && !_configuring) merge(&states[_keyboardPlayer], _keyboard); }
    for (DmPlayerSource *s in _entries) {
        if (!controller_supported(s.controller)) continue;
        for (int side = 0; side < (s.split ? 2 : 1); side++) {
            NSInteger p = side ? s.rightPlayer : s.player;
            DmPlayerRole role = side ? s.rightRole : s.role;
            // Fail closed if callers bypass the assignment seam: conflicting own sources
            // must never silently merge, including any shared input on that invalid slot.
            if (!valid(p) || (role != DmPlayerOwn && role != DmPlayerShare) || [self ownCount:p] > 1) continue;
            mask |= 1u << p;
            if (_active && !_configuring) {
                DmPlayerState state = s.split ? half(s.controller.extendedGamepad, side) :
                    s.controller.extendedGamepad ? whole(s.controller.extendedGamepad) : standard_state(standard_profile(s.controller));
                int joySide = single_side(s, side);
                if (_nativeSingleJoyCons && joySide >= 0) {
                    // Never fall back to outer shoulders when raw rails are unavailable.
                    state.buttons &= ~(uint64_t)(DM_BTN_L | DM_BTN_R | DM_BTN_ZL | DM_BTN_ZR);
                    uint8_t bits;
                    if ([self railsForSide:joySide bits:&bits]) {
                        if (bits & 0x20) state.buttons |= 1ull << 24;
                        if (bits & 0x10) state.buttons |= 1ull << 25;
                    }
                }
                merge(&states[p], state);
            }
        }
    }
    // Normally emit only actual changes; publish also initializes newly attached consumers.
    // Recompute from every live source so a release/disconnect cannot erase
    // another source sharing that player, or leave the disconnected source's buttons/sticks held.
    for (int p = 0; p < DM_PLAYERS; p++) {
        uint32_t style = [self controllerStyleForPlayer:p];
        if (force || _lastStyles[p] != style) { _lastStyles[p] = style; if (_styles) _styles(p, style); }
        if (force || memcmp(&_last[p], &states[p], sizeof states[p])) {
            _last[p] = states[p]; if (_output) _output(p, states[p]);
        }
    }
    if (force || _mask != mask) { _mask = mask; if (_players) _players(mask); }
}
- (NSUInteger)ownCount:(NSInteger)p {
    NSUInteger n = _keyboardPlayer == p && _keyboardRole == DmPlayerOwn;
    for (DmPlayerSource *s in _entries) if (controller_supported(s.controller)) {
        n += s.player == p && s.role == DmPlayerOwn;
        n += s.split && s.rightPlayer == p && s.rightRole == DmPlayerOwn;
    }
    return n;
}
- (NSString *)name:(DmPlayerSource *)s side:(NSInteger)side {
    if (!s) return @"Keyboard";
    NSString *name = side < 0 ? (s.controller.vendorName ?: s.controller.productCategory ?: @"Controller")
                               : (side ? @"Right Joy-Con" : @"Left Joy-Con");
    return [NSString stringWithFormat:@"%@ #%lu", name, (unsigned long)s.number];
}
- (NSString *)description:(NSString *)name player:(NSInteger)p role:(DmPlayerRole)role {
    if (!valid(p)) return [name stringByAppendingString:@": off (Not used)"];
    return [NSString stringWithFormat:@"%@: Player %ld (%@)", name, (long)p + 1, role == DmPlayerOwn ? @"own" : @"shared"];
}
- (NSArray<NSString *> *)descriptionsFor:(DmPlayerSource *)s {
    if (!s) return @[[self description:@"Keyboard" player:_keyboardPlayer role:_keyboardRole]];
    NSMutableArray *lines = [NSMutableArray new];
    [lines addObject:[self description:[self name:s side:s.split ? 0 : -1] player:controller_supported(s.controller) ? s.player : -1 role:s.role]];
    if (s.split) [lines addObject:[self description:[self name:s side:1] player:s.rightPlayer role:s.rightRole]];
    return lines;
}
- (NSArray<NSString *> *)routeDescriptions {
    NSMutableArray *lines = [NSMutableArray new];
    for (DmPlayerSource *s in _entries) [lines addObjectsFromArray:[self descriptionsFor:s]];
    [lines addObjectsFromArray:[self descriptionsFor:nil]];
    return lines;
}
- (void)log:(NSString *)line { if (_routeLog) _routeLog(line); else fprintf(stderr, "players: %s\n", line.UTF8String); }
- (void)logRoutes:(NSString *)event source:(DmPlayerSource *)s {
    for (NSString *line in [self descriptionsFor:s]) [self log:[NSString stringWithFormat:@"%@ — %@", event, line]];
}
- (NSArray<NSString *> *)playerSummaries {
    NSMutableArray *lines = [NSMutableArray new];
    for (int p = 0; p < DM_PLAYERS; p++) {
        NSMutableArray *names = [NSMutableArray new];
        for (DmPlayerSource *s in _entries) if (controller_supported(s.controller)) {
            for (int side = 0; side < (s.split ? 2 : 1); side++) if ((side ? s.rightPlayer : s.player) == p) {
                NSString *name = [self name:s side:s.split ? side : -1];
                [names addObject:name];
            }
        }
        if (_keyboardPlayer == p) [names addObject:@"Keyboard"];
        if (!names.count) continue;
        NSString *inputs = [self ownCount:p] > 1 ? @"Conflict — input blocked; choose separate players" :
            [names componentsJoinedByString:@" + "];
        [lines addObject:[NSString stringWithFormat:@"Player %d: %@", p + 1, inputs]];
    }
    return lines;
}
- (void)identify:(NSString *)name {
    _identifiedName = name;
    _activity.stringValue = [@"Input: " stringByAppendingString:name];
    for (NSView *view in _rows.subviews) if ([view isKindOfClass:NSTextField.class]) {
        NSTextField *label = (NSTextField *)view;
        BOOL selected = [label.stringValue isEqualToString:name];
        label.drawsBackground = selected;
        label.backgroundColor = NSColor.selectedContentBackgroundColor;
        label.textColor = selected ? NSColor.selectedTextColor : NSColor.labelColor;
        // Tab/arrows also identify the keyboard; never scroll away from the native
        // controller control the user is navigating. Physical-device presses reveal rows.
        if (selected && ![name isEqualToString:@"Keyboard"]) [label scrollRectToVisible:label.bounds];
    }
}
- (NSTextField *)label:(NSString *)text frame:(NSRect)frame {
    NSTextField *label = [NSTextField labelWithString:text]; label.frame = frame; [_rows addSubview:label]; return label;
}
- (DmPlayerPicker *)picker:(NSArray<NSString *> *)titles index:(NSInteger)index source:(DmPlayerSource *)source field:(DmPlayerAssignment)field frame:(NSRect)frame {
    DmPlayerPicker *b = [[DmPlayerPicker alloc] initWithFrame:frame pullsDown:NO];
    b.source = source; [b addItemsWithTitles:titles]; [b selectItemAtIndex:index];
    for (NSInteger i = 0; i < b.numberOfItems; i++) [b itemAtIndex:i].representedObject = @(i);
    b.tag = field; b.target = self; b.action = @selector(chosen:); [_rows addSubview:b]; return b;
}
- (NSArray<DmPlayerRoute *> *)logicalRoutes {
    NSMutableArray *routes = [NSMutableArray new];
    NSMutableArray *sources = [_entries mutableCopy];
    [sources addObject:NSNull.null];
    for (id entry in sources) {
        DmPlayerSource *s = entry == NSNull.null ? nil : entry;
        for (int side = 0; side < (s.split ? 2 : 1); side++) {
            DmPlayerRoute *r = [DmPlayerRoute new]; r.source = s;
            r.field = !s ? DmPlayerAssignmentKeyboard : (side ? DmPlayerAssignmentRight : DmPlayerAssignmentWhole);
            r.name = [self name:s side:s.split ? side : -1];
            r.supported = !s || controller_supported(s.controller);
            r.player = s ? (side ? s.rightPlayer : s.player) : _keyboardPlayer;
            r.role = s ? (side ? s.rightRole : s.role) : _keyboardRole;
            [routes addObject:r];
        }
    }
    return routes;
}
- (NSArray<DmPlayerRoute *> *)assignedRoutes {
    NSMutableArray *routes = [NSMutableArray new];
    for (DmPlayerRoute *r in [self logicalRoutes]) if (r.supported && valid(r.player)) [routes addObject:r];
    return routes;
}
- (NSArray<DmPlayerRoute *> *)availableRoutes {
    NSMutableArray *routes = [NSMutableArray new];
    for (DmPlayerRoute *r in [self logicalRoutes]) if (!r.supported || !valid(r.player)) [routes addObject:r];
    return routes;
}
- (void)button:(NSString *)title source:(DmPlayerSource *)source field:(DmPlayerAssignment)field value:(NSInteger)value x:(CGFloat)x y:(CGFloat)y enabled:(BOOL)enabled {
    DmPlayerButton *b = [[DmPlayerButton alloc] initWithFrame:NSMakeRect(x, y, 100, 28)];
    b.title = title; b.bezelStyle = NSBezelStyleRounded; b.source = source;
    b.tag = field; b.value = value; b.target = self; b.action = @selector(pressed:); b.enabled = enabled;
    if (value >= 0) {
        BOOL right = field == DmPlayerAssignmentRightMode;
        NSInteger player = source ? (right ? source.rightPlayer : source.player) : _keyboardPlayer;
        DmPlayerRole role = source ? (right ? source.rightRole : source.role) : _keyboardRole;
        [b setButtonType:NSButtonTypePushOnPushOff];
        b.state = valid(player) && role == value ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [_rows addSubview:b];
}
- (void)pressed:(DmPlayerButton *)sender {
    [self assignSource:sender.source field:sender.tag value:sender.value];
}
- (void)toggleChooser:(id)sender { (void)sender; _choosing = !_choosing; [self rebuild]; }
- (void)row:(DmPlayerSource *)s right:(BOOL)right y:(CGFloat)y {
    NSInteger p = s ? (right ? s.rightPlayer : s.player) : _keyboardPlayer;
    NSString *name = [self name:s side:s.split ? (right ? 1 : 0) : -1];
    BOOL supported = !s || controller_supported(s.controller);
    if (!supported) name = [name stringByAppendingString:@" (unavailable)"];
    NSTextField *label = [self label:name frame:NSMakeRect(8, y + 2, 396, 22)];
    if ([_identifiedName isEqualToString:name]) {
        label.drawsBackground = YES; label.backgroundColor = NSColor.selectedContentBackgroundColor;
        label.textColor = NSColor.selectedTextColor;
        [label scrollRectToVisible:label.bounds];
    }
    DmPlayerAssignment field = !s ? DmPlayerAssignmentKeyboard : (right ? DmPlayerAssignmentRight : DmPlayerAssignmentWhole);
    DmPlayerAssignment mode = !s ? DmPlayerAssignmentKeyboardMode : (right ? DmPlayerAssignmentRightMode : DmPlayerAssignmentWholeMode);
    CGFloat controlsY = y + 26;
    [self label:@"Controls" frame:NSMakeRect(8, controlsY + 4, 60, 22)];
    [self button:@"Remove" source:s field:mode value:-1 x:320 y:controlsY enabled:supported && valid(p)];
    [_rows.subviews.lastObject setFrameSize:NSMakeSize(84, 28)];
    NSArray<NSNumber *> *ids = [self playerChoices];
    NSMutableArray *titles = [NSMutableArray new];
    for (NSNumber *pid in ids) [titles addObject:pid.integerValue == -2 ? @"New player" : [NSString stringWithFormat:@"Player %ld", (long)pid.integerValue + 1]];
    DmPlayerPicker *picker = [self picker:titles index:-1 source:s field:field frame:NSMakeRect(74, controlsY, 236, 28)];
    for (NSUInteger i = 0; i < ids.count; i++) {
        [picker itemAtIndex:i].representedObject = ids[i];
        if (ids[i].integerValue == p) [picker selectItemAtIndex:i];
    }
    picker.enabled = supported;
}
- (void)rebuild {
    NSArray *summaries = [self playerSummaries];
    _summary.stringValue = [NSString stringWithFormat:@"%lu player%@%@%@", (unsigned long)summaries.count,
        summaries.count == 1 ? @"" : @"s", summaries.count ? @"\n" : @"", [summaries componentsJoinedByString:@"\n"]];
    _errorLabel.stringValue = _assignmentError ?: @"";
    if (!_rows) return; // windowless tests use exactly the same model/actions
    for (NSView *v in [_rows.subviews copy]) [v removeFromSuperview];
    CGFloat y = 0;
    for (NSNumber *index in [self pendingLayoutIndexes]) {
        NSUInteger i = index.unsignedIntegerValue;
        NSTextField *saved = [self label:[@"Saved: " stringByAppendingString:[self savedLayoutDescription:i]] frame:NSMakeRect(8, y, 396, 60)];
        saved.maximumNumberOfLines = 3; saved.cell.wraps = YES;
        NSButton *restore = [NSButton buttonWithTitle:_armedLayout == (NSInteger)i ? @"Press controller button… (Cancel)" : @"Identify controller to restore" target:self action:@selector(restorePressed:)];
        restore.tag = (NSInteger)i; restore.frame = NSMakeRect(8, y + 62, 300, 28); [_rows addSubview:restore];
        NSButton *forget = [NSButton buttonWithTitle:@"Forget" target:self action:@selector(forgetPressed:)];
        forget.tag = (NSInteger)i; forget.frame = NSMakeRect(316, y + 62, 88, 28); [_rows addSubview:forget];
        y += 98;
    }
    NSArray *groups = _choosing ? @[[self assignedRoutes], [self availableRoutes]] : @[[self assignedRoutes]];
    NSMutableSet *splitControls = [NSMutableSet new];
    for (NSUInteger group = 0; group < groups.count; group++) {
        if (group) {
            NSTextField *heading = [self label:@"Add input device — unavailable devices cannot be assigned" frame:NSMakeRect(8, y, 396, 40)];
            heading.maximumNumberOfLines = 2;
            heading.cell.wraps = YES;
            y += 48;
        }
        for (DmPlayerRoute *r in groups[group]) {
            DmPlayerSource *s = r.source;
            if (s && combined(s.controller) && ![splitControls containsObject:s]) {
                [splitControls addObject:s];
                [self label:[self name:s side:-1] frame:NSMakeRect(8, y + 5, 240, 24)];
                [self picker:@[@"Together", @"Separate"] index:s.split source:s field:DmPlayerAssignmentSplit frame:NSMakeRect(252, y, 152, 28)].enabled = r.supported;
                y += 38;
            }
            [self row:s right:r.field == DmPlayerAssignmentRight y:y]; y += 64;
        }
    }
    _rows.frame = NSMakeRect(0, 0, 410, y);
    _rowsHeight.constant = MIN(260, MAX(32, y));
    _errorLabel.hidden = !_assignmentError.length;
    [_configuration invalidateIntrinsicContentSize];
    [_configuration setFrameSize:_configuration.fittingSize];
    [_configuration layoutSubtreeIfNeeded];
}
- (NSArray<NSNumber *> *)playerChoices {
    NSMutableIndexSet *active = [NSMutableIndexSet new];
    for (DmPlayerRoute *route in [self assignedRoutes]) [active addIndex:route.player];
    NSMutableArray *choices = [NSMutableArray new];
    [active enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL *stop) { (void)stop; [choices addObject:@(idx)]; }];
    if (active.count < DM_PLAYERS) [choices addObject:@(-2)];
    return choices;
}
- (void)chosen:(NSControl<DmPlayerControl> *)sender {
    id value = [(NSPopUpButton *)sender selectedItem].representedObject;
    if (![value isKindOfClass:NSNumber.class]) return;
    [self assignSource:sender.source field:sender.tag value:[value integerValue]];
}
- (void)assignSource:(DmPlayerSource *)source field:(DmPlayerAssignment)field value:(NSInteger)value {
    // Popups retain source objects, never row numbers. A disconnected object's queued
    // action cannot affect its replacement, even if the very same controller reconnects.
    if (source && [_entries indexOfObjectIdenticalTo:source] == NSNotFound) return;
    _armedLayout = -1;
    BOOL keyboard = field == DmPlayerAssignmentKeyboard || field == DmPlayerAssignmentKeyboardMode;
    if ((keyboard && source) || (!keyboard && (!source || !controller_supported(source.controller)))) return;
    if (field == DmPlayerAssignmentSplit) {
        if (!combined(source.controller) || (value != 0 && value != 1)) return;
        BOOL old = source.split; source.split = value;
        for (int p = 0; p < DM_PLAYERS; p++) if ([self ownCount:p] > 1) {
            source.split = old; [self conflict:p source:source]; return;
        }
        source.manuallyAssigned = YES; source.automaticallyRestored = NO;
        if (old != source.split) {
            // Name the routes that disappeared as well as their replacements.
            for (int side = 0; side < (old ? 2 : 1); side++)
                [self log:[@"assignment — " stringByAppendingString:[self description:[self name:source side:old ? side : -1] player:-1 role:DmPlayerOwn]]];
        }
        _assignmentError = nil; [self changed:source]; return;
    }
    BOOL mode = field == DmPlayerAssignmentKeyboardMode || field == DmPlayerAssignmentWholeMode || field == DmPlayerAssignmentRightMode;
    BOOL right = field == DmPlayerAssignmentRight || field == DmPlayerAssignmentRightMode;
    if (!keyboard && !mode && field != DmPlayerAssignmentWhole && field != DmPlayerAssignmentRight) return;
    NSInteger p = keyboard ? _keyboardPlayer : (right ? source.rightPlayer : source.player);
    DmPlayerRole role = keyboard ? _keyboardRole : (right ? source.rightRole : source.role);
    if (mode) {
        if (value < -1 || value > DmPlayerShare) return;
        if (value < 0) p = -1;
        else {
            role = value;
            if (!valid(p)) {
                p = 0;
                if (role == DmPlayerOwn) { while (p < DM_PLAYERS && [self ownCount:p]) p++; if (!valid(p)) p = -1; }
                if (!valid(p)) { _assignmentError = @"No free player. Choose the same player as another device or remove an input."; [self rebuild]; return; }
            }
        }
    } else {
        [self selectPlayer:value source:source field:field]; return;
    }
    [self assignRoute:source field:keyboard ? DmPlayerAssignmentKeyboard : (right ? DmPlayerAssignmentRight : DmPlayerAssignmentWhole) player:p role:role];
}
- (void)conflict:(NSInteger)p source:(DmPlayerSource *)s {
    _assignmentError = [NSString stringWithFormat:@"Player %ld already has a primary input. Select its player in Controls to use the same player. Previous assignment kept.", (long)p + 1];
    [self log:[NSString stringWithFormat:@"assignment rejected — %@; %@", [self name:s side:-1], _assignmentError]];
    [self logRoutes:@"unchanged" source:s];
    [self rebuild];
}
- (void)changed:(DmPlayerSource *)s {
    if (!s) _keyboard = (DmPlayerState){0};
    [self logRoutes:@"assignment" source:s];
    if (_routesChanged) _routesChanged(); [self read]; [self rebuild];
}
- (BOOL)selectPlayer:(NSInteger)p source:(DmPlayerSource *)s field:(DmPlayerAssignment)field {
    // Reject stale objects before resolving New or consulting current ownership.
    if (field == DmPlayerAssignmentKeyboard) { if (s) return NO; }
    else if ((field != DmPlayerAssignmentWhole && field != DmPlayerAssignmentRight) || !s ||
             [_entries indexOfObjectIdenticalTo:s] == NSNotFound || !controller_supported(s.controller) ||
             (field == DmPlayerAssignmentRight && !s.split)) return NO;
    if (p == -2) {
        NSArray *choices = [self playerChoices];
        for (p = 0; p < DM_PLAYERS && [choices containsObject:@(p)]; p++) {}
        if (!valid(p)) { _assignmentError = @"No free player. Choose the same player as another device or remove an input."; [self rebuild]; return NO; }
    }
    if (p != -1 && !valid(p)) return NO;
    BOOL right = field == DmPlayerAssignmentRight;
    NSInteger previous = s ? (right ? s.rightPlayer : s.player) : _keyboardPlayer;
    DmPlayerRole previousRole = s ? (right ? s.rightRole : s.role) : _keyboardRole;
    NSUInteger otherOwners = valid(p) ? [self ownCount:p] : 0;
    if (p == previous && previousRole == DmPlayerOwn && otherOwners) otherOwners--;
    return [self assignRoute:s field:field player:p role:otherOwners ? DmPlayerShare : DmPlayerOwn];
}
- (BOOL)assignRoute:(DmPlayerSource *)s field:(DmPlayerAssignment)field player:(NSInteger)p role:(DmPlayerRole)role {
    if ((p != -1 && !valid(p)) || (role != DmPlayerOwn && role != DmPlayerShare)) return NO;
    if (field == DmPlayerAssignmentKeyboard) { if (s) return NO; }
    else if ((field != DmPlayerAssignmentWhole && field != DmPlayerAssignmentRight) || !s ||
             [_entries indexOfObjectIdenticalTo:s] == NSNotFound || !controller_supported(s.controller)) return NO;
    BOOL right = field == DmPlayerAssignmentRight;
    NSInteger oldPlayer = s ? (right ? s.rightPlayer : s.player) : _keyboardPlayer;
    DmPlayerRole oldRole = s ? (right ? s.rightRole : s.role) : _keyboardRole;
    if (!s) { _keyboardPlayer = p; _keyboardRole = role; }
    else if (right) { s.rightPlayer = p; s.rightRole = role; }
    else { s.player = p; s.role = role; }
    if (valid(p) && [self ownCount:p] > 1) {
        if (!s) { _keyboardPlayer = oldPlayer; _keyboardRole = oldRole; }
        else if (right) { s.rightPlayer = oldPlayer; s.rightRole = oldRole; }
        else { s.player = oldPlayer; s.role = oldRole; }
        [self conflict:p source:s]; return NO;
    }
    if (s) { s.manuallyAssigned = YES; s.automaticallyRestored = NO; }
    _assignmentError = nil; [self changed:s]; return YES;
}
- (BOOL)beginConfiguration {
    if (_configuring) return NO;
    _configuring = YES; _choosing = NO; _keyboard = (DmPlayerState){0};
    _connectionObservers = [NSMutableArray new];
    __weak DmMacPlayers *weakSelf = self;
    for (NSNotificationName name in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification]) {
        id observer = [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            NSMutableArray *controllers = [GCController.controllers mutableCopy];
            id disconnected = note.object;
            if ([note.name isEqualToString:GCControllerDidDisconnectNotification] && disconnected) [controllers removeObjectIdenticalTo:disconnected];
            [weakSelf sync:controllers];
        }];
        [_connectionObservers addObject:observer];
    }
    [self installKeyMonitor]; [self read]; [self rebuild]; return YES;
}
- (void)endConfiguration {
    if (!_configuring) return;
    _configuring = NO; _hasConfigured = YES; _armedLayout = -1;
    for (id observer in _connectionObservers) [NSNotificationCenter.defaultCenter removeObserver:observer];
    _connectionObservers = nil;
    if (_keyMonitor) { [NSEvent removeMonitor:_keyMonitor]; _keyMonitor = nil; }
    _identifiedName = nil;
    for (NSString *line in [self routeDescriptions]) [self log:[@"confirm — " stringByAppendingString:line]];
    for (NSString *line in [self playerSummaries]) [self log:[@"summary — " stringByAppendingString:line]];
    [self read];
}
- (void)commitPreferences {
    if (!_remember || !_preferences) return;
    if (_remember.state == NSControlStateValueOn) [self savePreferences:_preferences];
    else for (NSString *key in @[@"players.controllers.v1", @"players.keyboard.v1", @"players.keyboard.mode.v1"])
        [_preferences removeObjectForKey:key];
}
- (NSView *)configurationView {
    if (_configuration) return _configuration;
    NSStackView *box = [NSStackView new];
    box.orientation = NSUserInterfaceLayoutOrientationVertical;
    box.alignment = NSLayoutAttributeLeading; box.spacing = 6;
    box.edgeInsets = NSEdgeInsetsMake(4, 8, 4, 8);
    _configuration = box;
    _summary = [NSTextField wrappingLabelWithString:@""];
    _summary.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    [box addArrangedSubview:_summary];
    NSTextField *explanation = [NSTextField wrappingLabelWithString:@"Choose the same player to combine inputs, or New player to play separately."];
    [box addArrangedSubview:explanation];
    NSButton *add = [NSButton buttonWithTitle:@"Add input device" target:self action:@selector(toggleChooser:)];
    [box addArrangedSubview:add];
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = NO;
    scroll.scrollerStyle = NSScrollerStyleOverlay;
    _rows = [DmPlayerRows new]; scroll.documentView = _rows;
    [box addArrangedSubview:scroll];
    _rowsHeight = [scroll.heightAnchor constraintEqualToConstant:32]; _rowsHeight.active = YES;
    _activity = [NSTextField labelWithString:@"Input: press a button to identify a device"];
    [box addArrangedSubview:_activity];
    _errorLabel = [NSTextField wrappingLabelWithString:@""]; _errorLabel.textColor = NSColor.systemRedColor;
    [box addArrangedSubview:_errorLabel];
    _remember = [NSButton checkboxWithTitle:@"Remember player setup" target:nil action:nil];
    _remember.state = _rememberSetup ? NSControlStateValueOn : NSControlStateValueOff;
    _remember.enabled = _preferences != nil; [box addArrangedSubview:_remember];
    NSTextField *footer = [NSTextField wrappingLabelWithString:@"Remembered models restore when unique. For identical or unclear devices, identify the saved setup by pressing a controller button. Separate Joy-Cons use sideways input, not Bluetooth unpairing."];
    footer.textColor = NSColor.secondaryLabelColor; footer.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    [box addArrangedSubview:footer];
    for (NSView *view in box.arrangedSubviews) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [view.widthAnchor constraintEqualToConstant:414].active = YES;
    }
    [self rebuild];
    [self installKeyMonitor];
    return box;
}
- (void)installKeyMonitor {
    if (_keyMonitor || !_configuration || !_configuring) return;
    __weak DmMacPlayers *weakSelf = self;
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
        DmMacPlayers *owner = weakSelf;
        if (owner.configuring && owner.configuration.window && event.window == owner.configuration.window) [owner identify:@"Keyboard"];
        return event;
    }];
}
- (void)dealloc {
    for (id observer in _connectionObservers) [NSNotificationCenter.defaultCenter removeObserver:observer];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}
@end
