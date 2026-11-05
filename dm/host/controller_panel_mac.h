// Native runtime UI. The host already owns the devices, so no input or runtime
// settings are routed back through the prelaunch wrapper process.
#pragma once
#import <AppKit/AppKit.h>
#include "controller_requirements_apple.h"

static NSArray<NSString *> *controller_style_tokens(void) { return @[@"auto", @"pro", @"pair", @"left", @"right"]; }
static NSArray<NSString *> *controller_style_labels(void) { return @[@"Automatic", @"Pro Controller", @"Joy-Con pair", @"Joy-Con (L)", @"Joy-Con (R)"]; }

@interface DmControllerPopup : NSPopUpButton
@property(copy) NSString *deviceID;
@property BOOL styleChoice;
@end
@implementation DmControllerPopup
@end

@interface DmControllerFields : NSStackView @end
@implementation DmControllerFields
- (BOOL)isFlipped { return YES; }
@end

@interface DmControllerWindow : NSPanel @end
@implementation DmControllerWindow
- (void)performClose:(id)sender {
    if (self.sheetParent) [self.sheetParent endSheet:self returnCode:NSModalResponseCancel];
    else [super performClose:sender];
}
- (void)cancelOperation:(id)sender { [self performClose:sender]; }
@end

@interface DmControllerPanel : NSWindowController <NSWindowDelegate>
@property(copy) NSDictionary *(^snapshot)(void);
@property(copy) BOOL (^identify)(NSString *);
@property(copy) dm_controller_requirements (^requirements)(void);
@property(copy) NSArray *(^resolveChoices)(NSArray *);
@property(copy) NSString *(^applyChoices)(NSArray *, int, int);
@property int minimum, maximum;
@property BOOL recommend;
@property uint32_t recommendationStyles;
@property NSMutableSet *recommendationSeen, *signaled;
@property NSStackView *fields;
@property NSTextField *requirementHeading;
@property NSTextField *requirementDetails;
@property NSTextField *statusDetail;
@property NSTextField *message;
@property NSTextField *operationError;
@property dm_controller_requirements displayedRequirements;
@property NSTimer *requirementsTimer;
@property NSButton *applyButton;
@property NSArray *rows;
@property NSMutableDictionary<NSString *, NSDictionary *> *drafts;
@property id parentCloseObserver;
- (void)refresh;
- (NSArray *)choices;
- (void)apply:(id)sender;
- (void)cancel:(id)sender;
@end

