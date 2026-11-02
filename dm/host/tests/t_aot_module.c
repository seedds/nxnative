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
//   with the JIT fallback, both runs also reach the hidden function (aot_synth.py HIDDEN): a manifest obs row on code no pack holds (the load's [RED] line, "1 not"),
//     called through a pointer the entry computes, so the JIT runs it and serves its gate: the observer sees x0 = 7, x19 = &results and the call's return address,
//     and the 1000 it adds is what the add reads (results[13] = 1009); 3 observation gate calls in the run instead of 2
// What this does not show: a real NSO or a real game (no game files here), a real observer's or the back-off's effect on a game
//   t_aot_module [--module <dir>]
//     the module's folder: --module, else <packs>/module with the packs of NXAOT_PACKS (the environment variable, else the CMake option); with none, a note and exit 0
//   the sampler (Linux; aot_profile.c, DM_AOT_PROFILE): a run of about 0.4 s in synth_hot (aot_synth.py's hot loop) and 0.04 s of guest calls of synth_cut from the host, with
//     DM_AOT_PROFILE set: the profile the process writes at exit, <folder>/main.tsv, names the module, its build ID and its image's SHA-256, every line of it is
//     `<offset> <samples>` as nxaot build --profile reads it, and synth_hot has the most samples (more than half of them at its instructions, the row with the most in
//     its loop, not at its entry: the pack's positions, design 12; all at its entry for a pack without positions); the same run without the switch writes nothing,
//     and the time per pass of synth_hot with and without it is the sampler's overhead on compiled code
//   the JIT fallback's misses (with the JIT fallback; aot.h, "The JIT fallback's misses"): the run with DM_AOT_PROFILE set writes <folder>/main.misses.tsv beside the profile, whose
//     one row is the hidden function's (`<offset> indirect 1`: the dispatcher's miss at an address with no compiled entry), under the profile's header; a second run adds its entry
//     to the row, and a file of another image is replaced (dm/tools/aot/tests/test_misses.py packs the module with such a file: the hidden function becomes compiled code)
//   a recorded session's end (with the JIT fallback): the run with DM_AOT_PROFILE at 1000 Hz, ended by exit() on its guest thread (the one that made the first JIT instance,
//     its sampling timer armed), 20 times: every run exits 0 (dynarmic's first instance replaced that thread's alternate signal stack with its own, which its static
//     destructor frees at exit: aot_fault.c dm_aot_fault_after_jit puts the thread's own back, aot_profile.c dm_aot_profile_write ignores SIGPROF once it wrote)
//   t_aot_module --run <dir>     one run (the child); --fault <dir>: the fault run; --profile <dir>: the sampler's run; --exit <dir>: the run ended by exit()
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
#include <time.h>
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
enum { HIDDEN_PARAMETER = 6, HIDDEN_X0 = 7, HIDDEN_KEY = 0xA5A5 };   // aot_synth.py OBS_HIDDEN's parameter, HIDDEN_X0, HIDDEN_KEY
#define HIDDEN_WORD 0x91000800u                // aot_synth.py HIDDEN_WORD: add x0, x0, #2, the hidden function's observed first word

