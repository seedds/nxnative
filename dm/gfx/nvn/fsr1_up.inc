// ---------- A game's FSR 1 upscale drawn by MetalFX or FSR 4 (DM_UPSCALER=metalfx / fsr4) ----------
// Many games end their 3D frame with AMD FSR 1's EASU: one full-screen triangle that reads the render-resolution scene (texture slot 0, twelve texel fetches) and writes
// the output-resolution target the UI is then drawn over (Tears of the Kingdom: 1600x896 → 1920x1080 docked). The profile names the draw's fragment shader and the game's
// camera uniform block (game.tsv fsr1_upscale, game.py: DM_GAME_FSR1_*). docs/reference/rendering.md, "A game's FSR 1 drawn by MetalFX or FSR 4".
//   metalfx  MetalFX's spatial scaler in HDR mode, the same sizes (up_mfx_scaler_for).
//   fsr4     AMD FSR 4 (the FSR 4.1.2 Metal port, github.com/UzenUPoZiTiV4ik/fsr4-native-macos), loaded at run time: nothing of it is built in. DM_FSR4_ROOT (default
//            ~/Library/Application Support/fsr4-native-macos, where its installer keeps it) holds the model and shaders, DM_FSR4_LIB (default <root>/dist-arm64/libfsr4live.dylib)
//            the arm64 library with fsr4_encode_live. DM_FSR4_PROFILE and DM_FSR4_SHARPNESS reach it as FSR4_PROFILE and FSR4_SHARPNESS (mtl_f1_env, at the host's start).
// FSR 4 is temporal: it takes depth and motion vectors, and these games have none (no TAA). They are made here (f1mv): each pixel's clip position from its depth
// (GL depth mode: stored 0.5 * ndc + 0.5) taken back to the last frame by R = VP_prev * VP_cur^-1. The camera block is the first in the frame bound to the vertex stage of a draw
// into a Depth32Float target that holds the block's render size (dynamic resolution renders into its top-left corner), with a perspective projection (last row 0, 0, -1, 0).
// VP_prev is the block's (DM_GAME_FSR1_VP_PREV), else the last frame's VP kept here. Camera motion only: what moves by itself has none of its own, and FSR 4's history rejection
// takes it. The first FSR 1 draw of a frame is FSR 4's; another in the same frame, a frame with no camera, a missing library or kernel, and FSR 4 after three failed frames
// draw MetalFX. A frame FSR 4 did not encode gets a bilinear scale (f1bil) instead of its output. DM_FSR4_DEBUG=mv draws the motion vectors in place of the picture
// (red: right, green: down, 0.5 still); DM_FSR4_FLIPY=1 takes texture row 0 as the bottom of clip space.
#include <dlfcn.h>
enum { F1_GAME, F1_METALFX, F1_FSR4 };
typedef int (*f1_fsr4_fn)(void *device, void *commandBuffer, void *color, void *depth, void *motion, void *output, void *exposure, uint32_t rw, uint32_t rh, uint32_t uw, uint32_t uh,
                          float jitterX, float jitterY, float mvScaleX, float mvScaleY, float preExposure, int reset, int depthReversed);
static f1_fsr4_fn g_f1_fsr4;
static int g_f1_debug, g_f1_flipy;
static _Atomic int g_f1_fails;   // FSR 4 frames in a row that did not encode (on the encoding thread); 3 turn it off for the run
static char g_f1_lib[1100];      // DM_FSR4_LIB or its default, from mtl_f1_env
static id<MTLComputePipelineState> g_f1_mv_pso, g_f1_dbg_pso, g_f1_bil_pso;
static id<MTL4ArgumentTable> g_f1_tab;
static id<MTLBuffer> g_f1_cbuf; static unsigned g_f1_cslot;   // reprojection constants, a ring of slots (a recorded block reads its slot when the encoding thread runs it)
enum { F1_SLOTS = 8, F1_SLOT = 256, F1_SEEN = 2048, F1_READS = 4096 };
typedef struct { double vp[16], vpp[16]; uint32_t w, h; uint64_t frame; int has_prev; } f1_cam_t;
static f1_cam_t g_f1c;
static id<MTLTexture> g_f1_depth;   // the camera frame's depth target
static double g_f1_last_vp[16]; static uint64_t g_f1_last_vp_frame;   // the last camera seen, for a block without the last frame's view-projection
static uint64_t g_f1_rej_frame; static int g_f1_reads;   // block reads this frame (f1_seen holds the blocks that are no camera)
// FSR 1 draws by what drew them since the last report: FSR 4, MetalFX for want of a camera, MetalFX for a second draw in a frame, MetalFX after FSR 4 failed
enum { F1S_FSR4, F1S_NOCAM, F1S_SECOND, F1S_FAILED, F1S_N };
static uint64_t g_f1_stat[F1S_N], g_f1_stat_frame;
static id<MTLTexture> g_f1_mv, g_f1_out; static uint64_t g_f1_last_frame, g_f1_draw_frame; static uint32_t g_f1_key[4];
static uint64_t g_f1_n;

