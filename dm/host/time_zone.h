// The time service's time zone answers, worked out from the host's zone (hle_time.c, hle_sdk16.c). Plain C and POSIX, so tests/t_time_zone.c checks them against the zone
// database without a game or a Mac.
#pragma once
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { DM_LOCATION_NAME_SIZE = 0x24 };   // nn::time::LocationName: char[0x24], NUL-terminated

// The IANA name in a TZ value or a zoneinfo path ("America/Los_Angeles" from ":America/Los_Angeles", "/var/db/timezone/zoneinfo/America/Los_Angeles" or
// "/usr/share/zoneinfo/Asia/Tokyo"), into out. A name is letters, digits and _ + - /, never begins with / and has no "..": a POSIX rule with commas or angle
// brackets ("PST8PDT,M3.2.0,M11.1.0", "<+09>-9") is not one. Returns 1 when out holds a name, 0 otherwise (out untouched)
static inline int dm_tz_name_from(const char *s, char *out, size_t cap) {
    if (!s) return 0;
    if (*s == ':') s++;
    for (const char *z; (z = strstr(s, "zoneinfo/"));) s = z + 9;   // the part after the last zoneinfo/ folder
    size_t n = strlen(s);
    if (!n || n >= cap || *s == '/' || strstr(s, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-' || c == '/')) return 0;
    }
    memcpy(out, s, n + 1);
    return 1;
}

// The zone a localtime link names: its own target, else the end of its chain of links. Returns 1 when out holds a name
static inline int dm_tz_name_from_link(const char *path, char *out, size_t cap) {
    char link[PATH_MAX]; ssize_t l = readlink(path, link, sizeof link - 1);
    if (l > 0) { link[l] = 0; if (dm_tz_name_from(link, out, cap)) return 1; }
    return realpath(path, link) && dm_tz_name_from(link, out, cap);
}

// nn::time::LocationName of the device: an IANA name ("Asia/Tokyo"), as a Switch reports it, where the host answered its zone abbreviation ("JST"). In order: TZ (main.c sets
// it to UTC for DM_FIXED_TIME); the system zone, from where /etc/localtime links (macOS: /var/db/timezone/zoneinfo/<name>, where CoreFoundation takes the system zone from
// too), or from the end of its whole chain of links (for a second link in between, such as iOS's
// /var/db/timezone/localtime; not checked on an iPhone); the abbreviation as before when none
// of these names a zone; "UTC" last. Plain POSIX, so the HLE files take no Apple headers for it. out has DM_LOCATION_NAME_SIZE bytes, zero-filled past the name
static inline void dm_device_location_name(char *out) {
    char name[DM_LOCATION_NAME_SIZE] = "";
    if (!dm_tz_name_from(getenv("TZ"), name, sizeof name)) {
        dm_tz_name_from_link("/etc/localtime", name, sizeof name);
        if (!*name) { tzset(); if (tzname[0] && *tzname[0]) dm_tz_name_from(tzname[0], name, sizeof name); }
        if (!*name) memcpy(name, "UTC", 4);
    }
    memset(out, 0, DM_LOCATION_NAME_SIZE);
    memcpy(out, name, strlen(name));
}

// nn::time::ToPosixTime in the host zone: the absolute times a local calendar time names, earliest first. Where clocks go back (the end of daylight saving time) one local
// time happens twice and both are given; otherwise one. A time the change skips (clocks going forward) gets mktime's single normalized answer, as before: what the time
// service answers there is not known. Each candidate is mktime with daylight saving time off and on, kept when localtime gives the same calendar back.
// Returns the count (1 or 2); out has room for 2
static inline int dm_local_to_posix(int year, int month, int day, int hour, int minute, int second, int64_t out[2]) {
    struct tm want = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = hour, .tm_min = minute, .tm_sec = second};
    int n = 0;
    for (int dst = 0; dst <= 1; dst++) {
        struct tm t = want; t.tm_isdst = dst;
        time_t v = mktime(&t);
        struct tm back;
        if (!localtime_r(&v, &back)) continue;
        if (back.tm_year != want.tm_year || back.tm_mon != want.tm_mon || back.tm_mday != want.tm_mday || back.tm_hour != want.tm_hour || back.tm_min != want.tm_min || back.tm_sec != want.tm_sec) continue;
        if (n && out[0] == (int64_t)v) continue;   // a zone without daylight saving time: both flags give the same time
        out[n++] = (int64_t)v;
    }
    if (n == 2 && out[1] < out[0]) { int64_t s = out[0]; out[0] = out[1]; out[1] = s; }
    if (!n) { struct tm t = want; t.tm_isdst = -1; out[0] = (int64_t)mktime(&t); n = 1; }
    return n;
}
