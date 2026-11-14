// The hot indirect targets of design 12 in the host (dm/host/aot_targets.c; aot.h, "The hot indirect targets"; dm/tools/aot/README.md, "Hot indirect targets"): the corpus program
// fnptr (dm/tools/aot/corpus/src/fnptr.c: vtables, a dispatch table with an indirect tail call, callbacks, init and fini arrays, a pointer in data) run from its corpus pack
// (dm/tools/aot/tests/build_packs.sh, tier W, no profile) with DM_AOT_PROFILE, as t_aot_host.c runs the corpus: the host's loader maps the NRO, the pack is loaded, dm_aot_run
// enters _start, svcOutputDebugString lines are the program's output. Each run is a child process (this program with --run). The rows, for fnptr-gcc and fnptr-clang:
//   once      DM_AOT_PROFILE set, the child ends with exit() (the atexit writers): its lines as corpus/expected/fnptr.out, and <folder>/<name>.targets.tsv under the module's
//             header (its name, build ID none as the loader keeps an NRO's, the NRO's SHA-256) with a row `0x<site> 0x<target> <binds>` per site and target its cache was bound
//             to: every site a blr or br of the NRO's words, every target a function of corpus/nro/<name>.syms.tsv, and what the program's source says each site reaches:
//             with_cleanup's three calls through work_ptr bound to work once each, main's call through hidden_ptr to hidden_target once, rt_exit's fini array to dtor_plain
//             once, rt_init's init array to the three constructors once each, fold's callback (gcc keeps fold out of line; clang's fold.specialized.N copies inline
//             their callback and have no site) to the three callbacks once each, shape_total's
//             three vtable calls each to one slot of rect, tri and sq with 2, 1 and 2 binds (the shapes rect, tri, sq, rect, sq: a bind at each switch), apply_op's tail call
//             only to the op_ functions; the binds add up to the child's dispatcher misses (dm_aot_stats: binds of the caller's pack's own entries)
//   merged    the same folder again, the child writing the files itself (dm_aot_targets_write, then _exit): every row's binds doubled, the header's sites all of this session
//   host      hidden_ptr set to a host function of this test (the host-function page): main's site has no row, the log counts the misses to host functions
//   roundtrip with nxaot (--nxaot, or the environment variable NXAOT; the aot image): a profile with a sample at every recorded site (their functions at tier H) and the targets
//             file beside it; `nxaot build --profile` logs `aot: profile targets: <s> sites, <g> guarded, <m> with several targets, not guarded, <o> outside tier H` with g the
//             sites of one target, m the others, o 0; the guarded pack prints fnptr.out; recorded again under DM_AOT_PROFILE it binds no guarded site (its guards call the
//             target directly, which is why the host merges the files across sessions) and the others as before; NXAOT_PROFILE_TARGETS=0 logs off and gives another pack
//   t_aot_targets [--packs <dir>] [--corpus <dir>] [--nxaot <nxaot>]
//     the packs: --packs, else the environment variable NXAOT_PACKS, else the CMake option NXAOT_PACKS; with none, a note and exit 0 (as t_aot_host)
//   t_aot_targets --run <pack> <nro> [--hidden-host] [--write]      one run (the child); --write: dm_aot_profile_write and dm_aot_targets_write, then _exit
// The runner is the minimum of t_aot_host.c's (copied: that file is not shared); its stubs are t_aot_host_stub.c's, its one host function has a hand-written thunk below
#include "../aot.h"
#include "../aot_hostfn.h"
#include <stdatomic.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#ifndef T_AOT_CORPUS
#define T_AOT_CORPUS ""
#endif
#ifndef T_AOT_PACKS
#define T_AOT_PACKS ""
#endif
#if defined(_WIN32)
#define PACK_EXT ".dll"
#define popen _popen
#define pclose _pclose
#else
#define PACK_EXT ".so"
#endif

void guest_thread_init(void);          // thread.c
uint32_t svc_thread_main_init(void);   // hle_thread.c

// ---------- The host-function page's one entry (aot_hostfn.h: a test links its own table) ----------
// fnptr's hidden_target in the host (x ^ 0x5a5a5a5a5a5a5a5a), as t_aot_host_entries.c's t_aot_hidden: with hidden_ptr set to its slot the program's check "pointer.hidden"
// passes only when its blr went through the dispatcher into the page and back
static _Atomic uint64_t g_hidden_calls;
static uint64_t t_targets_hidden(uint64_t x) {
    atomic_fetch_add(&g_hidden_calls, 1);
    return x ^ 0x5a5a5a5a5a5a5a5aull;
}
static void t_targets_hidden_thunk(dm_guest_state *st) { st->x[0] = t_targets_hidden(st->x[0]); }
const hle_thunk_ent hle_thunks[] = {{"_Z16t_targets_hiddenm", "t_targets_hidden", t_targets_hidden_thunk, (const void *)t_targets_hidden}};
const size_t hle_thunks_count = sizeof hle_thunks / sizeof *hle_thunks;

