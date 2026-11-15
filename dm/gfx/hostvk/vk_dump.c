// The Vulkan backend's pass and texture dumps: the counterparts of the Metal backend's DM_CTL "passdump" and "texdump" (nvn_mtl.m), and "texraw" / "texlist", for finding which pass
// of a frame goes wrong (docs/agents/04-graphics.md, "Diagnostics"; docs/env_switches.tsv DM_CTL).
// - passdump <n>: the next n frames' commands as "[pass] F<frame> ..." lines, in recording order: P (a render pass: its attachments, serial, VkFormat, size, level, layer, load
//   operation), D (a draw: its shaders' keys, primitive, count, instances, indexed or indirect), Q (a draw or dispatch skipped, and why), B (a binding of the draw or dispatch:
//   texture ID, serial, format, or the dummy; uniform buffers with their address and first four floats), C (a dispatch), E (the pass's end and its draw count), X (clears),
//   T (copies), Y (counter reports), R (render conditions, with the two counters as the CPU sees them when the condition is recorded). Every draw is listed, not only the first
//   of each pass (Metal's DM_NVN_PASSDUMP_ALL), since one frame of a 3D scene is a few hundred draws. The lines are written by vk_commands.c and vk_pipeline.c through
//   hvk_dump_line; the pass numbers count the frame's render passes as the backend begins them.
// - texdump <path with %d>: the next frame's colour and depth attachments as each pass ends, and the storage images each dispatch wrote, copied into readback buffers in the
//   command buffer at that point of the GPU timeline (Metal's DM_NVN_TEXSNAP copies them in a separate command buffer; here each snapshot is exactly what the next command reads).
//   Each is written as an uncompressed PNG once its submission completed (%d = the snapshot's number in GPU order) with a "[texdump]" line: the pass, the slot, the serial, the
//   VkFormat, the size and, per channel, minimum, maximum and mean, the non-finite texels and the share of texels whose colour channels are all zero (a black target shows there).
//   Float formats are clamped to [0, 1], depth is stretched between its minimum and maximum; at most 1 GiB of snapshots per frame (a line says what was left out).
//   "texdump <path> draws <pass> [<slot>]" splits that pass before every draw (below), "-" as the path writes the lines only.
// - texraw <serial> <path>: level 0, layer 0 of the texture with that serial, raw (a text header line "width height VkFormat row-bytes", then the rows), and a PNG beside it;
//   a BC texture's blocks raw, with no PNG.
// - texlist <target>: the textures made so far of that NVN target (-1: all), with their serial, format, size, levels and layers.
#include "plat/plat.h"
#include "vk_backend.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hvk_png_write(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h);   // vk_backend.c
void hvk_finish(void);                                                              // vk_commands.c
int hvk_oneoff_begin(VkCommandBuffer *cb);
int hvk_oneoff_end(VkCommandBuffer cb);

// ---------- Which frames ----------
static _Atomic long g_pd_a0 = -1, g_pd_a1 = -1;   // passdump: frames [a0, a1]
static _Atomic long g_td_frame = -1;               // texdump: that frame
static char g_td_path[1024];
static uint64_t g_seen_frame = ~0ull; static int g_pass_no, g_snap_no; static uint64_t g_snap_bytes; static int g_snap_capped;
static void frame_note(void) {   // submit thread: the pass and snapshot numbers restart with each frame
    if (hvk.frame != g_seen_frame) { g_seen_frame = hvk.frame; g_pass_no = 0; g_snap_no = 0; g_snap_bytes = 0; g_snap_capped = 0; }
}
int hvk_dump_passes(void) { long f = (long)hvk.frame; return f >= atomic_load(&g_pd_a0) && f <= atomic_load(&g_pd_a1); }
int hvk_dump_snaps(void) { return (long)hvk.frame == atomic_load(&g_td_frame); }
void dm_gfx_passdump_next(long n);
void dm_gfx_passdump_next(long n) { long f = (long)hvk.frame; atomic_store(&g_pd_a1, f + (n > 0 ? n : 1)); atomic_store(&g_pd_a0, f + 1); }
// texdump <path> [draws <pass> [<slot>]]: with "draws", the pass of that number and the passes into the same targets after it (until SetRenderTargets names others) are split
// before every draw, so each snapshot follows one draw (only attachment slot <slot>: 0-7 colour, 8 depth; every slot without it), and each [texdump] line also gives what that
// draw changed against the previous snapshot of the same subresource. A path of "-" writes no PNG, only the lines
static _Atomic int g_td_split_pass = -1, g_td_split_slot = -1;
void dm_gfx_texdump_next(const char *arg);
void dm_gfx_texdump_next(const char *arg) {
    char path[1024]; int pass = -1, slot = -1;
    if (!arg || sscanf(arg, "%1023s draws %d %d", path, &pass, &slot) < 1) { hostvk_log("[RED] [hostvk] DM_CTL texdump: no path"); return; }
    if (strcmp(path, "-") && !strstr(path, "%")) { hostvk_log("[RED] [hostvk] DM_CTL texdump: the path needs a %%d for the snapshot number: %s", path); return; }
    snprintf(g_td_path, sizeof g_td_path, "%s", path);
    atomic_store(&g_td_split_pass, pass); atomic_store(&g_td_split_slot, slot);
    atomic_store(&g_td_frame, (long)hvk.frame + 1);
}
int hvk_dump_split_pass(void) { return hvk_dump_snaps() ? atomic_load(&g_td_split_pass) : -1; }
int hvk_dump_split_slot(void) { return atomic_load(&g_td_split_pass) >= 0 ? atomic_load(&g_td_split_slot) : -1; }
void hvk_dump_line(const char *fmt, ...) {
    if (!hvk_dump_passes()) return;
    char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    hostvk_log("[pass] F%llu %s", (unsigned long long)hvk.frame, b);
}
int hvk_dump_pass_no(void) { frame_note(); return g_pass_no; }
int hvk_dump_pass_next(void) { frame_note(); return ++g_pass_no; }

