// time_zone.h against the zone database: the device location's IANA name, and ToPosixTime's one or two absolute times around daylight saving time changes.
#include "time_zone.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>

static void zone(const char *tz) { setenv("TZ", tz, 1); tzset(); }
static int64_t utc(int y, int mo, int d, int h, int mi) { struct tm t = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi}; return (int64_t)timegm(&t); }

int main(void) {
    char n[DM_LOCATION_NAME_SIZE];
    // Names out of TZ values and zoneinfo paths
    assert(dm_tz_name_from("America/Los_Angeles", n, sizeof n) && !strcmp(n, "America/Los_Angeles"));
    assert(dm_tz_name_from(":Asia/Tokyo", n, sizeof n) && !strcmp(n, "Asia/Tokyo"));
    assert(dm_tz_name_from("/var/db/timezone/zoneinfo/Europe/Berlin", n, sizeof n) && !strcmp(n, "Europe/Berlin"));   // where macOS links /etc/localtime
    assert(dm_tz_name_from("/usr/share/zoneinfo/America/Argentina/Buenos_Aires", n, sizeof n) && !strcmp(n, "America/Argentina/Buenos_Aires"));
    assert(dm_tz_name_from("Etc/GMT+5", n, sizeof n) && !strcmp(n, "Etc/GMT+5"));
    assert(dm_tz_name_from("UTC", n, sizeof n) && !strcmp(n, "UTC"));
    strcpy(n, "keep");
    assert(!dm_tz_name_from("PST8PDT,M3.2.0,M11.1.0", n, sizeof n) && !strcmp(n, "keep"));   // a POSIX rule, no name
    assert(!dm_tz_name_from("<+09>-9", n, sizeof n) && !dm_tz_name_from("", n, sizeof n) && !dm_tz_name_from(NULL, n, sizeof n));
    assert(!dm_tz_name_from("/etc/localtime", n, sizeof n) && !dm_tz_name_from("../../etc/passwd", n, sizeof n));
    assert(!dm_tz_name_from("America/A_name_far_too_long_for_LocationName", n, sizeof n));   // 0x24 bytes with the NUL
    // Links as the systems lay them out: macOS links /etc/localtime into a zoneinfo folder; iOS links it to a second link, which links into one
    {
        char d[] = "/tmp/t_time_zone.XXXXXX", p[3][300]; assert(mkdtemp(d));
        snprintf(p[0], sizeof p[0], "%s/zoneinfo", d); assert(!mkdir(p[0], 0700));
        snprintf(p[0], sizeof p[0], "%s/zoneinfo/Pacific", d); assert(!mkdir(p[0], 0700));
        snprintf(p[0], sizeof p[0], "%s/zoneinfo/Pacific/Auckland", d); FILE *f = fopen(p[0], "w"); if (!f) { perror(p[0]); return 1; } fclose(f);
        snprintf(p[1], sizeof p[1], "%s/localtime", d); assert(!symlink(p[0], p[1]));       // iOS: localtime → .../zoneinfo/Pacific/Auckland
        snprintf(p[2], sizeof p[2], "%s/etc-localtime", d); assert(!symlink(p[1], p[2]));   // /etc/localtime → .../localtime
        assert(dm_tz_name_from_link(p[1], n, sizeof n) && !strcmp(n, "Pacific/Auckland"));   // one link (macOS)
        assert(dm_tz_name_from_link(p[2], n, sizeof n) && !strcmp(n, "Pacific/Auckland"));   // two links (iOS)
        assert(!dm_tz_name_from_link(d, n, sizeof n));                                       // no link, no zoneinfo folder
        char nl[320]; snprintf(nl, sizeof nl, "%s/none", d); assert(!dm_tz_name_from_link(nl, n, sizeof n));
        unlink(p[2]); unlink(p[1]); unlink(p[0]);
        snprintf(p[0], sizeof p[0], "%s/zoneinfo/Pacific", d); rmdir(p[0]); snprintf(p[0], sizeof p[0], "%s/zoneinfo", d); rmdir(p[0]); rmdir(d);
    }
    // The device location: TZ first, zero-filled past the name
    char loc[DM_LOCATION_NAME_SIZE];
    memset(loc, 0x55, sizeof loc); zone("Asia/Tokyo"); dm_device_location_name(loc);
    assert(!strcmp(loc, "Asia/Tokyo")); for (size_t i = strlen(loc); i < sizeof loc; i++) assert(!loc[i]);
    zone("UTC"); dm_device_location_name(loc); assert(!strcmp(loc, "UTC"));   // DM_FIXED_TIME's zone (main.c)
    unsetenv("TZ"); tzset(); dm_device_location_name(loc); assert(*loc && !strchr(loc, ','));   // the system zone, whatever this machine's is

    int64_t t[2];
    // Los Angeles: 1 November 2026, 01:00-01:59 happens twice (PDT, then PST): both, earliest first
    zone("America/Los_Angeles");
    assert(dm_local_to_posix(2026, 11, 1, 1, 30, 0, t) == 2 && t[0] == utc(2026, 11, 1, 8, 30) && t[1] == utc(2026, 11, 1, 9, 30));
    assert(dm_local_to_posix(2026, 11, 1, 0, 59, 0, t) == 1 && t[0] == utc(2026, 11, 1, 7, 59));   // just before: PDT only
    assert(dm_local_to_posix(2026, 11, 1, 2, 0, 0, t) == 1 && t[0] == utc(2026, 11, 1, 10, 0));    // just after: PST only
    assert(dm_local_to_posix(2026, 7, 1, 12, 0, 0, t) == 1 && t[0] == utc(2026, 7, 1, 19, 0));     // summer
    assert(dm_local_to_posix(2026, 1, 15, 12, 0, 0, t) == 1 && t[0] == utc(2026, 1, 15, 20, 0));   // winter
    // 8 March 2026, 02:30 never happens (02:00 → 03:00): one answer, mktime's, as before the change
    { struct tm m = {.tm_year = 126, .tm_mon = 2, .tm_mday = 8, .tm_hour = 2, .tm_min = 30, .tm_isdst = -1}; int64_t want = (int64_t)mktime(&m);
      assert(dm_local_to_posix(2026, 3, 8, 2, 30, 0, t) == 1 && t[0] == want); }
    // Berlin: 25 October 2026, 02:00-02:59 twice (CEST, then CET)
    zone("Europe/Berlin");
    assert(dm_local_to_posix(2026, 10, 25, 2, 15, 0, t) == 2 && t[0] == utc(2026, 10, 25, 0, 15) && t[1] == utc(2026, 10, 25, 1, 15));
    // Lord Howe Island: the clocks go back half an hour (5 April 2026, 02:00 → 01:30), so 01:30-01:59 happens twice, 30 minutes apart
    zone("Australia/Lord_Howe");
    assert(dm_local_to_posix(2026, 4, 5, 1, 45, 0, t) == 2 && t[1] - t[0] == 30 * 60);
    assert(dm_local_to_posix(2026, 4, 5, 1, 15, 0, t) == 1);
    // A zone without daylight saving time, and UTC: one answer always
    zone("Asia/Tokyo");
    assert(dm_local_to_posix(2026, 11, 1, 1, 30, 0, t) == 1 && t[0] == utc(2026, 10, 31, 16, 30));
    zone("UTC");
    assert(dm_local_to_posix(2000, 2, 29, 23, 59, 59, t) == 1 && t[0] == utc(2000, 2, 29, 23, 59) + 59);
    // Out-of-range fields: mktime normalizes them, as before
    assert(dm_local_to_posix(2026, 1, 31, 24, 0, 0, t) == 1 && t[0] == utc(2026, 2, 1, 0, 0));
    puts("t_time_zone: ok");
    return 0;
}