// ---------- The child: one run ----------
static int g_checks, g_fails, g_status = DM_AOT_USAGE, g_fault_run, g_jit_forced, g_profile_run;   // g_jit_forced: DM_AOT_JIT_ALL is set (the parent's runs 5 and 6)
static int g_exit_run;            // --exit: the run ends with exit() on the guest thread (the parent's step 8)
static pthread_t g_exit_thread;
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
static int g_hidden_hits;
static uint64_t g_hidden_x0, g_hidden_x19, g_hidden_x30;
static void observer(uint64_t *regs, int param) {
    if (param == HIDDEN_PARAMETER) {   // the hidden function's gate, which only the JIT fallback serves
        g_hidden_hits++; g_hidden_x0 = regs[0]; g_hidden_x19 = regs[19]; g_hidden_x30 = regs[30];
        regs[0] += 1000;
        return;
    }
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

// The sampler's run (DM_AOT_PROFILE set by the parent, or not): synth_hot called from the host with 2^22 passes until 0.4 s have gone (its sums checked), then synth_cut
// for 0.04 s; the profile is written at exit (child: exit, not _exit). The time per pass of synth_hot is printed for the overhead
static void profile_run(module_t *m) {
    uint8_t *f_hot = module_sym(m, "synth_hot"), *f_cut = module_sym(m, "synth_cut");
    check(f_hot && f_cut, "the module's synth_hot and synth_cut");
    if (!f_hot || !f_cut) return;
    const char *want = getenv("DM_AOT_PROFILE");
    check(dm_aot_profile_on() == (want && *want), "the sampler is %s (DM_AOT_PROFILE %s)", dm_aot_profile_on() ? "on" : "off", want && *want ? want : "unset");
    dm_guest_state *st = dm_aot_main_begin();
    const uint64_t n = 1u << 22;
    uint64_t calls = 0;
    int sums = 1;
    const uint64_t t0 = plat_now_ns();
    while (plat_now_ns() - t0 < 400000000ull) {
        sums &= dm_guest_call(st, (uint64_t)(uintptr_t)f_hot, 1, n) == n * (n + 1) / 2;
        calls++;
    }
    const double per_pass = (double)(plat_now_ns() - t0) / ((double)calls * (double)n);
    uint64_t cuts = 0;
    const uint64_t t1 = plat_now_ns();
    while (plat_now_ns() - t1 < 40000000ull) for (int i = 0; i < 1000; i++, cuts++) dm_guest_call(st, (uint64_t)(uintptr_t)f_cut, 0);
    printf("t_aot_profile: synth_hot at main+%#llx, synth_cut at main+%#llx: %llu calls of synth_hot (%llu passes each, %.4f ns per pass), %llu calls of synth_cut\n",
           (unsigned long long)(f_hot - m->base), (unsigned long long)(f_cut - m->base), (unsigned long long)calls, (unsigned long long)n, per_pass, (unsigned long long)cuts);
    int pk = -1;   // synth_hot's guest code by the pack's function map, for the parent's sums of its rows (the profile names its instructions: design 12)
    const long fn = dm_aot_fn_of_guest((uint64_t)(uintptr_t)f_hot, &pk);
    printf("t_aot_profile: synth_hot's guest code main+%#llx to main+%#llx, %s\n", (unsigned long long)(f_hot - m->base),
           fn >= 0 ? (unsigned long long)dm_aot_pack_fn_end(dm_aot_pack_at(pk), (size_t)fn) : 0ull,
           fn >= 0 && dm_aot_pack_pos(dm_aot_pack_at(pk)) ? "the pack has positions" : "the pack has no positions");
    check(sums && calls > 0, "synth_hot's sums (n (n + 1) / 2 for n = %llu, %llu calls)", (unsigned long long)n, (unsigned long long)calls);
    g_status = DM_AOT_EXITED;
}

static void *guest_main(void *arg) {
    (void)arg;
    guest_thread_init();                             // thread.c: TLS block and thread-local region in the arena
    const uint32_t hmain = svc_thread_main_init();   // hle_thread.c: this thread is the main guest thread
    const char *names[] = {"main"};
    load_modules(g_dir, names, 1);                   // dies with a [RED] line when the module or its pack is refused
    module_t *m = &g_mod[0];
    if (g_profile_run) { profile_run(m); return NULL; }
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
        printf("t_aot_module: timing (%s): a guest call from the host of synth_cut %.0f ns, of synth_spin through its untaken back-edge gate %.0f ns (%+.0f), "
               "of synth_observed through its observation gate %.0f ns (%+.0f); the JIT's stop lookup %.1f ns per translated instruction (%llu)\n",
               g_jit_forced ? "JIT-forced" : "compiled", t_cut, t_spin, t_spin - t_cut, t_obs, t_obs - t_cut, t_look, (unsigned long long)(sink & 0xF));
        check(f_cut && t_cut > 0 && t_spin > 0 && t_obs > 0, "the timing calls returned");
    }
    dm_aot_stats_t s0;
    dm_aot_stats(&s0);   // the counts before the run: the timing's observation gate calls are not the run's
    // The hidden function: its offset ^ HIDDEN_KEY in synth_hidden makes the entry call it, which only a host with the JIT fallback can run (design 11)
    uint32_t *hidden = module_sym(m, "synth_hidden");
    const uint64_t hidden_at = text_find_word(m, HIDDEN_WORD);
    uint64_t hidden_ret = 0;   // the return address of the entry's blr x16 after its mov x0, #7
    for (uint64_t a = 0; a + 8 <= m->text_end && !hidden_ret; a += 4) {
        uint32_t w0, w1; memcpy(&w0, m->base + a, 4); memcpy(&w1, m->base + a + 4, 4);
        if (w0 == 0xD28000E0u && w1 == 0xD63F0200u) hidden_ret = a + 8;
    }
    check(hidden && hidden_at && hidden_ret && !*hidden, "the hidden function at main+%#llx (no symbol, no FDE), called from main+%#llx through synth_hidden (0 in the image)",
          (unsigned long long)hidden_at, (unsigned long long)(hidden_ret - 4));
    int hidden_on = 0;
#if NXHOST_JIT
    if (hidden && hidden_at) { *hidden = (uint32_t)hidden_at ^ HIDDEN_KEY; hidden_on = 1; }
#endif
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
    check(g_released_by_gates && g_spins_at_release >= SPIN_PASSES && s.spin_gates >= SPIN_PASSES && s.obs_gates - s0.obs_gates == 2u + (unsigned)hidden_on,
          "the spin wait's back-edge reached the host's back-off %llu times before the flag was set (%llu in all; released by the gates: %d); %d observation gate calls (%llu)",
          (unsigned long long)g_spins_at_release, (unsigned long long)s.spin_gates, g_released_by_gates, 2 + hidden_on, (unsigned long long)(s.obs_gates - s0.obs_gates));
    if (hidden_on)
        check(g_hidden_hits == 1 && g_hidden_x0 == HIDDEN_X0 && g_hidden_x19 == (uint64_t)(uintptr_t)results && g_hidden_x30 == base + hidden_ret &&
                  results[13] == HIDDEN_X0 + 2 + 1000,
              "the hidden function's gate, which no pack serves, served by the JIT fallback: %d call, x0 %llu, x19 = &results, x30 main+%#llx (expected main+%#llx); "
              "the observer's x0 + 1000 is what its add read (results[13] %llu)", g_hidden_hits, (unsigned long long)g_hidden_x0,
              (unsigned long long)(g_hidden_x30 - base), (unsigned long long)hidden_ret, (unsigned long long)results[13]);
    else
        check(g_hidden_hits == 0 && results[13] == 0, "no JIT fallback: the hidden function is not called (results[13] %llu)", (unsigned long long)results[13]);
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
    if (g_exit_run) {
        // The recorded session's end as a game's (--exit): this guest thread, which made the first JIT instance and whose sampling timer is still armed, ends the
        // process with exit(): the sampler's writer (atexit), the C++ destructors (dynarmic's SigHandler frees the alternate stack it installed), then exit_burn below
        dm_aot_stats_t s;
        dm_aot_stats(&s);
        printf("t_aot_module: exit run status %d, %d checks, %d failed; %llu JIT addresses, %llu JIT runs\n", g_status, g_checks, g_fails, (unsigned long long)s.jit_addresses,
               (unsigned long long)s.jit_runs);
        fflush(NULL);
        g_exit_thread = pthread_self();
        exit(g_fails ? 1 : g_status);
    }
    return NULL;
}