static const char *f1_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct P { float4x4 R; float2 size; float flipy; float pad; };\n"
    "kernel void f1mv(depth2d<float, access::read> dep [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]], constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  if (g.x >= (uint)p.size.x || g.y >= (uint)p.size.y) return;\n"
    "  float d = dep.read(g);\n"
    "  float2 uv = (float2(g) + 0.5) / p.size;\n"
    "  float4 c = float4(uv.x * 2 - 1, p.flipy > 0 ? uv.y * 2 - 1 : 1 - uv.y * 2, d * 2 - 1, 1);\n"
    "  float4 q = p.R * c;\n"
    "  float2 m = float2(0);\n"
    "  if (abs(q.w) > 1e-6) { float2 n = q.xy / q.w; float2 puv = float2(n.x * 0.5 + 0.5, p.flipy > 0 ? n.y * 0.5 + 0.5 : 0.5 - n.y * 0.5); m = (puv - uv) * p.size; }\n"
    "  mv.write(float4(m, 0, 0), g);\n"
    "}\n"
    "kernel void f1dbg(texture2d<float, access::read> mv [[texture(0)]], texture2d<float, access::write> o [[texture(1)]], constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  if (g.x >= o.get_width() || g.y >= o.get_height()) return;\n"
    "  uint2 s = min(uint2(float2(g) * p.size / float2(o.get_width(), o.get_height())), uint2(p.size) - 1);\n"
    "  o.write(float4(clamp(0.5 + mv.read(s).xy / 32, 0.0, 1.0), 0.5, 1), g);\n"
    "}\n"
    // FSR 4 did not encode this frame: the content rectangle scaled bilinearly, so the game's target never gets an output nobody wrote
    "kernel void f1bil(texture2d<float, access::read> in [[texture(0)]], texture2d<float, access::write> o [[texture(1)]], constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  if (g.x >= o.get_width() || g.y >= o.get_height()) return;\n"
    "  float2 s = (float2(g) + 0.5) * p.size / float2(o.get_width(), o.get_height()) - 0.5;\n"
    "  int2 i = int2(floor(s)), hi = int2(p.size) - 1; float2 f = s - float2(i);\n"
    "  float4 a = in.read(uint2(clamp(i, int2(0), hi))), b = in.read(uint2(clamp(i + int2(1, 0), int2(0), hi)));\n"
    "  float4 c = in.read(uint2(clamp(i + int2(0, 1), int2(0), hi))), d = in.read(uint2(clamp(i + int2(1, 1), int2(0), hi)));\n"
    "  o.write(float4(mix(mix(a, b, f.x), mix(c, d, f.x), f.y).rgb, 1), g);\n"
    "}\n";

// At the host's start, before any thread (main.c): the FSR 4 switches as the library reads them. Here and not at the first draw, as setenv may move environ under a getenv
void mtl_f1_env(void) {
    if (!DM_GAME_FSR1_N) return;
    char rbuf[1024]; const char *root = getenv("DM_FSR4_ROOT"), *lib = getenv("DM_FSR4_LIB"), *home = getenv("HOME"), *e;
    if (!root || !*root) root = getenv("FSR4_ROOT");
    if ((!root || !*root) && home) { snprintf(rbuf, sizeof rbuf, "%s/Library/Application Support/fsr4-native-macos", home); root = rbuf; }
    if (lib && *lib) snprintf(g_f1_lib, sizeof g_f1_lib, "%s", lib); else if (root) snprintf(g_f1_lib, sizeof g_f1_lib, "%s/dist-arm64/libfsr4live.dylib", root);
    if (root) setenv("FSR4_ROOT", root, 1);   // the library finds its model, shaders and compiler there
    if ((e = getenv("DM_FSR4_PROFILE")) && *e) setenv("FSR4_PROFILE", e, 1);
    if ((e = getenv("DM_FSR4_SHARPNESS")) && *e) setenv("FSR4_SHARPNESS", e, 1);
}

