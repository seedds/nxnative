// The portable host program, nxhost-run (Linux and Windows, x86-64; dm/host/CMakeLists.txt, NXHOST_PROGRAM): main.c's counterpart for the x86-64 execution engine. main.c stays the
// Apple program, unchanged; this file is a sibling because the two differ in substance, not in a few lines: every step of the SDK start is a guest call through dm_guest_call on the
// engine's main guest thread (design docs/design/aot-recompiler.md 2.4) that may end the process, the game's arguments live in guest memory, the start-up needs the arena, the
// engine's fault handlers and the process's start time on Linux and Windows, and a module without the SDK can be run as homebrew (DM_AOT_RAW). The order of the start is main.c's.
//   nxhost-run <dir> [module...] [-- game arguments...]
//     <dir>: the game's recompiler input, <NSP folder>/<id>_build/aot_<version>/ (dm/tools/aot/README.md, "A game module in the host"): per module its manifest <name>.aot.tsv, its
//     behaviour image <name>.image.bin (patch_nso.py --aot) and its pack <name>.aot.so (.aot.dll on Windows; nxaot build). The modules: the arguments, else the game profile's
//     modules list; the game's arguments: those after "--", else the profile's game_args. The DM_* switches mean what they mean on Apple (docs/env_switches.tsv): DM_WINDOW opens
//     the SDL3 window (front_sdl.c), DM_AUDIO the sound output, DM_ROMFS / DM_SAVE / DM_TEMP the game's files, DM_WATCH the watchdog, and so on.
//   The engine: the JIT fallback is in the program when it was built with NXHOST_JIT (CMake option); without it guest code that has no compiled entry stops the process with
//   status 3 and a [RED] line naming the address. The exit status is the run's (aot.h: 0 the process ended, 1 a trap, 3 the JIT needed, 4 a fault, 6 guest code returned to the
//   host, 7 guest threads that did not stop).
// Switches of this program alone: DM_AOT_RAW=1 runs the first module's own entry (its pack's entry, x0 = 0, x1 = the main thread's handle, as a homebrew NRO's _start is entered)
// instead of the SDK start: for a module without the SDK (t_host_program's synthetic module, homebrew). It is a switch and not a guess from the missing SDK symbols, because a game's
// module list that lacks its sdk module would otherwise run main's rtld entry as homebrew: without it a module set with no nnMain stops with a [RED] line that names the switch.
// DM_VK_HEADLESS=1 keeps the Vulkan backend's frames off the window (dm/gfx/hostvk/vk_present.c).
// The engine's sampler: DM_AOT_PROFILE=<folder> (DM_AOT_PROFILE_HZ) writes the profile of the run, <folder>/<module>.tsv, when the process exits (aot.h, "The sampler"); the packer reads it
// from <NSP folder>/<id>-work/profile/ for the next packs (nxaot build --profile; docs/development.md). Linux only for now.
#include "aot.h"
#include "front.h"
#include <stdatomic.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
#if NXHOST_VULKAN
#include "vk_present.h"   // the window the Vulkan backend's presenter draws into (front_sdl.c's surface)
#endif

void hid_keys_init(void); void stubs_report(void); void guest_thread_init(void); uint32_t svc_thread_main_init(void);
void crash_set_dump_hook(void (*fn)(void)); void svc_thread_dump(void);
static void *req(const char *n) { void *p = guest_sym(n); if (!p) die("missing symbol %s", n); return p; }

// Game arguments (main.c): argv[0] is "game", what follows "--" is appended; the SDK reads them from guest memory (SetHostArgv), so they are copied into the arena at the start
static const char *g_argv[64] = {"game"}; static int g_argc = 1;
static int g_status = DM_AOT_EXITED;

static int left(void) { return dm_aot_main_left() != DM_AOT_LEFT_NONE; }   // the guest call ended the process (or the thread): the start goes no further
static void call0(dm_guest_state *st, const char *name) { dm_guest_call(st, (uint64_t)(uintptr_t)req(name), 0); }
static char **guest_argv(void) {   // g_argv in the arena: the pointer array and the strings, guest addresses being host addresses there
    size_t bytes = sizeof(char *) * 64;
    for (int i = 0; i < g_argc; i++) bytes += strlen(g_argv[i]) + 1;
    char **a = dm_guest_alloc(bytes), *s = (char *)(a + 64);
    for (int i = 0; i < g_argc; i++) { size_t n = strlen(g_argv[i]) + 1; memcpy(s, g_argv[i], n); a[i] = s; s += n; }
    return a;
}

