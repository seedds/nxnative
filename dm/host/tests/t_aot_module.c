// A game module through the x86-64 engine's game-module path (docs/design/aot-recompiler.md 1.1, 1.2, 2.4, 9.4; loader.c, "The game-module path"), with no game files:
// the synthetic module of dm/tools/tests/aot_synth.py (an NSO named main of hand-assembled AArch64 code), its behaviour image and manifest as patch_nso.py --aot writes
// them (test_aot_manifest.py checks, where capstone is installed, that the rewriter writes exactly these bytes), packed by nxaot (dm/tools/aot/tests/build_packs.sh, into
// <packs>/module). Each run is a child process (this program with --run), as a game is a process:
//   the run: load_modules maps the image its manifest names (size, layout and SHA-256 checked) one to one in the arena, keeps its build ID and frame step state page,
//     registers its observation gate and spin wait in the gate table, relocates it and binds its two imports into its GOT (t_aot_mod_combine, a host function of
//     t_aot_module_entries.c, at its slot of the host-function page; t_aot_mod_missing, which nothing implements, at a stub slot), loads its pack and checks the gate
//     sites the pack's code calls (its descriptor, ABI 3) against the manifest's rows. Checked: the GOT, the relocated function pointer, the image's permissions (text
//     readable and never executable, data writable), the build ID, the state page; the two gates through dm_aot_gate (the test's observer gets the block's registers
//     and the row's parameter; the spin wait's back-off returns). Then dm_aot_run from the module's entry: a call through the GOT reaches the host function through the
//     dispatcher and its thunk, the stub answers 0 with an UNIMPL line, SetHeapSize reaches the stub backend's be_guest_map with the heap's range, the module runs both
//     x18 jump tables, the exclusive window, the spin wait, the observed function, the cut function, the frame step's function through its stub in the aux region and a
//     relocated pointer, prints through OutputDebugString and ends with ExitProcess; results[] must be aot_synth.py's EXPECTED. The pack calls the gates itself (design
//     1.1, nxaot_rt_gate): the observer runs before the observed instruction with x0, x19 and x30 as the guest has them there and the row's parameter, and the x0 it
//     writes is what the instruction then reads (results[9] = 1042, not 42); the spin wait's flag is 0 for the run, so the loop spins, its back-edge reaching the host's
//     back-off until a host thread sets the flag after three passes; the frame step's stub ran once (state page +0x30)
//   a fault: the same module with synth_faultptr set to an address no page backs (the arena's first page): its fault probe's load faults, and the host's report (the
//     line table, design 3.5) must name that guest instruction exactly
//   a refusal: an image that is not its manifest's (one byte of data changed): the load stops with a [RED] line naming both SHA-256s
//   a refusal: a manifest and image that agree, with the pack of the old image: dm_aot_pack_load refuses the pack with both SHA-256s
//   with the JIT fallback (NXHOST_JIT), the run and the fault again with DM_AOT_JIT_ALL=1: every instruction in the JIT, no compiled code entered (0 dispatcher
//     misses). The same checks hold: the JIT stops at the pack's gate sites (design 11) and the host serves them, the observer before its instruction with the
//     same registers and its x0 taken back, the spin wait's back-off on the taken back-edge only; the frame step's stub runs from the aux region, one of the JIT's
//     code ranges; the fault probe's load is named exactly (the JIT's memory abort, jit.h). Each run also times guest calls of three leaf functions from the host
//     (no gate, an untaken spin back-edge, an observation gate) and the JIT's stop lookup per translated instruction, for design 11's figures
// What this does not show: a real NSO or a real game (no game files here), a real observer's or the back-off's effect on a game
//   t_aot_module [--module <dir>]
//     the module's folder: --module, else <packs>/module with the packs of NXAOT_PACKS (the environment variable, else the CMake option); with none, a note and exit 0
//   t_aot_module --run <dir>     one run (the child); --fault <dir>: the fault run
#include "../aot.h"
#include "../aot_hostfn.h"
#if NXHOST_JIT
#include "rt/jit.h"   // dm_jit_get_counts: the JIT-forced run's gate stops, steps and memory aborts
#endif
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
uint64_t t_aot_mod_combine(uint64_t a, uint64_t b);   // t_aot_module_entries.c
extern _Atomic uint64_t g_t_aot_mod_calls;
extern _Atomic uint64_t g_t_guest_maps, g_t_guest_unmaps;   // t_aot_module_stub.c
extern uint64_t g_t_guest_map_a, g_t_guest_map_n;
extern uint8_t *dm_fps_state;          // loader.c