// ---------- The child: one run ----------
typedef struct { const char *pack, *nro; int hidden, write, status; } run_args;

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len > 0 ? (size_t)len + 1 : 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(p); return NULL; }
    fclose(f); *n = (size_t)len;
    return p;
}
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void *guest_main(void *p) {
    run_args *a = p;
    a->status = DM_AOT_USAGE;
    guest_thread_init();
    const uint32_t hmain = svc_thread_main_init();
    size_t len = 0;
    uint8_t *bytes = slurp(a->nro, &len);
    if (!bytes || len < 0x80 || memcmp(bytes + 0x10, "NRO0", 4)) { printf("[RED] t_aot_targets: %s is no NRO\n", a->nro); free(bytes); return NULL; }
    // The NRO header: text, read-only data and data at +0x20, bss at +0x38; the image is the file (as t_aot_host.c guest_main)
    const uint64_t text_end = rd32(bytes + 0x24), ro_off = rd32(bytes + 0x28), data_off = rd32(bytes + 0x30), bss = rd32(bytes + 0x38);
    char name[16];
    const char *base = a->nro;
    for (const char *c = a->nro; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    snprintf(name, sizeof name, "%s", base);
    if (strstr(name, ".nro")) *strstr(name, ".nro") = 0;
    dm_aot_identity id;
    plat_sha256(bytes, len, id.sha256);
    id.image_size = len; id.text_end = text_end; id.bss_size = bss;
    module_t *m = loader_add_image(name, bytes, len, (size_t)bss, text_end, ro_off, data_off);
    free(bytes);
    char err[600];
    dm_aot_pack *pack = dm_aot_pack_load(a->pack, m, &id, err, sizeof err);
    if (!pack) { printf("[RED] t_aot_targets: %s\n", err); return NULL; }
    if (a->hidden) {   // fnptr's hidden_ptr: the guest-visible address of the host function above (2.4)
        uint64_t *hp = module_sym(m, "hidden_ptr");
        if (!hp) { printf("[RED] t_aot_targets: no hidden_ptr in %s\n", name); return NULL; }
        *hp = dm_guest_fn((const void *)t_targets_hidden);
    }
    a->status = dm_aot_run((uint64_t)(uintptr_t)m->base + dm_aot_pack_desc(pack)->entry, 0, hmain, NULL);
    return NULL;
}

static void timeout_exit(void) {
    fprintf(stderr, "[RED] t_aot_targets: the run did not end within 120 s\n");
    fflush(NULL);
    _exit(DM_AOT_STUCK);
}

static int child(int argc, char **argv) {
    run_args a;
    memset(&a, 0, sizeof a);
    a.pack = argv[2]; a.nro = argv[3];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--hidden-host")) a.hidden = 1;
        else if (!strcmp(argv[i], "--write")) a.write = 1;
        else { fprintf(stderr, "t_aot_targets: unknown option %s\n", argv[i]); return DM_AOT_USAGE; }
    }
    g_flag.dbg = 1;   // hle_svc.c prints every svcOutputDebugString as a "[guest] " line
    crash_install();
    dm_aot_arena_init();
    plat_after_ms(120000, timeout_exit);
    pthread_attr_t attr; pthread_t th;
    pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, guest_main, &a)) { fprintf(stderr, "t_aot_targets: pthread_create failed\n"); return DM_AOT_USAGE; }
    pthread_join(th, NULL);
    dm_aot_stats_t s;
    dm_aot_stats(&s);
    printf("t_aot_targets: status %d: %llu dispatcher misses, %llu calls into another pack, %llu host calls, hidden %llu\n", a.status, (unsigned long long)s.misses,
           (unsigned long long)s.cross_pack, (unsigned long long)s.host_calls, (unsigned long long)atomic_load(&g_hidden_calls));
    fflush(NULL);
    if (a.write) {   // the files now, as hle_lifecycle.c's graceful exit writes them before _exit
        dm_aot_profile_write();
        dm_aot_targets_write();
        fflush(NULL);
        _exit(a.status);
    }
    exit(a.status);   // the atexit writers (aot_profile.c's, and aot_targets.c's, registered at the first inline-cache miss)
}

