// nn::time: a real device gets the network-calibrated user clock through the time service; the host reads the system clock directly (single machine, offline).
// Layout follows the public Horizon ABI: PosixTime = int64 seconds; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 day of week, u32 day of year, char time zone name[8], u32 DST, s32 UTC offset in seconds}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;
static const int64_t ns_per_second = 1000000000;

// DM_FIXED_TIME=seconds (for deterministic comparison): the user clock always returns this value (and main.c pins the host time zone to UTC)
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) { const char *e = getenv("DM_FIXED_TIME"); v = e ? strtoll(e, 0, 0) : -1;
                   if (v >= 0) LOG("DM_FIXED_TIME=%lld: the user clock returns a fixed value", (long long)v); }
    return v;
}
HLE_FORCE("_ZN2nn4time10InitializeEv", "connect to the time service; clocks come from the host system time instead", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "the time service is taken over, always initialized", int, h_time_IsInitialized, void) { return 1; }
// Steady clock (seconds since boot + a clock source ID, a 16-byte UUID): the host's monotonic seconds with a fixed source ID. GetSpanBetween runs as is in sdk: it subtracts only when the source IDs match
static void write_steady_point(uint8_t *out, int64_t seconds) {
    memcpy(out, &seconds, 8); memcpy(out + 8, "dmhost-steady-01", 16);
}
HLE_FORCE("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock → host monotonic clock (seconds), fixed source ID", Result, h_time_SteadyNow, uint8_t *out) {
    write_steady_point(out, (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / ns_per_second));
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time38GetStandardSteadyClockCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock (the free function form, SDK 20) → host monotonic clock", Result, h_time_SteadyNowFn, uint8_t *out) { return h_time_SteadyNow(out); }
// The C++ clock form, a time_point of seconds in x0: the SDK asks the time service's steady-clock session (a global that time::Initialize fills) for GetCurrentTimePoint and returns 0
// without one (SDK 3.7, sdk+0x16edf4 in Mega Man Legacy Collection). Initialize is taken over and makes no session, so the clock never moved: the same seconds as GetCurrentTimePoint
HLE_FORCE("_ZN2nn4time19StandardSteadyClock3nowEv", "steady clock (std::chrono form) → host monotonic clock (seconds)", int64_t, h_time_SteadyClockNow, void) {
    uint8_t tp[24]; h_time_SteadyNow(tp); int64_t s; memcpy(&s, tp, 8); return s;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "user clock → host time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time(); *out = ft >= 0 ? ft : time(NULL); return RES_OK;
}
// The adjustable user clock (Animal Crossing: New Horizons 3.0.3, whose days follow it): the SDK keeps it in a MonotonicSystemClock that Adjust sets from the user clock's context, and its
// GetCurrentTime reads the time service's shared memory, which the host never maps (an abort in MonotonicSystemClock::GetCurrentTime, sdk+0x4421cc on that build). On the host the user clock
// is the host's own clock, so it is this one too; Adjust runs natively (it reads the user clock context, a host take-over) and only sets the SDK's copy. Its other readers and the
// adjustable network clock's follow the user clock context below
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "adjustable user clock → host time(), as the user clock", Result, h_time_AdjGetCurrentTime, int64_t *out) { return h_time_GetCurrentTime(out); }
// The network clock: a Switch keeps it from its last time sync, so one that has ever been online reads it offline too. The host's clock is set from the network, so on the host the
// network clock is the user clock, in every reader: GetCurrentTime here, now() below and the clock snapshot's network fields. Games date their saves by it: Divinity: Original Sin 2
// failed its read here, kept its PosixTime 2, and showed every save as 18:00:02 31/12/1969, so Continue, which takes the newest, loaded an older save (PR #2359)
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "network clock → host time(), as the user clock (the host's clock is network-set)", Result, h_time_NetNow, int64_t *out) {
    Result r = h_time_GetCurrentTime(out);
    static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("time: the network clock's first read: %lld (the host's clock)", (long long)*out);
    return r;
}
// The C library's real-time clock (SDK 17): clock_gettime(CLOCK_REALTIME) calls the source that nntimeStartup selects once time::Initialize succeeds (sdk+0x8e414: 1 = the standard user clock,
// 2 = the adjustable one; 0, the system tick, before that), and std::chrono::system_clock::now() throws on its failure. The SDK's getters ask the time service: Ace Attorney Investigations
// Collection called std::terminate at its first real-time read, right after the first-boot language dialog (main+0x3f3f68; found by armyja, GitHub issue #18). The host user clock, to the nanosecond
static int h_time_timespec(int64_t *ts) {
    int64_t ft = fixed_time();
    if (ft >= 0) { ts[0] = ft; ts[1] = 0; return 0; }
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); ts[0] = t.tv_sec; ts[1] = t.tv_nsec; return 0;
}
HLE_FORCE("_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the user clock → host real time", int, h_time_StdTimespec, int64_t *ts) { return h_time_timespec(ts); }
HLE_FORCE("_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the adjustable user clock → host real time", int, h_time_AdjTimespec, int64_t *ts) { return h_time_timespec(ts); }
// The C library's monotonic clock (SDK 22): musl's clock_gettime dispatches CLOCK_MONOTONIC to the steady clock through an ifunc over this symbol, whose SDK implementation asks the time service's
// shared memory, which the host never maps. Unbound, the call fails and std::chrono::steady_clock::now() throws std::system_error: Well Dweller reached that at its first frame timer read (sdk+0x5f2e58
// → std::terminate). The host's monotonic clock is the steady clock here, so clock_gettime and the steadiness agree
static int h_time_mono_timespec(int64_t *ts) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); ts[0] = t.tv_sec; ts[1] = t.tv_nsec; return 0;
}
HLE_FORCE("_ZN2nn4time37GetStandardSteadyClockCurrentTimespecEP8timespec", "C library monotonic clock from the steady clock → host monotonic time", int, h_time_MonoTimespec, int64_t *ts) { return h_time_mono_timespec(ts); }
typedef struct { int64_t wall_ns, steady_ns; } TimeSample;
static TimeSample sample_clocks(void) {
    int64_t before = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int64_t wall = clock_gettime_nsec_np(CLOCK_REALTIME);
    int64_t after = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    return (TimeSample){wall, before + (after - before) / 2};
}
// The context is an s64 offset and the steady time point where it was set. Subtract the
// paired clocks before rounding to the SDK's whole seconds. Rounding each one first
// made the offset depend on the phase of the first read after each process start.
static TimeSample read_clock_context(uint8_t *out, int64_t fixed) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint8_t ctx[0x20];
    static int64_t anchor_offset;
    static int made;
    pthread_mutex_lock(&mu);
    TimeSample sample = sample_clocks();
    int64_t delta_ns = sample.wall_ns - sample.steady_ns;
    int64_t steady = sample.steady_ns / ns_per_second;
    int64_t off = fixed >= 0 ? fixed - steady :
        (delta_ns + (delta_ns >= 0 ? ns_per_second / 2 : -ns_per_second / 2)) / ns_per_second;
    // Preserve the existing whole-second threshold. Use the snapshot's own steady
    // clock so pausing guest ticks cannot look like a wall-clock adjustment.
    if (!made || (fixed < 0 && llabs(off - anchor_offset) > 1)) {
        if (made) LOG("time: host clock moved %+lld s against the steady clock, new user clock context",
                      (long long)(off - anchor_offset));
        memcpy(ctx, &off, 8);
        write_steady_point(ctx + 8, steady);
        anchor_offset = off;
        made = 1;
    }
    memcpy(out, ctx, sizeof ctx);
    pthread_mutex_unlock(&mu);
    return sample;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "user clock context from the time service; the host's user clock over its steady clock, renewed when the host clock is set",
          Result, h_time_GetContext, uint8_t *out) {
    read_clock_context(out, fixed_time());
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock3nowEv", "user clock → host time()", int64_t, h_time_now, void) {
    int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock9to_time_tERKNSt3__16chrono10time_pointINS0_17SystemClockTraitsENS3_8durationIlNS2_5ratioILl1ELl1EEEEEEE",
          "the SDK implementation pairs with now(), taken over with it", int64_t, h_time_to_time_t, const int64_t *tp) { return *tp; }