// What the module stores in results[i] (dm/tools/tests/aot_synth.py EXPECTED; results[3] is the heap's base, checked against be_guest_map). The observed function f_obs
// runs twice, called directly with 41 (results[9]) and through the relocated pointer with 1 (results[11]): the observer adds 1000 to the x0 it sees, so 1042 and 1002,
// where the module alone gives 42 and 2; results[12]: the frame step's function, through its stub
static const uint64_t EXPECTED[13] = {7003, 0, 0, 0, 0x1234, 30, 400, 2, 9, 1042, 2, 1002, 5};
static const char BUILD_ID[] = "aa07aa07aa07aa07aa07aa07aa07aa07";
enum { OBS_NUMBER = 23, OBS_PARAMETER = 5 };   // the module's gate: OBS_REGION_GOT with parameter 5
enum { FLAG_VALUE = 9, SPIN_PASSES = 3 };      // aot_synth.py FLAG_VALUE; the spin gates the host waits for before it sets the flag
#define FAULT_WORD 0xF9400129u                 // aot_synth.py FAULT_WORD: ldr x9, [x9], the fault probe's load

// ---------- The child: one run ----------
static int g_checks, g_fails, g_status = DM_AOT_USAGE, g_fault_run, g_jit_forced;   // g_jit_forced: DM_AOT_JIT_ALL is set (the parent's runs 5 and 6)
static const char *g_dir;
static void check(int ok, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    g_checks++; g_fails += !ok;
    printf("check %s ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap); putchar('\n');
    va_end(ap);
}

// The observer of the module's gate (OBS_REGION_GOT's number, the test's): before the run (dm_aot_gate called by the test) it records what it got; during the run (the pack's
// own call, design 1.1) also the registers the observed instruction will read, and it changes x0, which the code must reload after the gate
static int g_obs_hits, g_obs_param, g_obs_in_run, g_obs_run_hits;
static uint64_t *g_obs_regs, g_obs_x5, g_obs_run_x0[2], g_obs_run_x19[2], g_obs_run_x30[2];
static void observer(uint64_t *regs, int param) {
    g_obs_hits++; g_obs_regs = regs; g_obs_param = param; g_obs_x5 = regs[5];
    if (g_obs_in_run) {
        const int k = g_obs_run_hits++ & 1;
        g_obs_run_x0[k] = regs[0]; g_obs_run_x19[k] = regs[19]; g_obs_run_x30[k] = regs[30];
        regs[0] += 1000;
    }
}

// The spin wait's other side: the flag set once the pack's back-edge has reached the host's back-off SPIN_PASSES times (or after 5 s, which the checks catch)
static volatile uint32_t *g_spin_flag;
static uint64_t g_spins_at_release;
static int g_released_by_gates;
static void *spin_release(void *arg) {
    (void)arg;
    dm_aot_stats_t s;
    for (int i = 0; i < 5000; i++) {
        dm_aot_stats(&s);
        if (s.spin_gates >= SPIN_PASSES) { g_released_by_gates = 1; break; }
        plat_sleep_ns(1000000);
    }
    dm_aot_stats(&s);
    g_spins_at_release = s.spin_gates;
    __atomic_store_n(g_spin_flag, (uint32_t)FLAG_VALUE, __ATOMIC_RELEASE);
    return NULL;
}

// The guest instructions the checks name, found in the module's text: the return address of the call to the observed function, the fault probe's load
static uint64_t text_find_call(const module_t *m, const uint8_t *target) {
    for (uint64_t a = 0; a + 4 <= m->text_end; a += 4) {
        uint32_t w; memcpy(&w, m->base + a, 4);
        if ((w & 0xFC000000u) != 0x94000000u) continue;
        const int64_t imm = (int64_t)((uint64_t)(w & 0x3FFFFFFu) << 38) >> 36;   // imm26 * 4, sign-extended
        if (m->base + a + imm == target) return a + 4;
    }
    return 0;
}
static uint64_t text_find_word(const module_t *m, uint32_t word) {
    for (uint64_t a = 0; a + 4 <= m->text_end; a += 4) { uint32_t w; memcpy(&w, m->base + a, 4); if (w == word) return a; }
    return 0;
}

// A guest call of fn(x0) from the host, n times after a warm-up: nanoseconds per call
static double time_calls(dm_guest_state *st, uint64_t fn, uint64_t x0, int n) {
    for (int i = 0; i < 100; i++) dm_guest_call(st, fn, 1, x0);
    const uint64_t t0 = plat_now_ns();
    for (int i = 0; i < n; i++) dm_guest_call(st, fn, 1, x0);
    return (double)(plat_now_ns() - t0) / n;
}

#if NXHOST_JIT
static dm_jit_counts jit_counts(void) {
    dm_jit_counts c;
    memset(&c, 0, sizeof c);
    const dm_aot_thread *t = dm_aot_self();
    if (t && t->jit) c = dm_jit_get_counts((const dm_jit *)t->jit);
    return c;
}
#endif

static void *guest_main(void *arg) {
    (void)arg;
    guest_thread_init();                             // thread.c: TLS block and thread-local region in the arena
    const uint32_t hmain = svc_thread_main_init();   // hle_thread.c: this thread is the main guest thread
    const char *names[] = {"main"};
    load_modules(g_dir, names, 1);                   // dies with a [RED] line when the module or its pack is refused
    module_t *m = &g_mod[0];
    const uint64_t base = (uint64_t)(uintptr_t)m->base;
    uint64_t *got = module_sym(m, "synth_got"), *fnptr = module_sym(m, "synth_fnptr"), *results = module_sym(m, "results");
    uint8_t *f_obs = module_sym(m, "synth_observed"), *f_spin = module_sym(m, "synth_spin");
    check(got && fnptr && results && f_obs && f_spin, "the module's symbols (synth_got, synth_fnptr, results, synth_observed, synth_spin)");
    if (!got || !fnptr || !results || !f_obs || !f_spin) return NULL;
    // The imports in the GOT (design 2.4)
    const char *slot = dm_aot_hostfn_name(got[0]);
    check(got[0] == dm_guest_fn((const void *)t_aot_mod_combine) && slot && !strcmp(slot, "t_aot_mod_combine"),
          "GOT[0], the import t_aot_mod_combine: %#llx, the host function's slot in the host-function page (%s)", (unsigned long long)got[0], slot ? slot : "none");
    check(dm_aot_stub_index(got[1]) >= 0 && got[1] == dm_aot_stub_addr((unsigned)dm_aot_stub_index(got[1])),
          "GOT[1], the import t_aot_mod_missing: %#llx, stub slot %ld", (unsigned long long)got[1], dm_aot_stub_index(got[1]));
    check(*fnptr == (uint64_t)(uintptr_t)f_obs, "the relocated pointer (R_AARCH64_RELATIVE): %#llx = synth_observed", (unsigned long long)*fnptr);
    // The image one to one in the arena, as loader_add_image maps one
    uint64_t qb = 0, qs = 0; int prot = 0, prot_data = 0;
    const int q1 = plat_vm_query(base, &qb, &qs, &prot), q2 = plat_vm_query(base + m->data_off, &qb, &qs, &prot_data);
    check(dm_aot_in_arena(base, m->size) && !q1 && prot == PLAT_PROT_READ && !q2 && (prot_data & PLAT_PROT_WRITE) && !(prot_data & PLAT_PROT_EXEC),
          "the image at %#llx (+%#zx) in the arena: text readable only (%d), data writable and not executable (%d)", (unsigned long long)base, m->size, prot, prot_data);
    check(!strcmp(m->build_id, BUILD_ID) && !strcmp(dm_main_build(), BUILD_ID), "the build ID from the manifest: %s", m->build_id);
    check(m->fps_state_off && dm_fps_state == m->base + m->fps_state_off && m->fps_state_off + 0x40 <= m->size,
          "the frame step's state page at +%#llx (the aux row)", (unsigned long long)m->fps_state_off);
    // The gates (the obs and spin rows), as the lifter will call them (design 1.1)
    dm_guest_state *st = dm_aot_thread_begin();
    st->x[5] = 0x5a5a5a5aull;
    dm_aot_gate(st, (uint64_t)(uintptr_t)f_obs);
    check(g_obs_hits == 1 && g_obs_regs == st->x && g_obs_param == OBS_PARAMETER && g_obs_x5 == 0x5a5a5a5aull,
          "the observation gate at synth_observed: observer %d with the block's registers and parameter %d", OBS_NUMBER, g_obs_param);
    dm_aot_gate(st, (uint64_t)(uintptr_t)f_spin + 4);
    check(1, "the spin wait's back-edge at synth_spin+4: the back-off ran and returned");
    // The run
    const dm_aot_pack *p = NULL;
    dm_aot_find(base, &p);
    check(p != NULL, "the module's pack is loaded");
    if (!p) return NULL;
    const nxaot_module_desc *d = dm_aot_pack_desc(p);
    const nxaot_gate_site *gobs = nxaot_gate_at(d, (uint64_t)(f_obs - m->base)), *gspin = nxaot_gate_at(d, (uint64_t)(f_spin + 4 - m->base));
    check(d->gate_count == 2 && gobs && gobs->kind == NXAOT_GATE_OBS && gobs->arg == (OBS_NUMBER | OBS_PARAMETER << 8) && gspin && gspin->kind == NXAOT_GATE_SPIN &&
          gspin->arg == (uint64_t)(f_spin - m->base), "the pack's gate sites (descriptor, ABI 3): the observation gate at synth_observed (number %d, parameter %d) and the spin "
          "wait's back-edge at synth_spin+4 (head synth_spin)", OBS_NUMBER, OBS_PARAMETER);
    uint32_t *flag = module_sym(m, "synth_flag");
    uint64_t *faultptr = module_sym(m, "synth_faultptr");
    const uint64_t obs_ret = text_find_call(m, f_obs), fault_at = text_find_word(m, FAULT_WORD);
    check(flag && faultptr && obs_ret && fault_at, "the module's probes: synth_flag, synth_faultptr, the call of synth_observed (returns to main+%#llx), the fault probe's "
          "load (main+%#llx)", (unsigned long long)obs_ret, (unsigned long long)fault_at);
    if (!flag || !faultptr || !obs_ret || !fault_at) return NULL;
    if (g_fault_run) {
        // The fault run: the probe's load reads the arena's first page, which nothing backs; the host's report names the instruction through the line table
        *faultptr = DM_AOT_ARENA_BASE + 8;
        printf("t_aot_module: the fault probe at main+%#llx\n", (unsigned long long)fault_at);
        fflush(stdout);
        g_status = dm_aot_run(base + d->entry, 0, hmain, NULL);
        check(g_status == DM_AOT_FAULT, "the fault probe's load faulted and stopped the run (status %d)", g_status);
#if NXHOST_JIT
        if (g_jit_forced) {
            const dm_jit_counts c = jit_counts();
            check(c.aborts >= 1, "JIT-forced: the probe's load reached the JIT's memory callback and halted at its exact instruction (%llu memory aborts)", (unsigned long long)c.aborts);
        }
#endif
        return NULL;
    }
    // Timing (design 11's figures): guest calls from the host of synth_cut (mov; ret: no gate, the cost of a call), synth_spin with its flag set (ldr; its back-edge,
    // a gate site, not taken; mov; ret) and synth_observed (an observation gate at its add; the observer only records outside the run); the JIT's stop lookup over
    // the module's code (dm_aot_jit_stop_at, what its code reader asks per translated instruction)
    {
        uint8_t *f_cut = module_sym(m, "synth_cut");
        uint32_t *spare = (uint32_t *)&results[15];   // a word of results no check reads: the timing's own flag, set
        *spare = FLAG_VALUE;
        const int n = 20000;
        const double t_cut = f_cut ? time_calls(st, (uint64_t)(uintptr_t)f_cut, 0, n) : 0;
        const double t_spin = time_calls(st, (uint64_t)(uintptr_t)f_spin, (uint64_t)(uintptr_t)spare, n);
        const double t_obs = time_calls(st, (uint64_t)(uintptr_t)f_obs, 41, n);
        *spare = 0;
        double t_look = 0;
        const int reps = 200;
        uint64_t sink = 0, looks = 0;
        const uint64_t l0 = plat_now_ns();
        for (int r = 0; r < reps; r++)
            for (uint64_t a = 0; a < 0x1000; a += 4, looks++) sink += (uint64_t)dm_aot_jit_stop_at(base + a);
        t_look = (double)(plat_now_ns() - l0) / (double)looks;
        printf("t_aot_module: timing (%s): a guest call from the host of synth_cut %.0f ns, of synth_spin through its untaken back-edge gate %.0f ns (+%.0f), "
               "of synth_observed through its observation gate %.0f ns (+%.0f); the JIT's stop lookup %.1f ns per translated instruction (%llu)\n",
               g_jit_forced ? "JIT-forced" : "compiled", t_cut, t_spin, t_spin - t_cut, t_obs, t_obs - t_cut, t_look, (unsigned long long)(sink & 0xF));
        check(f_cut && t_cut > 0 && t_spin > 0 && t_obs > 0, "the timing calls returned");
    }
    dm_aot_stats_t s0;
    dm_aot_stats(&s0);   // the counts before the run: the timing's observation gate calls are not the run's
    g_spin_flag = flag;
    *flag = 0;   // the spin wait spins until spin_release sets it
    pthread_t releaser;
    if (pthread_create(&releaser, NULL, spin_release, NULL)) { check(0, "the spin wait's releasing thread"); return NULL; }
    g_obs_in_run = 1;
    g_status = dm_aot_run(base + d->entry, 0, hmain, NULL);
    g_obs_in_run = 0;
    pthread_join(releaser, NULL);
    check(g_status == DM_AOT_EXITED, "the run ended with ExitProcess (status %d)", g_status);
    for (int i = 0; i < 13; i++) {
        if (i == 3) continue;
        check(results[i] == EXPECTED[i], "results[%d] = %#llx (expected %#llx)", i, (unsigned long long)results[i], (unsigned long long)EXPECTED[i]);
    }
    // Design 1.1, the pack's own gate calls
    check(g_obs_run_hits == 2 && g_obs_run_x0[0] == 41 && g_obs_run_x19[0] == (uint64_t)(uintptr_t)results && g_obs_run_x30[0] == base + obs_ret &&
          g_obs_run_x0[1] == 1 && g_obs_run_x19[1] == (uint64_t)(uintptr_t)results && g_obs_param == OBS_PARAMETER,
          "the pack's observation gate, at both calls of synth_observed, before its add: x0 %llu and %llu (the guest's 41 and 1), x19 = &results (%#llx), x30 at the "
          "direct call its return address main+%#llx (%#llx), parameter %d; the observer's x0 + 1000 is what the add read (results[9] %llu, results[11] %llu)",
          (unsigned long long)g_obs_run_x0[0], (unsigned long long)g_obs_run_x0[1], (unsigned long long)g_obs_run_x19[0], (unsigned long long)obs_ret,
          (unsigned long long)g_obs_run_x30[0], g_obs_param, (unsigned long long)results[9], (unsigned long long)results[11]);
    dm_aot_stats_t s;
    dm_aot_stats(&s);
    check(g_released_by_gates && g_spins_at_release >= SPIN_PASSES && s.spin_gates >= SPIN_PASSES && s.obs_gates - s0.obs_gates == 2,
          "the spin wait's back-edge reached the host's back-off %llu times before the flag was set (%llu in all; released by the gates: %d); 2 observation gate calls (%llu)",
          (unsigned long long)g_spins_at_release, (unsigned long long)s.spin_gates, g_released_by_gates, (unsigned long long)(s.obs_gates - s0.obs_gates));
#if NXHOST_JIT
    if (g_jit_forced) {
        // Every instruction ran in the JIT: no dispatcher miss (only compiled code misses), and its gate stops served the gates checked above
        const dm_jit_counts c = jit_counts();
        check(s.misses == 0 && s.jit_runs > 0 && c.gates >= 2 + SPIN_PASSES,
              "JIT-forced: no compiled code ran (%llu dispatcher misses); %llu JIT runs at %llu addresses, %llu gate stops, %llu single steps, %llu memory aborts",
              (unsigned long long)s.misses, (unsigned long long)s.jit_runs, (unsigned long long)s.jit_addresses, (unsigned long long)c.gates,
              (unsigned long long)c.steps, (unsigned long long)c.aborts);
    }
#endif
    uint32_t stepped = 0;
    if (dm_fps_state) memcpy(&stepped, dm_fps_state + 0x30, 4);
    check(results[12] == 5 && stepped == 1, "the frame step's function through its stub in the aux region: 5, and the stub's pass counted at the state page's +0x30 (%u)", stepped);
    check(results[3] && results[3] == g_t_guest_map_a && g_t_guest_map_n == 0x200000 && g_t_guest_maps >= 1,
          "SetHeapSize (2 MiB at %#llx) reached be_guest_map(%#llx, %#llx): %llu calls, %llu be_guest_unmap", (unsigned long long)results[3],
          (unsigned long long)g_t_guest_map_a, (unsigned long long)g_t_guest_map_n, (unsigned long long)g_t_guest_maps, (unsigned long long)g_t_guest_unmaps);
    check(g_t_aot_mod_calls == 1, "the host function ran once (%llu)", (unsigned long long)g_t_aot_mod_calls);
    return NULL;
}

static void timeout_exit(void) {
    fprintf(stderr, "[RED] t_aot_module: the run did not end within 120 s\n");
    fflush(NULL);
    _exit(DM_AOT_STUCK);
}

static int child(const char *dir) {
    g_dir = dir;
    const char *forced = getenv("DM_AOT_JIT_ALL");
    g_jit_forced = forced && *forced && strcmp(forced, "0") != 0;
    g_flag.dbg = 1;   // hle_svc.c prints svcOutputDebugString as "[guest] " lines
    crash_install();
    dm_aot_arena_init();
    plat_after_ms(120000, timeout_exit);
    gate_obs_set(OBS_NUMBER, observer);   // after hle_svc.c's own (a constructor): the test's observer for the module's gate
    pthread_attr_t attr; pthread_t th;
    pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, guest_main, NULL)) { fprintf(stderr, "t_aot_module: pthread_create failed\n"); return DM_AOT_USAGE; }
    pthread_join(th, NULL);
    dm_aot_stats_t s;
    dm_aot_stats(&s);
    printf("t_aot_module: run status %d, %d checks, %d failed; %llu SVCs, %llu host calls, %llu dispatcher misses, %llu JIT addresses, %llu observation gate calls, "
           "%llu spin gate calls\n", g_status, g_checks, g_fails, (unsigned long long)s.svcs, (unsigned long long)s.host_calls, (unsigned long long)s.misses,
           (unsigned long long)s.jit_addresses, (unsigned long long)s.obs_gates, (unsigned long long)s.spin_gates);
    fflush(NULL);
    _exit(g_fails ? 1 : g_status);
}

