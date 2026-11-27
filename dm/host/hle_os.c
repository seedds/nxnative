// Host common miscellany: guest object side table, monotonic clock, C library imports that none of the modules define.
// (The former 136 HLE entries for nn::os/mem/lmem/C++ ABI have been deleted: the SDK brings its own nn::os, and at load time not one of these entries was resolved and used.)
#include <pthread.h>
#include "hle.h"
#include <mach/mach_time.h>
#include <stdatomic.h>

// ---------- Side table ----------
// Guest object -> host state, linear probing with backward-shift deletion: no deleted markers, so every key sits once in the run of
// occupied slots from its home slot, and an empty slot ends every search (t_objmap). Keys are guest pointers, never NULL.
#define OM_SIZE (1u << 18)
#define OM_MASK (OM_SIZE - 1)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & OM_MASK; }
// The key's slot, or the empty slot that ends its run. Called with g_om_lock held
static unsigned om_find(const void *k) {
    unsigned i = om_h(k);
    while (g_om[i].k && g_om[i].k != k) i = (i + 1) & OM_MASK;
    return i;
}
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = g_om[om_find(k)].v;   // an empty slot holds NULL
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    unsigned i = om_find(k);
    g_om[i].k = k; g_om[i].v = v;
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    unsigned i = om_find(k);
    void *r = g_om[i].v;
    if (g_om[i].k) {
        // Close the hole: each later entry of the run whose home slot is not after the hole (cyclically) moves back into it
        for (unsigned j = (i + 1) & OM_MASK; g_om[j].k; j = (j + 1) & OM_MASK)
            if (((j - om_h(g_om[j].k)) & OM_MASK) >= ((j - i) & OM_MASK)) { g_om[i] = g_om[j]; i = j; }
        g_om[i].k = NULL; g_om[i].v = NULL;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
// Guest clock: the system counters the guest can read (mrs cntpct gate, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver DM_TICK_HOLD): while frozen the guest clock does not advance ⇒ the game's next tick never comes due; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the first present on the guest clock becomes "advance a fixed step per present", not following the wall clock --
// the client's per-frame dt and the server tick catch-up snapshots are both determined by the present count, so frame N in a run is byte-for-byte comparable between two passes of the same replay (the foundation of byte-for-byte picture proof).
// Before the first frame the wall clock is still used (the boot period has waits that poll by time, freezing would hang them); at the switch it continues from the guest clock value at that time, no jump. It does not advance while frozen.
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int guest_clock_pinned(void) { return atomic_load(&g_gdet_on) || atomic_load(&g_gfz); }
uint64_t guest_ns_at_host_time(uint64_t host_ns) {
    if (atomic_load(&g_gdet_on)) return atomic_load(&g_gdet);
    if (atomic_load(&g_gfz)) return atomic_load(&g_gfrozen);
    uint64_t lag = atomic_load(&g_glag);
    return host_ns >= lag ? host_ns - lag : 0;
}
// The system counter on that clock: 19.2 MHz on the Switch, 31.25 MHz (32 ns a tick) on the Switch 2, whose SDK reads cntpct_el0 and converts with that rate
// (nn::os::GetSystemTickFrequency answers 0x1dcd650, sdk 20.5.19 Ounce). Read at 19.2 MHz, every tick span a Switch 2 game measured came out 1.63 times too short, and the
// same clock's GPU timestamps 1.63 times too late: Donkey Kong Bananza's frame-rate controller took its frame for seconds long and held it at 20 fps (present interval 3)
uint64_t guest_ticks(void) { return dm_sdk_ounce() ? guest_ns() / 32 : guest_ns() * 12 / 625; }
__attribute__((weak)) void dm_cheats_present(void) {}   // strong definition in hle_cheats.c; standalone tests linking hle_os.c without cheats stay no-ops
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
    dm_cheats_present();   // apply enabled cheats once per presented guest frame across NVN, Vulkan, and OpenGL
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        LOG("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step %llu ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) atomic_fetch_add(&g_gdet, g_gdet_step);
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); }
}
// Freezing has several reasons (0 testing, 1 app in background, 2 system keyboard, 3 NVN prepare, 4 NVN archive init, 5 AlwaysSuspend applet):
// freeze if any holds, release only when all are lifted -- otherwise "switch to background and back while the keyboard is up" or "archive init finishes inside the prepare step"
// would release the clock early
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
// A suspended application does not run: Horizon suspends the whole process out of focus, iOS suspends a background app. The host freezes the guest clock in the background but its
// guest threads go on until they block, so a guest reading the clock twice there saw no time pass. Super Mario Galaxy 2's frame pacing (main+0x5926a8) took the interval between two
// frames as its period, got 0, and the next frame's loop that steps by it (main+0x9abd0) never ended. The same stall on the render thread during the host's first-submission prepare
// step or pipeline archive open (reasons 3 and 4: seconds on a slow disk or cold prebuild) let Vanillaware's main thread skip hundreds of frames and advance into texture teardown
// before frame 1 finished (13 Sentinels: Aegis Rim, SIGSEGV at main+0x309128 / +0x309e28). Guest reads of the system counter (the cntpct gate, svc GetSystemTick) wait while any
// GFZ_WAIT_MASK reason holds. Graphics host work can read the frozen clock during lifecycle and prepare waits so the graphics drain can finish.
enum { GFZ_APPLET = 1u << DM_CLOCK_APPLET, GFZ_WAIT_MASK = (1u << 1) | (1u << 3) | (1u << 4) | GFZ_APPLET };
static pthread_cond_t g_gfz_cv = PTHREAD_COND_INITIALIZER; static _Atomic unsigned g_gfz_wait;
static void guest_wait_for(unsigned mask) {
    pthread_mutex_lock(&g_gfz_mu);
    while (g_gfz_reasons & mask) pthread_cond_wait(&g_gfz_cv, &g_gfz_mu);
    pthread_mutex_unlock(&g_gfz_mu);
}
// Each read also counts on the reading thread: a run of yields whose passes read the clock is a wait for a time, which the yield back-off sleeps from its start (hle_thread.c thr_yield)
__thread uint64_t dm_clock_reads;
void guest_clock_read_wait(void) {
    dm_clock_reads++;
    if (!atomic_load(&g_gfz_wait)) return;
    guest_wait_for(atomic_load(&dm_host_work) ? GFZ_APPLET : GFZ_WAIT_MASK);
}
void guest_applet_wait(void) {
    if (!(atomic_load_explicit(&g_gfz_wait, memory_order_relaxed) & GFZ_APPLET)) return;
    dm_thread_native_wait(1);
    guest_wait_for(GFZ_APPLET);
    dm_thread_native_wait(0);
}
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    unsigned wait = g_gfz_reasons & GFZ_WAIT_MASK;
    unsigned old = atomic_exchange(&g_gfz_wait, wait);
    if (old & ~wait) pthread_cond_broadcast(&g_gfz_cv);   // applet waiters can also resume when another clock freeze still holds
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// Entropy source: true random by default (arc4random); with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (so that replay pictures are byte-for-byte comparable, the camp blue fire particles differed between two passes).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK: deterministic entropy call %llu", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { arc4random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
// The SDK's own getentropy (Unicorn Overlord, SDK 16.2.4: std::random_device at nnMain) fills through nn::crypto::GenerateCryptographicallyRandomBytes, which opens the csrng service;
// Pokémon Legends: Z-A (SDK 19) calls it first thing in nnMain (main+0x1734b4)
// (InitializeCsrng → sm::Initialize, the game aborted there). The call goes through the sdk's PLT, so the takeover gives it the same stream as getentropy above
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service; host entropy (the getentropy stream)", void, g_csrng_bytes, void *p, size_t n) {
    g_getentropy(p, n);
}

