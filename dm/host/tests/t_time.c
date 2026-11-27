// Clock snapshot arithmetic (hle_time.c, from PR #36): CalculateSpanBetween and CalculateStandardUserSystemClockDifferenceByUser looked up by their mangled names in the real swforce
// registrations, run on both host snapshot entries and on edited copies; the C library's local-time functions, UTC as on a Switch, against nn::time's local conversion (issue #2784)
#include "../hle.h"
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <mach-o/getsect.h>

static uint64_t test_wall_ns, test_steady_ns, test_guest_ns;
static uint64_t test_clock_gettime_nsec_np(clockid_t clock) {
    if (!test_wall_ns) return clock_gettime_nsec_np(clock);
    return clock == CLOCK_REALTIME ? test_wall_ns : test_steady_ns;
}
static time_t test_time(time_t *out) {
    if (!test_wall_ns) return time(out);
    time_t seconds = test_wall_ns / 1000000000ull;
    if (out) *out = seconds;
    return seconds;
}
#define clock_gettime_nsec_np test_clock_gettime_nsec_np
#define time test_time
#include "../hle_time.c"
#undef time
#undef clock_gettime_nsec_np

uint64_t guest_ns(void) { return test_wall_ns ? test_guest_ns : clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static int checks, fails;
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) if (!strcmp(e[i].name, name)) { count++; fn = (void *)e[i].fn; }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
static void *section_fn(const char *sect, const char *name, unsigned *count) {
    unsigned long bytes = 0; void *fn = NULL; *count = 0;
    const hle_ent *e = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", sect, &bytes);   // hle_force_ent and hle_ent both begin {name, fn}
    size_t step = !strcmp(sect, "swforce") ? sizeof(hle_force_ent) : sizeof(hle_ent);
    for (size_t i = 0; e && i < bytes / step; i++) { const hle_ent *x = (const hle_ent *)((const char *)e + i * step); if (!strcmp(x->name, name)) { ++*count; fn = x->fn; } }
    return fn;
}
// The C library's local time is UTC (the SDK's own functions run; the pass-through table maps to the host's UTC ones), nn::time's is the host zone's
static void check_utc_libc(void) {
    static const char *names[] = {"localtime", "localtime_r", "mktime"}; unsigned n;
    for (int i = 0; i < 3; i++) { section_fn("swforce", names[i], &n); CHECK(n == 0, "the SDK's localtime / localtime_r / mktime are not taken over"); }
    setenv("TZ", "Asia/Seoul", 1); tzset();
    int64_t (*mk)(struct tm *) = section_fn("swhle", "mktime", &n);
    struct tm *(*lt)(const int64_t *, struct tm *) = section_fn("swhle", "localtime_r", &n);
    const int64_t save = 1794761043;   // Hades II's save header time, 2026-11-15 16:44:03 UTC
    struct tm u; time_t sv = (time_t)save; gmtime_r(&sv, &u);
    CHECK(mk && mk(&u) == save, "mktime of a UTC struct tm gives the time back unchanged (TZ=Asia/Seoul)");
    struct tm l; CHECK(lt && lt(&save, &l) == &l && l.tm_hour == 16 && l.tm_min == 44 && l.tm_gmtoff == 0, "localtime_r is UTC (TZ=Asia/Seoul)");
    Result (*cal)(CalendarTime *, CalendarAdditionalInfo *, const int64_t *) = hook("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE");
    CalendarTime ct; CalendarAdditionalInfo ai;
    CHECK(cal && cal(&ct, &ai, &save) == RES_OK && ct.day == 16 && ct.hour == 1 && ct.minute == 44 && ai.offset == 9 * 3600, "ToCalendarTime converts in the host zone (01:44 KST)");
    setenv("TZ", "UTC", 1); tzset();
}
// The SDK's detail:: periodic benefit claim, as guest_sym finds it: records what the host's public entry passed on and answers 7
static const void *claim_args[5];
static uint64_t fake_claim(void *ctx, const uint8_t *snap, const int64_t *max_penalty, const int64_t *acceptable, const void *callback) {
    claim_args[0] = ctx; claim_args[1] = snap; claim_args[2] = max_penalty; claim_args[3] = acceptable; claim_args[4] = callback; return 7;
}
void *guest_sym(const char *name) {
    return !strcmp(name, "_ZN2nn4time6detail39ClaimPeriodicBenefitWithUserSystemClockEPNS0_27PeriodicBenefitClaimContextERKNS0_13ClockSnapshotERKNS_8TimeSpanES9_PKNS0_25CheckCalendarTimeCallbackE") ? (void *)fake_claim : NULL;
}
static void put64(uint8_t *s, int off, int64_t v) { memcpy(s + off, &v, 8); }
static int64_t get64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
static void check_sample_phase(uint64_t phase) {
    const int64_t offset = 1794821231;
    pid_t pid = fork();
    if (pid == 0) {
        unsetenv("DM_FIXED_TIME");
        test_steady_ns = 300000000000000ull + phase;
        test_guest_ns = test_steady_ns;
        test_wall_ns = test_steady_ns + offset * 1000000000ull + 400000000ull;
        uint8_t a[0xd0], b[0xd0];
        h_time_Snapshot(a);
        int phase_stable = get64(a, 0) == offset;
        test_wall_ns += 1000000000ull;
        h_time_Snapshot(b);
        int threshold = !memcmp(a, b, 0x20);
        test_wall_ns += 1000000000ull;
        h_time_Snapshot(b);
        int forward = h_time_UserDiff(a, b) == 2000000000ll;
        test_wall_ns -= 4000000000ull;
        h_time_Snapshot(b);
        int backward = h_time_UserDiff(a, b) == -2000000000ll;
        // Simulate an elapsed interval with frozen guest ticks.
        memcpy(a, b, sizeof a);
        test_steady_ns += 60000000000ull;
        test_wall_ns += 60000000000ull;
        h_time_Snapshot(b);
        int elapsed = !memcmp(a, b, 0x20) && get64(b, 0x90) - get64(a, 0x90) == 60;
        _exit((!phase_stable) | ((!threshold) << 1) | ((!forward) << 2) |
              ((!backward) << 3) | ((!elapsed) << 4));
    }
    int status;
    int result = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : 31;
    CHECK(!(result & 1), "same wall-to-steady separation gives one offset across fresh processes");
    CHECK(!(result & 2), "a one-second change keeps the existing context");
    CHECK(!(result & 4), "a forward wall-clock adjustment beyond the threshold is reported");
    CHECK(!(result & 8), "a backwards wall-clock adjustment beyond the threshold is reported");
    CHECK(!(result & 16), "elapsed time with suspended guest ticks does not change the user clock");
}
int main(void) {
    check_sample_phase(250000000ull);
    check_sample_phase(750000000ull);
    Result (*snap)(uint8_t *) = hook("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_");
    Result (*adj_snap)(uint8_t *) = hook("_ZN2nn4time13ClockSnapshot31CreateWithAdjustableSystemClockEPS1_");
    unsigned (*span)(int64_t *, const uint8_t *, const uint8_t *) = hook("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_");
    int64_t (*udiff)(const uint8_t *, const uint8_t *) = hook("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_");
    check_utc_libc();
    if (!snap || !adj_snap || !span || !udiff) { printf("time: %d checks, %d failed\n", checks, fails); return 1; }
    uint8_t a[0xd0], b[0xd0], c[0xd0]; int64_t ts = -1;
    // Before the first snapshot: h_time_GetCurrentTime reads DM_FIXED_TIME once and caches it
    setenv("DM_FIXED_TIME", "1700000000", 1);
    setenv("TZ", "UTC", 1); tzset();
    CHECK(snap(a) == RES_OK, "snapshot a");
    CHECK(a[0xcd] == 1, "standard snapshot type is 1");
    // Both entries must stay inside the SDK's 0xd0-byte buffer, including the type near its end.
    uint8_t guarded[0xf0]; memset(guarded, 0xa5, sizeof guarded);
    uint8_t *adjustable = guarded + 0x10;
    CHECK(snap(adjustable) == RES_OK, "standard snapshot in guarded buffer");
    int standard_guards_ok = 1;
    for (int i = 0; i < 0x10; i++) standard_guards_ok &= guarded[i] == 0xa5 && guarded[0xe0 + i] == 0xa5;
    CHECK(standard_guards_ok, "standard snapshot writes only its 0xd0 bytes");
    memset(guarded, 0xa5, sizeof guarded);
    CHECK(adj_snap(adjustable) == RES_OK, "adjustable snapshot");
    int guards_ok = 1;
    for (int i = 0; i < 0x10; i++) guards_ok &= guarded[i] == 0xa5 && guarded[0xe0 + i] == 0xa5;
    CHECK(guards_ok, "adjustable snapshot writes only its 0xd0 bytes");
    CHECK(adjustable[0xcd] == 2, "adjustable snapshot type is 2");
    CHECK(get64(adjustable, 0x40) == 1700000000, "adjustable user time follows the host clock");
    // StandardSteadyClock::now(), the C++ clock form (SDK 3.7: 0 without the time service session): the steady clock's seconds, as GetCurrentTimePoint gives them
    int64_t (*steady_now)(void) = hook("_ZN2nn4time19StandardSteadyClock3nowEv");
    Result (*steady_tp)(uint8_t *) = hook("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE");
    uint8_t tp0[24]; int64_t sn = steady_now ? steady_now() : -1;
    CHECK(steady_tp && steady_tp(tp0) == RES_OK && sn > 0 && llabs(sn - get64(tp0, 0)) <= 1, "StandardSteadyClock::now() is the steady clock's seconds, not 0");
    Result (*net)(int64_t *) = hook("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    int64_t nt = -1;
    CHECK(net && net(&nt) == RES_OK && nt == 1700000000, "the network clock reads the host clock, as the user clock (saves are dated by it)");
    const CalendarTime *cal = (const CalendarTime *)(adjustable + 0x50);
    CHECK(cal->year == 2023 && cal->month == 11 && cal->day == 14 && cal->hour == 22 && cal->minute == 13 && cal->second == 20,
          "adjustable snapshot has the UTC calendar for the fixed time");
    CHECK(!memcmp(adjustable + 0x20, adjustable, 0x20) && get64(adjustable, 0x48) == 1700000000 && !memcmp(adjustable + 0x58, adjustable + 0x50, 8) &&
          !memcmp(adjustable + 0x78, adjustable + 0x60, 0x18), "snapshot network fields are the user clock's (context, time, calendar, its info)");
    int64_t (*net_now)(void) = hook("_ZN2nn4time26StandardNetworkSystemClock3nowEv");
    CHECK(net_now && net_now() == 1700000000, "the network clock's now() reads the host clock, as GetCurrentTime does");
    CHECK(!memcmp(a + 0x98, adjustable + 0x98, 16), "both snapshots use the same steady clock source");
    CHECK(udiff(a, adjustable) == 0, "changing snapshot type does not report a user clock change");
    CHECK(!strcmp((const char *)adjustable + 0xa8, "UTC"), "adjustable snapshot carries the host time zone");
    memcpy(b, a, sizeof b); b[0xcd] = 2;
    CHECK(span(&ts, a, b) == 1 && ts == 0, "changing only the type leaves the elapsed span at zero");
    unsetenv("DM_FIXED_TIME");
    CHECK(span(&ts, a, a) == 1 && ts == 0, "span of a snapshot with itself is 0");
    memcpy(b, a, sizeof b); put64(b, 0x90, get64(a, 0x90) + 90);   // b: 90 s of steady clock later
    CHECK(span(&ts, a, b) == 1 && ts == 90 * 1000000000LL, "span from the steady time points, in nanoseconds");
    CHECK(span(&ts, b, a) == 1 && ts == -90 * 1000000000LL, "span backwards is negative");
    memcpy(c, b, sizeof c); c[0x98] ^= 1; ts = 1234;   // c: another steady clock source, no network time
    put64(a, 0x48, 0); put64(c, 0x48, 0);   // as a snapshot made with no network clock
    CHECK(span(&ts, a, c) == 0 && ts == 1234, "different sources and no network time: false, output untouched");
    put64(a, 0x48, 1000); put64(c, 0x48, 1600);   // with network times the span comes from them
    CHECK(span(&ts, a, c) == 1 && ts == 600 * 1000000000LL, "different sources: span from the network times");
    CHECK(snap(a) == RES_OK && snap(b) == RES_OK && udiff(a, b) == 0, "two host snapshots in a row: the user did not move the clock");
    memcpy(b, a, sizeof b); put64(b, 0x00, get64(a, 0x00) + 3600);   // b: the user clock offset one hour later
    CHECK(udiff(a, b) == 3600 * 1000000000LL, "user clock moved an hour");
    b[0x10] ^= 1; CHECK(udiff(a, b) == 0, "user contexts of different steady sources: 0");
    b[0x10] ^= 1; a[0xcc] = b[0xcc] = 1; CHECK(udiff(a, b) == 0, "automatic correction on in both: 0");
    a[0xcc] = 0; CHECK(udiff(a, b) == 3600 * 1000000000LL, "automatic correction in one only: the difference");
    // The periodic benefit claim skips the SDK's time service session check and hands every argument to its detail:: implementation, whose answer it returns
    uint64_t (*claim)(void *, const uint8_t *, const int64_t *, const int64_t *, const void *) =
        hook("_ZN2nn4time39ClaimPeriodicBenefitWithUserSystemClockEPNS0_27PeriodicBenefitClaimContextERKNS0_13ClockSnapshotERKNS_8TimeSpanES8_PKNS0_25CheckCalendarTimeCallbackE");
    uint8_t ctx[0x100] = {0}; int64_t pen = 1, acc = 2; int cbk = 0;
    CHECK(claim && claim(ctx, a, &pen, &acc, &cbk) == 7, "periodic benefit claim returns the SDK implementation's answer");
    CHECK(claim_args[0] == ctx && claim_args[1] == a && claim_args[2] == &pen && claim_args[3] == &acc && claim_args[4] == &cbk, "periodic benefit claim passes every argument on");
    // Its two service questions: the location the host's snapshots carry, and time points of no clock (a source ID no snapshot has)
    Result (*auto_upd)(uint8_t *) = hook("_ZN2nn4time6detail56GetStandardUserSystemClockAutomaticCorrectionUpdatedTimeEPNS0_20SteadyClockTimePointE");
    Result (*loc_upd)(char *, uint8_t *) = hook("_ZN2nn4time6detail35GetDeviceLocationNameAndUpdatedTimeEPNS0_12LocationNameEPNS0_20SteadyClockTimePointE");
    uint8_t tp[0x18], none[0x18] = {0}; char loc[0x24];
    memset(tp, 0xaa, sizeof tp); CHECK(auto_upd && auto_upd(tp) == RES_OK && !memcmp(tp, none, sizeof tp), "automatic correction updated time: a cleared time point");
    memset(tp, 0xaa, sizeof tp); memset(loc, 0xaa, sizeof loc);
    CHECK(loc_upd && loc_upd(loc, tp) == RES_OK && !memcmp(loc, a + 0xa8, sizeof loc) && !memcmp(tp, none, sizeof tp), "device location: the snapshot's name, a cleared time point");
    CHECK(memcmp(tp + 8, a + 0x90 + 8, 16), "the cleared time point's source ID is not the snapshots' steady clock");
    // The network clock's context and the adjustable clocks (PR #1875): each is the user clock in every reader, its context the snapshot's, within the context's 0x20 bytes
    // (DM_FIXED_TIME is read once, above, so the clocks still read 1700000000)
    static const char *ctx_hooks[] = {
        "_ZN2nn4time26StandardNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE",
        "_ZN2nn4time25AdjustableUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE",
        "_ZN2nn4time28AdjustableNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE"};
    CHECK(snap(a) == RES_OK, "snapshot for the contexts");
    for (int h = 0; h < 3; h++) {
        Result (*get_ctx)(uint8_t *) = hook(ctx_hooks[h]);
        uint8_t g[0x40]; memset(g, 0xa5, sizeof g);
        int ok = get_ctx && get_ctx(g + 0x10) == RES_OK && !memcmp(g + 0x10, a, 0x20) && !memcmp(g + 0x10, a + 0x20, 0x20);
        for (int i = 0; i < 0x10; i++) ok &= g[i] == 0xa5 && g[0x30 + i] == 0xa5;
        CHECK(ok, ctx_hooks[h]);
    }
    Result (*adj_net)(int64_t *) = hook("_ZN2nn4time28AdjustableNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    nt = -1; CHECK(adj_net && adj_net(&nt) == RES_OK && nt == 1700000000, "the adjustable network clock reads the host clock");
    int64_t (*adj_now)(void) = hook("_ZN2nn4time25AdjustableUserSystemClock3nowEv");
    int64_t (*adj_net_now)(void) = hook("_ZN2nn4time28AdjustableNetworkSystemClock3nowEv");
    CHECK(adj_now && adj_now() == 1700000000 && adj_net_now && adj_net_now() == 1700000000, "the adjustable clocks' now() read the host clock");
    int (*net_ts)(int64_t *) = hook("_ZN2nn4time44GetStandardNetworkSystemClockCurrentTimespecEP8timespec");
    int (*adj_net_ts)(int64_t *) = hook("_ZN2nn4time46GetAdjustableNetworkSystemClockCurrentTimespecEP8timespec");
    int64_t tsp[2] = {-1, -1}, tsq[2] = {-1, -1};
    CHECK(net_ts && net_ts(tsp) == 0 && tsp[0] == 1700000000 && tsp[1] == 0 && adj_net_ts && adj_net_ts(tsq) == 0 && tsq[0] == 1700000000 && tsq[1] == 0,
          "the network clocks' real-time getters read the host clock");
    // Device location name (IANA name): GetDeviceLocationName, ClockSnapshot (+0xa8), and detail::GetDeviceLocationNameAndUpdatedTime
    Result (*get_loc)(char *) = hook("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE");
    if (get_loc) {
        // Guarded buffer and zero-fill past the NUL
        uint8_t lg[0x44]; memset(lg, 0xa5, sizeof lg);
        setenv("TZ", "Asia/Tokyo", 1); tzset();
        CHECK(get_loc((char *)lg + 0x10) == RES_OK && !strcmp((const char *)lg + 0x10, "Asia/Tokyo"), "GetDeviceLocationName honors IANA TZ");
        int loc_ok = 1;
        for (int i = 0; i < 0x10; i++) loc_ok &= lg[i] == 0xa5 && lg[0x34 + i] == 0xa5;
        for (size_t i = strlen("Asia/Tokyo"); i < LOCATION_NAME_SIZE; i++) loc_ok &= lg[0x10 + i] == 0;
        CHECK(loc_ok, "GetDeviceLocationName writes only 0x24 bytes and zero-fills past the name");
        // All three hooks agree on the IANA name
        CHECK(snap(a) == RES_OK && !memcmp(a + 0xa8, lg + 0x10, LOCATION_NAME_SIZE), "ClockSnapshot carries the IANA location name");
        memset(loc, 0x55, sizeof loc); memset(tp, 0x55, sizeof tp);
        CHECK(loc_upd && loc_upd(loc, tp) == RES_OK && !memcmp(loc, lg + 0x10, LOCATION_NAME_SIZE), "detail::GetDeviceLocationNameAndUpdatedTime agrees with GetDeviceLocationName");
        // Accepted TZ forms (plain, leading ':', zoneinfo path, UTC, multi-segment, Etc)
        static const struct { const char *in, *want; } valid_tz[] = {
            {"America/Los_Angeles", "America/Los_Angeles"},
            {":Asia/Tokyo", "Asia/Tokyo"},
            {"/var/db/timezone/zoneinfo/Europe/Berlin", "Europe/Berlin"},
            {"/usr/share/zoneinfo/America/Argentina/Buenos_Aires", "America/Argentina/Buenos_Aires"},
            {"Australia/Lord_Howe", "Australia/Lord_Howe"},
            {"Etc/GMT+5", "Etc/GMT+5"},
            {"UTC", "UTC"},
        };
        for (size_t i = 0; i < sizeof valid_tz / sizeof *valid_tz; i++) {
            char buf[LOCATION_NAME_SIZE]; memset(buf, 0x55, sizeof buf);
            int ok = location_name_from_tz(valid_tz[i].in, buf) && !strcmp(buf, valid_tz[i].want);
            for (size_t j = strlen(valid_tz[i].want); ok && j < LOCATION_NAME_SIZE; j++) ok &= buf[j] == 0;
            CHECK(ok, valid_tz[i].in);
        }
        // Rejected TZ forms (abbreviations, POSIX rules with or without commas, invalid zone, traversal, too long)
        static const char *const bad_tz[] = {
            NULL, "", "PST", "JST", "EST5EDT", "PST8PDT,M3.2.0,M11.1.0", "JST-9", "<+09>-9", "+08",
            "Not/A_Zone", "/etc/localtime", "../../etc/passwd", "America/A_name_far_too_long_for_LocationName",
        };
        for (size_t i = 0; i < sizeof bad_tz / sizeof *bad_tz; i++) {
            char buf[LOCATION_NAME_SIZE]; memset(buf, 'K', sizeof buf); buf[4] = 0;
            CHECK(!location_name_from_tz(bad_tz[i], buf) && !strcmp(buf, "KKKK"), bad_tz[i] ? bad_tz[i] : "(null)");
        }
        // Live system zone via CoreFoundation when TZ is unset or invalid
        unsetenv("TZ"); tzset();
        memset(loc, 0x55, sizeof loc);
        CHECK(get_loc(loc) == RES_OK && loc[0] && loc[LOCATION_NAME_SIZE - 1] == 0 &&
              (strchr(loc, '/') != NULL || !strcmp(loc, "UTC")), "unset TZ resolves system IANA zone via CoreFoundation");
        int sys_zero = 1;
        for (size_t i = strlen(loc); i < LOCATION_NAME_SIZE; i++) sys_zero &= loc[i] == 0;
        CHECK(sys_zero, "system IANA location is zero-filled past the NUL");
        CHECK(snap(a) == RES_OK && !memcmp(a + 0xa8, loc, LOCATION_NAME_SIZE), "ClockSnapshot agrees on live system zone");
        setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1); tzset();
        char fallback_loc[LOCATION_NAME_SIZE];
        CHECK(get_loc(fallback_loc) == RES_OK && !memcmp(fallback_loc, loc, LOCATION_NAME_SIZE), "POSIX rule TZ falls back to system IANA zone");
        setenv("TZ", "UTC", 1); tzset();
    }
    printf("time: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
