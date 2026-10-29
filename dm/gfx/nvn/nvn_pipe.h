// Neutral render-pipeline description of the NVN replay (pure C11, every host; docs/design/vulkan-backend.md, sections 1.3, 1.5, 3.1 and 3.3, step M5): a pipeline in NVN terms
// (NVN formats, NVN state), from which the Metal backend's key (pso_key / pso_skey, nvn_pso_skey.h, whose formats are MTLPixelFormat numbers) is one projection and the Vulkan
// backend's pipeline another. It also reads and writes the prewarm lists' pipeline rows: today's R rows (the hex of pso_skey: Metal formats, decoded through nvn_fmt_from_mtl) and
// the portable P rows below (NVN formats), which the Vulkan backend's per-device list will hold. S and C rows (shaders, compute pipelines) are not its business.
//
// P row, version 1: one line, fields separated by one TAB, in this order (names are fixed; numbers are decimal, or hexadecimal with 0x where shown so):
//   P   v1   vs=<key>@<stage>   fs=<key>@<stage>   c=<f0>,<f1>,...,<f7>   ds=<depth>,<stencil>   guess=<mask>   ms=<samples>,<a2c>   blend_en=<mask>   chmask=<mask>
//   blend=<8 entries>   attr=<16 entries>   stream=<16 entries>   rast=<raster state>   dss=<depth-stencil state>
// - <key>: the shader's content key as the shader map names it (printable ASCII without space, ',', '@' or '=', up to 24 characters), or '-' for no shader; <stage>: the stage
//   number pso_skey holds (NvShader.stage).
// - c=, ds=: NVN format numbers (NVNformat) in hexadecimal (0x25), 0 for no attachment. ds= gives the depth and the stencil attachment as the pass attaches them: a combined format
//   appears in both, or in one when the pass attaches only that plane.
// - guess=: hexadecimal mask, bit i for c's slot i, bit 8 for depth, bit 9 for stencil: that format was decoded from a Metal format several NVN formats produce (an R row), so it
//   is nvn_fmt_from_mtl's first candidate, not necessarily the game's format.
// - blend_en=, chmask=: hexadecimal masks (color_st.blend_en, chmask_st.mask: 4 bits per target, r=1 g=2 b=4 a=8).
// - blend=: per colour target, comma-separated, '-' for an all-zero state, else target.src_rgb.dst_rgb.src_a.dst_a.eq_rgb.eq_a.adv.adv_npm.adv_ovl (blend_st's NVN enum values).
// - attr=: per vertex attribute, '-' or fmt.offset.stream (va_st); stream=: per vertex stream, '-' or stride.divisor (vs_st).
// - rast=: '-' when the row does not carry the state beyond the Metal key (an R row decoded), else cull.front.poly_mode.bias_en.depth_clamp.rast_discard.restart.logic_op.alpha_func.
// - dss=: '-' as rast=, else dt.dw.df.st.sf_f.sf_b.fail_f.zfail_f.zpass_f.fail_b.zfail_b.zpass_b (ds_st's fields).
// A later version adds fields at the end and bumps v<N>; a reader refuses a version it does not know. nvn_pipe_parse_row(nvn_pipe_write_row(d)) gives d back byte for byte.
//
// Objective-C includers (the Metal replay) include this after declaring their NvShader class: nvn_pso_skey.h's helpers read its key and stage. In C the small struct below stands in.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"
#include "nvn_state.h"
#include "../tex/nvn_fmt_mtl.h"
#ifndef __OBJC__
typedef struct NvShader { int stage; char key[24]; } NvShader;   // what pso_skey_from reads of a shader
#endif
#include "nvn_pso_skey.h"

#define NVN_PIPE_ROW_VERSION 1
#define NVN_PIPE_ROW_MAX 1024   // a P row's longest form, with its NUL, fits in this many bytes

typedef struct {
    char key[24];    // content key, NUL-padded, as pso_skey holds it; all zero = no shader
    uint8_t stage;   // as pso_skey holds it (NvShader.stage)
} nvn_pipe_stage;