#if !defined(_WIN32)
// --exit: after the C++ destructors (a destructor of the executable runs from _dl_fini, the first exit handler registered and so the last to run), some CPU time on the
// exiting thread, so that its sampling timer fires once the process's own exit code has run: the signal must land on an alternate stack the host owns (aot_fault.c
// dm_aot_fault_after_jit), not on dynarmic's freed one, or be ignored (aot_profile.c dm_aot_profile_write). T_AOT_EXIT_BURN_MS (default 20) sets how much
__attribute__((destructor)) static void exit_burn(void) {
    if (!g_exit_run || !pthread_equal(pthread_self(), g_exit_thread)) return;
    const char *e = getenv("T_AOT_EXIT_BURN_MS");
    const long long ns = (e && *e ? atoll(e) : 20) * 1000000LL;
    struct timespec a, b;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
    do clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b); while ((b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec) < ns);
}
#endif

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
    if (!g_profile_run && dm_aot_profile_on()) dm_aot_profile_write();   // DM_AOT_PROFILE on a run (the parent's step 7): the profile and the misses now, as _exit runs no atexit
    fflush(NULL);
    if (g_profile_run) exit(g_fails ? 1 : g_status);   // exit: the sampler's writer runs (atexit)
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
    if (argc == 3 && !strcmp(argv[1], "--profile")) { g_profile_run = 1; return child(argv[2]); }
    if (argc == 3 && !strcmp(argv[1], "--exit")) { g_exit_run = 1; return child(argv[2]); }
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
    {
        // The hidden function's row is the one the pack does not serve (no compiled code holds it): one [RED] line naming it, at the offset the child found
        unsigned long long hidden_at = 0;
        const char *h = strstr(r.out, "check ok   the hidden function at main+");
        if (h) sscanf(h, "check ok   the hidden function at main+%llx", &hidden_at);
        char want_red[96];
        snprintf(want_red, sizeof want_red, "has an observation gate at main+%#llx that its pack does not call", hidden_at);
        int reds = 0;
        for (const char *q = strstr(r.out, "its pack does not call"); q; q = strstr(q + 1, "its pack does not call")) reds++;
        printf("      %s\n", red_line(r.out, "its pack does not call"));
        verdict(strstr(r.out, "aot: main: 2 gates (2 observation gates, 1 spin wait back-edges) called by its pack, 1 not") != NULL && hidden_at && reds == 1 &&
                    strstr(red_line(r.out, "its pack does not call"), want_red),
                "the loader: every gate row of the manifest is a gate site the pack's code calls but the hidden function's, which no compiled code holds (design 1.1)");
    }
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
    // 6. The sampler (DM_AOT_PROFILE, aot_profile.c): the profile run with the switch and without it (Linux through SIGPROF timers, Windows through the sampler thread)
    {
        const char *pdir = "t_aot_module.tmp/profile", *pfile = "t_aot_module.tmp/profile/main.tsv";
        mkdir("t_aot_module.tmp", 0755);
        remove(pfile);
        set_env("DM_AOT_PROFILE", pdir);
        run_child_mode(argv[0], "--profile", dir, &r);
        set_env("DM_AOT_PROFILE", NULL);
        fputs(r.out, stdout);
        unsigned long long hot = 0, cut = 0, calls = 0, passes = 0;
        double on_ns = 0, off_ns = 0;
        const char *h = strstr(r.out, "t_aot_profile: synth_hot at main+");
        if (h) sscanf(h, "t_aot_profile: synth_hot at main+%llx, synth_cut at main+%llx: %llu calls of synth_hot (%llu passes each, %lf ns per pass)", &hot, &cut, &calls, &passes, &on_ns);
        unsigned long long hot_lo = 0, hot_end = 0;
        const char *he = strstr(r.out, "t_aot_profile: synth_hot's guest code main+");
        char hel[200] = "";
        if (he) { snprintf(hel, sizeof hel, "%s", he); hel[strcspn(hel, "\n")] = 0; }
        sscanf(hel, "t_aot_profile: synth_hot's guest code main+%llx to main+%llx", &hot_lo, &hot_end);
        const int positions = strstr(hel, "the pack has positions") != NULL;
        size_t n = 0;
        char *text = (char *)slurp(pfile, &n);
        int rows = 0, bad_lines = 0, shown = 0, header = 0, hot_rows = 0;
        unsigned long long top = 0, top_n = 0, hot_n = 0, total = 0;
        char want_head[160];
        snprintf(want_head, sizeof want_head, "# module main, build ID %s, image SHA-256 ", BUILD_ID);
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (shown++ < 8) printf("      | %s\n", line);
            if (line[0] == '#') { header += !strncmp(line, want_head, strlen(want_head)) && strlen(line) == strlen(want_head) + 64; continue; }
            unsigned long long off = 0, cnt = 0; char extra = 0;
            if (sscanf(line, "%llx %llu %c", &off, &cnt, &extra) != 2 || strncmp(line, "0x", 2)) { bad_lines++; continue; }
            rows++; total += cnt;
            if (cnt > top_n) { top_n = cnt; top = off; }
            if (off >= hot && off < hot_end) { hot_n += cnt; hot_rows++; }
        }
        // With positions (aot.h, "The sampler") synth_hot's samples are at its instructions, the most in its loop after the entry's movz; without, all at its entry
        printf("      %d rows, %llu samples; the most at main+%#llx (%llu), synth_hot [main+%#llx, main+%#llx) %llu at %d rows%s\n", rows, total, top, top_n, hot, hot_end, hot_n,
               hot_rows, positions ? "" : " (the pack has no positions)");
        verdict(r.status == 0 && strstr(r.out, "check ok   synth_hot's sums") && text && header == 1 && !bad_lines && rows >= 1 && hot && hot_lo == hot && hot_end > hot &&
                    top >= hot && top < hot_end && hot_n * 2 > total && (positions ? top != hot : top == hot) && strstr(r.out, "aot: profile: t_aot_module.tmp/profile/main.tsv: "),
                "the sampler (DM_AOT_PROFILE): the profile written at exit names the module, its build ID and image, every line is <offset> <samples>, and synth_hot has the most, "
                "in its loop (the pack's positions)");
        free(text);
        free(r.out);
        remove(pfile);
        run_child_mode(argv[0], "--profile", dir, &r);
        h = strstr(r.out, "t_aot_profile: synth_hot at main+");
        if (h) sscanf(h, "t_aot_profile: synth_hot at main+%llx, synth_cut at main+%llx: %llu calls of synth_hot (%llu passes each, %lf ns per pass)", &hot, &cut, &calls, &passes, &off_ns);
        FILE *none = fopen(pfile, "rb");
        verdict(r.status == 0 && !none && strstr(r.out, "check ok   the sampler is off"), "without DM_AOT_PROFILE: the sampler is off and no profile is written");
        if (none) fclose(none);
        printf("t_aot_module: the sampler's cost on compiled code: synth_hot %.4f ns per pass with DM_AOT_PROFILE (1000 Hz), %.4f without (%+.2f %%)\n", on_ns, off_ns,
               off_ns > 0 ? 100.0 * (on_ns - off_ns) / off_ns : 0.0);
        free(r.out);
    }
    // 7. The JIT fallback's misses (aot.h, "The JIT fallback's misses"; aot_profile.c write_misses): the run with DM_AOT_PROFILE writes <folder>/main.misses.tsv beside the
    //    profile. The hidden function, called through the dispatcher (blr) at an address with no compiled entry, is its one row: `<offset> indirect 1`, under the header that
    //    names the module, its build ID and image as the profile's does (dm/tools/aot/tests/test_misses.py packs the module with a file of this form: the hidden function is
    //    then compiled code). A second run adds its entry to that row (the same image); a file of another image is replaced, its rows dropped
