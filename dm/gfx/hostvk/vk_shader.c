// The Vulkan backend's shaders (docs/design/vulkan-backend.md 2.3, 2.4, 2.6): a program's shaders by content key (prog_key, hle_nvn.c), their stage from the profile's shader map
// (shader_map.tsv, DM_NVN_SHMAP), their modules and reflection from the Vulkan library: <DM_MSL_DIR or the default shader cache>/vk-<DMSH_CHAIN_VK>/<key>.spv and <key>.json, written
// by dm/gfx/tools/dmsh_build_vk.py, as the Metal backend reads b-<DMSH_CHAIN>/ beneath the same folder (DM_MSL_DIR is the existing library switch; no new one).
// The reflection: the Vulkan build writes the neutral binding kinds of nvn_refl.h with each binding's set and binding and its SPIR-V class (2.6), where the Metal build wrote MSL
// names and types. nvn_refl_from_json_text, the frontend's parser, reads the Metal form; this file rewrites the Vulkan document into that form (route B, the kinds as they are,
// msl_type made from the class, the image dimension and the sampled type, arg_id the binding's index) and keeps beside it what the Metal form has no field for: each binding's
// set, binding, descriptor type and count, merging a combined image and its sampler into one descriptor (hvk_binding).
// The module's capabilities are checked against the features the device layer enabled (vk_device.c hostvk_dev.x): a module that needs one the device lacks is refused with a red
// line, as a draw with it would fail validation. The counter kernels embedded in games' main (nvn_builtin.h) are the backend's own GLSL (shaders/q_*.comp), found by the same key.
#include "plat/plat.h"
#include "vk_backend.h"
#include "../nvn/dmsh_chain_vk.h"
#include "../nvn/nvn_builtin.h"
#include "../nvn/nvn_refl_textype.h"
#include "../../third_party/cjson/cJSON.h"
#include "shaders/gen/q_val0.h"
#include "shaders/gen/q_val8.h"
#include "shaders/gen/q_dlt0.h"
#include "shaders/gen/q_dlt8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- The shader map and the library directory ----------
typedef struct { char key[17]; int stage; } map_ent;
static nvn_hmap g_map;            // 16-hex-digit key → map_ent
static int g_map_loaded;
static char g_dir[1024];
static int name_index(const char *s, const char *const names[6]) {   // the NVN stage a name of the table gives (0 vertex, 1 fragment, 2 geometry, 3 hull, 4 domain, 5 compute)
    for (int i = 0; s && i < 6; i++) if (!strcmp(s, names[i])) return i;
    return -1;
}
static int stage_of(const char *s) { static const char *const n[6] = {"vertex", "pixel", "geometry", "hull", "domain", "compute"}; return name_index(s, n); }
static int stage_of_refl(const char *s) { static const char *const n[6] = {"vs", "ps", "gs", "hs", "ds", "cs"}; return name_index(s, n); }
static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[len] = 0; *n = (size_t)len; }
    return b;
}
const char *hvk_shader_dir(void) {
    if (!g_dir[0]) {
        const char *m = getenv("DM_MSL_DIR"), *root = getenv("DM_ROOT"); char parent[900];
        if (m && *m) snprintf(parent, sizeof parent, "%s", m);
        else if (root && *root) snprintf(parent, sizeof parent, "%s/build/cache/metal_shader_cache", root);
        else plat_data_path(parent, sizeof parent, "build/cache/metal_shader_cache");
        snprintf(g_dir, sizeof g_dir, "%s/vk-%s", parent, DMSH_CHAIN_VK);
    }
    return g_dir;
}
static void map_load(void) {
    if (g_map_loaded) return;
    g_map_loaded = 1; nvn_hm_init(&g_map, 16);
    const char *path = getenv("DM_NVN_SHMAP");
    size_t n = 0; char *txt = path && *path ? read_file(path, &n) : NULL;
    if (!txt) { hostvk_log("[RED] [hostvk] no shader map (DM_NVN_SHMAP=%s): every game shader is missing", path ? path : "unset"); return; }
    int count = 0;
    for (char *ln = txt, *next; ln && *ln; ln = next) {
        next = strchr(ln, '\n'); if (next) *next++ = 0;
        size_t l = strlen(ln); if (l && ln[l - 1] == '\r') ln[l - 1] = 0;
        if (!*ln || *ln == '#' || !strncmp(ln, "package", 7)) continue;
        char *col[5] = {0}; int c = 0;
        for (char *p = ln; c < 5 && p; c++) { col[c] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        if (c < 4 || strlen(col[3]) != 16) continue;
        int st = stage_of(col[2]); if (st < 0) continue;
        map_ent *e = malloc(sizeof *e); if (!e) break;
        memcpy(e->key, col[3], 16); e->key[16] = 0; e->stage = st;
        free(nvn_hm_put(&g_map, e->key, e)); count++;
    }
    free(txt);
    hostvk_log("[hostvk] shader map %s: %d shaders; library %s", path, count, hvk_shader_dir());
    char stamp[1100]; snprintf(stamp, sizeof stamp, "%s/inputs.tsv", hvk_shader_dir());
    size_t sn; char *st = read_file(stamp, &sn);
    if (!st) hostvk_log("[RED] [hostvk] the shader library %s has no stamp (inputs.tsv): no full dmsh_build_vk.py run made it as it is", hvk_shader_dir());
    free(st);
}

// ---------- Modules ----------
VkShaderModule hvk_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(hvk.d, &ci, NULL, &m)) return VK_NULL_HANDLE;
    return m;
}
// The capabilities a module declares that the device lacks (OpCapability, opcode 17): a description into why, 0 when it has them all
static int caps_missing(const uint32_t *w, size_t nw, char *why, size_t n) {
    why[0] = 0; int bad = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len) break;
        if (op == 17 && len >= 2) {
            uint32_t c = w[i + 1]; const char *need = NULL;
            switch (c) {
                case 10: if (!hvk.dev.x.float64) need = "Float64"; break;
                case 11: if (!hvk.dev.x.int64) need = "Int64"; break;
                case 22: if (!hvk.dev.x.int16) need = "Int16"; break;
                case 55: case 56: if (!hvk.dev.x.storage_without_format) need = "StorageImage*WithoutFormat"; break;
                case 57: if (!hvk.dev.x.multi_viewport) need = "MultiViewport"; break;
                case 53: if (!hvk.dev.caps.xfb) need = "TransformFeedback"; break;
                default: break;
            }
            if (need) { size_t k = strlen(why); snprintf(why + k, n - k, "%s%s", k ? ", " : "", need); bad = 1; }
        }
        if (op == 54) break;   // OpFunction: past the capabilities
        i += len;
    }
    return bad;
}

