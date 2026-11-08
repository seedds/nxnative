// The game clock offset (hle_time.c, the Mac's Clock menu): DM_TIME_OFFSET at start, then dm_time_set_offset, move every user clock read and not the steady clock,
// and the game sees the move as a clock change by the user
#include "../hle_time.c"

uint64_t guest_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void *guest_sym(const char *name) { (void)name; return NULL; }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static int64_t get64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
static int near(int64_t a, int64_t b) { return llabs(a - b) <= 1; }   // reads a second apart may straddle a tick
int main(void) {
    unsetenv("DM_FIXED_TIME");
    setenv("DM_TIME_OFFSET", "86400", 1);
    int64_t t = 0, ts[2], steady0, steady1; uint8_t tp[0x18], a[0xd0], b[0xd0];
    CHECK(!dm_time_fixed(), "no DM_FIXED_TIME");
    CHECK(dm_time_offset() == 86400, "DM_TIME_OFFSET sets the offset at start");
    h_time_GetCurrentTime(&t); CHECK(near(t, time(NULL) + 86400), "GetCurrentTime runs a day ahead");
    h_time_timespec(ts); CHECK(near(ts[0], time(NULL) + 86400), "the C library's real-time clock runs a day ahead");
    CHECK(near(h_time_now(), time(NULL) + 86400), "StandardUserSystemClock::now runs a day ahead");
    h_time_AdjGetCurrentTime(&t); CHECK(near(t, time(NULL) + 86400), "the adjustable user clock follows");
    h_time_SteadyNow(tp); steady0 = get64(tp, 0);
    CHECK(near(steady0, (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull)), "the steady clock is not moved");
    h_time_Snapshot(a);
    dm_time_set_offset(-3600);   // the Clock menu: an hour behind
    CHECK(dm_time_offset() == -3600, "dm_time_set_offset replaces the offset");
    h_time_GetCurrentTime(&t); CHECK(near(t, time(NULL) - 3600), "GetCurrentTime follows the new offset");
    h_time_Snapshot(b);
    CHECK(near(h_time_UserDiff(a, b) / 1000000000, -90000), "the game sees the user move the clock back 25 hours");
    h_time_SteadyNow(tp); steady1 = get64(tp, 0); CHECK(near(steady1, steady0), "the steady clock is still not moved");
    dm_time_set_offset(0);   // Match System Clock
    h_time_GetCurrentTime(&t); CHECK(near(t, time(NULL)), "offset 0 is the system clock");
    printf("time offset: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
