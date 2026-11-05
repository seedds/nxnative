// Plan both automatic routes and per-device overrides before touching live input.
#pragma once
#include "controller_setup_apple.h"
#include "pad_slots.h"

typedef struct {
    pad_slots slots;
    uint32_t styles[DM_PLAYERS];
    uint32_t manual;
} dm_ios_routes;

static uint32_t ios_style(NSString *name) {
    return [name isEqual:@"auto"] ? DM_STYLE_AUTO : cs_style(name);
}

static NSString *ios_validate_overrides(NSArray *choices, NSDictionary **accepted) {
    NSMutableDictionary *overrides = [NSMutableDictionary new];
    NSMutableSet *seen = [NSMutableSet new];
    uint32_t fixed = 0;
    for (NSDictionary *choice in choices) {
        NSString *key = choice[@"id"], *style = choice[@"style"];
        NSNumber *value = choice[@"player"];
        if (![key isKindOfClass:NSString.class] || ![style isKindOfClass:NSString.class] ||
            ![value isKindOfClass:NSNumber.class] || [seen containsObject:key] || value.doubleValue != value.intValue ||
            value.intValue < -1 || value.intValue > DM_PLAYERS ||
            (![style isEqual:@"auto"] && !ios_style(style))) return @"Invalid controller assignment.";
        if (!cs_devices[key]) return @"A controller disconnected. Review the assignments and try again.";
        [seen addObject:key];
        int player = value.intValue;
        if (player > 0) {
            uint32_t bit = 1u << (player - 1);
            if (fixed & bit) return @"Two controllers cannot use the same player.";
            fixed |= bit;
        }
        if (player != -1 || ![style isEqual:@"auto"]) overrides[key] = [choice copy];
    }
    *accepted = [overrides copy];
    return nil;
}

static void ios_route(dm_ios_routes *plan, NSArray<NSMutableArray<GCController *> *> *sources,
                      GCController *controller, int slot, NSDictionary *choice) {
    if (slot < 0) return;
    if (!sources[slot].count)
        pad_slot_assign(&plan->slots, (__bridge const void *)controller, controller.vendorName.UTF8String ?: "", slot);
    [sources[slot] addObject:controller];
    if (choice) {
        plan->styles[slot] = ios_style(choice[@"style"]);
        plan->manual |= 1u << slot;
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
        if (player > 0) ios_route(&plan, sources, cs_devices[key], player - 1, choice);
    }

    // Retain existing slots for connected devices. In legacy merge mode, the
    // automatic group is rebuilt together after any manual slots are reserved.
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"];
        NSDictionary *choice = choices[key];
        if ((choice && [choice[@"player"] intValue] != -1) || (merge && !choice && !(plan.manual & 1u))) continue;
        GCController *controller = cs_devices[key];
        int slot = pad_slot_of(&previous, (__bridge const void *)controller);
        if (slot >= 0 && !plan.slots.owner[slot]) ios_route(&plan, sources, controller, slot, choice);
    }

    // Devices with a type override and an automatic player need their own slot.
    // Completely automatic pads follow connection order and remembered slots.
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"];
        NSDictionary *choice = choices[key];
        if (choice && [choice[@"player"] intValue] != -1) continue;
        GCController *controller = cs_devices[key];
        const void *owner = (__bridge const void *)controller;
        if (pad_slot_of(&plan.slots, owner) >= 0) continue;
        if (merge && !choice && !(plan.manual & 1u)) {
            ios_route(&plan, sources, controller, 0, nil);
        } else {
            int slot = pad_slot_take(&plan.slots, owner, controller.vendorName.UTF8String ?: "");
            ios_route(&plan, sources, controller, slot, choice);
        }
    }
    return plan;
}

static dm_ios_routes ios_plan_routes(pad_slots previous, NSArray *rows, NSDictionary *choices, BOOL merge,
                                     NSArray<NSMutableArray<GCController *> *> *sources) {
    return ios_plan_routes_reserved(previous, rows, choices, merge, sources, 0);
}
