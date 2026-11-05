// The Apple device-swap adapter, independent of its window. Tests use fake
// devices with the same methods, while the host supplies its actual slot table and callbacks.
#pragma once
#include "controller_routes_ios.h"
#include "pad_slots.h"
#include "pad_apple.h"
#include "motion_apple.h"
#include "haptics_apple.h"

typedef struct {
    pad_slots *slots;
    GCController * __strong *held;
    int *merge, *keyboardOwn;
    NSArray<GCController *> *(*devices)(void);
    int (*mode)(void);
    void (*read)(GCExtendedGamepad *);
    void (*keep)(GCController *);
    void (*changed)(GCController *);
} dm_controller_host;

static NSDictionary *controller_assignment_state(dm_controller_host host) {
    NSArray *rows = cs_update(host.devices());
    NSMutableArray *choices = [NSMutableArray new];
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"];
        NSDictionary *choice = cs_choices[key] ?: @{@"id": key, @"player": @(-1), @"style": @"auto"};
        [choices addObject:choice];
    }
    return @{@"controllers": rows, @"choices": choices};
}
static NSArray *controller_assignment_resolve(dm_controller_host host, NSArray *choices) {
    NSArray *rows = cs_update(host.devices());
    NSMutableDictionary *overrides = [NSMutableDictionary new];
    for (NSDictionary *choice in choices)
        if ([choice[@"player"] intValue] != -1 || ![choice[@"style"] isEqual:@"auto"]) overrides[choice[@"id"]] = choice;
    NSMutableArray *sources = [NSMutableArray new];
    for (int p = 0; p < DM_PLAYERS; p++) [sources addObject:[NSMutableArray new]];
    uint32_t reserved = 0;
    for (int p = 0; p < DM_PLAYERS; p++) if (host.slots->owner[p] && !host.held[p]) reserved |= 1u << p;
    for (NSDictionary *choice in overrides.allValues) {
        int p = [choice[@"player"] intValue] - 1;
        if (p >= 0) reserved &= ~(1u << p);
    }
    (void)ios_plan_routes_reserved(*host.slots, rows, overrides, *host.merge && !overrides.count, sources, reserved);
    NSMutableArray *resolved = [NSMutableArray new];
    for (NSDictionary *row in rows) {
        NSString *key = row[@"id"]; GCController *device = cs_devices[key];
        NSDictionary *choice = overrides[key];
        int slot = -1;
        for (int p = 0; p < DM_PLAYERS; p++) if ([sources[p] containsObject:device]) { slot = p; break; }
        uint32_t configured = choice ? cs_style(choice[@"style"]) : 0;
        NSString *automatic = slot == 0 && !host.mode() ? @"handheld" : @"pro";
        [resolved addObject:@{@"id": key, @"player": @(slot + 1),
            @"style": configured ? cs_style_name(configured) : automatic,
            @"configuredStyle": choice[@"style"] ?: @"auto", @"automaticStyle": automatic}];
    }
    return resolved;
}
static dm_controller_requirements controller_assignment_requirements(dm_controller_host host) {
    dm_controller_requirements game;
    dm_pad_supported(&game.styles, &game.ids);
    int docked = host.mode();
    BOOL supported = (game.styles & (docked ? DM_STYLE_PRO : 2u)) && (game.ids & (docked ? 1u : 1u << 8));
    game.fallback = supported ? DM_FALLBACK_IF_PLAYER_ONE_FREE : DM_FALLBACK_NONE;
    return game;
}
static NSString *controller_assignment_apply(dm_controller_host host, NSArray *choices, int minimum, int maximum, uint32_t *changed) {
    cs_update(host.devices());
    NSDictionary *next = nil; NSString *error = cs_validate_choices(choices, &next);
    if (error) return error;
    dm_pad_reconfigure_begin();
    @try {
        NSArray *resolved = controller_assignment_resolve(host, choices);
        int count = controller_ready_players(controller_assignment_requirements(host), resolved, NULL);
        if (count < minimum || count > maximum) return @"Assign the number of supported controllers this game requests.";
        BOOL edited = NO;
        for (NSDictionary *choice in next.allValues)
            if ([choice[@"player"] intValue] != -1 || ![choice[@"style"] isEqual:@"auto"]) edited = YES;
        if (!edited && !cs_explicit) { if (changed) *changed = 0; return nil; }
        GCController *owners[DM_PLAYERS] = {nil}; uint32_t styles[DM_PLAYERS] = {0}, affected = 0;
        for (NSDictionary *choice in resolved) {
            int p = [choice[@"player"] intValue] - 1; if (p < 0) continue;
            owners[p] = cs_devices[choice[@"id"]]; styles[p] = cs_style(choice[@"configuredStyle"]);
        }
        for (int p = 0; p < DM_PLAYERS; p++) {
            // Keyboard and raw-HID slots are outside this device list. An
            // Automatic choice must not displace a source the sheet cannot edit.
            if (host.slots->owner[p] && !host.held[p] && !owners[p]) continue;
            if (*host.merge || host.slots->owner[p] != (__bridge const void *)owners[p] || dm_pad_configured_style(p) != styles[p]) affected |= 1u << p;
        }
        // Legacy player 1 can still have a queued controller sample after its device
        // disconnected. Entering explicit mode must invalidate that source as well.
        if (!cs_explicit) affected |= 1u;
        dm_pad_reconfigure_reset(affected);
        // Invalidate native routes before any new owner is attached. Queued gyro samples
        // carry a generation and cannot write back into a slot after its owner changes.
        if (*host.merge) for (GCController *c in host.devices()) {
            c.extendedGamepad.valueChangedHandler = nil; dm_motion_detach(c, 0);
        }
        for (int p = 0; p < DM_PLAYERS; p++) if (affected >> p & 1) {
            if (host.held[p]) { host.held[p].extendedGamepad.valueChangedHandler = nil; dm_motion_detach(host.held[p], p); }
            dm_haptics_apple_controller(p, nil); host.held[p] = nil;
            pad_slot_free(host.slots, host.slots->owner[p]);
        }
        cs_choices = next; cs_explicit = 1; *host.merge = 0;
        if (host.keyboardOwn && owners[0]) *host.keyboardOwn = 0;
        dm_pad_explicit(1);
        for (int p = 0; p < DM_PLAYERS; p++) {
            dm_pad_style(p, styles[p], owners[p] && !dm_pad_is_joycon(owners[p]));
            if (!(affected >> p & 1) || !owners[p]) continue;
            GCController *c = owners[p];
            pad_slot_assign(host.slots, (__bridge const void *)c, (c.vendorName.UTF8String ?: "?"), p); host.held[p] = c;
            host.keep(c);
            void (*read)(GCExtendedGamepad *) = host.read;
            c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; read(g); };
            dm_motion_attach(c, p); dm_haptics_apple_controller(p, c);
        }
        dm_pad_players(pad_slot_mask(host.slots));
        if (host.changed) host.changed(nil);
        if (changed) *changed = affected;
        return nil;
    } @finally { dm_pad_reconfigure_end(); }
}
