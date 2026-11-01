// Every way a function address crosses between the host and recompiled guest code on the x86-64 engine (docs/design/aot-recompiler.md 2.4; aot.h, "Host functions handed
// to the guest"), with no game files: the host-function module of dm/tools/tests/aot_synth.py (--hostfn; a module named main of hand-assembled AArch64 code, its behaviour
// image and manifest as patch_nso.py --aot writes them, and an NRO named sub with its nro.tsv row), packed by nxaot (dm/tools/aot/tests/build_packs.sh, into <packs>/hostfn:
// main.aot.so from the manifest, sub.aot.so from the NRO). Each run is a child process (this program with --run), as a game is a process:
//   the run: load_modules binds main's imports into its GOT: strlen and sqrtf, the C library passthrough (dm/host/libc_pass.c's PASS entries), at their host-call thunks'
//     slots; the test's entries (t_aot_hostfn_entries.c) and nn::ro's forced take-overs (hle_ro.c) at theirs; sub_value, which nothing defines yet, at a stub slot. The host
//     puts sub.nro's bytes in guest memory (nro_image) and runs main from its entry (dm_aot_run). main calls the passthrough functions and checks nothing itself; it stores
//     what each step returned in results[], which this test checks against aot_synth.py's HF_EXPECTED: strlen and sqrtf through the GOT; a host function an entry returns
//     (t_hf_get: its thunk hands over the guest-visible slot, gen_hle_thunks.py's function pointer return) called with blr, then stored in a vtable-like table and called
//     from there; host functions the host writes into guest memory (t_hf_install: dm_guest_fnptr), one with a thunk, one without (its stub answers 0, with a [RED] line when
//     handed over and an UNIMPL line when called); a guest thread the host creates through hle_guestthread.c (the module's own nn::os functions and malloc, its thread entry
//     the slot of spawn_tramp), whose host body calls the module's synth_triple on the new thread; an NRO loaded at run time through hle_ro.c (loader.c loader_nro_map:
//     its nro.tsv row, its image mapped from the game's bytes, its pack loaded, its strlen import bound, its DT_INIT a guest call), two symbols looked up in it and
//     called four times in a loop, one of them calling main back (calls between packs: the dispatcher binds the call site's inline cache to the other pack's standard
//     entry, whose signature every pack shares since ABI 3, so after the first pass no call reaches the host; t_hf_mark snapshots the counters after the first pass and
//     after the last), main's own import of it rebound at the load and again at UnloadModule
//   then the host calling the module's code back (aot.h dm_gcall, DM_GUEST_CALL: the form of the host's plain-pointer call sites): main registers its callback hf_cb with
//     t_re_register; t_re_fire calls it from inside that host call on the same thread (nested re-entry), with ten arguments (two on the guest stack) and x22 kept around it;
//     t_re_deep nests two levels (host, guest, host, guest); t_re_thread calls it from a guest thread the host creates (the audio renderer's render thread is one);
//     t_re_hostthread from a host thread that is no guest thread (its own guest context, a [RED] line); t_re_bind takes hle_thread.c's real system-event path, whose SDK
//     functions (main's) are nested guest calls, and main then waits on the signalled event; t_re_stop holds a lock across a guest call that ends the process: the call
//     returns, the host function releases the lock, and the run ends with ExitProcess's status
//   then host memory the guest sees (aot.h, "Host memory the guest sees"): t_ga_buf hands main a garena buffer, which it reads; t_ga_str an arena copy of a literal, which it
//     reads and hands to strlen; t_ga_local passes main's hf_sum2 an array of the thread's scoped region (DM_GLOCAL); each address lies in the arena below 2^39. t_ga_outside
//     hands over host memory on purpose (a host stack array to hf_sum2, a host string returned and passed on to strlen): run again with DM_AOT_ARENA_CHECK=1 (1b), the guard
//     flags exactly those three, once each, and nothing else the run exchanges
//   a refusal: the same folder without sub.aot.so: ro::LoadModule fails with a [RED] line naming the pack, and the module's call through the missing symbol stops the run
// What this does not show: a real NRO, SDK or game (no game files here); Windows (a DLL pack: not built here)
//   t_aot_hostfn [--module <dir>]
//     the module's folder: --module, else <packs>/hostfn with the packs of NXAOT_PACKS (the environment variable, else the CMake option); with none, a note and exit 0
//   t_aot_hostfn --run <dir>     one run (the child)
#include "../hle.h"
#include "../aot.h"
#include "../aot_hostfn.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>
#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

