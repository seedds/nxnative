// Standalone host test of opt-in timing hooks and saved-register preservation.
// clang -O1 -Wall -I dm/host -I <generated profile directory> -o /tmp/t_za_timing dm/host/tests/t_za_timing.c
// Run each case in a fresh process: off, trace, static-fixed, static-adaptive,
// native-adaptive, wrong-build, live, concurrent. No game inputs/device needed.
#include "../hle_za_timing.c"
#include <assert.h>

static module_t test_modules[1];
module_t *g_mod = test_modules; int g_nmod;
static _Atomic uint64_t g_test_ns = 1000000000ull;
static void (*g_readonly)(uint64_t *, int);
static void (*g_mutable)(uint64_t *, int);
static void (*g_skill_visibility_observer)(uint64_t *, int);
static void (*g_skill_destroy_observer)(uint64_t *, int);
uint64_t now_ns(void) { return atomic_load(&g_test_ns); }
uint64_t guest_ns(void) { return atomic_load(&g_test_ns); }
int dm_detclock_enabled(void) { return 0; }
int gate_obs_present(int id) { return id == OBS_ZA_CLOCK_DELTA || id == OBS_ZA_INTERVAL; }
void gate_obs_set(int id, void (*fn)(uint64_t *, int)) {
    switch (id) {
    case OBS_ZA_CLOCK_DELTA:
        assert(!g_readonly && fn == za_clock_delta); g_readonly = fn; break;
    case OBS_ZA_INTERVAL:
        assert(!g_mutable && fn == za_interval); g_mutable = fn; break;
    case OBS_ZA_SKILL_VISIBILITY:
        assert(!g_skill_visibility_observer && fn == za_skill_visibility); g_skill_visibility_observer = fn; break;
    case OBS_ZA_SKILL_DESTROY:
        assert(!g_skill_destroy_observer && fn == za_skill_destroy); g_skill_destroy_observer = fn; break;
    default: assert(!"unexpected read-only observation gate");
    }
}