#if NXHOST_JIT
    {
        const char *mdir = "t_aot_module.tmp/misses", *mfile = "t_aot_module.tmp/misses/main.misses.tsv";
        mkdir("t_aot_module.tmp", 0755);
        mkdir(mdir, 0755);
        remove(mfile);
        set_env("DM_AOT_PROFILE", mdir);
        static const char *const what[3] = {
            "the JIT fallback's misses (DM_AOT_PROFILE): main.misses.tsv names the module, its build ID and image, and has one row, the hidden function's: indirect, 1 entry",
            "a second run: its entry added to the same row (2), the file of the same image merged",
            "a file of another image (its header's SHA-256 changed, a row of its own): replaced, only this run's row (1)"};
        for (int pass = 0; pass < 3; pass++) {
            if (pass == 2) {   // another image's misses: a header with another SHA-256 and a row that must not survive
                const char *other = "# nxaot misses: another image's\n# module main, build ID aa07aa07aa07aa07aa07aa07aa07aa07, image SHA-256 "
                                    "0000000000000000000000000000000000000000000000000000000000000000\n0x10 call 5\n";
                if (spit(mfile, other, strlen(other))) verdict(0, "a misses file of another image");
            }
            run_child(argv[0], dir, &r);
            unsigned long long hidden_at = 0;
            const char *h = strstr(r.out, "check ok   the hidden function at main+");
            if (h) sscanf(h, "check ok   the hidden function at main+%llx", &hidden_at);
            size_t n = 0;
            char *text = (char *)slurp(mfile, &n);
            int rows = 0, bad_lines = 0, shown = 0, header = 0;
            unsigned long long hidden_n = 0;
            char want_head[160];
            snprintf(want_head, sizeof want_head, "# module main, build ID %s, image SHA-256 ", BUILD_ID);
            for (char *line = text, *next; line && *line; line = next) {
                next = strchr(line, '\n');
                if (next) *next++ = 0;
                if (shown++ < 8 && pass == 0) printf("      | %s\n", line);
                if (line[0] == '#') { header += !strncmp(line, want_head, strlen(want_head)) && strlen(line) == strlen(want_head) + 64; continue; }
                unsigned long long off = 0, cnt = 0; char word[32], extra = 0;
                if (sscanf(line, "%llx %31s %llu %c", &off, word, &cnt, &extra) != 3 || strncmp(line, "0x", 2)) { bad_lines++; continue; }
                rows++;
                if (off == hidden_at && !strcmp(word, "indirect")) hidden_n = cnt;
            }
            const char *note = pass == 0 ? "aot: misses: t_aot_module.tmp/misses/main.misses.tsv: 1 addresses, 1 entries into the JIT fallback, 1 of the addresses this session (main)"
                             : pass == 1 ? "; merged with an earlier session's" : "; the file held another image's misses: replaced";
            printf("      pass %d: %d rows, the hidden function main+%#llx: %llu entries\n", pass + 1, rows, hidden_at, hidden_n);
            verdict(r.status == 0 && text && header == 1 && !bad_lines && rows == 1 && hidden_at && hidden_n == (pass == 1 ? 2u : 1u) && strstr(r.out, note), what[pass]);
            free(text);
            free(r.out);
        }
        set_env("DM_AOT_PROFILE", NULL);
    }