// ---------- getenv: the game profile's game_env ----------
// Horizon gives a program no environment, and the SDK's C library starts with an empty environ. Runtimes that take their settings from the environment then run on defaults sized for the
// Switch's 64 GB alias region: Absolum's .NET GC (NativeAOT) reserves 5 × its heap limit as its region range, 3.75 GB, unless DOTNET_GCRegionRange says otherwise.
// The profile's game_env (KEY=VALUE, separated by spaces; DM_GAME_ENV overrides it, also from Documents/env.txt) answers getenv for those names; any other name goes to the SDK's own getenv
static const char **g_env_kv;   // "KEY=VALUE" strings, NULL-terminated
static void game_env_init(void) {
    const char *e = getenv("DM_GAME_ENV"); if (!e) e = DM_GAME_ENV;
    int n = 0; for (const char *p = e; *p; p++) n += *p == '=';
    if (!(g_env_kv = calloc((size_t)n + 1, sizeof *g_env_kv))) die("game_env allocation failed");
    char *s = strdup(e); int k = 0;
    for (char *t = strtok(s, " "); t && k < n; t = strtok(NULL, " ")) if (strchr(t, '=')) g_env_kv[k++] = t;
    if (k) LOG("game environment (%s): %s", getenv("DM_GAME_ENV") ? "DM_GAME_ENV" : "game profile game_env", e);
    else free(s);   // no accepted entry points into the copy
}
static const char *const *game_env(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, game_env_init); return g_env_kv; }
HLE_FORCE("getenv", "the game profile's game_env answers its names; the SDK's own getenv (an empty environ on Horizon) the rest", char *, g_getenv, const char *name) {
    static char *(*orig)(const char *);
    size_t n = name ? strlen(name) : 0;
    for (const char *const *p = game_env(); n && *p; p++) if (!strncmp(*p, name, n) && (*p)[n] == '=') return (char *)*p + n + 1;
    if (!orig && !(orig = guest_sym("getenv"))) return NULL;
    return orig(name);
}

