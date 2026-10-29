// Conditional rendering (nvnCommandBufferSetRenderEnable / SetRenderEnableConditional, docs/design/vulkan-backend.md 4.7): SetRenderEnable(false) skips draws and clears
// and true restores them; the conditional forms compare the two 64-bit counters at addr and addr + 16 on the GPU (VK_EXT_conditional_rendering over a predicate the backend's
// kernel writes, shaders/predicate.comp): mode 0 renders when they are equal, mode 1 when they differ, with counters the CPU wrote and with counters SAMPLES_PASSED reports wrote
// earlier in the same submission (the predicate reads them after the reports, in GPU order); a clear under a false condition is skipped too. No Metal counterpart (the Metal
// backend evaluates the profile key render_cond on the CPU). Checks: each case's pixels, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/red_frag.h"

static NVNqueue g_q; static hv_cmds g_c; static hv_pool g_p; static NVNprogram g_prog;
static void cond(int mode, const void *addr) { p_render_cond rc = {mode, 0, (uint64_t)(uintptr_t)addr}; hv_emit(&g_c, OP_RENDER_COND, &rc, sizeof rc); }
// A fresh target cleared blue, the condition set by setup, a full-target red draw (and, when clear, a green clear after it under the same condition), read back
static int run(void (*setup)(void), int clear, const char *label, int want_red, int want_green) {
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    cond(2, NULL);
    hv_rt(&g_c, 1, rt, NULL); hv_clear_color(&g_c, 0, 15, 0, 0, 1, 1);
    hv_bind(&g_c, &g_prog, 3); hv_depth(&g_c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&g_c, NVN_FACE_NONE);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_POINTS, 0, 0, 1);   // nothing: the pass opens with the blue clear before the condition
    setup();
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    if (clear) hv_clear_color(&g_c, 0, 15, 0, 1, 0, 1);
    cond(2, NULL);
    hv_submit(&g_q, &g_c);
    uint8_t px[256]; int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px);
    const uint8_t want[4] = {(uint8_t)(want_red ? 255 : 0), (uint8_t)(want_green ? 255 : 0), (uint8_t)(want_red || want_green ? 0 : 255), 255};
    for (int i = 0; ok && i < 64; i++) ok &= !memcmp(px + 4 * i, want, 4);
    if (!ok) fprintf(stderr, "%s: pixel 0 = %u %u %u %u, expected %u %u %u %u\n", label, px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
    t(label, ok);
    return ok;
}
static uint64_t *g_cnt;   // two 16-byte reports' counters
static void off(void) { cond(3, NULL); }
static void on(void) { cond(3, NULL); cond(2, NULL); }
static void eq_cpu(void) { g_cnt[0] = 5; g_cnt[2] = 5; cond(0, g_cnt); }
static void ne_cpu(void) { g_cnt[0] = 5; g_cnt[2] = 6; cond(0, g_cnt); }
static void eq_cpu_m1(void) { g_cnt[0] = 7; g_cnt[2] = 7; cond(1, g_cnt); }
static void ne_cpu_m1(void) { g_cnt[0] = 7; g_cnt[2] = 9; cond(1, g_cnt); }
static void hi_word(void) { g_cnt[0] = 1ull << 40; g_cnt[2] = 0; cond(0, g_cnt); }   // the counters differ in their upper 32 bits only
static void reports(void) {   // SAMPLES_PASSED reports in this submission: 0 (right after a reset) and 64 (after a full draw): not equal
    g_cnt[0] = g_cnt[2] = 3;  // what the CPU sees when the condition is encoded: equal
    uint64_t reset = NVN_COUNTER_TYPE_SAMPLES_PASSED; hv_emit(&g_c, OP_RESET_COUNTER, &reset, 8);
    uint64_t a[2] = {NVN_COUNTER_TYPE_SAMPLES_PASSED, (uint64_t)(uintptr_t)g_cnt}; hv_emit(&g_c, OP_REPORT_COUNTER, a, 16);
    hv_draw(&g_c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_clear_color(&g_c, 0, 15, 0, 0, 1, 1);   // back to blue: what the conditional draw below changes, or not
    a[1] = (uint64_t)(uintptr_t)(g_cnt + 2); hv_emit(&g_c, OP_REPORT_COUNTER, a, 16);
    cond(1, g_cnt);
}

int main(void) {
    hv_up("t_vk_cond_render");
    hv_device(0);
    NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&g_q, &qb);
    if (!hv_pool_make(&g_p, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    g_c = (hv_cmds){g_p.mem + 65536, 0, 65536};
    g_cnt = (uint64_t *)(void *)(g_p.mem + 8192);
    const hv_shader sh[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&g_prog, &g_p, 0, sh, 2);
    run(off, 0, "SetRenderEnable(false) skips the draw", 0, 0);
    run(off, 1, "SetRenderEnable(false) skips a clear too", 0, 0);
    run(on, 0, "SetRenderEnable(true) draws again", 1, 0);
    run(eq_cpu, 0, "mode 0 (equal) with equal counters draws", 1, 0);
    run(ne_cpu, 0, "mode 0 (equal) with different counters skips", 0, 0);
    run(ne_cpu, 1, "a clear under a false condition is skipped", 0, 0);
    run(eq_cpu_m1, 0, "mode 1 (not equal) with equal counters skips", 0, 0);
    run(ne_cpu_m1, 1, "mode 1 (not equal) with different counters draws and clears", 0, 1);
    run(hi_word, 0, "counters that differ in their upper 32 bits are different", 0, 0);
    run(reports, 0, "counters SAMPLES_PASSED reports wrote earlier in the submission are compared on the GPU, after the reports", 1, 0);
    hv_pool_free(&g_p);
    return hv_finish();
}