// ---------- The parent ----------
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len > 0 ? (size_t)len + 1 : 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(p); return NULL; }
    fclose(f); p[len] = 0; *n = (size_t)len;
    return p;
}
static int spit(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    int bad = fwrite(p, 1, n, f) != n;
    return fclose(f) || bad;
}
static void hex(const uint8_t *b, char *out) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", b[i]); }

typedef struct { int status; char *out; size_t n; } result;
static void run_child_mode(const char *self, const char *mode, const char *dir, result *r) {
    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" %s \"%s\" 2>&1\"", self, mode, dir);
#else
    snprintf(cmd, sizeof cmd, "'%s' %s '%s' 2>&1", self, mode, dir);
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
static void run_child(const char *self, const char *dir, result *r) { run_child_mode(self, "--run", dir, r); }
static void set_env(const char *k, const char *v) {   // for the children popen starts
#if defined(_WIN32)
    char b[256]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static int checks, fails;
static void verdict(int ok, const char *what) { checks++; fails += !ok; printf("%-5s %s\n", ok ? "ok" : "FAIL", what); }
static const char *red_line(const char *out, const char *needle) {   // the first [RED] line that holds needle, for the table
    static char buf[1200];
    for (const char *r = strstr(out, "[RED]"); r; r = strstr(r + 1, "[RED]")) {
        const int n = (int)strcspn(r, "\n");
        snprintf(buf, sizeof buf, "%.*s", n, r);
        if (strstr(buf, needle)) return buf;
    }
    return "(no such [RED] line)";
}

// A copy of the module's three files in tmp/<sub>, the image changed at one byte of data; with fix_manifest the manifest's SHA-256 follows the image
static int tampered(const char *dir, const char *sub, int fix_manifest, char *out_dir, size_t n, char old_hash[65], char new_hash[65]) {
    char src[1024], dst[1024];
    mkdir("t_aot_module.tmp", 0755);
    snprintf(out_dir, n, "t_aot_module.tmp/%s", sub);
    mkdir(out_dir, 0755);
    size_t li = 0, lm = 0, lp = 0;
    snprintf(src, sizeof src, "%s/main.image.bin", dir); uint8_t *img = slurp(src, &li);
    snprintf(src, sizeof src, "%s/main.aot.tsv", dir); char *man = (char *)slurp(src, &lm);
    snprintf(src, sizeof src, "%s/main" DM_AOT_PACK_SUFFIX, dir); uint8_t *pack = slurp(src, &lp);
    if (!img || !man || !pack || li < 0x3100) { free(img); free(man); free(pack); return 1; }
    uint8_t h[32];
    plat_sha256(img, li, h); hex(h, old_hash);
    img[0x3100] ^= 0x5a;   // a byte of data no instruction reads: the image is another one
    plat_sha256(img, li, h); hex(h, new_hash);
    char *at = strstr(man, old_hash);
    if (!at) { free(img); free(man); free(pack); return 1; }
    if (fix_manifest) memcpy(at, new_hash, 64);
    int bad = 0;
    snprintf(dst, sizeof dst, "%s/main.image.bin", out_dir); bad |= spit(dst, img, li);
    snprintf(dst, sizeof dst, "%s/main.aot.tsv", out_dir); bad |= spit(dst, man, lm);
    snprintf(dst, sizeof dst, "%s/main" DM_AOT_PACK_SUFFIX, out_dir); bad |= spit(dst, pack, lp);
    free(img); free(man); free(pack);
    return bad;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--run")) return child(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--fault")) { g_fault_run = 1; return child(argv[2]); }
    char dir[1024] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
        else { fprintf(stderr, "usage: t_aot_module [--module <dir>]\n"); return 2; }
    }
    if (!dir[0]) {
        const char *packs = getenv("NXAOT_PACKS");
        if (!packs || !*packs) packs = T_AOT_PACKS;
        if (!*packs) { puts("note: no packs (NXAOT_PACKS unset; dm/tools/aot/tests/build_packs.sh makes them, with the module's, in the nxnative-aot image): skipped"); return 0; }
        snprintf(dir, sizeof dir, "%s/module", packs);
    }
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/main" DM_AOT_PACK_SUFFIX, dir);
    FILE *pf = fopen(probe, "rb");
    if (!pf) { printf("FAIL: no %s (dm/tools/aot/tests/build_packs.sh writes it)\n", probe); return 1; }
    fclose(pf);
    printf("t_aot_module: module %s\n", dir);
    result r;
    // 1. The run
    run_child(argv[0], dir, &r);
    fputs(r.out, stdout);
    int cks = 0, cfail = -1, cst = -1;
    const char *sum = strstr(r.out, "t_aot_module: run status ");
    if (sum) sscanf(sum, "t_aot_module: run status %d, %d checks, %d failed", &cst, &cks, &cfail);
    verdict(r.status == 0 && cst == 0 && cks >= 20 && cfail == 0, "the run: every check of the child passed and the module ended with ExitProcess");
    verdict(strstr(r.out, "[guest] aot module: done") != NULL, "the module's OutputDebugString line");
    verdict(strstr(r.out, "UNIMPL t_aot_mod_missing") != NULL, "the unimplemented import answered by its stub (an UNIMPL line)");
    verdict(strstr(r.out, "aot: main: 2 gates (1 observation gates, 1 spin wait back-edges) called by its pack, 0 not") != NULL && !strstr(r.out, "its pack does not call"),
            "the loader: every gate row of the manifest is a gate site the pack's code calls (design 1.1)");
    free(r.out);
    // 2. The fault probe: the host's fault report names the faulting guest instruction (the line table)
    run_child_mode(argv[0], "--fault", dir, &r);
    unsigned long long fault_at = 0;
    const char *at = strstr(r.out, "t_aot_module: the fault probe at main+");
    if (at) sscanf(at, "t_aot_module: the fault probe at main+%llx", &fault_at);
    char want[96];
    snprintf(want, sizeof want, "guest instruction main+%#llx)", fault_at);
    printf("      %s\n", red_line(r.out, "SIGSEGV"));
    verdict(r.status == DM_AOT_FAULT && fault_at && strstr(r.out, "[RED] aot: SIGSEGV at address 0x1000000008 in guest code (") && strstr(red_line(r.out, "SIGSEGV"), want),
            "a fault in compiled code: the report names the probe's load (the line table: native address -> guest instruction, exactly)");
    free(r.out);
    // 3. An image that is not its manifest's
    char tdir[1100], h_old[65], h_new[65];
    if (tampered(dir, "image", 0, tdir, sizeof tdir, h_old, h_new)) verdict(0, "a copy of the module with another image (t_aot_module.tmp/image)");
    else {
        run_child(argv[0], tdir, &r);
        printf("      %s\n", red_line(r.out, "does not match"));
        verdict(r.status != 0 && strstr(r.out, "[RED] aot:") && strstr(r.out, "does not match") && strstr(r.out, h_old) && strstr(r.out, h_new) && !strstr(r.out, "run status"),
                "an image that is not its manifest's: refused with a [RED] line naming both SHA-256s, nothing run");
        free(r.out);
    }
    // 4. A manifest and image that agree, with the pack of the old image
    if (tampered(dir, "pack", 1, tdir, sizeof tdir, h_old, h_new)) verdict(0, "a copy of the module with another image and its manifest (t_aot_module.tmp/pack)");
    else {
        run_child(argv[0], tdir, &r);
        printf("      %s\n", red_line(r.out, "compiled from another image"));
        verdict(r.status != 0 && strstr(r.out, "[RED] aot:") && strstr(r.out, "compiled from another image") && strstr(r.out, h_old) && strstr(r.out, h_new) &&
                    !strstr(r.out, "run status"),
                "the pack of another image: refused with a [RED] line naming both SHA-256s, nothing run");
        free(r.out);
    }
    // 5. The run and the fault again, every instruction in the JIT fallback (DM_AOT_JIT_ALL=1): the same results, gates, stub and fault report as compiled code's