// ---------- nn::mem::StandardAllocator::Allocate(size): Vanillaware's large blocks aligned to 64 KB (profile key alloc_big_align) ----------
// 13 Sentinels 1.10's own malloc (main 4b632573…, main+0x310550) is StandardAllocator::Allocate(size). It gets the storage of a GPU memory pool from malloc rounded only to 4 KB (main+0x3073b8),
//   then cuts three ring heaps out of the pool at 64 KB alignment relative to the GPU address (main+0x30c118), and the pool size is exactly their sum (main+0x307adc). On the Switch a pool's GPU address is aligned to a big page;
//   the host's GPU address is the CPU address, which is only 4 KB aligned, so up to 60 KB is lost at the start, the third ring heap (16 MB, used by the main thread's allocations while a command buffer is recorded) does not fit and keeps base 0,
//   and the first allocation while recording writes through a null pointer (main+0x2fa9a0, when the prologue enters its first battle).
// Unicorn Overlord 1.05 (main c841ffe2…), the same engine: operator new (main+0x5a55d4) takes the calling thread's ring heap (bump allocator main+0x5b8504: cursor +0x20 starts at the base) and one kept base 0.
//   Past the title screen the first allocation from it returned 0, which operator new's null check skipped, and the next returned 0x200; its constructor (main+0x5a3224 via main+0x4c754) wrote to 0x268.
//   With the profile key, allocations of 64 KB or more are aligned to 64 KB (Allocate(size, 0x10000)), which puts the pool storage on a 64 KB boundary. Other games forward unchanged
static void *(*g_sa_alloc)(void *, size_t), *(*g_sa_alloc_al)(void *, size_t, size_t);
static int g_sa_big_align;
static pthread_once_t g_sa_once = PTHREAD_ONCE_INIT;
static void sa_init(void) {
    module_t *ms = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) ms = &g_mod[i];
    g_sa_alloc = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEm") : NULL;
    g_sa_alloc_al = ms ? module_sym(ms, "_ZN2nn3mem17StandardAllocator8AllocateEmm") : NULL;
    if (!g_sa_alloc || !g_sa_alloc_al) die("StandardAllocator::Allocate not found in the SDK");
    g_sa_big_align = DM_GAME_ALLOC_BIG_ALIGN;
    if (g_sa_big_align) LOG("StandardAllocator::Allocate: allocations ≥ 64 KB are 64 KB aligned (game profile alloc_big_align: memory pool storage, see hle_os.c)");
}
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEm", "passed straight to the SDK; with the profile's alloc_big_align (13 Sentinels, Unicorn Overlord) allocations ≥ 64 KB are 64 KB aligned (memory pool start)", void *, h_mem_SAAllocate, void *self, size_t size) {
    pthread_once(&g_sa_once, sa_init);
    return g_sa_big_align && size >= 0x10000 ? g_sa_alloc_al(self, size, 0x10000) : g_sa_alloc(self, size);
}
