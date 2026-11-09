// Evaluate live and draft routes without assigning devices or changing input.
#pragma once
#include "dm_input.h"

typedef struct {
    uint32_t styles, ids;
    int minimum, maximum;
} dm_controller_limits;

typedef struct {
    uint32_t occupied, manual, grouped;
    uint32_t styles[DM_PLAYERS], sides[DM_PLAYERS];
    int handheld, explicit_routes, fallback;
} dm_controller_routes;

enum {
    DM_CONTROLLER_ABSENT=1, DM_CONTROLLER_ID=2,
    DM_CONTROLLER_STYLE=4, DM_CONTROLLER_PAIR_INCOMPLETE=8
};
typedef struct {
    uint32_t joined, ready, fallback;
    uint32_t rejected[DM_PLAYERS], styles[DM_PLAYERS], ids[DM_PLAYERS];
    int ready_count, result_count;
    uint32_t selected_id;
} dm_controller_evaluation;

static inline dm_controller_limits dm_controller_request_limits(uint32_t styles,uint32_t ids,
        int minimum,int maximum,int single,int permit_pair) {
    if (minimum<0) minimum=0;
    if (minimum>DM_PLAYERS) minimum=DM_PLAYERS;
    if (maximum<=0 || maximum>DM_PLAYERS) maximum=DM_PLAYERS;
    if (single) minimum=maximum=1;
    return (dm_controller_limits){permit_pair ? styles : styles & ~DM_STYLE_PAIR,ids,minimum,maximum};
}
static inline int dm_controller_ios_fallback(uint32_t occupied,uint32_t manual) {
    return !manual || (occupied & ~manual) || (occupied & 1u);
}
static inline int dm_controller_capped_count(int count,int maximum) {
    return maximum>0 && count>maximum ? maximum : count;
}
static inline dm_controller_evaluation dm_controller_evaluate(dm_controller_routes routes,dm_controller_limits game) {
    dm_controller_evaluation result={0};
    uint32_t occupied=routes.occupied;
    if (routes.fallback && (!routes.explicit_routes || !routes.styles[0])) occupied|=1u;
    result.selected_id=!routes.styles[0] && routes.handheld ? 0x20 : 0;
    for (int p=0;p<DM_PLAYERS;p++) {
        uint32_t bit=1u<<p;
        uint32_t id=!p && !routes.styles[0] && routes.handheld ? 0x20 : (uint32_t)p;
        uint32_t id_bit=id==0x20 ? 1u<<8 : bit;
        uint32_t style=routes.styles[p] ? routes.styles[p] : (id==0x20 ? 2u : DM_STYLE_PRO);
        // This is the existing default Player 1 rule, including its legacy ID
        // fallback. Preserve it rather than silently adding/removing a player.
        int fallback=!p && routes.fallback && (!routes.styles[0] || (!routes.explicit_routes && !(routes.manual&1u)));
        result.ids[p]=id; result.styles[p]=style;
        if (!(occupied&bit) && !fallback) result.rejected[p]|=DM_CONTROLLER_ABSENT;
        if (!(game.ids&id_bit) && !fallback) result.rejected[p]|=DM_CONTROLLER_ID;
        if (!(result.rejected[p]&(DM_CONTROLLER_ABSENT|DM_CONTROLLER_ID))) result.joined|=bit;
        if (!(game.styles&style)) result.rejected[p]|=DM_CONTROLLER_STYLE;
        if (style==DM_STYLE_PAIR && (routes.grouped&bit) && routes.sides[p]!=3)
            result.rejected[p]|=DM_CONTROLLER_PAIR_INCOMPLETE;
        if (!result.rejected[p]) {
            if (!result.ready) result.selected_id=id;
            result.ready|=bit;
            if (fallback && !(routes.occupied&1u)) result.fallback|=1u;
        }
    }
    result.ready_count=__builtin_popcount(result.ready);
    result.result_count=dm_controller_capped_count(result.ready_count,game.maximum);
    return result;
}
// An automatic source in an unsupported slot feeds Player 1 in the existing
// one-player mode; manual sources and explicit Mac layouts retain their slots.
static inline int dm_controller_input_player(dm_controller_routes routes,dm_controller_limits game,int player) {
    return player>0 && !routes.explicit_routes && !(routes.manual>>player&1u) && !(game.ids&0xfeu) ? 0 : player;
}
// Existing assignments and pair members are consumed sources, not joiners.
// chosen uses cs_source_player's -2 automatic / -1 Unassigned / >=0 fixed slot.
static inline int dm_controller_source_waiting(int assigned,int chosen,int merge) {
    return !merge && !assigned && chosen==-2;
}

// Live snapshots are taken under the HID reconfiguration barrier.
dm_controller_routes dm_controller_current_routes(void);
dm_controller_evaluation dm_controller_support_evaluation(void);
