// The Apple frontends use session IDs to match choices to connected devices.
#pragma once
#import <GameController/GameController.h>
#include "controller_requirements_apple.h"

static NSMutableDictionary<NSString *, id> *cs_devices;
static NSDictionary<NSString *, NSDictionary *> *cs_choices;
static NSDictionary<NSString *, NSDictionary *> *cs_source_info;
static int cs_explicit;
// Admission belongs to this connection, and only affects the existing optional
// press-to-join mode. A draft preview never admits a waiting automatic device.
static BOOL cs_press_to_join;
static NSMutableSet<NSString *> *cs_admitted;
static void cs_admit(id source) {
    NSString *key=source ? [cs_devices allKeysForObject:source].firstObject : nil;
    if (!key) return;
    if (!cs_admitted) cs_admitted=[NSMutableSet new];
    [cs_admitted addObject:key];
}
static BOOL cs_waits_for_press(id source,NSDictionary *choice) {
    NSString *key=source ? [cs_devices allKeysForObject:source].firstObject : nil;
    return key && cs_press_to_join && !choice && ![cs_admitted containsObject:key];
}

// The owner is the input adapter's connection object, not a model name or an
// enumeration position. GC-only clients use cs_update below. The Mac backend
// supplies its existing transport objects, including individual HID halves.
static NSArray *cs_update_sources(NSArray<NSDictionary *> *sources) {
    if (!cs_devices) cs_devices = [NSMutableDictionary new];
    NSMutableSet *seen = [NSMutableSet new];
    NSMutableArray *rows = [NSMutableArray new];
    NSMutableDictionary *info=[NSMutableDictionary new];
    for (NSDictionary *source in sources) {
        id owner=source[@"owner"]; if (!owner) continue;
        NSString *key = [cs_devices allKeysForObject:owner].firstObject ?: NSUUID.UUID.UUIDString;
        cs_devices[key] = owner; [seen addObject:key];
        NSMutableDictionary *row=[source mutableCopy]; [row removeObjectForKey:@"owner"];
        row[@"id"]=key; row[@"name"]=source[@"name"] ?: @"Controller"; [rows addObject:row]; info[key]=row;
    }
    for (NSString *key in cs_devices.allKeys) if (![seen containsObject:key]) [cs_devices removeObjectForKey:key];
    [cs_admitted intersectSet:seen];
    cs_source_info=info;
    return rows;
}
static NSArray *cs_update(NSArray<GCController *> *devices) {
    NSMutableArray *sources=[NSMutableArray new];
    for (GCController *c in devices) if (c.extendedGamepad) [sources addObject:@{@"owner":c,@"name":c.vendorName ?: @"Controller"}];
    return cs_update_sources(sources);
}
static int cs_source_player(id source, uint32_t *style) {
    NSString *key=source ? [cs_devices allKeysForObject:source].firstObject : nil;
    NSDictionary *choice=key ? cs_choices[key] : nil;
    int player=[choice[@"player"] intValue];
    if (style) *style=cs_style(choice[@"style"]);
    return !choice || player==-1 ? -2 : player ? player-1 : -1;
}

static int cs_device_role(GCController *c, NSDictionary *choices) {
    NSString *key=c ? [cs_devices allKeysForObject:c].firstObject : nil;
    return key ? cs_role(choices[key]) : 0;
}
// Shared by startup and live Apply. Neither UI may partially publish an invalid draft.
static NSString *cs_validate_choices(NSArray *choices, NSDictionary **accepted) {
    if (![choices isKindOfClass:NSArray.class]) return @"Invalid controller choices.";
    NSMutableDictionary *next = [NSMutableDictionary new];
    NSMutableDictionary *players = [NSMutableDictionary new];
    for (id row in choices) {
        if (![row isKindOfClass:NSDictionary.class]) return @"Invalid controller choice.";
        NSString *key = row[@"id"], *style = row[@"style"];
        NSNumber *player = row[@"player"];
        if (![key isKindOfClass:NSString.class] || ![style isKindOfClass:NSString.class] || ![player isKindOfClass:NSNumber.class] ||
            player.doubleValue != player.intValue || player.intValue < -1 || player.intValue > DM_PLAYERS || (!cs_style(style) && ![style isEqual:@"auto"]) || next[key]) return @"Invalid controller assignment.";
        id role = row[@"role"];
        if (role && (![role isKindOfClass:NSString.class] || ![@[@"both",@"left",@"right"] containsObject:role])) return @"Invalid pair role.";
        if (cs_role(row) && (![style isEqual:@"pair"] || player.intValue <= 0)) return @"Choose a player and Joy-Con pair before assigning a pair role.";
        NSString *physicalSide=cs_source_info[key][@"physicalSide"];
        if (physicalSide && player.intValue!=0 && [style isEqual:@"pair"] && ![role isEqual:physicalSide])
            return @"Use the physical Joy-Con side as this controller's pair input.";
        if (!cs_devices[key]) return @"A controller disconnected. Choose a connected controller and try again.";
        if (player.intValue > 0) {
            NSArray *group=players[player] ?: @[];
            if (group.count && (group.count!=1 || !cs_can_share(group[0],row))) return @"A shared player needs one left and one right controller set to Joy-Con pair.";
            players[player]=[group arrayByAddingObject:row];
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
    return cs_source_player(controller,style);
}

typedef BOOL (*cs_identify_fn)(GCController *);

static BOOL cs_identify(NSString *key, cs_identify_fn signal) {
    GCController *device = cs_devices[key];
    return device && signal && signal(device);
}