static void guest_main(void) {
    guest_thread_init();                // the TLS block and thread-local region in the arena (thread.c), into this thread's state block
    uint32_t hmain = svc_thread_main_init();   // the main thread's handle (x0 of nnosInitialize); its engine state block (hle_thread.c)
    const char *raw = getenv("DM_AOT_RAW");
    dm_guest_state *st = dm_aot_main_begin();   // the main guest stack (8 MiB in the arena) and the packs' FPCR
    // main.c's tool modes run the game's own decoder from the host (hle_oodle.c): here each call is a guest call on this thread, from its guest stack
    if (!(raw && atoi(raw))) { void dm_oodle_serve_maybe(int); dm_oodle_serve_maybe(0); }   // DM_OODLE_SERVE (exits when the input ends)
    if (raw && atoi(raw)) {
        const module_t *m = &g_mod[0];
        const dm_aot_pack *p = NULL;
        dm_aot_find((uint64_t)(uintptr_t)m->base, &p);
        if (!p) die("[RED] DM_AOT_RAW: module %s has no pack loaded", m->name);
        const uint64_t entry = (uint64_t)(uintptr_t)m->base + dm_aot_pack_desc(p)->entry;
        char at[64], msg[160];
        dm_aot_guest_name(entry, at, sizeof at);
        LOG(">>> entry %s (DM_AOT_RAW=1: the module's own entry, no SDK start)", at);
        dm_guest_call(st, entry, 2, (uint64_t)0, (uint64_t)hmain);   // as a homebrew _start: x0 = no loader configuration, x1 = the main thread's handle
        snprintf(msg, sizeof msg, "[RED] aot: guest code returned to the host from its entry %s", at);
        g_status = dm_aot_main_end(msg);
    } else {
        // nn::init::Start's order (main.c has why each step is where it is); each step is an outermost guest call, after which the process may have ended
        if (!guest_sym("nnMain"))
            die("[RED] missing symbol nnMain: no module of %s defines it (the game's sdk module missing from the list?); DM_AOT_RAW=1 runs a module without the SDK from its own entry",
                g_nmod ? g_mod[0].name : "the list");
        call0(st, "_init_libc0"); if (left()) goto end;
        LOG(">>> nnosInitialize"); dm_guest_call(st, (uint64_t)(uintptr_t)req("nnosInitialize"), 2, (uint64_t)hmain, (uint64_t)0); if (left()) goto end;
        call0(st, "_init_libc1"); if (left()) goto end;
        LOG(">>> nninitInitializeSdkModule"); call0(st, "nninitInitializeSdkModule"); if (left()) goto end;
        dm_guest_call(st, (uint64_t)(uintptr_t)req("_ZN2nn2os11SetHostArgcEi"), 1, (uint64_t)g_argc); if (left()) goto end;
        dm_guest_call(st, (uint64_t)(uintptr_t)req("_ZN2nn2os11SetHostArgvEPPc"), 1, (uint64_t)(uintptr_t)guest_argv()); if (left()) goto end;
        LOG(">>> nninitStartup"); call0(st, "nninitStartup"); if (left()) goto end;
        call0(st, "_init_libc2"); if (left()) goto end;
        run_module_inits(); if (left()) goto end;   // each module's DT_INIT, each a guest call (loader.c)
        { void dm_guest_spawn_selftest(void); dm_guest_spawn_selftest(); }   // DM_GUEST_SPAWN_TEST (main.c): a host-created guest thread, its SDK calls guest calls (hle_guestthread.c)
        if (left()) goto end;
        { void dm_oodle_serve_maybe(int); dm_oodle_serve_maybe(1); }   // DM_OODLE_SERVE with a decoder that needs the game's malloc (LZ5, hle_oodle.c)
        { void dm_oodle_job_maybe(void); dm_oodle_job_maybe(); }       // DM_OODLE_JOB: a job file decoded with the game's own decoder, then exit (hle_oodle.c)
        LOG(">>> nnMain"); call0(st, "nnMain");
        if (!left()) LOG("nnMain returned");
    end:
        g_status = dm_aot_main_end(NULL);   // nnMain returning ends the process, as on Horizon
    }
    dm_aot_stats_t s; dm_aot_stats(&s);
    static const char *const why[] = {"the process ended", "a trap", "usage", "the JIT fallback needed", "a fault in guest code", "?", "guest code returned to the host", "guest threads that did not stop"};
    LOG("guest code ended: status %d (%s); %llu SVCs, %llu host calls, %llu dispatcher misses, %llu JIT addresses, %llu threads", g_status,
        g_status >= 0 && g_status <= DM_AOT_STUCK ? why[g_status] : "?", (unsigned long long)s.svcs, (unsigned long long)s.host_calls, (unsigned long long)s.misses,
        (unsigned long long)s.jit_addresses, (unsigned long long)s.threads);
    fflush(NULL);
    exit(g_status);   // the window's thread or the main thread: front_sdl.c's exit handler gives the screen's mode back
}

