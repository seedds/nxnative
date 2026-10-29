// The plain C reflection parser (nvn_refl.c, nvn_refl_from_json_text) on every host: the samples and hand-computed expectations of t_refl.m, every failure path with its text, the
// array<…> forms, the storage buffer write flag, the c1 segment, the limits, every texture type prefix and pixel class. t_refl_cmp.m (Mac) includes this file for its corpus
// (T_REFL_CMP: the case table and the generated documents, without main) and checks that the Objective-C parser makes the same structs of every document.
// The real samples of t_refl.m (Super Mario Bros. Wonder's translated shaders in the NSP script's build cache) are read from DM_ROOT (or ~/dm-work) when present, else skipped.
#include "nvn_refl.h"
#include "nvn_refl_textype.h"
#include "dmsh_chain.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define J(...) #__VA_ARGS__   // a json document written as tokens (true / false / null are not expanded under #)

static int n_checks, n_failed;
static const char *cur = "";
static void check(const char *what, long want, long got) {
    n_checks++;
    if (want != got) { n_failed++; printf("  %s: %-48s expected %ld, got %ld FAILED\n", cur, what, want, got); }
}
#define CK(what, want, got) check(what, (long)(want), (long)(got))

enum { F = NVN_DPF_FLOAT, U = NVN_DPF_UINT, S = NVN_DPF_SINT, D = NVN_DPF_DEPTH, X = -1 };
typedef struct { int i, kind, reg, aid, rw, dtt, dpf, n, off; } want_bind;   // X = not checked
static void check_binds(const nvn_refl *r, const want_bind *w, int count) {
    for (int k = 0; k < count; k++) {
        const nvn_rbind *g = &r->b[w[k].i]; char m[64];
#define WB(field, label) if (w[k].field != X) { snprintf(m, sizeof m, "binding[%d] %s", w[k].i, label); CK(m, w[k].field, g->field); }
        WB(kind, "kind") WB(reg, "slot") WB(aid, "argument index") WB(rw, "read-write") WB(dtt, "dummy texture type") WB(dpf, "pixel class") WB(n, "array length") WB(off, "offset")
#undef WB
    }
}
#define NW(w) (int)(sizeof w / sizeof *w)

