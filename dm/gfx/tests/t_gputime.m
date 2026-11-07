// DM_NVN_GPUTIME (nvn_gputime.inc) through the replay, on the GPU: a slow compute kernel (a bounded loop calibrated to about 150 ms, never more than about 250 ms, far from the 4 s
// GPU watchdog) between two fast ones in one submission (one shared encoder), direct and indirect, then a draw pass with a slow and a fast draw (pass mode, then DM_NVN_GPUTIME_DRAWS).
// The slow job must be named with its key, grid and ms over DM_NVN_GPUTIME_RED_MS (60 here), the fast ones must not be flagged. The measured times are printed: they show the
// granularity the machine gives (a fast dispatch right after the slow one in the same encoder near 0 ms: per dispatch; near the slow one's time: collapsed to the encoder).
// Run only when no game is running (sw2_tools/safety/gpu_preflight.sh): make GAME=<id> check_t_gputime
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
enum { RED_MS = 60 };
static const double TARGET_MS = 150, CAP_MS = 250;   // the slow job's aim, and the most a calibration step may ask for (by the wall clock, which is never below the GPU time)
static uint64_t g_frame = 1000;

// ---- compute: one kernel, its loop count read from its storage buffer (word 0), so one pipeline serves every case ----
static NvShader *cshader(id<MTLFunction> fn, const char *key) {
    NvShader *s = [NvShader new]; s->stage = 5; s->fn = fn; s->ae = [fn newArgumentEncoderWithBufferIndex:0];
    s->rf.tg[0] = 32; s->rf.tg[1] = s->rf.tg[2] = 1; s->rf.nb = 1; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 2, .aid = 0};
    snprintf(s->key, sizeof s->key, "%s", key); return s;
}
typedef struct { NvShader *sh; NvProg *pr; prog_t pt; uint64_t addr; } cjob;
static uint8_t *g_mem; static uint64_t g_base;
static void cjob_make(cjob *j, id<MTLFunction> fn, const char *key, int slot, uint64_t serial) {
    j->sh = cshader(fn, key); j->pr = [NvProg new]; j->pr->sh[5] = j->sh; j->pt.serial = serial; serial_put(serial, j->pr); j->addr = g_base + 256 * (uint64_t)slot;
}
static void cjob_dispatch(cjob *j, uint32_t loops, int indirect) {
    ((uint32_t *)(uintptr_t)j->addr)[0] = loops;
    S.cprog = &j->pt; S.ssbo[5][2] = (p_bind_buf){.addr = j->addr, .size = 64};
    p_dispatch d = {.x = 1, .y = 1, .z = 1};
    if (indirect) { uint32_t *c = (uint32_t *)(uintptr_t)(j->addr + 128); c[0] = 1; c[1] = 1; c[2] = 1; d.ind = j->addr + 128; }
    dispatch(&d);
}
static uint64_t g_seen;   // g_gt_last_n before the case
static int wait_jobs(uint64_t n) {   // the completion handler has resolved n more jobs
    for (int i = 0; i < 400; i++) { pthread_mutex_lock(&g_gt_mu); uint64_t m = g_gt_last_n; pthread_mutex_unlock(&g_gt_mu); if (m >= g_seen + n) return 1; usleep(5000); }
    return 0;
}
static const gt_rec *find(const char *key) {   // the newest record of that key since the case began
    for (uint64_t i = g_gt_last_n; i-- > g_seen && g_gt_last_n - i <= GT_LAST;) { const gt_rec *r = &g_gt_last[i % GT_LAST]; if (r->key && !strcmp(r->key, key)) return r; }
    return NULL;
}
static double wall_one(cjob *j, uint32_t loops) {   // ms from commit to completion of a submission with that one dispatch
    mtl_set_frame(g_frame++); mtl_sub_begin(); cjob_dispatch(j, loops, 0);
    uint64_t t0 = bp_now(); int ok = mtl_wait(mtl_sub_commit(), 3000000000ull); double ms = (bp_now() - t0) / 1e6;
    CHECK(ok && !mtl_last_commit_error(), "calibration dispatch completes"); return ms;
}
static uint32_t calibrate(cjob *j) {   // a loop count for TARGET_MS, grown at most 4x a step from about 1 ms
    uint32_t n = 1u << 12; double ms = wall_one(j, n);
    while (ms < 20 && n < (1u << 30)) { n *= 4; ms = wall_one(j, n); }
    if (ms < 20) { CHECK(0, "the spin kernel never took 20 ms (%.2f ms at %u loops): not scaled further", ms, n); return n; }   // never extrapolate from a run that did not do the work
    double scale = TARGET_MS / (ms > 0.01 ? ms : 0.01); if (scale * ms > CAP_MS) scale = CAP_MS / ms;
    uint64_t m = (uint64_t)((double)n * scale); if (m > 0xffffffffu) m = 0xffffffffu;
    printf("  calibration: %u loops took %.1f ms (wall); slow kernel %llu loops\n", n, ms, (unsigned long long)m);
    return (uint32_t)m;
}
// One submission: fast, slow, fast (one shared encoder unless the replay ends it), the slow one direct or indirect
static void compute_case(cjob *fast, cjob *slow, cjob *fast2, uint32_t loops, int indirect) {
    pthread_mutex_lock(&g_gt_mu); g_seen = g_gt_last_n; pthread_mutex_unlock(&g_gt_mu); uint64_t red0 = g_gt_red_n;
    mtl_set_frame(g_frame++); mtl_sub_begin();
    cjob_dispatch(fast, 16, 0); cjob_dispatch(slow, loops, indirect); cjob_dispatch(fast2, 16, 0);
    CHECK(mtl_wait(mtl_sub_commit(), 3000000000ull) && !mtl_last_commit_error(), "compute case completes");
    CHECK(wait_jobs(3), "three jobs resolved");
    const gt_rec *a = find(fast->sh->key), *b = find(slow->sh->key), *c = find(fast2->sh->key);
    printf("  %s: fast %.3f ms, slow %.3f ms (grid %u %u %u), fast after slow %.3f ms; RED lines %llu; pairs %llu collapsed %llu\n", indirect ? "indirect" : "direct",
           a ? a->ms : -1, b ? b->ms : -1, b ? b->x : 0, b ? b->y : 0, b ? b->z : 0, c ? c->ms : -1, (unsigned long long)(g_gt_red_n - red0), (unsigned long long)g_gt_pairs, (unsigned long long)g_gt_collapsed);
    CHECK(b && b->kind == 'c' && b->ms >= RED_MS, "%s: the slow kernel is named with its time (%.1f ms)", indirect ? "indirect" : "direct", b ? b->ms : -1);
    CHECK(b && b->x == 1 && b->y == 1 && b->z == 1, "the slow kernel's grid (%u %u %u)", b ? b->x : 0, b ? b->y : 0, b ? b->z : 0);
    CHECK(a && a->ms < RED_MS && c && c->ms < RED_MS, "the fast kernels are not flagged (%.1f, %.1f ms)", a ? a->ms : -1, c ? c->ms : -1);
    CHECK(g_gt_red_n - red0 == 1, "exactly one RED line (%llu)", (unsigned long long)(g_gt_red_n - red0));
}

