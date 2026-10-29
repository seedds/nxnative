// The Vulkan backend's prewarm lists, the prediction from their rows and the prebuild (docs/design/vulkan-backend.md 3.3, 3.5; phase P5).
//
// Lists. As the Metal backend reads them (nvn_mtl.m warm_start / warm_parse): the profile's seed list pso_manifest.tsv beside the shader map (DM_NVN_SHMAP), then this device's
// own list; DM_PSO_MANIFEST=<file> reads that list alone, DM_PSO_MANIFEST=off none. The device's list is pso_user_vk.tsv beside the save (the parent of DM_SAVE; DM_VK_PSO_USER=<file>):
// a P row (nvn_pipe.h, NVN formats) for each pipeline a draw had to build that no list named, appended at once and prewarmed after the seed at the next start (the Metal backend's
// pso_user.tsv holds R rows, which name Metal formats; the two lists are kept apart). Rows: S (a shader: on levels A and B its library, the pre-rasterization library of a vertex
// shader or the fragment-shader library of a fragment shader, for its stage combination's universal layout; on C and D its module), C (a compute pipeline), and R and P (a render
// pipeline, through nvn_pipe_parse_row: an R row's Metal formats decoded through the inverse format table; merged-pass and readable-tile rows are Metal's and are skipped).
// Order: the lists' order (the order the recording first used each one), repeated lines once; the render pipelines of one shader pair are one task at the pair's first row, as the
// Metal backend groups them, so its libraries are made once and the pair's links follow; rows naming a shader the shader map does not have (another game version) are dropped
// quietly and counted. A row's pipeline is keyed exactly as a draw keys it (hvk_key_fill, vk_pso.h), so a draw of a listed pipeline finds it made.
// What a row cannot say: the topology class (rows are made for triangles; on a device that draws every class from one pipeline the class is not in the key, as on lavapipe and
// [A] NVIDIA), the geometry and tessellation stages (nvn_pipe.h's TODO list), whether a vertex stream has a buffer (a row takes every stream it names as bound), and the
// texture's format behind a view of another format (the row's format is attached as an image of that format would be).
//
// Background (the default): the first submission reads the lists on a background worker and queues the tasks in list order; the game does not wait. Prediction (from the rows
// alone): when a draw first uses a pipeline a row named, the tasks of the rows recorded after it are built next, ahead of the list order; when a draw misses, the task of its
// shader pair (the pair's other rows: the same shaders with other formats) and the task after it; at most two speculative builds outstanding (the Metal backend's budget,
// development.md "Prewarm lists"). The look-ahead over the rest of a live submission (pso_predict_rest_ in nvn_mtl.m: replaying the state of the draws
// still to come after a miss, with the frontend's state save and restore) needs the command lists a game submits: declared, with a red line at the first miss.
// Prebuild (decision D3, DM_VK_PREBUILD=1): the first submission waits until every row is built, and on levels A and B the libraries of every shader in the shader map too
// (the counterpart of the Metal prebuild's bases of every pair), on one worker per hardware thread less one at normal priority, with the progress reported through the front
// end's front_progress ("Preparing shaders", shown past 0.5 s when 0.3 s or more is left, as the Metal prepare step); then the disk cache is written. On levels C and D the
// pipelines of the pair tables (shader_pairs.tsv) cannot be made before the game runs: they name no attachment formats, which a Vulkan pipeline is made with.
#include "plat/plat.h"
#include "vk_pso.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_SHADER, T_COMPUTE, T_PAIR };
typedef struct {
    int kind; char vs[24], fs[24];   // keys (a shader or compute task: vs)
    nvn_pipe_desc *d; int *row; int nd, cap;   // a pair's rows and their numbers
    _Atomic int claimed;             // a worker took it (the list order or a prediction)
} task;
static task *g_t; static int g_nt; static int *g_row_task; static int g_nrows;   // the tasks; row number → its task
static nvn_hmap g_pairs;             // a pair's two map keys (16 hex digits each, NUL-padded to 24) → its task index + 1
static _Atomic int g_ready;          // the tasks are published (the prediction reads them)
static _Atomic int g_left;           // tasks not finished
static _Atomic uint64_t s_lines, s_rows, s_srows, s_crows, s_stale, s_skipped, s_built, s_failed, s_spec, s_user;
static uint64_t g_t0, g_t1; static int g_prebuild, g_started; static _Atomic int g_running;
static uint64_t g_show_after = 500000000ull;
static nvn_hmap g_seen;              // SHA-256 of each list line (the lists' lines and those appended): the user list holds each pipeline once
static pthread_mutex_t g_user_mu = PTHREAD_MUTEX_INITIALIZER; static FILE *g_user; static int g_user_tried; static char g_user_path[1024];

