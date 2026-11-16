// ---------- Scratch: frame generation for Ocarina of Time, DM_FRAMEGEN=1 (off by default). An evaluation build (seedds/nxnative#2279). ----------
// A game held at 30 fps shown at 60: MetalFX's frame interpolator makes the frame between the previous scene and this one, from the scene before the HUD (the upscaler's
// input, 1440x810 linear), the scene depth and motion vectors reprojected from the game's camera block (zero where the depth says a pixel is not still scenery).
// The generated scene is upscaled by the same MetalFX spatial scaler (a second run), and the HUD is not interpolated: at the present, a pixel of the canvas that changed
// after the upscale (the HUD passes) keeps the real frame's picture, every other pixel takes the generated scene. The present thread (DM_PRESENT_LINK=0) shows the
// generated frame, then the real one, each for half the present's period; generated frames pause while frames come late (as seedds/nxnative#2241's FG_STEADY).
// Scratch limits: OoT only (the camera block is vertex uniform 0 of 13,568 bytes, the scene camera the binding with far plane 25000; the canvas is presented 1:1 inside the
// window texture's crop); DM_UPSCALER=metalfx, DM_NVN_ENC_THREAD=0 and DM_PRESENT_LINK=0 only; the constants belong in a profile if this goes further.
// Pacing and accounting borrow from seedds/nxnative#2241 (pause while late, own present allocators) and from framegen-stage1-v4.patch (the held frame's presented time shifted
// back by its hold; the first engagement delayed past boot).
// Switches: DM_FRAMEGEN=0|1 (0 still logs the shown-frame statistics, for comparison); DM_FRAMEGEN_PAUSE=0 shows every generated frame, late or not;
// DM_FRAMEGEN_VIEW=1 the generated scene without the HUD split, 2 = HUD pixels in magenta; DM_FRAMEGEN_FLIP=1 flips the canvas rows against the window's;
// DM_FRAMEGEN_OVERLAP=1: the generator (motion pass, interpolation, second upscale, window picture) on a Metal 4 queue of its own, behind the frame by a shared event,
// so the GPU draws it beside the game's next frame; the generated frame's present and the real frame's behind it go to that queue too (no present upscaling there).
// The interpolator's other inputs (item 6): DM_FRAMEGEN_DT=1 gives the measured time between real frames as deltaTime (default 1/30 s); DM_FRAMEGEN_CAM=1 gives the camera's
// view and projection (macOS 27's worldToViewMatrix / viewToClipMatrix), 2 = and zero motion vectors in place of our reprojection. Motion scale is 1 and jitter 0, both right for this game.
// DM_FRAMEGEN_OWNPREV=0: the reprojection goes through the game's own previous view, not the host's record of the last frame's view-projection (see fg_frame).
// DM_FRAMEGEN_SHOT=<dir>:<a>-<b> saves the generated and the real picture of frames a..b (slow: it waits for the GPU).
#if FG_PART == 0
static int g_fg_mode = -1;   // -1 unread, 0 off, 1 on
static int fg_on(void); static void fg_cam_note(const float *c); static void fg_depth_note(MTL4RenderPassDescriptor *pd);
static void fg_shown(int gen, double tt, double tin);
static __thread double g_fg_pres_adj;   // present thread: seconds added to the presented time recorded for the real frame being presented (negative: it was held behind its generated frame)
static double g_fg_tin[64];   // by ticket: when the game queued the present (present_main), for the enqueue-to-screen time of real frames
#elif FG_PART == 1
#define FG_WIN 3
static int g_fg_seen, g_fg_view, g_fg_flip, g_fg_pause = 1;
static int g_fg_upto = 9;   // DM_FRAMEGEN_UPTO=<n>, to split the cost in play: 0 nothing drawn, 1 the three copies, 2 + motion pass, 3 + interpolation, 4 + second upscale, 5 + window picture (not shown), 9 all
static _Atomic int g_fg_hooked;   // the hook was reached: an upscale of the game's was drawn by MetalFX (up_mfx_encode -> fg_frame)
static struct {
    id<MTL4FXFrameInterpolator> fi; NSUInteger iw, ih, ow, oh; MTLPixelFormat cvf; int bad;
    id<MTLTexture> c[3], d[3], mv, gen, gen_up, cv0[3], cv1[FG_WIN], cv, depth_seen[4], win[FG_WIN];   // c, d, cv0: rings of three (this frame, the previous, and the one a late generator job still reads)
    uint64_t win_serial[FG_WIN], win_ticket[FG_WIN], win_next;
    id<MTL4CommandAllocator> walloc[FG_WIN]; id<MTL4CommandBuffer> wcb[FG_WIN]; id<MTLBuffer> wpar[FG_WIN];
    id<MTL4ArgumentTable> margs, wargs; id<MTLRenderPipelineState> mv_pso, comp_pso; MTLPixelFormat comp_fmt; id<MTLFunction> vf, f_mv, f_comp;
    int cur, warm, prev_ok, gen_ready; long last_frame, cam_frame; float cam[96];
    uint64_t made, resets, nocam, nodepth, busy, late, behind, settling, gen_behind;
    int kc;   // the ring slot of the frame being composed
    // DM_FRAMEGEN_OVERLAP=1: the generator's work on a queue of its own (q), ordered behind the frame by a shared event; job_n jobs committed, job_done finished (feedback)
    id<MTL4CommandQueue> q; id<MTLSharedEvent> ev; uint64_t job_n; _Atomic uint64_t job_done; void *q_layer;
    id<MTL4FXSpatialScaler> sc2; id<MTL4ArgumentTable> margs2;
    id<MTL4CommandAllocator> jalloc[FG_WIN], qalloc[FG_WIN]; id<MTL4CommandBuffer> jcb[FG_WIN], qcb[FG_WIN]; id<MTLBuffer> mpar[FG_WIN], jpar[FG_WIN];
    int p_ok, p_cur, p_prev, p_reset; float p_m[20], p_near, p_far, p_fov, p_aspect, p_dt;
    double t_prev, dt_sum; uint64_t dt_n; int cam_ok; float pj[4];   // pj: the least and greatest off-centre terms of the projection seen (x, x, y, y): a jittered projection would move them
    simd_float4x4 w2v, v2c;   // the job the frame left for its present (fg_frame -> fg_job)
    double pvp_own[16]; long pvp_frame;   // the view-projection of the last frame hooked, kept by the host (DM_FRAMEGEN_OWNPREV)
} FG;
static int g_fg_ownprev = 1;   // DM_FRAMEGEN_OWNPREV=0: reproject through the game's own previous view (floats 84-95), as before
static int g_fg_dt, g_fg_cam;   // item 6: DM_FRAMEGEN_DT=1 the measured time between real frames as deltaTime; DM_FRAMEGEN_CAM=1 the camera's matrices to the interpolator, 2 = and no reprojected motion (zero vectors)
// DM_FRAMEGEN_GATE=1|0|ab (on by default; ab alternates with DM_AB): the generator does no work for frames that cannot be shown.
// fg_pace's pause only stops showing: with nothing else, the copies, motion pass, interpolation and second upscale were still paid for every late frame, which made the
// real frames later still. The user's first play of this build (2026-11-13) went from 33 ms a frame to 67, 104 and 257 ms from the moment the generator started: 498
// interpolations paid, 516 generated frames late, 4 shown. The gate, decided on the present thread (fg_pace) and read by the game's thread (fg_frame):
//   closes when two real frames within 30 presents missed their period (over 1.25 periods since the one before), or at once when the present queue is more than two behind;
//   opens when the real frame has room for the generator, not merely when it is in time (at a cap every frame in time shows the same interval, and so does the host's
//     "frame work", which holds the cap's wait; opening on that alone puts the generator back into the overload it was stopped for): the frame's GPU time
//     (nvn_last_gpu_ms, a few frames behind) plus the generator's cost under 95 % of the period, by its smoothed value, for g_fg_need presents without a late one (30). The cost is the difference
//     between the GPU time with the generator and without it as measured here (5 ms until both are known), plus a penalty: a close within 120 presents of an opening
//     shows the opening was wrong, and adds 4 ms (up to 20), so a place that cannot hold it stays closed after one or two tries;
//   a third close within 600 presents doubles g_fg_need (up to 300); 1,800 presents without a close, or 600 closed ones, halve it and the penalty;
//   a late frame in the 30 presents after the generator's first window picture is made does not close it (first-use work; three times at most);
//   closed for over 80 % of 1,800 presents: one line saying frame generation cannot be held here, and it stays closed for the next 1,800.
// It replaces DM_FRAMEGEN_SHED (seedds/nxnative#2289's rule as first tried here: a close only at the second drop within 30 presents, and a resume on "in time" alone)
#define FG_NEED 30
static int g_fg_gate_mode, g_fg_need = FG_NEED; static _Atomic int g_fg_closed; static _Atomic uint64_t g_fg_closes, g_fg_closed_frames;
double nvn_last_gpu_ms(void);   // hle_nvn.c
static int g_fg_overlap, g_fg_real_on_q;   // g_fg_real_on_q: the present thread, around the real frame's present behind a generated one
static const char *g_fg_shot_dir; static long g_fg_shot_a = -1, g_fg_shot_b = -1;
static int fg_on(void) {
    if (g_fg_mode >= 0) return g_fg_mode;
    const char *e = getenv("DM_FRAMEGEN"), *x; int v = e && atoi(e) > 0; g_fg_seen = e != NULL;
    if ((x = getenv("DM_FRAMEGEN_VIEW"))) g_fg_view = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_FLIP"))) g_fg_flip = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_PAUSE"))) g_fg_pause = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_UPTO"))) g_fg_upto = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_OVERLAP"))) g_fg_overlap = atoi(x) > 0;
    if ((x = getenv("DM_FRAMEGEN_DT"))) g_fg_dt = atoi(x) > 0;
    if ((x = getenv("DM_FRAMEGEN_CAM"))) g_fg_cam = atoi(x);
    if ((x = getenv("DM_FRAMEGEN_OWNPREV"))) g_fg_ownprev = atoi(x) > 0;
    if (v) g_fg_gate_mode = sw_mode("DM_FRAMEGEN_GATE", 1);
    if ((x = getenv("DM_FRAMEGEN_SHOT"))) { const char *c = strrchr(x, ':'); if (c && sscanf(c + 1, "%ld-%ld", &g_fg_shot_a, &g_fg_shot_b) == 2) g_fg_shot_dir = strndup(x, (size_t)(c - x)); }
    FG.last_frame = -2; FG.cam_frame = -1; FG.pvp_frame = -2;
    if (v && !(g_q4 && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g_dev])) { MLOG("[RED] frame generation (DM_FRAMEGEN): this GPU has no Metal 4 MetalFX frame interpolator; frames are shown as the game draws them"); v = 0; }
    if (v && rec_on()) { MLOG("[RED] frame generation (scratch build): not with DM_NVN_ENC_THREAD=1; frames are shown as the game draws them"); v = 0; }
    if (v) MLOG("frame generation (DM_FRAMEGEN=1, scratch build): the scene before the HUD is interpolated from depth and the camera, upscaled a second time, and the HUD is kept from the real frame; "
                "pause while late %d, view %d, flip %d, stages up to %d, generator %s", g_fg_pause, g_fg_view, g_fg_flip, g_fg_upto,
                g_fg_overlap ? "on a queue of its own, beside the game's next frame (DM_FRAMEGEN_OVERLAP=1)" : "in the frame's command buffer");
    if (v) MLOG("frame generation inputs: deltaTime %s; camera matrices %s; motion vectors %s, scale 1 (pixels toward the previous position, as the interpolator wants them); jitter 0 (the statistics give the projection's off-centre terms)",
                g_fg_dt ? "measured between real frames (DM_FRAMEGEN_DT=1)" : "1/30 s", g_fg_cam ? "given (DM_FRAMEGEN_CAM)" : "not given", g_fg_cam == 2 ? "zero (DM_FRAMEGEN_CAM=2: the interpolator's own camera motion)" : "reprojected from depth and the camera");
    if (v) MLOG("frame generation gate (DM_FRAMEGEN_GATE %s): no generator work after a real frame missed its period; again once the frame's GPU time leaves room for it for %d frames in a row",
                g_fg_gate_mode == 1 ? "on" : g_fg_gate_mode ? "ab" : "off: the generator works on every frame, shown or not", FG_NEED);
    if (!v && g_fg_seen) MLOG("frame generation off (DM_FRAMEGEN=0): the shown-frame statistics are still logged, for comparison");
    return g_fg_mode = v;
}
// The camera block as the game binds it (encode_args_): the scene camera's binding has far plane 25000; the environment probe's (far 10000, one cube face a frame) is not taken
static void fg_cam_note(const float *c) { if (!fg_on() || !(c[57] > 20000.0f)) return; memcpy(FG.cam, c, sizeof FG.cam); FG.cam_frame = (long)atomic_load(&g_cur_frame); }
// Each render pass (td_note): the last few D32F depth attachments, newest first. The scene depth is the newest of them that holds the scene being upscaled
// (fg_frame); with dynamic resolution the scene is narrower than a shadow map, so "the last large one" is not it
static void fg_depth_note(MTL4RenderPassDescriptor *pd) {
    if (!g_fg_mode || !pd || !fg_on()) return;
    id<MTLTexture> d = pd.depthAttachment.texture;
    if (!d || d.pixelFormat != MTLPixelFormatDepth32Float || d.textureType != MTLTextureType2D || d == FG.depth_seen[0]) return;
    int n = 3; for (int i = 1; i < 4; i++) if (FG.depth_seen[i] == d) { n = i; break; }
    for (int i = n; i > 0; i--) FG.depth_seen[i] = FG.depth_seen[i - 1];
    FG.depth_seen[0] = d;
}
static int fg_inv4(const double a_[16], double r[16]) {
    double a[16]; memcpy(a, a_, sizeof a); for (int i = 0; i < 16; i++) r[i] = i % 5 == 0;
    for (int c = 0; c < 4; c++) { int p = c; for (int k = c + 1; k < 4; k++) if (fabs(a[k * 4 + c]) > fabs(a[p * 4 + c])) p = k;
        if (fabs(a[p * 4 + c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 4; k++) { double t = a[c * 4 + k]; a[c * 4 + k] = a[p * 4 + k]; a[p * 4 + k] = t; t = r[c * 4 + k]; r[c * 4 + k] = r[p * 4 + k]; r[p * 4 + k] = t; }
        double s = 1.0 / a[c * 4 + c]; for (int k = 0; k < 4; k++) { a[c * 4 + k] *= s; r[c * 4 + k] *= s; }
        for (int q = 0; q < 4; q++) if (q != c) { double f = a[q * 4 + c]; if (f != 0) for (int k = 0; k < 4; k++) { a[q * 4 + k] -= f * a[c * 4 + k]; r[q * 4 + k] -= f * r[c * 4 + k]; } } }
    return 1;
}
static void fg_mul4(const double a[16], const double b[16], double r[16]) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; } }
static NSString *const FG_MSL = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V fg_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "struct MP { float4x4 reproj; float2 size; float2 inv_size; };\n"
    "static float vz(float d) { return 1.0 / max(1.0 - d, 1e-7); }\n"
    // Motion in pixels toward the previous position: this pixel's depth reprojected through the previous view-projection; zero where the previous depth there disagrees (a moving
    // object, or a disocclusion): the test of MacNativeGames' mfx_mv (MIT), as the offline test used it
    "fragment float4 fg_mv(V in [[stage_in]], depth2d<float> d [[texture(0)]], depth2d<float> pd [[texture(1)]], constant MP *p [[buffer(0)]]) {\n"
    "  uint2 g = uint2(in.p.xy); float2 uv = (float2(g) + 0.5) * p->inv_size; float d0 = d.read(g); float2 m = 0.0; int still = -1;\n"
    "  float4 c = p->reproj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d0 * 2.0 - 1.0, 1.0);\n"
    "  if (c.w > 1e-7) { float2 n = c.xy / c.w; float2 prev = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y); m = (prev - uv) * p->size; int2 qq = int2(floor(prev * p->size));\n"
    "    if (d0 < 1.0 && all(qq >= int2(1)) && all(qq < int2(p->size) - int2(1))) { float ze = vz(c.z / c.w * 0.5 + 0.5), zmin = 3.4e38, zmax = 0.0; bool nr = false;\n"
    "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) { float z = vz(pd.read(uint2(qq + int2(x, y)))); zmin = min(zmin, z); zmax = max(zmax, z); nr = nr || fabs(z - ze) <= ze * 0.01; }\n"
    "      still = (zmax <= zmin * 1.05 ? (ze >= zmin * 0.99 && ze <= zmax * 1.01) : nr) ? 1 : 0; } }\n"
    "  if (still == 0) m = 0.0; if (any(isnan(m)) || any(isinf(m))) m = 0.0; return float4(m, 0.0, 0.0); }\n"
    "struct CP { int2 org; int2 csize; int flip; int view; int encode; int pad; };\n"
    "static float3 oetf(float3 c) { c = saturate(c); return mix(12.92 * c, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, step(float3(0.0031308), c)); }\n"
    // The generated frame's window picture: win = the real frame's window texture (encoded bytes), gen = the generated scene upscaled (linear), cv = the canvas now (with the HUD),
    // cv0 = the canvas right after the real frame's upscale. A canvas pixel that changed is HUD: it keeps the real frame's picture. Any other takes the real picture plus the
    // difference between the generated and the real scene, both encoded, so a still scene comes out exactly as the real frame whatever the game's last pass does to the colour
    "fragment float4 fg_comp(V in [[stage_in]], texture2d<float> win [[texture(0)]], texture2d<float> gen [[texture(1)]], texture2d<float> cv [[texture(2)]], texture2d<float> cv0 [[texture(3)]], constant CP *p [[buffer(0)]]) {\n"
    "  uint2 g = uint2(in.p.xy); float4 w = win.read(g); int2 q = int2(g) - p->org; if (p->flip != 0) q.y = p->csize.y - 1 - q.y;\n"
    "  if (any(q < int2(0)) || any(q >= p->csize)) return w;\n"
    "  float3 a = cv.read(uint2(q)).rgb; float3 b = cv0.read(uint2(q)).rgb; bool hud = any(a != b);\n"
    "  if (p->view == 2 && hud) return float4(1.0, 0.0, 1.0, 1.0);\n"
    "  if (hud && p->view != 1) return w;\n"
    "  float3 c = gen.read(uint2(q)).rgb;\n"
    "  if (p->encode == 0) return float4(c, w.a);\n"
    "  if (p->view == 1) return float4(oetf(c), w.a);\n"
    "  return float4(saturate(w.rgb + oetf(c) - oetf(b)), w.a); }\n";