// ---------- The parent ----------
static int checks, fails;
static void row(const char *name, const char *what, int ok, const char *note) {
    checks++; fails += !ok;
    printf("%-12s %-9s %-4s %s\n", name, what, ok ? "ok" : "FAIL", note);
}

static void set_env(const char *k, const char *v) {   // for the children popen starts
#if defined(_WIN32)
    char b[1100]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

// A command's whole output (stdout and stderr) into *out (malloc'd), its exit status
static int capture(const char *cmd, char **out) {
    size_t n = 0, cap = 4096;
    char *s = malloc(cap), buf[8192];
    if (!s) abort();
    s[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f) { *out = s; return -1; }
    while (fgets(buf, sizeof buf, f)) {
        const size_t l = strlen(buf);
        if (n + l + 1 > cap) { cap = 2 * (n + l + 1); if (!(s = realloc(s, cap))) abort(); }
        memcpy(s + n, buf, l + 1); n += l;
    }
    const int st = pclose(f);
    *out = s;
#if defined(_WIN32)
    return st;
#else
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
}

typedef struct { int status; char *log, *guest; unsigned long long misses, cross, host_calls, hidden; int have_stat; } result;
static void run_child(const char *self, const char *pack, const char *nro, const char *extra, result *r) {
    char cmd[4096];
    memset(r, 0, sizeof *r);
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --run \"%s\" \"%s\" %s 2>&1\"", self, pack, nro, extra);
#else
    snprintf(cmd, sizeof cmd, "'%s' --run '%s' '%s' %s 2>&1", self, pack, nro, extra);
#endif
    r->status = capture(cmd, &r->log);
    size_t gn = 0;
    r->guest = calloc(strlen(r->log) + 1, 1);
    if (!r->guest) abort();
    for (const char *l = r->log; *l;) {
        const size_t len = strcspn(l, "\n") + (l[strcspn(l, "\n")] == '\n');
        if (!strncmp(l, "[guest] ", 8)) { memcpy(r->guest + gn, l + 8, len - 8); gn += len - 8; }
        const char *st = strstr(l, "t_aot_targets: status ");
        if (st && st < l + len)
            r->have_stat = sscanf(st, "t_aot_targets: status %*d: %llu dispatcher misses, %llu calls into another pack, %llu host calls, hidden %llu", &r->misses, &r->cross,
                                  &r->host_calls, &r->hidden) == 4;
        l += len;
    }
}
static void free_result(result *r) { free(r->log); free(r->guest); }

// The log's line that starts with prefix (after any "[dm] "-style tag), into out; 0 when none
static int log_line(const char *log, const char *prefix, char *out, size_t n) {
    const char *p = strstr(log, prefix);
    if (!p) { out[0] = 0; return 0; }
    snprintf(out, n, "%.*s", (int)strcspn(p, "\n"), p);
    return 1;
}

typedef struct { uint64_t lo, hi; char name[64]; } sym_fn;
// The functions of corpus/nro/<name>.syms.tsv (`offset size type binding name`, hexadecimal): their number, into fns[0 .. max) (as t_aot_host.c corpus_functions)
static int corpus_functions(const char *corpus, const char *name, sym_fn *fns, int max) {
    char path[1100];
    snprintf(path, sizeof path, "%s/nro/%s.syms.tsv", corpus, name);
    size_t len = 0;
    char *text = (char *)slurp(path, &len);
    if (!text) return 0;
    text[len] = 0;
    int n = 0;
    for (char *line = text, *next; line && *line && n < max; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        unsigned long long off, size; char type[16], bind[16], sym[64];
        if (sscanf(line, "%llx\t%llx\t%15s\t%15s\t%63s", &off, &size, type, bind, sym) == 5 && !strcmp(type, "func") && size) {
            fns[n].lo = off; fns[n].hi = off + size;
            snprintf(fns[n].name, sizeof fns[n].name, "%s", sym);
            n++;
        }
    }
    free(text);
    return n;
}
// The function whose name is base, or base and a compiler's suffix (gcc's fold.constprop.0); -1 when none
static int fn_named(const sym_fn *f, int n, const char *base) {
    const size_t l = strlen(base);
    for (int i = 0; i < n; i++) if (!strncmp(f[i].name, base, l) && (f[i].name[l] == 0 || f[i].name[l] == '.')) return i;
    return -1;
}
static int fn_at(const sym_fn *f, int n, uint64_t entry) { for (int i = 0; i < n; i++) if (f[i].lo == entry) return i; return -1; }
static int fn_holding(const sym_fn *f, int n, uint64_t off) { for (int i = 0; i < n; i++) if (off >= f[i].lo && off < f[i].hi) return i; return -1; }
// The NRO's indirect sites in [lo, hi): blr, and br other than br x30 (a return), the two an inline cache serves (module offsets are file offsets)
static int is_site(const uint8_t *img, size_t len, uint64_t pc) {
    if (pc + 4 > len || (pc & 3)) return 0;
    const uint32_t w = rd32(img + pc);
    return (w & 0xFFFFFC1Fu) == 0xD63F0000u || ((w & 0xFFFFFC1Fu) == 0xD61F0000u && ((w >> 5) & 31) != 30);
}
static int sites_in(const uint8_t *img, size_t len, const sym_fn *f, uint64_t *out, int max) {
    int k = 0;
    for (uint64_t pc = f->lo; pc < f->hi && k < max; pc += 4) if (is_site(img, len, pc)) out[k++] = pc;
    return k;
}