// Whether the module declares the Xfb execution mode (OpExecutionMode, opcode 16, mode 11): its draws capture while transform feedback is on
static int module_xfb(const uint32_t *w, size_t nw) {
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len) break;
        if (op == 16 && len >= 3 && w[i + 2] == 11) return 1;
        if (op == 54) break;
        i += len;
    }
    return 0;
}

// ---------- The reflection: the Vulkan form into the Metal form nvn_refl_from_json_text reads ----------
static const cJSON *jget(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }
static const char *jstr(const cJSON *o, const char *k) { const cJSON *v = jget(o, k); return cJSON_IsString(v) ? v->valuestring : NULL; }
static int jint(const cJSON *o, const char *k, int def) { const cJSON *v = jget(o, k); return cJSON_IsNumber(v) ? (int)v->valuedouble : def; }
static int jbool(const cJSON *o, const char *k) { const cJSON *v = jget(o, k); return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0); }
static void msl_image_type(const cJSON *b, char *out, size_t n) {   // texture2d_array<uint, access::read_write>, depth2d<float>, texture_buffer<float>, array<..., N>
    const char *cls = jstr(b, "class"), *dim = jstr(b, "dim"), *sampled = jstr(b, "sampled");
    int arrayed = jbool(b, "arrayed"), depth = jbool(b, "depth"), ms = jbool(b, "ms"), count = jint(b, "n", 1);
    const char *el = !sampled || !strcmp(sampled, "float") ? "float" : !strcmp(sampled, "uint") ? "uint" : "int";
    char base[64], ty[96];
    if (cls && (!strcmp(cls, "texel_buffer") || !strcmp(cls, "storage_texel_buffer")) ) snprintf(base, sizeof base, "texture_buffer");
    else if (dim && !strcmp(dim, "1d")) snprintf(base, sizeof base, arrayed ? "texture1d_array" : "texture1d");
    else if (dim && !strcmp(dim, "3d")) snprintf(base, sizeof base, "texture3d");
    else if (dim && !strcmp(dim, "cube")) snprintf(base, sizeof base, depth ? (arrayed ? "depthcube_array" : "depthcube") : (arrayed ? "texturecube_array" : "texturecube"));
    else if (dim && !strcmp(dim, "buffer")) snprintf(base, sizeof base, "texture_buffer");
    else snprintf(base, sizeof base, "%s%s%s", depth ? "depth2d" : "texture2d", ms ? "_ms" : "", arrayed ? "_array" : "");
    int storage = cls && (!strcmp(cls, "storage_image") || !strcmp(cls, "storage_texel_buffer"));
    snprintf(ty, sizeof ty, "%s<%s%s>", base, el, storage ? (jbool(b, "rw") ? ", access::read_write" : ", access::read") : "");
    if (cls && !strcmp(cls, "sampler")) snprintf(ty, sizeof ty, "sampler");
    if (count != 1 || jget(b, "n")) snprintf(out, n, "array<%s, %d>", ty, count); else snprintf(out, n, "%s", ty);
}
static VkImageViewType dim_type(const cJSON *b) {
    const char *dim = jstr(b, "dim"); int arrayed = jbool(b, "arrayed");
    if (!dim) return VK_IMAGE_VIEW_TYPE_2D;
    if (!strcmp(dim, "1d")) return arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    if (!strcmp(dim, "3d")) return VK_IMAGE_VIEW_TYPE_3D;
    if (!strcmp(dim, "cube")) return arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}
