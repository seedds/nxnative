// NVN's queue initialization captures WINDOW_ORIGIN.FLIP_Y; the device origin used to construct later viewports can change independently (docs/reference/rendering.md, "Queue
// initialization captures the raster origin"). The Vulkan port of dm/gfx/tests/t_queue_origin.m: two queues created under different device origins on one device, alternated, every
// cull mode, both windings, triangles of both orientations, a negated-y viewport swizzle or none, both current device origins; each draw's pixels read back from an offscreen render
// target (green front, red back, the blue clear where culled). Also the per-queue state of the frontend (nvn_state.c) as the backend switches it.
// 394 checks: the 389 rendering.md quotes and the 5 per-queue state checks the Metal test gained since. The Metal test's "full-coverage proof agrees with native culling" checks a Metal
// look-ahead (first_draw_covers) the Vulkan backend has not; in its place each draw checks the raster state the backend gave Vulkan (cull mode, front face, the viewport's
// direction) against what the NVN semantics above derive
#include "t_vk_harness.h"
#include "shaders/gen/pos_vert.h"
#include "shaders/gen/facing_frag.h"
void hvk_test_raster(uint32_t *cull, uint32_t *front, int *flipped);

int main(void) {
    hv_up("t_vk_queue_origin");
    static NVNqueue queues[2];
    hv_device(0);
    t("device initialized", ((dev_t_ *)&g_dev)->magic == MAGIC('D', 'E', 'V', 0));
    hv_pool vp; t("vertex storage allocated", hv_pool_make(&vp, 65536));
    float *vertices = (float *)(void *)vp.mem;
    uint8_t *pixels = vp.mem + 32768;
    hv_cmds cmds = {vp.mem + 40960, 0, 16384};
    for (int origin = 0; origin < 2; origin++) {
        h_nvnDeviceSetWindowOriginMode(&g_dev, (NVNwindowOriginMode)origin);
        NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev);
        t("queue initialized", h_nvnQueueInitialize(&queues[origin], &qb));
    }
    // The program: the position as it is, and the facing colour; its shaders go into the library under the keys the frontend computes
    static NVNprogram prog;
    const hv_shader sh[2] = {{"pos", 0, pos_vert_spv, sizeof pos_vert_spv, NULL, "[{\"location\":0,\"type\":\"float\",\"n\":4}]"},
                             {"facing", 1, facing_frag_spv, sizeof facing_frag_spv, NULL, NULL}};
    hv_program(&prog, &vp, 16384, sh, 2);
    char path[800]; FILE *f; uint8_t md[8]; char key[17]; int have = 1;
    for (int i = 0; i < 2; i++) { prog_key((const prog_t *)&prog, (unsigned)i, md); for (int b = 0; b < 8; b++) snprintf(key + 2 * b, 3, "%02x", md[b]);
        snprintf(path, sizeof path, "%s/%s.spv", g_libv, key); if ((f = fopen(path, "rb"))) fclose(f); else have = 0; }
    t("test shaders in the library under the program's keys", have);
    // The frontend's per-queue state (nvn_state.c), switched by be_queue_activate as the Metal backend switches it
    uint64_t first_serial = ((queue_t *)&queues[0])->serial;
    be_queue_activate(first_serial); S.tex_base = 123; R.has_sc = 1; R.sc.x = 77;
    be_queue_activate(((queue_t *)&queues[1])->serial);
    t("a new queue starts with independent default state", S.tex_base == 0 && !R.has_sc && !S.bst[0]);
    S.tex_base = 456;
    be_queue_activate(first_serial);
    t("switching back restores the first queue state", S.tex_base == 123 && R.has_sc && R.sc.x == 77);
    be_queue_activate(((queue_t *)&queues[1])->serial);
    t("the second queue keeps its own bindings", S.tex_base == 456 && !R.has_sc);
    be_queue_activate(first_serial); S.bst[0] = S.prog = (const prog_t *)(uintptr_t)0xdead0000;
    h_nvnQueueFinalize(&queues[0]);
    h_nvnDeviceSetWindowOriginMode(&g_dev, (NVNwindowOriginMode)0);
    NVNqueueBuilder fresh; memset(&fresh, 0, sizeof fresh); h_nvnQueueBuilderSetDefaults(&fresh); h_nvnQueueBuilderSetDevice(&fresh, &g_dev);
    t("queue storage can be initialized again", h_nvnQueueInitialize(&queues[0], &fresh));
    be_queue_activate(((queue_t *)&queues[0])->serial);
    t("reinitializing a queue never inherits finalized program pointers", ((queue_t *)&queues[0])->serial != first_serial && !S.bst[0] && !S.prog && !R.has_sc);

    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0);
    // Native GPU state: matching queue / device origins keep NVN's NDC facing; differing origins reverse it. CW and CCW expectations are explicit here (the Metal test's table)
    const int front_in_clip[2][2][2] = {{{0, 1}, {1, 0}}, {{1, 0}, {0, 1}}};
    int bad_pixels = 0, bad_state = 0;
    for (int current = 0; current < 2; current++) for (int winding = 0; winding < 2; winding++)
    for (int cull = 0; cull < 4; cull++) for (int ccw = 0; ccw < 2; ccw++) for (int flip = 0; flip < 2; flip++) for (int queue = 0; queue < 2; queue++) {
        h_nvnDeviceSetWindowOriginMode(&g_dev, (NVNwindowOriginMode)current);
        float positions[3][4] = {{-1, -1, .5f, 1}, {3, -1, .5f, 1}, {-1, 3, .5f, 1}};
        if (!ccw) { float tmp[4]; memcpy(tmp, positions[1], 16); memcpy(positions[1], positions[2], 16); memcpy(positions[2], tmp, 16); }
        memcpy(vertices, positions, sizeof positions);
        memset(pixels, 0xEE, 256);
        p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = &target; hv_emit(&cmds, OP_SET_RT, &rt, sizeof rt);
        p_clear_color clear = {.index = 0, .mask = 15, .rgba = {0, 0, 1, 1}}; hv_emit(&cmds, OP_CLEAR_COLOR, &clear, sizeof clear);
        p_bind_program bp = {(const prog_t *)&prog, 3}; hv_emit(&cmds, OP_BIND_PROGRAM, &bp, sizeof bp);
        ds_st ds; memset(&ds, 0, sizeof ds); ds.df = NVN_DEPTH_FUNC_ALWAYS; p_bind_state bs; memset(&bs, 0, sizeof bs); bs.kind = 5; memcpy(bs.bytes, &ds, sizeof ds); hv_emit(&cmds, OP_BIND_STATE, &bs, sizeof bs);
        poly_st poly; memset(&poly, 0, sizeof poly); poly.cull = (uint32_t)cull; poly.front = (uint32_t)winding; poly.mode = NVN_POLYGON_MODE_FILL;
        bs.kind = 4; memset(bs.bytes, 0, sizeof bs.bytes); memcpy(bs.bytes, &poly, sizeof poly); hv_emit(&cmds, OP_BIND_STATE, &bs, sizeof bs);
        struct { int32_t first, n; va_st attr; } attr = {0, 1, {.fmt = NVN_FORMAT_RGBA32F}}; hv_emit(&cmds, OP_BIND_VATTR, &attr, sizeof attr);
        struct { int32_t first, n; vs_st stream; } stream = {0, 1, {.stride = 16}}; hv_emit(&cmds, OP_BIND_VSTREAM, &stream, sizeof stream);
        p_bind_vbuf1 vb = {0, (uint32_t)sizeof positions, (uint64_t)(uintptr_t)vertices}; hv_emit(&cmds, OP_BIND_VBUF1, &vb, sizeof vb);
        p_vp_swizzle sw; memset(&sw, 0, sizeof sw); sw.n = 1; sw.sw[0][1] = (uint8_t)flip; hv_emit(&cmds, OP_VP_SWIZZLE, &sw, sizeof sw);
        p_draw draw; memset(&draw, 0, sizeof draw); draw.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; draw.count = 3; draw.instances = 1; hv_emit(&cmds, OP_DRAW, &draw, 24);
        p_copy_b2t rb; memset(&rb, 0, sizeof rb); rb.src = (uint64_t)(uintptr_t)pixels; rb.tex = &target; rb.r = (NVNcopyRegion){0, 0, 0, 8, 8, 1};
        hv_emit(&cmds, OP_COPY_T2B, &rb, sizeof rb);
        hv_submit(&queues[queue], &cmds);
        int front = ccw == (front_in_clip[queue][current][winding] ^ flip);
        int visible = cull == NVN_FACE_NONE || (cull == NVN_FACE_FRONT && !front) || (cull == NVN_FACE_BACK && front);
        // the raster state Vulkan was given: the cull mode as NVN names it, the front face from the queue's captured origin (queues[0] was made under the lower-left origin),
        // the viewport flipped for the upper-left device origin and again for the negated y
        uint32_t vc, vf; int vflip; hvk_test_raster(&vc, &vf, &vflip);
        int queue_ll = queue == 0;
        uint32_t want_cull = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
        uint32_t want_front = (winding == NVN_FRONT_FACE_CCW) != queue_ll ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        int want_flip = (current == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT) ^ flip;
        int state_ok = vc == want_cull && vf == want_front && vflip == want_flip;
        bad_state += !state_ok;
        t("the raster state given to Vulkan: cull mode, front face from the queue's origin, viewport direction", state_ok);
        int written = 1; for (int i = 0; i < 256; i++) if (pixels[i] == 0xEE && (i % 4 != 3)) { written = 0; break; }
        t("pixel readback completes", written);
        uint8_t expected[4] = {(uint8_t)(visible && !front ? 255 : 0), (uint8_t)(visible && front ? 255 : 0), (uint8_t)(visible ? 0 : 255), 255};
        int good = 1; for (int i = 0; i < 64; i++) good &= !memcmp(pixels + 4 * i, expected, 4);
        if (!good) { bad_pixels++; fprintf(stderr, "queue origin=%d device origin=%d front=%d cull=%d triangle CCW=%d flip=%d: pixel 0 = %u %u %u %u, expected %u %u %u %u\n", queue, current, winding, cull,
                                           ccw, flip, pixels[0], pixels[1], pixels[2], pixels[3], expected[0], expected[1], expected[2], expected[3]); }
        t("queue-latched culling and front_facing match native GPU state", good);
    }
    printf("queue origin: %d draws with wrong pixels, %d with a wrong raster state\n", bad_pixels, bad_state);
    hv_pool_free(&vp);
    return hv_finish();
}