// ---- the cases: a document, whether it fails, its error text, the checks of its struct ----
static void v_atomic(const nvn_refl *r) {   // native image atomics (dmsh_atomic.py): one read_write image, marked atomic by the build; an image the shader only stores to is not
    CK("IMG kind", NVN_RB_IMG, r->b[0].kind); CK("texture type buffer", NVN_MTL_TEX_BUFFER, r->b[0].dtt); CK("unsigned format", U, r->b[0].dpf);
    CK("written", 1, r->b[0].rw); CK("atomic flag", 1, r->b[0].atom); CK("stored image: 3D texture type", NVN_MTL_TEX_3D, r->b[1].dtt); CK("stored image: no atomic flag", 0, r->b[1].atom);
    CK("stored image: written (access::write)", 1, r->b[1].rw); CK("binding count", 2, r->nb); CK("B flag", 1, r->route_b);
}
static void v_handle_array(const nvn_refl *r) {   // Paper Mario: The Thousand-Year Door b352f7af1c86e9fa_b, its last four bindings (t_refl.m)
    CK("single texture n", 0, r->b[0].n); CK("single texture offset", 16, r->b[0].off); CK("single sampler n", 0, r->b[1].n);
    CK("texture kind", NVN_RB_TEXU, r->b[2].kind); CK("texture n", 32, r->b[2].n); CK("texture dummy type 2D", 2, r->b[2].dtt); CK("texture slot", 11, r->b[2].reg);
    CK("sampler kind", NVN_RB_SAMPU, r->b[3].kind); CK("sampler n", 32, r->b[3].n); CK("bindless", 1, r->bindless); CK("argument index of the sampler array", 48, r->b[3].aid);
}
static void v_every_kind(const nvn_refl *r) {
    static const want_bind w[] = {
        {0, NVN_RB_SUPPORT, 0, 0, 0, 0, 0, 0, 0}, {1, NVN_RB_CBUF, 3, 1, 0, 0, 0, 0, 0}, {2, NVN_RB_C1, 0, 2, X, X, X, X, X}, {3, NVN_RB_DRV0, 0, 3, X, X, X, X, X},
        {4, NVN_RB_DRV2, 0, 4, X, X, X, X, X}, {5, NVN_RB_SAMP, 7, 5, X, X, X, X, X}, {6, NVN_RB_SAMPT, 4, 6, X, X, X, X, X}, {7, NVN_RB_TEX, 41, 7, 0, 2, F, 0, 0},
        {8, NVN_RB_SAMPI, 12, 8, X, X, X, X, X}, {9, NVN_RB_SBUFP, 4, 9, 1, X, X, X, X}, {10, NVN_RB_SBUFP, 5, 10, 0, X, X, X, X}, {11, NVN_RB_SBUFP, 6, 11, 0, X, X, X, X},
        {12, NVN_RB_SBUFP, 7, 12, 1, X, X, X, X}, {13, NVN_RB_UBOP, 2, 13, 0, X, X, X, X}, {14, NVN_RB_SBUFU, 0, 14, 1, X, X, X, 56}, {15, NVN_RB_GEOMETRY, 2, 15, X, X, X, X, X},
        {16, NVN_RB_TBUF, 8, 16, 0, 0, U, X, X}, {17, NVN_RB_IMG, 1, 17, 0, 2, F, X, X}, {18, NVN_RB_TEXA, 0, 18, X, 3, F, 4, 128}, {19, NVN_RB_SAMPA, 0, 22, X, 0, X, 4, 128},
        {20, NVN_RB_SAMPD, 0, 26, X, 0, X, 11, X}, {21, NVN_RB_TEXD, 0, 37, X, 2, F, 11, X}, {22, NVN_RB_TEXD, 0, 48, X, NVN_MTL_TEX_BUFFER, S, 2, X}, {23, NVN_RB_TEXD, 0, 50, X, 2, D, 3, X}};
    check_binds(r, w, NW(w));
    CK("binding count", 24, r->nb); CK("fetch1d flag of the texture", 1, r->b[7].f1d); CK("dynamic handle arrays", 1, r->dyn); CK("no uniform-buffer handles", 0, r->bindless);
    CK("reads c0 (declared binding)", 1, r->drv_read[0]); CK("reads c2 (declared binding)", 1, r->drv_read[2]); CK("entry after the last binding zero", 0, r->b[24].kind);
}
static void v_tex_types(const nvn_refl *r) {   // every prefix rule of dummy_kind and every pixel class
    static const struct { int dtt, dpf; } w[] = {
        {NVN_MTL_TEX_1D, F}, {NVN_MTL_TEX_1D_ARRAY, F}, {NVN_MTL_TEX_2D, F}, {NVN_MTL_TEX_2D_ARRAY, F}, {NVN_MTL_TEX_CUBE, F}, {NVN_MTL_TEX_CUBE_ARRAY, F}, {NVN_MTL_TEX_3D, F},
        {NVN_MTL_TEX_BUFFER, F}, {NVN_MTL_TEX_2D, D}, {NVN_MTL_TEX_2D_ARRAY, D}, {NVN_MTL_TEX_CUBE, D}, {NVN_MTL_TEX_CUBE_ARRAY, D}, {NVN_MTL_TEX_2D, F}, {NVN_MTL_TEX_2D, U},
        {NVN_MTL_TEX_2D_ARRAY, S}, {NVN_MTL_TEX_3D, F}, {NVN_MTL_TEX_2D, F}, {NVN_MTL_TEX_1D_ARRAY, U}};
    for (int i = 0; i < NW(w); i++) { char m[48]; snprintf(m, sizeof m, "binding[%d] dummy texture type", i); CK(m, w[i].dtt, r->b[i].dtt);
        snprintf(m, sizeof m, "binding[%d] pixel class", i); CK(m, w[i].dpf, r->b[i].dpf); }
    CK("binding count", 18, r->nb); CK("image written (access::write)", 1, r->b[17].rw);
}
static void v_arrays(const nvn_refl *r) {
    static const want_bind w[] = {
        {0, NVN_RB_TEXU, 1, 0, X, 2, F, 0, 8}, {1, NVN_RB_TEXU, 1, 1, X, 3, U, 8, 0}, {2, NVN_RB_SAMPU, 1, 9, X, X, X, 32, 4}, {3, NVN_RB_TEXU, 1, 41, X, 2, F, 0, X},
        {4, NVN_RB_TEXU, 2, 42, X, 5, S, 1, X}, {5, NVN_RB_TEXU, 2, 43, X, X, X, 255, X}, {6, NVN_RB_SAMPU, 2, 44, X, X, X, 0, X}, {7, NVN_RB_TEXU, 2, 45, X, 2, F, 0, X}};
    check_binds(r, w, NW(w));   // [3] "array<…,32>" has no ", ": not an array; [5] a negative count passes the n > 32 test (as nvn_refl.m: n = (uint8_t)-1); [7] no closing ">" last
}
static void v_rw(const nvn_refl *r) {
    static const int w[] = {1, 0, 0, 1, 1, 0, 1, 0, 0, 1};
    for (int i = 0; i < NW(w); i++) { char m[40]; snprintf(m, sizeof m, "binding[%d] read-write", i); CK(m, w[i], r->b[i].rw); }
}
static void v_c1(const nvn_refl *r) {
    CK("has c1", 1, r->has_c1); CK("c1 length", 4, r->c1_len); CK("c1 allocated", 1, r->c1 != NULL);
    if (r->c1) { CK("c1[0]", 0x00, r->c1[0]); CK("c1[1]", 0x01, r->c1[1]); CK("c1[2]", 0xff, r->c1[2]); CK("c1[3] (upper case hex)", 0x7f, r->c1[3]); }
}
static void v_c1_odd_digits(const nvn_refl *r) {   // strtoul of each two-character substring: no digit → 0; "-1" → ULONG_MAX → 0xff; " f" → 15; "1z" → 1
    static const int w[] = {0, 0x10, 0xff, 15, 1};
    CK("c1 length", 5, r->c1_len);
    for (int i = 0; r->c1 && i < NW(w); i++) { char m[24]; snprintf(m, sizeof m, "c1[%d]", i); CK(m, w[i], r->c1[i]); }
}
static void v_no_c1(const nvn_refl *r) { CK("no c1", 0, r->has_c1); CK("c1 length 0", 0, r->c1_len); CK("c1 NULL", 1, r->c1 == NULL); }
static void v_defaults(const nvn_refl *r) {   // a document with nothing but its bindings: every default nvn_refl.m sets
    CK("B flag", 1, r->route_b); CK("argument buffer slot", 0, r->abi); CK("no bindings", 0, r->nb); CK("no inputs", 0, r->nv); CK("no c1", 1, r->c1 == NULL && !r->has_c1);
    CK("pos_attr unknown", -1, r->pos_attr); CK("may discard", 1, r->may_discard); CK("may write sample_mask", 1, r->writes_smask); CK("colors unknown", 0, r->color_known);
    CK("color mask", 0, r->color_mask); CK("pos_w1", 0, r->pos_w1); CK("uv_attr[0]", -1, r->uv_attr[0]); CK("uv_attr[31]", -1, r->uv_attr[31]); CK("uv_aff_attr[17]", -1, r->uv_aff_attr[17]);
    CK("thread group", 0, r->tg[0] | r->tg[1] | r->tg[2]); CK("no shadow library", 0, r->has_vin); CK("no driver reads", 0, r->drv_read[0] | r->drv_read[2]); CK("no fetch calls", 0, r->nfc);
    CK("input shadows untouched (memset 0)", 0, r->v[0].sh[0]);
}
static void v_numbers(const nvn_refl *r) {   // NSNumber / NSString answers: 2.0 → 2, true → 1, "7" → 7, 1e1 → 10, false → 0, "24" → 24, "2" → bank 2
    static const want_bind w[] = {
        {0, NVN_RB_CBUF, 2, 7, X, X, X, X, X}, {1, NVN_RB_CBUF, 1, 10, X, X, X, X, X}, {2, NVN_RB_TEXU, 0, 3, X, X, X, X, 24}, {3, NVN_RB_CBUF, 0, 4, X, X, X, X, X},
        {4, NVN_RB_DRV2, 0, 5, X, X, X, X, X}, {5, NVN_RB_CBUF, 0, 3, X, X, X, X, X}, {6, NVN_RB_CBUF, 9, 2, X, X, X, X, X}};
    check_binds(r, w, NW(w));   // [5] arg_id 65539 → uint16 3; [6] reg 9.75 → 9, arg_id -65534 → uint16 2
}
static void v_vin(const nvn_refl *r) {
    static const struct { int loc, ct, sf, su, si; } w[] = {
        {0, NVN_CT_FLOAT, 0xff, 0xff, 0xff}, {1, NVN_CT_UINT, 16, 0xff, 17}, {2, NVN_CT_INT, 18, 19, 0xff}, {3, NVN_CT_FLOAT, 0xff, 0xff, 0xff}, {4, NVN_CT_UINT, 0xff, 0xff, 30}};
    CK("input count", 5, r->nv); CK("has the shadow library", 1, r->has_vin);
    for (int i = 0; i < NW(w); i++) { char m[48];
        snprintf(m, sizeof m, "input[%d] location", i); CK(m, w[i].loc, r->v[i].loc); snprintf(m, sizeof m, "input[%d] NVN attribute", i); CK(m, w[i].loc, r->v[i].nvn);
        snprintf(m, sizeof m, "input[%d] component type", i); CK(m, w[i].ct, r->v[i].ct);
        snprintf(m, sizeof m, "input[%d] float shadow", i); CK(m, w[i].sf, r->v[i].sh[NVN_CT_FLOAT]); snprintf(m, sizeof m, "input[%d] uint shadow", i); CK(m, w[i].su, r->v[i].sh[NVN_CT_UINT]);
        snprintf(m, sizeof m, "input[%d] sint shadow", i); CK(m, w[i].si, r->v[i].sh[NVN_CT_INT]); }
}
static void v_vin_empty(const nvn_refl *r) { CK("no shadow attributes → no shadow library", 0, r->has_vin); CK("input count", 1, r->nv); CK("shadow none", 0xff, r->v[0].sh[NVN_CT_FLOAT]); }
static void v_raster_ps(const nvn_refl *r) {
    CK("may discard", 0, r->may_discard); CK("writes sample_mask", 0, r->writes_smask); CK("color mask {0, 2}", 5, r->color_mask); CK("colors known", 1, r->color_known); CK("pos_attr", -1, r->pos_attr);
}
static void v_raster_vs(const nvn_refl *r) {
    CK("pos_attr", 3, r->pos_attr); CK("w is one", 1, r->pos_w1); CK("discard default", 1, r->may_discard); CK("colors unknown", 0, r->color_known);
    CK("uv_attr[0]", 1, r->uv_attr[0]); CK("uv_attr[5]", 3, r->uv_attr[5]); CK("uv_attr[1] (attribute 20 out of range)", -1, r->uv_attr[1]);
    CK("uv_aff_attr[2]", 1, r->uv_aff_attr[2]); CK("uv_aff[2] sx", 3 << 8 | 4 << 2 | 1, r->uv_aff[2][0]); CK("uv_aff[2] ox", 3 << 8 | 4 << 2 | 3, r->uv_aff[2][1]);
    CK("uv_aff[2] sy", 3 << 8 | 5 << 2 | 1, r->uv_aff[2][2]); CK("uv_aff[2] oy", 3 << 8 | 5 << 2 | 3, r->uv_aff[2][3]);
    CK("uv_aff_attr[3] (slot 16 refused)", -1, r->uv_aff_attr[3]); CK("uv_aff[3] sx written before the refusal", 785, r->uv_aff[3][0]); CK("uv_aff[3] ox", 0, r->uv_aff[3][1]);
    CK("uv_aff_attr[4] (attribute 20)", -1, r->uv_aff_attr[4]); CK("uv_aff_attr[6] (two-element reference)", -1, r->uv_aff_attr[6]); CK("uv_aff[6] sx", 0, r->uv_aff[6][0]);
}
static void v_raster_odd(const nvn_refl *r) {   // pos_attr 16 → -1, pos_w other than "one", a dictionary of colors (its keys count, colors not known), discard 2, sample_mask "0"
    CK("pos_attr out of range", -1, r->pos_attr); CK("w not one", 0, r->pos_w1); CK("color mask from the keys {1, 3}", 10, r->color_mask); CK("colors not an array", 0, r->color_known);
    CK("discard 2", 1, r->may_discard); CK("sample_mask \"0\"", 0, r->writes_smask);
}
static void v_raster_str(const nvn_refl *r) { CK("pos_attr \"2\"", 2, r->pos_attr); CK("colors [\"4\"]", 16, r->color_mask); CK("known", 1, r->color_known); }
static void v_fetch(const nvn_refl *r) {
    CK("fetch calls", 2, r->nfc); CK("call 0 slot", 5, r->fc_reg[0]); CK("call 0 output", 2, r->fc_locn[0]); CK("call 1 slot", 200, r->fc_reg[1]); CK("call 1 output", 31, r->fc_locn[1]);
    CK("mixed slot 3", 8, (long)r->fc_mixed[0]); CK("mixed slot 64", 1, (long)r->fc_mixed[1]); CK("mixed slot 255", 1, r->fc_mixed[3] == 1ull << 63); CK("mixed 2", 0, (long)r->fc_mixed[2]);
    CK("outputs", 2, r->nfo); CK("output 0 color", 0, r->fo_color[0]); CK("output 1 color", 2, r->fo_color[1]);
}
static void v_tg(const nvn_refl *r) { CK("tg x", 32, r->tg[0]); CK("tg y", 8, r->tg[1]); CK("tg z", 1, r->tg[2]); }
static void v_tg_none(const nvn_refl *r) { CK("tg of two elements ignored", 0, r->tg[0]); }
static void v_tg_wrap(const nvn_refl *r) { CK("tg 70000 → uint16", 70000 & 0xffff, r->tg[0]); }
static void v_drv0(const nvn_refl *r) { CK("reads c0", 1, r->drv_read[0]); CK("not c2", 0, r->drv_read[2]); }
static void v_drv_none(const nvn_refl *r) { CK("drv_reads as a string: none", 0, r->drv_read[0] | r->drv_read[2]); }
static void v_drv2_null(const nvn_refl *r) { CK("drv_reads \"2\": null counts", 1, r->drv_read[2]); CK("not c0", 0, r->drv_read[0]); }
static void v_ws(const nvn_refl *r) { CK("white space around the document", 1, r->route_b); }
static void v_geom4(const nvn_refl *r) { CK("geometry slot 4", 4, r->b[0].reg); CK("geometry kind", NVN_RB_GEOMETRY, r->b[0].kind); }
static void v_fail_c1(const nvn_refl *r) { CK("c1 freed by the failure", 1, r->c1 == NULL); CK("has_c1 left as set", 1, r->has_c1); CK("c1_len left as set", 2, r->c1_len); }
static void v_aid_limit(const nvn_refl *r) {   // the parser keeps arg_id as is; the argument-buffer limit NVN_REFL_MAXARG is the replay's check (nvn_mtl.m, max_aid >= NVN_REFL_MAXARG)
    CK("arg_id MAXARG - 1", NVN_REFL_MAXARG - 1, r->b[0].aid); CK("arg_id MAXARG kept", NVN_REFL_MAXARG, r->b[1].aid);
}