// The player's choice for this game: F1_GAME (the game's shader), F1_METALFX or F1_FSR4; FSR 4 that cannot load draws MetalFX
static int f1_mode(void) {
    static int v = -1;
    if (v >= 0) return v;
    v = F1_GAME;
    if (!DM_GAME_FSR1_N) return v;
    const char *e = getenv("DM_UPSCALER"); if (!e || !*e) e = DM_GAME_UPSCALER_DEFAULT;
    int want = !strcmp(e, "fsr4") ? F1_FSR4 : !strcmp(e, "metalfx") ? F1_METALFX : F1_GAME;
    if (want == F1_GAME) { MLOG("upscaler (DM_UPSCALER): game, the game's FSR 1 draws"); return v; }
    if (!g_q4 || ![MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) { MLOG("upscaler: this GPU has no Metal 4 MetalFX; the game's FSR 1 draws"); return v; }
    g_f1_debug = (e = getenv("DM_FSR4_DEBUG")) && !strcmp(e, "mv");
    g_f1_flipy = (e = getenv("DM_FSR4_FLIPY")) && *e == '1';
    if (want == F1_FSR4 && !g_f1_debug) {
        void *h = g_f1_lib[0] ? dlopen(g_f1_lib, RTLD_NOW | RTLD_LOCAL) : NULL;
        g_f1_fsr4 = h ? (f1_fsr4_fn)dlsym(h, "fsr4_encode_live") : NULL;
        if (!g_f1_fsr4) { MLOG("[RED] upscaler: FSR 4 not found (%s: %s; DM_FSR4_ROOT / DM_FSR4_LIB); MetalFX draws the game's FSR 1 instead", g_f1_lib[0] ? g_f1_lib : "-", dlerror() ?: "no fsr4_encode_live"); want = F1_METALFX; }
    }
    if (want == F1_FSR4 || g_f1_debug) {
        NSError *err = nil;
        id<MTLLibrary> L = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:f1_msl] options:nil error:&err];
        g_f1_mv_pso = L ? [g_dev newComputePipelineStateWithFunction:[L newFunctionWithName:@"f1mv"] error:&err] : nil;
        g_f1_dbg_pso = L ? [g_dev newComputePipelineStateWithFunction:[L newFunctionWithName:@"f1dbg"] error:&err] : nil;
        g_f1_bil_pso = L ? [g_dev newComputePipelineStateWithFunction:[L newFunctionWithName:@"f1bil"] error:&err] : nil;
        MTL4ArgumentTableDescriptor *td = [MTL4ArgumentTableDescriptor new]; td.maxTextureBindCount = 2; td.maxBufferBindCount = 1;
        g_f1_tab = [g_dev newArgumentTableWithDescriptor:td error:&err];
        g_f1_cbuf = [g_dev newBufferWithLength:F1_SLOTS * F1_SLOT options:MTLResourceStorageModeShared];
        if (!g_f1_mv_pso || !g_f1_dbg_pso || !g_f1_bil_pso || !g_f1_tab || !g_f1_cbuf) { MLOG("[RED] upscaler: the motion-vector kernels were not made (%s); MetalFX draws instead", err ? err.localizedDescription.UTF8String : "-"); want = F1_METALFX; g_f1_debug = 0; }
        else { res_add(g_f1_cbuf); res_commit(); }
    }
    v = want;
    MLOG("upscaler (DM_UPSCALER): %s, the game's FSR 1 draw (%s) %s%s", v == F1_FSR4 ? "fsr4" : "metalfx", DM_GAME_FSR1_KEY,
         g_f1_debug ? "shows the motion vectors (DM_FSR4_DEBUG=mv)" : v == F1_FSR4 ? "is drawn by FSR 4 with motion vectors from the camera" : "is drawn by MetalFX's spatial scaler", g_f1_flipy ? ", rows bottom-up (DM_FSR4_FLIPY=1)" : "");
    return v;
}