#ifndef T_AOT_PACKS
#define T_AOT_PACKS ""
#endif

void guest_thread_init(void);          // thread.c
uint32_t svc_thread_main_init(void);   // hle_thread.c
// The host functions the module reaches (t_aot_hostfn_entries.c, libc_pass.c, hle_ro.c) and the entries' counters
uint64_t t_hf_mul(uint64_t a, uint64_t b), t_hf_add(uint64_t a, uint64_t b);
void *t_hf_get(void); void t_hf_install(void **vt); uint64_t t_hf_spawn(uint64_t x);
size_t libc_strlen(const char *s); float libc_sqrtf(float x);
Result h_ro_LoadModule(void *mod, const void *image, void *buf, size_t size, int flag);
Result h_ro_LookupModuleSymbol(uintptr_t *out, const void *mod, const char *name);
void h_ro_UnloadModule(void *mod);
extern _Atomic uint64_t g_t_hf_calls[3];
void t_hf_mark(uint64_t phase);
extern dm_aot_stats_t g_t_hf_mark[2];   // the engine's counters after the cross-pack loop's first pass and after its last
extern _Atomic int g_t_hf_marks;
struct t_re_seen { int fire_depth, leaf_depth, thread_other, host_fresh, host_ctx, signalled, stop_left, stop_depth, stop_status, stop_resumed; uint64_t stop_r; };
extern struct t_re_seen g_re_seen;     // what the t_re_* entries saw (t_aot_hostfn_entries.c)
extern pthread_mutex_t g_re_stop_mu;   // t_re_stop's lock
// Host memory the guest sees (aot.h, the one rule): what the t_ga_* entries handed main (t_aot_hostfn_entries.c), and the word t_ga_buf wrote (aot_synth.py HF_GA_WORD)
struct t_ga_seen { uint64_t buf, str, local, local_sum, outside_sum; };
extern struct t_ga_seen g_ga_seen;
#define T_GA_WORD 0x1122334455667788ull
static int below_2_39(uint64_t a, uint64_t n) { return dm_aot_in_arena(a, n) && a + n <= 1ull << 39; }

// aot_synth.py's HF_EXPECTED (results[12], the pointer t_hf_get handed over, is checked against the slot of t_hf_mul)
static const uint64_t EXPECTED[12] = {19, 0x40200000, 1030, 1056, 123, 0, 42, 0, 0, 4048, 77, 0};
static const char *const STEP[12] = {"strlen through the GOT (the C library passthrough)", "sqrtf(6.25f) through the GOT, the float's bits",
                                     "the host function t_hf_get returned, called with blr", "the same, from the module's table hf_vt[1]",
                                     "hf_vt2[0], written by the host through dm_guest_fnptr (t_hf_add)", "hf_vt2[1], a host function with no thunk: its stub answers 0",
                                     "t_hf_spawn: a guest thread made through hle_guestthread.c ran synth_triple(14)", "nn::ro::LoadModule of sub.nro",
                                     "nn::ro::LookupModuleSymbol(sub_fn)", "sub_fn(5) in the NRO's pack, four passes of the cross-pack loop: 4 x (5 + strlen + the flag its DT_INIT set)",
                                     "sub_value through main's GOT, bound to the NRO at its load", "sub_value through main's GOT after UnloadModule: the stub again"};
// aot_synth.py HF_EXPECTED[13..19]: the host calling guest code back (results[18], the event's binding, is any non-zero value)
static const uint64_t RE_EXPECTED[7] = {1045, 0x1234, 116, 1054, 1064, 0, 0};
static const char *const RE_STEP[7] = {"t_re_fire(1): hf_cb(data, 1, 2..9) called by the host inside that host call, on main's thread (nested re-entry, two stacked arguments)",
                                       "x22 across t_re_fire's nested guest call (callee-saved)", "t_re_deep(5): host, hf_deep, host (t_re_leaf), synth_triple: 15 + 100 + 1",
                                       "t_re_thread(10): hf_cb from a guest thread the host created (hle_guestthread.c)",
                                       "t_re_hostthread(20): hf_cb from a host thread that is no guest thread (a guest context of its own)", "(the binding)",
                                       "svcWaitSynchronization(timeout 0) on the readable handle of the event t_re_bind bound and signalled"};
