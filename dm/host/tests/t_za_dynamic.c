// Real mapped-data and clock-provider integration test. No GPU or game files.
// Each case runs in a fresh process because production initialization is once.
#include "../hle_za_timing.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

static module_t test_modules[1];
module_t *g_mod = test_modules; int g_nmod;
static uint64_t g_test_guest = 1000000000, g_test_wall = 1000000000;
static int g_detclock;
static void (*g_readonly)(uint64_t *, int);
static void (*g_mutable)(uint64_t *, int);
static void (*g_skill_visibility_observer)(uint64_t *, int);
static void (*g_skill_destroy_observer)(uint64_t *, int);
uint64_t now_ns(void) { return g_test_wall; }
uint64_t guest_ns(void) { return g_test_guest; }
int dm_detclock_enabled(void) { return g_detclock; }
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

static uint64_t g_regs[31], g_original[31], *g_table;
static int64_t g_record[2], g_elapsed[2] = {0, 6000000};
static uint8_t g_frame[0x48], g_config[0x22];
static uint64_t g_consumed, g_pending;
static void table_ordered(void) {
    for (int i = 1; i < 5; i++) assert(g_table[i - 1] <= g_table[i]);
    assert(!memcmp(g_table + 2, k_za_duration + 2, 3 * sizeof *g_table));
}
static void interval(int paused) {
    memcpy(g_regs, g_original, sizeof g_regs); g_mutable(g_regs, 0);
    for (int i = 0; i < 31; i++) if (i != 20 && i != 22) assert(g_regs[i] == g_original[i]);
    assert(g_config[0x20] == 1 && !g_config[0x21]);
    int code = (int32_t)g_regs[20] > (int32_t)g_regs[22] ? (int)g_regs[20] : (int)g_regs[22];
    assert(code >= 0 && code < 5); if (g_za_table) table_ordered();
    // Original provider calls SetDeltaNs(0) on disabled channels. The optional
    // controller changes the table lookup, not this channel pause decision.
    g_pending = paused ? 0 : __atomic_load_n(&g_table[code], __ATOMIC_ACQUIRE);
}
static void consume_then_interval(uint64_t dt, uint64_t wall_extra, int paused) {
    g_test_guest += dt; g_test_wall += dt + wall_extra;
    g_record[1] = (int64_t)g_pending; g_consumed += g_pending;
    g_readonly(g_original, 0); interval(paused);
}
static void setup(void) {
    g_nmod = 1; module_t *m = &g_mod[0]; strcpy(m->name, "main");
    strcpy(m->build_id, "b1f12fd919eae86ab8a978317677e64b");
    m->size = 0x4000000; m->text_end = 0x3163f70; m->ro_off = 0x3164000; m->data_off = 0x3bc2000;
    m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(m->base != MAP_FAILED); g_table = (uint64_t *)(m->base + ZA_DURATION_OFFSET);
    memcpy(g_table, k_za_duration, sizeof k_za_duration);
    uint64_t config = (uint64_t)(uintptr_t)g_config; memcpy(g_frame + 0x40, &config, 8); g_config[0x20] = 1;
    for (int i = 0; i < 31; i++) g_original[i] = 0x1000 + i;
    g_original[0] = (uint64_t)(uintptr_t)g_record;
    g_original[19] = (uint64_t)(uintptr_t)g_frame;
    g_original[21] = (uint64_t)(uintptr_t)g_elapsed;
    g_original[20] = g_original[22] = 1;
    setenv("DM_ZA_TIMING", "1", 1); setenv("DM_ZA_FPS_MODE", "dynamic60", 1);
}
int main(int argc, char **argv) {
    assert(argc == 2); const char *test = argv[1]; setup();
    assert(g_skill_visibility_observer && g_skill_destroy_observer);
    int rejection = 1;
    if (!strcmp(test, "wrong-layout")) g_mod[0].ro_off++;
    else if (!strcmp(test, "wrong-build")) g_mod[0].build_id[0] = '0';
    else if (!strcmp(test, "wrong-table")) g_table[4]++;
    else if (!strcmp(test, "unaligned")) g_mod[0].base++;
    else if (!strcmp(test, "detclock")) g_detclock = 1;
    else if (!strcmp(test, "readonly") || !strcmp(test, "executable")) {
        uintptr_t page = (uintptr_t)g_table & ~((uintptr_t)getpagesize() - 1);
        int prot = PROT_READ | (!strcmp(test, "executable") ? PROT_EXEC : 0);
        assert(!mprotect((void *)page, (size_t)getpagesize(), prot));
    } else { assert(!strcmp(test, "clock") || !strcmp(test, "clock120") || !strcmp(test, "conflict") || !strcmp(test, "guard")); rejection = 0; }
    uint64_t before[5]; memcpy(before, g_table, sizeof before);
    interval(0);
    if (rejection) {
        assert(atomic_load(&g_za_mode) == ZA_GAME && !g_za_table);
        assert(!memcmp(before, g_table, sizeof before));
        assert(g_regs[20] == 1 && g_regs[22] == 1);
    } else if (!strcmp(test, "conflict")) {
        consume_then_interval(27000000, 0, 0);
        g_table[1] = 31000001; // a foreign writer must retain ownership
        consume_then_interval(27000000, 0, 0);
        assert(atomic_load(&g_za_mode) == ZA_GAME && !g_za_table);
        assert(g_table[0] == k_za_duration[0] && g_table[1] == 31000001);
        assert(g_regs[20] == 1 && g_regs[22] == 1);
    } else if (!strcmp(test, "guard")) {
        g_config[0x21] = 1; g_elapsed[1] = 1000000001;
        memcpy(g_regs, g_original, sizeof g_regs); g_mutable(g_regs, 0);
        assert(atomic_load(&g_za_mode) == ZA_GAME && !g_za_table);
        assert(!memcmp(g_table, k_za_duration, sizeof k_za_duration));
        assert(g_regs[20] == 1 && g_regs[22] == 1);
    } else {
        assert(g_za_table && g_regs[20] == 0 && g_regs[22] == 0);
        // Consume the startup seed, then compare real engine time with every
        // whole-frame input, with the original one-frame provider latency.
        const uint64_t normal[] = {24000000,30000000,26500000,28500000};
        const uint64_t high_refresh[] = {8333333,8333334,7500000,9166667};
        const uint64_t *pattern = !strcmp(test, "clock120") ? high_refresh : normal;
        uint64_t active = 0;
        for (int i = 0; i < 3600; i++) {
            uint64_t dt = pattern[i % 4]; active += dt;
            if (!(i % 23)) {
                uint64_t samples = g_za_frame.samples, pending = g_pending;
                int64_t debt = g_za_frame.debt;
                dm_za_timing_env_changed("DM_ZA_FPS_MODE", "dynamic60");
                assert(g_za_frame.samples == samples && g_za_frame.debt == debt && g_pending == pending);
            }
            consume_then_interval(dt, 0, 0);
            assert(g_regs[20] == 0); // graphics code 0 keeps native interval 1
            // Add the pending step to close the genuine provider's one-frame
            // latency. Diagnostic warmup resets every 23 frames, yet this
            // consumed total still conserves the game clock across all frames.
            int64_t error = (int64_t)active - (int64_t)(g_consumed + g_pending - ZA_FRAME_SEED_NS);
            assert(llabs(error) <= ZA_FRAME_PHASE_NS);
            // The clock has genuinely consumed only the preceding step. Check
            // that unadjusted total too: its error is at most one pending frame
            // plus the same phase bound, across every diagnostic epoch.
            int64_t actual_error = (int64_t)active - (int64_t)(g_consumed - ZA_FRAME_SEED_NS);
            assert(llabs(actual_error) <= (int64_t)ZA_FRAME_MAX_NS + ZA_FRAME_PHASE_NS);
        }
        assert(g_za_frame.samples == 3600 && !g_za_frame.limited_ns);
        double ratio = (double)(g_consumed + g_pending - ZA_FRAME_SEED_NS) / active;
        double tolerance = (double)ZA_FRAME_PHASE_NS / active + 1e-12;
        assert(ratio >= 1 - tolerance && ratio <= 1 + tolerance);
        double actual_ratio = (double)(g_consumed - ZA_FRAME_SEED_NS) / active;
        assert(actual_ratio > .9994 && actual_ratio < 1.0006);
        printf("actual consumed clock across epochs: ratio=%.9f; one-frame-latency-adjusted ratio=%.9f\n", actual_ratio, ratio);
        // Disable the channel before backgrounding; disabled calls stay zero.
        interval(1); uint64_t consumed = g_consumed;
        consume_then_interval(0, 300000000000ull, 1);
        assert(g_consumed == consumed && g_pending == 0 && (g_za_step.flags & ZA_FRAME_FROZEN));
        consume_then_interval(27000000, 0, 0);
        assert(!g_record[1] && g_pending <= 31000000);
        consume_then_interval(27000000, 60000000000ull, 0);
        assert(g_pending <= 31000000 && (g_za_step.flags & ZA_FRAME_RESUMED));
        consume_then_interval(1000000000, 0, 0);
        assert(g_za_step.flags & ZA_FRAME_LIMITED); assert(g_pending <= 50000000);
        // Returning to every existing mode restores original values first.
        dm_za_timing_env_changed("DM_ZA_FPS_MODE", "static60");
        assert(!g_za_table && !memcmp(g_table, k_za_duration, sizeof k_za_duration));
        interval(0); assert(g_regs[20] == 0 && g_regs[22] == 0);
        dm_za_timing_env_changed("DM_ZA_FPS_MODE", "dynamic60"); interval(0);
        assert(g_za_table && g_regs[20] == 0);
        dm_za_timing_env_changed("DM_ZA_FPS_MODE", "game");
        assert(!g_za_table && !memcmp(g_table, k_za_duration, sizeof k_za_duration));
        interval(0); assert(g_regs[20] == 1 && g_regs[22] == 1);
    }
    printf("Z-A dynamic %s: PASS\n", test); return 0;
}