static void f1_inv(const double m[16], double o[16]) {   // 4x4 inverse, row-major, Gauss-Jordan with partial pivoting; 0 when singular
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) { memset(o, 0, 16 * sizeof *o); return; }
        if (p != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        double d = a[c][c]; for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i][j + 4];
}

// n bytes of a uniform block as the GPU will see them: the game's inline updates still waiting in this submission first (as ubo_float); 0 when they cover it in part
static int f1_read(uint64_t a, uint32_t n, void *out) {
    int kind; if (pend_read(a, n, out, &kind)) return 1;
    if (pend_hits(a, n)) return 0;
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(a, n, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, n); return 1;
}

// Every draw until the frame has one: the frame's camera, the first block bound to the vertex stage of a draw into a Depth32Float target that holds the block's render size
// A frame's blocks known to be no camera: an open-addressed set stamped with the frame, so a busy frame (thousands of draws, as many
// uniform blocks) reads each block once and never runs out of room before the draw that holds the camera
static int f1_seen(uint64_t a, uint64_t fr, int add) {
    static struct { uint64_t a, fr; } set[F1_SEEN];
    for (uint32_t i = (uint32_t)(a >> 4) * 2654435761u, n = 0; n < 16; n++, i++) {
        __typeof__(set[0]) *e = &set[i & (F1_SEEN - 1)];
        if (e->fr == fr && e->a == a) return 1;
        if (e->fr != fr) { if (add) { e->a = a; e->fr = fr; } return 0; }
    }
    return 0;   // a full probe: read it again, which is only slower
}
static void f1_track(void) {
    if (!R.has_rt || !R.rt.depth) return;
    uint64_t fr = atomic_load(&g_cur_frame);
    if (g_f1c.frame == fr) return;
    if (g_f1_rej_frame != fr) { g_f1_rej_frame = fr; g_f1_reads = 0; }
    if (g_f1_reads >= F1_READS) return;   // a frame with no camera at all (menus, loading) stops looking after this many reads
    const tex_t *dt = R.rt.depth; uint32_t w = dt->d.width, h = dt->d.height;
    // Only a draw into a readable single-sample Depth32Float target can carry the camera: decided once per depth target
    static uint64_t dser; static int dok; static id<MTLTexture> dtex;
    if (dt->serial != dser) {
        id<MTLTexture> m = tex_mtl(dt); dser = dt->serial; dtex = m;
        dok = m && (m.pixelFormat == MTLPixelFormatDepth32Float || m.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) && (m.usage & MTLTextureUsageShaderRead) && m.sampleCount == 1;
    }
    if (!dok) return;
    const uint32_t need = MAX(MAX(DM_GAME_FSR1_SIZE + 8, DM_GAME_FSR1_PROJ + 64), MAX(DM_GAME_FSR1_VP + 64, DM_GAME_FSR1_VP_PREV + 64));
    if (need > 0x1000) return;
    for (int r = 0; r < 16; r++) {
        const p_bind_buf *u = &S.ubo[0][r];
        if (!u->addr || u->size < need || f1_seen(u->addr, fr, 0)) continue;
        float f[0x1000 / 4];
        g_f1_reads++;
        if (!f1_read(u->addr, need, f)) continue;
        const float *sz = f + DM_GAME_FSR1_SIZE / 4, *px = f + DM_GAME_FSR1_PROJ / 4, *pw = px + 12;
        if (!(pw[0] == 0 && pw[1] == 0 && pw[2] == -1 && pw[3] == 0 && px[0] > 0)) { f1_seen(u->addr, fr, 1); continue; }   // no camera, whatever the target
        // A camera of another view (a shadow or a smaller pass) stays a candidate for a later draw: only this target's size is checked
        if (!(sz[0] >= 1 && sz[1] >= 1 && sz[0] == (float)(uint32_t)sz[0] && sz[1] == (float)(uint32_t)sz[1] && sz[0] <= w && sz[1] <= h && sz[0] * 2 >= w && sz[1] * 2 >= h)) continue;
        id<MTLTexture> dm = dtex;
        for (int i = 0; i < 16; i++) g_f1c.vp[i] = f[DM_GAME_FSR1_VP / 4 + i];
        if (DM_GAME_FSR1_VP_PREV) { for (int i = 0; i < 16; i++) g_f1c.vpp[i] = f[DM_GAME_FSR1_VP_PREV / 4 + i]; g_f1c.has_prev = 1; }
        else { g_f1c.has_prev = g_f1_last_vp_frame + 1 == fr; memcpy(g_f1c.vpp, g_f1c.has_prev ? g_f1_last_vp : g_f1c.vp, sizeof g_f1c.vpp); }
        memcpy(g_f1_last_vp, g_f1c.vp, sizeof g_f1_last_vp); g_f1_last_vp_frame = fr;
        g_f1c.w = (uint32_t)sz[0]; g_f1c.h = (uint32_t)sz[1]; g_f1c.frame = fr; g_f1_depth = dm;
        { static int once; if (!once++) MLOG("upscaler: camera block at %#llx (vertex uniform %d), render %ux%u in a %ux%u depth target (format %lu)", (unsigned long long)u->addr, r, g_f1c.w, g_f1c.h, w, h, (unsigned long)dm.pixelFormat); }
        return;
    }
}