// The network clock's now(), as the user clock's: the SDK's never calls GetCurrentTime, it reads the time service once its own initialized flag is set, which the host's Initialize
// take-over never sets, and returns 0 (1970) otherwise (Ocarina of Time's sdk+0x5d8490; its main imports it). Its to_time_t only reads the time point and needs nothing
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock3nowEv", "network clock → host time(), as the user clock", int64_t, h_time_net_now, void) { return h_time_now(); }
// The network clock's context, as the user clock's (the snapshot's +0x20 carries the same). The SDK asks the time service, which the host never opens, and gets 0x74;
// AdjustableNetworkSystemClock::Adjust aborts on that (METAL GEAR SOLID: Peace Walker - Master Collection Version 1.3.1, SDK 19, sdk+0x42c514, PR #1875)
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "network clock context, as the user clock's (the host's clock is network-set)",
          Result, h_time_NetContext, uint8_t *out) { return h_time_GetContext(out); }
// The adjustable clocks (user and network) keep a copy of their standard clock's context in the SDK's detail::MonotonicSystemClock, which Adjust sets and the readers turn into a
// time through the time service's shared memory, which the host never maps: MonotonicSystemClock::IsAdjusted, GetCurrentTime and GetCurrentTimespec abort (diag::detail::AbortImpl)
// when it is not mapped. Every adjustable network reader asks IsAdjusted first; the adjustable user clock's now() reads GetCurrentTime. On the host each adjustable clock is its standard
// clock, which is the host's clock, in every reader. Adjust itself runs as is: it reads the standard clock's context (a take-over) and only sets the SDK's copy
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock3nowEv", "adjustable user clock now() → host time(), as the user clock", int64_t, h_time_adj_now, void) { return h_time_now(); }
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "adjustable user clock context, as the user clock's",
          Result, h_time_AdjContext, uint8_t *out) { return h_time_GetContext(out); }
