// The plain C reflection parser (nvn_refl.c) against the Objective-C one (nvn_refl.m) on the Mac: every document of a built-in corpus (t_refl_c.c's cases, limits and bad text,
// and generated variants: each binding kind, each texture type, arrays, slots of every json type, with and without the optional fields, the document-level fields) is parsed by both,
// NSJSONSerialization + nvn_refl_from_json and nvn_refl_from_json_text, and they must agree: success or failure, the error text, the struct byte for byte (c1 compared by content).
// DM_REFL_DIR=<folder>: every *.json under it as well (a build cache's b-<V>/ folders hold the real reflection of a game's libraries).
// Probes: documents where the C parser approximates Foundation (number forms cJSON cannot tell apart, duplicate keys, ...); a difference there is printed, not counted.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stddef.h>
#define T_REFL_CMP 1
#include "t_refl_c.c"   // the corpus (CASES, limit_doc, BAD_TEXT), without its main

// nvn_refl.c writes these numbers into dtt without a Metal header: they must be MTLTextureType's
_Static_assert(NVN_MTL_TEX_1D == MTLTextureType1D, "MTLTextureType1D");
_Static_assert(NVN_MTL_TEX_1D_ARRAY == MTLTextureType1DArray, "MTLTextureType1DArray");
_Static_assert(NVN_MTL_TEX_2D == MTLTextureType2D, "MTLTextureType2D");
_Static_assert(NVN_MTL_TEX_2D_ARRAY == MTLTextureType2DArray, "MTLTextureType2DArray");
_Static_assert(NVN_MTL_TEX_2D_MULTISAMPLE == MTLTextureType2DMultisample, "MTLTextureType2DMultisample");
_Static_assert(NVN_MTL_TEX_CUBE == MTLTextureTypeCube, "MTLTextureTypeCube");
_Static_assert(NVN_MTL_TEX_CUBE_ARRAY == MTLTextureTypeCubeArray, "MTLTextureTypeCubeArray");
_Static_assert(NVN_MTL_TEX_3D == MTLTextureType3D, "MTLTextureType3D");
_Static_assert(NVN_MTL_TEX_2D_MULTISAMPLE_ARRAY == MTLTextureType2DMultisampleArray, "MTLTextureType2DMultisampleArray");
_Static_assert(NVN_MTL_TEX_BUFFER == MTLTextureTypeTextureBuffer, "MTLTextureTypeTextureBuffer");

static int n_docs, n_mismatch, n_probes, n_probe_diff;

static void where(const nvn_refl *a, const nvn_refl *b) {   // the first differing byte of two structs (c1 pointers zeroed)
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    size_t i = 0;
    while (i < sizeof *a && x[i] == y[i]) i++;
    if (i == sizeof *a) return;
    size_t b0 = offsetof(nvn_refl, b), v0 = offsetof(nvn_refl, v);
    if (i >= b0 && i < b0 + sizeof a->b) printf("    first difference at byte %zu: binding %zu, byte %zu of nvn_rbind: Objective-C %u, C %u\n", i, (i - b0) / sizeof a->b[0], (i - b0) % sizeof a->b[0], (unsigned)x[i], (unsigned)y[i]);
    else if (i >= v0 && i < v0 + sizeof a->v) printf("    first difference at byte %zu: vertex input %zu, byte %zu: Objective-C %u, C %u\n", i, (i - v0) / sizeof a->v[0], (i - v0) % sizeof a->v[0], (unsigned)x[i], (unsigned)y[i]);
    else printf("    first difference at byte %zu of %zu (nb at %zu, has_vin at %zu, c1_len at %zu, nfc at %zu, uv_aff at %zu): Objective-C %u, C %u\n", i, sizeof *a, offsetof(nvn_refl, nb),
                offsetof(nvn_refl, has_vin), offsetof(nvn_refl, c1_len), offsetof(nvn_refl, nfc), offsetof(nvn_refl, uv_aff), (unsigned)x[i], (unsigned)y[i]);
}