struct dm_flags g_flag;
uint64_t g_t0_ns;
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

// Zero point of the boot timeline: the process's creation time on now_ns's clock, as main.c takes it from sysctl on Apple
static uint64_t proc_start_mono_ns(void) {
    uint64_t mono = now_ns();
    int64_t age = 0;
#if defined(_WIN32)
    FILETIME c, e, k, u, n;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        GetSystemTimeAsFileTime(&n);
        uint64_t c100 = (uint64_t)c.dwHighDateTime << 32 | c.dwLowDateTime, n100 = (uint64_t)n.dwHighDateTime << 32 | n.dwLowDateTime;
        age = n100 > c100 ? (int64_t)((n100 - c100) * 100) : 0;   // FILETIME counts 100 ns
    }
#else
    // /proc/self/stat field 22: the start in clock ticks since boot, the clock that counts suspend (plat_boot_ns)
    FILE *f = fopen("/proc/self/stat", "r");
    char b[1024]; size_t got = f ? fread(b, 1, sizeof b - 1, f) : 0;
    if (f) fclose(f);
    b[got] = 0;
    const char *p = strrchr(b, ')');   // the command name may hold spaces: the fields after it are counted from field 3
    unsigned long long start = 0; long hz = sysconf(_SC_CLK_TCK);
    for (int field = 2; p && *p && field < 22; ) { p = strchr(p + 1, ' '); field++; if (p && field == 22) start = strtoull(p + 1, NULL, 10); }
    if (start && hz > 0) age = (int64_t)plat_boot_ns() - (int64_t)(start * (1000000000ull / (unsigned long long)hz));
#endif
    return age > 0 && (uint64_t)age < mono ? mono - (uint64_t)age : mono;
}

static void host_preload(int argc, char **argv) {
    g_t0_ns = proc_start_mono_ns();
    flags_init();
    if (argc < 2) { fprintf(stderr, "usage: nxhost-run <dir> [module...] [-- game arguments...]\n  <dir>: the game's aot_<version> folder (<name>.aot.tsv, <name>.image.bin, <name>%s per module)\n", DM_AOT_PACK_SUFFIX); exit(DM_AOT_USAGE); }
    if (getenv("DM_FIXED_TIME")) setenv("TZ", "UTC", 1);   // deterministic comparison: the calendar in UTC (main.c)
#if defined(_WIN32)
    _setmaxstdio(4096);   // the C runtime's open stream limit (512 by default) as main.c raises the descriptor limit
#else
    struct rlimit rl; if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < 4096) {   // the game's files, RomFS archives, save transactions and caches share it (main.c)
        rlim_t was = rl.rlim_cur; rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &rl)) LOG("open file limit %llu could not be raised", (unsigned long long)was);
    }
#endif
    dm_aot_arena_init();   // the arena below 2^39 first, before anything else takes address space in it
    // The pages the arena commits and decommits for the host's guest memory (the garena: Unreal's heap, the guest Vulkan layer's mappings, ...) reach the graphics backend as
    // the memory SVCs' do (aot.h dm_garena_set_commit_hooks; dm/gfx/nvn/nvn_be.h), so a pool placed in them is imported, and its import released before they are decommitted
    { void be_guest_map(void *mem, uint64_t size), be_guest_unmap(void *mem, uint64_t size); dm_garena_set_commit_hooks(be_guest_map, be_guest_unmap); }
#if NXHOST_VULKAN
    // The guest Vulkan layer's host-visible memory, which the game maps, is arena pages it imports (dm/gfx/hostvk/guest_vk.c gmem_import, VK_EXT_external_memory_host)
    { void guest_vk_set_arena(void *(*alloc)(size_t n), void (*release)(void *p, size_t n)); guest_vk_set_arena(dm_guest_alloc, dm_guest_free); }
