// The pipeline cache on disk (docs/design/vulkan-backend.md 3.4; phase P5), over four starts of the backend, each a child process (t_vk_harness.h hv_spawn) on one cache folder and
// one device list:
//   write    a first start: 12 draws of 12 pipelines (two shader pairs, six colour formats), each waits once; at the end the cache file is written (its framing, length and
//            SHA-256 are checked by the parent) and the device's list holds the 12 pipelines as P rows
//   reload   a second start, after the parent planted a file of the same device under another chain version and a file of another device: the first is deleted at start,
//            the second kept; the file's driver data is read; the prewarm builds the 12 pipelines from the device's list, so the same 12 draws make no pipeline and wait 0 times;
//            the creations and the creations the driver answered from its cache are counted (lavapipe keeps none: its cache data is the 32-byte header)
//   corrupt  the file's data changed by the parent: exactly one red line about the cache, the file ignored, the draws still right, and a valid file written at the end
//   invalid  the file renamed by the parent as a previous version of the backend would have named it (another key): deleted at start, no data read, a new file written
#include "t_vk_harness.h"
#include "vk_pso.h"
#include "shaders/gen/gpl_vary_vert.h"
#include "shaders/gen/gpl_mix_frag.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/red_frag.h"

enum { NF = 6, NP = 2, NK = NF * NP };
static const int k_fmt[NF] = {NVN_FORMAT_RGBA8, NVN_FORMAT_RGBA16F, NVN_FORMAT_R32F, NVN_FORMAT_RGB10A2, NVN_FORMAT_RG16F, NVN_FORMAT_RGBA32F};
typedef struct { float x, y, z, w; } v4;
static const v4 k_pos[3] = {{-0.9f, -0.9f, 0.3f, 1}, {0.8f, -0.7f, 0.5f, 1}, {-0.6f, 0.9f, 0.7f, 1}};
static const uint8_t k_col[3][4] = {{255, 0, 0, 200}, {0, 255, 0, 128}, {0, 0, 255, 255}};
static int g_red_cache, g_lines;
static void on_log(const char *line) {   // the backend's log lines, counted: the red ones about the pipeline cache
    fprintf(stderr, "[dm] %s\n", line);
    g_lines++;
    if (strstr(line, "[RED]") && strstr(line, "pipeline cache")) g_red_cache++;
}
static uint64_t waits(void) { hvk_pso_stats s; hvk_pso_stats_get(&s); uint64_t n = 0; for (int i = 0; i < HVK_W_N; i++) n += s.wait_n[i]; return n; }

