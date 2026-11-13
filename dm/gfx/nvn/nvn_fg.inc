// Frame generation on the display link (DM_FRAMEGEN=1): the game held at 30 fps on the 60 Hz link shows each frame for two ticks
// (g_link_hold_until). With frame generation the first of those ticks shows a picture made between the previous frame and this one
// (fg_core.h: motion from the pictures, MetalFX frame interpolation), and the held tick shows the frame itself, from the copy the
// generation keeps. 60 pictures a second from 30 rendered ones, one 60 Hz tick (16.7 ms) more from render to screen.
// Only frames presented for longer than 25 ms (the 30 fps step) are generated toward: at 60 fps nothing changes.
// Included by nvn_mtl.m before present_now. All state is touched only on the display-link thread.
#include "fg_core.h"

static int g_fg_on = -1;
static fg_ctx *g_fg;
static int g_fg_refused;                    // the interpolator refused this size / format: frames are shown as they are
static NSUInteger g_fg_key[3];
static int g_fg_req;                        // present_now: the frame being presented is a real one to generate toward
static id<MTLTexture> g_fg_force_src;       // present_now: present this texture instead of the frame's (the held tick)
static id<MTLTexture> g_fg_pending;         // the real frame's copy, to show on the next (held) tick
static int g_fg_pending_crop[4];
static double g_fg_last_ts;                 // target time of the last generated present (a gap resets the history)
static uint64_t g_fg_gen_n, g_fg_held_n, g_fg_reset_n, g_fg_pass_n;
static _Atomic uint64_t g_fg_gpu_ns, g_fg_gpu_cnt;

static int fg_enabled(void) {
    if (g_fg_on < 0) {
        const char *e = getenv("DM_FRAMEGEN");
        g_fg_on = e && *e && strcmp(e, "0") != 0;
        if (g_fg_on) MLOG("frame generation (DM_FRAMEGEN): a picture generated between each two 30 fps frames, shown on the held 60 Hz tick before the frame itself");
    }
    return g_fg_on;
}
// The generated picture for src (the frame being presented), encoded into cb; nil when none (first frame, refused, reset). Sets the pending copy.
static id<MTLTexture> fg_generate(id<MTL4CommandBuffer> cb, id<MTLTexture> src, const int crop[4], double target_ts) {
    NSUInteger key[3] = {src.width, src.height, src.pixelFormat};
    if (memcmp(key, g_fg_key, sizeof key)) {
        memcpy(g_fg_key, key, sizeof key); g_fg_refused = 0;
        if (g_fg) { [g_q4 removeResidencySet:g_fg->res]; free(g_fg); g_fg = NULL; }   // textures released with the struct's ARC fields? (calloc'd: release by hand)
        NSString *why = nil; uint64_t t0 = now_ns();
        double sc = fg_env_f("DM_FG_SCALE", 0.75f); if (sc <= 0.1 || sc > 1) sc = 1;   // the generation's size against the picture's (the game's 1440p is its 1080p scaled up)
        g_fg = fg_create(g_dev, (NSUInteger)lround(src.width * sc), (NSUInteger)lround(src.height * sc), src.pixelFormat, &why);
        if (!g_fg) { g_fg_refused = 1; MLOG("[RED] frame generation off for %lux%lu format %lu: %s", (unsigned long)src.width, (unsigned long)src.height, (unsigned long)src.pixelFormat, why.UTF8String); }
        else { [g_q4 addResidencySet:g_fg->res]; MLOG("frame generation: %lux%lu format %lu generated at %lux%lu, motion %lux%lu, interpolator made in %.1f ms", (unsigned long)src.width, (unsigned long)src.height,
                    (unsigned long)src.pixelFormat, (unsigned long)g_fg->w, (unsigned long)g_fg->h, (unsigned long)g_fg->lw[0], (unsigned long)g_fg->lh[0], (now_ns() - t0) / 1e6); }
        g_fg_last_ts = 0;
    }
    if (g_fg_refused || !g_fg) return nil;
    if (!(src.usage & MTLTextureUsageShaderRead)) return nil;
    int reset = g_fg_last_ts <= 0 || target_ts - g_fg_last_ts > 0.1;   // a pause, a 60 fps stretch or a stall: the older frame is not this one's neighbour
    if (reset) g_fg_reset_n++;
    g_fg_last_ts = target_ts;
    id<MTLTexture> copy = nil;
    id<MTLTexture> gen = fg_encode(g_fg, cb, src, &copy, reset);
    g_fg_pending = copy; memcpy(g_fg_pending_crop, crop, sizeof g_fg_pending_crop);
    if (gen) g_fg_gen_n++; else g_fg_pass_n++;
    return gen;
}
static void fg_stats(void) {
    static uint64_t n; if (++n % 600) return;
    uint64_t c = atomic_load(&g_fg_gpu_cnt);
    MLOG("frame generation: %llu generated (%llu of them the frame itself, fast motion), %llu held ticks shown, %llu history resets, %llu passed through; present command buffers with generation %.2f ms GPU on average",
         (unsigned long long)g_fg_gen_n, (unsigned long long)(g_fg ? g_fg->bails : 0), (unsigned long long)g_fg_held_n, (unsigned long long)g_fg_reset_n, (unsigned long long)g_fg_pass_n, c ? atomic_load(&g_fg_gpu_ns) / 1e6 / c : 0.0);
}