// The Vulkan reflection document (json, n bytes) into sh: the neutral reflection through the frontend's parser and the descriptors. 0, or 1 with the reason in err
static int refl_vk(const char *json, size_t n, hvk_shader *sh, char *err, size_t en) {
    cJSON *j = cJSON_ParseWithLength(json, n);
    if (!j) { snprintf(err, en, "the reflection json does not parse"); return 1; }
    int rc = 1; cJSON *m = NULL; char *text = NULL;
    const char *chain = jstr(j, "chain"), *target = jstr(j, "target");
    if (!chain || strcmp(chain, DMSH_CHAIN_VK)) { snprintf(err, en, "translation chain version %s, but this host expects %s", chain ? chain : "(none)", DMSH_CHAIN_VK); goto out; }
    if (target && strcmp(target, "vk")) { snprintf(err, en, "a reflection for target %s, not vk", target); goto out; }
    int st = stage_of_refl(jstr(j, "stage"));
    if (st >= 0 && sh->stage >= 0 && st != sh->stage) { snprintf(err, en, "the library says stage %s, the shader map another", jstr(j, "stage")); goto out; }
    if (sh->stage < 0) sh->stage = st;
    snprintf(sh->entry, sizeof sh->entry, "%s", jstr(j, "entry") ? jstr(j, "entry") : "main");
    if (jstr(j, "entry_u")) snprintf(sh->entry_u, sizeof sh->entry_u, "%s", jstr(j, "entry_u"));
    if (jstr(j, "entry_m")) { snprintf(sh->entry_m, sizeof sh->entry_m, "%s", jstr(j, "entry_m")); sh->mask_m = (uint8_t)jint(j, "uint_mask_m", 0); }
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "route", "B");
    static const char *const copy[] = {"c1", "drv_reads", "drv_unfillable", "raster", "threadgroup_size", "vin"};
    for (size_t i = 0; i < sizeof copy / sizeof *copy; i++) { const cJSON *v = jget(j, copy[i]); if (v) cJSON_AddItemToObject(m, copy[i], cJSON_Duplicate(v, 1)); }
    cJSON *bs = cJSON_AddArrayToObject(m, "bindings");
    const cJSON *vb = jget(j, "bindings"); int i = 0;
    for (const cJSON *b = cJSON_IsArray(vb) ? vb->child : NULL; b; b = b->next, i++) {
        cJSON *o = cJSON_CreateObject(); char ty[160];
        const char *kind = jstr(b, "kind");
        cJSON_AddStringToObject(o, "kind", kind ? kind : "?");
        if (cJSON_IsNumber(jget(b, "reg"))) cJSON_AddNumberToObject(o, "reg", jint(b, "reg", 0));
        cJSON_AddNumberToObject(o, "arg_id", i);
        cJSON_AddStringToObject(o, "msl_name", jstr(b, "name") ? jstr(b, "name") : "");
        const char *cls = jstr(b, "class");
        if (cls && strcmp(cls, "ubo") && strcmp(cls, "ssbo")) { msl_image_type(b, ty, sizeof ty); cJSON_AddStringToObject(o, "msl_type", ty); }
        static const char *const pass[] = {"off", "bank", "rw", "atomic", "fetch1d"};
        for (size_t k = 0; k < sizeof pass / sizeof *pass; k++) { const cJSON *v = jget(b, pass[k]); if (v) cJSON_AddItemToObject(o, pass[k], cJSON_Duplicate(v, 1)); }
        cJSON_AddItemToArray(bs, o);
    }
    cJSON *vi = cJSON_AddArrayToObject(m, "vertex_inputs");
    const cJSON *vin = jget(j, "vertex_inputs");
    for (const cJSON *v = cJSON_IsArray(vin) ? vin->child : NULL; v; v = v->next) {
        cJSON *o = cJSON_CreateObject(); const char *t = jstr(v, "type");
        int loc = jint(v, "location", -1);
        cJSON_AddNumberToObject(o, "location", loc);
        cJSON_AddStringToObject(o, "msl_type", !t || !strcmp(t, "float") ? "float4" : !strcmp(t, "uint") ? "uint4" : "int4");
        cJSON_AddItemToArray(vi, o);
        if (loc >= 0 && loc < 32) sh->vin_types[loc] = (uint32_t)(1 + (!t || !strcmp(t, "float") ? NVN_CT_FLOAT : !strcmp(t, "uint") ? NVN_CT_UINT : NVN_CT_INT));
    }
    text = cJSON_PrintUnformatted(m);
    if (!text) { snprintf(err, en, "the reflection could not be rewritten"); goto out; }
    char perr[200];
    if (nvn_refl_from_json_text(text, strlen(text), &sh->rf, perr, sizeof perr)) { snprintf(err, en, "%s", perr); goto out; }
    // The descriptors: one per (set, binding); a combined image's texture and sampler entries share it
    sh->nb = 0; i = 0;
    for (const cJSON *b = cJSON_IsArray(vb) ? vb->child : NULL; b; b = b->next, i++) {
        uint32_t set = (uint32_t)jint(b, "set", 0), bind = (uint32_t)jint(b, "binding", 0);
        const char *cls = jstr(b, "class"); if (!cls) cls = "";
        int k = 0; while (k < sh->nb && !(sh->b[k].set == set && sh->b[k].binding == bind)) k++;
        if (k == sh->nb) {
            if (sh->nb == HVK_MAXB) { snprintf(err, en, "more than %d descriptors", HVK_MAXB); goto out; }
            hvk_binding *d = &sh->b[sh->nb++]; memset(d, 0, sizeof *d);
            d->set = set; d->binding = bind; d->count = (uint32_t)(jget(b, "n") ? jint(b, "n", 1) : 1); d->rb_tex = d->rb_samp = -1;
            d->type = !strcmp(cls, "ubo") ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : !strcmp(cls, "ssbo") ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                      !strcmp(cls, "combined") ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : !strcmp(cls, "texture") ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
                      !strcmp(cls, "sampler") ? VK_DESCRIPTOR_TYPE_SAMPLER : !strcmp(cls, "texel_buffer") ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                      !strcmp(cls, "storage_texel_buffer") ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : !strcmp(cls, "storage_image") ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_MAX_ENUM;
            if (d->type == VK_DESCRIPTOR_TYPE_MAX_ENUM) { snprintf(err, en, "binding %s of class %s: no descriptor type", jstr(b, "name") ? jstr(b, "name") : "?", cls); goto out; }
            d->dim = (uint8_t)dim_type(b);
            const char *sm = jstr(b, "sampled"); d->cls = !sm || !strcmp(sm, "float") ? NVN_VK_FLOAT : !strcmp(sm, "uint") ? NVN_VK_UINT : NVN_VK_SINT;
        }
        hvk_binding *d = &sh->b[k];
        uint8_t rk = sh->rf.b[i].kind;
        int is_samp = !strcmp(cls, "sampler") || rk == NVN_RB_SAMP || rk == NVN_RB_SAMPT || rk == NVN_RB_SAMPU || rk == NVN_RB_SAMPI || rk == NVN_RB_SAMPD || rk == NVN_RB_SAMPA;
        if (is_samp) d->rb_samp = (int16_t)i; else d->rb_tex = (int16_t)i;
        if (jbool(b, "rw") && (d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || d->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)) { d->rw = 1; sh->writes_storage = 1; }
    }
    rc = 0;
