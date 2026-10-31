// A game module through the x86-64 engine's game-module path (docs/design/aot-recompiler.md 1.1, 1.2, 2.4, 9.4; loader.c, "The game-module path"), with no game files:
// the synthetic module of dm/tools/tests/aot_synth.py (an NSO named main of hand-assembled AArch64 code), its behaviour image and manifest as patch_nso.py --aot writes
// them (test_aot_manifest.py checks, where capstone is installed, that the rewriter writes exactly these bytes), packed by nxaot (dm/tools/aot/tests/build_packs.sh, into
// <packs>/module). Each run is a child process (this program with --run), as a game is a process:
//   the run: load_modules maps the image its manifest names (size, layout and SHA-256 checked) one to one in the arena, keeps its build ID and frame step state page,
//     registers its observation gate and spin wait in the gate table, relocates it and binds its two imports into its GOT (t_aot_mod_combine, a host function of
//     t_aot_module_entries.c, at its slot of the host-function page; t_aot_mod_missing, which nothing implements, at a stub slot) and loads its pack. Checked: the GOT, the
//     relocated function pointer, the image's permissions (text readable and never executable, data writable), the build ID, the state page; the two gates through
//     dm_aot_gate (the test's observer gets the block's registers and the row's parameter; the spin wait's back-off returns). Then dm_aot_run from the module's entry: a
//     call through the GOT reaches the host function through the dispatcher and its thunk, the stub answers 0 with an UNIMPL line, SetHeapSize reaches the stub backend's
//     be_guest_map with the heap's range, the module runs both x18 jump tables, the exclusive window, the spin wait, the cut function and a relocated pointer, prints
//     through OutputDebugString and ends with ExitProcess; results[] must be aot_synth.py's EXPECTED
//   a refusal: an image that is not its manifest's (one byte of data changed): the load stops with a [RED] line naming both SHA-256s
//   a refusal: a manifest and image that agree, with the pack of the old image: dm_aot_pack_load refuses the pack with both SHA-256s
// What this does not show: a real NSO or a real game (no game files here); that a pack calls the gates (nxaot's ABI 2 has no gate sites: only the host calls dm_aot_gate)
//   t_aot_module [--module <dir>]
//     the module's folder: --module, else <packs>/module with the packs of NXAOT_PACKS (the environment variable, else the CMake option); with none, a note and exit 0
//   t_aot_module --run <dir>     one run (the child)
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
uint64_t t_aot_mod_combine(uint64_t a, uint64_t b);   // t_aot_module_entries.c
extern _Atomic uint64_t g_t_aot_mod_calls;
extern _Atomic uint64_t g_t_guest_maps, g_t_guest_unmaps;   // t_aot_module_stub.c
extern uint64_t g_t_guest_map_a, g_t_guest_map_n;
extern uint8_t *dm_fps_state;          // loader.c

// What the module stores in results[i] (dm/tools/tests/aot_synth.py EXPECTED; results[3] is the heap's base, checked against be_guest_map)
static const uint64_t EXPECTED[12] = {7003, 0, 0, 0, 0x1234, 30, 400, 2, 9, 42, 2, 2};
static const char BUILD_ID[] = "aa07aa07aa07aa07aa07aa07aa07aa07";
enum { OBS_NUMBER = 23, OBS_PARAMETER = 5 };   // the module's gate: OBS_REGION_GOT with parameter 5

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

static int g_obs_hits, g_obs_param;
static uint64_t *g_obs_regs, g_obs_x5;
static void observer(uint64_t *regs, int param) { g_obs_hits++; g_obs_regs = regs; g_obs_param = param; g_obs_x5 = regs[5]; }

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
    g_status = dm_aot_run(base + dm_aot_pack_desc(p)->entry, 0, hmain, NULL);
    check(g_status == DM_AOT_EXITED, "the run ended with ExitProcess (status %d)", g_status);
    for (int i = 0; i < 12; i++) {
        if (i == 3) continue;
        check(results[i] == EXPECTED[i], "results[%d] = %#llx (expected %#llx)", i, (unsigned long long)results[i], (unsigned long long)EXPECTED[i]);
    }
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
    printf("t_aot_module: run status %d, %d checks, %d failed; %llu SVCs, %llu host calls, %llu dispatcher misses, %llu JIT addresses\n", g_status, g_checks, g_fails,
           (unsigned long long)s.svcs, (unsigned long long)s.host_calls, (unsigned long long)s.misses, (unsigned long long)s.jit_addresses);
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
    verdict(strstr(r.out, "observation gates and 1 spin waits") != NULL, "the [RED] line that the pack does not call the module's gates yet");
    free(r.out);
    // 2. An image that is not its manifest's
    char tdir[1100], h_old[65], h_new[65];
    if (tampered(dir, "image", 0, tdir, sizeof tdir, h_old, h_new)) verdict(0, "a copy of the module with another image (t_aot_module.tmp/image)");
    else {
        run_child(argv[0], tdir, &r);
        printf("      %s\n", red_line(r.out, "does not match"));
        verdict(r.status != 0 && strstr(r.out, "[RED] aot:") && strstr(r.out, "does not match") && strstr(r.out, h_old) && strstr(r.out, h_new) && !strstr(r.out, "run status"),
                "an image that is not its manifest's: refused with a [RED] line naming both SHA-256s, nothing run");
        free(r.out);
    }
    // 3. A manifest and image that agree, with the pack of the old image
    if (tampered(dir, "pack", 1, tdir, sizeof tdir, h_old, h_new)) verdict(0, "a copy of the module with another image and its manifest (t_aot_module.tmp/pack)");
    else {
        run_child(argv[0], tdir, &r);
        printf("      %s\n", red_line(r.out, "compiled from another image"));
        verdict(r.status != 0 && strstr(r.out, "[RED] aot:") && strstr(r.out, "compiled from another image") && strstr(r.out, h_old) && strstr(r.out, h_new) &&
                    !strstr(r.out, "run status"),
                "the pack of another image: refused with a [RED] line naming both SHA-256s, nothing run");
        free(r.out);
    }
    printf("t_aot_module: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