@implementation DmControllerPanel
// Keep the sheet native so colors, controls and text follow macOS appearance.
- (NSTextField *)label:(NSString *)text size:(CGFloat)size weight:(NSFontWeight)weight {
    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:size weight:weight];
    return label;
}
- (NSStackView *)column:(NSArray<NSView *> *)views spacing:(CGFloat)spacing {
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading; stack.spacing = spacing;
    for (NSView *view in views) [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
    return stack;
}
- (NSBox *)card:(NSView *)content {
    NSBox *box = [NSBox new]; box.boxType = NSBoxCustom; box.titlePosition = NSNoTitle;
    box.borderWidth = 0.5; box.cornerRadius = 10;
    box.borderColor = NSColor.separatorColor; box.fillColor = NSColor.controlBackgroundColor;
    box.contentViewMargins = NSMakeSize(16, 14);
    NSView *inside = box.contentView; [inside addSubview:content]; content.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:inside.leadingAnchor], [content.trailingAnchor constraintEqualToAnchor:inside.trailingAnchor],
        [content.topAnchor constraintEqualToAnchor:inside.topAnchor], [content.bottomAnchor constraintEqualToAnchor:inside.bottomAnchor]
    ]];
    return box;
}
- (instancetype)init {
    NSPanel *window = [[DmControllerWindow alloc] initWithContentRect:NSMakeRect(0, 0, 660, 660)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    if (!(self = [super initWithWindow:window])) return nil;
    window.title = @"Controllers"; window.delegate = self; window.releasedWhenClosed = NO;
    self.recommendationSeen = [NSMutableSet new]; self.signaled = [NSMutableSet new];
    self.drafts = [NSMutableDictionary new]; self.minimum = 0; self.maximum = 8;
    NSView *root = window.contentView;
    NSTextField *title = [self label:@"Controllers" size:24 weight:NSFontWeightBold];
    NSTextField *subtitle = [self label:@"Choose how the game sees each controller." size:13 weight:NSFontWeightRegular];
    subtitle.textColor = NSColor.secondaryLabelColor;
    NSStackView *heading = [self column:@[title, subtitle] spacing:5];
    NSTextField *requirementsTitle = [self label:@"GAME REQUIREMENTS" size:10 weight:NSFontWeightSemibold];
    requirementsTitle.textColor = NSColor.secondaryLabelColor;
    self.requirementHeading = [self label:@"" size:17 weight:NSFontWeightSemibold];
    self.requirementDetails = [self label:@"" size:13 weight:NSFontWeightRegular];
    NSBox *requirements = [self card:[self column:@[requirementsTitle, self.requirementHeading, self.requirementDetails] spacing:8]];
    NSTextField *devicesTitle = [self label:@"Connected controllers" size:13 weight:NSFontWeightSemibold];
    NSScrollView *scroll = [NSScrollView new]; scroll.hasVerticalScroller = YES; scroll.drawsBackground = NO;
    self.fields = [DmControllerFields new]; self.fields.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.fields.alignment = NSLayoutAttributeLeading; self.fields.spacing = 12;
    self.fields.edgeInsets = NSEdgeInsetsMake(2, 0, 8, 0);
    scroll.documentView = self.fields;
    self.message = [self label:@"" size:14 weight:NSFontWeightSemibold];
    self.statusDetail = [self label:@"" size:12 weight:NSFontWeightRegular]; self.statusDetail.textColor = NSColor.secondaryLabelColor;
    self.operationError = [self label:@"" size:12 weight:NSFontWeightRegular];
    self.operationError.textColor = NSColor.systemRedColor; self.operationError.hidden = YES;
    NSStackView *status = [self column:@[self.message, self.statusDetail, self.operationError] spacing:5];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)]; cancel.keyEquivalent = @"\e";
    self.applyButton = [NSButton buttonWithTitle:@"Apply" target:self action:@selector(apply:)]; self.applyButton.keyEquivalent = @"\r";
    NSStackView *buttons = [NSStackView stackViewWithViews:@[cancel, self.applyButton]]; buttons.spacing = 8;
    NSBox *divider = [NSBox new]; divider.boxType = NSBoxSeparator;
    for (NSView *view in @[heading, requirements, devicesTitle, scroll, divider, status, buttons, self.fields]) view.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView *view in @[heading, requirements, devicesTitle, scroll, divider, status, buttons]) [root addSubview:view];
    [NSLayoutConstraint activateConstraints:@[
        [heading.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:24], [heading.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-24],
        [heading.topAnchor constraintEqualToAnchor:root.topAnchor constant:24],
        [requirements.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor], [requirements.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [requirements.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:20],
        [devicesTitle.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor], [devicesTitle.topAnchor constraintEqualToAnchor:requirements.bottomAnchor constant:22],
        [scroll.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:devicesTitle.bottomAnchor constant:10], [scroll.bottomAnchor constraintEqualToAnchor:divider.topAnchor constant:-16],
        [divider.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor], [divider.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor],
        [divider.bottomAnchor constraintEqualToAnchor:status.topAnchor constant:-18],
        [status.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor], [status.trailingAnchor constraintEqualToAnchor:buttons.leadingAnchor constant:-20],
        [status.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-24],
        [buttons.widthAnchor constraintEqualToConstant:160],
        [status.heightAnchor constraintGreaterThanOrEqualToConstant:44],
        [buttons.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor], [buttons.centerYAnchor constraintEqualToAnchor:status.centerYAnchor],
        [self.fields.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [self.fields.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor], [self.fields.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor]
    ]];
    return self;
}
- (NSArray *)choices {
    NSMutableArray *choices = [NSMutableArray new];
    for (NSDictionary *row in self.rows) if (self.drafts[row[@"id"]]) [choices addObject:self.drafts[row[@"id"]]];
    return choices;
}
- (NSArray *)resolved {
    if (self.resolveChoices) return self.resolveChoices(self.choices);
    NSMutableSet *used = [NSMutableSet new];
    for (NSDictionary *choice in self.choices) if ([choice[@"player"] intValue] > 0) [used addObject:choice[@"player"]];
    NSMutableArray *result = [NSMutableArray new];
    for (NSDictionary *choice in self.choices) {
        NSMutableDictionary *row = [choice mutableCopy];
        if ([row[@"player"] intValue] == -1) {
            row[@"player"] = @0;
            for (int p = 1; p <= DM_PLAYERS; p++) if (![used containsObject:@(p)]) { row[@"player"] = @(p); [used addObject:@(p)]; break; }
        }
        row[@"automaticStyle"] = @"pro";
        if ([row[@"style"] isEqual:@"auto"]) row[@"style"] = @"pro";
        [result addObject:row];
    }
    return result;
}
- (void)validity {
    dm_controller_requirements game = self.displayedRequirements;
    BOOL keyboard = NO;
    int count = controller_ready_players(game, self.resolved, &keyboard);
    self.applyButton.enabled = count >= self.minimum && count <= self.maximum;
    NSMutableArray *details = [NSMutableArray new];
    if (self.minimum > self.maximum) {
        [details addObject:@"The game requested conflicting player counts."];
    } else if (self.minimum == self.maximum) {
        [details addObject:[NSString stringWithFormat:@"%d player%@ required", self.minimum, self.minimum == 1 ? @"" : @"s"]];
    } else if (!self.minimum && self.maximum == DM_PLAYERS) {
        [details addObject:@"No player count requested"];
    } else if (!self.minimum) {
        [details addObject:[NSString stringWithFormat:@"Up to %d player%@", self.maximum, self.maximum == 1 ? @"" : @"s"]];
    } else {
        [details addObject:[NSString stringWithFormat:@"%d–%d players required", self.minimum, self.maximum]];
    }
    self.requirementHeading.stringValue = details.firstObject;
    [details removeAllObjects];
    NSArray *tokens = controller_style_tokens(), *labels = controller_style_labels();
    NSMutableArray *types = [NSMutableArray new], *slots = [NSMutableArray new];
    for (NSUInteger i = 0; i < tokens.count; i++) if (game.styles & cs_style(tokens[i])) [types addObject:labels[i]];
    [details addObject:types.count ? [NSString stringWithFormat:@"Controller type   %@", [types componentsJoinedByString:@" or "]] : @"No external controller types accepted"];
    for (int p = 1; p <= DM_PLAYERS; p++) if (controller_player_supported(game, p)) [slots addObject:@(p).stringValue];
    if (slots.count < DM_PLAYERS)
        [details addObject:slots.count ? [NSString stringWithFormat:@"Player slots          %@", [slots componentsJoinedByString:@", "]] : @"No external player slots available"];
    NSMutableParagraphStyle *paragraph = [NSMutableParagraphStyle new]; paragraph.lineSpacing = 7;
    self.requirementDetails.attributedStringValue = [[NSAttributedString alloc] initWithString:[details componentsJoinedByString:@"\n"]
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13], NSForegroundColorAttributeName: NSColor.labelColor, NSParagraphStyleAttributeName: paragraph}];

    NSString *status, *detail;
    if (self.minimum > self.maximum) { status = @"Setup unavailable"; detail = @"Cancel this setup and try again."; }
    else if (count < self.minimum) {
        status = [NSString stringWithFormat:@"%d of %d players ready", count, self.minimum];
        detail = @"Assign the required players to continue.";
    } else if (count > self.maximum) {
        int extra = count - self.maximum;
        status = [NSString stringWithFormat:@"%d players ready · Too many", count];
        detail = [NSString stringWithFormat:@"Unassign %d controller%@ to continue.", extra, extra == 1 ? @"" : @"s"];
    } else { status = [NSString stringWithFormat:@"%d player%@ ready", count, count == 1 ? @"" : @"s"]; detail = @"Changes apply to this session."; }
    if (keyboard) detail = [detail stringByAppendingString:@" Player 1’s keyboard counts."];
    self.message.stringValue = status;
    self.statusDetail.stringValue = detail;
    self.operationError.hidden = YES;
}
- (void)refreshRequirements {
    dm_controller_requirements now = self.requirements ? self.requirements() : (dm_controller_requirements){0};
    dm_controller_requirements shown = self.displayedRequirements;
    if (now.styles != shown.styles || now.ids != shown.ids || now.fallback != shown.fallback) [self refresh];
}
- (void)refresh {
    self.displayedRequirements = self.requirements ? self.requirements() : (dm_controller_requirements){0};
    NSResponder *responder = self.window.firstResponder;
    NSString *focus = [responder isKindOfClass:NSView.class] ? [(NSView *)responder identifier] : nil;
    NSDictionary *state = self.snapshot ? self.snapshot() : @{};
    self.rows = state[@"controllers"] ?: @[];
    NSMutableDictionary *seed = [NSMutableDictionary new], *next = [NSMutableDictionary new];
    for (NSDictionary *choice in state[@"choices"]) seed[choice[@"id"]] = choice;
    NSMutableSet *used = [NSMutableSet new];
    for (NSDictionary *row in self.rows) {
        NSString *key = row[@"id"]; NSDictionary *choice = self.drafts[key];
        if (choice) { next[key] = choice; if ([choice[@"player"] intValue] > 0) [used addObject:choice[@"player"]]; }
    }
    for (NSDictionary *row in self.rows) {
        NSString *key = row[@"id"]; if (next[key]) continue;
        NSMutableDictionary *choice = [seed[key] mutableCopy] ?: [@{@"id": key, @"player": @(-1), @"style": @"auto"} mutableCopy];
        if ([choice[@"player"] intValue] > 0 && [used containsObject:choice[@"player"]]) choice[@"player"] = @0;
        if ([choice[@"player"] intValue] > 0) [used addObject:choice[@"player"]];
        next[key] = choice;
    }
    self.drafts = next;
    if (self.recommendationStyles != self.displayedRequirements.styles) {
        self.recommendationStyles = self.displayedRequirements.styles;
        [self.recommendationSeen removeAllObjects];
    }
    if (self.recommend) for (NSDictionary *row in self.resolved) {
        NSString *key = row[@"id"];
        if ([self.recommendationSeen containsObject:key]) continue;
        [self.recommendationSeen addObject:key];
        if (![row[@"player"] intValue] || (self.displayedRequirements.styles & cs_style(row[@"style"]))) continue;
        NSString *style = controller_recommended_style(self.displayedRequirements.styles, [row[@"player"] intValue] - 1);
        if (style) {
            NSMutableDictionary *choice = [self.drafts[key] mutableCopy];
            choice[@"style"] = style; self.drafts[key] = choice;
        }
    }
    NSMutableDictionary *resolved = [NSMutableDictionary new];
    for (NSDictionary *choice in self.resolved) resolved[choice[@"id"]] = choice;
    for (NSView *view in self.fields.arrangedSubviews) { [self.fields removeArrangedSubview:view]; [view removeFromSuperview]; }
    if (!self.rows.count) {
        NSTextField *empty = [NSTextField wrappingLabelWithString:@"No controllers connected. Connect one in macOS Bluetooth settings or with a USB cable."];
        empty.textColor = NSColor.secondaryLabelColor; [self.fields addView:empty inGravity:NSStackViewGravityTop];
        [empty.widthAnchor constraintEqualToAnchor:self.fields.widthAnchor].active = YES;
    }
    NSUInteger index = 0;
    for (NSDictionary *device in self.rows) {
        NSString *key = device[@"id"]; NSDictionary *choice = self.drafts[key];
        ++index;
        DmControllerPopup *player = [[DmControllerPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
        player.deviceID = key; player.identifier = [@"player." stringByAppendingString:key];
        NSDictionary *effective = resolved[key];
        NSString *automaticPlayer = [effective[@"player"] intValue] > 0
            ? [NSString stringWithFormat:@"Player %@ • Automatic", effective[@"player"]] : @"Unassigned • Automatic";
        [player addItemWithTitle:automaticPlayer]; player.lastItem.tag = -1;
        [player addItemWithTitle:@"Unassigned"]; player.lastItem.tag = 0;
        for (int p = 1; p <= 8; p++) { [player addItemWithTitle:[NSString stringWithFormat:@"Player %d", p]]; player.lastItem.tag = p; }
        [player selectItemWithTag:[choice[@"player"] intValue]]; player.target = self; player.action = @selector(choiceChanged:);
        DmControllerPopup *style = [[DmControllerPopup alloc] initWithFrame:NSZeroRect pullsDown:NO];
        style.deviceID = key; style.styleChoice = YES; style.identifier = [@"style." stringByAppendingString:key];
        NSArray *tokens = controller_style_tokens(), *labels = controller_style_labels();
        for (NSUInteger i = 0; i < tokens.count; i++) {
            NSString *title = labels[i];
            if (i == 0) title = [NSString stringWithFormat:@"%@ • Automatic", [effective[@"automaticStyle"] isEqual:@"handheld"] ? @"Handheld" : @"Pro Controller"];
            else if (self.displayedRequirements.styles & cs_style(tokens[i])) title = [title stringByAppendingString:@" • Recommended"];
            [style addItemWithTitle:title]; style.lastItem.representedObject = tokens[i]; if ([choice[@"style"] isEqual:tokens[i]]) [style selectItem:style.lastItem]; }
        style.target = self; style.action = @selector(choiceChanged:);
        NSTextField *name = [self label:[NSString stringWithFormat:@"%@ · %lu", device[@"name"] ?: @"Controller", (unsigned long)index] size:14 weight:NSFontWeightSemibold];
        name.maximumNumberOfLines = 1; name.lineBreakMode = NSLineBreakByTruncatingTail;
        NSButton *identify = [NSButton buttonWithTitle:[self.signaled containsObject:key] ? @"Identify • signal sent" : @"Identify" target:self action:@selector(identifyDevice:)];
        identify.identifier = key;
        [identify setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
        int assigned = [effective[@"player"] intValue];
        dm_controller_requirements game = self.displayedRequirements;
        NSString *warning = nil;
        BOOL slotSupported = assigned == 1 && [effective[@"style"] isEqual:@"handheld"]
            ? (game.ids & (1u << 8)) != 0 : controller_player_supported(game, assigned);
        if (assigned && !slotSupported) warning = @"Choose a player slot listed in the game requirements.";
        else if (assigned && !(game.styles & cs_style(effective[@"style"]))) warning = @"Change “Game sees” to a type listed above.";
        if (warning) name.stringValue = [name.stringValue stringByAppendingString:@" • Not ready"];
        NSTextField *badge = [self label:assigned && !warning ? @"✓ Ready" : !assigned ? @"Unassigned" : @"" size:11 weight:NSFontWeightMedium];
        badge.textColor = NSColor.secondaryLabelColor;
        [badge setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSView *space = [NSView new]; [space setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSStackView *header = [NSStackView stackViewWithViews:@[name, space, badge, identify]];
        header.spacing = 12; header.alignment = NSLayoutAttributeCenterY;
        NSTextField *playerLabel = [self label:@"Player" size:11 weight:NSFontWeightMedium]; playerLabel.textColor = NSColor.secondaryLabelColor;
        NSTextField *styleLabel = [self label:@"Game sees" size:11 weight:NSFontWeightMedium]; styleLabel.textColor = NSColor.secondaryLabelColor;
        NSStackView *playerField = [self column:@[playerLabel, player] spacing:5];
        NSStackView *styleField = [self column:@[styleLabel, style] spacing:5];
        NSStackView *controls = [NSStackView stackViewWithViews:@[playerField, styleField]];
        controls.spacing = 16; controls.distribution = NSStackViewDistributionFillEqually;
        NSStackView *content = [self column:@[header, controls] spacing:14];
        if (warning) {
            NSTextField *hint = [self label:warning size:12 weight:NSFontWeightRegular];
            hint.textColor = NSColor.secondaryLabelColor;
            [content addArrangedSubview:hint]; [hint.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
        }
        NSBox *box = [self card:content];
        [self.fields addView:box inGravity:NSStackViewGravityTop]; [box.widthAnchor constraintEqualToAnchor:self.fields.widthAnchor].active = YES;
        for (NSView *control in @[player, style, identify]) if ([control.identifier isEqual:focus]) [self.window makeFirstResponder:control];
    }
    [self validity];
}
- (void)identifyDevice:(NSButton *)sender {
    BOOL ok = self.identify && self.identify(sender.identifier);
    if (ok) {
        [self.signaled addObject:sender.identifier];
        sender.title = @"Identify • signal sent";
        [self validity];
        return;
    }
    self.operationError.stringValue = @"This controller couldn’t be signaled.";
    self.operationError.hidden = NO;
}
- (void)choiceChanged:(DmControllerPopup *)sender {
    NSString *key = sender.deviceID; NSMutableDictionary *choice = [self.drafts[key] mutableCopy]; if (!choice) return;
    if (sender.styleChoice) choice[@"style"] = sender.selectedItem.representedObject;
    else {
        NSInteger player = sender.selectedTag;
        if (player > 0) for (NSString *other in self.drafts.allKeys) if (![other isEqual:key] && [self.drafts[other][@"player"] integerValue] == player) {
            NSMutableDictionary *displaced = [self.drafts[other] mutableCopy]; displaced[@"player"] = choice[@"player"]; self.drafts[other] = displaced;
        }
        choice[@"player"] = @(player);
    }
    self.drafts[key] = choice;
    [self refresh];
}
- (void)apply:(id)sender {
    (void)sender;
    NSString *error = self.applyChoices ? self.applyChoices(self.choices, self.minimum, self.maximum) : @"Controller setup is unavailable.";
    if (error) { [self refresh]; self.operationError.stringValue = error; self.operationError.hidden = NO; return; }
    [self.window.sheetParent endSheet:self.window returnCode:NSModalResponseOK];
}
- (void)cancel:(id)sender { (void)sender; [self.window.sheetParent endSheet:self.window returnCode:NSModalResponseCancel]; }
- (BOOL)windowShouldClose:(NSWindow *)sender { (void)sender; [self cancel:nil]; return NO; }
@end

// Both routes share this coordinator and the same sheet. A guest caller waits on
// its own thread. The main queue stays free for hotplug, input and Apply callbacks.
@interface DmControllerUI : NSObject
@property(weak) NSWindow *parent;
@property(copy) NSDictionary *(^snapshot)(void);
@property(copy) BOOL (^identify)(NSString *);
@property(copy) dm_controller_requirements (^requirements)(void);
@property(copy) NSArray *(^resolveChoices)(NSArray *);
@property(copy) NSString *(^applyChoices)(NSArray *, int, int);
@property(copy) void (^didOpen)(void);
@property(copy) void (^didClose)(void);
@property DmControllerPanel *panel;
@property NSMutableArray *waiters;
- (void)showFromMenu:(id)sender;
- (void)openMinimum:(int)minimum maximum:(int)maximum completion:(void (^)(BOOL))completion;
- (int)requestMinimum:(int)minimum maximum:(int)maximum;
- (void)refresh;
@end

@implementation DmControllerUI
- (void)showFromMenu:(id)sender {
    (void)sender;
    if (self.parent.miniaturized) [self.parent deminiaturize:nil];
    if (!self.panel) [self.parent makeKeyAndOrderFront:nil];
    [self openMinimum:0 maximum:8 completion:nil];
}
- (void)openMinimum:(int)minimum maximum:(int)maximum completion:(void (^)(BOOL))completion {
    if (!self.parent) { if (completion) completion(NO); return; }
    if (!self.waiters) self.waiters = [NSMutableArray new];
    if (completion) [self.waiters addObject:[completion copy]];
    minimum = MAX(0, MIN(8, minimum)); maximum = maximum > 0 ? MIN(8, maximum) : 8;
    if (self.panel) {
        if (completion) self.panel.recommend = YES;
        self.panel.minimum = MAX(self.panel.minimum, minimum); self.panel.maximum = MIN(self.panel.maximum, maximum);
        [self.panel refresh]; return;
    }
    if (self.didOpen) self.didOpen();
    DmControllerPanel *panel = [DmControllerPanel new]; self.panel = panel;
    panel.resolveChoices = self.resolveChoices; panel.recommend = completion != nil;
    panel.snapshot = self.snapshot; panel.identify = self.identify; panel.requirements = self.requirements; panel.applyChoices = self.applyChoices;
    panel.minimum = minimum; panel.maximum = maximum; [panel refresh];
    __weak DmControllerPanel *weakPanel = panel;
    panel.requirementsTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakPanel refreshRequirements];
    }];
    __weak DmControllerUI *weak = self;
    panel.parentCloseObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowWillCloseNotification object:self.parent queue:nil usingBlock:^(NSNotification *n) { (void)n; [weak.panel cancel:nil]; }];
    [self.parent beginSheet:panel.window completionHandler:^(NSModalResponse response) {
        [panel.requirementsTimer invalidate];
        DmControllerUI *owner = weak; if (!owner) return;
        [NSNotificationCenter.defaultCenter removeObserver:owner.panel.parentCloseObserver];
        [owner.panel.window orderOut:nil]; owner.panel = nil;
        NSArray *waiters = [owner.waiters copy]; [owner.waiters removeAllObjects];
        if (owner.didClose) owner.didClose();
        for (void (^waiter)(BOOL) in waiters) waiter(response == NSModalResponseOK);
    }];
}
- (int)requestMinimum:(int)minimum maximum:(int)maximum {
    if (NSThread.isMainThread) return -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0); __block int answer = -1;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.parent) { dispatch_semaphore_signal(done); return; }
        [self openMinimum:minimum maximum:maximum completion:^(BOOL applied) { answer = applied ? 1 : 0; dispatch_semaphore_signal(done); }];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return answer;
}
- (void)refresh { [self.panel refresh]; }
@end

static inline NSMenuItem *dm_controller_menu_add(NSMenu *bar, DmControllerUI *ui) {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Controllers"];
    NSMenuItem *item = [menu addItemWithTitle:@"Configure Controllers…" action:@selector(showFromMenu:) keyEquivalent:@""];
    item.target = ui;
    [[bar addItemWithTitle:@"Controllers" action:nil keyEquivalent:@""] setSubmenu:menu];
    return item;
}
