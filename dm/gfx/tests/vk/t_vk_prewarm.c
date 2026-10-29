// The prewarm lists, the prediction from their rows and the prebuild (docs/design/vulkan-backend.md 3.3, 3.5; phase P5). A synthetic seed list of 206 lines, written beside the
// shader map before the first submission from nvn_pipe_desc values (P rows through nvn_pipe_write_row, R rows in the Metal backend's form through nvn_pipe_to_key, S rows): 48
// shader pairs (eight vertex and six fragment shaders under keys of their own over two SPIR-V modules) times 4 attachment combinations (RGBA8 alone, RGBA16F with a 32-bit float
// depth, R32F with a depth-stencil attachment, RGB10A2 alone), 192 render pipeline rows, and the 14 shaders' S rows. Each phase is a new start of the backend, a child process
// (t_vk_harness.h hv_spawn):
//   background  the default: the prewarm runs on the background workers; once it is done, a draw of every listed pipeline waits 0 times, and 6 draws of pipelines outside the
//               list wait once each (then 0 times when drawn again); the backend appends their P rows to the device's list
//   warm        the same with the pipeline cache and the device's list the first start wrote: the list's 6 new rows are prewarmed too (0 waits for them now)
//   predict     the background prewarm with draws started at once, from the 41st pair on: a miss builds its pair's other rows and the next pair's ahead of the list order (counted:
//               on a fast machine the workers may be through the list before the draws come)
//   prebuild    DM_VK_PREBUILD=1: the first submission returns only when every row is built, with the progress reported to front_progress (counted here); its draws wait 0 times
//   level_d     DM_VK_LEVEL=D (the device made as a level-D one): the rows' monolithic pipelines with the blend, raster and vertex state baked, 0 waits for them
// The parent prints the numbers of each start: rows, libraries, pipelines, seconds on lavapipe (whose compile times are LLVM's on the CPU, not a GPU driver's)
#include "t_vk_harness.h"
#include "vk_pso.h"
#include "shaders/gen/gpl_vary_vert.h"
#include "shaders/gen/gpl_mix_frag.h"

enum { NVS = 8, NFS = 6, NPAIR = NVS * NFS, NCOMBO = 4, NCF = 4, NDF = 3, NOUT = 6, NROWS = NPAIR * NCOMBO };
static const int k_cf[NCF] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA16F, NVN_FORMAT_R32F, NVN_FORMAT_RGB10A2};
static const int k_df[NDF] = {0, NVN_FORMAT_DEPTH32F, NVN_FORMAT_DEPTH24_STENCIL8};
static const int k_combo[NCOMBO][2] = {{0, 0}, {1, 1}, {2, 2}, {3, 0}};   // colour and depth slots of each combination
static const int k_out[NOUT] = {NVN_FORMAT_RG16F, NVN_FORMAT_R16F, NVN_FORMAT_RG8, NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA16, NVN_FORMAT_BGRA8};   // not in the list
typedef struct { float x, y, z, w; } v4;
static const v4 k_pos[3] = {{-0.9f, -0.9f, 0.3f, 1}, {0.8f, -0.7f, 0.5f, 1}, {-0.6f, 0.9f, 0.7f, 1}};
static const uint8_t k_col[3][4] = {{255, 0, 0, 200}, {0, 255, 0, 128}, {0, 0, 255, 255}};
static NVNqueue queue; static hv_cmds cmds; static hv_pool mp; static NVNprogram prog[NVS][NFS]; static char vkey[NVS][24], fkey[NFS][24]; static uint8_t *vb0, *vb1;
static tex_t tcol[NCF + NOUT], tdep[NDF];
static int g_progress; static long g_progress_last, g_progress_total;
void front_progress(const char *title, long done, long total);
void front_progress(const char *title, long done, long total) { if (title) { g_progress++; g_progress_last = done; } g_progress_total = total; }   // the front end's progress hook, counted