static id<MTLTexture> f1_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u, NSString *label) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = u;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td]; t.label = label;
    if (t) res_add(t);
    return t;
}

// The game's FSR 1 draw through MetalFX or FSR 4: 1 when drawn, 0 to run the game's shader
static int f1_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    int mode = f1_mode();
    if (mode == F1_GAME) return 0;
    f1_track();   // also for MetalFX: the camera says how much of the input dynamic resolution filled
    NvShader *fs = pr ? pr->sh[1] : NULL;
    if (!fs || strncmp(fs->key, DM_GAME_FSR1_KEY, 16)) return 0;
    // One full-screen triangle, indexed (Tears of the Kingdom) or not
    uint64_t ind = dw ? dw->ind : di ? di->ind : 1; int cnt = dw ? (int)dw->count : di ? (int)di->count : 0, inst = dw ? (int)dw->instances : di ? (int)di->instances : 0;
    if (ind || cnt != 3 || inst != 1 || !R.has_rt || R.rt.n < 1 || !R.rt.color[0]) return 0;
    int ti = -1;
    for (int i = 0; i < fs->rf.nb; i++) if (fs->rf.b[i].kind == NVN_RB_TEX) { if (ti >= 0) return 0; ti = i; }
    if (ti < 0) return 0;
    int tid = (int)(tex_slot_h(S.tex[1], S.img[1], fs->rf.b[ti].reg) & 0xfffff);
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> in = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? tex_by_id(tid) : nil;
    os_unfair_lock_unlock(&g_rmu);
    id<MTLTexture> out = tex_mtl(R.rt.color[0]);
    int lv = R.rt.has_cview[0] && R.rt.cview[0].set_levels ? R.rt.cview[0].base_level : 0, sl = R.rt.has_cview[0] && R.rt.cview[0].set_layers ? R.rt.cview[0].min_layer : 0;
    if (!in || !out || in.textureType != MTLTextureType2D || in.sampleCount != 1 || out.textureType != MTLTextureType2D || lv || sl || out.width <= in.width || out.height <= in.height ||
        !(out.usage & MTLTextureUsageRenderTarget) || fmt_int_or_ds(in.pixelFormat) || fmt_int_or_ds(out.pixelFormat) || mtl_fmt_block(out.pixelFormat)) {
        static int once; if (!once++) MLOG("upscaler: the FSR 1 draw's textures are not a shape the swap takes (in %lux%lu, out %lux%lu); the game's shader draws",
                                         (unsigned long)in.width, (unsigned long)in.height, (unsigned long)out.width, (unsigned long)out.height);
        return 0;
    }
    uint64_t fr = atomic_load(&g_cur_frame);
    int cam = g_f1c.frame == fr && g_f1c.w <= in.width && g_f1c.h <= in.height;
    // The input rectangle: the camera's render size in the top-left corner (dynamic resolution), else the whole texture
    uint32_t rw = cam ? g_f1c.w : (uint32_t)in.width, rh = cam ? g_f1c.h : (uint32_t)in.height, uw = (uint32_t)out.width, uh = (uint32_t)out.height;
    int temporal = (mode == F1_FSR4 && atomic_load(&g_f1_fails) < 3) || g_f1_debug;
    int second = g_f1_draw_frame == fr;
    if (mode == F1_FSR4) g_f1_stat[atomic_load(&g_f1_fails) >= 3 ? F1S_FAILED : !cam ? F1S_NOCAM : second ? F1S_SECOND : F1S_FSR4]++;
    if (fr - g_f1_stat_frame >= 600 && mode == F1_FSR4) {   // how the FSR 1 draws went, every 600 frames
        MLOG("upscaler fsr4 up to frame %llu: %llu by FSR 4, MetalFX %llu without a camera, %llu as a second draw in a frame, %llu after FSR 4 failed; camera search %d block reads in the last frame",
             (unsigned long long)fr, (unsigned long long)g_f1_stat[F1S_FSR4], (unsigned long long)g_f1_stat[F1S_NOCAM], (unsigned long long)g_f1_stat[F1S_SECOND], (unsigned long long)g_f1_stat[F1S_FAILED], g_f1_reads);
        memset(g_f1_stat, 0, sizeof g_f1_stat); g_f1_stat_frame = fr;
    }
    if (temporal && (!cam || second)) temporal = 0;   // no camera (a loading screen) or a second FSR 1 draw this frame: MetalFX draws it
    g_f1_draw_frame = fr;
    NVNcopyRegion iall = {0, 0, 0, (int)rw, (int)rh, 1}, all = {0, 0, 0, (int)uw, (int)uh, 1};
    zskip_del(R.rt.color[0]->serial);
    if (!temporal) {   // MetalFX's spatial scaler in HDR mode, through its RGBA16F textures (as up_mfx_encode)
        up_mfx_scaler *e = up_mfx_scaler_for(rw, rh, uw, uh, 0); if (!e) return 0;
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        ubd_land_compute();
        if (!copy_t2t_draw(in, e->in, 0, 0, 0, 0, &iall, &iall, 0, 0)) { args_dirty_all(); return 0; }
        id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];   // as up_mfx_encode: MetalFX opens encoders of its own, which take no barrier
        [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
        [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
        [ce endEncoding];
        id<MTL4FXSpatialScaler> sc = e->sc; id<MTLTexture> ein = e->in, eout = e->out;
        rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) { sc.colorTexture = ein; sc.outputTexture = eout; sc.inputContentWidth = rw; sc.inputContentHeight = rh; [sc encodeToCommandBuffer:cb]; });
        g_last_end_relaxed = 0;
        if (!copy_t2t_draw(e->read, out, 0, 0, 0, 0, &all, &all, 0, 0)) { args_dirty_all(); return 0; }
        args_dirty_all();
        if (!g_f1_n++) MLOG("upscaler: first FSR 1 draw of %s drawn by MetalFX, %ux%u of %lux%lu (format %lu) → %ux%u (format %lu)", fs->key, rw, rh, (unsigned long)in.width, (unsigned long)in.height,
                            (unsigned long)in.pixelFormat, uw, uh, (unsigned long)out.pixelFormat);
        return 1;
    }
    int reset = g_f1_last_frame + 1 != fr || !g_f1c.has_prev;
    if (g_f1_key[0] != rw || g_f1_key[1] != rh || g_f1_key[2] != uw || g_f1_key[3] != uh || !g_f1_mv || !g_f1_out) {
        if (g_f1_mv) { res_remove(g_f1_mv); defer_obj_release(g_f1_mv); } if (g_f1_out) { res_remove(g_f1_out); defer_obj_release(g_f1_out); }
        g_f1_mv = f1_tex(MTLPixelFormatRG16Float, rw, rh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, @"fsr4 motion");
        g_f1_out = f1_tex(MTLPixelFormatRGBA16Float, uw, uh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget, @"fsr4 output");
        res_commit();
        g_f1_key[0] = rw; g_f1_key[1] = rh; g_f1_key[2] = uw; g_f1_key[3] = uh; reset = 1;
        if (!g_f1_mv || !g_f1_out) { g_f1_key[0] = 0; return 0; }
        MLOG("upscaler fsr4: %ux%u → %ux%u (input %lux%lu format %lu, output format %lu)", rw, rh, uw, uh, (unsigned long)in.width, (unsigned long)in.height, (unsigned long)in.pixelFormat, (unsigned long)out.pixelFormat);
    }
    g_f1_last_frame = fr;
    // R = VP_prev * VP_cur^-1, as a Metal float4x4 (column-major)
    double inv[16], Rm[16];
    f1_inv(g_f1c.vp, inv);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += g_f1c.vpp[i * 4 + k] * inv[k * 4 + j]; Rm[i * 4 + j] = s; }
    unsigned slot = g_f1_cslot++ % F1_SLOTS; float *cbv = (float *)((uint8_t *)g_f1_cbuf.contents + slot * F1_SLOT);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) cbv[j * 4 + i] = (float)Rm[i * 4 + j];
    cbv[16] = (float)rw; cbv[17] = (float)rh; cbv[18] = g_f1_flipy ? 1.f : 0.f; cbv[19] = 0;
    uint64_t caddr = g_f1_cbuf.gpuAddress + (uint64_t)slot * F1_SLOT;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    id<MTLTexture> mv = g_f1_mv, res = g_f1_out, depth = g_f1_depth; id<MTLComputePipelineState> mpso = g_f1_mv_pso, dpso = g_f1_dbg_pso, bpso = g_f1_bil_pso; id<MTL4ArgumentTable> tab = g_f1_tab;
    f1_fsr4_fn enc = g_f1_fsr4; int dbg = g_f1_debug; id<MTLDevice> dev = g_dev;
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> c) {   // the real command buffer, on the encoding thread, in order with the recorded encoders
        id<MTL4ComputeCommandEncoder> ce = [c computeCommandEncoder];
        [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
        [ce setComputePipelineState:mpso]; [ce setArgumentTable:tab];
        [tab setTexture:depth.gpuResourceID atIndex:0]; [tab setTexture:mv.gpuResourceID atIndex:1]; [tab setAddress:caddr atIndex:0];
        [ce dispatchThreads:MTLSizeMake(rw, rh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        if (dbg) {
            [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageDispatch visibilityOptions:resource_vis()];
            [ce setComputePipelineState:dpso];
            [tab setTexture:mv.gpuResourceID atIndex:0]; [tab setTexture:res.gpuResourceID atIndex:1];
            [ce dispatchThreads:MTLSizeMake(uw, uh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }
        [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
        [ce endEncoding];
        if (dbg) return;
        if (enc((__bridge void *)dev, (__bridge void *)c, (__bridge void *)in, (__bridge void *)depth, (__bridge void *)mv, (__bridge void *)res, NULL, rw, rh, uw, uh, 0, 0, 1, 1, 1, reset, 0)) { atomic_store(&g_f1_fails, 0); return; }
        // Not encoded (its log: FSR4_HOOK_LOG): the output is the content scaled bilinearly; three frames in a row hand the rest of the run to MetalFX
        int n = atomic_fetch_add(&g_f1_fails, 1) + 1;
        if (n <= 3) MLOG("[RED] upscaler fsr4: FSR 4 did not encode a frame (%d in a row)%s", n, n == 3 ? "; MetalFX draws the game's FSR 1 from now on" : ", drawn bilinearly");
        id<MTL4ComputeCommandEncoder> be = [c computeCommandEncoder];
        [be barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
        [be setComputePipelineState:bpso]; [be setArgumentTable:tab];
        [tab setTexture:in.gpuResourceID atIndex:0]; [tab setTexture:res.gpuResourceID atIndex:1]; [tab setAddress:caddr atIndex:0];
        [be dispatchThreads:MTLSizeMake(uw, uh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [be barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
        [be endEncoding];
    });
    g_last_end_relaxed = 0;   // the next pass waits for FSR 4 whole (enc_r_begin_pass)
    if (!copy_t2t_draw(res, out, 0, 0, 0, 0, &all, &all, 0, 0)) { args_dirty_all(); return 0; }
    args_dirty_all();
    if (!g_f1_n++) MLOG("upscaler: first FSR 1 draw of %s drawn by FSR 4, %ux%u of %lux%lu (format %lu) → %ux%u (format %lu)%s", fs->key, rw, rh, (unsigned long)in.width, (unsigned long)in.height,
                        (unsigned long)in.pixelFormat, uw, uh, (unsigned long)out.pixelFormat, dbg ? ", motion vectors shown" : "");
    return 1;
}
