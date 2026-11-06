// The GPU memory guard's system side (guard_sys_tick): the level follows the memory the system can still give against the margin (DM_NVN_SYS_MARGIN_MB), and guard_level takes the higher of it
// and the working-set side. The test ends in well under the timer's GUARD_TICKS (1 s) over the stop level, so the front end's stop is never reached
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, s) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", s); } } while (0)
int main(void) { @autoreleasepool {
    setenv("DM_NVN_SYS_MARGIN_MB", "1048576", 1);   // a 1 TB margin: the system never has that much left
    mtl_init();
    CHECK(atomic_load(&g_sys_margin) == (1048576ull << 20), "the margin comes from DM_NVN_SYS_MARGIN_MB");
    guard_sys_tick();
    CHECK(atomic_load(&g_sys_avail) > 0, "the system's available memory is read");
    CHECK(atomic_load(&g_sys_lv) == GUARD_STOP, "available memory under the margin: stop");
    CHECK(!g_guard.stop || guard_level(0) == GUARD_STOP, "guard_level takes the system side even with nothing resident");
    atomic_store(&g_sys_margin, 1ull << 20); guard_sys_tick();   // 1 MB: always more left than that
    CHECK(atomic_load(&g_sys_lv) == GUARD_OK, "available memory over twice the margin: no level");
    CHECK(!g_guard.stop || guard_level(0) == GUARD_OK, "and guard_level is back to the working-set side");
    uint64_t avail = atomic_load(&g_sys_avail);
    atomic_store(&g_sys_margin, avail / 2 + (64ull << 20)); guard_sys_tick();   // between the margin and twice it, unless the system moved by more than 64 MB in between
    CHECK(atomic_load(&g_sys_lv) == GUARD_EVICT, "available memory between the margin and twice it: evict");
    atomic_store(&g_sys_margin, 0); atomic_store(&g_sys_lv, GUARD_OK); guard_sys_tick();
    CHECK(atomic_load(&g_sys_lv) == GUARD_OK, "a margin of 0 turns the system side off");
    printf("t_guard_sys: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