typedef struct { const char *name, *json; int fails; const char *err; void (*expect)(const nvn_refl *); } refl_case;
static const refl_case CASES[] = {
    // t_refl.m's synthetic samples
    {"atomic image", J({"route": "B", "bindings": [
        {"kind": "img", "reg": 2, "arg_id": 0, "msl_name": "cp_i_cb2_4C_r32ui", "msl_type": "texture_buffer<uint, access::read_write>", "atomic": true},
        {"kind": "img", "reg": 3, "arg_id": 1, "msl_name": "cp_i_cb2_4E_r32ui", "msl_type": "texture3d<uint, access::write>"}]}), 0, NULL, v_atomic},
    {"PC chain reflection (no route = B) rejected", J({"arg_buffer_index": 0, "bindings": [{"type": "cbuffer", "reg": 10, "arg_id": 0, "access": "constant", "msl_type": "constant cb1_struct*"}],
        "vertex_inputs": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"array of 32 handles in uniform slot 11", J({"route": "B", "bindings": [
        {"kind": "tex_ubo", "reg": 11, "off": 16, "arg_id": 14, "msl_name": "fp_t_cb14_4", "msl_type": "texture2d<float>"},
        {"kind": "samp_ubo", "reg": 11, "off": 16, "arg_id": 15, "msl_name": "fp_t_cb14_4Smplr", "msl_type": "sampler"},
        {"kind": "tex_ubo", "reg": 11, "off": 0, "arg_id": 16, "msl_name": "fp_t2d_cb14_0", "msl_type": "array<texture2d<float>, 32>"},
        {"kind": "samp_ubo", "reg": 11, "off": 0, "arg_id": 48, "msl_name": "fp_t2d_cb14_0Smplr", "msl_type": "array<sampler, 32>"}]}), 0, NULL, v_handle_array},
    {"unknown kind", J({"route": "B", "bindings": [{"kind": "foo", "arg_id": 0, "msl_name": "x", "msl_type": "constant _16*"}]}), 1, "B binding kind foo not implemented (x)", NULL},
    {"texture without a resolved slot", J({"route": "B", "bindings": [{"kind": "tex", "reg": null, "arg_id": 0, "msl_name": "fp_t_x", "msl_type": "texture2d<float>"}]}),
        1, "B binding fp_t_x has no resolved slot number", NULL},
    // every binding kind
    {"every binding kind", J({"route": "B", "drv_reads": {}, "bindings": [
        {"kind": "support", "arg_id": 0, "msl_name": "support_buffer", "msl_type": "constant support_buf&"},
        {"kind": "cbuf", "reg": 3, "arg_id": 1, "msl_name": "fp_c6", "msl_type": "constant fp_c6_t*"},
        {"kind": "c1", "bank": 1, "arg_id": 2, "msl_name": "fp_c1", "msl_type": "constant fp_c1_t*"},
        {"kind": "drvcb", "bank": 0, "arg_id": 3, "msl_name": "fp_c0", "msl_type": "constant fp_c0_t*"},
        {"kind": "drvcb", "bank": 2, "arg_id": 4, "msl_name": "fp_c2", "msl_type": "constant fp_c2_t*"},
        {"kind": "samp", "reg": 7, "arg_id": 5, "msl_name": "fp_t_cb2_8Smplr", "msl_type": "sampler"},
        {"kind": "samp_tex", "reg": 4, "arg_id": 6, "msl_name": "fp_t_cb2_9Smplr", "msl_type": "sampler"},
        {"kind": "tex", "reg": 41, "arg_id": 7, "msl_name": "fp_t_cb2_a", "msl_type": "texture2d<float>", "fetch1d": true},
        {"kind": "samp_id", "reg": 12, "arg_id": 8, "msl_name": "fp_t_cb2_bSmplr", "msl_type": "sampler"},
        {"kind": "sbuf", "index": 0, "reg": 4, "rw": true, "arg_id": 9, "msl_name": "fp_s0", "msl_type": "device fp_s0_t*"},
        {"kind": "sbuf", "index": 1, "reg": 5, "rw": false, "arg_id": 10, "msl_name": "fp_s1", "msl_type": "device fp_s1_t*"},
        {"kind": "sbuf", "index": 2, "reg": 6, "arg_id": 11, "msl_name": "fp_s2", "msl_type": "const device fp_s2_t*"},
        {"kind": "sbuf", "index": 3, "reg": 7, "arg_id": 12, "msl_name": "fp_s3", "msl_type": "device fp_s3_t*"},
        {"kind": "ubo_ptr", "reg": 2, "rw": false, "arg_id": 13, "msl_name": "cp_s4", "msl_type": "device cp_s4_t*"},
        {"kind": "sbuf_ubo", "reg": 0, "off": 56, "rw": true, "arg_id": 14, "msl_name": "cp_s5", "msl_type": "device cp_s5_t*"},
        {"kind": "geometry", "reg": 2, "arg_id": 15, "msl_name": "geo_x", "msl_type": "device uint*"},
        {"kind": "tbuf", "reg": 8, "arg_id": 16, "msl_name": "fp_tb_cb2_c", "msl_type": "texture_buffer<uint>"},
        {"kind": "img", "reg": 1, "arg_id": 17, "msl_name": "fp_i_cb2_d", "msl_type": "texture2d<float, access::read>"},
        {"kind": "tex_c2a", "off": 128, "arg_id": 18, "msl_name": "fp_t2das_cb2_20", "msl_type": "array<texture2d_array<float>, 4>"},
        {"kind": "samp_c2a", "off": 128, "arg_id": 22, "msl_name": "fp_t2das_cb2_20Smplr", "msl_type": "array<sampler, 4>"},
        {"kind": "?", "arg_id": 26, "msl_name": "vp_s_cb0_40000000", "msl_type": "array<sampler, 11>"},
        {"kind": "tex", "arg_id": 37, "msl_name": "vp_t_cb0_40000000", "msl_type": "array<texture2d<float>, 11>"},
        {"kind": "tbuf", "arg_id": 48, "msl_name": "vp_tb_cb0_40000000", "msl_type": "array<texture_buffer<int>, 2>"},
        {"kind": "tex", "arg_id": 50, "msl_name": "fp_t_cb0_40000000", "msl_type": "array<depth2d<float>, 3>"}]}), 0, NULL, v_every_kind},
    {"every texture type prefix and pixel class", J({"route": "B", "bindings": [
        {"kind": "tex", "reg": 0, "arg_id": 0, "msl_type": "texture1d<float>"}, {"kind": "tex", "reg": 1, "arg_id": 1, "msl_type": "texture1d_array<float>"},
        {"kind": "tex", "reg": 2, "arg_id": 2, "msl_type": "texture2d<float>"}, {"kind": "tex", "reg": 3, "arg_id": 3, "msl_type": "texture2d_array<float>"},
        {"kind": "tex", "reg": 4, "arg_id": 4, "msl_type": "texturecube<float>"}, {"kind": "tex", "reg": 5, "arg_id": 5, "msl_type": "texturecube_array<float>"},
        {"kind": "tex", "reg": 6, "arg_id": 6, "msl_type": "texture3d<float>"}, {"kind": "tex", "reg": 7, "arg_id": 7, "msl_type": "texture_buffer<float>"},
        {"kind": "tex", "reg": 8, "arg_id": 8, "msl_type": "depth2d<float>"}, {"kind": "tex", "reg": 9, "arg_id": 9, "msl_type": "depth2d_array<float>"},
        {"kind": "tex", "reg": 10, "arg_id": 10, "msl_type": "depthcube<float>"}, {"kind": "tex", "reg": 11, "arg_id": 11, "msl_type": "depthcube_array<float>"},
        {"kind": "tex", "reg": 12, "arg_id": 12, "msl_type": "texture2d_ms<float>"}, {"kind": "tex", "reg": 13, "arg_id": 13, "msl_type": "texture2d<uint>"},
        {"kind": "tex", "reg": 14, "arg_id": 14, "msl_type": "texture2d_array<int>"}, {"kind": "tex", "reg": 15, "arg_id": 15, "msl_type": "texture3d<half>"},
        {"kind": "tex", "reg": 16, "arg_id": 16, "msl_name": "fp_t_no_type"}, {"kind": "img", "reg": 17, "arg_id": 17, "msl_type": "texture1d_array<uint, access::write>"}]}), 0, NULL, v_tex_types},
    {"array<...> forms", J({"route": "B", "bindings": [
        {"kind": "tex_ubo", "reg": 1, "off": 8, "arg_id": 0, "msl_name": "fp_t_cb4_2", "msl_type": "texture2d<float>"},
        {"kind": "tex_ubo", "reg": 1, "off": 0, "arg_id": 1, "msl_name": "fp_t2da_cb4_0", "msl_type": "array<texture2d_array<uint>, 8>"},
        {"kind": "samp_ubo", "reg": 1, "off": 4, "arg_id": 9, "msl_name": "fp_t2da_cb4_0Smplr", "msl_type": "array<sampler, 32>"},
        {"kind": "tex_ubo", "reg": 1, "arg_id": 41, "msl_name": "fp_t2d_cb4_8", "msl_type": "array<texture2d<float>,32>"},
        {"kind": "tex_ubo", "reg": 2, "arg_id": 42, "msl_name": "fp_tc_cb5_0", "msl_type": "array<texturecube<int>, 1>"},
        {"kind": "tex_ubo", "reg": 2, "arg_id": 43, "msl_name": "fp_t2d_cb5_8", "msl_type": "array<texture2d<float>, -1>"},
        {"kind": "samp_ubo", "reg": 2, "arg_id": 44, "msl_name": "fp_t2d_cb5_8Smplr", "msl_type": "array<sampler, 0>"},
        {"kind": "tex_ubo", "reg": 2, "arg_id": 45, "msl_name": "fp_t3d_cb5_10", "msl_type": "array<texture3d<float>, 4> "}]}), 0, NULL, v_arrays},
    {"texture array of 33 handles", J({"route": "B", "bindings": [{"kind": "tex_ubo", "reg": 11, "off": 0, "arg_id": 0, "msl_name": "fp_t2d_cb14_0", "msl_type": "array<texture2d<float>, 33>"}]}),
        1, "B binding fp_t2d_cb14_0: array of 33", NULL},
    {"sampler array of 40 handles, no name", J({"route": "B", "bindings": [{"kind": "samp_ubo", "reg": 11, "off": 0, "arg_id": 0, "msl_type": "array<sampler, 40>"}]}),
        1, "B binding (null): array of 40", NULL},
    {"dynamic sampler array of 33", J({"route": "B", "bindings": [{"kind": "?", "arg_id": 0, "msl_name": "vp_s_cb0_40000000", "msl_type": "array<sampler, 33>"}]}),
        1, "B binding vp_s_cb0_40000000: array of 33", NULL},
    {"dynamic array of 0", J({"route": "B", "bindings": [{"kind": "tex", "arg_id": 0, "msl_name": "vp_t_cb0_40000000", "msl_type": "array<texture2d<float>, 0>"}]}),
        1, "B binding vp_t_cb0_40000000: array of 0", NULL},
    {"dynamic array of buffers", J({"route": "B", "bindings": [{"kind": "?", "arg_id": 0, "msl_name": "vp_x_cb0_40000000", "msl_type": "array<float4, 4>"}]}),
        1, "B binding vp_x_cb0_40000000: dynamic handle array of float4", NULL},
    {"driver-table texture array that is no array", J({"route": "B", "bindings": [{"kind": "tex_c2a", "off": 128, "arg_id": 0, "msl_name": "fp_t2das_cb2_20", "msl_type": "texture2d_array<float>"}]}),
        1, "B binding fp_t2das_cb2_20: array of 0", NULL},
    {"driver-table sampler array of 33", J({"route": "B", "bindings": [{"kind": "samp_c2a", "off": 128, "arg_id": 0, "msl_name": "fp_t2das_cb2_20Smplr", "msl_type": "array<sampler, 33>"}]}),
        1, "B binding fp_t2das_cb2_20Smplr: array of 33", NULL},
    // the storage buffer write flag: rw true / false / "1" / "no" / 0 / 1 / "yes", absent with and without the const prefix
    {"storage buffer write flag", J({"route": "B", "bindings": [
        {"kind": "sbuf", "reg": 0, "arg_id": 0, "rw": true, "msl_type": "const device a*"}, {"kind": "sbuf", "reg": 1, "arg_id": 1, "rw": false, "msl_type": "device a*"},
        {"kind": "sbuf", "reg": 2, "arg_id": 2, "msl_type": "const device a*"}, {"kind": "sbuf", "reg": 3, "arg_id": 3, "msl_type": "device a*"},
        {"kind": "sbuf", "reg": 4, "arg_id": 4, "rw": "1"}, {"kind": "sbuf", "reg": 5, "arg_id": 5, "rw": "no"},
        {"kind": "sbuf_ubo", "reg": 6, "arg_id": 6, "rw": 1, "off": 8}, {"kind": "sbuf_ubo", "reg": 7, "arg_id": 7, "rw": 0, "msl_type": "device a*"},
        {"kind": "sbuf_ubo", "reg": 8, "arg_id": 8, "msl_type": "const device b*"}, {"kind": "sbuf", "reg": 9, "arg_id": 9, "rw": "yes"}]}), 0, NULL, v_rw},
    // the c1 segment
    {"c1 segment", J({"route": "B", "bindings": [], "c1": "0001fF7F"}), 0, NULL, v_c1},
    {"c1 bytes strtoul reads oddly", J({"route": "B", "bindings": [], "c1": "zz10-1 f1z"}), 0, NULL, v_c1_odd_digits},
    {"c1 empty", J({"route": "B", "bindings": [], "c1": ""}), 0, NULL, v_no_c1},
    {"c1 absent", J({"route": "B", "bindings": []}), 0, NULL, v_no_c1},
    {"c1 of an odd hex length", J({"route": "B", "bindings": [], "c1": "abc"}), 1, "c1 is 1 bytes: over the limit or an odd hex length", NULL},
    {"c1 freed by a later failure", J({"route": "B", "bindings": [], "c1": "0011", "vertex_inputs": [{"location": 40, "msl_type": "float4"}]}), 1, "B vertex input 40 out of range", v_fail_c1},
    // defaults, numbers, routes
    {"defaults of a bare document", J({"route": "B", "bindings": []}), 0, NULL, v_defaults},
    {"numbers and strings as Foundation reads them", J({"route": "B", "bindings": [
        {"kind": "cbuf", "reg": 2.0, "arg_id": "7", "msl_name": "vp_c5"}, {"kind": "cbuf", "reg": true, "arg_id": 1e1}, {"kind": "tex_ubo", "reg": 0, "off": "24", "arg_id": 3, "msl_type": "texture2d<float>"},
        {"kind": "cbuf", "reg": false, "arg_id": 4}, {"kind": "drvcb", "bank": "2", "arg_id": 5}, {"kind": "cbuf", "reg": -0, "arg_id": 65539}, {"kind": "cbuf", "reg": 9.75, "arg_id": -65534}]}),
        0, NULL, v_numbers},
    {"slot given as a string", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": "3", "arg_id": 0, "msl_name": "vp_c6"}]}), 1, "B binding vp_c6 has no resolved slot number", NULL},
    {"negative slot", J({"route": "B", "bindings": [{"kind": "samp", "reg": -1, "arg_id": 0, "msl_name": "fp_t_cb2_8Smplr"}]}), 1, "B binding fp_t_cb2_8Smplr has no resolved slot number", NULL},
    {"storage buffer without a slot", J({"route": "B", "bindings": [{"kind": "sbuf", "index": 0, "reg": null, "rw": false, "arg_id": 0, "msl_name": "hp_s0"}]}), 1, "B binding hp_s0 has no resolved slot number", NULL},
    {"image without a slot", J({"route": "B", "bindings": [{"kind": "img", "arg_id": 0, "msl_name": "fp_i_x", "msl_type": "texture2d<float, access::write>"}]}), 1, "B binding fp_i_x has no resolved slot number", NULL},
    {"uniform-buffer texture without a slot", J({"route": "B", "bindings": [{"kind": "tex_ubo", "off": 0, "arg_id": 0, "msl_name": "fp_t_y"}]}), 1, "B binding fp_t_y has no resolved slot number", NULL},
    {"constant sampler without a slot", J({"route": "B", "bindings": [{"kind": "samp_id", "arg_id": 0}]}), 1, "B binding (null) has no resolved slot number", NULL},
    {"compute uniform pointer without a slot", J({"route": "B", "bindings": [{"kind": "ubo_ptr", "arg_id": 0, "msl_name": "cp_s1"}]}), 1, "B binding cp_s1 has no resolved slot number", NULL},
    {"route A", J({"route": "A", "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"route b (case)", J({"route": "b", "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"key Route (case)", J({"Route": "B", "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"route as an array", J({"route": ["B"], "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"route as a number", J({"route": 66, "bindings": []}), 1, "not a B reflection (route ≠ B; route A / PC-chain reflection is retired)", NULL},
    {"bindings missing", J({"route": "B"}), 1, "missing bindings", NULL},
    {"bindings as a dictionary", J({"route": "B", "bindings": {}}), 1, "missing bindings", NULL},
    {"kind and name missing", J({"route": "B", "bindings": [{"reg": 0, "arg_id": 0}]}), 1, "B binding kind (null) not implemented ((null))", NULL},
    {"geometry helper slot 5", J({"route": "B", "bindings": [{"kind": "geometry", "reg": 5, "arg_id": 0}]}), 1, "invalid geometry helper slot", NULL},
    {"geometry helper without a slot", J({"route": "B", "bindings": [{"kind": "geometry", "arg_id": 0}]}), 1, "invalid geometry helper slot", NULL},
    {"geometry helper slot 4", J({"route": "B", "bindings": [{"kind": "geometry", "reg": 4, "arg_id": 0}]}), 0, NULL, v_geom4},
    {"argument indices at the replay's limit", J({"route": "B", "bindings": [{"kind": "support", "arg_id": 191}, {"kind": "support", "arg_id": 192}]}), 0, NULL, v_aid_limit},
    // vertex inputs and their shadows
    {"vertex inputs and shadow attributes", J({"route": "B", "bindings": [], "vertex_inputs": [
        {"location": 0, "msl_type": "float4"}, {"location": 1, "msl_type": "uint4"}, {"location": 2, "msl_type": "int2"}, {"location": 3, "msl_type": "half4"}, {"location": "4", "msl_type": "uint"}],
        "vin": {"entry": "main0", "entry_z": "main0_z", "shadow": [{"location": 1, "float": 16, "sint": 17}, {"location": 2, "float": 18, "uint": 19}, {"location": 4, "sint": 30}]}}), 0, NULL, v_vin},
    {"shadow list missing", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 0, "msl_type": "uint"}], "vin": {}}), 0, NULL, v_vin_empty},
    {"vertex input at location 32", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 32, "msl_type": "float4"}]}), 1, "B vertex input 32 out of range", NULL},
    {"vertex input at location -1", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": -1, "msl_type": "float4"}]}), 1, "B vertex input -1 out of range", NULL},
    {"shadow of a float input", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 0, "msl_type": "float4"}], "vin": {"shadow": [{"location": 0, "uint": 16}]}}),
        1, "shadow input for location 0, which is not an integer input", NULL},
    {"shadow of a missing input", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 0, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 9, "float": 16}]}}),
        1, "shadow input for location 9, which is not an integer input", NULL},
    {"shadow attribute 15", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 1, "float": 15}]}}),
        1, "shadow input float of location 1 at attribute 15: out of 16-30, used twice or of the input's own type", NULL},
    {"shadow attribute 31", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 1, "sint": 31}]}}),
        1, "shadow input sint of location 1 at attribute 31: out of 16-30, used twice or of the input's own type", NULL},
    {"shadow attribute used twice", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}, {"location": 2, "msl_type": "int4"}],
        "vin": {"shadow": [{"location": 1, "float": 16}, {"location": 2, "float": 16}]}}), 1, "shadow input float of location 2 at attribute 16: out of 16-30, used twice or of the input's own type", NULL},
    {"shadow of the input's own type", J({"route": "B", "bindings": [], "vertex_inputs": [{"location": 1, "msl_type": "uint4"}], "vin": {"shadow": [{"location": 1, "uint": 20}]}}),
        1, "shadow input uint of location 1 at attribute 20: out of 16-30, used twice or of the input's own type", NULL},
    // raster facts
    {"raster facts of a fragment shader", J({"route": "B", "bindings": [], "raster": {"discard": 0, "sample_mask": 0, "colors": [0, 2, 9, -1]}}), 0, NULL, v_raster_ps},
    {"raster facts of a vertex shader", J({"route": "B", "bindings": [], "raster": {"pos_attr": 3, "pos_w": "one",
        "uv_from_attr": {"0": 1, "5": 3, "40": 2, "1": 20, "-1": 2},
        "uv_affine": {"2": {"attr": 1, "sx": [3, 4, 1], "ox": [3, 4, 3], "sy": [3, 5, 1], "oy": [3, 5, 3]}, "3": {"attr": 1, "sx": [3, 4, 1], "ox": [16, 0, 0], "sy": [3, 5, 1], "oy": [3, 5, 3]},
                      "4": {"attr": 20, "sx": [3, 4, 1], "ox": [3, 4, 3], "sy": [3, 5, 1], "oy": [3, 5, 3]}, "6": {"attr": 2, "sx": [1, 2], "ox": [3, 4, 3], "sy": [3, 5, 1], "oy": [3, 5, 3]}}}}),
        0, NULL, v_raster_vs},
    {"raster facts out of range", J({"route": "B", "bindings": [], "raster": {"pos_attr": 16, "pos_w": "attr", "colors": {"1": 0, "3": 5}, "discard": 2, "sample_mask": "0"}}), 0, NULL, v_raster_odd},
    {"raster facts as strings", J({"route": "B", "bindings": [], "raster": {"pos_attr": "2", "colors": ["4"]}}), 0, NULL, v_raster_str},
    // readable-tile fetch tables, thread group, driver reads
    {"fetch tables", J({"route": "B", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 5, "varying_locn": 2}, {"fc": 1, "tex_reg": 200, "varying_locn": 31}],
        "mixed_regs": [3, 64, 255, 256, -1], "outputs": [{"color": 0, "type": "float4"}, {"color": 2, "type": "half4"}]}}), 0, NULL, v_fetch},
    {"fetch call table not contiguous", J({"route": "B", "bindings": [], "fetch": {"calls": [{"fc": 1, "tex_reg": 5, "varying_locn": 2}]}}),
        1, "fetch call table not contiguous or out of range (entry 1)", NULL},
    {"fetch call slot 256", J({"route": "B", "bindings": [], "fetch": {"calls": [{"fc": 0, "tex_reg": 256, "varying_locn": 2}]}}),
        1, "fetch call table not contiguous or out of range (entry 0)", NULL},
    {"five fetch outputs", J({"route": "B", "bindings": [], "fetch": {"outputs": [{"color": 0}, {"color": 1}, {"color": 2}, {"color": 3}, {"color": 4}]}}), 1, "more than 4 fetch outputs", NULL},
    {"thread group size", J({"route": "B", "bindings": [], "threadgroup_size": [32, 8, 1]}), 0, NULL, v_tg},
    {"thread group of two", J({"route": "B", "bindings": [], "threadgroup_size": [64, 1]}), 0, NULL, v_tg_none},
    {"thread group null", J({"route": "B", "bindings": [], "threadgroup_size": null}), 0, NULL, v_tg_none},
    {"thread group over 16 bits", J({"route": "B", "bindings": [], "threadgroup_size": [70000, 1, 1]}), 0, NULL, v_tg_wrap},
    {"driver reads of c0", J({"route": "B", "bindings": [], "drv_reads": {"0": {"vec4_offsets": [16], "dynamic": false}}}), 0, NULL, v_drv0},
    {"driver reads as a string", J({"route": "B", "bindings": [], "drv_reads": ""}), 0, NULL, v_drv_none},
    {"driver reads with a null", J({"route": "B", "bindings": [], "drv_reads": {"2": null}}), 0, NULL, v_drv2_null},
    {"driver locations the replay cannot fill", J({"route": "B", "bindings": [], "drv_unfillable": ["c0 dynamic index", "c2+0x160"]}),
        1, "reads driver-bank locations at run time that the replay layer cannot fill: c0 dynamic index c2+0x160", NULL},
    {"no unfillable driver locations", J({"route": "B", "bindings": [], "drv_unfillable": []}), 0, NULL, NULL},
    // the text itself
    {"white space around the document", " \n\t{\"route\": \"B\", \"bindings\": []}\r\n ", 0, NULL, v_ws},
};
#define NCASES (int)(sizeof CASES / sizeof *CASES)
// Text that is not one json value: the C parser's own failure (NSJSONSerialization returns nil for it; t_refl_cmp.m expects both to fail)
static const char *const BAD_TEXT[] = { "{", "", "{\"route\": \"B\", \"bindings\": []} x", "{\"route\": \"B\", \"bindings\": [}", "nul" };
#define NBAD (int)(sizeof BAD_TEXT / sizeof *BAD_TEXT)

