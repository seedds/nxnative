// Exercise production build gates and frame-limit writes against sparse synthetic module memory; Unreal's rows come from the console variables the rewriter names in main's fixups,
// a target frame rate the game steps by from the field it names there (FPS_TARGET_PTR, FPS_TARGET_FIELD), Unreal's fixed frame rate on GEngine (UE_FIXED_RATE_PTR / _FIELD), and
// Unity's targetFrameRate and vSyncCount from its internal-call table (UNITY_*).
#include "../hle.h"
#undef DM_GAME_FPS_MAX
static int test_fps_max = 120;
#define DM_GAME_FPS_MAX test_fps_max
static int test_high = 120;   // what the frame step's hold allows (hle_nvn.c)
int nvn_fps_high_now(void) { return test_high; }
#include "../hle_fpslock.c"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static module_t test_modules[1];
module_t *g_mod = test_modules;
int g_nmod;
static int writes;
void dm_guest_write_prepare(void *p, size_t n) { (void)p; assert(n == 4 || n == 8); writes++; }

// adrp / add of a module offset into register r, at word i of the text (module offsets: the text at 0)
static void cvar(const char *name, uint64_t off) { strcpy(g_mod[0].ue_cvar[g_mod[0].n_ue_cvar].name, name); g_mod[0].ue_cvar[g_mod[0].n_ue_cvar++].off = off; }

static void adrp_add(uint32_t *t, uint32_t i, int r, uint64_t target) {
    int64_t imm = (int64_t)((target & ~0xfffull) - ((i * 4ull) & ~0xfffull)) >> 12;
    t[i] = 0x90000000u | (uint32_t)(imm & 3) << 29 | (uint32_t)((imm >> 2) & 0x7ffff) << 5 | (uint32_t)r;
    t[i + 1] = 0x91000000u | (uint32_t)(target & 0xfff) << 10 | (uint32_t)r << 5 | (uint32_t)r;
}
// A Frostbite main in miniature: the class name and field names in read-only data, the class record and its field list, the static constructor's registration of the
// type info and a reader's settings lookup in the text, the instance cache word and the instance
static uint8_t *frostbite(uint8_t *base, uint8_t *inst) {
    g_mod[0].text_end = 0x1000; g_mod[0].ro_off = 0x2000; g_mod[0].size = 0x13000000;
    uint64_t b = (uint64_t)(uintptr_t)base, rec = 0x3008, fields = 0x3100, ti = 0x40000, cache = 0x41000;
    static const char names[] = "\0SimulationTimeSettingsX\0SimulationTimeSettings\0ForceSimRate\0MaxSimFps";
    memcpy(base + 0x2000, names, sizeof names);
    *(uint64_t *)(base + 0x3000) = b + 0x2001;   // a record of another class whose name starts the same
    *(uint64_t *)(base + rec) = b + 0x2019; *(uint64_t *)(base + rec + 0x40) = b + fields;
    *(uint64_t *)(base + fields) = b + 0x203d; *(uint32_t *)(base + fields + 0x10) = 0x34;
    *(uint64_t *)(base + fields + 0x20) = b + 0x2030; *(uint32_t *)(base + fields + 0x30) = 0x38;
    uint32_t *t = (uint32_t *)base;
    adrp_add(t, 100, 19, ti); adrp_add(t, 102, 8, rec); t[104] = 0xF9000000u | 19 << 5 | 8;   // str x8, [x19]
    adrp_add(t, 200, 2, 0x42000);   // another word in x2 well before the lookup
    adrp_add(t, 220, 1, ti); adrp_add(t, 222, 2, cache); t[224] = 0x94000000u;   // bl
    *(uint64_t *)(inst + 8) = b + ti; *(uint64_t *)(base + cache) = (uint64_t)(uintptr_t)inst;
    return inst;
}