// Every byte is a field (no padding), so descriptions compare with memcmp; a producer starts from a zeroed one (all functions below do)
typedef struct {
    // ---- What today's Metal key (pso_skey) holds, in NVN terms ----
    nvn_pipe_stage vs, fs;
    uint16_t cfmt[8];        // NVN formats of the colour attachments, 0 = none
    uint16_t dfmt, sfmt;     // NVN formats of the depth and the stencil attachment as the pass attaches them, 0 = not attached
    uint16_t fmt_guess;      // bit i: cfmt[i], bit 8: dfmt, bit 9: sfmt was decoded from a Metal format with several NVN sources (see the P row's guess=)
    uint8_t samples;         // the attachments' sample count as the pass reports it (0 for a pass without attachments)
    uint8_t blend_en;        // color_st.blend_en: one bit per colour target
    uint8_t a2c;             // ms_st.a2c: alpha to coverage
    uint8_t xstate;          // 1: the fields under "Beyond the key" were filled from S; 0: they are unknown (decoded from an R row) and all zero
    uint32_t chmask;         // chmask_st.mask: 4 bits per colour target
    blend_st blend[8];       // per colour target; zero where blending is off or the target is absent
    va_st attr[16];          // the attributes the vertex shader reads (S.attr), the others zero
    vs_st stream[16];        // the streams those attributes read (S.stream), the others zero
    // ---- Beyond the key: the rest of the pipeline state S holds (design 3.1) ----
    ds_st ds;                // depth-stencil state (its fields only; the unused bits zero)
    uint8_t cull, front, poly_mode, bias_en;   // poly_st: cull face, front face (winding as the game set it, before the queue's origin), polygon mode, depth-bias enable bits
    uint8_t depth_clamp, rast_discard, restart;   // S.depth_clamp, S.rast_discard, S.restart.on, as 0 / 1
    uint8_t logic_op, alpha_func;                 // color_st: logic operation and alpha-test function (NVN enum values)
    uint8_t rsv[7];          // zero
    // TODO, state the recorder does not carry yet (hle_nvn.c), with the field it would get in S (nvn_state.h) or where it lives today:
    // - topology class: per draw, the record's primitive (p_draw.prim / p_draw_idx.prim), not in S; the Vulkan backend passes it in when it fills a description
    // - the queue's origin for the winding: queue_t.origin_ll, given to the backend by be_set_queue_origin_lower_left, not in S
    // - depth mode (near 0 or -w): dev_t_.depth_mode, given by be_set_depth_zero, not in S
    // - conservative rasterization: nvnCommandBufferSetConservativeRasterEnable is only logged (hle_nvn.c); S.conservative
    // - line mode and width: nvnCommandBufferSetLineWidth is only logged; S.line_width (and a smooth / stipple mode if the polygon state gains one)
    // - alpha-test reference: nvnCommandBufferSetAlphaRef is only logged; S.alpha_ref (alpha_func above is recorded)
    // - patch size: nvnCommandBufferSetPatchSize is a no-op; S.patch_size
    // - the geometry and tessellation stages: S.bst[2..4] hold their programs, but pso_skey names only the vertex and fragment shaders; nvn_pipe_stage gs, tcs, tes
    // - multisample state beyond the sample count and alpha to coverage (S.ms: raster samples, coverage modulation, coverage to colour, sample locations)
} nvn_pipe_desc;
_Static_assert(sizeof(nvn_pipe_desc) == 2 * 25 + 16 + 6 + 4 + 4 + 64 + 64 + 128 + 8 + 9 + 7, "nvn_pipe_desc has padding");

// Today's key → neutral: 0, or -1 when the key is not representable, with the reason in why (n bytes; may be NULL): "merged pass", "readable-tile variant",
// "Metal format %u has no NVN source", "live pointers in the key", "shader key is not text", "field out of range", "bits outside the NVN fields".
// k is a key as pso_skey_parse leaves it (normalised)
int nvn_pipe_from_key(const pso_skey *k, nvn_pipe_desc *out, char *why, size_t n);
// Neutral → today's key, through nvn_fmt_to_mtl, normalised as pso_skey_parse does (a normal pass: not merged, no readable-tile function): 0, or -1 when an attachment's NVN format
// has no Metal format (the Metal backend could not attach it)
int nvn_pipe_to_key(const nvn_pipe_desc *d, pso_skey *out);
// One prewarm row: 0 with *out filled (an R or a P row); 1 for a row that is not a render pipeline (S, C, a comment, a blank line); -1 for a malformed row; -2 for an R row that is
// valid but not representable (nvn_pipe_from_key). why (may be NULL) says what was wrong. A trailing CR / LF is ignored
int nvn_pipe_parse_row(const char *line, nvn_pipe_desc *out, char *why, size_t n);
// A P row (no line end): its length, or -1 when it does not fit in n bytes or a shader key cannot be written (see the format above)
int nvn_pipe_write_row(const nvn_pipe_desc *d, char *out, size_t n);
// From the draw state and the pass, as the Metal backend's pso_key_fill fills its key (nvn_mtl.m) but with NVN formats, and with the state beyond the key (xstate 1):
// vs_key / fs_key: the shaders' 24-byte keys (fs_key NULL for none); vs_inputs: bit a for each NVN attribute location the vertex shader reads (its reflection's nvn_refl.v[].nvn;
// locations of 16 and up are ignored, as the key has 16); cfmt / dfmt / sfmt: the NVN formats the pass attaches (a view's format where the view sets one), 0 for none.
// The result through nvn_pipe_to_key is the Metal key wherever the Metal backend attaches what the NVN formats map to (not where it attaches a shadow or keeps a base texture's
// format, nvn_mtl.m rt_view_fmt). Returns 0, or -1 for a sample count above 255
int nvn_pipe_from_state(nvn_pipe_desc *out, const nvn_draw_state *s, const char vs_key[24], int vs_stage, uint32_t vs_inputs, const char *fs_key, int fs_stage,
                        const uint16_t cfmt[8], uint16_t dfmt, uint16_t sfmt, uint32_t samples);