// ---- generated documents: the limits ----
typedef struct { char *s; size_t n, cap; } sbuf;
static void put(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sbuf *b, const char *fmt, ...) {
    va_list a; va_start(a, fmt); int k = vsnprintf(NULL, 0, fmt, a); va_end(a);
    if (b->n + (size_t)k + 1 > b->cap) { b->cap = (b->n + (size_t)k + 1) * 2; b->s = realloc(b->s, b->cap); if (!b->s) { perror("realloc"); exit(2); } }
    va_start(a, fmt); vsnprintf(b->s + b->n, (size_t)k + 1, fmt, a); va_end(a); b->n += (size_t)k;
}
enum { LIM_B96, LIM_B97, LIM_V32, LIM_V33, LIM_C1MAX, LIM_C1OVER, LIM_FC32, LIM_FC33, LIM_COUNT };
static const char *const LIM_NAME[LIM_COUNT] = { "96 bindings (NVN_REFL_MAXB)", "97 bindings", "32 vertex inputs (NVN_REFL_MAXV)", "33 vertex inputs", "c1 of 65535 bytes",
                                                 "c1 of 65536 bytes", "32 fetch calls", "33 fetch calls" };
static char *limit_doc(int i) {   // malloc'd
    sbuf b = {0};
    put(&b, "{\"route\": \"B\", \"bindings\": [");
    int nb = i == LIM_B96 ? NVN_REFL_MAXB : i == LIM_B97 ? NVN_REFL_MAXB + 1 : 0;
    for (int k = 0; k < nb; k++) put(&b, "%s{\"kind\": \"cbuf\", \"reg\": %d, \"arg_id\": %d, \"msl_name\": \"fp_c%d\"}", k ? ", " : "", k % 14, k, k % 14 + 3);
    put(&b, "]");
    if (i == LIM_V32 || i == LIM_V33) {
        put(&b, ", \"vertex_inputs\": [");
        for (int k = 0; k < (i == LIM_V32 ? NVN_REFL_MAXV : NVN_REFL_MAXV + 1); k++) put(&b, "%s{\"location\": %d, \"msl_type\": \"%s\"}", k ? ", " : "", k < NVN_REFL_MAXV ? k : 5, k & 1 ? "uint4" : "float4");
        put(&b, "]");
    }
    if (i == LIM_C1MAX || i == LIM_C1OVER) {
        put(&b, ", \"c1\": \"");
        for (int k = 0; k < (i == LIM_C1MAX ? 0xffff : 0x10000); k++) put(&b, "%02x", k & 0xff);
        put(&b, "\"");
    }
    if (i == LIM_FC32 || i == LIM_FC33) {
        put(&b, ", \"fetch\": {\"calls\": [");
        for (int k = 0; k < (i == LIM_FC32 ? 32 : 33); k++) put(&b, "%s{\"fc\": %d, \"tex_reg\": %d, \"varying_locn\": %d}", k ? ", " : "", k, k * 7, 31 - k % 32);
        put(&b, "]}");
    }
    put(&b, "}");
    return b.s;
}

