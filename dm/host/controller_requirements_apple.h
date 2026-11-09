// The same game acceptance rules drive the controller sheet and live Apply.
#pragma once
#import <Foundation/Foundation.h>
#include "dm_input.h"
#include "controller_readiness.h"

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

static inline dm_controller_evaluation controller_choices_evaluate(dm_controller_requirements game, NSArray *choices) {
    dm_controller_routes routes={.explicit_routes=1,.fallback=game.fallback==DM_FALLBACK_IF_PLAYER_ONE_FREE,
        .handheld=!(game.styles&DM_STYLE_PRO) && (game.styles&2u)};
    for (NSDictionary *choice in choices) {
        int p=[choice[@"player"] intValue]-1;
        if (p<0 || p>=DM_PLAYERS) continue;
        routes.occupied|=1u<<p; routes.styles[p]=cs_style(choice[@"style"]);
        if (choice[@"pairComplete"]) {
            routes.grouped|=1u<<p; routes.sides[p]=[choice[@"pairComplete"] boolValue] ? 3 : 1;
        }
    }
    return dm_controller_evaluate(routes,(dm_controller_limits){game.styles,game.ids,0,DM_PLAYERS});
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

// The badge names a sole accepted type. A suggested draft may still choose
// between several accepted types, and does not imply physical sensor support.
static inline NSString *controller_required_style(uint32_t supported) {
    if (!supported || (supported & (supported - 1u))) return nil;
    return controller_recommended_style(supported, 0);
}

static inline BOOL controller_style_recommended(uint32_t supported, uint32_t style) {
    return (supported == (DM_STYLE_LEFT|DM_STYLE_RIGHT) && (style==DM_STYLE_LEFT || style==DM_STYLE_RIGHT)) ||
        (style && supported==style && controller_required_style(supported)!=nil);
}

static int cs_role(NSDictionary *choice) {
    return [choice[@"role"] isEqual:@"left"] ? 1 : [choice[@"role"] isEqual:@"right"] ? 2 : 0;
}
static BOOL cs_can_share(NSDictionary *a, NSDictionary *b) {
    return [a[@"style"] isEqual:@"pair"] && [b[@"style"] isEqual:@"pair"] && cs_role(a) && cs_role(b) && cs_role(a)!=cs_role(b);
}
static NSString *cs_pair_error(NSArray *choices) {
    uint32_t sides[DM_PLAYERS]={0};
    for (NSDictionary *c in choices) {
        int p=[c[@"player"] intValue]-1, role=cs_role(c);
        if (p>=0 && p<DM_PLAYERS && role) sides[p] |= 1u << (role-1);
    }
    for (int p=0;p<DM_PLAYERS;p++) if (sides[p] && sides[p]!=3)
        return @"Assign a left and a right controller to the same player to complete the pair.";
    return nil;
}