static void clocks(uint64_t *regs, int count) {
    for (int i = 0; i < count; i++) { g_test_ns += 16666667; g_readonly(regs, 0); }
}
static uint64_t g_worker_regs[31];
static _Atomic int g_worker_started, g_switches;
static void *observe_concurrently(void *unused) {
    (void)unused; uint64_t regs[31];
    atomic_store(&g_worker_started, 1);
    for (int i = 0; i < 3000 || atomic_load(&g_switches) < 1000; i++) {
        memcpy(regs, g_worker_regs, sizeof regs); g_mutable(regs, 0);
        assert((regs[20] == 1 && regs[22] == 1) ||
               (regs[20] == 0 && (regs[22] == 0 || regs[22] == 2)));
        for (int r = 0; r < 31; r++) if (r != 20 && r != 22) assert(regs[r] == g_worker_regs[r]);
        clocks(regs, 1);
    }
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2); const char *test = argv[1];
    assert(g_skill_visibility_observer && g_skill_destroy_observer);
    unsetenv("DM_ZA_TIMING"); unsetenv("DM_ZA_FPS_MODE");
    g_nmod = 1; strcpy(g_mod[0].name, "main");
    strcpy(g_mod[0].build_id, "b1f12fd919eae86ab8a978317677e64b");
    int native_adaptive = !strcmp(test, "static-adaptive"), change = 0, want22 = 1;
    if (!strcmp(test, "trace") || !strcmp(test, "live") || !strcmp(test, "concurrent")) setenv("DM_ZA_TIMING", "1", 1);
    else if (!strcmp(test, "static-fixed") || native_adaptive) {
        setenv("DM_ZA_FPS_MODE", "static60", 1); change = 1;
        want22 = native_adaptive ? 2 : 0;
    } else if (!strcmp(test, "native-adaptive")) {
        setenv("DM_ZA_FPS_MODE", "adaptive60", 1); change = 1; want22 = 2;
    } else if (!strcmp(test, "wrong-build")) {
        setenv("DM_ZA_FPS_MODE", "static60", 1);
        strcpy(g_mod[0].build_id, "00000000000000000000000000000000");
    } else assert(!strcmp(test, "off"));

    const int64_t input[] = {-1, 0, 16666667, 16666668, 33333334, 33333335,
                              50000000, 50000001, 66666667, 66666668};
    const int output[] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4};
    for (unsigned i = 0; i < sizeof input / sizeof *input; i++) assert(za_elapsed_code(input[i]) == output[i]);

    uint8_t frame[0x48] = {0}, config[0x22] = {0};
    int64_t elapsed[2] = {0, 35000000};
    config[0x20] = 1; config[0x21] = native_adaptive;
    uint64_t pointer = (uint64_t)(uintptr_t)config; memcpy(frame + 0x40, &pointer, sizeof pointer);
    uint64_t regs[31], expected[31];
    for (int i = 0; i < 31; i++) regs[i] = 0x1000 + i;
    regs[19] = (uint64_t)(uintptr_t)frame; regs[21] = (uint64_t)(uintptr_t)elapsed;
    regs[20] = 1; regs[22] = native_adaptive ? 2 : 1;
    memcpy(expected, regs, sizeof regs);
    if (change) { expected[20] = 0; expected[22] = want22; }
    assert(g_readonly && g_mutable); g_mutable(regs, 0);
    assert(!memcmp(regs, expected, sizeof regs));
    assert(config[0x20] == 1 && config[0x21] == native_adaptive);

    if (g_za_valid) {
        int64_t record[2] = {0, 33333333};
        regs[0] = (uint64_t)(uintptr_t)record; clocks(regs, 2);
        assert(g_za.calls == 0 && g_za_warmup == 0);
        record[1] = 16666667; clocks(regs, 2);
        assert(g_za.calls == 2 && g_za.sum_ns == 16666667 && g_za.invalid == 0);
        if (!strcmp(test, "live")) {
            uint64_t before = atomic_load(&g_za_epoch);
            dm_za_timing_env_changed("UNRELATED", "static60");
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "unknown");
            assert(atomic_load(&g_za_epoch) == before && g_za.calls == 2);
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "static60");
            assert(atomic_load(&g_za_epoch) == before + 1 && g_za.calls == 0 && g_za.sum_ns == 0);
            assert(atomic_load(&g_za_mode) == ZA_STATIC60);
            clocks(regs, 1); assert(g_za.calls == 0 && g_za_warmup == 2); // no new-mode interval yet
            regs[20] = regs[22] = 1; g_mutable(regs, 0);
            assert(regs[20] == 0 && regs[22] == 0);
            clocks(regs, 4); assert(g_za.calls == 2 && g_za.sum_ns == 16666667);
            g_test_ns += 10000000000ull; // pause before explicit reset must not enter the new epoch
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "adaptive60");
            regs[20] = regs[22] = 1; g_mutable(regs, 0); assert(regs[20] == 0 && regs[22] == 2);
            clocks(regs, 4); assert(g_za.calls == 2 && g_za.sum_ns == 16666667);
            assert(now_ns() - g_za.start_wall == 16666667);
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "game");
            regs[20] = regs[22] = 1; g_mutable(regs, 0); assert(regs[20] == 1 && regs[22] == 1);
            before = atomic_load(&g_za_epoch);
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "game"); // same-mode command resets after pause/loading
            assert(atomic_load(&g_za_epoch) == before + 1 && g_za.calls == 0);
        } else if (!strcmp(test, "concurrent")) {
            memcpy(g_worker_regs, regs, sizeof regs); g_worker_regs[20] = g_worker_regs[22] = 1;
            pthread_t worker; assert(!pthread_create(&worker, NULL, observe_concurrently, NULL));
            while (!atomic_load(&g_worker_started)) sched_yield();
            const char *modes[] = {"game", "static60", "adaptive60"};
            for (int i = 0; i < 1000; i++) {
                dm_za_timing_env_changed("DM_ZA_FPS_MODE", modes[i % 3]);
                atomic_store(&g_switches, i + 1);
            }
            assert(!pthread_join(worker, NULL));
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "game");
            assert(atomic_load(&g_za_mode) == ZA_GAME && g_za.calls == 0 && g_za.sum_ns == 0);
        } else if (!atomic_load(&g_za_live_allowed)) {
            int prior = atomic_load(&g_za_mode); uint64_t before = atomic_load(&g_za_epoch);
            dm_za_timing_env_changed("DM_ZA_FPS_MODE", "game");
            assert(atomic_load(&g_za_mode) == prior && atomic_load(&g_za_epoch) == before);
        }
    } else assert(!strcmp(test, "off") || !strcmp(test, "wrong-build"));
    if (!g_za_valid) {
        int prior = atomic_load(&g_za_mode); uint64_t before = atomic_load(&g_za_epoch);
        dm_za_timing_env_changed("DM_ZA_FPS_MODE", "static60");
        assert(atomic_load(&g_za_mode) == prior && atomic_load(&g_za_epoch) == before);
    }
    printf("Z-A timing %s: PASS\n", test);
    return 0;
}