static void setup(void) {
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    if (!hv_pool_make(&mp, 1 << 20)) { t("memory pool allocated", 0); exit(hv_finish()); }
    cmds = (hv_cmds){mp.mem + 524288, 0, 262144};
    vb0 = mp.mem + 786432; vb1 = vb0 + 4096; memcpy(vb0, k_pos, sizeof k_pos); memcpy(vb1, k_col, sizeof k_col);
    static const char vin[] = "[{\"location\":0,\"type\":\"float\"},{\"location\":1,\"type\":\"float\"}]";
    char vn[NVS][16], fn[NFS][16];
    for (int v = 0; v < NVS; v++) for (int f = 0; f < NFS; f++) {   // one program per pair; the shaders' keys come from their names, so every start has the same ones
        snprintf(vn[v], sizeof vn[v], "pw_vs%d", v); snprintf(fn[f], sizeof fn[f], "pw_fs%d", f);
        const hv_shader s[2] = {{vn[v], 0, HV_SPV(gpl_vary_vert_spv), NULL, vin}, {fn[f], 1, HV_SPV(gpl_mix_frag_spv), NULL, NULL}};
        hv_program(&prog[v][f], &mp, (size_t)(v * NFS + f) * 1024, s, 2);
        uint8_t md[8];
        prog_key((const prog_t *)&prog[v][f], 0, md); for (int b = 0; b < 8; b++) snprintf(vkey[v] + 2 * b, 3, "%02x", md[b]);
        prog_key((const prog_t *)&prog[v][f], 1, md); for (int b = 0; b < 8; b++) snprintf(fkey[f] + 2 * b, 3, "%02x", md[b]);
    }
    for (int i = 0; i < NCF + NOUT; i++) tcol[i] = hv_tex(NVN_TEXTURE_TARGET_2D, i < NCF ? k_cf[i] : k_out[i - NCF], 16, 16, 1, 1, 0);
    for (int i = 1; i < NDF; i++) tdep[i] = hv_tex(NVN_TEXTURE_TARGET_2D, k_df[i], 16, 16, 1, 1, 0);
}
// A row's description: the pair, the formats, and the draw state the draws below have (S after nvn_state_reset and the vertex input they bind)
static void desc_of(nvn_pipe_desc *d, int v, int f, int cf, int df) {
    nvn_draw_state st; memset(&st, 0, sizeof st);
    st.color.logic_op = NVN_LOGIC_OP_COPY; st.chmask.mask = 0xffffffffu; st.poly.mode = NVN_POLYGON_MODE_FILL; st.poly.front = NVN_FRONT_FACE_CCW;
    st.attr[0].fmt = NVN_FORMAT_RGBA32F; st.attr[0].stream = 0; st.attr[1].fmt = NVN_FORMAT_RGBA8; st.attr[1].stream = 1; st.nattr = 2;
    st.stream[0] = (vs_st){16, 0}; st.stream[1] = (vs_st){4, 0}; st.nstream = 2;
    uint16_t c[8] = {(uint16_t)cf}; uint16_t ds = (uint16_t)df;
    nvn_pipe_from_state(d, &st, vkey[v], 0, 3, fkey[f], 1, c, ds, df == NVN_FORMAT_DEPTH24_STENCIL8 ? ds : 0, 1);
}
static int write_manifest(const char *path) {   // 206 lines: 14 S rows, 48 R rows (RGBA8, no depth), 144 P rows
    FILE *m = fopen(path, "w"); if (!m) return 0;
    fprintf(m, "# t_vk_prewarm: a synthetic seed list\n");
    int n = 0;
    for (int v = 0; v < NVS; v++) { fprintf(m, "S\t%s_b\t0\n", vkey[v]); n++; }   // the Metal library names, as the profiles' lists have them
    for (int f = 0; f < NFS; f++) { fprintf(m, "S\t%s_b\t1\n", fkey[f]); n++; }
    for (int c = 0; c < NCOMBO; c++) for (int v = 0; v < NVS; v++) for (int f = 0; f < NFS; f++) {
        int cf = k_combo[c][0], df = k_combo[c][1];
        nvn_pipe_desc d; desc_of(&d, v, f, k_cf[cf], k_df[df]);
        if (c == 0) {   // an R row: the Metal key's hex (its formats Metal's; nvn_pipe_parse_row decodes them back)
            pso_skey k; char hex[2 * sizeof(pso_skey) + 1];
            if (nvn_pipe_to_key(&d, &k)) { fclose(m); return 0; }
            pso_skey_hex(&k, hex); fprintf(m, "R\t%s\n", hex);
        } else { char row[NVN_PIPE_ROW_MAX]; if (nvn_pipe_write_row(&d, row, sizeof row) < 0) { fclose(m); return 0; } fprintf(m, "%s\n", row); }
        n++;
    }
    fclose(m);
    return n;
}
// Draws of pair (v, f) into colour format slot ci and depth slot di; the state the rows describe
static void target(hv_cmds *c, int ci, int di) {
    const tex_t *col[1] = {&tcol[ci]}; hv_rt(c, 1, col, di ? &tdep[di] : NULL); hv_scissor(c, 0, 0, 16, 16);
    va_st a[2]; memset(a, 0, sizeof a); a[0].fmt = NVN_FORMAT_RGBA32F; a[0].stream = 0; a[1].fmt = NVN_FORMAT_RGBA8; a[1].stream = 1;
    vs_st s[2] = {{16, 0}, {4, 0}};
    hv_attrs(c, 2, a); hv_streams(c, 2, s); hv_vbuf(c, 0, vb0, sizeof k_pos); hv_vbuf(c, 1, vb1, sizeof k_col);
}
static void draw_pair(hv_cmds *c, int v, int f) { hv_bind(c, &prog[v][f], 3); hv_draw(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); }
static uint64_t waits(void) { hvk_pso_stats s; hvk_pso_stats_get(&s); uint64_t n = 0; for (int i = 0; i < HVK_W_N; i++) n += s.wait_n[i]; return n; }
static void wait_prewarm(void) { uint64_t t0 = plat_now_ns(); while (!hvk_prewarm_done() && plat_now_ns() - t0 < 60000000000ull) plat_sleep_ns(2000000); hvk_work_drain(HVK_Q_BG); }
static void draw_listed(int from) {   // every listed pipeline once, pair by pair from pair `from` on (then the ones before it), a submission per pair
    for (int k = 0; k < NPAIR; k++) {
        int pi = (from + k) % NPAIR;
        for (int c = 0; c < NCOMBO; c++) { target(&cmds, k_combo[c][0], k_combo[c][1]); draw_pair(&cmds, pi / NFS, pi % NFS); }
        hv_submit(&queue, &cmds);
    }
}
static void draw_outside(void) { for (int i = 0; i < NOUT; i++) { target(&cmds, NCF + i, 0); draw_pair(&cmds, 0, 0); } hv_submit(&queue, &cmds); }

