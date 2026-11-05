// The same game acceptance rules drive the controller sheet and live Apply.
#pragma once
#import <Foundation/Foundation.h>
#include "dm_input.h"

typedef enum {
    DM_FALLBACK_NONE, DM_FALLBACK_IF_PLAYER_ONE_FREE
} dm_controller_fallback;

typedef struct {
    uint32_t styles, ids;
    dm_controller_fallback fallback;
} dm_controller_requirements;

static inline uint32_t cs_style(NSString *name) {
    if ([name isEqual:@"handheld"]) return 2u;
    if ([name isEqual:@"pro"]) return DM_STYLE_PRO;
    if ([name isEqual:@"pair"]) return DM_STYLE_PAIR;
    if ([name isEqual:@"left"]) return DM_STYLE_LEFT;
    if ([name isEqual:@"right"]) return DM_STYLE_RIGHT;
    return 0;
}

static inline BOOL controller_player_supported(dm_controller_requirements game, int player) {
    return player > 0 && player <= DM_PLAYERS && (game.ids & (1u << (player - 1)));
}

static inline int controller_ready_players(dm_controller_requirements game, NSArray *choices, BOOL *fallbackCounts) {
    int count = 0; uint32_t counted = 0;
    BOOL firstAssigned = NO;
    for (NSDictionary *choice in choices) {
        int player = [choice[@"player"] intValue];
        if (player == 1) firstAssigned = YES;
        BOOL slotSupported = player == 1 && [choice[@"style"] isEqual:@"handheld"]
            ? (game.ids & (1u << 8)) != 0 : controller_player_supported(game, player);
        if (slotSupported && (game.styles & cs_style(choice[@"style"])) && !(counted >> (player - 1) & 1)) {
            counted |= 1u << (player - 1); count++;
        }
    }
    BOOL fallback = game.fallback == DM_FALLBACK_IF_PLAYER_ONE_FREE && !firstAssigned;
    if (fallbackCounts) *fallbackCounts = fallback;
    return count + fallback;
}

// A recommendation only changes the draft. Prefer full controllers when
// accepted, otherwise alternate the accepted single Joy-Con sides by player.
static inline NSString *controller_recommended_style(uint32_t supported, int player) {
    if (supported & DM_STYLE_PRO) return @"pro";
    if (supported & DM_STYLE_PAIR) return @"pair";
    if ((supported & (DM_STYLE_LEFT | DM_STYLE_RIGHT)) == (DM_STYLE_LEFT | DM_STYLE_RIGHT))
        return player % 2 ? @"right" : @"left";
    if (supported & DM_STYLE_LEFT) return @"left";
    if (supported & DM_STYLE_RIGHT) return @"right";
    return nil;
}