// ---- draws: one pass into an 8 x 4 target, a fast draw then a slow one (its fragment shader loops a compiled-in count) ----
enum { W = 8, H = 4 };   // 32 pixels: one SIMD group of fragments, so the slow draw costs about what the slow kernel's one group does (no calibration of its own: a draw step that silently did no work could not hide a jump)
static uint32_t g_serial = 98000;
static NvShader *rshader(id<MTLLibrary> lib, NSString *name, int stage, const char *key) {
    NvShader *s = [NvShader new]; s->fn = fn4_note([lib newFunctionWithName:name], lib, name, nil); s->stage = stage; snprintf(s->key, sizeof s->key, "%s", key); return s;
}
static id<MTLLibrary> rlib(uint32_t loops) {
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vt(uint i [[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0.5, 1); }\n"
        "fragment float4 ffast(float4 p [[position]]) { return float4(p.x / 8.0, 0, 0, 1); }\n"
        "fragment float4 fslow(float4 p [[position]]) { uint x = uint(p.x) * 977u + uint(p.y) + 1u; for (uint i = 0; i < %uu; i++) x = x * 1664525u + 1013904223u;\n"
        "    return float4(float(x & 255u) / 255.0, 0, 0, 1); }\n", loops];
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&e];
    if (!lib) fprintf(stderr, "%s\n", e.localizedDescription.UTF8String);
    return lib;
}
static NvProg *g_rp[2]; static prog_t g_rpt[2]; static char g_kv[24], g_kf[24], g_ks[24];   // keys of this library: pipelines are cached by shader key, so each compiled count gets its own
static void rprogs(uint32_t loops, uint64_t serial0) {   // 0: fast, 1: slow
    id<MTLLibrary> lib = rlib(loops);
    snprintf(g_kv, sizeof g_kv, "7e57%012x", loops); snprintf(g_kf, sizeof g_kf, "fa57%012x", loops); snprintf(g_ks, sizeof g_ks, "5105%012x", loops);
    NvShader *v = rshader(lib, @"vt", 0, g_kv), *ff = rshader(lib, @"ffast", 1, g_kf), *fs = rshader(lib, @"fslow", 1, g_ks);
    for (int i = 0; i < 2; i++) { if (g_rpt[i].serial) serial_del(g_rpt[i].serial); g_rp[i] = [NvProg new]; g_rp[i]->sh[0] = v; g_rp[i]->sh[1] = i ? fs : ff; g_rpt[i].serial = serial0 + i; serial_put(g_rpt[i].serial, g_rp[i]); }
}
static double rframe(int slow_too) {   // one frame with the pass; ms from commit to completion
    tex_t target = {.magic = MAGIC('T', 'E', 'X', 0), .serial = g_serial++};
    target.d = (nvn_texdesc){.width = W, .height = H, .depth = 1, .samples = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    uint8_t *commands; if (posix_memalign((void **)&commands, getpagesize(), 65536)) abort(); memset(commands, 0, 65536); mtl_pool_add(commands, 65536);
    size_t used = 0;
    #define EMIT(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(commands + used, &h, 8); memcpy(commands + used + 8, &(X), sizeof(X)); used += z; } while (0)
    p_set_rt rt = {.n = 1}; rt.color[0] = &target; EMIT(OP_SET_RT, rt);
    p_clear_color cl = {.index = 0, .mask = 15, .rgba = {0.25f, 0, 0, 1}}; EMIT(OP_CLEAR_COLOR, cl);
    for (int i = 0; i < 1 + slow_too; i++) {
        p_bind_state bs = {.kind = 1}; color_st cs = {0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; memcpy(bs.bytes, &cs, sizeof cs); EMIT(OP_BIND_STATE, bs);   // as t_blame: plain colour writes
        p_bind_state bm = {.kind = 2}; chmask_st cm = {15}; memcpy(bm.bytes, &cm, sizeof cm); EMIT(OP_BIND_STATE, bm);
        p_bind_state bd = {.kind = 5}; ds_st dss = {0}; dss.df = NVN_DEPTH_FUNC_LESS; dss.sf_f = dss.sf_b = NVN_STENCIL_FUNC_ALWAYS;
        dss.fail_f = dss.zfail_f = dss.zpass_f = dss.fail_b = dss.zfail_b = dss.zpass_b = NVN_STENCIL_OP_KEEP; memcpy(bd.bytes, &dss, sizeof dss); EMIT(OP_BIND_STATE, bd);
        p_bind_program bp = {&g_rpt[i], 3}; EMIT(OP_BIND_PROGRAM, bp);
        p_viewports vp = {.n = 1}; vp.v[0][2] = W; vp.v[0][3] = H; vp.v[0][5] = 1; EMIT(OP_VIEWPORTS, vp);
        p_scissor sc = {0, 0, W, H}; EMIT(OP_SCISSOR, sc);
        p_draw dr = {.prim = 4, .count = 3, .instances = 1}; EMIT(OP_DRAW, dr);
    }
    op_hdr end = {OP_END, 8}; memcpy(commands + used, &end, 8);
    #undef EMIT
    state_reset(); mtl_set_frame(g_frame++);
    memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t handle = (uint64_t)commands; mtl_sub_index(&handle, 1); mtl_exec(commands);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:4 * W * H options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:4 * W destinationBytesPerImage:4 * W * H];
    uint64_t t0 = bp_now(); int ok = mtl_wait(mtl_sub_commit(), 3000000000ull); double ms = (bp_now() - t0) / 1e6;
    CHECK(ok && !mtl_last_commit_error(), "draw frame completes");
    if (getenv("T_GPUTIME_PX")) printf("  pixels: %08x %08x %08x\n", ((uint32_t *)pixels.contents)[0], ((uint32_t *)pixels.contents)[W + 7], ((uint32_t *)pixels.contents)[W * H - 1]);
    mtl_pool_del(commands); free(commands);
    return ms;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_GPUTIME", "1", 1); setenv("DM_NVN_GPUTIME_TOP", "3", 1); setenv("DM_NVN_GPUTIME_EVERY", "1", 1);
    setenv("DM_PSO_HYBRID", "0", 1);   // a draw waits for its pipeline (no stand-in, no draw skipped while it builds in the background)
    char rm[16]; snprintf(rm, sizeof rm, "%d", RED_MS); setenv("DM_NVN_GPUTIME_RED_MS", rm, 1); unsetenv("DM_NVN_GPUTIME_DRAWS");
    mtl_init(); state_reset(); memset(&R, 0, sizeof R);
    CHECK(gt_on() == 1, "DM_NVN_GPUTIME is on");
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *w [[id(0)]]; };\n"
        "kernel void spin(constant Args &a [[buffer(0)]], uint t [[thread_position_in_grid]]) {\n"
        "    uint n = a.w[0], x = t + 1u;\n"
        "    for (uint i = 0; i < n; i++) x = x * 1664525u + 1013904223u;\n"
        "    a.w[2 + (t & 7u)] = x;\n"
        "}\n" options:nil error:&error];
    CHECK(lib != nil, "spin kernel compiles"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"spin"];
    size_t msz = 4 * (size_t)getpagesize(); if (posix_memalign((void **)&g_mem, (size_t)getpagesize(), msz)) abort(); memset(g_mem, 0, msz); mtl_pool_add(g_mem, msz); g_base = (uint64_t)(uintptr_t)g_mem;
    cjob fast, slow, fast2;
    cjob_make(&fast, fn, "fa57fa57fa57fa57", 0, UINT64_MAX - 20); cjob_make(&slow, fn, "26166d37ed56d6bb", 1, UINT64_MAX - 21); cjob_make(&fast2, fn, "fa5702fa5702fa57", 2, UINT64_MAX - 22);
    uint32_t loops = calibrate(&slow);
    compute_case(&fast, &slow, &fast2, loops, 0);
    compute_case(&fast, &slow, &fast2, loops, 1);

    // draws, pass mode: the pass is the job, named by its first key, with its draws' keys. OFF unless T_GPUTIME_DRAWS=1: on 2026-11-05 this draw (the compute count,
    // 11.7M loops a pixel, 32 pixels) ran far longer than the kernel and the GPU restarted ("CDM Kill timeout", gpuEvent-t_gputime-2026-11-05-191218.ips). Fragment work does not
    // scale like one compute SIMD group; a safe draw case needs its own bounded calibration from a draw that is proven to do its work (pixels read back)
    if (!getenv("T_GPUTIME_DRAWS")) { printf("  draw cases skipped (T_GPUTIME_DRAWS=1 runs them; see the comment: they restarted the GPU once)\n"); goto done; }
    uint32_t rl = loops / 64; rprogs(rl, 97200);   // the compute calibration's count: a 32-pixel draw is one or two SIMD groups of it, about 150-300 ms
    pthread_mutex_lock(&g_gt_mu); g_seen = g_gt_last_n; pthread_mutex_unlock(&g_gt_mu); uint64_t red0 = g_gt_red_n;
    rframe(1); CHECK(wait_jobs(1), "the pass resolved");
    const gt_rec *p = NULL; for (uint64_t i = g_gt_last_n; i-- > g_seen;) if (g_gt_last[i % GT_LAST].kind == 'R') { p = &g_gt_last[i % GT_LAST]; break; }
    printf("  pass mode: pass %.3f ms (%ux%u); RED lines %llu\n", p ? p->ms : -1, p ? p->x : 0, p ? p->y : 0, (unsigned long long)(g_gt_red_n - red0));
    CHECK(p && p->ms >= RED_MS && p->x == W && p->y == H, "the slow pass is flagged (%.1f ms)", p ? p->ms : -1);
    CHECK(g_gt_red_n - red0 == 1, "one RED line for the pass (%llu)", (unsigned long long)(g_gt_red_n - red0));
    // draws mode: each draw is a job, named by its fragment shader
    g_gt_draws = 1;
    pthread_mutex_lock(&g_gt_mu); g_seen = g_gt_last_n; pthread_mutex_unlock(&g_gt_mu); red0 = g_gt_red_n;
    rframe(1); CHECK(wait_jobs(2), "two draws resolved");
    const gt_rec *df = find(g_kf), *ds = find(g_ks);
    printf("  draws mode: fast draw %.3f ms, slow draw %.3f ms; RED lines %llu\n", df ? df->ms : -1, ds ? ds->ms : -1, (unsigned long long)(g_gt_red_n - red0));
    CHECK(ds && ds->kind == 'd' && ds->ms >= RED_MS, "the slow draw is named (%.1f ms)", ds ? ds->ms : -1);
    CHECK(df && df->ms < RED_MS, "the fast draw is not flagged (%.1f ms)", df ? df->ms : -1);
    g_gt_draws = 0;
done:
    // the heaps all went back to the pool; the untimed count stayed 0
    usleep(20000); CHECK(atomic_load(&g_gt_free) == (uint32_t)((1ull << GT_HEAPS) - 1), "every heap is free again (%#x)", atomic_load(&g_gt_free));
    CHECK(atomic_load(&g_gt_untimed) == 0, "no job went untimed");
    mtl_drain(); S.cprog = NULL;
    serial_del(fast.pt.serial); serial_del(slow.pt.serial); serial_del(fast2.pt.serial); for (int i = 0; i < 2; i++) serial_del(g_rpt[i].serial);
    mtl_pool_del(g_mem); free(g_mem);
    printf("t_gputime: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