// ---------- Texels ----------
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = h >> 10 & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { float f = ldexpf((float)m, -24); return s ? -f : f; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13;
    else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static float uf(uint32_t v, int mbits) {   // an unsigned small float with 5 exponent bits (B10G11R11)
    uint32_t e = v >> mbits & 31, m = v & ((1u << mbits) - 1);
    if (!e) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}
// Bytes per texel of a format the dump reads (the depth aspect alone for a depth-stencil format), its channels, its kind (0 colour, 1 depth); 0 when it does not read it
static int fmt_info(VkFormat f, int *nch, int *depth) {
    *depth = 0;
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SNORM: *nch = 1; return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SNORM: *nch = 2; return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: *nch = 4; return 4;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: *nch = 3; return 4;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SNORM: *nch = 1; return 2;
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SNORM: *nch = 2; return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SNORM: *nch = 4; return 8;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: *nch = 1; return 4;
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: *nch = 2; return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: *nch = 4; return 16;
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: *nch = 1; *depth = 1; return 2;
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: *nch = 1; *depth = 1; return 4;
    default: *nch = 0; return 0;
    }
}
static void texel(VkFormat f, const uint8_t *p, float o[4]) {
    int nch_, depth_; uint32_t v = 0;
    if (fmt_info(f, &nch_, &depth_) >= 4) memcpy(&v, p, 4);   // the packed 32-bit formats read v
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    switch (f) {
    case VK_FORMAT_R8_UNORM: o[0] = p[0] / 255.f; break;
    case VK_FORMAT_R8_UINT: o[0] = p[0]; break;
    case VK_FORMAT_R8_SNORM: o[0] = fmaxf((int8_t)p[0] / 127.f, -1.f); break;
    case VK_FORMAT_R8G8_UNORM: o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; break;
    case VK_FORMAT_R8G8_UINT: o[0] = p[0]; o[1] = p[1]; break;
    case VK_FORMAT_R8G8_SNORM: o[0] = fmaxf((int8_t)p[0] / 127.f, -1.f); o[1] = fmaxf((int8_t)p[1] / 127.f, -1.f); break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        for (int i = 0; i < 4; i++) o[i] = p[i] / 255.f; break;
    case VK_FORMAT_R8G8B8A8_UINT: for (int i = 0; i < 4; i++) o[i] = p[i]; break;
    case VK_FORMAT_R8G8B8A8_SNORM: for (int i = 0; i < 4; i++) o[i] = fmaxf((int8_t)p[i] / 127.f, -1.f); break;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: o[0] = (v & 1023) / 1023.f; o[1] = (v >> 10 & 1023) / 1023.f; o[2] = (v >> 20 & 1023) / 1023.f; o[3] = (v >> 30) / 3.f; break;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: o[0] = (float)(v & 1023); o[1] = (float)(v >> 10 & 1023); o[2] = (float)(v >> 20 & 1023); o[3] = (float)(v >> 30); break;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: o[2] = (v & 1023) / 1023.f; o[1] = (v >> 10 & 1023) / 1023.f; o[0] = (v >> 20 & 1023) / 1023.f; o[3] = (v >> 30) / 3.f; break;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: o[0] = uf(v & 2047, 6); o[1] = uf(v >> 11 & 2047, 6); o[2] = uf(v >> 22 & 1023, 5); break;
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: { float s = ldexpf(1.f, (int)(v >> 27) - 15 - 9); o[0] = (float)(v & 511) * s; o[1] = (float)(v >> 9 & 511) * s; o[2] = (float)(v >> 18 & 511) * s; break; }
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: {
        int n = f == VK_FORMAT_R16_SFLOAT ? 1 : f == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
        for (int i = 0; i < n; i++) { uint16_t hh; memcpy(&hh, p + 2 * i, 2); o[i] = half_f(hh); } break; }
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: {
        int n = f == VK_FORMAT_R16_UNORM || f == VK_FORMAT_R16_UINT ? 1 : f == VK_FORMAT_R16G16_UNORM || f == VK_FORMAT_R16G16_UINT ? 2 : 4;
        int un = f == VK_FORMAT_R16_UNORM || f == VK_FORMAT_R16G16_UNORM || f == VK_FORMAT_R16G16B16A16_UNORM;
        for (int i = 0; i < n; i++) { uint16_t hh; memcpy(&hh, p + 2 * i, 2); o[i] = un ? hh / 65535.f : hh; } break; }
    case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16B16A16_SNORM: {
        int n = f == VK_FORMAT_R16_SNORM ? 1 : f == VK_FORMAT_R16G16_SNORM ? 2 : 4;
        for (int i = 0; i < n; i++) { int16_t hh; memcpy(&hh, p + 2 * i, 2); o[i] = fmaxf(hh / 32767.f, -1.f); } break; }
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: {
        int n = f == VK_FORMAT_R32G32_SFLOAT ? 2 : f == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 1;
        for (int i = 0; i < n; i++) memcpy(&o[i], p + 4 * i, 4); break; }
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: {
        int n = f == VK_FORMAT_R32G32_UINT ? 2 : f == VK_FORMAT_R32G32B32A32_UINT ? 4 : 1;
        for (int i = 0; i < n; i++) { uint32_t u; memcpy(&u, p + 4 * i, 4); o[i] = (float)u; } break; }
    case VK_FORMAT_R32_SINT: { int32_t s; memcpy(&s, p, 4); o[0] = (float)s; break; }
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: { uint16_t hh; memcpy(&hh, p, 2); o[0] = hh / 65535.f; break; }
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: o[0] = (v & 0xffffff) / 16777215.f; break;
    default: break;
    }
}
static int is_int_fmt(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32_SINT: return 1;
    default: return 0;
    }
}
// The statistics line's text and the PNG of w x h texels in format f (rows of row bytes)
static int write_image(const char *path, VkFormat f, const uint8_t *src, uint32_t w, uint32_t h, size_t row, char *stats, size_t ns) {
    int nch, depth, bpt = fmt_info(f, &nch, &depth);
    if (!bpt) { snprintf(stats, ns, "format not read by the dump"); return -1; }
    double mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY}, sum[4] = {0};
    uint64_t bad = 0, zero = 0, n = (uint64_t)w * h;
    int nc = nch < 3 ? nch : 3;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float o[4]; texel(f, src + y * row + (size_t)x * bpt, o);
        int z = 1;
        for (int c = 0; c < nch; c++) {
            if (!isfinite(o[c])) { bad++; continue; }
            if (o[c] < mn[c]) mn[c] = o[c];
            if (o[c] > mx[c]) mx[c] = o[c];
            sum[c] += o[c];
            if (c < nc && o[c] != 0) z = 0;
        }
        zero += (uint64_t)z;
    }
    size_t o = (size_t)snprintf(stats, ns, "%s", depth ? "depth" : "");
    static const char cn[4] = {'r', 'g', 'b', 'a'};
    for (int c = 0; c < nch && o < ns; c++) o += (size_t)snprintf(stats + o, ns - o, "%s%c [%.4g, %.4g] mean %.4g", c || depth ? "; " : "", depth ? 'z' : cn[c], mn[c], mx[c], sum[c] / (double)(n ? n : 1));
    if (o < ns) snprintf(stats + o, ns - o, "; non-finite %llu; colour all zero %.1f %%", (unsigned long long)bad, n ? 100.0 * (double)zero / (double)n : 0.0);
    if (!path) return 0;
    uint8_t *rgb = malloc((size_t)n * 3); if (!rgb) return -1;
    float lo = depth ? (float)mn[0] : 0.f, span = depth ? (float)(mx[0] - mn[0]) : 1.f;
    int integ = is_int_fmt(f);
    if (integ) { double m = 0; for (int c = 0; c < nc; c++) if (mx[c] > m) m = mx[c]; span = m > 0 ? (float)m : 1.f; }
    if (!(span > 0)) span = 1.f;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        float t[4]; texel(f, src + y * row + (size_t)x * bpt, t);
        uint8_t *d = rgb + ((size_t)y * w + x) * 3;
        for (int c = 0; c < 3; c++) {
            float v = depth || nch == 1 ? t[0] : c < nch ? t[c] : 0.f;
            if (depth || integ) v = (v - lo) / span;
            v = isfinite(v) ? (v < 0 ? 0 : v > 1 ? 1 : v) : (c == 0 ? 1.f : 0.f);   // a non-finite texel shows red
            d[c] = (uint8_t)(v * 255.f + 0.5f);
        }
    }
    int r = hvk_png_write(path, rgb, w, h);
    free(rgb);
    return r;
}

