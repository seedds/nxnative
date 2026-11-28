// Double-residency memory census (mem_census, #1281): verifies classification of resident textures by NVN pool type
// (CPU_NO_ACCESS, CPU-visible, virtual/sparse), double-held vs single-stored GPU-only / placement sparse / zero-copy views,
// overlapping guest-range deduplication, wired pool byte attribution, D24S8->Depth32Float_Stencil8 expansion, tex_upgrade
// metadata preservation, host extras (recycled heaps, packaged spares, upload staging slots), and automatic logging on mem_trim.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static char g_census_dh[1024], g_census_ss[1024], g_census_he[1024];
static int census_capture(const char *why, void (^action)(void)) {
    g_census_dh[0] = g_census_ss[0] = g_census_he[0] = 0;
    if (fflush(stderr)) return -1;
    int saved = dup(2); if (saved < 0) return -1;
    FILE *tmp = tmpfile(); if (!tmp) { close(saved); return -1; }
    if (dup2(fileno(tmp), 2) < 0) { fclose(tmp); close(saved); return -1; }
    action();
    int flushed = fflush(stderr), restored = dup2(saved, 2); close(saved);
    if (flushed || restored < 0 || fseek(tmp, 0, SEEK_SET)) { fclose(tmp); return -1; }
    char line[8192], tag[128]; snprintf(tag, sizeof tag, "] %s census: ", why); int n = 0;
    while (fgets(line, sizeof line, tmp)) {
        const char *at = strstr(line, tag); if (!at) continue; n++;
        const char *dh = strstr(at, "double-held textures (guest + Metal): "); if (dh) snprintf(g_census_dh, sizeof g_census_dh, "%s", dh + strlen("double-held textures (guest + Metal): "));
        const char *ss = strstr(at, "single-stored textures: "); if (ss) snprintf(g_census_ss, sizeof g_census_ss, "%s", ss + strlen("single-stored textures: "));
        const char *he = strstr(at, "host extras: "); if (he) snprintf(g_census_he, sizeof g_census_he, "%s", he + strlen("host extras: "));
    }
    if (ferror(tmp)) n = -1;
    if (fclose(tmp)) n = -1;
    return n;
}
static void settle(void) {
    mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
    dispatch_sync(g_feedback_q, ^{}); defer_drain(); mtl_sub_begin();
}