#if NXHOST_JIT
    set_env("DM_AOT_JIT_ALL", "1");
    run_child(argv[0], dir, &r);
    fputs(r.out, stdout);
    cks = 0; cfail = -1; cst = -1;
    sum = strstr(r.out, "t_aot_module: run status ");
    if (sum) sscanf(sum, "t_aot_module: run status %d, %d checks, %d failed", &cst, &cks, &cfail);
    verdict(r.status == 0 && cst == 0 && cks >= 20 && cfail == 0 && strstr(r.out, "[guest] aot module: done") && strstr(r.out, "UNIMPL t_aot_mod_missing") &&
                strstr(r.out, "DM_AOT_JIT_ALL: every guest instruction runs in the JIT fallback"),
            "the JIT-forced run (DM_AOT_JIT_ALL=1): every check of the child passed, the same results, gates and stub as compiled code's, its OutputDebugString and UNIMPL lines");
    free(r.out);
    run_child_mode(argv[0], "--fault", dir, &r);
    set_env("DM_AOT_JIT_ALL", NULL);
    fault_at = 0;
    at = strstr(r.out, "t_aot_module: the fault probe at main+");
    if (at) sscanf(at, "t_aot_module: the fault probe at main+%llx", &fault_at);
    snprintf(want, sizeof want, "guest instruction main+%#llx)", fault_at);
    printf("      %s\n", red_line(r.out, "SIGSEGV"));
    verdict(r.status == DM_AOT_FAULT && fault_at && strstr(r.out, "[RED] aot: SIGSEGV at address 0x1000000008 in guest code (a memory access of the JIT fallback, ") &&
                strstr(red_line(r.out, "SIGSEGV"), want) && strstr(r.out, "check ok   JIT-forced: the probe's load"),
            "a fault in the JIT fallback (DM_AOT_JIT_ALL=1): the report names the probe's load exactly (the JIT's memory abort)");
    free(r.out);
#else
    puts("note: this build has no JIT fallback (NXHOST_JIT off): the JIT-forced run and fault are skipped");
#endif
    printf("t_aot_module: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