// aot_synth.py HF_EXPECTED[20..21]: the cross-pack loop
static const uint64_t CROSS_EXPECTED[2] = {88, 0};
static const char *const CROSS_STEP[2] = {"sub_call(synth_triple, 7) four times: the NRO's pack calls main's synth_triple back, 4 x (21 + 1)",
                                          "nn::ro::LookupModuleSymbol(sub_call)"};
enum { HF_CROSS_SITES = 3 };   // the loop's calls between packs: main -> sub_fn, main -> sub_call, sub_call -> main's synth_triple
enum { HF_GOT_STRLEN, HF_GOT_SQRTF, HF_GOT_GET, HF_GOT_INSTALL, HF_GOT_SPAWN, HF_GOT_LOAD, HF_GOT_LOOKUP, HF_GOT_SUB_VALUE, HF_GOT_UNLOAD, HF_GOT_MARK = 17 };   // aot_synth.py HF_GOT_SYMS
enum { SUB_GOT = 0x2000, SUB_FLAG = 0x2008 };   // aot_synth.py: the NRO's GOT slot (strlen) and the word its DT_INIT sets

// ---------- The child: one run ----------
static int g_checks, g_fails, g_status = DM_AOT_USAGE;
static const char *g_dir;
static void check(int ok, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    g_checks++; g_fails += !ok;
    printf("check %s ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap); putchar('\n');
    va_end(ap);
}
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len > 0 ? (size_t)len + 1 : 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(p); return NULL; }
    fclose(f); p[len] = 0; *n = (size_t)len;
    return p;
}
static uint64_t slot(const void *fn) { return dm_aot_hostfn_find(fn); }
static int is_slot_of(uint64_t v, const void *fn, const char *cname) {
    const char *n = dm_aot_hostfn_name(v);
    return v && v == slot(fn) && n && !strcmp(n, cname);
}