// Both parsers on one document; a difference is a mismatch (or, for a probe, a printed note)
static void compare(const char *name, const char *json, size_t len, int probe) {
    @autoreleasepool {
        nvn_refl a, b; char ea[256], eb[256];
        memset(&a, 0, sizeof a); memset(ea, 0, sizeof ea); memset(eb, 0, sizeof eb);
        id obj = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json length:len] options:0 error:nil];
        int rb = nvn_refl_from_json_text(json, len, &b, eb, sizeof eb);
        const char *why = NULL; int ra = -1;
        if (![obj isKindOfClass:[NSDictionary class]]) {   // Foundation does not parse it (or not to a dictionary, which nvn_refl_from_json does not take): the C parser must fail
            if (!obj && !(rb && !strncmp(eb, "reflection json does not parse", 30))) why = "NSJSONSerialization does not parse it; the C parser does not fail with \"reflection json does not parse\"";
            else if (obj && !rb) why = "the document is not a dictionary; the C parser succeeds";
        } else {
            ra = nvn_refl_from_json(obj, &a, ea, sizeof ea);
            if ((ra != 0) != (rb != 0)) why = "one parser fails, the other succeeds";
            else if (strcmp(ea, eb)) why = "error texts differ";
            else if (a.c1_len != b.c1_len || (a.c1 == NULL) != (b.c1 == NULL) || (a.c1 && memcmp(a.c1, b.c1, a.c1_len))) why = "c1 segments differ";
            else { uint8_t *pa = a.c1, *pb = b.c1; a.c1 = b.c1 = NULL; if (memcmp(&a, &b, sizeof a)) why = "structs differ"; a.c1 = pa; b.c1 = pb; }
        }
        if (probe) { n_probes++; n_probe_diff += why != NULL; printf("probe %-58s %s%s\n", name, why ? "differs: " : "same", why ? why : ""); }
        else { n_docs++; if (why) { n_mismatch++; printf("MISMATCH %s: %s\n", name, why); } }
        if (why) {
            if (ra >= 0) printf("    Objective-C: %s%s\n", ra ? "fails: " : "succeeds", ra ? ea : "");
            printf("    C:           %s%s\n", rb ? "fails: " : "succeeds", rb ? eb : "");
            if (ra >= 0 && !strcmp(why, "structs differ")) { uint8_t *pa = a.c1, *pb = b.c1; a.c1 = b.c1 = NULL; where(&a, &b); a.c1 = pa; b.c1 = pb; }
            if (len < 600) printf("    document: %.*s\n", (int)len, json);
        }
        free(a.c1); free(b.c1);
    }
}
static void compare_str(const char *name, const char *json, int probe) { compare(name, json, strlen(json), probe); }

// ---- generated variants ----
static const char *const KINDS[] = { "cbuf", "samp", "samp_tex", "tex", "tex_ubo", "samp_ubo", "samp_id", "tex_c2a", "samp_c2a", "img", "tbuf", "sbuf", "ubo_ptr", "sbuf_ubo",
                                     "geometry", "support", "c1", "drvcb", "?", "foo", NULL };
static const char *const TYPES[] = { NULL, "texture1d<float>", "texture1d_array<uint>", "texture2d<float>", "texture2d_array<int>", "texturecube<float>", "texturecube_array<uint>",
                                     "texture3d<half>", "texture_buffer<uint>", "texture_buffer<int, access::read_write>", "texture2d<float, access::write>", "texture2d<uint, access::read>",
                                     "depth2d<float>", "depth2d_array<float>", "depthcube<float>", "depthcube_array<float>", "texture2d_ms<float>", "sampler", "const device x*", "device x*",
                                     "constant y*", "array<texture2d<float>, 4>", "array<sampler, 32>", "array<texture2d_array<uint>, 33>", "array<texturecube<float>,4>",
                                     "array<depth2d<float>, 1>", "array<texture_buffer<int>, 2>", "array<float4, 3>", "array<texture3d<float>, 0>", "array<texture2d<int>, -2>", "array<>" };
static const char *const REGS[] = { NULL, "null", "0", "4", "5", "-1", "\"3\"", "true", "false", "2.5", "70000" };
static const char *const NAMES[] = { NULL, "\"fp_t_x\"", "\"vp_t_cb0_40000000\"", "\"fp_s_cb0_40000000Smplr\"", "\"cp_s_cb0_40000000\"" };   // (a name that is no string: nvn_refl.m's error texts send it UTF8String and raise)
static const char *const EXTRA[] = { "", ", \"rw\": true", ", \"rw\": false", ", \"rw\": \"0\"", ", \"rw\": \"Yes\"", ", \"atomic\": true", ", \"fetch1d\": true", ", \"off\": 24",
                                     ", \"bank\": 2", ", \"bank\": 1", ", \"atomic\": 0, \"fetch1d\": 1, \"off\": \"8\"", ", \"off\": 70000, \"rw\": 1", ", \"index\": 2" };