out:
    free(text); cJSON_Delete(m); cJSON_Delete(j);
    return rc;
}

// ---------- Loading ----------
static nvn_hmap g_sh;     // 24-byte key → hvk_shader (NULL entries are never stored: a failed key is in g_fail)
static nvn_hmap g_fail;   // 24-byte key → (void *)1
static int c1_make(hvk_shader *sh) {   // the shader's constants as a uniform buffer: 64 KiB, as the Metal backend gives a c1 whose indices are not proven bounded (c1_full.tsv, phase P4)
    if (!sh->rf.has_c1) return 1;
    void *map;
    if (hvk_buffer_make(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1, &sh->c1buf, &sh->c1mem, &map)) return 0;
    memset(map, 0, 65536); memcpy(map, sh->rf.c1, sh->rf.c1_len);
    return 1;
}
static hvk_shader *shader_from(const char *key, int stage, const uint32_t *code, size_t bytes, const char *json, size_t jn, char *err, size_t en) {
    hvk_shader *sh = calloc(1, sizeof *sh);
    if (!sh) { snprintf(err, en, "out of memory"); return NULL; }
    snprintf(sh->key, sizeof sh->key, "%s", key); sh->stage = stage;
    char why[200];
    if (caps_missing(code, bytes / 4, why, sizeof why)) { snprintf(err, en, "the module needs %s, which this device lacks", why); free(sh); return NULL; }
    if (refl_vk(json, jn, sh, err, en)) { free(sh->rf.c1); free(sh); return NULL; }
    sh->xfb = (uint8_t)module_xfb(code, bytes / 4);
    if (!(sh->module = hvk_module(code, bytes))) { snprintf(err, en, "vkCreateShaderModule failed"); free(sh->rf.c1); free(sh); return NULL; }
    if (!c1_make(sh)) { snprintf(err, en, "no memory for its constants"); vkDestroyShaderModule(hvk.d, sh->module, NULL); free(sh->rf.c1); free(sh); return NULL; }
    atomic_fetch_add(&hvk.st_shaders, 1);
    return sh;
}
// The backend's counter kernels for nvn_builtin.h's keys: the module and a reflection in the Vulkan form (n in uniform slot 0 = c3, src and dst in storage slots 0 and 1)
static hvk_shader *builtin_get(const char *ctl) {
    static const struct { const char *fn; const uint32_t *code; size_t bytes; } K[] = {
        {"q_val0", q_val0_spv, sizeof q_val0_spv}, {"q_val8", q_val8_spv, sizeof q_val8_spv}, {"q_dlt0", q_dlt0_spv, sizeof q_dlt0_spv}, {"q_dlt8", q_dlt8_spv, sizeof q_dlt8_spv}};
    for (unsigned i = 0; i < sizeof k_nvn_builtin_map / sizeof *k_nvn_builtin_map; i++) {
        if (strcmp(ctl, k_nvn_builtin_map[i].ctl)) continue;
        for (unsigned k = 0; k < sizeof K / sizeof *K; k++) if (!strcmp(K[k].fn, k_nvn_builtin_map[i].fn)) {
            char json[640], err[200];
            snprintf(json, sizeof json, "{\"key\":\"%s\",\"target\":\"vk\",\"chain\":\"%s\",\"stage\":\"cs\",\"entry\":\"main\",\"threadgroup_size\":[32,1,1],\"bindings\":["
                     "{\"name\":\"cs_c3\",\"set\":0,\"binding\":4,\"class\":\"ubo\",\"kind\":\"cbuf\",\"reg\":0},"
                     "{\"name\":\"cs_s0\",\"set\":1,\"binding\":0,\"class\":\"ssbo\",\"kind\":\"sbuf\",\"reg\":0,\"rw\":false},"
                     "{\"name\":\"cs_s1\",\"set\":1,\"binding\":1,\"class\":\"ssbo\",\"kind\":\"sbuf\",\"reg\":1,\"rw\":true}]}", K[k].fn, DMSH_CHAIN_VK);
            hvk_shader *sh = shader_from(K[k].fn, 5, K[k].code, K[k].bytes, json, strlen(json), err, sizeof err);
            if (!sh) hostvk_log("[RED] [hostvk] built-in counter kernel %s: %s", K[k].fn, err);
            else sh->builtin = 1;
            return sh;
        }
    }
    return NULL;
}
static hvk_shader *shader_get(const char *key, int stage) {
    char k24[24] = {0}; snprintf(k24, sizeof k24, "%s", key);
    if (!g_sh.klen) { nvn_hm_init(&g_sh, 24); nvn_hm_init(&g_fail, 24); }
    hvk_shader *sh = nvn_hm_get(&g_sh, k24);
    if (sh || nvn_hm_get(&g_fail, k24)) return sh;
    char p[1200], err[300]; size_t sn = 0, jn = 0;
    snprintf(p, sizeof p, "%s/%s.spv", hvk_shader_dir(), key); char *spv = read_file(p, &sn);
    snprintf(p, sizeof p, "%s/%s.json", hvk_shader_dir(), key); char *json = read_file(p, &jn);
    if (!spv || !json || (sn & 3) || sn < 20) {
        hostvk_log("[RED] [hostvk] shader %s: %s is missing from %s (build the library with dm/gfx/tools/dmsh_build_vk.py)", key, !spv ? "the module" : "the reflection", hvk_shader_dir());
        sh = NULL;
    } else if (!(sh = shader_from(key, stage, (const uint32_t *)(void *)spv, sn, json, jn, err, sizeof err))) hostvk_log("[RED] [hostvk] shader %s failed to load: %s", key, err);
    free(spv); free(json);
    if (sh) nvn_hm_put(&g_sh, k24, sh); else nvn_hm_put(&g_fail, k24, (void *)1);
    return sh;
}

