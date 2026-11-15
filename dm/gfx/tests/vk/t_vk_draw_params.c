// A vertex shader that declares the DrawParameters capability (it reads gl_BaseInstance, as the translations of game shaders that read the base vertex, the base instance or
// the draw index do; Mario vs. Donkey Kong's level and world intro): the device is made with shaderDrawParameters where it has it (vk_device.c), so the module is valid, and a
// device without it refuses the module with a red line (vk_shader.c caps_missing). Before, the device was made without the feature: every pipeline with such a module was a
// validation error (VUID-VkShaderModuleCreateInfo-pCode-08740), 146 of them on the way into Mario vs. Donkey Kong's level 1-1. No Metal counterpart. Checks: the feature is
// enabled where the device has it, draws with base instances 0 and 2 land (the depth the shader gives each), and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/dp_vert.h"
#include "shaders/gen/red_frag.h"

int main(void) {
    const hostvk_dev *d = hv_up("t_vk_draw_params");
    hv_device(0);
    char name[200];
    snprintf(name, sizeof name, "shaderDrawParameters offered by the device and enabled: %u (1 on lavapipe)", (unsigned)d->x.draw_parameters);
    t(name, 1);
    if (!d->x.draw_parameters) { printf("note: the device lacks shaderDrawParameters: a module that declares DrawParameters is refused (not drawn here)\n"); return hv_finish(); }
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    hv_pool p; if (!hv_pool_make(&p, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds c = {p.mem + 65536, 0, 65536};
    NVNprogram prog;
    const hv_shader sh[2] = {{"dp", 0, HV_SPV(dp_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog, &p, 0, sh, 2);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 8, 8, 1, 1, 0), depth = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH32F, 8, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    float zs[2] = {-1, -1};
    for (int bi = 0; bi <= 2; bi += 2) {
        hv_rt(&c, 1, rt, &depth); hv_clear_color(&c, 0, 15, 0, 0, 1, 1);
        p_clear_ds cd = {1.0f, 1, 0, 0}; hv_emit(&c, OP_CLEAR_DS, &cd, sizeof cd);
        hv_bind(&c, &prog, 3); hv_depth(&c, 1, 1, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&c, NVN_FACE_NONE);
        p_draw dw; memset(&dw, 0, sizeof dw); dw.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; dw.count = 3; dw.instances = 1; dw.base_instance = bi;
        hv_emit(&c, OP_DRAW, &dw, offsetof(p_draw, ind));   // as the frontend records a direct draw (hv_draw), with its base instance
        hv_submit(&q, &c);
        uint8_t px[256]; float z[64];
        int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, 8, 8, 4, px) && hv_read(&depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 0, 0, 8, 8, 4, z);
        for (int i = 0; ok && i < 64; i++) ok &= px[4 * i] == 255 && px[4 * i + 2] == 0 && z[i] == z[0];
        zs[bi / 2] = z[0];
        if (!ok) fprintf(stderr, "base instance %d: pixel 0 = %u %u %u, depth %g\n", bi, px[0], px[1], px[2], (double)z[0]);
        snprintf(name, sizeof name, "a draw with base instance %d: red over the whole target, at one depth", bi);
        t(name, ok);
    }
    // clip z 0 and 0.5 from gl_BaseInstance 0 and 2: depth 0 and 0.5 in the zero-to-one depth mode, 0.5 and 0.75 in NVN's default (near at -w)
    snprintf(name, sizeof name, "the depths follow gl_BaseInstance (%g, %g)", (double)zs[0], (double)zs[1]);
    t(name, (zs[0] == 0.0f && zs[1] == 0.5f) || (zs[0] == 0.5f && zs[1] == 0.75f));
    hv_pool_free(&p);
    return hv_finish();
}