#else
    puts("note: this build has no JIT fallback (NXHOST_JIT off): the run calls no hidden function, so it has no misses; the misses file is not checked");
#endif
    // 8. A recorded session with JIT entries that ends as a game's does (--exit: exit() on the guest thread that made the first JIT instance, its sampling timer armed,
    //    DM_AOT_PROFILE at the default 1000 Hz), EXIT_RUNS times: every run exits 0, none killed by a signal. dynarmic's first instance installs an alternate signal
    //    stack of its own on that thread and its static SigHandler frees it at exit; a SIGPROF after that was delivered onto unmapped memory and the kernel killed the
    //    process with SIGSEGV (aot_fault.c dm_aot_fault_after_jit puts the thread's own stack back; aot_profile.c dm_aot_profile_write ignores SIGPROF once written)
#if NXHOST_JIT
    {
        enum { EXIT_RUNS = 20 };
        const char *edir = "t_aot_module.tmp/exit";
        mkdir("t_aot_module.tmp", 0755);
        mkdir(edir, 0755);
        set_env("DM_AOT_PROFILE", edir);
        set_env("DM_AOT_PROFILE_HZ", NULL);
        int ok = 0, signalled = 0, jit = 0;
        char first_bad[300] = "";
        for (int i = 0; i < EXIT_RUNS; i++) {
            run_child_mode(argv[0], "--exit", dir, &r);
            unsigned long long ja = 0, jr = 0;
            const char *x = strstr(r.out, "t_aot_module: exit run status ");
            int st = -1, cks = 0, cf = -1;
            if (x) sscanf(x, "t_aot_module: exit run status %d, %d checks, %d failed; %llu JIT addresses, %llu JIT runs", &st, &cks, &cf, &ja, &jr);
            const int good = r.status == 0 && st == 0 && cf == 0 && jr > 0 && strstr(r.out, "aot: misses: t_aot_module.tmp/exit/main.misses.tsv: ");   // the writer ran at exit (a run this short may have no sample in compiled code)
            ok += good; signalled += r.status > 128; jit += jr > 0;
            if (!good && !first_bad[0]) snprintf(first_bad, sizeof first_bad, "run %d: status %d%s", i + 1, r.status, r.status > 128 ? " (a signal)" : "");
            free(r.out);
        }
        set_env("DM_AOT_PROFILE", NULL);
        printf("      %d of %d runs exited 0 after the sampler wrote its files, %d killed by a signal, %d with JIT runs%s%s\n", ok, EXIT_RUNS, signalled, jit, first_bad[0] ? "; first: " : "",
               first_bad);
        verdict(ok == EXIT_RUNS, "a recorded session with JIT entries ending with exit() on its guest thread (DM_AOT_PROFILE, 1000 Hz): every run exits 0 after the sampler wrote its files");
    }
#else
    puts("note: this build has no JIT fallback (NXHOST_JIT off): the exit runs with JIT entries are skipped");
#endif
    printf("t_aot_module: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
