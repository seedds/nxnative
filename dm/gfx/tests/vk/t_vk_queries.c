// Counter reports (nvnCommandBufferReportCounter / ResetCounter, docs/design/vulkan-backend.md 4.6) as the GPU writes them into guest memory: SAMPLES_PASSED counts the samples
// of every draw since the last reset, across passes, a culled draw and a submission boundary, and reads 0 right after a reset; TIMESTAMP and TIMESTAMP_TOP write NVN ticks
// (614.4 MHz, nvn_timestamp.h) on the host clock, ordered, inside the CPU's window around the submission, with the counter field holding the timestamp (the Metal backend's form);
// every report carries a timestamp; a counter type the backend does not map (pipeline statistics, phase P8) is still written, with a zero count, and said once. No Metal
// counterpart: the Metal backend's reports are checked by its game smoke routes. Checks: each report's value, the timestamps' order and window, and the validation check
#include "t_vk_harness.h"
#include "../../nvn/nvn_timestamp.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/red_frag.h"

typedef struct { uint64_t counter, timestamp; } report;
static void reset(hv_cmds *c, uint64_t type) { hv_emit(c, OP_RESET_COUNTER, &type, 8); }
static void rep(hv_cmds *c, uint64_t type, report *at) { uint64_t a[2] = {type, (uint64_t)(uintptr_t)at}; hv_emit(c, OP_REPORT_COUNTER, a, 16); }

int main(void) {
    hv_up("t_vk_queries");
    hv_device(0);
    NVNqueue queue; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&queue, &qb);
    hv_pool mp; if (!hv_pool_make(&mp, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds c = {mp.mem + 65536, 0, 65536};
    report *r = (report *)(void *)(mp.mem + 8192); memset(r, 0xEE, 64 * sizeof *r);
    static NVNprogram prog;
    const hv_shader sh[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog, &mp, 0, sh, 2);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    uint64_t cpu0 = nvn_timestamp_from_ns(plat_now_ns());
    hv_rt(&c, 1, rt, NULL); hv_bind(&c, &prog, 3); hv_depth(&c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&c, NVN_FACE_NONE); hv_scissor(&c, 0, 0, 8, 8);
    reset(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[0]);                                      // 0: nothing drawn since the reset
    hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[1]);   // 64
    hv_scissor(&c, 0, 0, 4, 4); hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[2]);   // + 16
    hv_cull(&c, NVN_FACE_FRONT_AND_BACK); hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[3]);   // culled: + 0
    hv_cull(&c, NVN_FACE_NONE);
    hv_clear_color(&c, 0, 15, 0, 0, 0, 0);   // a clear inside the pass draws no sample that counts
    hv_t2b(&c, &target, NULL, mp.mem + 16384, 0, 0, 0, 8, 8, 1);   // a copy between: the pass ends, the counting goes on in the next
    hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[4]);   // + 16 (the scissor is 4×4)
    rep(&c, NVN_COUNTER_TYPE_TIMESTAMP, &r[5]); rep(&c, NVN_COUNTER_TYPE_TIMESTAMP_TOP, &r[6]);
    rep(&c, NVN_COUNTER_TYPE_PRIMITIVES_GENERATED, &r[7]);
    hv_submit(&queue, &c);
    hv_scissor(&c, 0, 0, 8, 8); hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[8]);   // the next submission: + 64
    reset(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[9]);                                        // reset: 0
    hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1); rep(&c, NVN_COUNTER_TYPE_SAMPLES_PASSED, &r[10]);                             // 64
    rep(&c, NVN_COUNTER_TYPE_TIMESTAMP, &r[11]);
    hv_submit(&queue, &c);
    uint64_t cpu1 = nvn_timestamp_from_ns(plat_now_ns());
    const struct { int i; uint64_t want; const char *label; } occ[] = {
        {0, 0, "a report right after a reset counts 0"}, {1, 64, "a full-target draw passes 64 samples"}, {2, 80, "a 4x4-scissored draw adds 16"},
        {3, 80, "a draw with both faces culled adds nothing"}, {4, 96, "the count goes on across a pass split (a copy between draws)"},
        {8, 160, "the count goes on across a submission"}, {9, 0, "a reset in the next submission starts again at 0"}, {10, 64, "and counts the draw after it"},
    };
    for (unsigned k = 0; k < sizeof occ / sizeof *occ; k++) {
        if (r[occ[k].i].counter != occ[k].want) fprintf(stderr, "report %d: %llu samples, expected %llu\n", occ[k].i, (unsigned long long)r[occ[k].i].counter, (unsigned long long)occ[k].want);
        t(occ[k].label, r[occ[k].i].counter == occ[k].want);
    }
    int ordered = 1, inside = 1;
    const int seq[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11};
    for (unsigned k = 0; k < sizeof seq / sizeof *seq; k++) {
        uint64_t ts = r[seq[k]].timestamp;
        inside &= ts >= cpu0 && ts <= cpu1;
        if (k) ordered &= ts >= r[seq[k - 1]].timestamp;
    }
    if (!inside) fprintf(stderr, "timestamps %llu .. %llu, the CPU's window %llu .. %llu (NVN ticks)\n", (unsigned long long)r[0].timestamp, (unsigned long long)r[11].timestamp,
                         (unsigned long long)cpu0, (unsigned long long)cpu1);
    t("every report's timestamp lies in the CPU's window around the submissions (host clock, NVN ticks)", inside);
    t("the timestamps follow the reports' order", ordered);
    t("TIMESTAMP: the counter field holds the timestamp", r[5].counter == r[5].timestamp && r[5].timestamp >= r[4].timestamp);
    t("TIMESTAMP_TOP is written in order", r[6].counter == r[6].timestamp && r[6].timestamp >= cpu0 && r[6].timestamp <= cpu1);
    t("an unmapped counter type is written with a zero count (declared, phase P8)", r[7].counter == 0 && r[7].timestamp != 0xEEEEEEEEEEEEEEEEull);
    hv_pool_free(&mp);
    return hv_finish();
}
