// The Vulkan backend's DM_CTL dumps (dm/gfx/hostvk/vk_dump.c; docs/env_switches.tsv DM_CTL): "passdump" logs the next frame's pass (its attachment, serial, format and load
// operation), its draw (shader keys, primitive) and the pass end with its draw count; "texdump" copies the attachment as the pass ends and writes it as a PNG with a statistics
// line whose per-channel ranges and means are those of the picture drawn (a blue clear, a red draw over the left half); "texdump - draws <pass> <slot>" splits that pass before
// every draw and says what each draw changed (its texel count and bounding box), writing no PNG. No Metal counterpart (the Metal backend's dumps are checked on its routes).
// Checks: the lines, the PNG's size, and the validation check
#include "t_vk_harness.h"
#include "shaders/gen/fullscreen_vert.h"
#include "shaders/gen/red_frag.h"

void dm_gfx_passdump_next(long n);
void dm_gfx_texdump_next(const char *arg);
int hvk_dump_passes(void);
int hvk_dump_snaps(void);
enum { NLINE = 256 };
static char g_line[NLINE][600]; static int g_nline;
static void on_log(const char *line) {
    fprintf(stderr, "[dm] %s\n", line);
    if ((strstr(line, "[pass]") || strstr(line, "[texdump]")) && g_nline < NLINE) snprintf(g_line[g_nline++], sizeof g_line[0], "%s", line);
}
static const char *find(const char *a, const char *b, const char *c) {   // the first captured line holding all three parts (NULL: any)
    for (int i = 0; i < g_nline; i++) if (strstr(g_line[i], a) && (!b || strstr(g_line[i], b)) && (!c || strstr(g_line[i], c))) return g_line[i];
    return NULL;
}
static int png_size(const char *path, uint32_t *w, uint32_t *h) {   // the IHDR's width and height
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t b[24]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
    if (n < 24 || memcmp(b + 12, "IHDR", 4)) return 0;
    *w = (uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19]; *h = (uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23];
    return 1;
}

int main(void) {
    hostvk_set_log(on_log);
    hv_up("t_vk_dump");
    hv_device(0);
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    hv_pool p; if (!hv_pool_make(&p, 1 << 18)) { t("memory pool allocated", 0); return hv_finish(); }
    hv_cmds c = {p.mem + 65536, 0, 65536};
    NVNprogram prog;
    const hv_shader sh[2] = {{"fullscreen", 0, HV_SPV(fullscreen_vert_spv), NULL, NULL}, {"red", 1, HV_SPV(red_frag_spv), NULL, NULL}};
    hv_program(&prog, &p, 0, sh, 2);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 16, 8, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    char png[700], want[200]; snprintf(png, sizeof png, "%s/t_vk_dump_%%d.png", g_lib);

    // Frame 11: both dumps. A blue clear folded into the pass, a red draw over the left half (8 of 16 columns)
    be_set_frame(10);
    dm_gfx_passdump_next(1); dm_gfx_texdump_next(png);
    be_set_frame(11);
    hv_rt(&c, 1, rt, NULL); hv_scissor(&c, 0, 0, 16, 8); hv_clear_color(&c, 0, 15, 0, 0, 1, 1);
    hv_bind(&c, &prog, 3); hv_depth(&c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&c, NVN_FACE_NONE); hv_scissor(&c, 0, 0, 8, 8);
    hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&q, &c);
    snprintf(want, sizeof want, "c0=ser%llu fmt%u", (unsigned long long)target.serial, (unsigned)VK_FORMAT_R8G8B8A8_UNORM);
    t("passdump: a P line with the attachment's serial and format and its folded clear", find("[pass] F11 P 1 16x8", want, "clear(0,0,1,1)") != NULL);
    t("passdump: a D line with both shaders' keys and the primitive", find("[pass] F11 D 1 vs=", "fs=", "prim 4 count 3") != NULL);
    t("passdump: the pass end with its draw count", find("[pass] F11 E 1 draws 1", NULL, NULL) != NULL);
    const char *s = find("[texdump] F11 #0 pass 1 colour slot 0", "16x8", NULL);
    t("texdump: a line for the attachment as its pass ended", s != NULL);
    t("texdump: its statistics are the picture's (red on half, blue on the other half, no black texel)",
      s && strstr(s, "r [0, 1] mean 0.5;") && strstr(s, "g [0, 0] mean 0;") && strstr(s, "b [0, 1] mean 0.5;") && strstr(s, "colour all zero 0.0 %"));
    char path[800]; uint32_t w = 0, h = 0; snprintf(path, sizeof path, png, 0);
    t("texdump: the PNG is written with the attachment's size", png_size(path, &w, &h) && w == 16 && h == 8);
    remove(path);
    t("frame 12 is not dumped", (be_set_frame(12), hvk_dump_passes() == 0 && hvk_dump_snaps() == 0));

    // Frame 13: the per-draw split of pass 1, slot 0, no PNG. A clear, a red draw over 4x8 texels, then one over 8x8 (4 more columns)
    dm_gfx_texdump_next("- draws 1 0");
    be_set_frame(13);
    hv_rt(&c, 1, rt, NULL); hv_scissor(&c, 0, 0, 16, 8); hv_clear_color(&c, 0, 15, 0, 0, 1, 1);
    hv_bind(&c, &prog, 3); hv_scissor(&c, 0, 0, 4, 8); hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_scissor(&c, 0, 0, 8, 8); hv_draw(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 1);
    hv_submit(&q, &c);
    t("texdump draws: a snapshot after the first draw", find("[texdump] F13 #0 pass 1 colour slot 0", "r [0, 1] mean 0.25;", NULL) != NULL);
    t("texdump draws: the second draw's own snapshot says it changed the 4x8 texels it drew", find("[texdump] F13 #1 pass 2 colour slot 0", "changed 32 texels (25.0 %) in 4,0..7,7", NULL) != NULL);
    t("texdump draws: no PNG (path -)", !find("[texdump] F13", "->", NULL));
    be_set_frame(15);
    hv_pool_free(&p);
    return hv_finish();
}