typedef struct { uint64_t site, target, n; } trow;
typedef struct { trow rows[512]; int nrows, header, bad; char sites_line[200]; } tfile;
// A targets file: its rows, whether its header names module with build ID none and image sha, the lines that do not parse
static int read_targets(const char *path, const char *module, const char *sha, tfile *t) {
    memset(t, 0, sizeof *t);
    size_t len = 0;
    char *text = (char *)slurp(path, &len);
    if (!text) return 0;
    text[len] = 0;
    char head[200];
    snprintf(head, sizeof head, "# module %s, build ID none, image SHA-256 %s", module, sha);
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#') {
            t->header += !strcmp(line, head);
            if (strstr(line, " of the sites this session")) snprintf(t->sites_line, sizeof t->sites_line, "%s", line);
            continue;
        }
        unsigned long long s, g, n; char extra = 0;
        if (sscanf(line, "0x%llx 0x%llx %llu %c", &s, &g, &n, &extra) != 3 || t->nrows == 512) { t->bad++; continue; }
        t->rows[t->nrows++] = (trow){s, g, n};
    }
    free(text);
    return 1;
}
static int rows_at(const tfile *t, uint64_t site, const trow **first) {   // the rows of one site (the file is sorted by site): their number
    int k = 0;
    *first = NULL;
    for (int i = 0; i < t->nrows; i++) if (t->rows[i].site == site) { if (!k) *first = &t->rows[i]; k++; }
    return k;
}

