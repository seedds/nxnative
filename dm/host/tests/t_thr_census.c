// What the guest main thread's frames wait on and which host threads use the CPU outside the guest threads (thr_census.h), fed as hle_thread.c feeds it
#include "../thr_census.h"
#include <stdio.h>
#include <string.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-86s %s\n", what, ok ? "✓" : "✗"); }

int main(void) {
    // wait kinds: the SVCs that block, and SleepThread split into yields (0, -1, -2) and sleeps
    check(thr_wait_kind(0x34, 0) == TW_ADDRESS && thr_wait_kind(0x18, 0) == TW_SYNC && thr_wait_kind(0x1A, 0) == TW_LOCK && thr_wait_kind(0x1C, 0) == TW_CONDVAR, "WaitForAddress, WaitSynchronization, ArbitrateLock and the condition variable wait are waits");
    check(thr_wait_kind(0x0B, 0) == TW_YIELD && thr_wait_kind(0x0B, (uint64_t)-1) == TW_YIELD && thr_wait_kind(0x0B, (uint64_t)-2) == TW_YIELD, "SleepThread 0, -1 and -2 are yields");
    check(thr_wait_kind(0x0B, 1000000) == TW_SLEEP, "SleepThread of 1 ms is a sleep");
    check(thr_wait_kind(0x35, 0) < 0 && thr_wait_kind(0x26, 0) < 0, "SignalToAddress and GetInfo are not waits");

    {   // a window of 300 frames at a mean 70 ms: 50 ms of address waits a frame, 1 ms of yields, nothing else
        thr_waits was = {0}, now = {0};
        now.n[TW_ADDRESS] = 300 * 2400; now.ns[TW_ADDRESS] = 300ull * 50000000ull;
        now.n[TW_YIELD] = 300 * 5000; now.ns[TW_YIELD] = 300ull * 1000000ull;
        char b[512]; thr_waits_line(b, sizeof b, &now, &was, 300, 70.0);
        printf("  %s\n", b);
        check(strstr(b, "address waits 50.0 ms (2400)") != NULL, "the line gives each kind's time and count per frame");
        check(strstr(b, "address waits") < strstr(b, "yields 1.0 ms"), "the longest kind comes first");
        check(strstr(b, "sleeps") == NULL && strstr(b, "ArbitrateLock") == NULL, "kinds that took no time are left out");
        check(strstr(b, "51.0 ms in all, 73% of the 70.0 ms frame") != NULL, "their sum is set against the mean frame");
        thr_waits rare = was; rare.n[TW_SYNC] = 60; rare.ns[TW_SYNC] = 300000;   // one wait in 5 frames
        thr_waits_line(b, sizeof b, &rare, &was, 300, 33.3);
        check(strstr(b, "WaitSynchronization 0.0 ms (0.2)") != NULL, "a wait rarer than once a frame keeps a decimal in its count");
        was = now; thr_waits_line(b, sizeof b, &now, &was, 300, 33.3);
        check(strstr(b, "no waits") != NULL, "a window with no waits says so");
    }
    {   // address heavy hitters: the table keeps the addresses that took the most time; a new address takes the smallest slot when it is full
        thr_addr t[THR_ADDR_N]; memset(t, 0, sizeof t);
        for (int i = 0; i < 100; i++) thr_addr_add(t, 0x7017000000ull, 400000);   // 40 ms on the job counter
        thr_addr_add(t, 0x7018000000ull, 3000000);                                 // 3 ms once
        for (uint64_t a = 1; a <= THR_ADDR_N + 4; a++) thr_addr_add(t, 0x7100000000ull + a * 8, 1000);   // many small ones
        const thr_addr *a1 = NULL, *a2 = NULL; thr_addr_top2(t, &a1, &a2);
        check(a1 && a1->addr == 0x7017000000ull && a1->ns == 40000000ull && a1->n == 100, "the address with the most wait time is first, with its total and count");
        check(a2 && a2->addr == 0x7018000000ull, "the second is the next largest, kept while small ones come and go");
        thr_addr_reset(t); thr_addr_top2(t, &a1, &a2);
        check(!a1 && !a2, "a reset empties the table for the next window");
    }
    {   // the census: CPU per second of each host thread over the window, the busiest first; a thread born in the window counts all its CPU, one that ended counts nothing
        thr_sample was[3] = {{.tid = 10, .cpu_us = 1000000, .sys_us = 100000, .name = "NVN queue replay"},
                             {.tid = 11, .cpu_us = 5000000, .sys_us = 0, .name = "nvn-present"},
                             {.tid = 12, .cpu_us = 700000, .name = "gone"}};
        thr_sample cur[3] = {{.tid = 10, .cpu_us = 2600000, .sys_us = 300000, .name = "NVN queue replay"},   // +1.6 s, 0.2 s of it kernel
                             {.tid = 11, .cpu_us = 5100000, .sys_us = 0, .name = "nvn-present"},            // +0.1 s
                             {.tid = 13, .cpu_us = 8000000, .sys_us = 7000000, .gcd = 1, .pri = 47}};      // new: 8 s, 7 s kernel
        thr_rank top[2]; double all = 0;
        int n = thr_census(was, 3, cur, 3, 10.0, top, 2, &all);
        check(n == 2, "the census returns at most the slots asked for");
        check(top[0].s == &cur[2] && top[0].ms > 799.9 && top[0].ms < 800.1 && top[0].sys_ms > 699.9 && top[0].sys_ms < 700.1, "a thread born in the window counts all its CPU: 800 ms/s, 700 of them in the kernel");
        check(top[1].s == &cur[0] && top[1].ms > 159.9 && top[1].ms < 160.1 && top[1].sys_ms > 19.9 && top[1].sys_ms < 20.1, "a thread seen before counts its CPU since: 160 ms/s, 20 in the kernel");
        check(all > 969.9 && all < 970.1, "the total covers every thread, the ones past the top too (800 + 160 + 10 ms/s)");
        char b[512]; thr_census_line(b, sizeof b, top, n, all, 3);
        printf("  %s\n", b);
        check(strstr(b, "970 ms/s of CPU in 3 threads") != NULL, "the line gives the total and the thread count");
        check(strstr(b, "an unnamed GCD thread (tid 13, priority 47) 800 ms/s, kernel 700") != NULL, "an unnamed thread is named by its tid, whether GCD runs it, and its priority");
        check(strstr(b, "'NVN queue replay' 160 ms/s, kernel 20") != NULL, "a named thread is named");
    }
    printf("%d/%d checks passed\n", checks - fail, checks);
    return fail != 0;
}
