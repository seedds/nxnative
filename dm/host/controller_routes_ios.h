// Plan both automatic routes and per-device overrides before touching live input.
#pragma once
#include "controller_setup_apple.h"
#include "pad_slots.h"

typedef struct {
    pad_slots slots;
    uint32_t styles[DM_PLAYERS];
    uint32_t manual;
    uint32_t sides[DM_PLAYERS];
} dm_ios_routes;

static NSString *ios_validate_overrides(NSArray *choices, NSDictionary **accepted) {
    NSDictionary *validated=nil;
    NSString *error=cs_validate_choices(choices,&validated); if (error) return error;
    NSMutableDictionary *overrides=[NSMutableDictionary new];
    for (NSString *key in validated) {
        NSDictionary *c=validated[key];
        if ([c[@"player"] intValue]!=-1 || ![c[@"style"] isEqual:@"auto"]) overrides[key]=c;
    }
    *accepted=[overrides copy]; return nil;
}

static void ios_route(dm_ios_routes *plan, NSArray<NSMutableArray<GCController *> *> *sources,
                      id controller, NSString *name, int slot, NSDictionary *choice) {
    if (slot < 0) return;
    if (!sources[slot].count)
        pad_slot_assign(&plan->slots, (__bridge const void *)controller, name.UTF8String ?: "", slot);
    [sources[slot] addObject:controller];
    if (choice) {
        plan->styles[slot] = cs_style(choice[@"style"]);
        plan->manual |= 1u << slot;
        int role=cs_role(choice); if (role) plan->sides[slot] |= 1u << (role-1);
    }
}

static dm_ios_routes ios_plan_routes_reserved(pad_slots previous, NSArray *rows, NSDictionary *choices, BOOL merge,
                                     NSArray<NSMutableArray<GCController *> *> *sources, uint32_t reserved) {
    dm_ios_routes plan = {.slots = previous};
    for (int p = 0; p < DM_PLAYERS; p++) if (!(reserved >> p & 1)) plan.slots.owner[p] = NULL;

    // Fixed slots take precedence, including when they displace an automatic pad.
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"];
        NSDictionary *choice = choices[key];
        int player = [choice[@"player"] intValue];
        if (player > 0) ios_route(&plan, sources, cs_devices[key], row[@"name"], player - 1, choice);
    }

    // Retain existing slots for connected devices. In legacy merge mode, the
    // automatic group is rebuilt together after any manual slots are reserved.
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"];
        NSDictionary *choice = choices[key];
        if ((choice && [choice[@"player"] intValue] != -1) || (merge && !choice && !(plan.manual & 1u))) continue;
        id controller = cs_devices[key];
        if (cs_waits_for_press(controller,choice)) continue;
        int slot = pad_slot_of(&previous, (__bridge const void *)controller);
        if (slot >= 0 && !plan.slots.owner[slot]) ios_route(&plan, sources, controller, row[@"name"], slot, choice);
    }

    // Devices with a type override and an automatic player need their own slot.
    // Completely automatic pads follow connection order and remembered slots.
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"];
        NSDictionary *choice = choices[key];
        if (choice && [choice[@"player"] intValue] != -1) continue;
        id controller = cs_devices[key];
        if (cs_waits_for_press(controller,choice)) continue;
        const void *owner = (__bridge const void *)controller;
        if (pad_slot_of(&plan.slots, owner) >= 0) continue;
        if (merge && !choice && !(plan.manual & 1u)) {
            ios_route(&plan, sources, controller, row[@"name"], 0, nil);
        } else {
            int slot = cs_press_to_join ? pad_slot_take_lowest(&plan.slots,owner,[row[@"name"] UTF8String] ?: "") : pad_slot_take(&plan.slots, owner, [row[@"name"] UTF8String] ?: "");
            ios_route(&plan, sources, controller, row[@"name"], slot, choice);
        }
    }
    return plan;
}

static dm_ios_routes ios_plan_routes(pad_slots previous, NSArray *rows, NSDictionary *choices, BOOL merge,
                                     NSArray<NSMutableArray<GCController *> *> *sources) {
    return ios_plan_routes_reserved(previous, rows, choices, merge, sources, 0);
}

static dm_controller_routes ios_plan_snapshot(dm_ios_routes plan,BOOL handheld) {
    dm_controller_routes snapshot={.occupied=pad_slot_mask(&plan.slots),.manual=plan.manual,
        .handheld=handheld,.fallback=dm_controller_ios_fallback(pad_slot_mask(&plan.slots),plan.manual)};
    for (int p=0;p<DM_PLAYERS;p++) {
        snapshot.styles[p]=plan.styles[p]; snapshot.sides[p]=plan.sides[p];
        if (plan.sides[p]) snapshot.grouped|=1u<<p;
    }
    return snapshot;
}

static NSDictionary *ios_route_status(dm_ios_routes plan,NSArray *sources,NSDictionary *overrides,
        BOOL inGame,BOOL handheld,dm_controller_limits limits) {
    uint32_t styles=limits.styles,ids=limits.ids;
    NSMutableDictionary *routes=[NSMutableDictionary new];
    dm_controller_routes snapshot=ios_plan_snapshot(plan,handheld);
    dm_controller_evaluation evaluation=dm_controller_evaluate(snapshot,limits);
    BOOL complete=YES;
    for (int p=0;p<DM_PLAYERS;p++) if (evaluation.rejected[p]&DM_CONTROLLER_PAIR_INCOMPLETE) complete=NO;
    BOOL any = pad_slot_mask(&plan.slots) != 0;
    for (int p = 0; p < DM_PLAYERS; p++) {
        int slot=inGame ? dm_controller_input_player(snapshot,limits,p) : p;
        BOOL accepted=(evaluation.ready>>slot&1u)!=0;
        for (GCController *controller in sources[p]) {
            NSString *key = [cs_devices allKeysForObject:controller].firstObject;
            NSString *automaticStyle = !slot && handheld ? @"handheld" : @"pro";
            routes[key] = @{@"player": @(slot + 1), @"ready": @(accepted), @"automaticStyle": automaticStyle,
                @"recommendation": controller_recommended_style(styles, slot) ?: @"", @"role": overrides[key][@"role"] ?: @"both", @"pairComplete": @(!plan.sides[p] || plan.sides[p]==3)};
        }
    }
    BOOL touch=inGame && !any && evaluation.fallback!=0;
    NSMutableArray *types = [NSMutableArray new], *players = [NSMutableArray new];
    const uint32_t externalStyles[] = {DM_STYLE_PRO, DM_STYLE_PAIR, DM_STYLE_LEFT, DM_STYLE_RIGHT};
    for (size_t i = 0; i < sizeof externalStyles / sizeof externalStyles[0]; i++)
        if (styles & externalStyles[i]) [types addObject:cs_style_name(externalStyles[i])];
    for (int p = 0; p < DM_PLAYERS; p++) if (ids >> p & 1) [players addObject:@(p + 1)];
    return @{@"minimum": @(limits.minimum), @"maximum": @(limits.maximum), @"styles": types, @"players": players,
        @"requiredStyle": controller_required_style(styles) ?: @"",
        @"recommendedStyles": styles==(DM_STYLE_LEFT|DM_STYLE_RIGHT) ? @[@"left",@"right"] : controller_required_style(styles) ? @[controller_required_style(styles)] : @[],
        @"ready": @(evaluation.result_count), @"complete": @(complete), @"touch": @(touch), @"fallback": @(inGame && !touch && evaluation.fallback!=0), @"routes": routes};
}