#define N(a) (int)(sizeof a / sizeof *a)
static void binding(char *d, size_t n, const char *kind, const char *type, const char *reg, const char *name, const char *extra, int aid) {
    char k[48] = "", t[80] = "", r[32] = "", m[48] = "";
    if (kind) snprintf(k, sizeof k, "\"kind\": \"%s\", ", kind);
    if (type) snprintf(t, sizeof t, ", \"msl_type\": \"%s\"", type);
    if (reg) snprintf(r, sizeof r, ", \"reg\": %s", reg);
    if (name) snprintf(m, sizeof m, ", \"msl_name\": %s", name);
    snprintf(d, n, "{%s\"arg_id\": %d%s%s%s%s}", k, aid, t, r, m, extra);
}
static void generated_bindings(void) {   // one binding per document: kind × type × slot × name, the optional fields in turn
    char doc[512], bd[400]; int e = 0;
    for (int k = 0; k < N(KINDS); k++) for (int t = 0; t < N(TYPES); t++) for (int r = 0; r < N(REGS); r++) for (int m = 0; m < N(NAMES); m++) {
        binding(bd, sizeof bd, KINDS[k], TYPES[t], REGS[r], NAMES[m], EXTRA[e++ % N(EXTRA)], k * 3 + 1);
        snprintf(doc, sizeof doc, "{\"route\": \"B\", \"bindings\": [%s]}", bd);
        char nm[64]; snprintf(nm, sizeof nm, "binding variant %d.%d.%d.%d", k, t, r, m);
        compare_str(nm, doc, 0);
    }
}
static const char *const SETS[] = {   // binding lists for the document-level variants
    "[]",
    "[{\"kind\": \"drvcb\", \"bank\": 0, \"arg_id\": 0}, {\"kind\": \"cbuf\", \"reg\": 0, \"arg_id\": 1}]",
    "[{\"kind\": \"tex\", \"reg\": 1, \"arg_id\": 0, \"msl_type\": \"depth2d<float>\"}, {\"kind\": \"samp\", \"reg\": 1, \"arg_id\": 1}, {\"kind\": \"drvcb\", \"bank\": 2, \"arg_id\": 2}]",
    "[{\"kind\": \"c1\", \"bank\": 1, \"arg_id\": 0}, {\"kind\": \"support\", \"arg_id\": 1}, {\"kind\": \"foo\", \"arg_id\": 2}]",
    "[{\"kind\": \"tex_ubo\", \"reg\": 2, \"off\": 0, \"arg_id\": 0, \"msl_type\": \"array<texture2d<float>, 32>\"}, {\"kind\": \"tex\", \"arg_id\": 32, \"msl_name\": \"vp_t_cb0_40000000\", \"msl_type\": \"array<texture2d<uint>, 8>\"}]",
};
static const char *const FRAGS[] = {   // document-level fields, alone and in pairs
    "", ", \"c1\": \"00ff\"", ", \"c1\": \"abc\"", ", \"c1\": \"\"", ", \"c1\": \"0123456789abcdefABCDEF\"",   // (c1 null: nvn_refl.m sends length to NSNull and raises)
    ", \"vertex_inputs\": [{\"location\": 0, \"msl_type\": \"uint4\"}, {\"location\": 1, \"msl_type\": \"int\"}, {\"location\": 3, \"msl_type\": \"half2\"}]",
    ", \"vertex_inputs\": [{\"location\": 0, \"msl_type\": \"uint4\"}, {\"location\": 1, \"msl_type\": \"int\"}], \"vin\": {\"shadow\": [{\"location\": 0, \"float\": 16, \"sint\": 17}, {\"location\": 1, \"uint\": 18}]}",
    ", \"vertex_inputs\": [{\"location\": 0, \"msl_type\": \"uint4\"}], \"vin\": {\"shadow\": [{\"location\": 0, \"float\": 16, \"uint\": 17}]}",
    ", \"vertex_inputs\": [{\"location\": 31}, {\"location\": 0}], \"vin\": \"none\"",
    ", \"vertex_inputs\": {}",
    ", \"raster\": {\"discard\": 1, \"sample_mask\": 1, \"colors\": [0, 1, 7, 8]}",
    ", \"raster\": {\"pos_attr\": 15, \"pos_w\": \"attr\", \"uv_from_attr\": {\"31\": 15, \"32\": 1, \"2\": -1}}",
    ", \"raster\": {\"pos_attr\": -1, \"uv_affine\": {\"0\": {\"attr\": 15, \"sx\": [15, 63, 3], \"ox\": [0, 0, 0], \"sy\": [1, 2, 3], \"oy\": [4, 5, 0]}, \"31\": {\"attr\": 0, \"sx\": [0, 64, 0], \"ox\": [0, 0, 0], \"sy\": [0, 0, 0], \"oy\": [0, 0, 0]}}}",
    ", \"raster\": {\"colors\": []}", ", \"raster\": []", ", \"raster\": {\"uv_affine\": [], \"uv_from_attr\": []}",
    ", \"fetch\": {\"calls\": [{\"fc\": 0, \"tex_reg\": 255, \"varying_locn\": 31}], \"mixed_regs\": [0, 63, 128], \"outputs\": [{\"color\": 7}]}",
    ", \"fetch\": {\"calls\": [{\"fc\": 0, \"tex_reg\": 1, \"varying_locn\": 32}]}", ", \"fetch\": {\"mixed_regs\": {\"5\": 1}}", ", \"fetch\": []",
    ", \"threadgroup_size\": [1024, 1, 1]", ", \"threadgroup_size\": [8, 8]", ", \"threadgroup_size\": \"64\"",
    ", \"drv_reads\": {\"0\": {}, \"2\": {}}", ", \"drv_reads\": {\"1\": {}}", ", \"drv_reads\": []",
    ", \"drv_unfillable\": [\"c0+0x10\"]", ", \"drv_unfillable\": []", ", \"drv_unfillable\": \"c0+0x10\"",
};
static void generated_documents(void) {
    char doc[1600];
    for (int s = 0; s < N(SETS); s++) for (int f = 0; f < N(FRAGS); f++) for (int g = f; g < N(FRAGS); g++) {
        if (g != f && (!f || !strncmp(FRAGS[f], FRAGS[g], 8))) continue;   // the empty fragment alone; the same key twice is the duplicate-key probe's
        snprintf(doc, sizeof doc, "{\"route\": \"B\", \"bindings\": %s%s%s}", SETS[s], FRAGS[f], g != f ? FRAGS[g] : "");
        char nm[64]; snprintf(nm, sizeof nm, "document variant %d.%d.%d", s, f, g);
        compare_str(nm, doc, 0);
    }
}