#ifndef T_REFL_CMP
static void check_str(const char *what, const char *want, const char *got) {
    n_checks++;
    if (strcmp(want, got)) { n_failed++; printf("  %s: %s\n    expected \"%s\"\n    got      \"%s\" FAILED\n", cur, what, want, got); }
}
static void run_case(const refl_case *c) {
    nvn_refl r; char err[256] = "";
    cur = c->name; int f0 = n_failed;
    int rc = nvn_refl_from_json_text(c->json, strlen(c->json), &r, err, sizeof err);
    CK("fails", c->fails, rc != 0);
    if (c->err) check_str("error text", c->err, err);
    if (c->fails) CK("c1 NULL after a failure", 1, r.c1 == NULL);
    if (c->expect) c->expect(&r);
    free(r.c1);
    printf("%-64s %s\n", c->name, n_failed == f0 ? "ok" : "FAILED");
}
static void run_limit(int i) {
    char *doc = limit_doc(i); nvn_refl r; char err[256] = "";
    cur = LIM_NAME[i]; int f0 = n_failed;
    int rc = nvn_refl_from_json_text(doc, strlen(doc), &r, err, sizeof err);
    switch (i) {
    case LIM_B96: CK("succeeds", 0, rc); CK("binding count", NVN_REFL_MAXB, r.nb); CK("last slot", (NVN_REFL_MAXB - 1) % 14, r.b[NVN_REFL_MAXB - 1].reg); break;
    case LIM_B97: CK("fails", 1, rc != 0); check_str("error text", "97 bindings, over the limit 96", err); CK("no binding converted", 0, r.nb); break;
    case LIM_V32: CK("succeeds", 0, rc); CK("input count", NVN_REFL_MAXV, r.nv); CK("last input", 31, r.v[31].loc); CK("last input uint", NVN_CT_UINT, r.v[31].ct); break;
    case LIM_V33: CK("fails", 1, rc != 0); check_str("error text", "B vertex input 5 out of range", err); CK("inputs before the failure", NVN_REFL_MAXV, r.nv); break;
    case LIM_C1MAX: CK("succeeds", 0, rc); CK("c1 length", 0xffff, r.c1_len); CK("c1 allocated", 1, r.c1 != NULL); if (r.c1) { CK("c1 last byte", 0xfe, r.c1[0xfffe]); CK("c1[256]", 0, r.c1[256]); } break;
    case LIM_C1OVER: CK("fails", 1, rc != 0); check_str("error text", "c1 is 65536 bytes: over the limit or an odd hex length", err); CK("c1 NULL", 1, r.c1 == NULL); break;
    case LIM_FC32: CK("succeeds", 0, rc); CK("fetch calls", 32, r.nfc); CK("last call slot", 31 * 7, r.fc_reg[31]); CK("last call output", 0, r.fc_locn[31]); break;
    case LIM_FC33: CK("fails", 1, rc != 0); check_str("error text", "fetch call table not contiguous or out of range (entry 32)", err); break;
    }
    free(r.c1); free(doc);
    printf("%-64s %s\n", LIM_NAME[i], n_failed == f0 ? "ok" : "FAILED");
}