// ---------- Snapshots (texdump) ----------
enum { SNAP_CAP_BYTES = 1u << 30 };
typedef struct snap {
    struct snap *next; uint64_t fence, frame, serial; VkBuffer buf; hvk_mem *mem; void *map; VkFormat fmt; uint32_t w, h, level, layer; int no, pass, slot; char what[48];
} snap;
static pthread_mutex_t g_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static snap *g_snaps, **g_snap_tail = &g_snaps;
// Copy the subresource of view v (its level and first layer; the depth aspect of a depth-stencil view) into a readback buffer, outside any pass, ordered after what came before
void hvk_dump_snap(VkCommandBuffer cb, const hvk_view *v, int pass, int slot, const char *what) {
    frame_note();
    if (!hvk_dump_snaps() || !v || !v->t || !cb) return;
    int nch, depth; VkFormat f = v->format; int bpt;
    VkFormat ifmt; VkImageAspectFlags iasp; uint32_t iw, ih; VkSampleCountFlagBits ns;
    if (!hvk_tex_info(v->t, &ifmt, &iasp, &iw, &ih, NULL, NULL, NULL, &ns, NULL)) return;
    if (ns != VK_SAMPLE_COUNT_1_BIT) { hostvk_log("[texdump] F%llu pass %d slot %d ser=%llu: multisampled, not copied", (unsigned long long)hvk.frame, pass, slot, (unsigned long long)hvk_tex_desc(v->t)->serial); return; }
    VkImageAspectFlags asp = v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (asp == VK_IMAGE_ASPECT_STENCIL_BIT) { f = VK_FORMAT_R8_UINT; }
    else if (asp == VK_IMAGE_ASPECT_COLOR_BIT) f = ifmt;   // the image's own texels (a view in another format reinterprets the same bytes)
    bpt = fmt_info(f, &nch, &depth);
    if (!bpt) { hostvk_log("[texdump] F%llu pass %d slot %d ser=%llu: VkFormat %u is not read by the dump", (unsigned long long)hvk.frame, pass, slot, (unsigned long long)hvk_tex_desc(v->t)->serial, (unsigned)f); return; }
    VkDeviceSize n = (VkDeviceSize)v->w * v->h * (VkDeviceSize)bpt;
    if (g_snap_bytes + n > SNAP_CAP_BYTES) { if (!g_snap_capped) hostvk_log("[texdump] F%llu: past 1 GiB of snapshots: the rest of the frame is not copied", (unsigned long long)hvk.frame); g_snap_capped = 1; return; }
    snap *s = calloc(1, sizeof *s); if (!s) return;
    if (hvk_mem_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &s->buf, &s->mem, &s->map)) { free(s); hostvk_log("[RED] [hostvk] texdump: no readback buffer of %llu bytes", (unsigned long long)n); return; }
    g_snap_bytes += n;
    hvk_full_barrier(cb);
    VkBufferImageCopy r = {0, 0, 0, {asp, v->level, v->layer0, 1}, {0, 0, 0}, {v->w, v->h, 1}};
    vkCmdCopyImageToBuffer(cb, hvk_tex_image(v->t), VK_IMAGE_LAYOUT_GENERAL, s->buf, 1, &r);
    hvk_need_barrier();
    s->fence = hvk_open_fence(); s->frame = hvk.frame; s->serial = hvk_tex_desc(v->t)->serial; s->fmt = f; s->w = v->w; s->h = v->h; s->level = v->level; s->layer = v->layer0;
    s->no = g_snap_no++; s->pass = pass; s->slot = slot; snprintf(s->what, sizeof s->what, "%s", what ? what : "");
    pthread_mutex_lock(&g_snap_mu); *g_snap_tail = s; g_snap_tail = &s->next; pthread_mutex_unlock(&g_snap_mu);
}
// The storage images a dispatch writes (noted while its descriptors are built), snapshotted after it
static hvk_view g_wr[8]; static int g_nwr;
void hvk_dump_note_reset(void) { g_nwr = 0; }
void hvk_dump_note_write(const hvk_view *v) {
    if (!hvk_dump_snaps() || !v || !v->t) return;
    for (int i = 0; i < g_nwr; i++) if (g_wr[i].t == v->t && g_wr[i].level == v->level && g_wr[i].layer0 == v->layer0) return;
    if (g_nwr < 8) g_wr[g_nwr++] = *v;
}
void hvk_dump_storage_writes(VkCommandBuffer cb, int pass) {
    for (int i = 0; i < g_nwr; i++) hvk_dump_snap(cb, &g_wr[i], pass, 10 + i, "storage");
    g_nwr = 0;
}
// What a snapshot changed against the previous snapshot of the same subresource in the frame (the per-draw split): the texels whose bytes differ, their bounding box and the
// mean of their new first channel (depth for a depth snapshot)
typedef struct { uint64_t frame, serial; uint32_t level, layer, w, h; VkFormat fmt; uint8_t *bytes; } prev_snap;
static prev_snap g_prev[4]; static int g_prev_next;
static void snap_diff(const snap *s, size_t bpt, char *out, size_t n) {
    prev_snap *p = NULL; size_t sz = (size_t)s->w * s->h * bpt;
    for (int i = 0; i < 4; i++) if (g_prev[i].bytes && g_prev[i].frame == s->frame && g_prev[i].serial == s->serial && g_prev[i].level == s->level && g_prev[i].layer == s->layer &&
                                    g_prev[i].fmt == s->fmt && g_prev[i].w == s->w && g_prev[i].h == s->h) p = &g_prev[i];
    if (p) {
        uint64_t ch = 0; uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0; double sum = 0;
        for (uint32_t y = 0; y < s->h; y++) for (uint32_t x = 0; x < s->w; x++) {
            size_t o = ((size_t)y * s->w + x) * bpt;
            if (!memcmp((const uint8_t *)s->map + o, p->bytes + o, bpt)) continue;
            ch++; if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y;
            float t[4]; texel(s->fmt, (const uint8_t *)s->map + o, t); sum += isfinite(t[0]) ? t[0] : 0;
        }
        if (ch) snprintf(out, n, "; changed %llu texels (%.1f %%) in %u,%u..%u,%u, their new first channel mean %.4g", (unsigned long long)ch, 100.0 * (double)ch / ((double)s->w * s->h), x0, y0, x1, y1, sum / (double)ch);
        else snprintf(out, n, "; changed nothing");
    } else { out[0] = 0; p = &g_prev[g_prev_next++ & 3]; free(p->bytes); p->bytes = NULL; }
    uint8_t *b = realloc(p->bytes, sz);
    if (b) { memcpy(b, s->map, sz); *p = (prev_snap){s->frame, s->serial, s->level, s->layer, s->w, s->h, s->fmt, b}; }
}
// Completion thread: the snapshots whose submission completed are converted and written
void hvk_dump_collect(uint64_t done) {
    for (;;) {
        pthread_mutex_lock(&g_snap_mu);
        snap *s = g_snaps;
        if (s && s->fence <= done) { g_snaps = s->next; if (!g_snaps) g_snap_tail = &g_snaps; } else s = NULL;
        pthread_mutex_unlock(&g_snap_mu);
        if (!s) return;
        char path[1100], stats[400], diff[200] = "";
        int nopng = !strcmp(g_td_path, "-");
        if (!nopng) snprintf(path, sizeof path, g_td_path, s->no);
        int nch, depth, bpt = fmt_info(s->fmt, &nch, &depth);
        int r = write_image(nopng ? NULL : path, s->fmt, s->map, s->w, s->h, (size_t)s->w * (size_t)bpt, stats, sizeof stats);
        if (atomic_load(&g_td_split_pass) >= 0) snap_diff(s, (size_t)bpt, diff, sizeof diff);
        hostvk_log("[texdump] F%llu #%d pass %d %s slot %d ser=%llu VkFormat %u %ux%u level %u layer %u: %s%s%s%s", (unsigned long long)s->frame, s->no, s->pass, s->what, s->slot,
                   (unsigned long long)s->serial, (unsigned)s->fmt, s->w, s->h, s->level, s->layer, stats, diff, nopng ? "" : r ? " (not written)" : " -> ", nopng || r ? "" : path);
        hvk_mem_buffer_free(s->buf, s->mem);
        free(s);
    }
}