// ---- probes: Foundation behaviour the C parser takes from documentation or CoreFoundation's conversions, not from a run ----
static const char *const PROBES[][2] = {
    {"integer slot past 32 bits (intValue keeps the low 32)", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 4294967298, "arg_id": 0}]})},
    {"integer slot 3000000000 (negative as int)", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 3000000000, "arg_id": 0}]})},
    {"fractional slot past 32 bits (clamped)", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 3000000000.5, "arg_id": 0}]})},
    {"exponent form past 32 bits (a double in Foundation)", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 1, "arg_id": 1e10}]})},
    {"integer past 64 bits", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 1, "arg_id": 18446744073709551621}]})},
    {"rw 256 (boolValue as a char)", J({"route": "B", "bindings": [{"kind": "sbuf", "reg": 0, "arg_id": 0, "rw": 256}]})},
    {"rw 0.5", J({"route": "B", "bindings": [{"kind": "sbuf", "reg": 0, "arg_id": 0, "rw": 0.5}]})},
    {"rw 2 and -1", J({"route": "B", "bindings": [{"kind": "sbuf", "reg": 0, "arg_id": 0, "rw": 2}, {"kind": "sbuf", "reg": 1, "arg_id": 1, "rw": -1}]})},
    {"rw \" y\" and \"-01\" and \"0.5\"", J({"route": "B", "bindings": [{"kind": "sbuf", "reg": 0, "arg_id": 0, "rw": " y"}, {"kind": "sbuf", "reg": 1, "arg_id": 1, "rw": "-01"}, {"kind": "sbuf", "reg": 2, "arg_id": 2, "rw": "0.5"}]})},
    {"intValue of \" 12\", \"+12\", \"12abc\"", J({"route": "B", "bindings": [{"kind": "support", "arg_id": " 12"}, {"kind": "support", "arg_id": "+12"}, {"kind": "support", "arg_id": "12abc"}]})},
    {"intValue of a newline and digits", "{\"route\": \"B\", \"bindings\": [{\"kind\": \"support\", \"arg_id\": \"\\n12\"}, {\"kind\": \"support\", \"arg_id\": \"\\t7\"}]}"},
    {"intValue overflow of a string", J({"route": "B", "bindings": [{"kind": "support", "arg_id": "2147483648"}, {"kind": "support", "arg_id": "-2147483649"}, {"kind": "support", "arg_id": "99999999999999999999"}]})},
    {"duplicate key (last wins)", J({"route": "A", "route": "B", "bindings": []})},
    {"duplicate binding key", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 1, "reg": 2, "arg_id": 0}]})},
    {"uv_from_attr keys \"01\" and \"1\" (enumeration order)", J({"route": "B", "bindings": [], "raster": {"uv_from_attr": {"01": 2, "1": 3}}})},
    {"c1 with a non-ASCII character", J({"route": "B", "bindings": [], "c1": "\u00e91f"})},
    {"c1 with a character outside the BMP", J({"route": "B", "bindings": [], "c1": "\ud83d\ude001f"})},
    {"msl_type with a non-ASCII character", J({"route": "B", "bindings": [{"kind": "tex", "reg": 0, "arg_id": 0, "msl_type": "texture2d<\u00e9int>"}]})},
    {"drv_unfillable of other types", J({"route": "B", "bindings": [], "drv_unfillable": [1, true, null, 2.5, "x"]})},
    {"route with a NUL", J({"route": "B\u0000", "bindings": []})},
    {"leading zero", J({"route": "B", "bindings": [{"kind": "cbuf", "reg": 012, "arg_id": 0}]})},
    {"control character in a string", "{\"route\": \"B\", \"bindings\": [{\"kind\": \"cbuf\", \"reg\": 1, \"arg_id\": 0, \"msl_name\": \"a\tb\"}]}"},
    {"trailing NUL byte", "{\"route\": \"B\", \"bindings\": []}\0"},
    {"UTF-8 byte order mark", "\xEF\xBB\xBF{\"route\": \"B\", \"bindings\": []}"},
};