void hvk_prewarm_test_show_after(uint64_t ns) { g_show_after = ns; }
static void parent_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    size_t l = strlen(out); while (l > 1 && (out[l - 1] == '/' || out[l - 1] == '\\')) out[--l] = 0;
    char *a = strrchr(out, '/'), *b = strrchr(out, '\\'); if (b && (!a || b > a)) a = b;
    if (a) { if (a == out) a[1] = 0; else *a = 0; } else snprintf(out, n, ".");
}
static const char *user_path(void) {
    if (g_user_path[0]) return g_user_path;
    const char *u = getenv("DM_VK_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) snprintf(g_user_path, sizeof g_user_path, "%s", u);
    else if (save && *save) { char p[900]; parent_of(save, p, sizeof p); snprintf(g_user_path, sizeof g_user_path, "%s/pso_user_vk.tsv", p); }
    return g_user_path[0] ? g_user_path : NULL;
}
static void seen_add(const char *line) {   // under g_user_mu
    uint8_t h[32]; plat_sha256(line, strlen(line), h);
    if (!g_seen.klen) nvn_hm_init(&g_seen, 32);
    if (!nvn_hm_get(&g_seen, h)) nvn_hm_put(&g_seen, h, (void *)1);
}
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}
static task *task_add(int kind) {
    if (!(g_nt & (g_nt + 1))) { task *t = realloc(g_t, (size_t)(2 * g_nt + 2) * sizeof *t); if (!t) return NULL; g_t = t; }
    task *t = &g_t[g_nt++]; memset(t, 0, sizeof *t); t->kind = kind;
    return t;
}
// A shader key as the shader map names it (a Metal library name <key>_b loses its suffix), NUL-padded: the pair table's half
static void map_key(char out[24], const char *k) { memset(out, 0, 24); size_t n = strnlen(k, 24); if (n == 18 && !strncmp(k + 16, "_b", 2)) n = 16; memcpy(out, k, n < 24 ? n : 23); }
// The lists' lines as tasks, in order (warm_parse's rules)
static void parse(const char *const paths[], int npaths) {
    nvn_hmap *pairs = &g_pairs; nvn_hm_init(pairs, 48);
    nvn_hmap descs; nvn_hm_init(&descs, sizeof(nvn_pipe_desc));
    nvn_hmap lines; nvn_hm_init(&lines, 32);
    for (int p = 0; p < npaths; p++) {
        char *txt = read_text(paths[p]);
        if (!txt) { if (p) hostvk_log("[hostvk] pipeline prewarm: this device's list %s does not exist yet", paths[p]); else hostvk_log("[RED] [hostvk] pipeline prewarm: cannot read the list %s", paths[p]); continue; }
        unsigned long nl = 0;
        for (char *ln = txt, *next; ln && *ln; ln = next) {
            next = strchr(ln, '\n'); if (next) *next++ = 0;
            size_t l = strlen(ln); if (l && ln[l - 1] == '\r') ln[--l] = 0;
            if (!l || ln[0] == '#') continue;
            nl++;
            pthread_mutex_lock(&g_user_mu); seen_add(ln); pthread_mutex_unlock(&g_user_mu);
            uint8_t h[32]; plat_sha256(ln, l, h);
            if (nvn_hm_get(&lines, h)) continue;
            nvn_hm_put(&lines, h, (void *)1);
            atomic_fetch_add(&s_lines, 1);
            if (ln[0] == 'S' && ln[1] == '\t') {
                char *k = ln + 2, *tab = strchr(k, '\t'); if (tab) *tab = 0;
                task *t = task_add(T_SHADER); if (t) snprintf(t->vs, sizeof t->vs, "%s", k);
                atomic_fetch_add(&s_srows, 1); continue;
            }
            if (ln[0] == 'C' && ln[1] == '\t') {
                char *k = ln + 2, *tab = strchr(k, '\t'); if (tab) *tab = 0;
                task *t = task_add(T_COMPUTE); if (t) snprintf(t->vs, sizeof t->vs, "%s", k);
                atomic_fetch_add(&s_crows, 1); continue;
            }
            nvn_pipe_desc d; char why[160];
            int rc = nvn_pipe_parse_row(ln, &d, why, sizeof why);
            if (rc == 1) continue;
            if (rc) { atomic_fetch_add(&s_skipped, 1); if (rc == -1) HVK_RED_ONCE("pipeline prewarm: a malformed row is skipped (first case: %s)", why); continue; }
            if (nvn_hm_get(&descs, &d)) continue;   // the same pipeline in two forms (an R row and the P row of it)
            nvn_pipe_desc *box = malloc(sizeof *box); if (!box) continue; *box = d; nvn_hm_put(&descs, &d, box);
            char pk[48]; map_key(pk, d.vs.key); map_key(pk + 24, d.fs.key);
            uintptr_t ti = (uintptr_t)nvn_hm_get(pairs, pk);
            task *t;
            if (!ti) { if (!(t = task_add(T_PAIR))) continue; memcpy(t->vs, d.vs.key, 24); memcpy(t->fs, d.fs.key, 24); nvn_hm_put(pairs, pk, (void *)(uintptr_t)g_nt); }
            else t = &g_t[ti - 1];
            if (t->nd == t->cap) { int c = t->cap ? 2 * t->cap : 4; nvn_pipe_desc *nd = realloc(t->d, (size_t)c * sizeof *nd); int *nr = realloc(t->row, (size_t)c * sizeof *nr);
                if (nd) t->d = nd; if (nr) t->row = nr; if (!nd || !nr) continue; t->cap = c; }
            t->d[t->nd] = d; t->row[t->nd] = (int)atomic_fetch_add(&s_rows, 1); t->nd++;
        }
        hostvk_log("[hostvk] pipeline prewarm list %s: %lu lines", paths[p], nl);
        free(txt);
    }
    g_nrows = (int)atomic_load(&s_rows);
    if ((g_row_task = calloc((size_t)(g_nrows ? g_nrows : 1), sizeof *g_row_task)))
        for (int i = 0; i < g_nt; i++) for (int k = 0; k < g_t[i].nd; k++) g_row_task[g_t[i].row[k]] = i;
    for (uint32_t i = 0; i < descs.cap; i++) if (descs.vals && descs.vals[i]) free(descs.vals[i]);
    nvn_hm_clear(&descs, NULL); nvn_hm_clear(&lines, NULL);
}
// A row's pipeline key, as a draw with that state would key it (vk_commands.c key_src: the same hvk_key_fill)
static int row_key(const nvn_pipe_desc *d, hvk_shader *const sh[6], hvk_vin *v, hvk_gfx_key *k) {
    hvk_key_src s; memset(&s, 0, sizeof s);
    int nc = 0;
    for (int i = 0; i < 8; i++) { s.cfmt[i] = hvk_attach_format(d->cfmt[i], &s.ccls[i]); if (s.cfmt[i]) nc = i + 1; }
    s.nc = nc; s.dfmt = hvk_attach_format(d->dfmt, NULL); s.sfmt = hvk_attach_format(d->sfmt, NULL);
    s.samples = d->samples ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT;
    s.blend = d->blend; s.blend_en = d->blend_en; s.chmask = d->chmask; s.a2c = d->a2c;
    s.logic_op = d->xstate ? d->logic_op : NVN_LOGIC_OP_COPY; s.poly_mode = d->xstate ? d->poly_mode : NVN_POLYGON_MODE_FILL; s.depth_clamp = d->xstate ? d->depth_clamp : 0;
    hvk_vertex_input(sh[0], d->attr, d->stream, 0xffffu, v); s.vin = v;
    hvk_key_fill(k, sh, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &s);
    return 1;
}
static hvk_shader *task_shader(const char *key) {
    int stale = 0; hvk_shader *s = key[0] ? hvk_shader_by_key(key, &stale) : NULL;
    if (stale) atomic_fetch_add(&s_stale, 1);
    return s;
}
static void finished(void);
static void task_run(task *t) {
    if (t->kind == T_SHADER) {
        hvk_shader *s = task_shader(t->vs);
        if (s) { if (!hvk_pso_gpl() || hvk_pso_build_shader(s)) atomic_fetch_add(&s_built, 1); else atomic_fetch_add(&s_failed, 1); }
    } else if (t->kind == T_COMPUTE) {
        hvk_shader *s = task_shader(t->vs);
        if (s && s->stage == 5) {
            hvk_shader *sh[6] = {0}; sh[5] = s;
            hvk_layout *l = hvk_layout_for(sh);
            if (l && hvk_cs_pipeline(s, l)) atomic_fetch_add(&s_built, 1); else atomic_fetch_add(&s_failed, 1);
        } else if (s) atomic_fetch_add(&s_skipped, 1);
    } else {
        hvk_shader *vs = task_shader(t->vs), *fs = t->fs[0] ? task_shader(t->fs) : NULL;
        if (!vs || (t->fs[0] && !fs)) return;
        hvk_shader *sh[6] = {vs, fs, NULL, NULL, NULL, NULL};
        hvk_layout *l = hvk_layout_for(sh);
        for (int i = 0; i < t->nd; i++) {
            hvk_vin v; hvk_gfx_key k;
            if (!l || !row_key(&t->d[i], sh, &v, &k)) { atomic_fetch_add(&s_failed, 1); continue; }
            if (hvk_pso_build(sh, l, &k, t->row[i])) atomic_fetch_add(&s_built, 1); else atomic_fetch_add(&s_failed, 1);
        }
    }
}
// A task runs once, on the worker that claims it: its job in list order, or a prediction's ahead of it; that worker counts it done
static void job(void *arg) {
    task *t = arg;
    if (atomic_exchange(&t->claimed, 1)) return;
    task_run(t);
    if (atomic_fetch_sub(&g_left, 1) == 1) finished();
}
static void spec_job(void *arg) {
    task *t = arg;
    if (atomic_exchange(&t->claimed, 1)) return;
    atomic_fetch_add(&s_spec, 1); task_run(t);
    if (atomic_fetch_sub(&g_left, 1) == 1) finished();
}
static void map_shader(const char *key, int stage, void *u) {   // the prebuild: a library for every vertex and pixel shader of the map
    (void)u;
    if (stage != 0 && stage != 1) return;
    task *t = task_add(T_SHADER); if (t) snprintf(t->vs, sizeof t->vs, "%s", key);
}
static void finished(void) {
    g_t1 = plat_now_ns();
    atomic_store(&g_running, 0);
    hostvk_log("[hostvk] pipeline prewarm done: %llu lines (%llu render pipeline rows, %llu shader rows, %llu compute rows), %llu built, %llu failed, %llu skipped, %llu built ahead by a prediction, in %.2f s; "
               "shader keys not in the shader map (another game version) %llu",
               (unsigned long long)s_lines, (unsigned long long)s_rows, (unsigned long long)s_srows, (unsigned long long)s_crows, (unsigned long long)s_built, (unsigned long long)s_failed,
               (unsigned long long)s_skipped, (unsigned long long)s_spec, (g_t1 - g_t0) / 1e9, (unsigned long long)s_stale);
    if (!g_prebuild) hvk_cache_flush("after the prewarm");
}
static int list_paths(const char *paths[2], char *seed, size_t n) {
    const char *e = getenv("DM_PSO_MANIFEST");
    if (e && !strcmp(e, "off")) { hostvk_log("[hostvk] pipeline prewarm off (DM_PSO_MANIFEST=off)"); return 0; }
    if (e && *e) { paths[0] = e; return 1; }
    const char *map = hvk_shader_map_path();
    int np = 0;
    if (map) { char dir[900]; parent_of(map, dir, sizeof dir); snprintf(seed, n, "%s/pso_manifest.tsv", dir); paths[np++] = seed; }
    else hostvk_log("[RED] [hostvk] pipeline prewarm: no shader map (DM_NVN_SHMAP), so no seed list beside it");
    const char *u = user_path();
    if (u) paths[np++] = u; else hostvk_log("[hostvk] pipeline prewarm: this device's list has no path (neither DM_VK_PSO_USER nor DM_SAVE is set)");
    return np;
}
static const char *g_paths[2]; static int g_npaths; static char g_seed[1024];
static void queue_all(void) {
    atomic_store(&g_left, g_nt);
    atomic_store(&g_ready, 1);
    if (!g_nt) { finished(); return; }
    for (int i = 0; i < g_nt; i++) hvk_work_submit(HVK_Q_BG, job, &g_t[i]);
}
static void parse_job(void *arg) { (void)arg; parse(g_paths, g_npaths); queue_all(); }
extern void front_progress(const char *title, long done, long total) __attribute__((weak));   // the front end's (dm/host/front.h); none in a unit test that does not define it
void hvk_prewarm_start(void) {
    if (g_started++) return;
    const char *e = getenv("DM_VK_PREBUILD"); g_prebuild = e && !strcmp(e, "1");
    g_npaths = list_paths(g_paths, g_seed, sizeof g_seed);
    g_t0 = plat_now_ns(); atomic_store(&g_running, 1);
    if (!g_prebuild) {
        if (!g_npaths) { atomic_store(&g_ready, 1); finished(); return; }
        hvk_work_submit(HVK_Q_BG, parse_job, NULL);
        return;
    }
    // The prepare step: everything before the first frame
    parse(g_paths, g_npaths);
    long rows_tasks = g_nt;
    int nmap = hvk_pso_gpl() ? hvk_shader_map_each(map_shader, NULL) : 0;
    int cpu = plat_cpu_count(), want = cpu > 1 ? cpu - 1 : 1;
    int made = hvk_work_prepare_begin(want);
    long total = g_nt;
    queue_all();
    int shown = 0;
    while (atomic_load(&g_left) > 0) {
        plat_sleep_ns(20000000ull);
        uint64_t el = plat_now_ns() - g_t0; long done = total - atomic_load(&g_left);
        double rest = done ? (double)el * (double)(total - done) / (double)done : 1e18;
        if (!shown && el >= g_show_after && (rest >= 3e8 || g_show_after == 0)) { shown = 1; hostvk_log("[hostvk] pipeline prepare: %ld of %ld tasks done after %.1f s, the progress screen shows", done, total, el / 1e9); }
        if (shown && front_progress) front_progress("Preparing shaders", done, total);
    }
    hvk_work_drain(HVK_Q_BG);
    hvk_work_prepare_end();
    if (shown && front_progress) front_progress(NULL, total, total);
    hostvk_log("[hostvk] pipeline prepare (DM_VK_PREBUILD=1): %ld tasks (%ld from the lists, %d shaders of the map) in %.2f s on %d workers (%d background) before the first frame, progress screen %s",
               total, rows_tasks, nmap, (plat_now_ns() - g_t0) / 1e9, made + (cpu <= 4 ? 1 : 2), cpu <= 4 ? 1 : 2, shown ? "shown" : "not shown");
    hvk_cache_flush("after the prepare step");
}
// The prediction: a task ahead of the list order, unless a worker has it or two speculative builds are outstanding (the Metal backend's budget)
static int spec_task(int ti) {
    task *t = &g_t[ti];
    if (atomic_load(&t->claimed)) return 1;
    if (hvk_work_pending(HVK_Q_SPEC) >= 2) return 0;
    hvk_work_submit(HVK_Q_SPEC, spec_job, t);
    return 1;
}
void hvk_prewarm_predict(int row) {   // a row's pipeline was first drawn: the tasks of the rows recorded after it
    if (!atomic_load(&g_ready) || row < 0 || row >= g_nrows || !g_row_task) return;
    int cur = g_row_task[row];
    for (int r = row + 1, n = 0; r < g_nrows && n < 2; r++) {
        int ti = g_row_task[r];
        if (ti == cur) continue;   // the same pair's task
        cur = ti; n++;
        if (!spec_task(ti)) return;
    }
}
void hvk_prewarm_predict_pair(const char *vs, const char *fs) {   // a miss: its pair's rows (other formats of the same shaders), then the task after it
    if (!atomic_load(&g_ready) || !vs || !g_pairs.n) return;
    char pk[48]; map_key(pk, vs); map_key(pk + 24, fs ? fs : "");
    uintptr_t ti = (uintptr_t)nvn_hm_get(&g_pairs, pk);
    if (!ti) return;
    if (spec_task((int)ti - 1) && (int)ti < g_nt) spec_task((int)ti);
}
void hvk_prewarm_user_row(const nvn_pipe_desc *d) {
    static _Atomic int noted;
    if (!atomic_exchange(&noted, 1))
        hostvk_log("[RED] [hostvk] pipeline prediction: the look-ahead over the rest of the submission after a miss (design 3.3, nvn_mtl.m pso_predict_rest_) is not done: it needs the frontend's state save over live command lists; misses build on demand");
    char line[NVN_PIPE_ROW_MAX];
    if (nvn_pipe_write_row(d, line, sizeof line) < 0) return;
    pthread_mutex_lock(&g_user_mu);
    uint8_t h[32]; plat_sha256(line, strlen(line), h);
    if (!g_seen.klen) nvn_hm_init(&g_seen, 32);
    if (nvn_hm_get(&g_seen, h)) { pthread_mutex_unlock(&g_user_mu); return; }
    nvn_hm_put(&g_seen, h, (void *)1);
    if (!g_user_tried) {
        g_user_tried = 1;
        const char *p = user_path();
        if (p) {
            FILE *probe = fopen(p, "rb"); int fresh = !probe; if (probe) fclose(probe);
            if ((g_user = fopen(p, "ab")) && fresh) fprintf(g_user, "# pso_user_vk.tsv: the render pipelines this device's Vulkan backend built on the spot, as P rows (dm/gfx/nvn/nvn_pipe.h), prewarmed after the seed list at the next start\n");
            if (!g_user) hostvk_log("[RED] [hostvk] pipeline prewarm: cannot append to this device's list %s", p);
        }
    }
    if (g_user) { fprintf(g_user, "%s\n", line); fflush(g_user); atomic_fetch_add(&s_user, 1); }
    pthread_mutex_unlock(&g_user_mu);
}
int hvk_prewarm_done(void) { return g_started && atomic_load(&g_ready) && atomic_load(&g_left) <= 0; }
void hvk_prewarm_stats_get(hvk_prewarm_stats *s) {
    memset(s, 0, sizeof *s);
    s->lines = s_lines; s->rows = s_rows; s->shader_rows = s_srows; s->compute_rows = s_crows; s->stale = s_stale; s->skipped = s_skipped; s->built = s_built; s->failed = s_failed;
    s->speculative = s_spec; s->user_rows = s_user; s->prebuild = g_prebuild; s->running = atomic_load(&g_running);
    s->seconds = g_started ? ((atomic_load(&g_running) ? plat_now_ns() : g_t1) - g_t0) / 1e9 : 0;
}
void hvk_prewarm_shutdown(void) {   // the workers have stopped (hvk_pso_shutdown)
    for (int i = 0; i < g_nt; i++) { free(g_t[i].d); free(g_t[i].row); }
    free(g_t); g_t = NULL; g_nt = 0; free(g_row_task); g_row_task = NULL; g_nrows = 0; atomic_store(&g_ready, 0);
    nvn_hm_clear(&g_pairs, NULL); memset(&g_pairs, 0, sizeof g_pairs);
    pthread_mutex_lock(&g_user_mu);
    if (g_user) fclose(g_user);
    g_user = NULL; g_user_tried = 0; nvn_hm_clear(&g_seen, NULL); memset(&g_seen, 0, sizeof g_seen);
    pthread_mutex_unlock(&g_user_mu);
}
