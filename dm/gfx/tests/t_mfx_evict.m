// GitHub issue #1810 (GPU address faults, Ocarina of Time with DM_UPSCALER=metalfx and dynamic resolution): the game's upscaler drawn by MetalFX keeps UP_MFX_SCALERS scalers, one per
// input / output size (nvn_mtl.m up_mfx_scaler_for), in a ring filled in order of creation. A size the ring does not hold takes the oldest slot, and the scaler that was there
// loses its own textures (the RGBA16F output MetalFX writes and the game's image is drawn back from, and the input copy). They are released behind the next fence
// (defer_obj_release: "a recorded scale still holds them"), but taken out of the residency set at once (res_remove), so a submission that has already recorded a scale with
// them, or one still on the GPU, is left with textures outside the set: what every other retirement in nvn_mtl.m avoids (defer_drain, arena_trim, fx_encode's wait for the
// presents in flight), and what macOS answers with kIOGPUCommandBufferCallbackErrorPageFault.
//
// This test never hands the GPU such a command buffer: the open submission in step 2 stands for the one whose recorded scale still uses the textures and records nothing
// itself, so a host without a fix fails the check and still runs nothing with a texture outside the set. No game shader runs (the scaler is MetalFX's; the "upscaler" is a
// reflection of one texture and one image, as in t_upscaler_mfx.m).
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
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->up_color = UP_COLOR_LINEAR;
    s->rf.nb = 2; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    return s;
}
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = o && [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
int main(void) { @autoreleasepool {
    if (!DM_GAME_UPSCALER_N) { printf("t_mfx_evict: this profile has no upscaler shaders, nothing to check\n"); return 0; }
    setenv("DM_UPSCALER", "metalfx", 1);
    mtl_init();
    // R11G11B10F into R11G11B10F, as Ocarina of Time's upscaler: the output is the scaler's own RGBA16F texture, drawn back into the game's image
    tex_t in = texture(77, 960, 540, NVN_FORMAT_R11G11B10F), out = texture(78, 1920, 1080, NVN_FORMAT_R11G11B10F);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_tex_init(&in); mtl_tex_init(&out);
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL);
    char key[24]; snprintf(key, sizeof key, "%.16s_b", DM_GAME_UPSCALER_KEYS);   // the profile's first upscaler
    NvShader *named = upscaler(key);
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }

    // 1. One frame's scale, completed: its scaler now has textures of its own
    mtl_sub_begin();
    CHECK(up_mfx_encode(named), "the first scale is drawn by MetalFX");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the first submission completes");
    int slot = -1; for (int i = 0; i < UP_MFX_SCALERS; i++) if (g_up_mfx[i].out) slot = i;
    CHECK(slot >= 0, "the scaler keeps an output texture of its own (the game's image is R11G11B10F)");
    if (slot < 0) { printf("t_mfx_evict: %d checks, %d failed\n", checks, failed); return 1; }
    id<MTLTexture> own_out = g_up_mfx[slot].out, own_in = g_up_mfx[slot].in;
    __weak id own_scaler = g_up_mfx[slot].sc;
    CHECK(resident(own_out) && (!own_in || resident(own_in)), "the scaler's own textures are in the residency set");

    // 2. A submission is open (the frame whose scale, at the size used a moment ago, would be recorded in it), and the resolution steps through sizes the ring does not
    //    hold until one takes the first scaler's slot: the ring goes by creation, not by last use. The textures it gives up were retired behind this submission's fence
    //    (defer_obj_release), so until it has completed they must still be in the residency set
    mtl_sub_begin();
    int made = 0;
    while (g_up_mfx[slot].out == own_out && made < 2 * UP_MFX_SCALERS) { made++;
        up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float); }
    CHECK(g_up_mfx[slot].out != own_out, "new sizes took the first scaler's slot");
    CHECK(own_scaler != nil, "the scaler the ring gave up stays alive until the open submission has completed: a Metal 4 command buffer does not retain it");
    int out_ok = resident(own_out), in_ok = !own_in || resident(own_in);
    CHECK(out_ok && in_ok, "the textures of the scaler the ring gave up stay in the residency set until the open submission, behind whose fence they were retired, has completed");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the second submission completes");

    // 3. With the retirement left to the deferred-release queue, they leave the set once the submission that used them has completed: nothing leaks
    if (out_ok && in_ok) {
        mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "an empty submission completes");
        for (int i = 0; i < 100 && resident(own_out); i++) { defer_drain(); res_commit(); usleep(10000); }
        CHECK(!resident(own_out) && (!own_in || !resident(own_in)), "after the submissions that used them, the given-up scaler's textures leave the residency set");
    }
    printf("t_mfx_evict: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