HLE_FORCE("_ZN2nn4time28AdjustableNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "adjustable network clock → host time(), as the network clock", Result, h_time_AdjNetNow, int64_t *out) { return h_time_NetNow(out); }
HLE_FORCE("_ZN2nn4time28AdjustableNetworkSystemClock3nowEv", "adjustable network clock now() → host time(), as the network clock", int64_t, h_time_adj_net_now, void) { return h_time_now(); }
HLE_FORCE("_ZN2nn4time28AdjustableNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "adjustable network clock context, as the network clock's",
          Result, h_time_AdjNetContext, uint8_t *out) { return h_time_GetContext(out); }
// The C library's real-time getters over the network clocks (SDK 17): the standard one reads the time service's shared memory (0x74 without it), the adjustable one its MonotonicSystemClock
HLE_FORCE("_ZN2nn4time44GetStandardNetworkSystemClockCurrentTimespecEP8timespec", "real time from the network clock → host real time, as the user clock", int, h_time_NetTimespec, int64_t *ts) { return h_time_timespec(ts); }
HLE_FORCE("_ZN2nn4time46GetAdjustableNetworkSystemClockCurrentTimespecEP8timespec", "real time from the adjustable network clock → host real time, as the user clock", int, h_time_AdjNetTimespec, int64_t *ts) { return h_time_timespec(ts); }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "time zone rules of the time service; uses the host localtime_r instead", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt; struct tm tm; localtime_r(&t, &tm);
    static int once; if (!once++) LOG("ToCalendarTime: first conversion %lld → %04d-%02d-%02d %02d:%02d (time zone %s, offset %ld s)", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) { memset(ai, 0, sizeof *ai); ai->wday = tm.tm_wday; ai->yday = tm.tm_yday; strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
              ai->dst = tm.tm_isdst > 0; ai->offset = (int32_t)tm.tm_gmtoff; }
    return RES_OK;
}
// Explicit time zone rules (Monster Hunter Rise, SDK 11): LoadTimeZoneRule reads the rule from the time service; the host keeps the game's rule buffer untouched and converts with localtime_r as above
HLE_FORCE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", "time zone rule from the time service; the host converts with its own zone, the rule stays unused", Result, h_time_LoadRule,
          void *rule, const char *name) { (void)rule; LOG("time::LoadTimeZoneRule %.36s (host time zone used instead)", name ? name : "?"); return RES_OK; }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", "conversion with an explicit time zone rule; same as the device zone (host localtime_r)",
          Result, h_time_ToCalendarTimeRule, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt, const void *rule) { (void)rule; return h_time_ToCalendarTime(ct, ai, pt); }