// What fnptr's source says its sites reach (the header's "once" row), with every bind count times mult: the problems into note; 1 when there are none
static int check_rows(const tfile *t, const uint8_t *img, size_t len, const sym_fn *f, int nf, unsigned mult, char *note, size_t n) {
    size_t o = 0;
    note[0] = 0;
#define BAD(...) do { if (o < n) o += (size_t)snprintf(note + o, n - o, __VA_ARGS__); } while (0)
    // Every row: a site of the NRO's words, a target that is a function's entry
    for (int i = 0; i < t->nrows; i++) {
        if (!is_site(img, len, t->rows[i].site)) BAD("; row %#llx is no blr or br", (unsigned long long)t->rows[i].site);
        if (fn_at(f, nf, t->rows[i].target) < 0) BAD("; row %#llx's target %#llx is no function", (unsigned long long)t->rows[i].site, (unsigned long long)t->rows[i].target);
    }
    // A site with exactly the targets named, each with binds bind * mult
    struct { const char *fn; int sites; const char *targets[3]; int binds; int optional; } want[] = {
        {"with_cleanup", 3, {"work"}, 1, 0},
        {"main", 1, {"hidden_target"}, 1, 0},
        {"rt_exit", 1, {"dtor_plain"}, 1, 0},
        {"rt_init", 1, {"ctor_first", "ctor_second", "ctor_plain"}, 1, 0},
        {"fold", 1, {"cb_sum", "cb_max", "cb_xor_rot"}, 1, 1},   // gcc's fold.constprop.0; clang specializes fold per callback (fold.specialized.N), inlined: no site
    };
    for (size_t w = 0; w < sizeof want / sizeof *want; w++) {
        const int fi = fn_named(f, nf, want[w].fn);
        if (fi < 0) { if (!want[w].optional) BAD("; no function %s", want[w].fn); continue; }
        uint64_t s[8];
        const int ns = sites_in(img, len, &f[fi], s, 8);
        if (ns == 0 && want[w].optional) continue;   // the callee inlined into a specialized copy: no indirect site to record
        if (ns != want[w].sites) { BAD("; %s has %d sites in the NRO, the test expects %d", want[w].fn, ns, want[w].sites); continue; }
        for (int k = 0; k < ns; k++) {
            const trow *r;
            const int nr = rows_at(t, s[k], &r);
            int nt = 0;
            for (int j = 0; j < 3 && want[w].targets[j]; j++) nt++;
            int good = nr == nt;
            for (int j = 0; good && j < nr; j++) {
                const int ti = fn_at(f, nf, r[j].target);
                int named = 0;
                for (int q = 0; q < nt && ti >= 0; q++) named |= !strcmp(f[ti].name, want[w].targets[q]);
                good = named && r[j].n == (uint64_t)want[w].binds * mult;
            }
            if (!good) BAD("; %s's site %#llx: %d rows, want %d to %s... with %u binds each", want[w].fn, (unsigned long long)s[k], nr, nt, want[w].targets[0], want[w].binds * mult);
        }
    }
    // shape_total: three sites, each bound to one slot of rect, tri and sq (rect_area, tri_area, sq_area, ...): 2, 1 and 2 binds
    const int st = fn_named(f, nf, "shape_total");
    uint64_t s[8];
    const int ns = st >= 0 ? sites_in(img, len, &f[st], s, 8) : 0;
    if (ns != 3) BAD("; shape_total has %d sites in the NRO, the test expects 3", ns);
    unsigned slots = 0;   // bit per slot kind seen: area, perimeter, scale
    for (int k = 0; k < ns; k++) {
        const trow *r;
        const int nr = rows_at(t, s[k], &r);
        char kind[32] = "";
        int good = nr == 3;
        for (int j = 0; good && j < nr; j++) {
            const int ti = fn_at(f, nf, r[j].target);
            const char *nm = ti >= 0 ? f[ti].name : "", *us = strchr(nm, '_');
            if (!us) { good = 0; break; }
            if (!kind[0]) snprintf(kind, sizeof kind, "%s", us + 1);
            const unsigned want_n = !strncmp(nm, "tri_", 4) ? 1 : 2;
            good = !strcmp(us + 1, kind) && (!strncmp(nm, "rect_", 5) || !strncmp(nm, "tri_", 4) || !strncmp(nm, "sq_", 3)) && r[j].n == (uint64_t)want_n * mult;
        }
        if (good) slots |= !strcmp(kind, "area") ? 1u : !strcmp(kind, "perimeter") ? 2u : !strcmp(kind, "scale") ? 4u : 0u;
        else BAD("; shape_total's site %#llx: %d rows, want rect_, tri_ and sq_ of one slot with %u, %u and %u binds", (unsigned long long)s[k], nr, 2 * mult, mult, 2 * mult);
    }
    if (ns == 3 && slots != 7) BAD("; shape_total's sites do not cover area, perimeter and scale");
    // apply_op: its tail call, when the inline cache serves it (a jump table of the analysis would not), only to the op_ functions
    const int ao = fn_named(f, nf, "apply_op");
    if (ao >= 0) {
        const int nsa = sites_in(img, len, &f[ao], s, 8);
        for (int k = 0; k < nsa; k++) {
            const trow *r;
            const int nr = rows_at(t, s[k], &r);
            for (int j = 0; j < nr; j++) {
                const int ti = fn_at(f, nf, r[j].target);
                if (ti < 0 || strncmp(f[ti].name, "op_", 3)) BAD("; apply_op's site %#llx bound to %#llx, no op_ function", (unsigned long long)s[k], (unsigned long long)r[j].target);
            }
        }
    }
#undef BAD
    return o == 0;
}

static uint64_t binds_of(const tfile *t) { uint64_t b = 0; for (int i = 0; i < t->nrows; i++) b += t->rows[i].n; return b; }
static void sha_hex(const char *path, char out[65]) {
    size_t len = 0;
    uint8_t *b = slurp(path, &len), h[32];
    out[0] = 0;
    if (!b) return;
    plat_sha256(b, len, h);
    free(b);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}
static int same_file(const char *a, const char *b) {
    size_t na = 0, nb = 0;
    uint8_t *x = slurp(a, &na), *y = slurp(b, &nb);
    const int same = x && y && na == nb && !memcmp(x, y, na);
    free(x); free(y);
    return same;
}
static void fresh_dir(const char *dir, const char *name) {   // the folder, made, with an earlier run's files of the module gone
    char p[1200];
    mkdir(dir, 0755);
    const char *ext[] = {".targets.tsv", ".tsv", ".misses.tsv"};
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) { snprintf(p, sizeof p, "%s/%s%s", dir, name, ext[i]); remove(p); }
}