// ---------- Programs ----------
static nvn_hmap g_prog;   // serial → hvk_prog
typedef struct { uint64_t serial; const prog_t *bst[5]; uint64_t bser[5]; } comp_t;
static nvn_hmap g_comp;   // serial → comp_t: a combined program (nvn_state_hook_compose)
uint64_t hvk_prog_compose(uint64_t old_serial, const prog_t *const bst[5]) {
    if (!g_comp.klen) nvn_hm_init(&g_comp, 8);
    if (old_serial) { free(nvn_hm_del(&g_comp, &old_serial)); if (g_prog.klen) free(nvn_hm_del(&g_prog, &old_serial)); }
    comp_t *c = calloc(1, sizeof *c); if (!c) return nvn_new_serial();
    c->serial = nvn_new_serial();
    for (int i = 0; i < 5; i++) { c->bst[i] = bst[i]; c->bser[i] = bst[i] ? bst[i]->serial : 0; }
    nvn_hm_put(&g_comp, &c->serial, c);
    return c->serial;
}
void hvk_prog_drop(uint64_t serial) { if (serial && g_prog.klen) free(nvn_hm_del(&g_prog, &serial)); }
static hvk_prog *prog_load(const prog_t *p);
hvk_prog *hvk_prog_get(const prog_t *p) {
    if (!p) return NULL;
    if (!g_prog.klen) nvn_hm_init(&g_prog, 8);
    hvk_prog *o = p->serial ? nvn_hm_get(&g_prog, &p->serial) : NULL;
    if (o) return o;
    comp_t *c = g_comp.klen && p->serial ? nvn_hm_get(&g_comp, &p->serial) : NULL;
    if (!(o = calloc(1, sizeof *o))) return NULL;
    o->serial = p->serial;
    if (c) {   // a combined program: each stage from the program bound there
        for (int st = 0; st < 5; st++) if (c->bst[st]) { hvk_prog *s = prog_load(c->bst[st]); if (s) o->sh[st] = s->sh[st]; }
    } else {
        hvk_prog *s = prog_load(p);
        if (s) { memcpy(o->sh, s->sh, sizeof o->sh); free(s); }
    }
    if (p->serial) nvn_hm_put(&g_prog, &p->serial, o);
    return o;
}
static hvk_prog *prog_load(const prog_t *p) {   // a program's own stages (by the shader map), not cached here
    if (p->serial && g_prog.klen) { hvk_prog *have = nvn_hm_get(&g_prog, &p->serial); if (have) { hvk_prog *cp = malloc(sizeof *cp); if (cp) *cp = *have; return cp; } }
    hvk_prog *o = calloc(1, sizeof *o); if (!o) return NULL;
    map_load();
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        uint8_t md[8]; if (!prog_key(p, i, md)) continue;
        char k[17]; for (int b = 0; b < 8; b++) snprintf(k + 2 * b, 3, "%02x", md[b]);
        map_ent *e = nvn_hm_get(&g_map, k);
        if (!e) {
            hvk_shader *b = builtin_get(k);
            if (b) { o->sh[5] = b; continue; }
            hostvk_log("[RED] [hostvk] shader %s is not in the shader map", k);
            continue;
        }
        hvk_shader *sh = shader_get(k, e->stage);
        if (sh) o->sh[e->stage] = sh;
    }
    return o;
}
void hvk_shader_shutdown(void) {
    for (uint32_t i = 0; i < g_sh.cap; i++) if (g_sh.vals && g_sh.vals[i]) {
        hvk_shader *sh = g_sh.vals[i];
        if (sh->module) vkDestroyShaderModule(hvk.d, sh->module, NULL);
        if (sh->c1buf) { vkDestroyBuffer(hvk.d, sh->c1buf, NULL); vkFreeMemory(hvk.d, sh->c1mem, NULL); hvk.dev.allocations--; }
        free(sh->rf.c1); free(sh);
    }
    nvn_hm_clear(&g_sh, NULL); nvn_hm_clear(&g_fail, NULL);
    for (uint32_t i = 0; i < g_prog.cap; i++) if (g_prog.vals && g_prog.vals[i]) free(g_prog.vals[i]);
    nvn_hm_clear(&g_prog, NULL);
    for (uint32_t i = 0; i < g_comp.cap; i++) if (g_comp.vals && g_comp.vals[i]) free(g_comp.vals[i]);
    nvn_hm_clear(&g_comp, NULL);
    for (uint32_t i = 0; i < g_map.cap; i++) if (g_map.vals && g_map.vals[i]) free(g_map.vals[i]);
    nvn_hm_clear(&g_map, NULL); g_map_loaded = 0; g_dir[0] = 0;
}
