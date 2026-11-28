// The stereo tap (nvn_mtl.m, mtl_stereo.h) and the headset's pass (dm/host/immersive_mac.m, msl/stereo.metal), without a headset: a frame whose scene has a far third, a middle
// and a small near patch is tapped as a game's would be, and each eye's picture must show the far marker behind the screen, the middle one on it and the near one in front.
// Then a HUD is drawn over a window texture through the replay (st_overlay): it must stand in front of the scene, whole, and leave no copy of itself where the scene moves
#include "../nvn/nvn_mtl.m"
#include "../../host/immersive_mac.h"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void dm_immersive_scene_open(void) {} void dm_immersive_menu(void) {}
static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
enum { W = 1024, H = 576 };
static id<MTLCommandQueue> g_tq;
static void fill(id<MTLTexture> t, const void *bytes, NSUInteger bpp) {
    id<MTLBuffer> b = [g_dev newBufferWithBytes:bytes length:t.width * t.height * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_tq commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:t.width * bpp sourceBytesPerImage:t.width * t.height * bpp sourceSize:MTLSizeMake(t.width, t.height, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static id<MTLTexture> make(MTLPixelFormat pf, NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; res_add(t); return t;
}
// A frame as the replay sees it: a pass over colour and depth with n depth-writing draws, then the present
static uint64_t frame_scene(id<MTLTexture> color, id<MTLTexture> depth, int n, int func, id<MTLTexture> decoy, id<MTLTexture> scene) {
    MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
    S.ds.dt = S.ds.dw = 1; S.ds.df = func; S.has_vp = 1; S.vp[0][0] = S.vp[0][1] = 0;
    if (decoy) { pd.colorAttachments[0].texture = color; pd.depthAttachment.texture = decoy; S.vp[0][2] = decoy.width; S.vp[0][3] = decoy.height; st_pass(pd); for (int i = 0; i < n / 2; i++) st_draw(); }
    if (depth) { pd.colorAttachments[0].texture = scene; pd.depthAttachment.texture = depth; S.vp[0][2] = W; S.vp[0][3] = H; st_pass(pd); for (int i = 0; i < n; i++) st_draw(); }
    pd.colorAttachments[0].texture = nil; pd.depthAttachment.texture = decoy ?: depth; st_pass(pd); for (int i = 0; i < 4 * n; i++) st_draw();   // a shadow map's pass: no colour, never the scene
    res_commit();
    uint64_t seq0 = g_st_pub.seq;
    tex_t none = {0}; g_st_tag = st_take_depth(&none, NULL); uint64_t tag = g_st_tag;
    st_present(color, NULL);
    for (int i = 0; i < 400 && g_st_pub.seq == seq0; i++) usleep(5000);
    CHECK(g_st_pub.seq != seq0, "a present's picture is published within 2 s");
    return tag;
}
static uint64_t frame(id<MTLTexture> color, id<MTLTexture> depth, int n, int func, id<MTLTexture> decoy) {
    return frame_scene(color, depth, n, func, decoy, color);
}
// The centre column of one channel's brightness in columns x0..x1 of a row of an eye's picture
static double column_in(const uint8_t *px, int eye, int row, int ch, int x0, int x1) {
    const uint8_t *r = px + ((size_t)eye * H + (size_t)row) * W * 4; double sum = 0, n = 0;
    for (int x = x0; x < x1; x++) { double v = r[x * 4 + ch]; sum += v * x; n += v; }
    return n > 0 ? sum / n : -1;
}
// A game texture of the picture's size, which the replay can draw into
static id<MTLTexture> game_tex(tex_t *t, uint64_t serial) {
    *t = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .d = {.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1}};
    MTLTextureDescriptor *td = tex_desc(t, 0); td.usage |= MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    id<MTLTexture> x = [g_dev newTextureWithDescriptor:td]; res_add(x); mtl_tex_init(t); serial_put(t->serial, x); return x;
}
// The frame's scene (40 depth-writing draws over colour and depth), then the present of the window texture
static void present_win(const tex_t *wt, id<MTLTexture> win, id<MTLTexture> color, id<MTLTexture> depth) {
    MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new]; pd.colorAttachments[0].texture = color; pd.depthAttachment.texture = depth;
    S.ds.dt = S.ds.dw = 1; S.ds.df = NVN_DEPTH_FUNC_LEQUAL; S.has_vp = 1; S.vp[0][0] = S.vp[0][1] = 0; S.vp[0][2] = W; S.vp[0][3] = H;
    st_pass(pd); for (int i = 0; i < 40; i++) st_draw();
    res_commit();
    uint64_t seq0 = g_st_pub.seq;
    g_st_tag = st_take_depth(wt, NULL);
    st_present(win, NULL);
    for (int i = 0; i < 400 && g_st_pub.seq == seq0; i++) usleep(5000);
}
static double column(const uint8_t *px, int eye, int row, int ch) { return column_in(px, eye, row, ch, 0, W); }
int main(void) { @autoreleasepool {
    mtl_init(); g_tq = [g_dev newCommandQueue];
    mtl_stereo_enable(1);
    mtl_stereo_frame f;
    CHECK(!mtl_stereo_acquire(&f), "no frame before the first present");
    // Colour (RGBA): a red line in the far third, a blue one in the middle, a green one in the near patch. Depth: 1 (far), 0.8, 0.2
    uint8_t *c = calloc(W * H, 4); float *z = malloc(W * H * sizeof *z), *zr = malloc(W * H * sizeof *zr);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int band = y / (H / 3), near = band == 2 && x >= 448 && x < 576;
        z[y * W + x] = band == 0 ? 1 : near ? 0.2f : 0.8f; zr[y * W + x] = 1 - z[y * W + x];
        if (band == 0 && x >= 198 && x < 203) c[(y * W + x) * 4] = 255;
        if (band == 1 && x >= 798 && x < 803) c[(y * W + x) * 4 + 2] = 255;
        if (near && x >= 510 && x < 515) c[(y * W + x) * 4 + 1] = 255;
    }
    id<MTLTexture> color = make(MTLPixelFormatRGBA8Unorm, W, H), depth = make(MTLPixelFormatDepth32Float, W, H), rdepth = make(MTLPixelFormatDepth32Float, W, H), decoy = make(MTLPixelFormatDepth32Float, 64, 64);
    fill(color, c, 4); fill(depth, z, 4); fill(rdepth, zr, 4);
    for (int pass = 0; pass < 2; pass++) {   // a far plane of 1, then of 0 (GREATER)
        const char *nm = pass ? "far plane 0" : "far plane 1";
        uint64_t tag = frame(color, pass ? rdepth : depth, 40, pass ? NVN_DEPTH_FUNC_GEQUAL : NVN_DEPTH_FUNC_LEQUAL, decoy);
        CHECK(tag != 0, "%s: the frame's scene depth is copied", nm);
        for (int i = 0; i < 400 && !(mtl_stereo_acquire(&f) && f.depth); i++) { if (f.c >= 0) mtl_stereo_release(&f); usleep(5000); }
        CHECK(f.color && f.depth, "%s: the frame comes with its depth", nm);
        if (!f.depth) return 1;
        id<MTLTexture> fc = (__bridge id<MTLTexture>)f.color, fz = (__bridge id<MTLTexture>)f.depth;
        CHECK(fc.width == W && fc.height == H && fc.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB, "%s: the picture is %lux%lu format %lu", nm, (unsigned long)fc.width, (unsigned long)fc.height, (unsigned long)fc.pixelFormat);
        CHECK(fz.width == W && fz.height == H, "%s: the depth with the most draws in a pass with colour is taken, not the decoy or the shadow map (%lux%lu)", nm, (unsigned long)fz.width, (unsigned long)fz.height);
        CHECK(f.depth_rect[0] == 0 && f.depth_rect[1] == 0 && f.depth_rect[2] == 1 && f.depth_rect[3] == 1, "%s: the scene covers the depth", nm);
        CHECK(f.depth_rev == pass, "%s: the far plane's side comes from the draws' compare (%d)", nm, f.depth_rev);
        NSUInteger w, h; id<MTLBuffer> px = dm_immersive_eyes(&f, 1, &w, &h);
        CHECK(px && w == W && h == H, "%s: both eyes are drawn", nm);
        if (!px) return 1;
        const uint8_t *p = px.contents;   // BGRA
        double far = 0.012 * W, fl = column(p, 0, 90, 2), fr = column(p, 1, 90, 2), ml = column(p, 0, 280, 0), mr = column(p, 1, 280, 0), nl = column(p, 0, 480, 1), nr = column(p, 1, 480, 1);
        CHECK(fabs(fl - (200 - far / 2)) < 1.5 && fabs(fr - (200 + far / 2)) < 1.5, "%s: the far marker lies behind the screen (left %.2f, right %.2f, want %.2f and %.2f)", nm, fl, fr, 200 - far / 2, 200 + far / 2);
        CHECK(fabs(ml - 800) < 1 && fabs(mr - 800) < 1, "%s: the middle is the screen's plane (left %.2f, right %.2f, want 800)", nm, ml, mr);
        CHECK(fabs(nl - (512 + far / 4)) < 1.5 && fabs(nr - (512 - far / 4)) < 1.5, "%s: the near marker lies in front, by the limit (left %.2f, right %.2f, want %.2f and %.2f)", nm, nl, nr, 512 + far / 4, 512 - far / 4);
        mtl_stereo_release(&f);
    }
    // The scene and its depth are rendered upside down, then postprocessing flips
    // only the colour into the window. The tap must carry enough information to
    // align depth with that final colour, without changing the window origin.
    {
        id<MTLTexture> picture = make(MTLPixelFormatRGBA8Unorm, W, H);
        id<MTLTexture> scene = make(MTLPixelFormatRGBA8Unorm, W, H), flipped = make(MTLPixelFormatDepth32Float, W, H);
        uint8_t *rc = malloc(W * H * 4); float *rz = malloc(W * H * sizeof *rz);
        // An asymmetric scene with detail at the reference grid's scale.
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            uint8_t *p = rc + (y * W + x) * 4;
            p[0] = 20 + y * 120 / H; p[1] = 20 + (x / 32 % 2) * 20; p[2] = 10; p[3] = 255;
        }
        fill(picture, rc, 4);
        uint8_t row[W * 4];
        for (int y = 0; y < H / 2; y++) {
            memcpy(row, rc + y * W * 4, W * 4);
            memcpy(rc + y * W * 4, rc + (H - 1 - y) * W * 4, W * 4);
            memcpy(rc + (H - 1 - y) * W * 4, row, W * 4);
        }
        fill(scene, rc, 4);
        for (int reverse = 0; reverse < 2; reverse++) {
            const float *source = reverse ? zr : z;
            for (int y = 0; y < H; y++) memcpy(rz + y * W, source + (H - 1 - y) * W, W * sizeof *rz);
            fill(flipped, rz, 4);
            int func = reverse ? NVN_DEPTH_FUNC_GEQUAL : NVN_DEPTH_FUNC_LEQUAL;
            frame(picture, reverse ? rdepth : depth, 40, func, nil);
            CHECK(mtl_stereo_acquire(&f) && f.reference, "alignment: upright scene has a reference (reverse %d)", reverse);
            NSUInteger ew, eh, aw, ah;
            id<MTLBuffer> expected = dm_immersive_eyes(&f, 1, &ew, &eh);
            mtl_stereo_release(&f);
            frame_scene(picture, flipped, 40, func, nil, scene);
            CHECK(mtl_stereo_acquire(&f) && f.reference && !f.depth_flip, "alignment: inverted scene has its own reference, despite an upper-left window (reverse %d)", reverse);
            id<MTLBuffer> actual = dm_immersive_eyes(&f, 1, &aw, &ah);
            CHECK(expected && actual && ew == aw && eh == ah && !memcmp(expected.contents, actual.contents, ew * eh * 8),
                  "alignment: both eyes match the upright scene after an offscreen Y flip (reverse %d)", reverse);
            mtl_stereo_release(&f);
        }
        free(rc); free(rz);
    }
    // A frame without a scene (a 2D game): the picture alone, drawn flat
    uint64_t tag = frame(color, nil, 0, NVN_DEPTH_FUNC_LEQUAL, nil);
    CHECK(tag == 0, "a frame without depth-writing draws has no depth copy");
    CHECK(mtl_stereo_acquire(&f) && !f.depth, "its frame comes without a depth");
    NSUInteger w, h; id<MTLBuffer> px = dm_immersive_eyes(&f, 1, &w, &h);
    if (px) { const uint8_t *p = px.contents; double l = column(p, 0, 90, 2), r = column(p, 1, 90, 2); CHECK(fabs(l - 200) < 0.6 && fabs(r - 200) < 0.6, "and both eyes get the same picture (%.2f, %.2f)", l, r); }
    mtl_stereo_release(&f);
    // A HUD: the scene composited into a window texture by one draw, then a white bar drawn over its far band by a second, without the depth test
    {
        tex_t wt; id<MTLTexture> win = game_tex(&wt, 99701); mtl_window_tex(wt.serial);
        fill(win, c, 4);
        uint8_t bar[16 * 100 * 4]; memset(bar, 255, sizeof bar);
        id<MTLBuffer> bb = [g_dev newBufferWithBytes:bar length:sizeof bar options:MTLResourceStorageModeShared]; res_add(bb); res_commit();
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &wt; S.ds.dt = 0;
        st_overlay(nil, 1);   // the composite: the picture is laid down
        st_overlay(nil, 1);   // the HUD's first draw: the window texture is copied before it
        enc_b_copy_buffer_texture(blit_open(), bb, 0, 16 * 4, sizeof bar, MTLSizeMake(16, 100, 1), win, 0, 0, MTLOriginMake(600, 40, 0));   // the bar, at columns 600..615 of the far band
        st_overlay(nil, 1);   // its second draw: copied once
        mtl_sub_commit(); mtl_drain();
        present_win(&wt, win, color, depth); CHECK(g_st_tag != 0, "HUD: the frame's depth is copied");
        CHECK(mtl_stereo_acquire(&f) && f.base && f.depth, "HUD: the frame comes with the picture before its overlays");
        if (f.base) {
            NSUInteger w2, h2; id<MTLBuffer> px2 = dm_immersive_eyes(&f, 1, &w2, &h2);
            CHECK(px2 != nil, "HUD: both eyes are drawn");
            if (px2) {
                const uint8_t *q = px2.contents; double far = 0.012 * W, lo = far / 2;
                double hl = column_in(q, 0, 90, 1, 560, 660), hr = column_in(q, 1, 90, 1, 560, 660), fl = column_in(q, 0, 90, 2, 150, 250), fr = column_in(q, 1, 90, 2, 150, 250);
                CHECK(fabs(hl - (607.5 + lo / 2)) < 1.5 && fabs(hr - (607.5 - lo / 2)) < 1.5, "HUD: the bar stands in front of the scene (left %.2f, right %.2f, want %.2f and %.2f)", hl, hr, 607.5 + lo / 2, 607.5 - lo / 2);
                CHECK(fabs(fl - (200 - far / 2)) < 1.5 && fabs(fr - (200 + far / 2)) < 1.5, "HUD: the far marker beside it keeps its depth (left %.2f, right %.2f)", fl, fr);
                int ghost = 0; for (int x = 590; x < 600; x++) ghost |= q[((size_t)0 * H + 90) * W * 4 + (size_t)x * 4 + 1] > 8;
                CHECK(!ghost, "HUD: no copy of the bar where the far band moves (left eye, columns 590..599)");
            }
            mtl_stereo_release(&f);
        }
    }
    // A HUD drawn into the scene's own texture, which one draw then composites into the window: the first frame finds the composite's source (st_feed), the
    // second copies that texture before its first overlay and gives the window that base. up: the scene is drawn into the texture with depth, an effect built from a blur
    // of it follows (an edge glow), then the HUD tested against the depth cleared again: the base holds the effect, which keeps the scene's depth
    for (int up = 0; up < 2; up++) {
        const char *nm = up ? "effect, then a HUD over a cleared depth" : "composited HUD";
        tex_t wt, ft, bt; id<MTLTexture> win = game_tex(&wt, 99721 + 3 * up), scn = game_tex(&ft, 99722 + 3 * up), blur = game_tex(&bt, 99723 + 3 * up);
        mtl_window_tex(wt.serial);
        const int tid = 4093, btid = 4092; atomic_store(&g_tser[tid], ft.serial); atomic_store(&g_tser[btid], bt.serial);
        NvShader *fsh = [NvShader new]; fsh->rf.nb = 1; fsh->rf.b[0].kind = NVN_RB_TEX; fsh->rf.b[0].reg = 0;
        NvProg *comp = [NvProg new]; comp->sh[1] = fsh;   // samples texture slot 0: the composite, the blur and the effect
        uint8_t bar[16 * 100 * 4]; memset(bar, 255, sizeof bar);
        id<MTLBuffer> bb = [g_dev newBufferWithBytes:bar length:sizeof bar options:MTLResourceStorageModeShared]; res_add(bb);
        uint8_t glow[5 * 100 * 4]; for (int i = 0; i < 500; i++) { glow[i * 4] = glow[i * 4 + 2] = 0; glow[i * 4 + 1] = glow[i * 4 + 3] = 255; }
        id<MTLBuffer> gb = [g_dev newBufferWithBytes:glow length:sizeof glow options:MTLResourceStorageModeShared]; res_add(gb); res_commit();
        for (int fr = 0; fr < 2; fr++) {
            fill(scn, c, 4);
            state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
            R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &ft; S.ds.dt = 0;
            st_feed(nil); st_overlay(nil, 1);   // the tone map lays the scene's texture down
            if (up) {   // the scene with depth, a blur of it, the effect from the blur (a green glow at columns 300..304 of the far band), then the depth cleared for the HUD
                MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new]; pd.colorAttachments[0].texture = scn; pd.depthAttachment.texture = depth;
                R.rt.depth = &ft; S.ds.dt = S.ds.dw = 1; S.ds.df = NVN_DEPTH_FUNC_LEQUAL; S.has_vp = 1; S.vp[0][0] = S.vp[0][1] = 0; S.vp[0][2] = W; S.vp[0][3] = H;
                st_pass(pd); for (int i = 0; i < 40; i++) st_draw();
                S.ds.dw = 0; S.ds.df = NVN_DEPTH_FUNC_ALWAYS;
                R.rt.color[0] = &bt; pd = [MTL4RenderPassDescriptor new]; pd.colorAttachments[0].texture = blur; st_pass(pd);
                S.tex[1][0] = tid; st_flow(comp);   // the blur samples the scene
                R.rt.color[0] = &ft; S.tex[1][0] = btid;
                st_feed(comp); st_overlay(comp, 1);   // the effect samples the blur: the picture is laid down again, no base yet
                enc_b_copy_buffer_texture(blit_open(), gb, 0, 5 * 4, sizeof glow, MTLSizeMake(5, 100, 1), scn, 0, 0, MTLOriginMake(300, 40, 0));
                S.tex[1][0] = 0; R.clear_d = 1; S.ds.df = NVN_DEPTH_FUNC_LEQUAL;   // the HUD tests against the depth cleared again
            }
            st_feed(nil); st_overlay(nil, 1);   // the HUD's first draw into it
            R.clear_d = 0; R.rt.depth = NULL; S.ds.dt = 0;
            enc_b_copy_buffer_texture(blit_open(), bb, 0, 16 * 4, sizeof bar, MTLSizeMake(16, 100, 1), scn, 0, 0, MTLOriginMake(600, 40, 0));
            R.rt.color[0] = &wt; S.tex[1][0] = tid;
            st_feed(comp); st_overlay(nil, 1);   // the composite into the window
            enc_b_copy_texture(blit_open(), scn, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), win, 0, 0, MTLOriginMake(0, 0, 0));
            mtl_sub_commit(); mtl_drain();
            present_win(&wt, win, color, depth);
            CHECK(g_st_feed == ft.serial, "%s: the window's composite source is found (frame %d)", nm, fr);
            CHECK(mtl_stereo_acquire(&f), "%s: frame %d is published", nm, fr);
            CHECK(fr ? f.base != NULL : f.base == NULL, "%s: frame %d %s", nm, fr, fr ? "comes with the composited texture before its overlays" : "has no base yet");
            if (fr && f.base) {
                NSUInteger w2, h2; id<MTLBuffer> px2 = dm_immersive_eyes(&f, 1, &w2, &h2);
                if (px2) { const uint8_t *q = px2.contents; double lo = 0.012 * W / 2;
                    double hl = column_in(q, 0, 90, 1, 560, 660), hr = column_in(q, 1, 90, 1, 560, 660), fl = column_in(q, 0, 90, 2, 150, 250), fr2 = column_in(q, 1, 90, 2, 150, 250);
                    CHECK(fabs(hl - (607.5 + lo / 2)) < 1.5 && fabs(hr - (607.5 - lo / 2)) < 1.5, "%s: the bar stands in front of the scene (left %.2f, right %.2f)", nm, hl, hr);
                    CHECK(fabs(fl - (200 - lo)) < 1.5 && fabs(fr2 - (200 + lo)) < 1.5, "%s: the far marker keeps its depth (left %.2f, right %.2f)", nm, fl, fr2);
                    if (up) { double gl = column_in(q, 0, 90, 1, 250, 350), gr = column_in(q, 1, 90, 1, 250, 350);
                        CHECK(fabs(gl - (302 - lo)) < 1.5 && fabs(gr - (302 + lo)) < 1.5, "%s: the effect keeps the scene's depth (left %.2f, right %.2f)", nm, gl, gr); } }
            }
            mtl_stereo_release(&f);
        }
        S.tex[1][0] = 0; atomic_store(&g_tser[tid], 0); atomic_store(&g_tser[btid], 0);
    }
    // The visible scene picks the depth (st_take_depth): not a square atlas with more draws, not a normals prepass with more draws, and the scene's depth as it was before a UI
    // pass cleared it
    {
        tex_t at = {.serial = 99801}, nt = {.serial = 99811}, ot = {.serial = 99814}, scn = {.serial = 99812}, pt = {.serial = 99813};
        pt.d = (nvn_texdesc){.width = W, .height = H};
        id<MTLTexture> acol = make(MTLPixelFormatRGBA8Unorm, 386, 386), adep = make(MTLPixelFormatDepth32Float, 386, 386), ncol = make(MTLPixelFormatRGBA8Unorm, W, H), ndep = make(MTLPixelFormatDepth32Float, W, H);
        res_commit();
        state_reset(); memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1;
        S.ds.dt = S.ds.dw = 1; S.ds.df = NVN_DEPTH_FUNC_LEQUAL; S.has_vp = 1; S.vp[0][0] = S.vp[0][1] = 0;
        MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
        #define PASS(tt, col, dep, n) do { R.rt.color[0] = &(tt); pd.colorAttachments[0].texture = col; pd.depthAttachment.texture = dep; S.vp[0][2] = (dep).width; S.vp[0][3] = (dep).height; \
                                           st_pass(pd); for (int i = 0; i < (n); i++) st_draw(); } while (0)
        #define TAKEN(tag) ((tag) ? g_st_d[((tag) & 0xff) - 1].src : nil)
        PASS(at, acol, adep, 60); PASS(scn, color, depth, 40);
        uint64_t tag = st_take_depth(&pt, NULL);
        CHECK(TAKEN(tag) == depth, "visible scene: a square atlas with more draws is not the widescreen picture's depth");
        PASS(nt, ncol, ndep, 60); PASS(scn, color, depth, 40);
        st_copied(nt.serial, ot.serial); st_copied(ot.serial, pt.serial); st_copied(scn.serial, pt.serial);   // normals through an effect, the scene directly
        tag = st_take_depth(&pt, NULL);
        CHECK(TAKEN(tag) == depth, "visible scene: the colour nearest the picture wins over a normals prepass with more draws");
        // The scene's depth (far band 1) cleared for a UI pass that draws a few things into it; the clear is simulated by filling the reversed depth after the seal ran
        mtl_drain(); state_reset(); memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; mtl_sub_begin();
        S.ds.dt = S.ds.dw = 1; S.ds.df = NVN_DEPTH_FUNC_LEQUAL; S.has_vp = 1; S.vp[0][0] = S.vp[0][1] = 0;
        PASS(scn, color, depth, 40);
        pd.depthAttachment.loadAction = MTLLoadActionClear; st_seal(pd); PASS(scn, color, depth, 5); pd.depthAttachment.loadAction = MTLLoadActionLoad;
        mtl_sub_commit(); mtl_drain(); fill(depth, zr, 4);
        tag = st_take_depth(&pt, NULL); int k = tag ? (int)(tag & 0xff) - 1 : -1;
        CHECK(k >= 0 && g_st_d[k].tex, "UI clear: the frame has a depth");
        for (int i = 0; i < 400 && k >= 0 && g_st_ev.signaledValue < g_st_d[k].ev; i++) usleep(5000);
        if (k >= 0 && g_st_d[k].tex) {
            id<MTLBuffer> rb = [g_dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [g_tq commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            [e copyFromTexture:g_st_d[k].tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            float v = ((const float *)rb.contents)[10 * W + 10];
            CHECK(v == 1, "UI clear: the copy holds the scene's far band (%g), not what replaced it", v);
        }
        fill(depth, z, 4);
        #undef PASS
        #undef TAKEN
    }
    mtl_drain();
    CHECK(!mtl_last_commit_error(), "no GPU error");
    printf("t_stereo: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
