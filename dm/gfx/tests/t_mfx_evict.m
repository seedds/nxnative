// GitHub issue #1810 (GPU address faults, Ocarina of Time with DM_UPSCALER=metalfx and dynamic resolution): the game's upscaler drawn by MetalFX keeps UP_MFX_SCALERS scalers, one per
// input / output size, colour and format (nvn_mtl.m up_mfx_scaler_for). Previously it evicted by creation order (taking the oldest created slot even if used a frame ago),
// took the evicted scaler's own input and output textures out of the residency set at once (res_remove) before their fence completed, and released the scaler object immediately
// even though a Metal 4 command buffer does not retain what is encoded into it.
//
// This test checks LRU eviction (a recently used scaler stays cached while a colder slot is taken), deferred residency and scaler lifetime while a submission is open (covering
// both scaler-owned input and output textures via UP_COLOR_ROOT), and clean release / residency removal once the submission completes. It never hands the GPU a texture outside
// the residency set. No game shader runs (the "upscaler" is a reflection of one texture and one image, as in t_upscaler_mfx.m).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static NvShader *upscaler(const char *key) {
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->up_color = UP_COLOR_ROOT;
    s->rf.nb = 2; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    return s;
}
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = o && [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
int main(void) { @autoreleasepool {
    if (!DM_GAME_UPSCALER_N) { printf("t_mfx_evict: this profile has no upscaler shaders, nothing to check\n"); return 0; }
    setenv("DM_UPSCALER", "metalfx", 1);
    mtl_init();
    if (![MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) { printf("t_mfx_evict: no Metal 4 MetalFX on this GPU, skipped\n"); return 0; }
    // Square-rooted R11G11B10F into R11G11B10F (as Ocarina of Time's upscaler): both the input (squared copy) and output (drawn back) are the scaler's own RGBA16F textures
    tex_t in = texture(77, 960, 540, NVN_FORMAT_R11G11B10F), out = texture(78, 1920, 1080, NVN_FORMAT_R11G11B10F);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_tex_init(&in); mtl_tex_init(&out);
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL);
    char key[24]; snprintf(key, sizeof key, "%.16s_b", DM_GAME_UPSCALER_KEYS);   // the profile's first upscaler
    NvShader *named = upscaler(key);
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }

    // 1. One frame's scale, completed: its scaler now has input and output textures of its own
    mtl_sub_begin();
    CHECK(up_mfx_encode(named), "the first scale is drawn by MetalFX");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the first submission completes");
    int slot = -1; for (int i = 0; i < UP_MFX_SCALERS; i++) if (g_up_mfx[i].out) slot = i;
    CHECK(slot >= 0 && g_up_mfx[slot].in && g_up_mfx[slot].out, "the scaler keeps both input and output textures of its own");
    if (slot < 0 || !g_up_mfx[slot].in || !g_up_mfx[slot].out) { printf("t_mfx_evict: %d checks, %d failed\n", checks, failed); return 1; }
    id<MTLTexture> own_in = g_up_mfx[slot].in, own_out = g_up_mfx[slot].out;
    __weak id own_scaler = g_up_mfx[slot].sc;
    CHECK(resident(own_in) && resident(own_out), "the scaler's own textures are in the residency set");

    // 2. LRU eviction: fill the other slots, touch the first scaler again, then allocate one more size — the active scaler stays cached and a colder slot is taken
    mtl_sub_begin();
    int made = 0;
    for (int i = 0; i < UP_MFX_SCALERS - 1; i++) { made++;
        up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float); }
    CHECK(up_mfx_scaler_for(960, 540, 1920, 1080, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float) == &g_up_mfx[slot], "touching the first scaler hits its slot");
    made++;
    up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float);
    CHECK(g_up_mfx[slot].out == own_out && g_up_mfx[slot].in == own_in, "LRU keeps the recently used scaler when a new size arrives");

    // 3. Step through enough new sizes without touching the first scaler to evict it while the submission is still open: its scaler must stay alive and both textures stay resident
    int extra = 0;
    while (g_up_mfx[slot].out == own_out && extra < 2 * UP_MFX_SCALERS) { made++; extra++;
        up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float); }
    CHECK(g_up_mfx[slot].out != own_out, "cold sizes eventually take the first scaler's slot");
    CHECK(own_scaler != nil, "the evicted scaler stays alive until the open submission has completed: a Metal 4 command buffer does not retain it");
    int in_ok = resident(own_in), out_ok = resident(own_out);
    CHECK(in_ok && out_ok, "both textures of the evicted scaler stay in the residency set until the open submission, behind whose fence they were retired, has completed");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the second submission completes");

    // 4. Once the submission that used them has completed and drained, the textures leave the residency set and the scaler is freed: nothing leaks
    if (in_ok && out_ok) {
        mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "an empty submission completes");
        for (int i = 0; i < 100 && (resident(own_in) || resident(own_out) || own_scaler != nil); i++) { defer_drain(); res_commit(); usleep(10000); }
        CHECK(!resident(own_in) && !resident(own_out), "after the submissions that used them, both evicted textures leave the residency set");
        CHECK(own_scaler == nil, "after the submissions that used it, the evicted scaler is released");
    }
    printf("t_mfx_evict: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
