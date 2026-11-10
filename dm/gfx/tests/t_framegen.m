// Frame generation (DM_FRAMEGEN, nvn_mtl.m fg_on): MetalFX's frame interpolator after each present snapshot, from the previous one, and the display link showing each generated
// frame on its present's first tick and the present's own picture half its interval later. A white square moves 16 px a present over a grey picture, with a still one at the right
// (a HUD): the generated frame has the moving one halfway and the still one in place. Real presents through an offscreen layer; the display link's ticks are called here
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
static CAMetalLayer *g_test_layer;
void *front_metal_layer(void) { return (__bridge void *)g_test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
enum { W = 256, H = 128, SQ = 32, Y0 = 48, STILL = 200 };
static int square_x(int i) { return 20 + 16 * (i % 8); }
static tex_t frame(int i) {   // present i: the moving square at square_x(i), the still one at STILL
    static uint32_t px[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int in = y >= Y0 && y < Y0 + SQ && ((x >= square_x(i) && x < square_x(i) + SQ) || (x >= STILL && x < STILL + SQ));
        px[y * W + x] = in ? 0xffffffff : 0xff303030;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    [mt replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=91001+(uint64_t)i};
    t.d.width = W; t.d.height = H; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
// The bright columns of row Y0 + SQ / 2 left of the still square, and whether the still square is whole; d: the texture's size is W / d by H / d (positions in its pixels)
static void bright_d(id<MTLTexture> t, int d, int *lo, int *hi, int *still);
static void bright(id<MTLTexture> t, int *lo, int *hi, int *still) { bright_d(t, 1, lo, hi, still); }
static void bright_d(id<MTLTexture> t, int d, int *lo, int *hi, int *still) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:W * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, (Y0 + SQ / 2) / d, 0) sourceSize:MTLSizeMake(W / d, 1, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * 4];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = out.contents; *lo = *hi = -1; *still = 1;
    for (int x = 0; x < W / d; x++) { int on = p[4 * x + 1] > 128;
        if (x < (STILL - 4) / d && on) { if (*lo < 0) *lo = x; *hi = x; }
        if (x >= STILL / d + 1 && x < (STILL + SQ) / d - 1 && !on) *still = 0;
        if ((x == STILL / d - 2 || x == (STILL + SQ) / d + 1) && on) *still = 0; }
}
static void copied(void) { snap_commit_wait(g_snap_v); [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]; }
static int last_slot(void) { return (int)((g_snap_next + (uint64_t)g_snap_n - 1) % (uint64_t)g_snap_n); }
@interface TUpdate : NSObject
@property (nonatomic, strong) id<CAMetalDrawable> drawable;
@property (nonatomic) CFTimeInterval targetPresentationTimestamp;
@end
@implementation TUpdate
@end
static double g_t = 100, g_tick_ms;   // g_tick_ms: how long the last tick's callback took (the drawable taken before it)
static uint64_t tick(void) {   // one 60 Hz tick of the display link; returns the serial it showed (0 = none)
    TUpdate *u = [TUpdate new]; u.drawable = [g_test_layer nextDrawable]; u.targetPresentationTimestamp = g_t; g_t += 1 / 60.0;
    uint64_t n = g_present_n; g_present_ser = 0;
    uint64_t t0 = now_ns();
    [[NvMetalDisplayLinkDelegate new] metalDisplayLink:(CAMetalDisplayLink *)nil needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    g_tick_ms = (now_ns() - t0) / 1e6;
    return g_present_n != n ? g_present_ser : 0;
}
// n presents from frame i0 at 30 fps on the link's 60 Hz ticks (two ticks each)
static void steady(int i0, int n) { for (int i = 0; i < n; i++) { tex_t t = frame(i0 + i); present_enqueue(&t, 1 / 30.0, NULL, 0); copied(); tick(); tick(); } }
int main(void) { @autoreleasepool {
    alarm(30); setbuf(stdout, NULL);
    setenv("DM_FRAMEGEN", "1", 1); setenv("DM_PRESENT_SNAP", "1", 1); setenv("DM_PRESENT_HZ", "0", 1); setenv("DM_PRESENT_LINK", "0", 1);   // the present thread first; the link's ticks are called below
    mtl_init();
    g_test_layer = [CAMetalLayer layer]; g_test_layer.device = g_dev; g_test_layer.drawableSize = CGSizeMake(W, H); g_test_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    mtl_present_screen(W, H); mtl_present_layer(W, H);   // the picture's shape, as a front end reports it
    CHECK("frame generation on", fg_on());
    CHECK("snapshots on with a ring of 3", mtl_snap_on() && g_snap_n == 3);
    // The present thread takes the presents (no display link): each slot's generated frame against the presents before
    int ok[6];
    for (int i = 0; i < 6; i++) {
        tex_t t = frame(i); present_enqueue(&t, 1 / 30.0, NULL, 0); mtl_present_flush(); copied();
        int k = last_slot(); ok[i] = g_snap[k].gen_ok;
        if (i == 5) CHECK("the queued present carries its generated frame", g_pq[(g_pq_in - 1) % PQ].gen == g_snap[k].gen_serial && g_snap[k].gen_serial);
        if (!ok[i]) continue;
        int lo, hi, still; bright(g_snap[k].gen, &lo, &hi, &still); int want = (square_x(i - 1) + square_x(i)) / 2;
        printf("present %d: generated square at %d..%d (halfway %d..%d), still square %s\n", i, lo, hi, want, want + SQ - 1, still ? "in place" : "moved");
        CHECK("the moving square is halfway", abs(lo - want) <= 2 && abs(hi - (want + SQ - 1)) <= 2);
        CHECK("the still square stays", still);
    }
    CHECK("generated frames from the fourth present on", !ok[0] && !ok[1] && !ok[2] && ok[3] && ok[4] && ok[5]);
    { tex_t t = frame(6); present_enqueue(&t, 1 / 60.0, NULL, 0); mtl_present_flush(); copied(); }
    CHECK("none for a present of one tick", !g_snap[last_slot()].gen_ok && !g_pq[(g_pq_in - 1) % PQ].gen);
    for (int i = 7; i < 10; i++) { tex_t t = frame(i); present_enqueue(&t, 1 / 30.0, NULL, 0); mtl_present_flush(); copied(); ok[i - 7] = g_snap[last_slot()].gen_ok; }
    CHECK("after it, the history starts again", !ok[0] && !ok[1] && ok[2]);
    // The display link at 30 fps on 60 Hz ticks: the generated frame on the present's first tick, its own picture on the second. Frame generation pauses while frames drop:
    // a present the link waited for (a tick found nothing new to show) or one taken while a newer one waits shows its own picture, until three have come in time
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    g_dm_present_link = 1; g_link_started = 1;   // no link object: the ticks below are the link's
    for (int i = 0; i < 4; i++) tick();
    CHECK("the link's tick interval is known", fabs(g_fg_tick - 1 / 60.0) < 1e-4);
    int order = 1, paused = 1;
    for (int i = 10; i < 16; i++) {
        tex_t t = frame(i); present_enqueue(&t, 1 / 30.0, NULL, 0); copied();
        int k = last_slot(); uint64_t gen = g_snap[k].gen_ok ? g_snap[k].gen_serial : 0, own = g_snap[k].serial;
        uint64_t a = tick(), b = tick();
        printf("present %d: ticks showed %llx %llx (generated %llx, own %llx)\n", i, (unsigned long long)a, (unsigned long long)b, (unsigned long long)gen, (unsigned long long)own);
        if (i < 13) paused &= a == own && !b;   // 10 came after ticks with nothing to show; 11 and 12 are its first two in time
        else order &= gen && a == gen && b == own;
    }
    CHECK("after the link waited for a present, its own pictures until three came in time", paused);
    CHECK("each present in time: its generated frame, then its own picture", order);
    CHECK("then held", !tick());
    // A tick missed after the generated frame: once the interval is over and the next present waits, that one is shown, not the stale picture
    steady(40, 3);
    tex_t t1 = frame(14); present_enqueue(&t1, 1 / 30.0, NULL, 0); copied();
    uint64_t g1 = g_snap[last_slot()].gen_serial, a = tick(); g_t += 1 / 60.0;
    tex_t t2 = frame(15); present_enqueue(&t2, 1 / 30.0, NULL, 0); copied();
    uint64_t g2 = g_snap[last_slot()].gen_serial, late = g_fg_late, b = tick();
    CHECK("a missed tick skips the stale picture for the next present", a == g1 && b == g2 && g_fg_late == late + 1);
    tick(); tick();
    // A present of one tick on the link: its own picture only
    tex_t t3 = frame(16); present_enqueue(&t3, 1 / 60.0, NULL, 0); copied();
    uint64_t own = g_snap[last_slot()].serial;
    CHECK("a present of one tick shows its own picture", tick() == own && !tick());
    // A present that comes late (the link shows nothing new on a tick past its interval): its own picture, and so for the next ones until three have come in time
    steady(30, 4); tick();   // in time, then a tick with nothing new: the next present is late
    uint64_t late_show[4], late_own[4], late_gen[4];
    for (int i = 0; i < 4; i++) { tex_t t = frame(34 + i); present_enqueue(&t, 1 / 30.0, NULL, 0); copied();
        late_own[i] = g_snap[last_slot()].serial; late_gen[i] = g_snap[last_slot()].gen_ok ? g_snap[last_slot()].gen_serial : 0; late_show[i] = tick(); tick(); }
    printf("after a late present the first ticks showed: %s %s %s %s\n", late_show[0] == late_own[0] ? "own" : "?", late_show[1] == late_own[1] ? "own" : "?",
           late_show[2] == late_own[2] ? "own" : "?", late_show[3] == late_gen[3] ? "generated" : "?");
    CHECK("a late present pauses generated frames until three come in time", late_show[0] == late_own[0] && late_show[1] == late_own[1] && late_show[2] == late_own[2] && late_gen[3] && late_show[3] == late_gen[3]);
    // One newer present waiting is the game running a frame ahead (snapshots): generated frames go on. Three waiting: the oldest is dropped, and the one taken shows its
    // own picture, as does the next
    tex_t b1 = frame(36), b2 = frame(37);
    present_enqueue(&b1, 1 / 30.0, NULL, 0); copied(); uint64_t gen_b1 = g_snap[last_slot()].gen_ok ? g_snap[last_slot()].gen_serial : 0, own_b1 = g_snap[last_slot()].serial;
    present_enqueue(&b2, 1 / 30.0, NULL, 0); copied(); uint64_t gen_b2 = g_snap[last_slot()].gen_ok ? g_snap[last_slot()].gen_serial : 0;
    uint64_t sb1 = tick(), ob1 = tick(), sb2 = tick(); tick();
    CHECK("one newer present waiting: generated frames go on", gen_b1 && sb1 == gen_b1 && ob1 == own_b1 && gen_b2 && sb2 == gen_b2);
    tex_t c1 = frame(38), c2 = frame(39), c3 = frame(40);
    present_enqueue(&c1, 1 / 30.0, NULL, 0); copied();
    present_enqueue(&c2, 1 / 30.0, NULL, 0); copied(); uint64_t own_c2 = g_snap[last_slot()].serial;
    present_enqueue(&c3, 1 / 30.0, NULL, 0); copied(); uint64_t own_c3 = g_snap[last_slot()].serial;
    uint64_t behind0 = g_fg_paused_behind, sc2 = tick(); tick(); uint64_t sc3 = tick(); tick();
    printf("three waiting: ticks showed %llx then %llx (own %llx, %llx)\n", (unsigned long long)sc2, (unsigned long long)sc3, (unsigned long long)own_c2, (unsigned long long)own_c3);
    // (c3's ring slot is still held, so it is shown from the game's own texture)
    CHECK("an older present dropped: the one taken and the next show their own pictures", sc2 == own_c2 && (sc3 == own_c3 || sc3 == c3.serial) && g_fg_paused_behind == behind0 + 1);
    // The picture after a generated frame while the GPU has not run that frame (a long frame, a load): it does not wait on the display link thread for the generated frame's
    // allocator, which held the link silent until its rescue (a Mac run while loading)
    steady(17, 3);
    tex_t t4 = frame(20); present_enqueue(&t4, 1 / 30.0, NULL, 0); copied();
    uint64_t g4 = g_snap[last_slot()].gen_ok ? g_snap[last_slot()].gen_serial : 0, own4 = g_snap[last_slot()].serial;
    id<MTLSharedEvent> gate = [g_dev newSharedEvent]; [g_q4 waitForEvent:gate value:1];   // the GPU stops before the generated frame's present
    uint64_t ga = tick(), gb = tick(); double ms = g_tick_ms;
    printf("with the GPU held: ticks showed %llx %llx, the second's callback took %.2f ms\n", (unsigned long long)ga, (unsigned long long)gb, ms);
    CHECK("the picture after a generated frame does not wait for the GPU", g4 && ga == g4 && gb == own4 && ms < 50);
    gate.signaledValue = 1;
    CHECK("the interpolation's GPU time is measured", g_fg_gpu_last > 2e-5 && g_fg_gpu_last < 1);   // (an empty or early stamp read 0 or 6 µs)
    printf("last interpolation: %.3f ms\n", g_fg_gpu_last * 1e3);
    // A screen smaller than the picture (an iPhone's): the snapshot and the frames generated from it are made at the size the picture is shown at
    for (int i = 0; i < 4; i++) tick();
    g_test_layer.drawableSize = CGSizeMake(W / 2, H / 2);
    const int whole_crop[4] = {0, 0, W, H};   // Ocarina of Time passes its window's crop, the whole texture, with every present (log 73)
    for (int i = 21; i < 26; i++) { tex_t t = frame(i); present_enqueue(&t, 1 / 30.0, whole_crop, 0); copied(); tick(); tick(); }
    CHECK("a present whose snapshot holds its crop is shown whole", !g_pq[(g_pq_in - 1) % PQ].crop[2] && !g_pq[(g_pq_in - 1) % PQ].crop[3]);
    int k5 = last_slot(); id<MTLTexture> half = g_snap[k5].tex;
    CHECK("the snapshot is at the size the picture is shown at", half.width == W / 2 && half.height == H / 2 && g_snap[k5].gen_ok && g_snap[k5].gen.width == W / 2);
    { int lo, hi, still; bright_d(g_snap[k5].tex, 2, &lo, &hi, &still);
      printf("half size, own picture: square at %d..%d (%d..%d)\n", lo, hi, square_x(25) / 2, (square_x(25) + SQ) / 2 - 1);
      CHECK("the half-size picture is the picture scaled", abs(lo - square_x(25) / 2) <= 1 && abs(hi - ((square_x(25) + SQ) / 2 - 1)) <= 1 && still);
      bright_d(g_snap[k5].gen, 2, &lo, &hi, &still); int want = (square_x(24) + square_x(25)) / 4;
      printf("half size, generated: square at %d..%d (halfway %d..%d)\n", lo, hi, want, want + SQ / 2 - 1);
      CHECK("the half-size generated frame has the square halfway", abs(lo - want) <= 2 && abs(hi - (want + SQ / 2 - 1)) <= 2 && still); }
    // A crop of the right half (the still square and what lies left of it, from x 128): the snapshot holds that half alone, at the half-size screen's size
    const int right[4] = {W / 2, 0, W / 2, H};
    for (int i = 26; i < 28; i++) { tex_t t = frame(i); present_enqueue(&t, 1 / 30.0, right, 0); copied(); tick(); tick(); }
    { int k6 = last_slot(); id<MTLTexture> c = g_snap[k6].tex;
      id<MTLBuffer> out = [g_dev newBufferWithLength:c.width * 4 options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
      [b copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, (Y0 + SQ / 2) * c.height / H, 0) sourceSize:MTLSizeMake(c.width, 1, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:c.width * 4 destinationBytesPerImage:c.width * 4];
      [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
      const uint8_t *p = out.contents; int a = -1, z = -1; for (int x = 0; x < (int)c.width; x++) if (p[4 * x + 1] > 128) { if (a < 0) a = x; z = x; }
      double sc = (double)c.width / (W / 2); int wa = (int)lround((STILL - W / 2) * sc), wz = (int)lround((STILL + SQ - W / 2) * sc) - 1;
      printf("right-half crop: snapshot %lux%lu, still square at %d..%d (%d..%d)\n", (unsigned long)c.width, (unsigned long)c.height, a, z, wa, wz);
      CHECK("a cropped picture is copied alone at its shown size", c.width == (NSUInteger)lround(H / 2.0 * (W / 2) / H) && c.height == H / 2 && abs(a - wa) <= 1 && abs(z - wz) <= 1); }
    // A crop shown at its size or larger (Ocarina of Time: 2352x1080 of a 3136x1440 texture on a 2622x1203 screen, log 75): the snapshot holds the crop alone, 1:1
    g_test_layer.drawableSize = CGSizeMake(W, H);
    for (int i = 28; i < 32; i++) { tex_t t = frame(i); present_enqueue(&t, 1 / 30.0, right, 0); copied(); tick(); tick(); }
    { int k7 = last_slot(); id<MTLTexture> c = g_snap[k7].tex;
      id<MTLBuffer> out = [g_dev newBufferWithLength:c.width * 4 options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
      [b copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, Y0 + SQ / 2, 0) sourceSize:MTLSizeMake(c.width, 1, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:c.width * 4 destinationBytesPerImage:c.width * 4];
      [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
      const uint8_t *p = out.contents; int a = -1, z = -1; for (int x = 0; x < (int)c.width; x++) if (p[4 * x + 1] > 128) { if (a < 0) a = x; z = x; }
      printf("right-half crop on a full-size screen: snapshot %lux%lu, bright from %d to %d (the moving square from %d, the still one to %d), generated %s\n", (unsigned long)c.width, (unsigned long)c.height, a, z, square_x(31) - W / 2, STILL + SQ - W / 2 - 1, g_snap[k7].gen_ok ? "yes" : "no");
      CHECK("a crop shown at its size is copied alone, 1:1, and shown whole", c.width == W / 2 && c.height == H && a == square_x(31) - W / 2 && z == STILL + SQ - W / 2 - 1 && g_snap[k7].gen_ok
            && !g_pq[(g_pq_in - 1) % PQ].crop[2]); }
    present_drop_for_suspend(); mtl_drain();
    printf("frame generation: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
