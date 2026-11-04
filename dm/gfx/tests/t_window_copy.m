// Window-copy eligibility, target lifetime and fail-stop guards. The GPU exercise
// uses an offscreen drawable and a manually invoked display-link callback.
#include "../nvn/nvn_mtl.m"
@interface FakeLinkUpdate : NSObject
@property id<CAMetalDrawable> drawable;
@property CFTimeInterval targetPresentationTimestamp;
@end
@implementation FakeLinkUpdate
@end
static CAMetalLayer *test_layer;
void *front_metal_layer(void) { return (__bridge void *)test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t window(uint64_t serial) { tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial}; return t; }
static void target(tex_t *w, uint64_t p, int ww, int wh, int pw, int ph) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:pw height:ph mipmapped:NO];
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; serial_put(p, mt);
    wc_owner *o = calloc(1, sizeof *o); o->serial = p; o->next = g_wc_owners; g_wc_owners = o;
    g_wc[0] = (wc_ent){w->serial, p, p, ww, wh, pw, ph}; atomic_store(&g_wc_n, 1);
}
static void lifecycle(void) {
    tex_t w = window(10001); target(&w, 10002, 100, 50, 200, 100);
    int crop[4] = {10, 5, 40, 20}, out[4];
    uint64_t p = wc_take(w.serial, crop, out);
    CHECK("take selects target and scales crop", p == 10002 && out[0] == 20 && out[1] == 10 && out[2] == 80 && out[3] == 40);
    CHECK("take consumes pending picture", wc_take(w.serial, crop, out) == 0);
    mtl_tex_release(&w);
    CHECK("finalize forgets the window", atomic_load(&g_wc_n) == 0);
    CHECK("taken target survives finalize", tex_get(p) != nil);
    // Reusing the NVN object's storage with a new serial must not recover the old picture.
    w = window(10003); target(&w, 10004, 100, 50, 300, 150);
    CHECK("reused object has a distinct target", wc_take(w.serial, NULL, out) == 10004 && tex_get(p) != nil);
    wc_release(p); CHECK("old target retires after present release", tex_get(p) == nil);
    mtl_tex_release(&w); CHECK("new taken target survives finalize", tex_get(10004) != nil);
    wc_release(10004); CHECK("new target retires after release", tex_get(10004) == nil);
    w = window(10005); target(&w, 10006, 100, 50, 200, 100);
    mtl_tex_release(&w); CHECK("untaken pending target retires on finalize", tex_get(10006) == nil);
    w = window(10007); target(&w, 10008, 100, 50, 200, 100);
    atomic_store(&g_present_discard, 1);
    CHECK("discarded present has no ticket", present_enqueue(&w, 1. / 60, NULL, 0) == 0);
    atomic_store(&g_present_discard, 0); mtl_tex_release(&w);
    CHECK("discarded present releases its target reference", tex_get(10008) == nil);
}
static void guards(void) {
    tex_t w = window(10101); target(&w, 10102, 100, 50, 200, 100);
    g_wc[0].pending = 0; wc_touch(&w, "test write", 0);
    CHECK("write after present does not poison", !atomic_load(&g_wc_off));
    wc_touch(&w, "test read", 1); CHECK("late read poisons even after present", atomic_load(&g_wc_off));
    atomic_store(&g_wc_off, 0); g_wc[0].pending = 10102;
    wc_touch(&w, "test write", 0); CHECK("late write before present poisons", atomic_load(&g_wc_off));
    atomic_store(&g_wc_off, 0); g_wc_cur = &g_wc[0]; g_wc_ndraw = 0; g_stat_draw = 20;
    wc_drawn(20); CHECK("no encoded draw leaves pending unchanged", g_wc_ndraw == 0 && g_wc[0].pending == 10102);
    g_wc[0].pending = 0; g_stat_draw++; wc_drawn(20);
    CHECK("first encoded draw marks picture pending", g_wc[0].pending == 10102 && !atomic_load(&g_wc_off));
    g_stat_draw++; wc_drawn(21); CHECK("second draw poisons", atomic_load(&g_wc_off));
    g_wc_cur = NULL; mtl_tex_release(&w);
}
static void production_target(void) {
    setenv("DM_NVN_WINDOW_COPY", "fixture", 1);
    test_layer = [CAMetalLayer layer]; test_layer.device = g_dev; test_layer.drawableSize = CGSizeMake(4, 4);
    tex_t w = window(10201); w.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=8, .height=8, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    id<MTLTexture> wt = [g_dev newTextureWithDescriptor:desc]; serial_put(w.serial, wt);
    g_xv_present[g_xv_npresent++] = w.serial;
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct I { float4 p [[attribute(0)]]; }; struct O { float4 p [[position]]; float2 uv; };\n"
        "vertex O v(I i [[stage_in]]) { return {float4(i.p.xyz,1), float2((i.p.x+1)*0.5, (1-i.p.y)*0.5)}; }\n"
        "struct A { texture2d<float, access::read> t [[id(0)]]; };\n"
        "fragment float4 f(O i [[stage_in]], constant A& a [[buffer(0)]]) { return a.t.read(uint2(i.uv * 4)); }\n"
        options:nil error:&error];
    CHECK("synthetic textured shaders compile", lib != nil);
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    NvProg *pr = [NvProg new]; NvShader *vs = [NvShader new], *fs = [NvShader new];
    vs->fn = [lib newFunctionWithName:@"v"]; vs->rf.nv = 1; vs->rf.v[0].ct = NVN_CT_FLOAT;
    memset(vs->rf.v[0].sh, 0xff, sizeof vs->rf.v[0].sh);
    vs->rf.pos_attr = 0; vs->rf.pos_w1 = 1; fs->stage = 1; strcpy(fs->key, "fixture");
    fs->fn = [lib newFunctionWithName:@"f"]; fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.color_known = 1; fs->rf.color_mask = 1; fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .aid=0, .dtt=MTLTextureType2D};
    pr->sh[0] = vs; pr->sh[1] = fs;
    prog_t pg = {.serial=10202}; serial_put(pg.serial, pr);
    tex_t sample = window(10203); sample.d = w.d; sample.d.width = sample.d.height = 4;
    MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    sd.storageMode = MTLStorageModeShared; sd.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> source = [g_dev newTextureWithDescriptor:sd];
    const uint8_t pattern[64] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255,
        0,255,255,255, 255,0,255,255, 64,0,0,255, 0,64,0,255,
        0,0,64,255, 128,64,0,255, 0,128,64,255, 64,0,128,255,
        255,128,0,255, 0,255,128,255, 128,0,255,255, 255,255,255,255};
    [source replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:pattern bytesPerRow:16];
    serial_put(sample.serial, source); mtl_reg_tex(1, &sample, NULL);
    void *vertices = NULL; int allocated = !posix_memalign(&vertices, getpagesize(), 65536);
    CHECK("vertex fixture allocated", allocated); if (!allocated) return;
    memset(vertices, 0, 65536); float triangle[3][4] = {{-1,-1,0,1},{3,-1,0,1},{-1,3,0,1}};
    memcpy(vertices, triangle, sizeof triangle); mtl_pool_add(vertices, 65536);
    state_reset(); memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &w;
    S.prog = S.bst[0] = S.bst[1] = &pg; S.tex[1][0] = 1; S.nattr = 1; S.attr[0].fmt = NVN_FORMAT_RGBA32F;
    S.stream[0].stride = 16; S.vbuf[0] = (NVNbufferRange){(uint64_t)vertices, sizeof triangle};
    S.has_vp = 1; S.vp[0][2] = S.vp[0][3] = 8; S.dr[1] = 1;
    p_draw dw = {.prim=NVN_DRAW_PRIMITIVE_TRIANGLES, .count=3, .instances=1}; g_fd_dw = &dw;
    op_hdr end = {OP_END, 8}; g_exec_next = (const uint8_t *)&end;
    wc_ent *e = wc_try(pr, &dw, NULL);
    CHECK("wc_try creates a drawable-sized target", e && e->pw == 4 && e->ph == 4 && g_wc_base && g_wc_base.width == 4);
    if (!e) goto cleanup;
    {
    uint64_t old = e->pser;
    CHECK("wc_try reuses matching target", wc_try(pr, &dw, NULL) && e->pser == old);
    g_wc_base = nil; g_wc_wser = 0; g_fd_dw = NULL;
    mtl_sub_begin(); uint64_t before = g_stat_draw; draw(&dw, NULL);
    CHECK("production draw redirects and marks the target pending", g_stat_draw == before + 1 && g_wc_cur == e && e->pending == old);
    rp_end();
    id<MTLBuffer> drawn = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(drawn);
    enc_b_copy_texture_buffer(blit(), (id<MTLTexture>)tex_get(old), 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), drawn, 0, 256, 1024);
    CHECK("redirected draw GPU submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error());
    int matched = 1; for (int y = 0; y < 4; y++) matched &= !memcmp((uint8_t *)drawn.contents + y*256, pattern + y*16, 16);
    CHECK("redirected textured draw preserves all 16 spatial colors", matched);
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    CAMetalLayer *present_layer = [CAMetalLayer layer]; present_layer.device = g_dev;
    present_layer.pixelFormat = MTLPixelFormatBGRA8Unorm; present_layer.framebufferOnly = NO;
    present_layer.drawableSize = CGSizeMake(4, 4); [g_q4 addResidencySet:present_layer.residencySet];
    id<CAMetalDrawable> drawable = [present_layer nextDrawable];
    CHECK("present gate and offscreen drawable created", gate && drawable);
    if (!gate || !drawable) goto cleanup;
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1;
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    [g_q4 waitForEvent:gate value:1];
    uint64_t ticket = present_enqueue(&w, 1. / 60, NULL, 0);
    CHECK("production enqueue takes pending target", ticket && e->pending == 0);
    FakeLinkUpdate *update = [FakeLinkUpdate new]; update.drawable = drawable;
    update.targetPresentationTimestamp = CACurrentMediaTime() + 0.016;
    [delegate metalDisplayLink:(CAMetalDisplayLink *)(id)update needsUpdate:(CAMetalDisplayLinkUpdate *)update];
    pthread_mutex_lock(&g_pq_mu); int inflight = g_pq_inflight; pthread_mutex_unlock(&g_pq_mu);
    CHECK("production callback queues gated present", inflight == 1);
    test_layer.drawableSize = CGSizeMake(2, 2); g_fd_dw = &dw; e = wc_try(pr, &dw, NULL);
    CHECK("wc_try resize replaces target", e && e->pser != old && e->pw == 2 && e->ph == 2);
    CHECK("retired target remains registered while present is gated", tex_get(old) != nil);
    gate.signaledValue = 1;
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int completed = !g_pq_inflight && g_pq_done >= ticket; pthread_mutex_unlock(&g_pq_mu);
    CHECK("production present completion arrives", completed);
    CHECK("completion releases retired target", completed && tex_get(old) == nil);
    if (completed) {
        id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(4,4,1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:64];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint8_t *bytes = pixels.contents; int good = cb.status == MTLCommandBufferStatusCompleted;
        for (int i = 0; i < 16; i++) good &= bytes[4*i] == pattern[4*i+2] && bytes[4*i+1] == pattern[4*i+1] && bytes[4*i+2] == pattern[4*i] && bytes[4*i+3] == pattern[4*i+3];
        CHECK("present drawable preserves all 16 asymmetric colors in BGRA", good);
    } else CHECK("present drawable preserves all 16 asymmetric colors in BGRA", 0);
    }
cleanup:
    g_fd_dw = NULL; g_exec_next = NULL; g_wc_base = nil; g_wc_wser = 0;
    mtl_tex_release(&sample); mtl_tex_release(&w); serial_del(pg.serial); g_xv_npresent--; test_layer = nil;
}
int main(void) { @autoreleasepool {
    setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_PSO_HYBRID", "0", 1);
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1); setenv("DM_PSO_USER", "/dev/null", 1);
    alarm(15); setbuf(stdout, NULL); mtl_init(); g_shmap = [NSMutableDictionary dictionary]; production_target(); lifecycle(); guards();
    printf("window copy: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