// ---------- texraw, texlist ----------
void dm_gfx_texraw(uint64_t serial, const char *path);
void dm_gfx_texraw(uint64_t serial, const char *path) {
    if (!hvk_up()) return;
    hvk_tex *x = hvk_tex_by_serial(serial);
    VkFormat f; VkImageAspectFlags asp; uint32_t w, h; VkSampleCountFlagBits ns;
    if (!x || !hvk_tex_info(x, &f, &asp, &w, &h, NULL, NULL, NULL, &ns, NULL)) { hostvk_log("[RED] [hostvk] DM_CTL texraw: no texture with serial %llu", (unsigned long long)serial); return; }
    int nch, depth, bpt = fmt_info(f, &nch, &depth);
    // BC formats: their blocks, raw (8 bytes per 4x4 block for BC1 and BC4, 16 for the others), with no PNG
    int bc = f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK ? (f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ? 8 : 16) : 0;
    if ((!bpt && !bc) || ns != VK_SAMPLE_COUNT_1_BIT) { hostvk_log("[RED] [hostvk] DM_CTL texraw: texture ser=%llu in VkFormat %u (%u samples) is not read by the dump", (unsigned long long)serial, (unsigned)f, (unsigned)ns); return; }
    uint32_t row_bytes = bc ? (w + 3) / 4 * (uint32_t)bc : w * (uint32_t)bpt, rows = bc ? (h + 3) / 4 : h;
    VkDeviceSize n = (VkDeviceSize)row_bytes * rows;
    VkBuffer buf; hvk_mem *mem; void *map;
    if (hvk_mem_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HVK_MEM_HOST, &buf, &mem, &map)) { hostvk_log("[RED] [hostvk] DM_CTL texraw: no readback buffer"); return; }
    hvk_finish();   // what the submissions so far wrote
    VkCommandBuffer cb;
    if (!hvk_oneoff_begin(&cb)) { hvk_mem_buffer_free(buf, mem); return; }
    hvk_full_barrier(cb);
    VkImageAspectFlags a = asp & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkBufferImageCopy r = {0, 0, 0, {a, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, hvk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    if (!hvk_oneoff_end(cb)) { hvk_mem_buffer_free(buf, mem); hostvk_log("[RED] [hostvk] DM_CTL texraw: the copy failed"); return; }
    FILE *fo = fopen(path, "wb");
    if (fo) { fprintf(fo, "%u %u %u %u\n", w, h, (unsigned)f, row_bytes); fwrite(map, 1, (size_t)n, fo); fclose(fo); }
    if (bc) {
        size_t nz = 0; char hex[100] = ""; for (size_t i = 0; i < (size_t)n; i++) nz += ((const uint8_t *)map)[i] != 0;
        for (size_t i = 0; i < (size_t)n && i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", ((const uint8_t *)map)[i]);
        hostvk_log("[hostvk] DM_CTL texraw: ser=%llu VkFormat %u %ux%u (level 0 of %u): %llu bytes of blocks, %zu not zero, first %s -> %s", (unsigned long long)serial, (unsigned)f, w, h,
                   hvk_tex_desc(x)->d.levels, (unsigned long long)n, nz, hex, fo ? path : "(not written)");
        hvk_mem_buffer_free(buf, mem); return;
    }
    char png[1100], stats[400]; snprintf(png, sizeof png, "%s.png", path);
    int rr = write_image(png, f, map, w, h, (size_t)w * (size_t)bpt, stats, sizeof stats);
    hostvk_log("[hostvk] DM_CTL texraw: ser=%llu VkFormat %u %ux%u: %s -> %s%s%s", (unsigned long long)serial, (unsigned)f, w, h, stats, fo ? path : "(not written)", rr ? "" : ", ", rr ? "" : png);
    hvk_mem_buffer_free(buf, mem);
}
void dm_gfx_texlist(int target);
void dm_gfx_texlist(int target) { if (hvk_up()) hvk_tex_list(target); }