static int child(const char *phase, const char *res) {
    static char tname[80]; snprintf(tname, sizeof tname, "t_vk_prewarm %s", phase);
    char name[300];
    if (!strcmp(phase, "prebuild")) setenv("DM_VK_PREBUILD", "1", 1);
    if (!strcmp(phase, "level_d")) setenv("DM_VK_LEVEL", "D", 1);
    hv_up(tname);
    hvk_prewarm_test_show_after(0);   // the prepare step's progress shows at once (0.5 s otherwise), so its calls can be counted on a fast machine
    setup();
    char seed[700]; snprintf(seed, sizeof seed, "%s/pso_manifest.tsv", g_lib);   // the profile's seed list: beside the shader map (DM_NVN_SHMAP), as a game's package has it
    int lines = write_manifest(seed);
    t("the synthetic manifest is written", lines == NROWS + NVS + NFS);
    uint64_t t0 = plat_now_ns();
    // the first submission starts the prewarm (and, with the prebuild, finishes it)
    target(&cmds, 0, 0); hv_clear_color(&cmds, 0, 15, 0, 0, 0, 0); hv_submit(&queue, &cmds);
    hvk_prewarm_stats pw; hvk_pso_stats ps; hvk_cache_stats cs;
    if (!strcmp(phase, "prebuild")) {
        hvk_prewarm_stats_get(&pw);
        snprintf(name, sizeof name, "prebuild: every row was built when the first submission began (%llu of %llu built)", (unsigned long long)pw.built, (unsigned long long)(pw.rows + pw.shader_rows));
        t(name, hvk_prewarm_done() && pw.built >= pw.rows + pw.shader_rows && pw.failed == 0);
        snprintf(name, sizeof name, "prebuild: the progress went to front_progress (%d calls, last %ld of %ld)", g_progress, g_progress_last, g_progress_total);
        t(name, g_progress > 0 && g_progress_total > 0);
    } else if (strcmp(phase, "predict")) wait_prewarm();
    double prewarm_s = (plat_now_ns() - t0) / 1e9;
    hvk_prewarm_stats_get(&pw); hvk_pso_stats_get(&ps);
    uint64_t libs = ps.lib[0] + ps.lib[1] + ps.lib[2] + ps.lib[3], pipes = ps.fast + ps.mono, lib_ns = ps.lib_ns[0] + ps.lib_ns[1] + ps.lib_ns[2] + ps.lib_ns[3];
    uint64_t w0 = waits();
    draw_listed(!strcmp(phase, "predict") ? 40 : 0);
    uint64_t w1 = waits();
    hvk_prewarm_stats_get(&pw);
    if (strcmp(phase, "predict")) {
        snprintf(name, sizeof name, "%s: the %d draws of listed pipelines waited %llu times", phase, NROWS, (unsigned long long)(w1 - w0)); t(name, w1 == w0);
    } else printf("predict: the draws (from the 41st pair on, the prewarm still running) waited %llu times; %llu tasks were built ahead of the list order by a prediction\n",
                  (unsigned long long)(w1 - w0), (unsigned long long)pw.speculative);
    if (!strcmp(phase, "background")) {   // a pass with an empty slot after its attachment: keyed by what it attaches, so the row of RGBA8 alone serves it
        const tex_t *col[2] = {&tcol[0], NULL}; hv_rt(&cmds, 2, col, NULL); draw_pair(&cmds, 0, 0); hv_submit(&queue, &cmds);
        uint64_t w = waits(); t("background: a pass with an empty colour slot after its attachment draws a listed pipeline without a wait", w == w1);
    }
    int outside = !strcmp(phase, "background") || !strcmp(phase, "level_d");
    uint64_t w2 = w1, w3 = w1;
    if (outside) {
        draw_outside(); w2 = waits(); draw_outside(); w3 = waits();
        snprintf(name, sizeof name, "%s: the %d draws of pipelines outside the list waited once each (%llu), and not when drawn again (%llu)", phase, NOUT, (unsigned long long)(w2 - w1), (unsigned long long)(w3 - w2));
        t(name, w2 - w1 == NOUT && w3 == w2);
    }
    if (!strcmp(phase, "warm")) { snprintf(name, sizeof name, "warm: the device's list from the first start was prewarmed too (%llu render rows read)", (unsigned long long)pw.rows); t(name, pw.rows == NROWS + NOUT); draw_outside(); w2 = waits(); t("warm: the pipelines the first start built on the spot did not wait now", w2 == w1); }
    hvk_pso_stats_get(&ps); hvk_cache_stats_get(&cs);
    libs = ps.lib[0] + ps.lib[1] + ps.lib[2] + ps.lib[3]; pipes = ps.fast + ps.mono; lib_ns = ps.lib_ns[0] + ps.lib_ns[1] + ps.lib_ns[2] + ps.lib_ns[3];   // made in all, by the end
    printf("%s: the longest wait of a draw %.3f ms, %llu over 2 ms\n", phase, ps.wait_max_ns / 1e6, (unsigned long long)ps.over_budget);
    printf("%s: %llu lines (%llu render pipeline rows, %llu shader rows), %llu built, %llu failed, %llu shader keys stale; %llu libraries (%.2f ms), %llu pipelines (%llu fast links %.3f ms, %llu monolithic %.2f ms), "
           "prewarm %.3f s; cache: %llu bytes read, %llu creations, %llu driver cache hits; user rows appended %llu\n",
           phase, (unsigned long long)pw.lines, (unsigned long long)pw.rows, (unsigned long long)pw.shader_rows, (unsigned long long)pw.built, (unsigned long long)pw.failed, (unsigned long long)pw.stale,
           (unsigned long long)libs, lib_ns / 1e6, (unsigned long long)pipes, (unsigned long long)ps.fast, ps.fast_ns / 1e6, (unsigned long long)ps.mono, ps.mono_ns / 1e6, pw.seconds,
           (unsigned long long)cs.loaded, (unsigned long long)cs.created, (unsigned long long)cs.hits, (unsigned long long)pw.user_rows);
    hv_res_put(res, "rows", (double)(pw.rows + pw.shader_rows)); hv_res_put(res, "libraries", (double)libs); hv_res_put(res, "pipelines", (double)pipes);
    hv_res_put(res, "seconds", pw.seconds); hv_res_put(res, "prewarm_wall", prewarm_s); hv_res_put(res, "listed_waits", (double)(w1 - w0)); hv_res_put(res, "spec", (double)pw.speculative);
    hv_res_put(res, "lib_ms", lib_ns / 1e6); hv_res_put(res, "fast_ms", ps.fast_ns / 1e6); hv_res_put(res, "mono_ms", ps.mono_ns / 1e6); hv_res_put(res, "progress", g_progress);
    hv_res_put(res, "built", (double)pw.built); hv_res_put(res, "max_wait_ms", ps.wait_max_ns / 1e6);
    hv_pool_free(&mp);
    return hv_finish();
}