static void *guest_main(void *arg) {
    (void)arg;
    guest_thread_init();                             // thread.c: TLS block and thread-local region in the arena
    const uint32_t hmain = svc_thread_main_init();   // hle_thread.c: this thread is the main guest thread
    const char *names[] = {"main"};
    load_modules(g_dir, names, 1);                   // dies with a [RED] line when the module or its pack is refused
    module_t *m = &g_mod[0];
    const uint64_t base = (uint64_t)(uintptr_t)m->base;
    uint64_t *got = module_sym(m, "hf_got"), *results = module_sym(m, "results"), *vt = module_sym(m, "hf_vt"), *vt2 = module_sym(m, "hf_vt2");
    uint64_t *nro_image = module_sym(m, "nro_image");
    check(got && results && vt && vt2 && nro_image, "the module's symbols (hf_got, results, hf_vt, hf_vt2, nro_image)");
    if (!got || !results || !vt || !vt2 || !nro_image) return NULL;
    // The imports in the GOT (design 2.4): the passthrough's, the entries', the forced take-overs', and a stub
    check(is_slot_of(got[HF_GOT_STRLEN], (const void *)libc_strlen, "libc_strlen") && is_slot_of(got[HF_GOT_SQRTF], (const void *)libc_sqrtf, "libc_sqrtf"),
          "GOT: strlen and sqrtf at the slots of the passthrough's libc_strlen and libc_sqrtf (%#llx, %#llx)", (unsigned long long)got[HF_GOT_STRLEN],
          (unsigned long long)got[HF_GOT_SQRTF]);
    check(is_slot_of(got[HF_GOT_GET], (const void *)t_hf_get, "t_hf_get") && is_slot_of(got[HF_GOT_INSTALL], (const void *)t_hf_install, "t_hf_install") &&
          is_slot_of(got[HF_GOT_SPAWN], (const void *)t_hf_spawn, "t_hf_spawn") && is_slot_of(got[HF_GOT_MARK], (const void *)t_hf_mark, "t_hf_mark"),
          "GOT: the test's entries at their slots");
    check(is_slot_of(got[HF_GOT_LOAD], (const void *)h_ro_LoadModule, "h_ro_LoadModule") && is_slot_of(got[HF_GOT_LOOKUP], (const void *)h_ro_LookupModuleSymbol, "h_ro_LookupModuleSymbol") &&
          is_slot_of(got[HF_GOT_UNLOAD], (const void *)h_ro_UnloadModule, "h_ro_UnloadModule"), "GOT: nn::ro's forced take-overs (hle_ro.c) at their slots");
    check(dm_aot_stub_index(got[HF_GOT_SUB_VALUE]) >= 0, "GOT: sub_value, which no loaded module defines yet, at stub slot %ld", dm_aot_stub_index(got[HF_GOT_SUB_VALUE]));
    // The NRO the module loads: its bytes in guest memory, as a game reads a file into its heap
    char path[1200]; size_t nn = 0;
    snprintf(path, sizeof path, "%s/sub.nro", g_dir);
    uint8_t *nro = slurp(path, &nn);
    check(nro != NULL && nn >= 0x80, "sub.nro read (%zu bytes)", nn);
    if (!nro) return NULL;
    uint8_t *gnro = dm_guest_alloc(nn);
    memcpy(gnro, nro, nn); free(nro);
    *nro_image = (uint64_t)(uintptr_t)gnro;
    // The run
    const dm_aot_pack *p = NULL;
    dm_aot_find(base, &p);
    check(p != NULL, "main's pack is loaded");
    if (!p) return NULL;
    g_status = dm_aot_run(base + dm_aot_pack_desc(p)->entry, 0, hmain, NULL);
    check(g_status == DM_AOT_EXITED, "the run ended with ExitProcess (status %d)", g_status);
    for (int i = 0; i < 12; i++)
        check(results[i] == EXPECTED[i], "results[%d] = %#llx (expected %#llx): %s", i, (unsigned long long)results[i], (unsigned long long)EXPECTED[i], STEP[i]);
    const uint64_t mul = slot((const void *)t_hf_mul);
    check(mul && results[12] == mul && vt[1] == mul, "t_hf_get's thunk handed over t_hf_mul's slot %#llx (the guest kept %#llx in hf_vt[1])", (unsigned long long)results[12],
          (unsigned long long)vt[1]);
    check(is_slot_of(vt2[0], (const void *)t_hf_add, "t_hf_add") && dm_aot_stub_index(vt2[1]) >= 0,
          "dm_guest_fnptr: hf_vt2[0] the slot of t_hf_add, hf_vt2[1] a stub slot (%ld) for the function with no thunk", dm_aot_stub_index(vt2[1]));
    check(g_t_hf_calls[0] == 2 && g_t_hf_calls[1] == 1 && g_t_hf_calls[2] == 1, "the host functions ran: t_hf_mul %llu times, t_hf_add %llu, the thread's body %llu",
          (unsigned long long)g_t_hf_calls[0], (unsigned long long)g_t_hf_calls[1], (unsigned long long)g_t_hf_calls[2]);
    module_t *sub = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sub")) sub = &g_mod[i];
    check(sub && sub->nro && !sub->loaded && sub->loads == 1 && dm_aot_in_arena((uint64_t)(uintptr_t)sub->base, sub->size),
          "the NRO sub: an NRO module in the arena at %p, loaded once and unloaded", sub ? (void *)sub->base : NULL);
    if (sub) {
        const dm_aot_pack *sp = NULL;
        dm_aot_find((uint64_t)(uintptr_t)sub->base + 0x80, &sp);
        uint64_t sg, sf; memcpy(&sg, sub->base + SUB_GOT, 8); memcpy(&sf, sub->base + SUB_FLAG, 8);
        check(sp != NULL && sp != p && is_slot_of(sg, (const void *)libc_strlen, "libc_strlen") && sf == 1000,
              "the NRO's pack sub.aot.so is loaded, its strlen import at the passthrough's slot, its DT_INIT ran (sub_flag %llu)", (unsigned long long)sf);
    }
    check(dm_aot_stub_index(got[HF_GOT_SUB_VALUE]) >= 0, "GOT: sub_value at its stub slot again after UnloadModule");
    // Calls between packs (ABI 3): both packs record the standard entries' one signature, so the dispatcher binds each call site to the other pack's entry and later passes
    // go there directly; nothing goes through jit_entry, the other pack's enter or the JIT fallback
    for (int i = 0; i < 2; i++)
        check(results[20 + i] == CROSS_EXPECTED[i], "results[%d] = %#llx (expected %#llx): %s", 20 + i, (unsigned long long)results[20 + i],
              (unsigned long long)CROSS_EXPECTED[i], CROSS_STEP[i]);
    const dm_aot_pack *sp2 = NULL;
    if (sub) dm_aot_find((uint64_t)(uintptr_t)sub->base + 0x80, &sp2);
    check(sp2 && nxaot_same_std_abi(dm_aot_pack_desc(p), dm_aot_pack_desc(sp2)) && dm_aot_pack_desc(p)->std_x == 0xFFu && dm_aot_pack_desc(p)->std_v == 0xFFu &&
          dm_aot_pack_desc(p)->rv == 0, "main's and sub's packs record the same standard-entry signature (x0..x7, v0..v7 in host registers, rv 0)");
    const dm_aot_stats_t *w0 = &g_t_hf_mark[0], *w1 = &g_t_hf_mark[1];
    check(g_t_hf_marks == 2 && w0->cross_pack == HF_CROSS_SITES && w1->misses == w0->misses && w1->cross_pack == w0->cross_pack &&
          w1->cross_pack_handovers == 0 && w1->jit_addresses == 0 && w1->jit_runs == 0,
          "the cross-pack loop: its %d call sites bound in the first pass (%llu cross-pack bindings), then %llu dispatcher misses over the other 3 passes (9 calls between "
          "the packs), %llu hand-overs through jit_entry, %llu JIT addresses", HF_CROSS_SITES, (unsigned long long)w0->cross_pack,
          (unsigned long long)(w1->misses - w0->misses), (unsigned long long)w1->cross_pack_handovers, (unsigned long long)w1->jit_addresses);
    dm_aot_stats_t se;
    dm_aot_stats(&se);
    check(se.cross_pack == HF_CROSS_SITES + 1 && se.cross_pack_handovers == 0,
          "after the loop, sub_value through main's GOT: one more call site bound to the NRO's pack (%llu in all), still no hand-over (%llu)",
          (unsigned long long)se.cross_pack, (unsigned long long)se.cross_pack_handovers);
    // The host calling guest code back
    for (int i = 0; i < 7; i++)
        if (i != 5) check(results[13 + i] == RE_EXPECTED[i], "results[%d] = %#llx (expected %#llx): %s", 13 + i, (unsigned long long)results[13 + i], (unsigned long long)RE_EXPECTED[i], RE_STEP[i]);
    check(results[18] != 0 && g_re_seen.signalled == 1, "t_re_bind: hle_bind_system_event made the event through main's CreateSystemEvent and GetWritableHandleOfSystemEvent "
          "(nested guest calls), binding %llu, signalled through it %d", (unsigned long long)results[18], g_re_seen.signalled);
    check(g_re_seen.fire_depth == 1 && g_re_seen.leaf_depth == 2, "nesting depth seen by the host functions: t_re_fire %d (expected 1), t_re_leaf %d (expected 2)",
          g_re_seen.fire_depth, g_re_seen.leaf_depth);
    check(g_re_seen.thread_other, "t_re_thread's callback ran on another guest thread than main's");
    check(g_re_seen.host_fresh && g_re_seen.host_ctx, "t_re_hostthread: the host thread had no state block before its call (%d) and a host thread's context after it (%d)",
          g_re_seen.host_fresh, g_re_seen.host_ctx);
    check(g_re_seen.stop_resumed && g_re_seen.stop_r == 0 && g_re_seen.stop_left == DM_AOT_LEFT_STOP && g_re_seen.stop_depth == 1 && g_re_seen.stop_status == DM_AOT_EXITED,
          "t_re_stop: its nested guest call ended the process and returned 0 to it (left %d, depth %d, stop status %d), and the host function ran on to its end",
          g_re_seen.stop_left, g_re_seen.stop_depth, g_re_seen.stop_status);
    const int unlocked = pthread_mutex_trylock(&g_re_stop_mu) == 0;
    if (unlocked) pthread_mutex_unlock(&g_re_stop_mu);
    check(unlocked, "t_re_stop's lock was released: no host frame was skipped when guest code left");
    // Host memory the guest sees: what the t_ga_* entries handed main lies in the arena below 2^39, and main read it (aot_synth.py HF_GA_EXPECTED in ga_results)
    const uint64_t *ga = module_sym(m, "ga_results");
    check(ga != NULL, "the module's ga_results");
    if (ga) {
        check(ga[0] && ga[0] == g_ga_seen.buf && below_2_39(ga[0], 64) && ga[1] == T_GA_WORD,
              "t_ga_buf: a garena buffer at %#llx, in the arena below 2^39; main read the host's word from it (%#llx)", (unsigned long long)ga[0], (unsigned long long)ga[1]);
        check(ga[2] && ga[2] == g_ga_seen.str && below_2_39(ga[2], 13) && !strcmp((const char *)(uintptr_t)ga[2], "guest string") && ga[3] == 12 && ga[4] == 'g',
              "t_ga_str: an arena copy of the literal at %#llx; strlen through the GOT %llu, its first byte read by main %#llx", (unsigned long long)ga[2],
              (unsigned long long)ga[3], (unsigned long long)ga[4]);
        check(ga[5] == 1055 && g_ga_seen.local_sum == 55 && below_2_39(g_ga_seen.local, 32),
              "t_ga_local: the scoped array at %#llx (in the arena) passed to main's hf_sum2, which read 11 + 44 (%llu)", (unsigned long long)g_ga_seen.local,
              (unsigned long long)g_ga_seen.local_sum);
        check(ga[6] == 7 && g_ga_seen.outside_sum == 11 && !below_2_39((uint64_t)(uintptr_t)&g_ga_seen, 1),
              "t_ga_outside: host memory on purpose, outside the arena: main read the host stack array (5 + 6 = %llu) and strlen of the host string (%llu)",
              (unsigned long long)g_ga_seen.outside_sum, (unsigned long long)ga[6]);
    }
    return NULL;
}