// Whether the network clock's last sync is recent enough (the service compares the steady time point of its context with now). The host does not know when its clock last synced,
// so it never claims it: the answer a Switch gives when its last sync is old
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", "network clock accuracy (time service): the host does not know its last sync, never sufficient", unsigned, h_time_NetAccurate, void) { return 0; }
// Clock snapshot (Tears of the Kingdom: CreateWithStandardSystemClock asks the time service, which the host never opens → abort in sm::Initialize). 0xD0 bytes, laid out
// as Ryujinx lays out the service's ClockSnapshot and as this SDK's constructor clears it (its last store is the byte at +0xcd): +0x00 user clock context {offset,
// steady time point}, +0x20 network clock context, +0x40 user time, +0x48 network time, +0x50 / +0x58 user / network calendar, +0x60 / +0x78 their additional info,
// +0x90 steady time point, +0xa8 location name (0x24), +0xcc automatic correction, +0xcd type. A steady time point is {int64 seconds, 16-byte source ID} (h_time_SteadyNow).
// The network fields are the user clock's, as the network clock is (h_time_NetNow): a Switch fills them whenever its network clock reads. Two host snapshots share their steady clock's
// source ID, so CalculateSpanBetween (below) takes their steady time points; one made on a Switch (a save brought over) has another, and the span comes from the network times
HLE_FORCE("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", "snapshot from the time service; the host fills it on the spot (network clock = user clock)", Result, h_time_Snapshot, uint8_t *s) {
    memset(s, 0, 0xd0);
    int64_t fixed = fixed_time();
    TimeSample sample = read_clock_context(s, fixed);
    int64_t now = fixed >= 0 ? fixed : sample.wall_ns / ns_per_second;
    memcpy(s + 0x40, &now, 8);
    h_time_ToCalendarTime((CalendarTime *)(s + 0x50), (CalendarAdditionalInfo *)(s + 0x60), &now);
    memcpy(s + 0x20, s, 0x20); memcpy(s + 0x48, &now, 8); memcpy(s + 0x58, s + 0x50, 8); memcpy(s + 0x78, s + 0x60, 0x18);   // the network clock's context, time, calendar and its info
    write_steady_point(s + 0x90, sample.steady_ns / ns_per_second);
    tzset(); strlcpy((char *)s + 0xa8, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);
    s[0xcd] = 1;   // The SDK passes type 1 for standard snapshots, 2 for adjustable snapshots; 0 is uninitialized.
    return RES_OK;
}
// The adjustable user clock already reads host time, so both snapshots share the fill path.
// Its SDK entry otherwise dereferences the time-service session, which the host never opens.
HLE_FORCE("_ZN2nn4time13ClockSnapshot31CreateWithAdjustableSystemClockEPS1_", "adjustable clock snapshot from host time; the host has no time service session", Result, h_time_AdjSnapshot, uint8_t *s) {
    h_time_Snapshot(s);
    s[0xcd] = 2;
    return RES_OK;
}
// Arithmetic on two snapshots: the SDK hands both to the time service (a call through its session, AbortImpl when there is none: STEINS;GATE RE:BOOT's and Tears of the Kingdom's SDK;
// Tears of the Kingdom imports both); the host computes what the service does (as Ryujinx's time service). TimeSpan is nanoseconds. CalculateSpanBetween returns bool, not a Result:
// true when the span was stored (SDK 12 to 20 end it with `cset w0, eq` on the service's result, Metroid Dread's sdk+0x3ab540, Tears of the Kingdom's sdk+0x440f60); a Result 0 read as false
// (Super Smash Bros. Ultimate's Spirit Board refilled at every visit, found by MacNativeGames)
static int64_t snap64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
static int same_source(const uint8_t *a, const uint8_t *b, int tp) { return !memcmp(a + tp + 8, b + tp + 8, 16); }   // the source IDs of the steady time points at +tp
HLE_FORCE("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", "span between two snapshots (time service): their steady time points, else their network times",
          unsigned, h_time_SpanBetween, int64_t *out, const uint8_t *a, const uint8_t *b) {
    int64_t s;
    if (same_source(a, b, 0x90)) s = snap64(b, 0x90) - snap64(a, 0x90);
    else if (snap64(a, 0x48) && snap64(b, 0x48)) s = snap64(b, 0x48) - snap64(a, 0x48);
    else return 0;   // no common clock: the service answers "not found", *out untouched (as the SDK leaves it)
    *out = s * 1000000000; return 1;
}
// How far the user moved the user clock between two snapshots (asked at Pokémon: Let's Go's first save, PR #36): the change of the user clock offset, 0 when the two contexts' steady
// sources differ or both snapshots have automatic correction on. The snapshots carry h_time_GetContext's context, renewed when the host clock is set, so that shows up here
HLE_FORCE("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", "user clock change between two snapshots (time service): from their user clock offsets",
          int64_t, h_time_UserDiff, const uint8_t *a, const uint8_t *b) {
    if (!same_source(a, b, 0x08) || (a[0xcc] && b[0xcc])) return 0;
    return (snap64(b, 0x00) - snap64(a, 0x00)) * 1000000000;
}
// A periodic benefit (Tetris 99 2.5.0's daily reward, asked right after its first save at the title): the public entry aborts unless g_pStaticService, the time service session that
// Initialize opens, is set (OnAssertionFailure from sdk+0x375b84 on its SDK 6.4 sdk), and the host's Initialize take-over opens none. Past that check it only tail-calls detail:: with the
// same arguments (SDK 6.5 and 20.5). detail:: compares the snapshots' fields (CalculateSpanBetween and the user clock difference above) and asks the service two things, both taken over below:
// when the context's snapshot and this one differ in automatic correction, when automatic correction was last switched; when they differ in location name, the device's location and when it was set
HLE_FORCE("_ZN2nn4time39ClaimPeriodicBenefitWithUserSystemClockEPNS0_27PeriodicBenefitClaimContextERKNS0_13ClockSnapshotERKNS_8TimeSpanES8_PKNS0_25CheckCalendarTimeCallbackE",
          "periodic benefit claim (time service session check); the SDK's detail implementation compares the snapshots", uint64_t, h_time_ClaimBenefit,
          void *ctx, const uint8_t *snap, const int64_t *max_penalty, const int64_t *acceptable, const void *callback) {
    static uint64_t (*impl)(void *, const uint8_t *, const int64_t *, const int64_t *, const void *);
    if (!impl && !(impl = guest_sym("_ZN2nn4time6detail39ClaimPeriodicBenefitWithUserSystemClockEPNS0_27PeriodicBenefitClaimContextERKNS0_13ClockSnapshotERKNS_8TimeSpanES9_PKNS0_25CheckCalendarTimeCallbackE")))
        die("time: the sdk has ClaimPeriodicBenefitWithUserSystemClock but not its detail:: implementation");
    uint64_t r = impl(ctx, snap, max_penalty, acceptable, callback);
    LOG("time: periodic benefit claim → %u", (unsigned)r);
    return r;
}
// The two service questions of the claim above (each through g_pStaticService in the SDK, which the host never sets). A saved claim context from a Switch (a location such as Asia/Tokyo where the
// host's is its zone abbreviation, automatic correction on) or a Mac whose time zone changed reaches them. The host knows neither moment, so the time point comes back cleared: its source ID matches
// no snapshot's, and the claim then uses only the snapshots (it takes the time point only when its source ID is the current snapshot's steady clock's, SDK 6.5's detail:: +0x2e8 and +0x3b0)
HLE_FORCE("_ZN2nn4time6detail56GetStandardUserSystemClockAutomaticCorrectionUpdatedTimeEPNS0_20SteadyClockTimePointE",
          "when automatic correction was last switched (time service); not known on the host: a time point of no clock", Result, h_time_AutoCorrUpdated, uint8_t *tp) {
    memset(tp, 0, 0x18); return RES_OK;
}
HLE_FORCE("_ZN2nn4time6detail35GetDeviceLocationNameAndUpdatedTimeEPNS0_12LocationNameEPNS0_20SteadyClockTimePointE",
          "device location and when it was set (time service): the host's zone, as in its snapshots, and a time point of no clock", Result, h_time_LocUpdated, char *name, uint8_t *tp) {
    memset(name, 0, 0x24); tzset(); strlcpy(name, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);
    memset(tp, 0, 0x18); return RES_OK;
}
// The reverse: local calendar → absolute seconds (13 Sentinels). Around a daylight-saving switch one local time maps to 0 or 2 absolute seconds; the host's mktime gives one
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE",
          "time zone rules of the time service; uses the host mktime instead", Result, h_time_ToPosixTime, int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t t = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)t;
    return RES_OK;
}
// With an explicit time zone rule (Pokémon Legends: Z-A, SDK 19; the rule came from the host's LoadTimeZoneRule above and stays unused): the device zone, as for ToCalendarTime
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", "conversion with an explicit time zone rule; same as the device zone (host mktime)",
          Result, h_time_ToPosixTimeRule, int *count, int64_t *out, int n, const CalendarTime *ct, const void *rule) { (void)rule; return h_time_ToPosixTime(count, out, n, ct); }
// UTC calendar → absolute seconds (Super Mario Odyssey); no time zone involved, the host's timegm
HLE_FORCE("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", "UTC calendar → absolute seconds; host timegm", int64_t, h_time_ToPosixUtc, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second};
    return (int64_t)timegm(&tm);
}
// The C library's localtime / localtime_r / mktime are not taken over: on a Switch they are UTC. The SDK's musl time zone helper (__secs_to_zone) is a fixed stub, offset 0, no DST, name "UTC",
// in every sdk checked from SDK 3.5.0 to 22.2.10, so its mktime(gmtime_r(t)) == t. Local time on a Switch comes only from nn::time (ToCalendarTime / ToPosixTime above, in the host's zone).
// The host once forced them onto its own local-time functions: Hades II then read its save times' UTC struct tm as local time and showed them shifted by the host's offset (issue #2784)