int main(void) { @autoreleasepool {
    mtl_init();
    mtl_sub_begin();
    void *na_mem = NULL, *cv_mem = NULL;
    if (posix_memalign(&na_mem, 16384, 16 << 20) || posix_memalign(&cv_mem, 16384, 16 << 20)) abort();
    memset(na_mem, 0x3c, 16 << 20);
    memset(cv_mem, 0x5a, 16 << 20);
    pool_t na_pool = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = na_mem, .size = 16 << 20};
    pool_t cv_pool = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = cv_mem, .size = 16 << 20};
    mtl_pool_add(na_mem, na_pool.size); nvn_dirty_add_range(na_mem, na_pool.size);
    mtl_pool_add(cv_mem, cv_pool.size); nvn_dirty_add_range(cv_mem, cv_pool.size);

    // 1. CPU_NO_ACCESS packaged (512x512 RGBA8 = 1 MB) + CPU_NO_ACCESS init-fill (512x512 RGBA8 = 1 MB, non-compressible, non-zero storage)
    tex_t na_pkg = {.magic = MAGIC('T','E','X',0), .has_layout = 1, .serial = 96001, .pool = &na_pool, .mem = (uint8_t *)na_mem, .off = 0};
    na_pkg.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 512, .height = 512, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&na_pkg.d, &na_pkg.L); mtl_tex_init(&na_pkg);

    tex_t na_fill = {.magic = MAGIC('T','E','X',0), .serial = 96002, .pool = &na_pool, .mem = (uint8_t *)na_mem + (2 << 20), .off = 2 << 20};
    na_fill.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 512, .height = 512, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&na_fill.d, &na_fill.L); mtl_tex_init(&na_fill);

    // 2. CPU-visible packaged: two distinct textures over the same 1 MB guest range (guest storage must deduplicate to 1.0 MB)
    tex_t cv_pkg1 = {.magic = MAGIC('T','E','X',0), .has_layout = 1, .serial = 96003, .pool = &cv_pool, .mem = (uint8_t *)cv_mem, .off = 0};
    cv_pkg1.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 512, .height = 512, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&cv_pkg1.d, &cv_pkg1.L); mtl_tex_init(&cv_pkg1);

    tex_t cv_pkg2 = {.magic = MAGIC('T','E','X',0), .has_layout = 1, .serial = 96004, .pool = &cv_pool, .mem = (uint8_t *)cv_mem, .off = 0};
    cv_pkg2.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8_SRGB, .width = 512, .height = 512, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&cv_pkg2.d, &cv_pkg2.L); mtl_tex_init(&cv_pkg2);

    // 3. Single-stored GPU-only D24S8 depth-stencil target in CPU_NO_ACCESS (1024x1024 -> 4 MB guest, >4 MB Metal D32F_S8 in a recycled heap)
    tex_t na_d24 = {.magic = MAGIC('T','E','X',0), .serial = 96005, .pool = &na_pool, .mem = (uint8_t *)na_mem + (4 << 20), .off = 4 << 20};
    na_d24.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_DEPTH24_STENCIL8, .flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width = 1024, .height = 1024, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&na_d24.d, &na_d24.L); mtl_tex_init(&na_d24);

    // 4. Zero-copy linear texture in CPU-visible pool (512x512 RGBA8 = 1 MB, aligned stride)
    tex_t cv_lin = {.magic = MAGIC('T','E','X',0), .serial = 96006, .pool = &cv_pool, .mem = (uint8_t *)cv_mem + (4 << 20), .off = 4 << 20};
    cv_lin.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_LINEAR, .width = 512, .height = 512, .depth = 1, .levels = 1, .samples = 1, .stride = 2048};
    nvn_tex_layout(&cv_lin.d, &cv_lin.L); mtl_tex_init(&cv_lin);

    id<MTLTexture> t_nap = tex_mtl(&na_pkg), t_naf = tex_mtl(&na_fill), t_cv1 = tex_mtl(&cv_pkg1), t_cv2 = tex_mtl(&cv_pkg2), t_d24 = tex_mtl(&na_d24), t_lin = tex_mtl(&cv_lin);
    CHECK(t_nap && t_naf && t_cv1 && t_cv2 && t_d24 && t_lin && t_lin.buffer, "all six accounting textures resolve as expected");

    // Upgrade na_pkg to allow PixelFormatView and verify its metadata survives the upgrade
    id<MTLTexture> t_nap_up = tex_upgrade_pfv(&na_pkg, t_nap, MTLPixelFormatRGBA8Unorm_sRGB);
    CHECK(t_nap_up && t_nap_up != t_nap, "CPU_NO_ACCESS packaged texture upgrades cleanly");
    t_nap = nil; t_nap_up = nil; t_naf = nil; t_cv1 = nil; t_cv2 = nil; t_d24 = nil; t_lin = nil;
    settle();

    // Trigger one re-upload on cv_pkg1 so it keeps a packaged spare in host extras
    dm_guest_write_prepare(cv_mem, 16384);
    ((uint8_t *)cv_mem)[0] ^= 0xff;
    CHECK(tex_mtl(&cv_pkg1) != nil, "CPU-visible packaged texture re-uploads and records a spare");

    int nlines = census_capture("t_double_res", ^{ mem_census("t_double_res"); });
    CHECK(nlines >= 7 && g_census_dh[0] && g_census_ss[0] && g_census_he[0], "census writes all three double-residency lines");

    unsigned long long na_n = 0, na_if_n = 0, cv_n = 0, cv_if_n = 0, vs_n = 0, tot_n = 0;
    double na_m = 0, na_g = 0, na_w = 0, na_if_m = 0, cv_m = 0, cv_g = 0, cv_w = 0, cv_if_m = 0, vs_m = 0, vs_g = 0, tot_m = 0, tot_g = 0, tot_w = 0;
    int parsed_dh = sscanf(g_census_dh,
        "CPU_NO_ACCESS %llu (Metal %lf MB, guest %lf MB, wired %lf MB; init-fill %llu, %lf MB); "
        "CPU-visible %llu (Metal %lf MB, guest %lf MB, wired %lf MB; init-fill %llu, %lf MB); "
        "virtual/sparse whole %llu (Metal %lf MB, guest %lf MB); "
        "total %llu (Metal %lf MB, guest %lf MB, wired %lf MB)",
        &na_n, &na_m, &na_g, &na_w, &na_if_n, &na_if_m,
        &cv_n, &cv_m, &cv_g, &cv_w, &cv_if_n, &cv_if_m,
        &vs_n, &vs_m, &vs_g, &tot_n, &tot_m, &tot_g, &tot_w);
    CHECK(parsed_dh == 19, "double-held census line parses cleanly");
    CHECK(na_n == 2 && na_if_n == 1 && na_m >= 1.9 && na_g >= 1.9 && na_g <= 2.2 && na_if_m >= 0.9,
          "CPU_NO_ACCESS double-held counts packaged + init-fill and survives tex_upgrade");
    CHECK(na_w == 0.0, "unwired CPU_NO_ACCESS pool reports 0.0 MB wired");
    CHECK(cv_n == 2 && cv_if_n == 0 && cv_m >= 1.9 && cv_g >= 0.9 && cv_g <= 1.1 && cv_w >= 0.9,
          "CPU-visible double-held deduplicates overlapping guest storage to 1 MB while keeping both Metal copies");
    CHECK(tot_n == 4 && tot_g >= 2.9 && tot_g <= 3.2, "double-held total sums across pool types");
    if (failed) fprintf(stderr, "  dh: %s", g_census_dh);

    unsigned long long go_na_n = 0, go_cv_n = 0, go_vs_n = 0, sp_n = 0, sp_h = 0, zc_n = 0, d24_n = 0;
    double go_na_m = 0, go_na_g = 0, go_na_w = 0, go_cv_m = 0, go_cv_g = 0, go_cv_w = 0, go_vs_m = 0, go_vs_g = 0;
    double sp_meta = 0, sp_hb = 0, sp_live = 0, sp_sh = 0, zc_g = 0, d24_m = 0, d24_g = 0;
    int parsed_ss = sscanf(g_census_ss,
        "GPU-only CPU_NO_ACCESS %llu (Metal %lf MB, guest %lf MB, wired %lf MB); "
        "GPU-only CPU-visible %llu (Metal %lf MB, guest %lf MB, wired %lf MB); "
        "GPU-only virtual/sparse %llu (Metal %lf MB, guest %lf MB); "
        "placement sparse %llu (meta %lf MB; %llu heaps %lf MB, live tiles %lf MB, shared %lf MB); "
        "zero-copy linear/buffer %llu (guest %lf MB); "
        "D24S8->D32F_S8 %llu (Metal %lf MB vs guest %lf MB)",
        &go_na_n, &go_na_m, &go_na_g, &go_na_w,
        &go_cv_n, &go_cv_m, &go_cv_g, &go_cv_w,
        &go_vs_n, &go_vs_m, &go_vs_g,
        &sp_n, &sp_meta, &sp_h, &sp_hb, &sp_live, &sp_sh,
        &zc_n, &zc_g, &d24_n, &d24_m, &d24_g);
    CHECK(parsed_ss == 22, "single-stored census line parses cleanly");
    CHECK(go_na_n == 1 && go_na_m >= 4.5 && go_na_g >= 3.9 && go_na_g <= 4.1 && go_na_w == 0.0,
          "GPU-only CPU_NO_ACCESS accounts for its recycled heap's Metal bytes");
    CHECK(zc_n == 1 && zc_g >= 0.9 && zc_g <= 1.1, "zero-copy linear texture is counted apart from double-held");
    CHECK(d24_n == 1 && d24_m > d24_g && d24_g >= 3.9, "D24S8->D32F_S8 expansion reports Metal > guest MB");
    if (failed) fprintf(stderr, "  ss: %s", g_census_ss);

    unsigned he_rh_n = 0; unsigned long long he_sp_n = 0, he_ar_n = 0, he_ht_n = 0, he_lb_n = 0;
    double he_rh_b = 0, he_sp_b = 0, he_up_b = 0, he_ar_b = 0, he_geo_b = 0, he_ht_b = 0, he_sn_b = 0, he_ed_b = 0, he_lb_b = 0;
    int parsed_he = sscanf(g_census_he,
        "waiting recycled heaps %u (%lf MB); packaged spares %llu (%lf MB); upload staging slots %lf MB; "
        "arena blocks %llu (%lf MB); geometry scratch %lf MB; host-internal textures %llu (%lf MB) + present snapshots %lf MB; "
        "edge snapshots %lf MB; shader library data %llu (%lf MB)",
        &he_rh_n, &he_rh_b, &he_sp_n, &he_sp_b, &he_up_b,
        &he_ar_n, &he_ar_b, &he_geo_b, &he_ht_n, &he_ht_b, &he_sn_b, &he_ed_b, &he_lb_n, &he_lb_b);
    CHECK(parsed_he == 14, "host-extras census line parses cleanly");
    CHECK(he_sp_n == 1 && he_sp_b >= 0.9, "re-upload spare is counted in host extras, not double-held");
    CHECK(he_ht_n == 0, "no game texture fell into host-internal textures");
    if (failed) fprintf(stderr, "  he: %s", g_census_he);

    // Wire the CPU_NO_ACCESS pool and confirm wired MB moves from 0 to match guest storage
    void *mb = NULL; uint64_t mo = 0;
    CHECK(mtl_addr((uint64_t)(uintptr_t)na_mem, na_pool.size, &mb, &mo), "CPU_NO_ACCESS pool wires cleanly");
    nlines = census_capture("t_double_res_wired", ^{ mem_census("t_double_res_wired"); });
    na_w = 0;
    sscanf(g_census_dh, "CPU_NO_ACCESS %llu (Metal %lf MB, guest %lf MB, wired %lf MB;", &na_n, &na_m, &na_g, &na_w);
    CHECK(nlines >= 7 && na_w >= 1.9 && na_w <= 2.2, "wiring CPU_NO_ACCESS pool reflects its double-held guest bytes as wired");

    // Memory pressure trim logs the census automatically before reclaiming spares / upload staging
    mtl_sub_commit(); mtl_drain();
    atomic_store(&g_mp_want, 2);
    int mp_lines = census_capture("[mem] memory pressure", ^{ mtl_sub_begin(); });
    CHECK(mp_lines >= 7 && g_census_dh[0] && g_census_he[0], "memory pressure trim logs the double-residency census automatically");
    settle();

    // After memory pressure trim, the packaged spare and upload staging slot blocks are reclaimed
    nlines = census_capture("t_after_trim", ^{ mem_census("t_after_trim"); });
    he_sp_n = 99; he_up_b = 99.0;
    sscanf(g_census_he, "waiting recycled heaps %u (%lf MB); packaged spares %llu (%lf MB); upload staging slots %lf MB;",
           &he_rh_n, &he_rh_b, &he_sp_n, &he_sp_b, &he_up_b);
    CHECK(nlines >= 7 && he_sp_n == 0 && he_up_b == 0.0, "trimmed spares and upload staging slots drop to 0 in host extras");

    // Clean up all textures and pools; verify census drops back to 0
    mtl_tex_release(&na_pkg); mtl_tex_release(&na_fill); mtl_tex_release(&cv_pkg1);
    mtl_tex_release(&cv_pkg2); mtl_tex_release(&na_d24); mtl_tex_release(&cv_lin);
    mtl_alias_drop_pool(&na_pool); mtl_alias_drop_pool(&cv_pool);
    nvn_dirty_remove_range(na_mem); mtl_pool_del(na_mem);
    nvn_dirty_remove_range(cv_mem); mtl_pool_del(cv_mem);
    settle();
    rh_trim_n(1, NULL); settle();

    nlines = census_capture("t_empty", ^{ mem_census("t_empty"); });
    tot_n = 99; go_na_n = 99; zc_n = 99; d24_n = 99;
    sscanf(g_census_dh, "CPU_NO_ACCESS %llu (Metal %lf MB, guest %lf MB, wired %lf MB; init-fill %llu, %lf MB); "
        "CPU-visible %llu (Metal %lf MB, guest %lf MB, wired %lf MB; init-fill %llu, %lf MB); "
        "virtual/sparse whole %llu (Metal %lf MB, guest %lf MB); total %llu",
        &na_n, &na_m, &na_g, &na_w, &na_if_n, &na_if_m,
        &cv_n, &cv_m, &cv_g, &cv_w, &cv_if_n, &cv_if_m,
        &vs_n, &vs_m, &vs_g, &tot_n);
    sscanf(g_census_ss, "GPU-only CPU_NO_ACCESS %llu (Metal %lf MB, guest %lf MB, wired %lf MB); "
        "GPU-only CPU-visible %llu (Metal %lf MB, guest %lf MB, wired %lf MB); "
        "GPU-only virtual/sparse %llu (Metal %lf MB, guest %lf MB); "
        "placement sparse %llu (meta %lf MB; %llu heaps %lf MB, live tiles %lf MB, shared %lf MB); "
        "zero-copy linear/buffer %llu (guest %lf MB); D24S8->D32F_S8 %llu",
        &go_na_n, &go_na_m, &go_na_g, &go_na_w,
        &go_cv_n, &go_cv_m, &go_cv_g, &go_cv_w,
        &go_vs_n, &go_vs_m, &go_vs_g,
        &sp_n, &sp_meta, &sp_h, &sp_hb, &sp_live, &sp_sh,
        &zc_n, &zc_g, &d24_n);
    CHECK(nlines >= 7 && tot_n == 0 && go_na_n == 0 && zc_n == 0 && d24_n == 0, "all categories return to 0 after release");

    mtl_sub_commit(); mtl_drain(); free(na_mem); free(cv_mem);
    printf("t_mem_census: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