static void timeout_exit(void) {
    fprintf(stderr, "[RED] t_aot_hostfn: the run did not end within 120 s\n");
    fflush(NULL);
    _exit(DM_AOT_STUCK);
}

static int child(const char *dir) {
    g_dir = dir;
    g_flag.dbg = 1;   // hle_svc.c prints svcOutputDebugString as "[guest] " lines
    crash_install();
    dm_aot_arena_init();
    plat_after_ms(120000, timeout_exit);
    pthread_attr_t attr; pthread_t th;
    pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, guest_main, NULL)) { fprintf(stderr, "t_aot_hostfn: pthread_create failed\n"); return DM_AOT_USAGE; }
    pthread_join(th, NULL);
    dm_aot_stats_t s;
    dm_aot_stats(&s);
    const unsigned long long warm = g_t_hf_marks == 2 ? (unsigned long long)(g_t_hf_mark[1].misses - g_t_hf_mark[0].misses) : ~0ull;
    printf("t_aot_hostfn: run status %d, %d checks, %d failed; %llu SVCs, %llu host calls, %llu dispatcher misses (%llu after the cross-pack loop's warm-up), "
           "%llu cross-pack calls bound in inline caches, %llu cross-pack hand-overs, %llu JIT addresses, %llu threads\n", g_status, g_checks, g_fails,
           (unsigned long long)s.svcs, (unsigned long long)s.host_calls, (unsigned long long)s.misses, warm, (unsigned long long)s.cross_pack,
           (unsigned long long)s.cross_pack_handovers, (unsigned long long)s.jit_addresses, (unsigned long long)s.threads);
    fflush(NULL);
    _exit(g_fails ? 1 : g_status);
}