static void check(int which) {
    size_t len = 0x13000000;
    uint8_t *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(base != MAP_FAILED);
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base;
    strcpy(g_mod[0].build_id, "94ceae325c205c4b9d6f7235552f28fd");
    setenv("DM_FPS_TARGET", which == 27 ? "0" : which == 26 ? "120" : which >= 25 ? "60" : which == 22 || which == 24 ? "60" : which >= 19 ? "120" : which == 1 || which == 10 || which == 18 ? "30" : which == 2 || which == 11 ? "0" : "60", 1);
    if (which == 20) test_fps_max = 60;
    int32_t *unity = (int32_t *)(base + 0x4afc868);
    *unity = which == 1 ? 60 : which == 4 ? -1 : which == 5 ? 0 : 30;
    uint8_t object[0x500] = {0};
    float *f = NULL; int32_t *sync = NULL;
    if (which <= 5 && which != 3) g_mod[0].unity_target_fps = 0x4afc868;   // 0 to 5: Unity's targetFrameRate where the rewriter found it (Pokémon Brilliant Diamond 1.3.0); 3: nothing found
    if (which == 6) {
        strcpy(g_mod[0].build_id, "c9a3dd7702075ecd90101f4555389582");
        *(uintptr_t *)(base + 0x1295b3f0) = (uintptr_t)object;
        f = (float *)(object + 0x434); *f = 30;
    } else if (which == 7) {
        strcpy(g_mod[0].build_id, "79e5950ffa85acf63e28c9aec051eac2");
        f = (float *)(base + 0x13c94dc); *f = 1000.0f / 30;
    } else if (which == 8) {   // Octopath Traveler 0: the frame limit and the present interval, and a variable the host does not write
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        cvar("r.Streaming.PoolSize", 0x7c71608); cvar(DM_GAME_UE_FPS_CVAR, 0x7c738d0); cvar("rhi.SyncInterval", 0x7b32690);
        *(uintptr_t *)(base + 0x7c738d0 + 0x10) = (uintptr_t)object;
        *(uintptr_t *)(base + 0x7b32690 + 0x10) = (uintptr_t)(object + 16);
        f = (float *)object; f[0] = f[1] = 30;
        sync = (int32_t *)(object + 16); sync[0] = sync[1] = 2;
    } else if (which == 14) {
        strcpy(g_mod[0].build_id, "a48c0de3bc353efa0e4d359bde05ef24");
        *(uintptr_t *)(base + 0xda20e38) = (uintptr_t)object;
        double *d = (double *)(object + 8); d[0] = d[1] = 30;
    } else if (which == 15 || which == 16) {
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        frostbite(base, object);
        *(float *)(object + 0x34) = 30; *(int32_t *)(object + 0x38) = 30;
        if (which == 16) *(uint64_t *)(object + 8) = 0;   // the cache word holds something else
    } else if (which == 21) {   // Unreal at 120: the frame limit follows, and the present interval stays one vsync (60 / 120 is no interval)
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        cvar(DM_GAME_UE_FPS_CVAR, 0x7c738d0); cvar("rhi.SyncInterval", 0x7b32690);
        *(uintptr_t *)(base + 0x7c738d0 + 0x10) = (uintptr_t)object;
        *(uintptr_t *)(base + 0x7b32690 + 0x10) = (uintptr_t)(object + 16);
        f = (float *)object; f[0] = f[1] = 30;
        sync = (int32_t *)(object + 16); sync[0] = sync[1] = 2;
    } else if (which == 22 || which == 23 || which == 24) {   // DRAGON BALL: Sparking! ZERO: GEngine->FixedFrameRate (+0x9c8), which the Switch frame pace sets to 30, and the present interval;
        // bUseFixedFrameRate (bit 0x40 of +0x9c4) set, except in 24 (an engine stepping by the time that passed, Princess Peach: Showtime!)
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        cvar(DM_GAME_UE_FPS_CVAR, 0x7c738d0); cvar("rhi.SyncInterval", 0x7b32690);   // t.MaxFPS registered but never set: 0 stays
        *(uintptr_t *)(base + 0x7b32690 + 0x10) = (uintptr_t)(object + 16);
        sync = (int32_t *)(object + 16); sync[0] = sync[1] = 2;
        g_mod[0].ue_fixed_ptr = 0x9aefa30; g_mod[0].ue_fixed_field = 0x9c8;
        *(uintptr_t *)(base + 0x9aefa30) = (uintptr_t)(object + 0x100) - 0x9c8;   // the engine object, placed so its field at +0x9c8 lands inside object
        f = (float *)(object + 0x100); *f = 30;
        g_mod[0].ue_fixed_flag = 0x9c4 << 8 | 0x40;
        if (which != 24) object[0x100 - 4] = 0x47;   // the flag byte at +0x9c4, bit 0x40 set among others (Pikmin 4: 0x47)
        else object[0x100 - 4] = 0x07;
    } else if (which >= 25 && which <= 29) {   // Unity, at the places the rewriter found through the internal-call table (The Rogue Prince of Persia 1.1.0): vSyncCount in quality
        // level 1 of 0x100-byte levels, set to 2 (0 in 29, where the game paces by targetFrameRate), and targetFrameRate (-1; 60 in 26, 30 in 29); 28: Pokémon Brilliant Diamond's
        // targetFrameRate field (found from code registration, Unity 2019.4), set to 30
        if (which != 28) strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        g_mod[0].unity_target_fps = which == 28 ? 0x4afc868 : 0x8d33de8;
        g_mod[0].unity_qs_ptr = 0x90957d8; g_mod[0].unity_qs_idx = 0x64; g_mod[0].unity_qs_arr = 0x20; g_mod[0].unity_qs_size = 0x100; g_mod[0].unity_qs_vsync = 0x88;
        *(uintptr_t *)(base + 0x90957d8) = (uintptr_t)object;
        *(int32_t *)(object + 0x64) = 1; *(uintptr_t *)(object + 0x20) = (uintptr_t)(object + 0x100);   // two levels from object + 0x100: level 1's count at +0x288
        sync = (int32_t *)(object + 0x100 + 0x100 + 0x88); *sync = which == 29 ? 0 : 2; sync[-0x40] = 3;   // and level 0's, which the host must not touch
        if (which != 28) { int32_t *t = (int32_t *)(base + 0x8d33de8); *t = which == 26 ? 60 : which == 29 ? 30 : -1; }
    } else if (which == 17 || which == 18 || which == 19 || which == 20) {   // Fire Emblem Fortune's Weave: the target the rewriter found, 30 in gameplay (17) or 60 in a menu (18, at DM_FPS_TARGET=30)
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        g_mod[0].fps_target_ptr = 0x21e60d8; g_mod[0].fps_target_field = 0x1dc;
        *(uintptr_t *)(base + 0x21e60d8) = (uintptr_t)object;
        *(int32_t *)(object + 0x1dc) = which == 17 ? 30 : 60;
    } else if (which >= 9) {   // the present interval alone (UE 4.18's nvn.SyncInterval); 12: a main the rewriter found no variable in
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        if (which != 12) cvar("nvn.SyncInterval", 0x8adca10);
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = which == 13 ? 0 : (uintptr_t)object;
        sync = (int32_t *)object; sync[0] = sync[1] = which == 10 ? 1 : 2;
    }
    int engine = which != 3 && which != 12;   // a limiter the host writes: any but a main the rewriter found no Unity field in (3) and one it found no Unreal variable in (12)
    if (which & 1) assert(dm_fps_lock_engine() == engine);   // the graphics layer's present cap may ask before the first present (the acquire)
    dm_fps_lock_tick();
    assert(dm_fps_lock_engine() == engine);
    if (which == 19) {   // 120: the target follows the hold, so the game's step stays what the screen shows; the game setting its own 30 again changes nothing
        int32_t *t = (int32_t *)(object + 0x1dc);
        assert(*t == 120);
        test_high = 60; dm_fps_lock_tick(); assert(*t == 60);
        test_high = 120; dm_fps_lock_tick(); assert(*t == 120);
        *t = 30; dm_fps_lock_tick(); assert(*t == 120);
        munmap(base, len); return;
    }
    if (which == 20) { assert(*(int32_t *)(object + 0x1dc) == 60); munmap(base, len); return; }   // a game not checked at 120 runs at 60
    if (which == 22) {   // 60: the fixed rate and the interval; the frame pace setting 30 again (a mode change) is raised again
        assert(*f == 60 && sync[0] == 1 && sync[1] == 1 && writes == 3);
        *f = 30; dm_fps_lock_tick(); assert(*f == 60 && writes == 4);
        munmap(base, len); return;
    }
    if (which == 24) { assert(*f == 30 && sync[0] == 1 && sync[1] == 1 && writes == 2); munmap(base, len); return; }   // flag off: the rate is left, the interval written
    if (which == 23) {   // 120: the fixed rate follows the hold, the interval stays one vsync
        assert(*f == 120 && sync[0] == 1 && sync[1] == 1);
        test_high = 60; dm_fps_lock_tick(); assert(*f == 60);
        test_high = 120; dm_fps_lock_tick(); assert(*f == 120);
        munmap(base, len); return;
    }
    if (which >= 25 && which <= 29) {
        int32_t *t = (int32_t *)(base + 0x8d33de8);
        assert(sync[-0x40] == 3);
        if (which == 25) {   // 60: one vsync; targetFrameRate unset (-1) stays; the game setting 2 again (a scene change) is undone
            assert(*sync == 1 && *t == -1 && writes == 1);
            dm_fps_lock_tick(); assert(writes == 1);
            *sync = 2; dm_fps_lock_tick(); assert(*sync == 1 && writes == 2);
        } else if (which == 26) {   // 120: still one vsync; the target follows the hold
            assert(*sync == 1 && *t == 120);
            test_high = 60; dm_fps_lock_tick(); assert(*sync == 1 && *t == 60);
        } else if (which == 27) assert(*sync == 2 && *t == -1 && writes == 0);   // no target: left as the game sets them, though the engine limit is there
        else if (which == 28) assert(*sync == 1 && *unity == 60 && writes == 2);   // the count and the target, each written once
        else assert(*sync == 0 && *t == 60 && writes == 1);   // 29: a count of 0 is the game's choice to pace by targetFrameRate, which is raised
        munmap(base, len); return;
    }
    if (which == 21) {
        assert(f[0] == 120 && f[1] == 120 && sync[0] == 1 && sync[1] == 1);
        test_high = 60; dm_fps_lock_tick(); assert(f[0] == 60 && f[1] == 60 && sync[0] == 1 && sync[1] == 1);
        munmap(base, len); return;
    }
    if (which == 0) {
        assert(*unity == 60 && writes == 1);
        dm_fps_lock_tick(); assert(writes == 1);
        *unity = 30; dm_fps_lock_tick(); assert(*unity == 60 && writes == 2);
    } else if (which == 1) assert(*unity == 30 && writes == 1);
    else if (which >= 2 && which <= 5) {
        assert(writes == 0);
        assert(*unity == (which == 4 ? -1 : which == 5 ? 0 : 30));
    } else if (which == 6) assert(*f == 60 && writes == 1);
    else if (which == 7) assert(fabsf(*f - (1000.0f / 60 - 0.5f)) < 0.001f && writes == 1);
    else if (which == 8) assert(f[0] == 60 && f[1] == 60 && sync[0] == 1 && sync[1] == 1 && writes == 4);
    else if (which == 9) {
        assert(sync[0] == 1 && sync[1] == 1 && writes == 2);
        dm_fps_lock_tick(); assert(writes == 2);
        sync[0] = sync[1] = 2; dm_fps_lock_tick();
        assert(sync[0] == 1 && sync[1] == 1 && writes == 4);
    } else if (which == 10) assert(sync[0] == 2 && sync[1] == 2 && writes == 2);
    else if (which == 14) { double *d = (double *)(object + 8); assert(d[0] == 60 && d[1] == 60 && writes == 2); }
    else if (which == 15) {
        assert(*(float *)(object + 0x34) == 60 && *(int32_t *)(object + 0x38) == 60 && writes == 2);
        dm_fps_lock_tick(); assert(writes == 2);
        *(int32_t *)(object + 0x38) = 30; dm_fps_lock_tick(); assert(*(int32_t *)(object + 0x38) == 60 && writes == 3);
    } else if (which == 17) {
        int32_t *t = (int32_t *)(object + 0x1dc);
        assert(*t == 60 && writes == 1);
        dm_fps_lock_tick(); assert(writes == 1);
        *t = 30; dm_fps_lock_tick(); assert(*t == 60 && writes == 2);   // the game sets 30 again where gameplay starts
    } else if (which == 18) assert(*(int32_t *)(object + 0x1dc) == 30 && writes == 1);
    else if (which == 16) assert(*(float *)(object + 0x34) == 30 && *(int32_t *)(object + 0x38) == 30 && writes == 0);
    else if (which == 13) {
        assert(sync[0] == 2 && sync[1] == 2 && writes == 0);
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = (uintptr_t)object;
        dm_fps_lock_tick();
        assert(sync[0] == 1 && sync[1] == 1 && writes == 2);
    }
    else assert(sync[0] == 2 && sync[1] == 2 && writes == 0);
    munmap(base, len);
}

int main(void) {
    // Each process starts the production one-time initialization with a fresh build and target.
    for (int i = 0; i < 30; i++) {
        pid_t p = fork(); assert(p >= 0);
        if (!p) { check(i); _exit(0); }
        int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Frame limits: integer/float FPS, milliseconds, vsync copies, build gates, found fields and overrides passed");
}