// t_refl.m's real samples: B reflection ps_models_p#0 / vs_models_v#0 of the current chain version, expectations hand-copied from the json text
static int skipped;
static char *load(const char *key, size_t *len) {
    const char *root = getenv("DM_ROOT"); char p[1024];
    snprintf(p, sizeof p, "%s%s/build/cache/metal_shader_cache/b-%s/%s.json", root ? root : getenv("HOME") ? getenv("HOME") : ".", root ? "" : "/dm-work", DMSH_CHAIN, key);
    FILE *f = fopen(p, "rb"); char *d = NULL; long n = -1;
    if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n + 1)) && fread(d, 1, (size_t)n, f) == (size_t)n) { d[n] = 0; *len = (size_t)n; }
    else { free(d); d = NULL; }
    if (f) fclose(f);
    if (!d) { printf("SKIP: no reflection sample %s (the samples are translated shaders of Super Mario Bros. Wonder in the NSP script's build cache; DM_ROOT or ~/dm-work/build/cache/metal_shader_cache), the checks on it do not run\n", p); skipped++; }
    return d;
}
static void real_samples(void) {
    nvn_refl r; char err[256] = ""; size_t len = 0; char *d;
    if ((d = load("5c1ca4e7c3e4657c_b", &len))) { cur = "B fragment ps_models_p#0"; int f1 = n_failed;
        CK("conversion succeeds", 0, nvn_refl_from_json_text(d, len, &r, err, sizeof err));
        CK("B flag", 1, r.route_b); CK("argument buffer slot", 0, r.abi); CK("binding count", 44, r.nb); CK("vertex input count", 0, r.nv);
        CK("has c1", 1, r.has_c1); CK("c1 length 256 (control section field)", 256, r.c1_len);
        uint32_t c1w = 0; if (r.c1) memcpy(&c1w, r.c1, 4); CK("first c1 word = -sqrt(1.5) (0xbf9cc471)", 0xbf9cc471u, c1w);
        static const want_bind w[] = {
            {0, NVN_RB_SUPPORT, 0, 0, 0, X, X, X, X}, {1, NVN_RB_CBUF, 10, 1, 0, X, X, X, X}, {4, NVN_RB_C1, 0, 4, 0, X, X, X, X}, {7, NVN_RB_DRV0, 0, 7, 0, X, X, X, X},
            {8, NVN_RB_DRV2, 0, 8, 0, X, X, X, X}, {11, NVN_RB_SBUFP, 4, 11, 0, X, X, X, X}, {13, NVN_RB_TBUF, 8, 13, 0, X, X, X, X}, {14, NVN_RB_TEX, 41, 14, 0, 2, F, X, X},
            {15, NVN_RB_SAMP, 3, 15, 0, X, X, X, X}, {28, NVN_RB_TEX, 39, 28, 0, 3, D, X, X}, {34, NVN_RB_TEX, 33, 34, 0, 5, D, X, X}, {36, NVN_RB_TEX, 46, 36, 0, 7, F, X, X}};
        check_binds(&r, w, NW(w)); free(r.c1); free(d);
        printf("%-64s %s\n", "B fragment ps_models_p#0: 44 bindings", n_failed == f1 ? "ok" : "FAILED"); }
    if ((d = load("54f730a6c0efc118_b", &len))) { cur = "B vertex vs_models_v#0"; int f1 = n_failed;
        CK("conversion succeeds", 0, nvn_refl_from_json_text(d, len, &r, err, sizeof err));
        CK("binding count", 2, r.nb); CK("input count", 4, r.nv); CK("c1 length 0 → no c1", 0, r.has_c1);
        for (int i = 0; i < 4 && i < r.nv; i++) { char m[48]; snprintf(m, sizeof m, "input[%d] attribute = location", i); CK(m, i, r.v[i].nvn); CK(m, i, r.v[i].loc); CK("component float", NVN_CT_FLOAT, r.v[i].ct); }
        CK("binding[0] uniform slot 0", 0, r.b[0].reg); CK("binding[1] uniform slot 3", 3, r.b[1].reg); free(r.c1); free(d);
        printf("%-64s %s\n", "B vertex vs_models_v#0: 4 attributes", n_failed == f1 ? "ok" : "FAILED"); }
}