static int child(const char *phase, const char *res) {
    static char name[120]; snprintf(name, sizeof name, "t_vk_pipeline_cache %s", phase);
    hostvk_set_log(on_log);
    hv_up(name);
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds cmds = {mp.mem + 65536, 0, 65536};
    uint8_t *vb0 = mp.mem + 131072, *vb1 = vb0 + 4096; memcpy(vb0, k_pos, sizeof k_pos); memcpy(vb1, k_col, sizeof k_col);
    static const char vin[] = "[{\"location\":0,\"type\":\"float\"},{\"location\":1,\"type\":\"float\"}]";
    NVNprogram prog[NP];
    const hv_shader s0[2] = {{"pc_vary", 0, HV_SPV(gpl_vary_vert_spv), NULL, vin}, {"pc_mix", 1, HV_SPV(gpl_mix_frag_spv), NULL, NULL}};
    const hv_shader s1[2] = {{"pc_full", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"pc_red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog[0], &mp, 0, s0, 2); hv_program(&prog[1], &mp, 1024, s1, 2);
    char seed[700]; snprintf(seed, sizeof seed, "%s/pso_manifest.tsv", g_lib);   // an empty seed list beside the shader map: only the device's list is prewarmed
    FILE *f = fopen(seed, "w"); if (f) { fprintf(f, "# t_vk_pipeline_cache: no seed rows\n"); fclose(f); }
    tex_t tx[NF]; for (int i = 0; i < NF; i++) tx[i] = hv_tex(NVN_TEXTURE_TARGET_2D, k_fmt[i], 16, 16, 1, 1, 0);
    hvk_cache_stats cs0; hvk_cache_stats_get(&cs0);
    char check[300];
    // the first submission starts the prewarm (the device's list); wait for it, then draw
    const tex_t *rt0[1] = {&tx[0]}; hv_rt(&cmds, 1, rt0, NULL); hv_clear_color(&cmds, 0, 15, 0, 0, 0, 0); hv_submit(&queue, &cmds);
    uint64_t t0 = plat_now_ns(); while (!hvk_prewarm_done() && plat_now_ns() - t0 < 30000000000ull) plat_sleep_ns(2000000);
    hvk_work_drain(HVK_Q_BG);
    hvk_pso_stats ps0; hvk_pso_stats_get(&ps0); hvk_cache_stats csp; hvk_cache_stats_get(&csp);
    uint64_t w0 = waits();
    for (int i = 0; i < NF; i++) {
        const tex_t *rt[1] = {&tx[i]}; hv_rt(&cmds, 1, rt, NULL); hv_scissor(&cmds, 0, 0, 16, 16);
        va_st a[2]; memset(a, 0, sizeof a); a[0].fmt = NVN_FORMAT_RGBA32F; a[1].fmt = NVN_FORMAT_RGBA8; a[1].stream = 1; vs_st st[2] = {{16, 0}, {4, 0}};
        hv_attrs(&cmds, 2, a); hv_streams(&cmds, 2, st); hv_vbuf(&cmds, 0, vb0, sizeof k_pos); hv_vbuf(&cmds, 1, vb1, sizeof k_col);
        for (int p = 0; p < NP; p++) { hv_bind(&cmds, &prog[p], 3); hv_draw(&cmds, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); }
    }
    hv_submit(&queue, &cmds);
    uint64_t w1 = waits();
    hvk_pso_stats ps1; hvk_pso_stats_get(&ps1); hvk_prewarm_stats pw; hvk_prewarm_stats_get(&pw); hvk_cache_stats cs1; hvk_cache_stats_get(&cs1);
    uint64_t made = ps1.draw_made - ps0.draw_made;
    int first = !strcmp(phase, "write");
    snprintf(check, sizeof check, "%s: the %d draws waited %llu times and made %llu pipelines (%s)", phase, NK, (unsigned long long)(w1 - w0), (unsigned long long)made,
             first ? "a first start: each once" : "the device's list prewarmed them: none");
    t(check, first ? (w1 - w0 == NK && made == NK) : (w1 == w0 && made == 0));
    if (!first) {
        snprintf(check, sizeof check, "%s: the prewarm built the %d pipelines of the device's list (%llu rows, %llu built)", phase, NK, (unsigned long long)pw.rows, (unsigned long long)pw.built);
        t(check, pw.rows == NK && pw.built == NK);
    }
    printf("%s: cache %s: %llu bytes of driver data read; the prewarm's and the draws' creations %llu, answered from the driver's cache %llu; device list rows appended %llu\n", phase,
           cs1.path, (unsigned long long)cs1.loaded, (unsigned long long)cs1.created, (unsigned long long)cs1.hits, (unsigned long long)pw.user_rows);
    if (cs1.loaded > 32) { snprintf(check, sizeof check, "%s: the driver keeps pipelines in its cache data: it answered some creations from it", phase); t(check, cs1.hits > 0); }
    else if (!first && !strcmp(phase, "reload")) printf("note: this driver's cache data is %llu bytes, its header alone: it keeps no pipeline in it, so a second start compiles again (lavapipe)\n", (unsigned long long)cs1.loaded);
    hv_res_put(res, "waits", (double)(w1 - w0)); hv_res_put(res, "made", (double)made); hv_res_put(res, "loaded", (double)cs1.loaded); hv_res_put(res, "created", (double)cs1.created);
    hv_res_put(res, "hits", (double)cs1.hits); hv_res_put(res, "red_cache", g_red_cache); hv_res_put(res, "user_rows", (double)pw.user_rows); hv_res_put(res, "ran", 1);
    hv_pool_free(&mp);
    int rc = hv_finish();   // the shutdown writes the cache (its line is logged)
    return rc;
}

// The parent: the cache file's framing, as vk_cache.c writes it (8 bytes "nxvkpc1\0", the length, SHA-256, the driver's data)
static int file_valid(const char *path, size_t *data_len) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t b[1 << 16]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
    if (n < 48 || memcmp(b, "nxvkpc1", 8)) return 0;
    uint64_t len = 0; for (int i = 7; i >= 0; i--) len = len << 8 | b[8 + i];
    if (len != n - 48) return 0;
    uint8_t sha[32]; plat_sha256(b + 48, (size_t)len, sha);
    if (data_len) *data_len = (size_t)len;
    return !memcmp(sha, b + 16, 32);
}
static int find_cache(const char *dir, char *out, size_t n) {   // the one .bin file of the device in dir (not the planted other device's)
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".bin") && strncmp(e->d_name, "ffff-ffff-", 10)) { snprintf(out, n, "%s/%s", dir, e->d_name); found++; } }
    closedir(d);
    return found == 1;
}
static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL; if (!a || !b) { if (a) fclose(a); return 0; }
    uint8_t buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, a))) fwrite(buf, 1, n, b);
    fclose(a); fclose(b); return 1;
}
int main(int argc, char **argv) {
    char res[800];
    if (argc > 1) { snprintf(res, sizeof res, "%s/%s.res", getenv("T_VK_PCACHE_DIR") ? getenv("T_VK_PCACHE_DIR") : ".", argv[1]); return child(argv[1], res); }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *tmp = getenv("TMPDIR");
    char dir[600], cache[700], user[700]; snprintf(dir, sizeof dir, "%s/t_vk_pcache_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(cache, sizeof cache, "%s/cache", dir); snprintf(user, sizeof user, "%s/pso_user_vk.tsv", dir);
    setenv("DM_VK_PSO_CACHE", cache, 1); setenv("DM_VK_PSO_USER", user, 1); setenv("DM_PSO_MANIFEST", "", 1); setenv("T_VK_PCACHE_DIR", dir, 1);
    char name[400], path[900], planted_same[1000] = "", planted_other[1000] = "", r[900];
    int fail = 0;
#define START(ph) do { printf("---- start: %s ----\n", ph); int rc_ = hv_spawn(argv[0], ph); snprintf(r, sizeof r, "%s/%s.res", dir, ph); if (rc_) { printf("start %s failed (exit status %d)\n", ph, rc_); fail = 1; } } while (0)
    START("write");
    if (hv_res_get(r, "ran", 0) == 0) { printf("== t_vk_pipeline_cache: skipped (no Vulkan device) ==\n"); rm_dir(cache); remove(user); rm_dir(dir); return fail; }
    size_t len = 0; int have = find_cache(cache, path, sizeof path);
    snprintf(name, sizeof name, "write: one cache file for this device, framed and hashed as written (%s)", have ? path : "none"); t(name, have && file_valid(path, &len));
    FILE *u = fopen(user, "r"); int rows = 0; char line[NVN_PIPE_ROW_MAX + 8]; if (u) { while (fgets(line, sizeof line, u)) rows += line[0] == 'P'; fclose(u); }
    snprintf(name, sizeof name, "write: the device's list holds the %d pipelines the draws built on the spot, as P rows (%d)", NK, rows); t(name, rows == NK);
    // plant: this device's file under another chain version (deleted at the next start) and another device's (kept)
    if (have) {
        const char *base = strrchr(path, '/') + 1, *d1 = strchr(base, '-'), *d2 = d1 ? strchr(d1 + 1, '-') : NULL;   // "<vendor>-<device>-" (hexadecimal, 4 digits or more)
        char prefix[64]; snprintf(prefix, sizeof prefix, "%.*s", d2 ? (int)(d2 + 1 - base) : 0, base);
        snprintf(planted_same, sizeof planted_same, "%s/%s00000000-00000000000000000000000000000000-000000000000-v1.bin", cache, prefix);
        snprintf(planted_other, sizeof planted_other, "%s/ffff-ffff-00000000-00000000000000000000000000000000-000000000000-v1.bin", cache);
        copy_file(path, planted_same); copy_file(path, planted_other);
    }
    START("reload");
    snprintf(name, sizeof name, "reload: the driver data of the file was read (%.0f bytes, the file holds %zu)", hv_res_get(r, "loaded", -1), len); t(name, hv_res_get(r, "loaded", -1) == (double)len && len > 0);
    t("reload: this device's file of another chain version was deleted at start", planted_same[0] && !exists(planted_same));
    t("reload: another device's file was kept", planted_other[0] && exists(planted_other));
    t("reload: no red line about the cache", hv_res_get(r, "red_cache", -1) == 0);
    double c_reload = hv_res_get(r, "created", 0), h_reload = hv_res_get(r, "hits", 0);
    // corrupt the data: one byte of the driver's header changed (the hash no longer matches)
    if (have) { FILE *c = fopen(path, "r+b"); if (c) { fseek(c, 48 + 20, SEEK_SET); int ch = fgetc(c); fseek(c, 48 + 20, SEEK_SET); fputc(ch ^ 0x5a, c); fclose(c); } }
    t("corrupt: the planted change makes the file invalid", have && !file_valid(path, NULL));
    START("corrupt");
    snprintf(name, sizeof name, "corrupt: exactly one red line about the cache (%.0f)", hv_res_get(r, "red_cache", -1)); t(name, hv_res_get(r, "red_cache", -1) == 1);
    t("corrupt: no driver data was read from it", hv_res_get(r, "loaded", -1) == 0);
    t("corrupt: a valid file was written at the end", have && file_valid(path, NULL));
    // another key: the file as a previous version of the backend named it
    char renamed[1000] = "";
    if (have) { snprintf(renamed, sizeof renamed, "%s", path); char *v = strstr(renamed, "-v1.bin"); if (v) memcpy(v, "-v0.bin", 7); rename(path, renamed); }
    START("invalid");
    t("invalid: the file of the other key was deleted at start", renamed[0] && !exists(renamed));
    t("invalid: no driver data was read", hv_res_get(r, "loaded", -1) == 0);
    t("invalid: a new file was written for the current key", have && file_valid(path, NULL));
    printf("---- the starts ----\n%-8s %6s %6s %10s %10s %6s\n", "start", "waits", "made", "creations", "cache hits", "bytes");
    static const char *const ph[] = {"write", "reload", "corrupt", "invalid"};
    for (int i = 0; i < 4; i++) { snprintf(r, sizeof r, "%s/%s.res", dir, ph[i]);
        printf("%-8s %6.0f %6.0f %10.0f %10.0f %6.0f\n", ph[i], hv_res_get(r, "waits", 0), hv_res_get(r, "made", 0), hv_res_get(r, "created", 0), hv_res_get(r, "hits", 0), hv_res_get(r, "loaded", 0)); }
    printf("reload: %.0f creations, %.0f answered from the driver's cache\n", c_reload, h_reload);
    rm_dir(cache); remove(user);
    for (int i = 0; i < 4; i++) { snprintf(r, sizeof r, "%s/%s.res", dir, ph[i]); remove(r); }
    rm_dir(dir);
    printf("== t_vk_pipeline_cache: %d checks, %d failed%s ==\n", g_total, g_fail + fail, fail ? " (a start failed)" : "");
    return g_fail || fail;
}
