// A controller's low-battery notices (front_mac.m, pad_battery_mac.h): one when the level first reads at or below 20 % and one at or below 10 % while it
// discharges; charging, or a level back above 20 %, arms them again. Pure C, so tests/t_pad_battery.c checks it on any machine.
#pragma once

enum { PAD_BATTERY_LOW = 20, PAD_BATTERY_VERY_LOW = 10 };

// pct: whole percent, -1 unknown. mark: the last threshold noticed for this controller (0 none). Returns the threshold to notice now, else 0
static inline int pad_battery_notice(int pct, int charging, int *mark) {
    if (pct < 0) return 0;
    if (charging || pct > PAD_BATTERY_LOW) { *mark = 0; return 0; }
    int t = pct <= PAD_BATTERY_VERY_LOW ? PAD_BATTERY_VERY_LOW : PAD_BATTERY_LOW;
    if (*mark && *mark <= t) return 0;
    *mark = t;
    return t;
}