static int fg_pipes(void) {
    static int st; if (st) return st > 0;
    st = -1; NSError *e = nil; uint64_t t0 = now_ns();
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:FG_MSL options:nil error:&e];
    if (!lib) { MLOG("[RED] frame generation: its shaders do not compile: %s", e.localizedDescription.UTF8String); return 0; }
    FG.vf = [lib newFunctionWithName:@"fg_vs"]; FG.f_mv = [lib newFunctionWithName:@"fg_mv"]; FG.f_comp = [lib newFunctionWithName:@"fg_comp"];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = FG.vf; pd.fragmentFunction = FG.f_mv; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
    FG.mv_pso = FG.vf && FG.f_mv ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 4; ad.maxBufferBindCount = 1;
    FG.margs = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]); FG.wargs = [g_dev newArgumentTableWithDescriptor:ad error:&e];
    if (!FG.mv_pso || !FG.f_comp || !FG.margs || !FG.wargs) { MLOG("[RED] frame generation: no pipeline or argument table: %s", e.localizedDescription.UTF8String); return 0; }
    MLOG("frame generation: shaders compiled in %.1f ms", (now_ns() - t0) / 1e6);
    st = 1; return 1;
}
static id<MTLTexture> fg_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u, NSString *label) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO]; td.storageMode = MTLStorageModePrivate; td.usage = u;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td]; if (t) { t.label = label; res_add(t); } return t;
}
// The interpolator and its textures for a scene of iw x ih upscaled to ow x oh (made again when a size changes: dynamic resolution)
static int fg_setup(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh, MTLPixelFormat cvf, id<MTL4FXSpatialScaler> sc) {
    if (FG.iw == iw && FG.ih == ih && FG.ow == ow && FG.oh == oh && FG.cvf == cvf) return !FG.bad;
    if (g_fg_overlap && FG.q) { uint64_t t1 = now_ns(); while (atomic_load(&FG.job_done) < FG.job_n && now_ns() - t1 < 500000000ull) usleep(500); }   // a size change: the jobs still read the old textures
    id<MTLTexture> old[] = {FG.c[0], FG.c[1], FG.c[2], FG.d[0], FG.d[1], FG.d[2], FG.mv, FG.gen, FG.gen_up, FG.cv0[0], FG.cv0[1], FG.cv0[2], FG.cv1[0], FG.cv1[1], FG.cv1[2]};
    for (unsigned i = 0; i < sizeof old / sizeof *old; i++) if (old[i]) { res_remove(old[i]); defer_obj_release(old[i]); }
    for (int i = 0; i < 3; i++) FG.c[i] = FG.d[i] = FG.cv0[i] = FG.cv1[i] = nil;
    FG.mv = FG.gen = FG.gen_up = nil; FG.fi = nil; FG.sc2 = nil; FG.p_ok = 0;
    FG.iw = iw; FG.ih = ih; FG.ow = ow; FG.oh = oh; FG.cvf = cvf; FG.bad = 1; FG.warm = 0; FG.prev_ok = 0; FG.gen_ready = 0; FG.last_frame = -2;
    NSError *err = nil; uint64_t t0 = now_ns();
    if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
    d.colorTextureFormat = MTLPixelFormatRGBA16Float; d.outputTextureFormat = MTLPixelFormatRGBA16Float; d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = iw; d.outputHeight = ih;
    id<MTL4FXFrameInterpolator> fi = g_fx_comp ? [d newFrameInterpolatorWithDevice:g_dev compiler:g_fx_comp] : nil;
    if (!fi) { MLOG("[RED] frame generation: MetalFX makes no frame interpolator for %lux%lu RGBA16Float; frames are shown as the game draws them", (unsigned long)iw, (unsigned long)ih); return 0; }
    MTLTextureUsage rd = MTLTextureUsageShaderRead;
    int bad = 0;
    for (int i = 0; i < 3; i++) { FG.c[i] = fg_tex(MTLPixelFormatRGBA16Float, iw, ih, fi.colorTextureUsage | rd | MTLTextureUsageRenderTarget, @"framegen colour"); FG.d[i] = fg_tex(MTLPixelFormatDepth32Float, iw, ih, fi.depthTextureUsage | rd, @"framegen depth");
        FG.cv0[i] = fg_tex(cvf, ow, oh, rd, @"framegen canvas before the HUD"); if (g_fg_overlap) FG.cv1[i] = fg_tex(cvf, ow, oh, rd, @"framegen canvas with the HUD");
        bad |= !FG.c[i] || !FG.d[i] || !FG.cv0[i] || (g_fg_overlap && !FG.cv1[i]); }
    if (g_fg_overlap) {   // a queue, an event and a scaler of the generator's own: the game's scaler draws the next frame on the game's queue meanwhile
        if (!FG.q) {   // a feedback queue of its own: a second descriptor given g_feedback_q released it once too often when it went away (SIGTRAP in dispatch_release at the first set-up)
            static dispatch_queue_t fbq; if (!fbq) fbq = dispatch_queue_create("dm.nvn.framegen-feedback", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
            MTL4CommandQueueDescriptor *qd = [MTL4CommandQueueDescriptor new]; qd.feedbackQueue = fbq; qd.label = @"framegen";
            FG.q = [g_dev newMTL4CommandQueueWithDescriptor:qd error:&err]; FG.ev = [g_dev newSharedEvent]; if (FG.q) [FG.q addResidencySet:g_res]; }
        MTLFXSpatialScalerDescriptor *sd = [MTLFXSpatialScalerDescriptor new];
        sd.colorTextureFormat = MTLPixelFormatRGBA16Float; sd.outputTextureFormat = MTLPixelFormatRGBA16Float; sd.inputWidth = iw; sd.inputHeight = ih; sd.outputWidth = ow; sd.outputHeight = oh;
        sd.colorProcessingMode = sc.colorProcessingMode;
        FG.sc2 = [sd newSpatialScalerWithDevice:g_dev compiler:g_fx_comp];
        if (!FG.q || !FG.ev || !FG.sc2) { MLOG("[RED] frame generation: no queue, event or scaler for the generator's own queue (%s); frames are shown as the game draws them", err ? err.localizedDescription.UTF8String : "-"); return 0; }
    }
    FG.mv = fg_tex(MTLPixelFormatRG16Float, iw, ih, fi.motionTextureUsage | MTLTextureUsageRenderTarget | rd, @"framegen motion");
    FG.gen = fg_tex(MTLPixelFormatRGBA16Float, iw, ih, fi.outputTextureUsage | sc.colorTextureUsage | rd, @"framegen generated scene");
    FG.gen_up = fg_tex(MTLPixelFormatRGBA16Float, ow, oh, sc.outputTextureUsage | rd, @"framegen generated scene, upscaled");
    res_commit();
    if (bad || !FG.mv || !FG.gen || !FG.gen_up) { MLOG("[RED] frame generation: a texture could not be made; frames are shown as the game draws them"); return 0; }
    FG.fi = fi; FG.bad = 0;
    MLOG("frame generation: MetalFX frame interpolator %lux%lu RGBA16Float, generated scene upscaled to %lux%lu (canvas format %lu), made in %.1f ms; about %.0f MB of textures",
         (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, (unsigned long)cvf, (now_ns() - t0) / 1e6, (iw * ih * (8 * 4 + 4 * 3 + 4) + ow * oh * (8 + 4 * (g_fg_overlap ? 6 : 3))) / 1048576.0);
    return 1;
}
static void fg_barrier(void) {   // as up_mfx_encode: MetalFX opens encoders of its own, which take no barrier
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
}
// After the game's upscale of this frame (up_mfx_encode): color = the scene MetalFX read, linear: the scaler's own RGBA16F input, or the game's texture in its own format
// when it went in straight (seedds/nxnative#2559: Ocarina of Time's R11G11B10F); out = the canvas the upscaled scene is now in, written straight or drawn back.
// e: the game's scaler. The generated scene is RGBA16F, so its second upscale takes the RGBA16F scaler of the same sizes (the game's own when that is its format)
static void fg_frame(up_mfx_scaler *e, id<MTLTexture> color, int srgb, id<MTLTexture> out, const NVNcopyRegion *orr, NSUInteger cw, NSUInteger ch) {
    if (!fg_on()) return;
    if (srgb) { static int once; if (!once++) MLOG("[RED] frame generation (scratch build): an sRGB upscale is not hooked; frames are shown as the game draws them"); return; }
    id<MTL4FXSpatialScaler> sc = e->sc;
    if (sc.colorTextureFormat != MTLPixelFormatRGBA16Float || sc.outputTextureFormat != MTLPixelFormatRGBA16Float) {
        up_mfx_scaler *g = up_mfx_scaler_for(cw, ch, (NSUInteger)orr->width, (NSUInteger)orr->height, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float);
        if (!g) { static int once; if (!once++) MLOG("[RED] frame generation: MetalFX makes no RGBA16Float scaler for the generated scene; frames are shown as the game draws them"); return; }
        sc = g->sc;
    }
    atomic_store(&g_fg_hooked, 1);
    long f = (long)atomic_load(&g_cur_frame); NSUInteger ow = (NSUInteger)orr->width, oh = (NSUInteger)orr->height;
    if (orr->xoffset || orr->yoffset || ow != out.width || oh != out.height || cw != color.width || ch != color.height) {
        static int once; if (!once++) MLOG("[RED] frame generation: the upscale is not whole textures (%lux%lu of %lux%lu into %lux%lu of %lux%lu); frames are shown as the game draws them",
                                           (unsigned long)cw, (unsigned long)ch, (unsigned long)color.width, (unsigned long)color.height, (unsigned long)ow, (unsigned long)oh, (unsigned long)out.width, (unsigned long)out.height);
        return; }
    // Not in the first 300 upscaled frames: its shaders and the interpolator are made at first use, and making them under the boot's compile load is what framegen-stage1-v4
    // blames a crash in the shader compiler's cache on (a base M5). A stopgap as that patch's: the pipelines belong in the prebuilt set if this goes further
    { static int seen; if (seen < 300) { seen++; return; } }
    if (!fg_pipes() || !fg_setup(cw, ch, ow, oh, out.pixelFormat, sc)) return;
    if (g_fg_upto < 1) return;
    FG.p_ok = 0;
    // The gate is closed (DM_FRAMEGEN_GATE): nothing is copied or generated for this frame, and the history starts again when it opens
    if (atomic_load(&g_fg_closed) && sw_on(g_fg_gate_mode)) { atomic_fetch_add(&g_fg_closed_frames, 1); if (FG.warm) FG.resets++; FG.warm = 0; FG.prev_ok = 0; FG.gen_ready = 0; FG.last_frame = -2; return; }
    // Overlap: this frame's copies overwrite the ring slot the job of two frames ago read. Not while that job is unfinished (the generator is more than a frame behind):
    // no copy and no generated frame for this one, and the history starts again
    if (g_fg_overlap && FG.job_n - atomic_load(&FG.job_done) >= 2) { FG.gen_behind++; if (FG.warm) FG.resets++; FG.warm = 0; FG.prev_ok = 0; FG.gen_ready = 0; FG.last_frame = -2; return; }
    id<MTLTexture> dep = nil;   // the newest recent depth attachment of exactly the scene's size (seedds/nxnative#2279: with dynamic resolution the scene is 953x536 or 854x480 and the last large one is a shadow map)
    for (int i = 0; i < 4 && !dep; i++) { id<MTLTexture> d = FG.depth_seen[i]; if (d && d.width == cw && d.height == ch) dep = d; }
    { static NSUInteger lw, lh, ldw; static int n; NSUInteger dw = dep ? dep.width : 0;
      if ((lw != cw || lh != ch || ldw != dw) && n < 40) { n++; lw = cw; lh = ch; ldw = dw; char b[96] = ""; int k = 0;
          for (int i = 0; i < 4; i++) if (FG.depth_seen[i]) k += snprintf(b + k, sizeof b - k, " %lux%lu", (unsigned long)FG.depth_seen[i].width, (unsigned long)FG.depth_seen[i].height);
          MLOG("frame generation: scene %lux%lu, depth used %lux%lu, recent depth attachments:%s", (unsigned long)cw, (unsigned long)ch, (unsigned long)dw, (unsigned long)(dep ? dep.height : 0), b); } }
    int have_d = dep != nil, have_cam = FG.cam_frame == f, cur = FG.cur, prev = (cur + 2) % 3;
    double tnow = CACurrentMediaTime(); float dt = 1.0f / 30;
    if (g_fg_dt && FG.last_frame == f - 1 && FG.t_prev > 0) { double d = tnow - FG.t_prev; if (d > 1.0 / 240 && d < 0.25) { dt = (float)d; FG.dt_sum += d; FG.dt_n++; } }
    FG.t_prev = tnow;
    if (have_cam) {
        const float *c = FG.cam, *P = c + 28;   // row-major, column vectors: the view's three rows at floats 0-11, the projection at 28-43, their product at 12-27
        if (!FG.dt_n && !FG.pj[0] && !FG.pj[1]) { FG.pj[0] = FG.pj[1] = P[2]; FG.pj[2] = FG.pj[3] = P[6]; }
        if (P[2] < FG.pj[0]) FG.pj[0] = P[2]; if (P[2] > FG.pj[1]) FG.pj[1] = P[2]; if (P[6] < FG.pj[2]) FG.pj[2] = P[6]; if (P[6] > FG.pj[3]) FG.pj[3] = P[6];
        if (g_fg_cam) {
            double V[16] = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], 0, 0, 0, 1}, Pd[16], VP[16], err = 0;
            for (int i = 0; i < 16; i++) Pd[i] = P[i];
            fg_mul4(Pd, V, VP); for (int i = 0; i < 16; i++) { double e = fabs(VP[i] - c[12 + i]) / (1.0 + fabs(c[12 + i])); if (e > err) err = e; }
            FG.cam_ok = err < 1e-3;
            { static int once; if (!once++) MLOG("%sframe generation: the camera block's view (floats 0-11) and projection (28-43) %s its view-projection (12-27), largest relative difference %.2g%s",
                                                 FG.cam_ok ? "" : "[RED] ", FG.cam_ok ? "multiply to" : "do not multiply to", err, FG.cam_ok ? "; given to the interpolator" : "; the matrices are not given"); }
            for (int cc = 0; cc < 4; cc++) for (int r = 0; r < 4; r++) {
                FG.w2v.columns[cc][r] = (float)V[r * 4 + cc];
                FG.v2c.columns[cc][r] = r == 2 ? 0.5f * (P[2 * 4 + cc] + P[3 * 4 + cc]) : P[r * 4 + cc];   // the game's clip z runs -1..1; the interpolator wants the near plane at 0
            }
        }
    }
    if (!have_d) FG.nodepth++; if (!have_cam) FG.nocam++;
    MTLOrigin o0 = MTLOriginMake(0, 0, 0);
    int drawn = color.pixelFormat != MTLPixelFormatRGBA16Float;   // the game's own format: a draw converts it (a blit copies only between equal formats)
    if (drawn) { NVNcopyRegion at0 = {0, 0, 0, (int)cw, (int)ch, 1};
        if (!copy_t2t_draw(color, FG.c[cur], 0, 0, 0, 0, &at0, &at0, 0, 0)) { static int once; if (!once++) MLOG("[RED] frame generation: the scene (format %lu) could not be drawn into the generator's colour; frames are shown as the game draws them", (unsigned long)color.pixelFormat);
            if (FG.warm) FG.resets++; FG.warm = 0; FG.prev_ok = 0; FG.gen_ready = 0; FG.last_frame = -2; return; }
        args_dirty_all(); }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (!drawn) enc_b_copy_texture(b, color, 0, 0, o0, MTLSizeMake(cw, ch, 1), FG.c[cur], 0, 0, o0);
    if (have_d) enc_b_copy_texture(b, dep, 0, 0, o0, MTLSizeMake(cw, ch, 1), FG.d[cur], 0, 0, o0);
    enc_b_copy_texture(b, out, 0, 0, o0, MTLSizeMake(ow, oh, 1), FG.cv0[cur], 0, 0, o0);
    FG.cv = out; FG.gen_ready = 0; FG.kc = cur;
    int ok = have_d && have_cam && FG.prev_ok && FG.last_frame == f - 1; double m[16];
    if (ok) {   // reprojection = previous view-projection x inverse(view-projection). The previous one is the host's own record of the last frame it hooked (DM_FRAMEGEN_OWNPREV,
        // on by default): the game's "previous view" (floats 84-95 of this frame's block, times the projection) is what its own temporal passes want, and reprojecting through it
        // overshot when the stick was released (read from the code, not reproduced; open until the picture test). =0, or no record of frame f - 1: the game's
        double cam[96], ic[16], pvp[16]; for (int i = 0; i < 96; i++) cam[i] = FG.cam[i];
        double pv[16] = {cam[84], cam[85], cam[86], cam[87], cam[88], cam[89], cam[90], cam[91], cam[92], cam[93], cam[94], cam[95], 0, 0, 0, 1};
        if (g_fg_ownprev && FG.pvp_frame == f - 1) memcpy(pvp, FG.pvp_own, sizeof pvp); else fg_mul4(&cam[28], pv, pvp);
        if (fg_inv4(&cam[12], ic)) fg_mul4(pvp, ic, m); else ok = 0;
    }
    if (have_cam) { for (int i = 0; i < 16; i++) FG.pvp_own[i] = FG.cam[12 + i]; FG.pvp_frame = f; }
    if (ok && g_fg_upto >= 2 && g_fg_overlap) {   // the job is committed at the present (fg_job), behind the whole frame
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) FG.p_m[c * 4 + r] = (float)m[r * 4 + c];
        FG.p_m[16] = cw; FG.p_m[17] = ch; FG.p_m[18] = 1.0f / cw; FG.p_m[19] = 1.0f / ch;
        FG.p_dt = dt; FG.p_cur = cur; FG.p_prev = prev; FG.p_reset = FG.warm == 0; FG.p_near = FG.cam[56]; FG.p_far = FG.cam[57]; FG.p_fov = FG.cam[70] * 57.29578f; FG.p_aspect = FG.cam[62];
        FG.p_ok = 1; FG.made++;
        if (++FG.warm >= 3 && g_fg_upto >= 5) FG.gen_ready = 1;
    } else if (ok && g_fg_upto >= 2) {
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLBuffer> pb; size_t po = arena_alloc(96, &pb); float *p = (float *)((uint8_t *)pb.contents + po);
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) p[c * 4 + r] = (float)m[r * 4 + c];
        p[16] = cw; p[17] = ch; p[18] = 1.0f / cw; p[19] = 1.0f / ch;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = FG.mv; rp.colorAttachments[0].loadAction = g_fg_cam == 2 ? MTLLoadActionClear : MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        ser_gate(); id<MTL4RenderCommandEncoder> en = enc_r_begin(rp);
        [en setArgumentTable:FG.margs atStages:MTLRenderStageFragment];
        [FG.margs setTexture:FG.d[cur].gpuResourceID atIndex:0]; [FG.margs setTexture:FG.d[prev].gpuResourceID atIndex:1]; [FG.margs setAddress:pb.gpuAddress + po atIndex:0];
        [en setViewport:(MTLViewport){0, 0, cw, ch, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, cw, ch}];
        [en setRenderPipelineState:FG.mv_pso]; if (g_fg_cam != 2) [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(en);
        id<MTL4FXFrameInterpolator> fi = FG.fi; id<MTLTexture> c1 = FG.c[cur], c0 = FG.c[prev], d1 = FG.d[cur], mv = FG.mv, gen = FG.gen, gup = FG.gen_up;
        int reset = FG.warm == 0; float nearp = FG.cam[56], farp = FG.cam[57], fov = FG.cam[70] * 57.29578f, aspect = FG.cam[62];
        int give_cam = g_fg_cam && FG.cam_ok; simd_float4x4 w2v = FG.w2v, v2c = FG.v2c;
        if (g_fg_upto >= 3) {
        fg_barrier();
        rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
            fi.colorTexture = c1; fi.prevColorTexture = c0; fi.depthTexture = d1; fi.motionTexture = mv; fi.outputTexture = gen;
            fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0; fi.deltaTime = dt;
            if (give_cam) { if (@available(macOS 27.0, iOS 27.0, *)) { fi.worldToViewMatrix = w2v; fi.viewToClipMatrix = v2c; } }
            fi.nearPlane = nearp; fi.farPlane = farp; fi.fieldOfView = fov; fi.aspectRatio = aspect; fi.depthReversed = NO; fi.shouldResetHistory = reset;
            [fi encodeToCommandBuffer:cb];
        });
        }
        if (g_fg_upto >= 4) {
        fg_barrier();
        rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {   // the generated scene through the game's own upscale, a second time
            sc.colorTexture = gen; sc.outputTexture = gup; sc.inputContentWidth = cw; sc.inputContentHeight = ch;
            [sc encodeToCommandBuffer:cb];
        });
        }
        g_last_end_relaxed = 0; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
        FG.made++;
        if (++FG.warm >= 3 && g_fg_upto >= 5) FG.gen_ready = 1;   // the first two outputs after a new history are the new frame unchanged
    } else if (!ok) { if (FG.warm) FG.resets++; FG.warm = 0; }
    FG.prev_ok = have_d; FG.last_frame = f; FG.cur = (cur + 1) % 3;
}
// The window picture's ring slot and texture for this present, or 0: no generated frame is shown for it
static int fg_win_prep(const tex_t *t, const int crop[4], id<MTLTexture> *raw_, NSUInteger r[4], int *k_, id<MTLTexture> *dst_) {
    if (g_dm_present_link) { static int once; if (!once++) MLOG("[RED] frame generation (scratch build): DM_PRESENT_LINK is not 0, generated frames are not shown (it paces on the present thread only)"); return 0; }
    id<MTLTexture> src = tex_mtl(t); if (!src) return 0;
    id<MTLTexture> raw = present_raw_src(src);
    if (!present_crop_rect(src, crop, r)) return 0;
    if (r[2] != FG.ow || r[3] != FG.oh) { static int once; if (!once++) MLOG("[RED] frame generation: the present shows %lux%lu of the window texture, not the %lux%lu canvas 1:1; frames are shown as the game draws them",
                                                                           (unsigned long)r[2], (unsigned long)r[3], (unsigned long)FG.ow, (unsigned long)FG.oh); return 0; }
    int k = (int)(FG.win_next % FG_WIN);
    pthread_mutex_lock(&g_pq_mu); int busy = FG.win_ticket[k] > g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    if (busy) { FG.busy++; return 0; }
    NSError *e = nil;
    if (!FG.comp_pso || FG.comp_fmt != raw.pixelFormat) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = FG.vf; pd.fragmentFunction = FG.f_comp; pd.colorAttachments[0].pixelFormat = raw.pixelFormat;
        FG.comp_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e]; FG.comp_fmt = raw.pixelFormat;
        if (!FG.comp_pso) { MLOG("[RED] frame generation: no pipeline for the window format %lu: %s", (unsigned long)raw.pixelFormat, e.localizedDescription.UTF8String); return 0; }
    }
    if (!FG.comp_pso) return 0;
    id<MTLTexture> dst = FG.win[k];
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != raw.pixelFormat) {
        dst = fg_tex(raw.pixelFormat, src.width, src.height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, @"framegen window picture");
        if (!dst) { MLOG("[RED] frame generation: no texture for the generated frame's window picture"); return 0; }
        if (!FG.win_serial[k]) { static uint64_t n; FG.win_serial[k] = (1ull << 61) + (1ull << 40) + ++n; }
        serial_put(FG.win_serial[k], dst); FG.win[k] = dst;
        MLOG("frame generation: window picture ring slot %d %lux%lu format %lu", k, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat);
    }
    *raw_ = raw; *k_ = k; *dst_ = dst; return 1;
}
static void fg_comp_params(id<MTLBuffer> buf, const NSUInteger r[4], id<MTLTexture> raw) {
    int32_t *p = (int32_t *)buf.contents; p[0] = (int32_t)r[0]; p[1] = (int32_t)r[1]; p[2] = (int32_t)FG.ow; p[3] = (int32_t)FG.oh; p[4] = g_fg_flip; p[5] = g_fg_view;
    p[6] = raw.pixelFormat == MTLPixelFormatRGBA8Unorm || raw.pixelFormat == MTLPixelFormatBGRA8Unorm; p[7] = 0;
}
static void fg_q_barrier(id<MTL4CommandBuffer> cb) {
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
}
// DM_FRAMEGEN_OVERLAP=1, at the present (the game's thread, the frame's work committed): the job fg_frame left. On the game's queue only a copy of the canvas with the HUD
// and the event's signal; on the generator's queue, behind that event, the motion pass, the interpolation, the second upscale and the window picture. The game's next
// frame does not wait for any of it: the GPU draws both side by side. Metal 4 queues take no priority, so the order alone is set
static uint64_t fg_job(const tex_t *t, const int crop[4], int *slot) {
    if (!FG.p_ok) { FG.gen_ready = 0; return 0; }
    FG.p_ok = 0; int show = FG.gen_ready; FG.gen_ready = 0;
    id<MTLTexture> raw = nil, dst = nil; NSUInteger r[4] = {0}; int k = 0;
    if (show) show = fg_win_prep(t, crop, &raw, r, &k, &dst);
    int j = (int)(FG.job_n % FG_WIN);
    if (!FG.jalloc[j]) {
        NSError *e = nil; MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 4; ad.maxBufferBindCount = 1;
        if (!FG.margs2) FG.margs2 = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        FG.jalloc[j] = [g_dev newCommandAllocator]; FG.jcb[j] = [g_dev newCommandBuffer]; FG.qalloc[j] = [g_dev newCommandAllocator]; FG.qcb[j] = [g_dev newCommandBuffer];
        FG.mpar[j] = [g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared]; FG.jpar[j] = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        if (!FG.margs2 || !FG.jalloc[j] || !FG.jcb[j] || !FG.qalloc[j] || !FG.qcb[j] || !FG.mpar[j] || !FG.jpar[j]) { MLOG("[RED] frame generation: no command allocator or buffer for the generator's job"); FG.jalloc[j] = nil; return 0; }
        res_add(FG.mpar[j]); res_add(FG.jpar[j]);
    }
    res_commit();
    memcpy(FG.mpar[j].contents, FG.p_m, sizeof FG.p_m);
    if (show) fg_comp_params(FG.jpar[j], r, raw);
    NSUInteger cw = FG.iw, ch = FG.ih; int cur = FG.p_cur, prev = FG.p_prev;
    uint64_t n = FG.job_n + 1;
    id<MTL4CommandBuffer> qc = nil;
    if (show) {   // the canvas as the frame left it, for the HUD test: the game's next frame draws over the canvas itself
        [FG.qalloc[j] reset]; qc = FG.qcb[j]; [qc beginCommandBufferWithAllocator:FG.qalloc[j]];
        id<MTL4ComputeCommandEncoder> ce = [qc computeCommandEncoder];
        [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
        [ce copyFromTexture:FG.cv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(FG.ow, FG.oh, 1) toTexture:FG.cv1[j] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
        [ce endEncoding]; [qc endCommandBuffer];
    }
    [FG.jalloc[j] reset]; id<MTL4CommandBuffer> cb = FG.jcb[j]; [cb beginCommandBufferWithAllocator:FG.jalloc[j]];
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = FG.mv; rp.colorAttachments[0].loadAction = g_fg_cam == 2 ? MTLLoadActionClear : MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { [cb endCommandBuffer]; MLOG("[RED] frame generation: no render encoder for the motion pass"); return 0; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [en setArgumentTable:FG.margs2 atStages:MTLRenderStageFragment];
    [FG.margs2 setTexture:FG.d[cur].gpuResourceID atIndex:0]; [FG.margs2 setTexture:FG.d[prev].gpuResourceID atIndex:1]; [FG.margs2 setAddress:FG.mpar[j].gpuAddress atIndex:0];
    [en setViewport:(MTLViewport){0, 0, cw, ch, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, cw, ch}];
    [en setRenderPipelineState:FG.mv_pso]; if (g_fg_cam != 2) [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    if (g_fg_upto >= 3) {
        fg_q_barrier(cb); id<MTL4FXFrameInterpolator> fi = FG.fi;
        fi.colorTexture = FG.c[cur]; fi.prevColorTexture = FG.c[prev]; fi.depthTexture = FG.d[cur]; fi.motionTexture = FG.mv; fi.outputTexture = FG.gen;
        fi.motionVectorScaleX = 1; fi.motionVectorScaleY = 1; fi.jitterOffsetX = 0; fi.jitterOffsetY = 0; fi.deltaTime = FG.p_dt;
        if (g_fg_cam && FG.cam_ok) { if (@available(macOS 27.0, iOS 27.0, *)) { fi.worldToViewMatrix = FG.w2v; fi.viewToClipMatrix = FG.v2c; } }
        fi.nearPlane = FG.p_near; fi.farPlane = FG.p_far; fi.fieldOfView = FG.p_fov; fi.aspectRatio = FG.p_aspect; fi.depthReversed = NO; fi.shouldResetHistory = FG.p_reset;
        [fi encodeToCommandBuffer:cb];
    }
    if (g_fg_upto >= 4) {
        fg_q_barrier(cb); id<MTL4FXSpatialScaler> sc = FG.sc2;
        sc.colorTexture = FG.gen; sc.outputTexture = FG.gen_up; sc.inputContentWidth = cw; sc.inputContentHeight = ch;
        [sc encodeToCommandBuffer:cb];
    }
    if (show) {
        fg_q_barrier(cb);
        rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        en = [cb renderCommandEncoderWithDescriptor:rp];
        if (en) {
            [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
            [en setArgumentTable:FG.wargs atStages:MTLRenderStageFragment];
            [FG.wargs setTexture:raw.gpuResourceID atIndex:0]; [FG.wargs setTexture:FG.gen_up.gpuResourceID atIndex:1]; [FG.wargs setTexture:FG.cv1[j].gpuResourceID atIndex:2]; [FG.wargs setTexture:FG.cv0[cur].gpuResourceID atIndex:3];
            [FG.wargs setAddress:FG.jpar[j].gpuAddress atIndex:0];
            [en setViewport:(MTLViewport){0, 0, dst.width, dst.height, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, dst.width, dst.height}];
            [en setRenderPipelineState:FG.comp_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [en endEncoding];
        } else { MLOG("[RED] frame generation: no render encoder for the window picture"); show = 0; }
    }
    [cb endCommandBuffer];
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
        if (fb.error) { MLOG("[RED] frame generation: generator command buffer error: %s", fb.error.description.UTF8String); gpu_lost_check(fb.error, "frame generation"); }
        static double sum; static int cnt; sum += (fb.GPUEndTime - fb.GPUStartTime) * 1e3;   // the feedback queue is serial
        if (++cnt == 300) { MLOG("frame generation: generator jobs on their own queue, GPU start to end mean %.2f ms over 300", sum / cnt); sum = 0; cnt = 0; }
        uint64_t d = atomic_load(&FG.job_done); while (d < n && !atomic_compare_exchange_weak(&FG.job_done, &d, n)) { }
    }];
    pthread_mutex_lock(&g_res_mu);
    if (qc) [g_q4 commit:&qc count:1];
    [g_q4 signalEvent:FG.ev value:n];
    [FG.q waitForEvent:FG.ev value:n];
    [FG.q commit:&cb count:1 options:o];
    pthread_mutex_unlock(&g_res_mu);
    FG.job_n = n;
    if (!show) return 0;
    *slot = k;
    return FG.win_serial[k];
}
// The queue the present thread commits the real frame's present to: the generator's, behind the generated frame's picture and present, when one was just shown from it
static id<MTL4CommandQueue> fg_present_q(CAMetalLayer *layer) {
    if (!g_fg_real_on_q || !FG.q) return nil;
    if (FG.q_layer != (__bridge void *)layer) { [FG.q addResidencySet:layer.residencySet]; FG.q_layer = (__bridge void *)layer; }
    return FG.q;
}
// At the present (present_enqueue, the game's thread): the generated frame's window picture into a ring texture of the host's, in a command buffer of its own committed
// behind the frame's work. Returns the texture's serial (0 = no generated frame for this present) and its ring slot
static uint64_t fg_compose(const tex_t *t, const int crop[4], int *slot) {
    if (g_fg_mode <= 0) return 0;
    if (g_fg_overlap) return fg_job(t, crop, slot);
    if (!FG.gen_ready) return 0;
    FG.gen_ready = 0;
    id<MTLTexture> raw = nil, dst = nil; NSUInteger r[4]; int k = 0;
    if (!fg_win_prep(t, crop, &raw, r, &k, &dst)) return 0;
    if (!FG.walloc[k]) { FG.walloc[k] = [g_dev newCommandAllocator]; FG.wcb[k] = [g_dev newCommandBuffer]; FG.wpar[k] = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        if (!FG.walloc[k] || !FG.wcb[k] || !FG.wpar[k]) { MLOG("[RED] frame generation: no command allocator or buffer for the window picture"); FG.walloc[k] = nil; return 0; }
        res_add(FG.wpar[k]); }
    res_commit();
    fg_comp_params(FG.wpar[k], r, raw);
    [FG.walloc[k] reset];
    id<MTL4CommandBuffer> cb = FG.wcb[k];
    [cb beginCommandBufferWithAllocator:FG.walloc[k]];
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { [cb endCommandBuffer]; MLOG("[RED] frame generation: no render encoder for the window picture"); return 0; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [en setArgumentTable:FG.wargs atStages:MTLRenderStageFragment];
    [FG.wargs setTexture:raw.gpuResourceID atIndex:0]; [FG.wargs setTexture:FG.gen_up.gpuResourceID atIndex:1]; [FG.wargs setTexture:FG.cv.gpuResourceID atIndex:2]; [FG.wargs setTexture:FG.cv0[FG.kc].gpuResourceID atIndex:3];
    [FG.wargs setAddress:FG.wpar[k].gpuAddress atIndex:0];
    [en setViewport:(MTLViewport){0, 0, dst.width, dst.height, 0, 1}]; [en setScissorRect:(MTLScissorRect){0, 0, dst.width, dst.height}];
    [en setRenderPipelineState:FG.comp_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    [cb endCommandBuffer];
    pthread_mutex_lock(&g_res_mu); [g_q4 commit:&cb count:1]; pthread_mutex_unlock(&g_res_mu);
    *slot = k;
    return FG.win_serial[k];
}
// The present thread: whether this present's generated frame is shown. Not while frames come late or the queue is behind, until three presents in a row came in time
static int fg_pace(double t_in, double period, int waiting) {
    static double prev; static int steady;
    double per = period > 0 ? period : 1.0 / 30; int late = prev > 0 && t_in - prev > per * 1.25; prev = t_in;
    // The hook is the game's upscale drawn by MetalFX. Without one nothing is generated, and that was silent (seedds/nxnative#2279, devMlGUE: 0 generated on main,
    // where Ocarina of Time's agl_ndsr_vr upscaler was not recognised): say so every 3,600 presents while it lasts. Not after 600: the boot and the
    // file menu draw no upscale, and every good run had this red line (fgpin, 11-16: 600 presents at the boot's 60 a second are 10 s, the hook came after it)
    { static uint64_t n; if (g_fg_mode > 0 && !atomic_load(&g_fg_hooked) && !(++n % 3600))
        MLOG("[RED] frame generation (DM_FRAMEGEN=1): %llu presents and no upscale of the game's was drawn by MetalFX, so no frame is generated. It needs DM_UPSCALER=metalfx and "
             "the game's upscaler among the profile's upscaler keys (the log then has \"upscaler: first dispatch of ... drawn by MetalFX\"); %llu upscaler dispatches were left to the game's shader",
             (unsigned long long)n, (unsigned long long)g_up_mfx_kept); }
    if (g_fg_upto < 9) return 0;
    if (g_fg_gate_mode) {   // see DM_FRAMEGEN_GATE
        static uint64_t n, closes_at[3], hold_until, win_n, win_closed, last_close, opened_at, made_open, grace_until, last_late; static int room, graces, had_win; static double on_ms, off_ms, penalty; int drop = late || waiting > 2; n++;
        double per_ms = per * 1e3, g = nvn_last_gpu_ms(); int closed = atomic_load(&g_fg_closed);
        if (!sw_on(g_fg_gate_mode)) { if (closed) atomic_store(&g_fg_closed, 0); room = 0; }   // an ab phase with the gate off
        else {
            // GPU time with the generator and without it (it arrives 8 frames late: not right after a change). "With" only once the generator made frames since the opening:
            // the boot's openings made none, and their 0.7 ms on both sides put the cost at the 2 ms floor for the run (fgpin, 11-16; busy time says 5 to 6.5 ms at 960x540)
            if (g > 0 && g < 4 * per_ms && (closed ? n - last_close > 12 : FG.made > made_open + 12)) { double *m = closed ? &off_ms : &on_ms; *m = *m > 0 ? *m + (g - *m) * 0.05 : g; }
            int measured = on_ms > 0 && off_ms > 0; double cost = (measured ? MIN(MAX(on_ms - off_ms, 4.0), 20.0) : 5.0) + penalty;
            // The generator's first frames after an opening make its window pictures and the compose pipeline (fg_win_prep), and the real frame beside them comes late once.
            // That close shut the gate for the run at 960x540, where the generator holds 59.6 shown a second with the gate off (fgpin and fgpin2, 11-16). So a late frame in
            // the 30 presents after the first of them is made is let through: three times at most, so a place that cannot hold it still closes. (Counted from the
            // opening it ran out before the first generated frame, which waits for its history: fgpin3, 11-16)
            { int w = FG.win[0] != nil; if (w && !had_win && graces < 3) { graces++; grace_until = n + 30; } had_win = w; }
            if (!closed && drop && n < grace_until) drop = 0;
            // One late frame alone does not close it: the route has late frames of its own, the same 9 over 1.25 periods in 3,400 at 960x540 with the generator idle, with the
            // gate off and with it on, and closing at each cost 190 frames a time (28 % generated against 44 % with the gate off; fgpin4, 11-16). The overload the gate is
            // for is late frame after late frame: it closes at the second within 30 presents (fg_pace's pause below still drops the generated frame beside each)
            if (!closed && drop && waiting <= 2) { int second = last_late && n - last_late <= 30; last_late = n; if (!second) drop = 0; }
            if (!closed && drop) {
                atomic_store(&g_fg_closed, 1); closed = 1; room = 0; uint64_t c = atomic_fetch_add(&g_fg_closes, 1) + 1;
                // the opening was wrong, if the generator worked since it: a late frame while it made nothing (the boot, a load, the first 300 upscales) is not its doing. Charged
                // regardless, three closes during a load put the estimate at 10 ms and the gate never opened again in a 9,000-frame run at a GPU time of 25-28 ms (cut9, 11-15)
                if (opened_at && n - opened_at <= 120 && FG.made > made_open + 8) penalty = MIN(penalty + 4.0, 20.0);   // + 8: not for the first-use frames above
                last_close = n; closes_at[0] = closes_at[1]; closes_at[1] = closes_at[2]; closes_at[2] = n;
                if (closes_at[0] && n - closes_at[0] <= 600) g_fg_need = MIN(g_fg_need * 2, FG_NEED * 10);   // it flaps: wait longer before the next try
                if (c <= 20 || !(c & (c - 1))) MLOG("frame generation gate: real frames come late (%s), the generator stops until the frame leaves room for it for %d frames (close #%llu; GPU %.1f ms, generator about %.1f ms %s, period %.1f ms)",
                                                    waiting > 2 ? "the present queue is behind" : "two over 1.25 periods within 30 presents", g_fg_need, (unsigned long long)c, g, cost, measured ? "measured" : "assumed", per_ms);
            } else if (closed) {
                // Room by the smoothed GPU time without the generator (off_ms), not frame by frame: the frame's GPU time is a span with long frames of its own
                // (960x540, generator off: p50 22.8 ms, p90 29.1), which the generator does not lengthen (generator on: p50 26.2, p90 28.3). Asked of each frame, 5 ms
                // of room was there in 79 % of them and 30 in a row, or nine of ten, never came (fgpin3, 11-16). A late frame starts the count again
                room = !drop && n - last_close > 12 && off_ms > 0 && off_ms + cost <= 0.95 * per_ms ? room + 1 : 0;
                // The penalty and the longer wait wear off while closed too (they did so only while open: one wrong opening closed the gate for the rest of a run)
                if (!((n - last_close) % 600) && (g_fg_need > FG_NEED || penalty > 0)) { if (g_fg_need > FG_NEED) g_fg_need /= 2; penalty *= 0.5; if (penalty < 1) penalty = 0; }
                if (room >= g_fg_need && n >= hold_until) { atomic_store(&g_fg_closed, 0); closed = 0; room = 0; opened_at = n; made_open = FG.made;
                    uint64_t c = atomic_load(&g_fg_closes); if (c <= 20 || !(c & (c - 1))) MLOG("frame generation gate: %d frames with room (GPU %.1f ms smoothed + generator about %.1f ms %s in a period of %.1f ms), frames are generated again", g_fg_need, off_ms, cost, measured ? "measured" : "assumed", per_ms); }
            } else if (n - last_close >= 1800 && (g_fg_need > FG_NEED || penalty > 0)) { if (g_fg_need > FG_NEED) g_fg_need /= 2; penalty *= 0.5; if (penalty < 1) penalty = 0; last_close = n; }
            if (n >= hold_until) { win_n++; win_closed += closed; }   // not during a hold: it is closed by the hold, and counting that renewed the hold for the rest of the run
            if (win_n >= 1800) { if (win_closed * 5 > win_n * 4) { hold_until = n + 1800;
                    MLOG("frame generation gate: closed for %llu of the last %llu presents: frame generation cannot be held here (GPU %.1f ms without it, generator about %.1f ms, period %.1f ms); it stays off for the next 1800",
                         (unsigned long long)win_closed, (unsigned long long)win_n, off_ms > 0 ? off_ms : g, cost, per_ms); }
                win_n = win_closed = 0; }
        }
        if (closed && sw_on(g_fg_gate_mode)) { steady = 0; return 0; }   // nothing was generated for this present; after it opens, fg_pace settles for three presents below
    }
    if (!g_fg_pause) return 1;
    if (late || waiting > 2) { steady = 0; if (late) FG.late++; else FG.behind++; return 0; }
    if (steady < 3) { steady++; FG.settling++; return 0; }
    return 1;
}
// Each frame put on screen (the drawable's presented handler): the intervals between shown frames and, for real frames, the time from the game's enqueue to the screen
static void fg_shown(int gen, double tt, double tin) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static double last, first, iv[600], lat[600]; static int n, nl, ngen;
    if (!g_fg_seen || !(tt > 0)) return;
    pthread_mutex_lock(&mu);
    if (last > 0 && tt > last) { if (!n) first = last; iv[n++] = (tt - last) * 1e3; ngen += gen; }
    if (tt > last) last = tt;
    if (!gen && tin > 0 && nl < 600) lat[nl++] = (tt - tin) * 1e3;
    if (n == 600) {
        for (int i = 1; i < n; i++) { double x = iv[i]; int j = i - 1; while (j >= 0 && iv[j] > x) { iv[j + 1] = iv[j]; j--; } iv[j + 1] = x; }
        for (int i = 1; i < nl; i++) { double x = lat[i]; int j = i - 1; while (j >= 0 && lat[j] > x) { lat[j + 1] = lat[j]; j--; } lat[j + 1] = x; }
        int over = 0; for (int i = 0; i < n; i++) over += iv[i] > 25.0;
        MLOG("frame generation %s: 600 shown frames at %.1f a second, %d generated; interval between shown frames p50 %.2f ms, p95 %.2f, max %.2f, over 25 ms %d; game enqueue to screen (real frames) p50 %.1f ms, p95 %.1f; "
             "interpolations %llu, history restarts %llu, no camera %llu, no depth %llu, ring slot busy %llu, generator behind %llu; generated frames not shown: late %llu, queue behind %llu, settling %llu; gate closed %llu times, for %llu frames (DM_FRAMEGEN_GATE)",
             g_fg_mode > 0 ? "on" : "off", 600.0 / (last - first), ngen, iv[299], iv[569], iv[599], over, nl ? lat[nl / 2] : 0.0, nl ? lat[nl * 95 / 100] : 0.0,
             (unsigned long long)FG.made, (unsigned long long)FG.resets, (unsigned long long)FG.nocam, (unsigned long long)FG.nodepth, (unsigned long long)FG.busy, (unsigned long long)FG.gen_behind,
             (unsigned long long)FG.late, (unsigned long long)FG.behind, (unsigned long long)FG.settling, (unsigned long long)atomic_load(&g_fg_closes), (unsigned long long)atomic_load(&g_fg_closed_frames));
        if (g_fg_mode > 0) MLOG("frame generation inputs: deltaTime mean %.2f ms over %llu frames (%s); the projection's off-centre terms x %.6f..%.6f, y %.6f..%.6f since the start (a jittered projection would move them each frame)",
                                FG.dt_n ? FG.dt_sum / FG.dt_n * 1e3 : 1000.0 / 30, (unsigned long long)FG.dt_n, g_fg_dt ? "measured" : "fixed", FG.pj[0], FG.pj[1], FG.pj[2], FG.pj[3]);
        n = nl = ngen = 0;
    }
    pthread_mutex_unlock(&mu);
}
#elif FG_PART == 2
// The generated frame onto the screen, ahead of its real frame (present_main; the present thread takes the drawable itself: DM_PRESENT_LINK=0). Command allocators of its
// own (two, in turn), as #2241 gives the picture after a generated frame one, so neither present waits for the other's GPU work
static id<MTL4CommandAllocator> g_fg_palloc[2]; static id<MTL4CommandBuffer> g_fg_pcb[2]; static id<MTLBuffer> g_fg_puv[2]; static uint64_t g_fg_pfence[2], g_fg_pdone[2], g_fg_pseq;
static int present_gen(const tex_t *t, double hold, const int crop[4]) {
    id<MTLTexture> src = tex_mtl(t);
    if (!src || atomic_load(&g_gpu_lost)) return 0;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
    g_fg_real_on_q = 1; id<MTL4CommandQueue> pq = g_fg_overlap ? fg_present_q(layer) : nil; g_fg_real_on_q = 0;   // overlap: behind the job that draws the picture, on its queue
    if (!pq) { pq = g_q4; if (g_present_layer_resident != layer) { [g_q4 addResidencySet:layer.residencySet]; g_present_layer_resident = layer; } }
    present_pipeline(layer.pixelFormat);
    uint64_t seq = ++g_fg_pseq; int i = (int)(seq & 1);
    if (!g_fg_palloc[i]) { g_fg_palloc[i] = [g_dev newCommandAllocator]; g_fg_pcb[i] = [g_dev newCommandBuffer]; g_fg_puv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_fg_palloc[i] || !g_fg_pcb[i] || !g_fg_puv[i]) { g_fg_palloc[i] = nil; return 0; }
        res_add(g_fg_puv[i]); }
    res_commit();
    id<CAMetalDrawable> dr = [layer nextDrawable];
    if (!dr) return 0;
    if (g_fg_pfence[i]) { pthread_mutex_lock(&g_pq_mu); while (g_fg_pdone[i] < g_fg_pfence[i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu); }
    [g_fg_palloc[i] reset];
    [pq waitForDrawable:dr];
    id<MTL4CommandBuffer> cb = g_fg_pcb[i];
    [cb beginCommandBufferWithAllocator:g_fg_palloc[i]];
    int kind = present_encode_m4(cb, src, dr.texture, crop, g_fg_puv[i], pq == g_q4 ? &g_fx_present : NULL);
    [cb endCommandBuffer];
    if (!kind) { [pq signalDrawable:dr]; return 0; }
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
        if (fb.error) { MLOG("[RED] frame generation: display command buffer error: %s", fb.error.description.UTF8String); gpu_lost_check(fb.error, "display (generated frame)"); }
        pthread_mutex_lock(&g_pq_mu); if (seq > g_fg_pdone[i]) g_fg_pdone[i] = seq; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }];
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    [pq commit:&cb count:1 options:o];
    [pq signalDrawable:dr];
    g_fg_pfence[i] = seq;
    [dr addPresentedHandler:^(id<MTLDrawable> d) { fg_shown(1, d.presentedTime, 0); }];
    if (hold > 0) [dr presentAfterMinimumDuration:hold]; else [dr present];
    return 1;
}
// DM_FRAMEGEN_SHOT: the generated and the real picture of a frame as PNGs (waits for the GPU: a diagnostic)
static void fg_shots(uint64_t frame, const tex_t *gen, const tex_t *real, const int crop[4]) {
    if (!g_fg_shot_dir || (long)frame < g_fg_shot_a || (long)frame > g_fg_shot_b) return;
    char path[1024]; mtl_finish_gpu();
    if (g_fg_overlap && FG.q) { uint64_t t1 = now_ns(), n = FG.job_n; while (atomic_load(&FG.job_done) < n && now_ns() - t1 < 2000000000ull) usleep(500); }   // the generator's queue too: the picture is drawn there
    id<MTLTexture> g = gen ? tex_mtl(gen) : nil, r = tex_mtl(real);
    if (g) { snprintf(path, sizeof path, "%s/F%llu_generated.png", g_fg_shot_dir, (unsigned long long)frame); shot(g, path, 1, crop); }
    if (r) { snprintf(path, sizeof path, "%s/F%llu_real.png", g_fg_shot_dir, (unsigned long long)frame); shot(r, path, 1, crop); }
}
#endif