#endif
    crash_install();       // aot_fault.c: the engine's fault handlers, the alternate signal stack, DM_WATCH / DM_WATCH_STALL
    dm_aot_profile_init(); // aot_profile.c: DM_AOT_PROFILE=<folder>, the sampler; at exit() (the game's end, the window closed; not SIGTERM's _exit) <folder>/<module>.tsv
    const char *race = getenv("DM_TEST_KEYRACE");
    if (race && *race) LOG("[RED] DM_TEST_KEYRACE tests Apple's TSD thread slots (loader.c tsd_check); the x86-64 engine keeps the thread registers in its state blocks: not run");
    { void dm_term_install(void); dm_term_install(); }   // SIGTERM / console close: the graceful exit (hle_lifecycle.c)
    crash_set_dump_hook(svc_thread_dump);                 // the guest threads' state in a crash report and the watchdog's dump
#if NXHOST_JIT
    const char *jit = "with the JIT fallback (dynarmic) for guest code without a compiled entry";
#else
    const char *jit = "without the JIT fallback (CMake NXHOST_JIT off): guest code without a compiled entry stops the process with status 3";
#endif
    LOG("nxhost-run: modules from %s; x86-64 engine %s", argv[1], jit);
    const char *names[argc + DM_GAME_NMOD]; int n = 0;
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--"); i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    else if (*DM_GAME_ARGS) {   // no "--": the game profile's game_args, split at spaces (main.c)
        char *s = strdup(DM_GAME_ARGS);
        for (char *t = strtok(s, " "); t && g_argc < 63; t = strtok(NULL, " ")) g_argv[g_argc++] = t;
        LOG("game arguments from the game profile: %s", DM_GAME_ARGS);
    }
    if (!n) { static const char *def[] = DM_GAME_MODULES; for (int k = 0; k < DM_GAME_NMOD; k++) names[n++] = def[k]; }   // the game profile's modules
    load_modules(argv[1], names, n);   // loader.c's game-module path: manifests, images, GOT bindings, packs
    dm_sdk_major();
    if (getenv("DM_PROBE")) LOG("[RED] DM_PROBE: the probes put brk words into the game's code (probe.c, Apple's native execution); the x86-64 engine runs packs: not set");
    hid_keys_init();   // DM_KEYS scripted input
    stubs_report();
    { void loader_hash_log(void); loader_hash_log(); }   // DM_LOADHASH=1
    // Unreal's allocator on a host zone, for the builds whose constructor call is cut (hle_uemalloc.c): its vtable's host functions at their guest-visible addresses
    // (dm_guest_fnptr), the vtable in the arena
    { void dm_ue_malloc_install(void); dm_ue_malloc_install(); }
}
static struct { int argc; char **argv; pthread_t th; _Atomic int started; } g_pre;
static void *preload_thread(void *a) { (void)a; host_preload(g_pre.argc, g_pre.argv); return NULL; }
// main.c's early start (DM_PRELOAD exercises it): the loading on a thread of its own, joined by main
void dm_host_preload_start(int argc, char **argv) {
    if (atomic_exchange(&g_pre.started, 1)) return;
    g_pre.argc = argc; g_pre.argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_pre.argv[i] = strdup(argv[i]);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&g_pre.th, &a, preload_thread, NULL)) die("dm_host_preload_start: cannot start the thread");
}
int main(int argc, char **argv) {
    const char *pso = getenv("DM_PSO_PREBUILD");
    if (pso && !strcmp(pso, "tool")) { fprintf(stderr, "[dm] [RED] DM_PSO_PREBUILD=tool is the Metal pipeline archive's prebuild (nvn_mtl.m); the Vulkan backend's is DM_VK_PREBUILD=1 at a game's start\n"); return DM_AOT_USAGE; }
    if (getenv("DM_PRELOAD")) dm_host_preload_start(argc, argv);
    if (atomic_load(&g_pre.started)) {
        pthread_join(g_pre.th, NULL);
        int same = argc == g_pre.argc; for (int i = 0; same && i < argc; i++) same = !strcmp(argv[i], g_pre.argv[i]);
        if (!same) die("[RED] dm_host_preload_start arguments differ from main's");
        crash_thread_init();   // the alternate signal stack is per thread: the loading made it on its own
        LOG("host loading already finished early in the background (dm_host_preload_start)");
    } else host_preload(argc, argv);
    { void dm_fps_state_apply(void); dm_fps_state_apply(); }   // frame rate and render scale into the frame step's state page (loader.c)
#if NXHOST_VULKAN
    hvk_present_set_window(front_vk_create_surface, front_vk_pixel_size);   // the Vulkan presenter draws into the front end's window (vk_present.c), when there is one
#endif
    front_run(guest_main);   // DM_WINDOW: the window on this thread, the guest on a 64 MiB thread; else the guest here
    return g_status;
}