int main(int argc, char **argv) {
    char dir[600], res[700];
    const char *tmp = getenv("TMPDIR");
    if (argc > 1) { snprintf(res, sizeof res, "%s/%s.res", getenv("T_VK_PREWARM_DIR") ? getenv("T_VK_PREWARM_DIR") : ".", argv[1]); return child(argv[1], res); }
    // The parent: a folder for the starts' manifest, caches, device lists and results
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(dir, sizeof dir, "%s/t_vk_prewarm_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    char cache1[700], cache2[700], user[700];
    snprintf(cache1, sizeof cache1, "%s/cache", dir); snprintf(cache2, sizeof cache2, "%s/cache_cold", dir); snprintf(user, sizeof user, "%s/pso_user_vk.tsv", dir);
    setenv("DM_PSO_MANIFEST", "", 1);   // set but empty: the lists a game's start reads (the seed beside the shader map, then the device's list), not the harness's "off"
    setenv("T_VK_PREWARM_DIR", dir, 1);
    static const char *const phases[] = {"background", "warm", "predict", "prebuild", "level_d"};
    int fail = 0, skipped = 0;
    for (size_t i = 0; i < sizeof phases / sizeof *phases; i++) {
        const char *ph = phases[i];
        // background and warm share the cache and the device list (warm reads both: the list after the seed); the others start cold, with no device list
        setenv("DM_VK_PSO_CACHE", !strcmp(ph, "background") || !strcmp(ph, "warm") ? cache1 : cache2, 1);
        if (!strcmp(ph, "background") || !strcmp(ph, "warm")) setenv("DM_VK_PSO_USER", user, 1); else unsetenv("DM_VK_PSO_USER");
        printf("---- start %zu: %s ----\n", i + 1, ph);
        if (!strcmp(ph, "warm") && !fail) {   // what the first start appended to the device's list
            FILE *u = fopen(user, "r"); char line[NVN_PIPE_ROW_MAX + 8]; int n = 0;
            if (u) { while (fgets(line, sizeof line, u)) n += line[0] == 'P'; fclose(u); }
            printf("the device's list of the first start holds %d P rows\n", n);
            if (n != NOUT) { printf("the device's list should hold the %d pipelines the first start built on the spot\n", NOUT); fail = 1; }
        }
        int rc = hv_spawn(argv[0], ph);
        char r[800]; snprintf(r, sizeof r, "%s/%s.res", dir, ph);
        if (rc) { printf("start %s failed (exit status %d)\n", ph, rc); fail = 1; }
        else if (hv_res_get(r, "rows", -1) < 0) { skipped++; break; }   // the child skipped (no Vulkan device): so would the others
    }
    printf("---- the starts (lavapipe: compile times are LLVM's on the CPU) ----\n%-12s %5s %6s %9s %9s %10s %12s %9s %8s %8s %8s %9s\n", "start", "rows", "built", "libraries",
           "pipelines", "prewarm s", "listed waits", "predicted", "lib ms", "link ms", "mono ms", "max wait");
    for (size_t i = 0; i < sizeof phases / sizeof *phases; i++) {
        char r[800]; snprintf(r, sizeof r, "%s/%s.res", dir, phases[i]);
        printf("%-12s %5.0f %6.0f %9.0f %9.0f %10.3f %12.0f %9.0f %8.2f %8.2f %8.2f %9.3f\n", phases[i], hv_res_get(r, "rows", 0), hv_res_get(r, "built", 0), hv_res_get(r, "libraries", 0),
               hv_res_get(r, "pipelines", 0), hv_res_get(r, "seconds", 0), hv_res_get(r, "listed_waits", 0), hv_res_get(r, "spec", 0), hv_res_get(r, "lib_ms", 0), hv_res_get(r, "fast_ms", 0),
               hv_res_get(r, "mono_ms", 0), hv_res_get(r, "max_wait_ms", 0));
    }
    // the folder: its files and the two cache folders
    rm_dir(cache1); rm_dir(cache2); rm_dir(dir);
    printf("== t_vk_prewarm: %d starts, %s ==\n", (int)(sizeof phases / sizeof *phases), fail ? "a start failed" : skipped ? "skipped (no Vulkan device)" : "all passed");
    return fail;
}