static int is_json(NSString *p) { return [[p pathExtension] isEqualToString:@"json"]; }

int main(void) { @autoreleasepool {
    for (int i = 0; i < NCASES; i++) compare_str(CASES[i].name, CASES[i].json, 0);
    for (int i = 0; i < LIM_COUNT; i++) { char *d = limit_doc(i); compare_str(LIM_NAME[i], d, 0); free(d); }
    for (int i = 0; i < NBAD; i++) { char nm[32]; snprintf(nm, sizeof nm, "bad text %d", i); compare_str(nm, BAD_TEXT[i], 0); }
    int c0 = n_docs; generated_bindings(); int c1 = n_docs; generated_documents();
    printf("corpus: %d cases, %d limits, %d bad texts, %d binding variants, %d document variants\n", NCASES, LIM_COUNT, NBAD, c1 - c0, n_docs - c1);
    for (int i = 0; i < N(PROBES); i++) {
        size_t len = strlen(PROBES[i][1]);
        if (!strcmp(PROBES[i][0], "trailing NUL byte")) len++;
        compare(PROBES[i][0], PROBES[i][1], len, 1);
    }
    const char *dir = getenv("DM_REFL_DIR");
    if (dir && *dir) {
        int d0 = n_docs, m0 = n_mismatch;
        NSString *root = [NSString stringWithUTF8String:dir];
        NSDirectoryEnumerator *en = [[NSFileManager defaultManager] enumeratorAtPath:root];
        for (NSString *rel in en) {
            if (!is_json(rel)) continue;
            @autoreleasepool {
                NSString *p = [root stringByAppendingPathComponent:rel];
                NSData *d = [NSData dataWithContentsOfFile:p];
                if (!d) { printf("MISMATCH %s: cannot read the file\n", p.UTF8String); n_docs++; n_mismatch++; continue; }
                compare(p.UTF8String, d.bytes, d.length, 0);
            }
        }
        printf("DM_REFL_DIR %s: %d documents, %d mismatches\n", dir, n_docs - d0, n_mismatch - m0);
    } else printf("DM_REFL_DIR not set: the built-in corpus only\n");
    printf("probes: %d of %d differ (informational: Foundation behaviour nvn_refl.c approximates)\n", n_probe_diff, n_probes);
    printf("== t_refl_cmp: %d documents, %d mismatches ==\n", n_docs, n_mismatch);
    return n_mismatch != 0;
} }
