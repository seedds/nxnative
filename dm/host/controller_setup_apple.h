// The Apple frontends use session IDs to match choices to connected devices.
#pragma once
#import <GameController/GameController.h>
#include "controller_requirements_apple.h"

static NSMutableDictionary<NSString *, GCController *> *cs_devices;
static NSDictionary<NSString *, NSDictionary *> *cs_choices;
static int cs_explicit;

static NSArray *cs_update(NSArray<GCController *> *devices) {
    if (!cs_devices) cs_devices = [NSMutableDictionary new];
    NSMutableSet *seen = [NSMutableSet new];
    NSMutableArray *rows = [NSMutableArray new];
    for (GCController *device in devices) if (device.extendedGamepad) {
        NSString *key = [cs_devices allKeysForObject:device].firstObject ?: NSUUID.UUID.UUIDString;
        cs_devices[key] = device; [seen addObject:key];
        [rows addObject:@{@"id": key, @"name": device.vendorName ?: @"Controller"}];
    }
    for (NSString *key in cs_devices.allKeys) if (![seen containsObject:key]) [cs_devices removeObjectForKey:key];
    return rows;
}

// Shared by startup and live Apply. Neither UI may partially publish an invalid draft.
static NSString *cs_validate_choices(NSArray *choices, NSDictionary **accepted) {
    if (![choices isKindOfClass:NSArray.class]) return @"Invalid controller choices.";
    NSMutableDictionary *next = [NSMutableDictionary new];
    uint32_t players = 0;
    for (id row in choices) {
        if (![row isKindOfClass:NSDictionary.class]) return @"Invalid controller choice.";
        NSString *key = row[@"id"], *style = row[@"style"];
        NSNumber *player = row[@"player"];
        if (![key isKindOfClass:NSString.class] || ![style isKindOfClass:NSString.class] || ![player isKindOfClass:NSNumber.class] ||
            player.doubleValue != player.intValue || player.intValue < -1 || player.intValue > DM_PLAYERS || (!cs_style(style) && ![style isEqual:@"auto"]) || next[key]) return @"Invalid controller assignment.";
        if (!cs_devices[key]) return @"A controller disconnected. Choose a connected controller and try again.";
        if (player.intValue > 0) {
            uint32_t bit = 1u << (player.intValue - 1);
            if (players & bit) return @"Two controllers cannot use the same player.";
            players |= bit;
        }
        next[key] = [row copy];
    }
    if (accepted) *accepted = [next copy];
    return nil;
}

static NSString *cs_style_name(uint32_t style) {
    return style == DM_STYLE_PAIR ? @"pair" : style == DM_STYLE_LEFT ? @"left" : style == DM_STYLE_RIGHT ? @"right" : @"pro";
}

static int cs_player(GCController *controller, uint32_t *style) {
    NSString *key = [cs_devices allKeysForObject:controller].firstObject;
    NSDictionary *choice = key ? cs_choices[key] : nil;
    int player = [choice[@"player"] intValue];
    if (style) *style = cs_style(choice[@"style"]);
    return !choice || player == -1 ? -2 : player ? player - 1 : -1;
}

typedef BOOL (*cs_identify_fn)(GCController *);

static BOOL cs_identify(NSString *key, cs_identify_fn signal) {
    GCController *device = cs_devices[key];
    return device && signal && signal(device);
}
