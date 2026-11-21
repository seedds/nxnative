// Exercise the production frame-cap getter with a controlled clock and thermal
// source. No game files or GPU; concurrent readers model acquire and present.
#include "../../host/hle.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nvn_game_fps(void);
static _Alignas(8) uint8_t fps_state[0x40];
uint8_t *dm_fps_state = fps_state;
static _Atomic uint64_t test_now;
static _Atomic int test_thermal = -1, thermal_reads;
uint64_t now_ns(void) { return atomic_load(&test_now); }
int dm_thermal_now(void) { atomic_fetch_add(&thermal_reads, 1); return atomic_load(&test_thermal); }
static _Atomic int test_low_power, low_power_reads;
int dm_low_power_now(void) { atomic_fetch_add(&low_power_reads, 1); return atomic_load(&test_low_power); }   // front_ios.m: NSProcessInfo lowPowerModeEnabled
static void low_power(int on) { atomic_store(&test_low_power, on); }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}

static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static void at(int seconds, int thermal) {
    atomic_store(&test_thermal, thermal);
    atomic_store(&test_now, (uint64_t)seconds * 1000000000ull);
}
static void target(float fps) {
    uint32_t bits; memcpy(&bits, &fps, sizeof bits);
    __atomic_store_n((uint32_t *)fps_state, bits, __ATOMIC_RELAXED);
}
static _Atomic int test_movie;
int dm_movie_hz(void) { return atomic_load(&test_movie); }   // hle_movie.c: the picture rate of the movie playing, 0 when none
static void movie(int hz) { atomic_store(&test_movie, hz); }

static _Atomic int ready, start;
static void *reader(void *unused) {
    (void)unused; atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) sched_yield();
    for (int i = 0; i < 10000; i++) {
        atomic_fetch_add(&test_now, 1000000ull);
        (void)nvn_game_fps();
    }
    return NULL;
}