int main(void) {
    for (int i = 0; i < NCASES; i++) run_case(&CASES[i]);
    for (int i = 0; i < LIM_COUNT; i++) run_limit(i);
    for (int i = 0; i < NBAD; i++) {
        nvn_refl r; char err[256] = ""; char m[64]; cur = "text that is not one json value";
        snprintf(m, sizeof m, "bad text %d fails", i); CK(m, 1, nvn_refl_from_json_text(BAD_TEXT[i], strlen(BAD_TEXT[i]), &r, err, sizeof err) != 0);
        snprintf(m, sizeof m, "bad text %d error text", i); CK(m, 1, !strncmp(err, "reflection json does not parse", 30));
    }
    {   nvn_refl r; char err[8] = ""; cur = "error buffer"; int f0 = n_failed;   // a short buffer gets the truncated text, as snprintf writes it
        CK("fails", 1, nvn_refl_from_json_text("{\"route\": \"B\"}", 14, &r, err, sizeof err) != 0); check_str("truncated text", "missing", err);
        CK("NULL text fails", 1, nvn_refl_from_json_text(NULL, 0, &r, err, sizeof err) != 0);
        const char *t = "{\"route\": \"B\", \"bindings\": []}garbage"; CK("length bounds the text", 0, nvn_refl_from_json_text(t, strlen(t) - 7, &r, err, sizeof err));
        printf("%-64s %s\n", "error buffer, NULL text, length-bounded text", n_failed == f0 ? "ok" : "FAILED"); }
    real_samples();
    if (skipped) printf("%d reflection sample files missing (those checks skipped)\n", skipped);
    printf("== t_refl_c: %d checks, %d failed ==\n", n_checks, n_failed);
    return n_failed != 0;
}
#endif