// ---------- The parent ----------
typedef struct { int status; char *out; size_t n; } result;
static void run_child(const char *self, const char *dir, result *r) {
    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --run \"%s\" 2>&1\"", self, dir);
#else
    snprintf(cmd, sizeof cmd, "'%s' --run '%s' 2>&1", self, dir);
#endif
    memset(r, 0, sizeof *r);
    FILE *f = popen(cmd, "r");
    if (!f) { r->status = -1; r->out = calloc(1, 1); return; }
    size_t cap = 0; char line[8192];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        if (r->n + len + 1 > cap) { cap = (r->n + len + 1) * 2; r->out = realloc(r->out, cap); if (!r->out) abort(); }
        memcpy(r->out + r->n, line, len + 1); r->n += len;
    }
    int st = pclose(f);
#if defined(_WIN32)
    r->status = st;
#else
    r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
    if (!r->out) r->out = calloc(1, 1);
}

static int checks, fails;
static void verdict(int ok, const char *what) { checks++; fails += !ok; printf("%-5s %s\n", ok ? "ok" : "FAIL", what); }
static const char *red_line(const char *out, const char *needle) {   // the first [RED] line that holds needle, for the table
    static char buf[1600];
    for (const char *r = strstr(out, "[RED]"); r; r = strstr(r + 1, "[RED]")) {
        const int n = (int)strcspn(r, "\n");
        snprintf(buf, sizeof buf, "%.*s", n, r);
        if (strstr(buf, needle)) return buf;
    }
    return "(no such [RED] line)";
}
static int count_of(const char *s, const char *needle) { int n = 0; for (const char *p = strstr(s, needle); p; p = strstr(p + 1, needle)) n++; return n; }
// The arena guard's switch for the next child (DM_AOT_ARENA_CHECK, aot.h): "0", "1", or NULL to remove it
static void guard_env(const char *v) {
#if defined(_WIN32)
    _putenv_s("DM_AOT_ARENA_CHECK", v ? v : "");
#else
    if (v) setenv("DM_AOT_ARENA_CHECK", v, 1); else unsetenv("DM_AOT_ARENA_CHECK");
#endif
}
// A copy of the folder without sub's pack, in t_aot_hostfn.tmp/nopack
static int copy_without_pack(const char *dir, char *out_dir, size_t n) {
    static const char *const files[] = {"main.image.bin", "main.aot.tsv", "main" DM_AOT_PACK_SUFFIX, "sub.nro", "nro.tsv"};
    mkdir("t_aot_hostfn.tmp", 0755);
    snprintf(out_dir, n, "t_aot_hostfn.tmp/nopack");
    mkdir(out_dir, 0755);
    char src[1200], dst[1200];
    snprintf(dst, sizeof dst, "%s/sub" DM_AOT_PACK_SUFFIX, out_dir); remove(dst);
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        size_t len = 0;
        snprintf(src, sizeof src, "%s/%s", dir, files[i]);
        uint8_t *b = slurp(src, &len);
        if (!b) return 1;
        snprintf(dst, sizeof dst, "%s/%s", out_dir, files[i]);
        FILE *f = fopen(dst, "wb");
        int bad = !f || fwrite(b, 1, len, f) != len;
        if (f && fclose(f)) bad = 1;
        free(b);
        if (bad) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--run")) return child(argv[2]);
    char dir[1024] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
        else { fprintf(stderr, "usage: t_aot_hostfn [--module <dir>]\n"); return 2; }
    }
    if (!dir[0]) {
        const char *packs = getenv("NXAOT_PACKS");
        if (!packs || !*packs) packs = T_AOT_PACKS;
        if (!*packs) { puts("note: no packs (NXAOT_PACKS unset; dm/tools/aot/tests/build_packs.sh makes them, with the host-function module's, in the nxnative-aot image): skipped"); return 0; }
        snprintf(dir, sizeof dir, "%s/hostfn", packs);
    }
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/sub" DM_AOT_PACK_SUFFIX, dir);
    FILE *pf = fopen(probe, "rb");
    if (!pf) { printf("FAIL: no %s (dm/tools/aot/tests/build_packs.sh writes it)\n", probe); return 1; }
    fclose(pf);
    printf("t_aot_hostfn: module %s\n", dir);
    result r;
    // 1. The run (the arena guard off whatever the environment says: run 1b turns it on)
    guard_env("0");
    run_child(argv[0], dir, &r);
    fputs(r.out, stdout);
    int cks = 0, cfail = -1, cst = -1;
    const char *sum = strstr(r.out, "t_aot_hostfn: run status ");
    if (sum) sscanf(sum, "t_aot_hostfn: run status %d, %d checks, %d failed", &cst, &cks, &cfail);
    verdict(r.status == 0 && cst == 0 && cks >= 25 && cfail == 0, "the run: every check of the child passed and the module ended with ExitProcess");
    verdict(strstr(r.out, "[guest] hostfn module: done") != NULL, "the module's OutputDebugString line");
    verdict(strstr(r.out, "[RED] aot: the host hands the guest the host function") != NULL && strstr(r.out, "UNIMPL host function") != NULL,
            "the host function with no thunk: a [RED] line when handed over, an UNIMPL line when called");
    verdict(strstr(r.out, "NRO sub mapped from the game's image") != NULL && strstr(r.out, "ro::LoadModule sub") != NULL && strstr(r.out, "NRO sub unloaded") != NULL,
            "the NRO: mapped from the game's image, loaded and unloaded through hle_ro.c");
    verdict(strstr(r.out, "UNIMPL sub_value") != NULL, "sub_value after the unload: its stub (an UNIMPL line)");
    verdict(strstr(r.out, "t_aot_hostfn: the host-created guest thread") != NULL, "the host-created guest thread's line");
    verdict(strstr(r.out, "[RED] aot: guest code (main+") != NULL && strstr(r.out, "called from a host thread that is no guest thread") != NULL,
            "the host thread with no guest thread: one [RED] line naming the guest function and what it lacks");
    verdict(!strstr(r.out, "arena check"), "the arena guard off: no arena check line");
    free(r.out);
    // 1b. The same run with the arena guard on (DM_AOT_ARENA_CHECK=1; aot.h): every pointer the host and the module exchange lies in the arena but the three t_ga_outside hands over
    // on purpose, each flagged once: its host stack array passed to hf_sum2 (the guest-call side, DM_GUEST_CALL), its host string returned (its thunk's result), and that
    // string passed to strlen (the passthrough thunk's argument)
    guard_env("1");
    run_child(argv[0], dir, &r);
    const int arena_lines = count_of(r.out, "[RED] aot: arena check: ");
    printf("      %d arena check lines:\n", arena_lines);
    for (const char *l = strstr(r.out, "[RED] aot: arena check: "); l; l = strstr(l + 1, "[RED] aot: arena check: ")) printf("      %.*s\n", (int)strcspn(l, "\n"), l);
    sum = strstr(r.out, "t_aot_hostfn: run status ");
    cks = 0; cfail = -1; cst = -1;
    if (sum) sscanf(sum, "t_aot_hostfn: run status %d, %d checks, %d failed", &cst, &cks, &cfail);
    verdict(r.status == 0 && cst == 0 && cfail == 0, "the run with DM_AOT_ARENA_CHECK=1: every check of the child passed");
    verdict(arena_lines == 3 && strstr(r.out, "[RED] aot: arena check: guest call argument 1 at ") && strstr(r.out, "t_aot_hostfn_entries.c:") &&
            strstr(r.out, "[RED] aot: arena check: t_ga_outside returns the guest an address outside the arena") &&
            strstr(r.out, "[RED] aot: arena check: the guest passes libc_strlen an address outside the arena"),
            "the guard: exactly the three host addresses handed over on purpose, each once (a guest call's argument, a thunk's result, a thunk's argument)");
    free(r.out);
    guard_env(NULL);
    // 2. The NRO's pack missing
    char tdir[1100];
    if (copy_without_pack(dir, tdir, sizeof tdir)) verdict(0, "a copy of the folder without sub's pack (t_aot_hostfn.tmp/nopack)");
    else {
        run_child(argv[0], tdir, &r);
        printf("      %s\n", red_line(r.out, "ro::LoadModule"));
        verdict(r.status != 0 && strstr(r.out, "[RED] ro::LoadModule: ") && strstr(r.out, "sub" DM_AOT_PACK_SUFFIX) && strstr(r.out, "nxaot build"),
                "sub's pack missing: ro::LoadModule fails with a [RED] line naming the pack and how to make it, and the run stops");
        free(r.out);
    }
    printf("t_aot_hostfn: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