int main(void) {
    target(60);
    const char *enabled = getenv("DM_THERMAL_CAP");
    const char *lp_env = getenv("DM_LOW_POWER_CAP"); int lp_enabled = !(lp_env && !strcmp(lp_env, "0"));
    if (enabled && !strcmp(enabled, "0")) {
        at(10, 3); CHECK("disabled: a critical device keeps the configured cap", nvn_game_fps() == 60);
        low_power(1); at(11, 0); CHECK("disabled: Low Power Mode keeps the configured cap", nvn_game_fps() == 60);
        movie(30); CHECK("disabled: the movie cap still applies", nvn_game_fps() == 30);
        CHECK("disabled: thermal state is never read", atomic_load(&thermal_reads) == 0);
        CHECK("disabled: Low Power Mode is never read", atomic_load(&low_power_reads) == 0);
    } else {
        dm_fps_state = NULL; at(1, 2);
        CHECK("no frame step: thermal state is not read", nvn_game_fps() == 60 && !atomic_load(&thermal_reads));
        dm_fps_state = fps_state; target(0);
        CHECK("inactive frame step: thermal state is not read", nvn_game_fps() == 60 && !atomic_load(&thermal_reads));
        target(60); at(10, -1);
        CHECK("Mac with no thermal state keeps 60 fps", nvn_game_fps() == 60);
        at(11, 1); CHECK("fair keeps 60 fps", nvn_game_fps() == 60);
        at(12, 2); CHECK("serious holds 30 fps", nvn_game_fps() == 30);
        int reads = atomic_load(&thermal_reads);
        at(12, 0); CHECK("a second call uses the cached thermal sample", nvn_game_fps() == 30 && atomic_load(&thermal_reads) == reads);
        at(13, 1); CHECK("fair starts recovery while keeping 30 fps", nvn_game_fps() == 30);
        movie(30); at(14, 2); CHECK("a serious movie keeps 30 fps", nvn_game_fps() == 30);
        CHECK("a movie still samples thermal state", atomic_load(&thermal_reads) == reads + 2);
        at(44, 2); (void)nvn_game_fps();
        movie(0); at(45, 1);
        CHECK("heat during a movie resets the earlier cooling interval", nvn_game_fps() == 30);
        at(74, 1); CHECK("29 calm seconds do not release the cap", nvn_game_fps() == 30);
        at(75, 1); CHECK("30 calm seconds restore 60 fps", nvn_game_fps() == 60);

        at(76, 3); CHECK("critical also holds 30 fps", nvn_game_fps() == 30);
        movie(30); at(77, 1); (void)nvn_game_fps();
        at(107, 1); CHECK("a calm movie keeps its own 30 fps cap", nvn_game_fps() == 30);
        movie(0); CHECK("cooling during a movie needs no additional recovery hold", nvn_game_fps() == 60);

        at(108, 2); (void)nvn_game_fps();
        target(30); at(109, 0); (void)nvn_game_fps();
        at(139, 0); CHECK("a configured 30 fps target keeps its cap", nvn_game_fps() == 30);
        target(60); CHECK("cooling at a 30 fps target is tracked before raising it", nvn_game_fps() == 60);

        at(140, 2); (void)nvn_game_fps();
        at(141, 1); (void)nvn_game_fps();
        at(170, 2); CHECK("renewed heat interrupts recovery", nvn_game_fps() == 30);
        at(171, 0); (void)nvn_game_fps();
        at(200, 0); CHECK("the restarted recovery lasts all 30 seconds", nvn_game_fps() == 30);
        at(201, 0); CHECK("recovery also completes at nominal", nvn_game_fps() == 60);

        target(30); movie(60); at(201, 0); CHECK("a 60-picture movie lifts a 30 fps target to 60", nvn_game_fps() == 60);
        movie(24); CHECK("a 24-picture movie holds 30", nvn_game_fps() == 30);
        movie(0); CHECK("the 30 fps target again once the movie ends", nvn_game_fps() == 30);
        target(60); movie(60); CHECK("a 60-picture movie at a 60 fps target keeps 60", nvn_game_fps() == 60);
        at(202, 2); CHECK("a serious device holds 30 during a 60-picture movie", nvn_game_fps() == 30);
        movie(0);

        at(202, 2);
        pthread_t threads[4];
        for (unsigned i = 0; i < 4; i++) if (pthread_create(&threads[i], NULL, reader, NULL)) return 2;
        while (atomic_load(&ready) != 4) sched_yield();
        atomic_store(&start, 1);
        for (unsigned i = 0; i < 4; i++) pthread_join(threads[i], NULL);
        CHECK("concurrent acquire/present readers retain the hot cap", nvn_game_fps() == 30);

        // Low Power Mode: the same cap and calm timer, only where heat applies (DM_LOW_POWER_CAP=0 keeps 60 in it)
        at(250, 0); (void)nvn_game_fps();
        at(280, 0); CHECK("the hot cap from the readers recovers", nvn_game_fps() == 60);
        dm_fps_state = NULL; low_power(1); at(300, 0); int lpr = atomic_load(&low_power_reads);
        CHECK("no frame step: Low Power Mode is not read", nvn_game_fps() == 60 && atomic_load(&low_power_reads) == lpr);
        dm_fps_state = fps_state; target(0);
        CHECK("inactive frame step: Low Power Mode is not read", nvn_game_fps() == 60 && atomic_load(&low_power_reads) == lpr);
        target(60);
        if (lp_enabled) {
            at(301, 0); CHECK("Low Power Mode holds 30 fps", nvn_game_fps() == 30);
            CHECK("Low Power Mode is read with the thermal sample", atomic_load(&low_power_reads) == lpr + 1);
            low_power(0); at(301, 0); CHECK("a second call uses the cached Low Power Mode sample", nvn_game_fps() == 30 && atomic_load(&low_power_reads) == lpr + 1);
            at(302, 0); CHECK("Low Power Mode off starts the calm interval", nvn_game_fps() == 30);
            at(331, 0); CHECK("29 s after Low Power Mode went off the cap holds", nvn_game_fps() == 30);
            at(332, 0); CHECK("30 s after Low Power Mode went off 60 fps returns", nvn_game_fps() == 60);
            low_power(1); at(333, 2); CHECK("heat and Low Power Mode hold 30 fps", nvn_game_fps() == 30);
            low_power(0); at(334, 2); CHECK("Low Power Mode off while still hot keeps the cap", nvn_game_fps() == 30);
            at(335, 1); (void)nvn_game_fps();
            low_power(1); at(350, 1); CHECK("Low Power Mode during the heat calm keeps 30 fps", nvn_game_fps() == 30);
            low_power(0); at(351, 1); (void)nvn_game_fps();
            at(380, 1); CHECK("Low Power Mode restarted the calm interval", nvn_game_fps() == 30);
            at(381, 1); CHECK("the restarted calm interval ends after 30 s", nvn_game_fps() == 60);
        } else {
            at(301, 0); CHECK("DM_LOW_POWER_CAP=0: Low Power Mode keeps 60 fps", nvn_game_fps() == 60);
            CHECK("DM_LOW_POWER_CAP=0: Low Power Mode is never read", atomic_load(&low_power_reads) == 0);
            at(302, 2); CHECK("DM_LOW_POWER_CAP=0: heat still holds 30 fps", nvn_game_fps() == 30);
        }
        low_power(0);
    }
    printf("== %d checks, %d failed ==\n", checks, failed);
    return failed != 0;
}