// nxaot build <nro> --shared <out> --profile <profile> [NXAOT_PROFILE_TARGETS=0]: its exit status, its output into *log
static int nxaot_build(const char *nxaot, const char *nro, const char *out, const char *profile, int targets_off, char **log) {
    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" build \"%s\" --shared \"%s\" --profile \"%s\" 2>&1\"", nxaot, nro, out, profile);
#else
    snprintf(cmd, sizeof cmd, "'%s' build '%s' --shared '%s' --profile '%s' 2>&1", nxaot, nro, out, profile);
#endif
    set_env("NXAOT_PROFILE_TARGETS", targets_off ? "0" : NULL);
    const int st = capture(cmd, log);
    set_env("NXAOT_PROFILE_TARGETS", NULL);
    return st;
}

static void program(const char *self, const char *name, const char *packs, const char *corpus, const char *nxaot) {
    char pack[1024], nro[1024], exp_path[1024], note[3000], line[600], root[300], dir[400], file[600], sha[65];
    snprintf(pack, sizeof pack, "%s/%s" PACK_EXT, packs, name);
    snprintf(nro, sizeof nro, "%s/nro/%s.nro", corpus, name);
    snprintf(exp_path, sizeof exp_path, "%s/expected/fnptr.out", corpus);
    size_t elen = 0, ilen = 0;
    char *expected = (char *)slurp(exp_path, &elen);
    uint8_t *img = slurp(nro, &ilen);
    FILE *pf = fopen(pack, "rb");
    if (!expected || !img || !pf) {
        snprintf(note, sizeof note, "missing: %s%s%s", !expected ? exp_path : "", !img ? nro : "", !pf ? pack : "");
        row(name, "files", 0, note);
        if (pf) fclose(pf);
        free(expected); free(img);
        return;
    }
    fclose(pf);
    expected[elen] = 0;
    sha_hex(nro, sha);
    sym_fn f[256];
    const int nf = corpus_functions(corpus, name, f, 256);
    mkdir("t_aot_targets.tmp", 0755);
    snprintf(root, sizeof root, "t_aot_targets.tmp/%s", name);
    mkdir(root, 0755);
    result r;
    tfile t1, t2;

    // once: the atexit writer
    snprintf(dir, sizeof dir, "%s/once", root);
    fresh_dir(dir, name);
    snprintf(file, sizeof file, "%s/%s.targets.tsv", dir, name);
    set_env("DM_AOT_PROFILE", dir);
    run_child(self, pack, nro, "", &r);
    set_env("DM_AOT_PROFILE", NULL);
    int run_ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_stat;
    int got = read_targets(file, name, sha, &t1);
    char why[1500];
    int rows_ok = got && check_rows(&t1, img, ilen, f, nf, 1, why, sizeof why);
    log_line(r.log, "aot: targets: this session", line, sizeof line);
    const uint64_t b1 = binds_of(&t1);
    int ok = run_ok && got && t1.header == 1 && !t1.bad && rows_ok && b1 == r.misses && r.cross == 0;
    snprintf(note, sizeof note, "%s%d rows, %llu binds (the child's dispatcher misses: %llu), header %s; %s%s%s", run_ok ? "" : "the run's lines or status differ; ", t1.nrows,
             (unsigned long long)b1, r.misses, t1.header == 1 ? "names the image" : "missing or another image's", line[0] ? line : "no `aot: targets: this session` line",
             got ? "" : "; no targets file", why);
    row(name, "once", ok, note);
    for (int i = 0; i < t1.nrows; i++) {   // the file's rows, named
        const int si = fn_holding(f, nf, t1.rows[i].site), ti = fn_at(f, nf, t1.rows[i].target);
        printf("%-12s %-9s      0x%llx 0x%llx %llu   (%s -> %s)\n", "", "", (unsigned long long)t1.rows[i].site, (unsigned long long)t1.rows[i].target,
               (unsigned long long)t1.rows[i].n, si >= 0 ? f[si].name : "?", ti >= 0 ? f[ti].name : "?");
    }
    free_result(&r);

    // merged: the same folder again, the child writing the files itself
    set_env("DM_AOT_PROFILE", dir);
    run_child(self, pack, nro, "--write", &r);
    set_env("DM_AOT_PROFILE", NULL);
    run_ok = r.status == 0 && !strcmp(r.guest, expected);
    got = read_targets(file, name, sha, &t2);
    rows_ok = got && check_rows(&t2, img, ilen, f, nf, 2, why, sizeof why);
    int doubled = got && t2.nrows == t1.nrows;
    for (int i = 0; doubled && i < t2.nrows; i++) doubled = t2.rows[i].site == t1.rows[i].site && t2.rows[i].target == t1.rows[i].target && t2.rows[i].n == 2 * t1.rows[i].n;
    log_line(r.log, "aot: targets: t_aot_targets.tmp", line, sizeof line);
    ok = run_ok && got && t2.header == 1 && !t2.bad && doubled && rows_ok && strstr(line, "; merged with an earlier session's") != NULL;
    snprintf(note, sizeof note, "%s%s; %s; %s%s", run_ok ? "" : "the run's lines or status differ; ", doubled ? "every row's binds doubled" : "the rows are not the first run's doubled",
             t2.sites_line[0] ? t2.sites_line : "no sites header", line[0] ? line : "no writer's line", why);
    row(name, "merged", ok, note);
    free_result(&r);

    // host: hidden_ptr to a host function: main's call is a miss to the host-function page each time, no row
    snprintf(dir, sizeof dir, "%s/host", root);
    fresh_dir(dir, name);
    snprintf(file, sizeof file, "%s/%s.targets.tsv", dir, name);
    set_env("DM_AOT_PROFILE", dir);
    run_child(self, pack, nro, "--hidden-host", &r);
    set_env("DM_AOT_PROFILE", NULL);
    run_ok = r.status == 0 && !strcmp(r.guest, expected) && r.hidden >= 1;
    tfile th;
    got = read_targets(file, name, sha, &th);
    const int mi = fn_named(f, nf, "main");
    uint64_t ms[4];
    const int nms = mi >= 0 ? sites_in(img, ilen, &f[mi], ms, 4) : 0;
    const trow *mr;
    const int main_rows = nms == 1 ? rows_at(&th, ms[0], &mr) : -1;
    unsigned long long pairs = 0, bound = 0, other = 0, handed = 0, host = 0, nocode = 0;
    int files = 0;
    const int parsed = log_line(r.log, "aot: targets: this session", line, sizeof line) &&
                       sscanf(line, "aot: targets: this session %llu (site, target) pairs bound %llu times in their own module's code, %d files; not written: %llu misses bound to "
                                    "another pack's code, %llu handed to another pack of another signature, %llu to host functions, %llu to addresses with no compiled entry",
                              &pairs, &bound, &files, &other, &handed, &host, &nocode) == 7;
    ok = run_ok && got && main_rows == 0 && th.nrows == t1.nrows - 1 && parsed && host == r.hidden && bound == binds_of(&th);
    snprintf(note, sizeof note, "%s%llu calls of the host function; main's site: %d rows; %d rows (the plain run's less one); %s", run_ok ? "" : "the run's lines or status differ; ",
             r.hidden, main_rows, th.nrows, line[0] ? line : "no `aot: targets: this session` line");
    row(name, "host", ok, note);
    free_result(&r);

    // roundtrip: the recorded targets through nxaot build --profile
    if (!nxaot || !*nxaot) {
        printf("%-12s %-9s note: no nxaot (--nxaot, NXAOT): the targets' round trip through nxaot build --profile is not checked here\n", name, "roundtrip");
        free(expected); free(img);
        return;
    }
    char prof[600], tgt[600], so[600], so_off[600], *blog = NULL, *olog = NULL;
    snprintf(dir, sizeof dir, "%s/rt", root);
    fresh_dir(dir, name);
    snprintf(prof, sizeof prof, "%s/%s.tsv", dir, name);
    snprintf(tgt, sizeof tgt, "%s/%s.targets.tsv", dir, name);
    FILE *pw = fopen(prof, "wb"), *tw = fopen(tgt, "wb");
    if (pw) fprintf(pw, "# t_aot_targets: a sample at every recorded site, so that the functions holding them are at tier H\n");
    if (tw) fprintf(tw, "# t_aot_targets: the once run's targets\n# module %s, build ID none, image SHA-256 %s\n", name, sha);
    unsigned single = 0, several = 0;
    for (int i = 0; i < t1.nrows; i++) {
        const trow *sr;
        const int nr = rows_at(&t1, t1.rows[i].site, &sr);
        if (sr == &t1.rows[i]) {   // the site's first row
            if (pw) fprintf(pw, "0x%llx 1\n", (unsigned long long)t1.rows[i].site);
            if (nr == 1) single++; else several++;
        }
        if (tw) fprintf(tw, "0x%llx 0x%llx %llu\n", (unsigned long long)t1.rows[i].site, (unsigned long long)t1.rows[i].target, (unsigned long long)t1.rows[i].n);
    }
    if (pw) fclose(pw);
    if (tw) fclose(tw);
    char gdir[600], odir[600];   // one file name in two folders: the name is the pack's soname, part of its bytes
    snprintf(gdir, sizeof gdir, "%s/guarded", dir); mkdir(gdir, 0755); snprintf(so, sizeof so, "%s/x" PACK_EXT, gdir);
    snprintf(odir, sizeof odir, "%s/off", dir); mkdir(odir, 0755); snprintf(so_off, sizeof so_off, "%s/x" PACK_EXT, odir);
    const int s1 = nxaot_build(nxaot, nro, so, prof, 0, &blog), s2 = nxaot_build(nxaot, nro, so_off, prof, 1, &olog);
    unsigned long long ns = 0, ng = 0, nm = 0, no = 0;
    const int tline = log_line(blog, "aot: profile targets: ", line, sizeof line) &&
                      sscanf(line, "aot: profile targets: %llu sites, %llu guarded, %llu with several targets, not guarded, %llu outside tier H", &ns, &ng, &nm, &no) == 4;
    char offline[300];
    log_line(olog, "aot: profile targets: ", offline, sizeof offline);
    ok = s1 == 0 && s2 == 0 && tline && ns == single + several && ng == single && ng >= 1 && nm == several && no == 0 &&
         strstr(offline, "off (NXAOT_PROFILE_TARGETS=0)") && !same_file(so, so_off);
    snprintf(note, sizeof note, "%s (want %u sites, %u guarded, %u with several); NXAOT_PROFILE_TARGETS=0: %s, its pack %s%s", line[0] ? line : "no `aot: profile targets:` line",
             single + several, single, several, offline[0] ? offline : "no line", same_file(so, so_off) ? "the same bytes (want another)" : "another",
             s1 || s2 ? "; nxaot build failed" : "");
    row(name, "roundtrip", ok, note);
    if (s1 || s2) printf("%s\n%s\n", blog, olog);
    free(blog); free(olog);
    // The guarded pack runs, and recorded again binds none of the guarded sites
    char rec[600], recfile[700];
    snprintf(rec, sizeof rec, "%s/rec", dir);
    fresh_dir(rec, name);
    snprintf(recfile, sizeof recfile, "%s/%s.targets.tsv", rec, name);
    set_env("DM_AOT_PROFILE", rec);
    run_child(self, so, nro, "--write", &r);
    set_env("DM_AOT_PROFILE", NULL);
    run_ok = r.status == 0 && !strcmp(r.guest, expected);
    tfile tg;
    got = read_targets(recfile, name, sha, &tg);
    unsigned guarded_rows = 0, kept = 0;
    for (int i = 0; i < t1.nrows; i++) {
        const trow *sr, *gr;
        const int nr = rows_at(&t1, t1.rows[i].site, &sr), ngr = rows_at(&tg, t1.rows[i].site, &gr);
        if (sr != &t1.rows[i]) continue;
        if (nr == 1) guarded_rows += ngr;
        else kept += ngr == nr;
    }
    // A file only when some cache was bound (the guarded pack's several-target sites are)
    ok = run_ok && (several ? got && guarded_rows == 0 && kept == several : guarded_rows == 0);
    snprintf(note, sizeof note, "%sthe guarded pack prints fnptr.out; recorded: %u rows at the %u guarded sites (want none), %u of the %u sites with several targets as before",
             run_ok ? "" : "the run's lines or status differ: ", guarded_rows, single, kept, several);
    row(name, "guarded", ok, note);
    free_result(&r);
    free(expected); free(img);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--run")) return child(argc, argv);
    const char *packs = NULL, *corpus = T_AOT_CORPUS, *nxaot = getenv("NXAOT");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--packs") && i + 1 < argc) packs = argv[++i];
        else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
        else if (!strcmp(argv[i], "--nxaot") && i + 1 < argc) nxaot = argv[++i];
        else { fprintf(stderr, "usage: t_aot_targets [--packs <dir>] [--corpus <dir>] [--nxaot <nxaot>]\n"); return 2; }
    }
    if (!packs) packs = getenv("NXAOT_PACKS");
    if (!packs || !*packs) packs = T_AOT_PACKS;
    if (!*packs) { puts("note: no corpus packs (NXAOT_PACKS unset; dm/tools/aot/tests/build_packs.sh makes them in the nxnative-aot image): skipped"); return 0; }
    printf("t_aot_targets: packs %s, corpus %s, nxaot %s\n", packs, corpus, nxaot && *nxaot ? nxaot : "none");
    set_env("DM_AOT_PROFILE_HZ", NULL);
    program(argv[0], "fnptr-gcc", packs, corpus, nxaot);
    program(argv[0], "fnptr-clang", packs, corpus, nxaot);
    printf("t_aot_targets: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
