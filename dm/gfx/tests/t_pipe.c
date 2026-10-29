// Unit test of the neutral pipeline description and the prewarm lists' pipeline rows (nvn_pipe.c), CPU only, on every host:
// - every R row of every profile's pso_manifest.tsv (dm/games/*/): pso_skey_parse -> nvn_pipe_from_key -> nvn_pipe_to_key -> pso_skey_hex gives the row's normalised hex back,
//   and its P row (nvn_pipe_write_row) parses back to the same description, or the row is counted as not representable with its reason (merged-pass and readable-tile rows are
//   Metal only; a Metal format without an NVN source is listed as a finding); any other reason, or a representable row that does not round-trip, fails;
// - nvn_pipe_from_state against a model of the Metal backend's key builder, written from nvn_mtl.m (pso_key_fill, pso_key_norm, pso_skey_from), over pseudo-random draw states;
// - hand-made descriptions (a P row compared with its literal text), keys that are not representable, and malformed rows that must be refused.
// The repository is found from DM_REPO, else by walking up from the working directory and from the executable (dm/gfx/tests on the Mac, the CMake build folder on Linux and Windows)
#include "../nvn/nvn_pipe.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-100s %s\n", name, ok ? "ok" : "FAILED");
}

// ---------- The repository ----------
static int has_games(const char *dir) {
    char p[4096]; snprintf(p, sizeof p, "%s%sdm/games/README.md", dir, *dir ? "/" : "");
    FILE *f = fopen(p, "r"); if (f) fclose(f);
    return f != NULL;
}
static int find_repo(const char *argv0, char *out, size_t n) {
    const char *e = getenv("DM_REPO");
    if (e && *e) { snprintf(out, n, "%s", e); return has_games(out) ? 0 : -1; }
    char up[64] = "";   // "", "..", "../..", ...: from the working directory
    for (int i = 0; i < 8; i++) {
        snprintf(out, n, "%s", i ? up : ".");
        if (has_games(out)) return 0;
        strcat(up, i ? "/.." : "..");
    }
    char dir[4096]; snprintf(dir, sizeof dir, "%s", argv0 ? argv0 : "");   // from the executable's folder
    char *s1 = strrchr(dir, '/'), *s2 = strrchr(dir, '\\'), *s = s1 > s2 ? s1 : s2;
    if (!s) return -1;
    *s = 0;
    for (int i = 0; i < 8; i++) {
        if (has_games(dir)) { snprintf(out, n, "%s", dir); return 0; }
        if (strlen(dir) + 4 >= sizeof dir) break;
        strcat(dir, "/..");
    }
    return -1;
}

// ---------- The Metal backend's key, as nvn_mtl.m builds it ----------
// pso_key_fill (nvn_mtl.m): formats, sample count, blend enables, masks and alpha to coverage from S; S.blend[i] where the pass attaches a Metal format at i; for each attribute
// location a the vertex shader reads, below 16 and below S.nattr, S.attr[a] and the stream it names when that is below 16; then pso_key_norm for a normal pass. pso_get_ records
// pso_skey_from(&k, vs, fs, merged 0, no readable-tile description) for a normal pass
static void metal_key(pso_skey *out, const nvn_draw_state *s, const char *vsk, int vss, uint32_t vin, const char *fsk, int fss,
                      const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples) {
    pso_key k; memset(&k, 0, sizeof k);
    memcpy(k.cfmt, cfmt, sizeof k.cfmt); k.dfmt = dfmt; k.sfmt = sfmt; k.samples = samples;
    k.blend_en = s->color.blend_en; k.chmask = s->chmask.mask; k.a2c = s->ms.a2c;
    for (int i = 0; i < 8; i++) if (k.cfmt[i]) k.blend[i] = s->blend[i];
    for (int a = 0; a < 32; a++) {
        if (!(vin >> a & 1)) continue;
        if (a < 16 && a < s->nattr) { k.attr[a] = s->attr[a]; int st = s->attr[a].stream; if (st < 16) k.stream[st] = s->stream[st]; }
    }
    pso_key_norm(&k, 0);
    NvShader vs, fs; memset(&vs, 0, sizeof vs); memset(&fs, 0, sizeof fs);
    vs.stage = vss; memcpy(vs.key, vsk, 24); fs.stage = fss; if (fsk) memcpy(fs.key, fsk, 24);
    pso_skey_from(&k, &vs, fsk ? &fs : NULL, 0, NULL, out);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return n ? (uint32_t)(rng % n) : (uint32_t)rng; }

// ---------- Manifests ----------
typedef struct { char reason[160]; int n; char where[512]; } reason_n;
static reason_n reasons[32]; static int nreasons;
static void note_reason(const char *why, const char *profile) {
    for (int i = 0; i < nreasons; i++) if (!strcmp(reasons[i].reason, why)) {
        reasons[i].n++;
        if (!strstr(reasons[i].where, profile) && strlen(reasons[i].where) + strlen(profile) + 3 < sizeof reasons[i].where) { strcat(reasons[i].where, " "); strcat(reasons[i].where, profile); }
        return;
    }
    if (nreasons < 32) { snprintf(reasons[nreasons].reason, sizeof reasons[nreasons].reason, "%s", why); reasons[nreasons].n = 1; snprintf(reasons[nreasons].where, sizeof reasons[nreasons].where, "%s", profile); nreasons++; }
}
static int reason_expected(const char *why) {
    return !strcmp(why, "merged pass") || !strcmp(why, "readable-tile variant") || !strncmp(why, "Metal format ", 13);   // the last: "Metal format %u has no NVN source", a finding
}
static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }   // strdup is not C11
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static long rows_all, rows_ok, rows_nr, row_fail, guess_slots[16], p_max;
static void manifest(const char *path, const char *profile) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[4096], why[160]; long rows = 0, ok = 0, nr = 0, bad = 0; int nr0 = nreasons; int rcount0[32]; for (int i = 0; i < 32; i++) rcount0[i] = reasons[i].n;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'R') continue;
        rows++;
        line[strcspn(line, "\r\n")] = 0;
        pso_skey a; char ha[sizeof a * 2 + 1], hb[sizeof a * 2 + 1];
        if (pso_skey_parse(line + 2, &a)) { bad++; printf("  %s: R row not parsed: %.40s...\n", profile, line); continue; }
        pso_skey_hex(&a, ha);   // the row as pso_skey_parse normalises it
        nvn_pipe_desc d;
        if (nvn_pipe_from_key(&a, &d, why, sizeof why)) {
            nr++; note_reason(why, profile);
            if (!reason_expected(why)) { bad++; printf("  %s: unexpected reason \"%s\" for %.40s...\n", profile, why, line + 2); }
            nvn_pipe_desc d2; if (nvn_pipe_parse_row(line, &d2, NULL, 0) != -2) { bad++; printf("  %s: nvn_pipe_parse_row does not report the row as not representable\n", profile); }
            continue;
        }
        pso_skey b; int rc = nvn_pipe_to_key(&d, &b); pso_skey_hex(&b, hb);
        int good = rc == 0 && !strcmp(ha, hb);
        nvn_pipe_desc d2; good &= nvn_pipe_parse_row(line, &d2, NULL, 0) == 0 && !memcmp(&d, &d2, sizeof d);   // the row reader gives the same description
        char prow[NVN_PIPE_ROW_MAX]; int pl = nvn_pipe_write_row(&d, prow, sizeof prow);
        good &= pl > 0 && nvn_pipe_parse_row(prow, &d2, why, sizeof why) == 0 && !memcmp(&d, &d2, sizeof d);   // its P row reads back byte for byte
        if (pl > p_max) p_max = pl;
        for (int i = 0; i < 10; i++) if (d.fmt_guess >> i & 1) guess_slots[i]++;
        if (good) ok++;
        else { bad++; printf("  %s: row does not round-trip (to_key %d, P row %d): %.40s...\n", profile, rc, pl, line + 2); }
    }
    fclose(f);
    rows_all += rows; rows_ok += ok; rows_nr += nr; row_fail += bad;
    if (!rows) return;
    printf("%s: rows %ld, round-tripped %ld, not representable %ld", profile, rows, ok, nr);
    for (int i = 0; i < nreasons; i++) { int d = reasons[i].n - (i < nr0 ? rcount0[i] : 0); if (d) printf(" (%s: %d)", reasons[i].reason, d); }
    printf("%s\n", bad ? " FAILED" : "");
}

// ---------- Hand-made descriptions ----------
static void desc_basic(nvn_pipe_desc *d) {
    memset(d, 0, sizeof *d);
    strcpy(d->vs.key, "6180d0b6557e5f4c_b"); d->vs.stage = 0;
    strcpy(d->fs.key, "fcec5d913dc496e4_b"); d->fs.stage = 1;
    d->cfmt[0] = 0x25; d->cfmt[1] = 0x3F; d->dfmt = d->sfmt = 0x36; d->samples = 1; d->blend_en = 1; d->chmask = 0xff;
    d->blend[0].src_rgb = 6; d->blend[0].dst_rgb = 7; d->blend[0].src_a = 2; d->blend[0].dst_a = 1; d->blend[0].eq_rgb = 1; d->blend[0].eq_a = 1;
    d->attr[0].fmt = 0x2E; d->attr[0].offset = 0; d->attr[0].stream = 0; d->attr[1].fmt = 0x22; d->attr[1].offset = 12; d->attr[1].stream = 1;
    d->stream[0].stride = 12; d->stream[1].stride = 8; d->stream[1].divisor = 1;
}

int main(int argc, char **argv) {
    (void)argc;
    char why[160], row[NVN_PIPE_ROW_MAX], repo[4096];
    nvn_pipe_desc d, e; pso_skey k, k2;

    // ---------- Hand-made descriptions and their P rows ----------
    {
        memset(&d, 0, sizeof d); strcpy(d.vs.key, "abc_b");
        int n = nvn_pipe_write_row(&d, row, sizeof row);
        const char *want = "P\tv1\tvs=abc_b@0\tfs=-@0\tc=0,0,0,0,0,0,0,0\tds=0,0\tguess=0\tms=0,0\tblend_en=0\tchmask=0\tblend=-,-,-,-,-,-,-,-\t"
                           "attr=-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-\tstream=-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-\trast=-\tdss=-";
        t("a vertex-only description writes the documented P row, character for character", n == (int)strlen(want) && !strcmp(row, want));
        t("... and reads back to the same description", nvn_pipe_parse_row(row, &e, why, sizeof why) == 0 && !memcmp(&d, &e, sizeof d));
    }
    desc_basic(&d);
    {
        int n = nvn_pipe_write_row(&d, row, sizeof row);
        const char *want = "P\tv1\tvs=6180d0b6557e5f4c_b@0\tfs=fcec5d913dc496e4_b@1\tc=0x25,0x3f,0,0,0,0,0,0\tds=0x36,0x36\tguess=0\tms=1,0\tblend_en=0x1\tchmask=0xff\t"
                           "blend=0.6.7.2.1.1.1.0.0.0,-,-,-,-,-,-,-\tattr=46.0.0,34.12.1,-,-,-,-,-,-,-,-,-,-,-,-,-,-\tstream=12.0,8.1,-,-,-,-,-,-,-,-,-,-,-,-,-,-\trast=-\tdss=-";
        t("a G-buffer-like description writes the documented P row", n > 0 && !strcmp(row, want));
        if (strcmp(row, want)) printf("  got  %s\n  want %s\n", row, want);
        t("... and reads back byte for byte", nvn_pipe_parse_row(row, &e, NULL, 0) == 0 && !memcmp(&d, &e, sizeof d));
        t("a trailing CR LF is ignored", (strcat(row, "\r\n"), nvn_pipe_parse_row(row, &e, NULL, 0) == 0 && !memcmp(&d, &e, sizeof d)));
        t("to_key: RGBA8 -> RGBA8Unorm (70), R11G11B10F -> RG11B10Float (92), DEPTH32F_STENCIL8 -> Depth32Float_Stencil8 (260) in both slots",
          nvn_pipe_to_key(&d, &k) == 0 && k.k.cfmt[0] == 70 && k.k.cfmt[1] == 92 && k.k.cfmt[2] == 0 && k.k.dfmt == 260 && k.k.sfmt == 260);
        t("to_key: keys, stages, samples, enables and masks carried", !strcmp(k.vs, "6180d0b6557e5f4c_b") && !strcmp(k.fs, "fcec5d913dc496e4_b") && k.fs_stage == 1
          && k.k.samples == 1 && k.k.blend_en == 1 && k.k.chmask == 0xff && k.merged == 0 && k.fz == 0 && !k.k.vs && !k.k.fs && !k.k.fsfn);
        t("to_key: blend, attributes and streams carried", k.k.blend[0].src_rgb == 6 && k.k.blend[0].eq_a == 1 && k.k.attr[1].offset == 12 && k.k.stream[1].divisor == 1);
        d.fmt_guess = 1u | 1u << 8 | 1u << 9;   // RGBA8Unorm and Depth32Float_Stencil8 have two NVN sources each: the same formats come back, marked as guesses
        t("from_key(to_key(d)) == d, with the ambiguous formats marked", nvn_pipe_from_key(&k, &e, why, sizeof why) == 0 && !memcmp(&d, &e, sizeof d));
    }
    {   // normalisation: the key drops blend state of a target without blending, and enables / masks of absent targets, as pso_key_norm does
        desc_basic(&d); d.blend[3].src_rgb = 5; d.blend_en |= 0x30; d.chmask |= 0xf0000;
        nvn_pipe_to_key(&d, &k);
        t("to_key normalises: absent targets lose their enable and mask, a target without blending its factors",
          k.k.blend_en == 1 && k.k.chmask == 0xff && k.k.blend[3].src_rgb == 0);
    }
    {
        desc_basic(&d); d.cfmt[2] = 0x19;   // RGB8: Metal has no three-component format
        t("to_key refuses an attachment whose NVN format has no Metal format", nvn_pipe_to_key(&d, &k) == -1);
    }
    {   // the state beyond the key round-trips through the P row
        desc_basic(&d); d.xstate = 1; d.cull = 2; d.front = 1; d.poly_mode = 2; d.bias_en = 4; d.depth_clamp = 1; d.restart = 1; d.logic_op = 3; d.alpha_func = 7;
        d.ds.dt = 1; d.ds.dw = 1; d.ds.df = 4; d.ds.st = 1; d.ds.sf_f = 8; d.ds.sf_b = 8; d.ds.zpass_f = 3; d.ds.zpass_b = 3;
        int n = nvn_pipe_write_row(&d, row, sizeof row);
        t("the rasterizer and depth-stencil state are written", n > 0 && strstr(row, "\trast=2.1.2.4.1.0.1.3.7\tdss=1.1.4.1.8.8.0.0.3.0.0.3") != NULL);
        t("... and read back byte for byte", nvn_pipe_parse_row(row, &e, NULL, 0) == 0 && !memcmp(&d, &e, sizeof d));
        pso_skey a; nvn_pipe_to_key(&d, &a); desc_basic(&e); nvn_pipe_to_key(&e, &k);
        t("the state beyond the key does not change the Metal key", !memcmp(&a, &k, sizeof a));
    }
    {   // write refusals
        desc_basic(&d);
        t("write: a buffer too small is refused", nvn_pipe_write_row(&d, row, 40) == -1 && row[0] == 0);
        desc_basic(&d); strcpy(d.vs.key, "a b");
        t("write: a shader key with a space is refused", nvn_pipe_write_row(&d, row, sizeof row) == -1);
        desc_basic(&d); d.fs.key[20] = 'x';
        t("write: a shader key with bytes after its end is refused", nvn_pipe_write_row(&d, row, sizeof row) == -1);
        desc_basic(&d); d.rsv[0] = 1;
        t("write: a reserved byte set is refused", nvn_pipe_write_row(&d, row, sizeof row) == -1);
        desc_basic(&d); d.cull = 1;
        t("write: state beyond the key without xstate is refused", nvn_pipe_write_row(&d, row, sizeof row) == -1);
        desc_basic(&d); memset((char *)&d.blend[2] + 6, 0xc0, 2);
        t("write: blend bits outside NVN's fields are refused", nvn_pipe_write_row(&d, row, sizeof row) == -1);
    }

    // ---------- Keys that are not representable, and ambiguous formats ----------
    {
        desc_basic(&d); nvn_pipe_to_key(&d, &k);
        k2 = k; k2.merged = 1;
        t("from_key: a merged pass is not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "merged pass"));
        k2 = k; k2.k.lmap[3] = 2;
        t("from_key: a merged-pass map is not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "merged pass"));
        k2 = k; k2.fz = 1; k2.fz_desc.kind = 1;
        t("from_key: a readable-tile variant is not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "readable-tile variant"));
        k2 = k; k2.k.cfmt[4] = 1;   // A8Unorm: no NVN format gives it
        t("from_key: Metal format 1 has no NVN source", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "Metal format 1 has no NVN source"));
        t("... and the description is left zeroed", e.cfmt[0] == 0 && e.vs.key[0] == 0);
        k2 = k; k2.k.fsfn = (const void *)&k;
        t("from_key: a live key (pointers set) is not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "live pointers in the key"));
        k2 = k; k2.vs[22] = 'z';
        t("from_key: a shader key with bytes after its end is not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "shader key is not text"));
        k2 = k; k2.k.blend_en = 0x100;
        t("from_key: blend enables beyond 8 targets are out of range", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "field out of range"));
        k2 = k; ((uint8_t *)&k2.k.blend[0])[7] = 0x80;
        t("from_key: blend bits outside NVN's fields are not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "bits outside the NVN fields"));
        k2 = k; ((uint8_t *)&k2)[offsetof(pso_skey, k) - 1] = 1;
        t("from_key: a padding byte set is not representable", nvn_pipe_from_key(&k2, &e, why, sizeof why) == -1 && !strcmp(why, "bits outside the NVN fields"));
        k2 = k; k2.k.cfmt[0] = 80; k2.k.cfmt[1] = 0; k2.k.dfmt = 252; k2.k.sfmt = 0;
        t("from_key: BGRA8Unorm (80) and Depth32Float (252) decode to BGRA8 (0x9A) and DEPTH32F (0x34), marked as guesses",
          nvn_pipe_from_key(&k2, &e, why, sizeof why) == 0 && e.cfmt[0] == 0x9A && e.dfmt == 0x34 && e.sfmt == 0 && e.fmt_guess == (1u | 1u << 8));
        k2.k.cfmt[0] = 92; k2.k.dfmt = 250;
        t("from_key: RG11B10Float (92) and Depth16Unorm (250) have one source each, no guess",
          nvn_pipe_from_key(&k2, &e, why, sizeof why) == 0 && e.cfmt[0] == 0x3F && e.dfmt == 0x32 && e.fmt_guess == 0);
        k2.k.dfmt = 0; k2.k.sfmt = 253;
        t("from_key: a stencil-only pass keeps STENCIL8 (0x31) in the stencil slot only", nvn_pipe_from_key(&k2, &e, why, sizeof why) == 0 && e.dfmt == 0 && e.sfmt == 0x31);
    }

    // ---------- Malformed rows are refused; other rows are skipped ----------
    {
        desc_basic(&d); d.xstate = 1; char good[NVN_PIPE_ROW_MAX]; nvn_pipe_write_row(&d, good, sizeof good);
        struct { const char *what, *from, *to; } cases[] = {
            {"an unknown version", "P\tv1\t", "P\tv2\t"},
            {"no version", "P\tv1\t", "P\t"},
            {"a field name changed", "\tc=", "\tcolor="},
            {"a colour list one short", "c=0x25,0x3f,0,0,0,0,0,0", "c=0x25,0x3f,0,0,0,0,0"},
            {"a colour list one long", "c=0x25,0x3f,0,0,0,0,0,0", "c=0x25,0x3f,0,0,0,0,0,0,0"},
            {"a format beyond 16 bits", "c=0x25", "c=0x10025"},
            {"a negative number", "ms=1,0", "ms=-1,0"},
            {"a blend target beyond 3 bits", "blend=0.6.", "blend=8.6."},
            {"a blend entry one number short", "blend=0.6.7.2.1.1.1.0.0.0", "blend=0.6.7.2.1.1.1.0.0"},
            {"an attribute offset beyond 19 bits", "34.12.1", "34.524288.1"},
            {"an attribute stream beyond 5 bits", "34.12.1", "34.12.32"},
            {"a shader key with '='", "vs=6180", "vs=6=80"},
            {"a shader key of 25 characters", "vs=6180d0b6557e5f4c_b@", "vs=6180d0b6557e5f4c_b1234567@"},
            {"no stage", "fs=fcec5d913dc496e4_b@1", "fs=fcec5d913dc496e4_b"},
            {"a guess mask beyond 10 bits", "guess=0", "guess=0x400"},
            {"rasterizer state one number short", "rast=0.0.0.0.0.0.0.0.0", "rast=0.0.0.0.0.0.0.0"},
            {"depth-stencil state cut off", "\tdss=", "\tdss"},
            {"spaces for tabs", "\tms=", " ms="},
            {"hex digits after decimal ones", "ms=1,0", "ms=1f,0"},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            char bad[NVN_PIPE_ROW_MAX + 64]; const char *at = strstr(good, cases[i].from);
            if (!at) { char nm[160]; snprintf(nm, sizeof nm, "malformed P row (%s): the good row has its text", cases[i].what); t(nm, 0); continue; }
            snprintf(bad, sizeof bad, "%.*s%s%s", (int)(at - good), good, cases[i].to, at + strlen(cases[i].from));
            char nm[160]; snprintf(nm, sizeof nm, "malformed P row refused: %s", cases[i].what);
            t(nm, nvn_pipe_parse_row(bad, &e, why, sizeof why) == -1 && e.vs.key[0] == 0);
        }
        char bad[NVN_PIPE_ROW_MAX + 16];
        snprintf(bad, sizeof bad, "%s\tx=1", good);
        t("malformed P row refused: an extra field", nvn_pipe_parse_row(bad, &e, why, sizeof why) == -1);
        snprintf(bad, sizeof bad, "%s", good); *strstr(bad, "\trast=") = 0; strcat(bad, "\trast=-\tdss=1.1.4.1.8.8.0.0.3.0.0.3");
        t("malformed P row refused: rast=- with a depth-stencil state", nvn_pipe_parse_row(bad, &e, why, sizeof why) == -1);
        t("a v2 row is refused naming the version", (snprintf(bad, sizeof bad, "P\tv2\tvs=a@0"), nvn_pipe_parse_row(bad, &e, why, sizeof why) == -1 && strstr(why, "version 2") != NULL));
        t("malformed R row refused: too short", nvn_pipe_parse_row("R\t00ff", &e, why, sizeof why) == -1);
        memset(bad, '0', 864); bad[0] = 'R'; bad[1] = '\t'; memset(bad + 2, '0', 864); bad[866] = 0; bad[100] = 'g';
        t("malformed R row refused: a character that is not a hex digit", nvn_pipe_parse_row(bad, &e, why, sizeof why) == -1);
        bad[100] = '0';
        t("an all-zero R row is a pipeline without shaders or attachments", nvn_pipe_parse_row(bad, &e, why, sizeof why) == 0 && e.vs.key[0] == 0 && e.cfmt[0] == 0);
        t("an S row is not a pipeline row", nvn_pipe_parse_row("S\tfcec5d913dc496e4_b\t1\n", &e, why, sizeof why) == 1);
        t("a C row is not a pipeline row", nvn_pipe_parse_row("C\tabc_b\n", &e, why, sizeof why) == 1);
        t("a comment is not a pipeline row", nvn_pipe_parse_row("# Prewarm seed list\n", &e, why, sizeof why) == 1);
        t("a blank line is not a pipeline row", nvn_pipe_parse_row("\n", &e, why, sizeof why) == 1);
        t("no row at all is refused", nvn_pipe_parse_row(NULL, &e, why, sizeof why) == -1);
    }

    // ---------- nvn_pipe_from_state against the Metal backend's key builder ----------
    {
        static const uint16_t colour[] = {0, 0, 0, 0x25, 0x38, 0x3D, 0x3F, 0x29, 0x2C, 0x0A, 0x01, 0x05, 0x11, 0x16, 0x2E, 0x6D, 0x9A, 0x9C, 0x0B, 0x13, 0x2F};
        static const uint16_t depth[] = {0, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36};
        int same = 0, cases = 0, p_ok = 0, back = 0, xs = 0;
        for (int it = 0; it < 400; it++) {
            nvn_draw_state s; memset(&s, 0, sizeof s);
            s.color.blend_en = rnd(256); s.color.logic_op = rnd(16); s.color.alpha_func = rnd(9);
            s.chmask.mask = rnd(0); s.ms.a2c = (uint8_t)rnd(2);
            for (int i = 0; i < 8; i++) {
                blend_st *b = &s.blend[i];
                b->target = rnd(8); b->src_rgb = rnd(20); b->dst_rgb = rnd(20); b->src_a = rnd(20); b->dst_a = rnd(20); b->eq_rgb = rnd(6); b->eq_a = rnd(6);
                b->adv = rnd(8) ? 0 : rnd(64); b->adv_npm = rnd(2); b->adv_ovl = rnd(3);
            }
            s.nattr = (int)rnd(17); s.nstream = (int)rnd(17);
            for (int a = 0; a < 16; a++) { s.attr[a].fmt = rnd(0x9D); s.attr[a].offset = rnd(1u << 19); s.attr[a].stream = rnd(20); }
            for (int i = 0; i < 16; i++) { s.stream[i].stride = rnd(256); s.stream[i].divisor = rnd(4) ? 0 : rnd(5); }
            s.poly.cull = rnd(4); s.poly.front = rnd(2); s.poly.mode = rnd(3); s.poly.offset_en = rnd(8);
            s.ds.dt = rnd(2); s.ds.dw = rnd(2); s.ds.df = rnd(9); s.ds.st = rnd(2); s.ds.sf_f = rnd(9); s.ds.sf_b = rnd(9);
            s.ds.fail_f = rnd(9); s.ds.zfail_f = rnd(9); s.ds.zpass_f = rnd(9); s.ds.fail_b = rnd(9); s.ds.zfail_b = rnd(9); s.ds.zpass_b = rnd(9);
            s.depth_clamp = (int)rnd(2); s.rast_discard = (int)rnd(4) == 0; s.restart.on = rnd(2); s.restart.index = rnd(0);
            uint16_t cf[8]; uint32_t mcf[8];
            for (int i = 0; i < 8; i++) { cf[i] = i < (int)rnd(9) ? colour[rnd(sizeof colour / sizeof *colour)] : 0; mcf[i] = 0; if (cf[i]) nvn_fmt_to_mtl(cf[i], &mcf[i], NULL); }
            uint16_t df = depth[rnd(sizeof depth / sizeof *depth)];
            int has_d = df && df != 0x31, has_s = df == 0x31 || df == 0x35 || df == 0x36, mode = (int)rnd(4);   // the pass may attach one plane only (nvn_mtl.m rp_begin_ex_ modes 3 and 4)
            uint16_t dfa = has_d && mode != 3 ? df : 0, sfa = has_s && mode != 4 ? df : 0;
            uint32_t mdf = 0, msf = 0; if (dfa) nvn_fmt_to_mtl(dfa, &mdf, NULL); if (sfa) nvn_fmt_to_mtl(sfa, &msf, NULL);
            uint32_t samples = rnd(3) ? 1 : 2 + 2 * rnd(2), vin = rnd(0);
            char vk[24] = {0}, fk[24] = {0}; snprintf(vk, sizeof vk, "%016x_b", rnd(0)); snprintf(fk, sizeof fk, "%016x_b", rnd(0));
            int has_fs = rnd(8) != 0;
            pso_skey want, got; metal_key(&want, &s, vk, 0, vin, has_fs ? fk : NULL, 1, mcf, mdf, msf, samples);
            nvn_pipe_from_state(&d, &s, vk, 0, vin, has_fs ? fk : NULL, 1, cf, dfa, sfa, samples);
            nvn_pipe_to_key(&d, &got);
            cases++; same += !memcmp(&want, &got, sizeof want);
            if (memcmp(&want, &got, sizeof want) && cases - same <= 3) {
                char hw[sizeof want * 2 + 1], hg[sizeof got * 2 + 1]; pso_skey_hex(&want, hw); pso_skey_hex(&got, hg);
                for (size_t i = 0; i < strlen(hw); i++) if (hw[i] != hg[i]) { printf("  case %d differs at byte %zu\n", it, i / 2); break; }
            }
            int n = nvn_pipe_write_row(&d, row, sizeof row);
            p_ok += n > 0 && nvn_pipe_parse_row(row, &e, NULL, 0) == 0 && !memcmp(&d, &e, sizeof d);
            if (n > p_max) p_max = n;
            pso_skey again; back += nvn_pipe_from_key(&got, &e, NULL, 0) == 0 && nvn_pipe_to_key(&e, &again) == 0 && !memcmp(&again, &got, sizeof again);
            xs += d.xstate == 1 && d.cull == s.poly.cull && d.front == s.poly.front && d.poly_mode == s.poly.mode && d.bias_en == s.poly.offset_en
                  && d.depth_clamp == (s.depth_clamp != 0) && d.rast_discard == (s.rast_discard != 0) && d.restart == (s.restart.on != 0)
                  && d.logic_op == s.color.logic_op && d.alpha_func == s.color.alpha_func && d.ds.df == s.ds.df && d.ds.zpass_b == s.ds.zpass_b && d.ds.st == s.ds.st;
        }
        char nm[160];
        snprintf(nm, sizeof nm, "from_state -> to_key equals the Metal builder's key (%d / %d draw states)", same, cases); t(nm, same == cases);
        snprintf(nm, sizeof nm, "from_state's P rows read back byte for byte (%d / %d)", p_ok, cases); t(nm, p_ok == cases);
        snprintf(nm, sizeof nm, "its key decodes and encodes again to the same key (%d / %d)", back, cases); t(nm, back == cases);
        snprintf(nm, sizeof nm, "the state beyond the key comes from S (%d / %d)", xs, cases); t(nm, xs == cases);
        // Where the two differ by design: bits of a game's blend object outside NVN's fields (a state object copied in from the game's data) reach the Metal key but not the
        // description, which holds NVN's fields only. The Mac cross-check on a live game (listed in the design's M5 row) shows whether any game has such bits
        nvn_draw_state s; memset(&s, 0, sizeof s); s.color.blend_en = 1; s.chmask.mask = 0xf; s.blend[0].src_rgb = 2;
        ((uint8_t *)&s.blend[0])[7] = 0x40;
        uint16_t cf[8] = {0x25}; uint32_t mcf[8] = {70}; char vk[24] = "v_b", fk[24] = "f_b";
        pso_skey want, got; metal_key(&want, &s, vk, 0, 0, fk, 1, mcf, 0, 0, 1);
        nvn_pipe_from_state(&d, &s, vk, 0, 0, fk, 1, cf, 0, 0, 1); nvn_pipe_to_key(&d, &got);
        t("from_state keeps NVN's blend fields only: a stray high bit of the game's object is the only difference",
          memcmp(&want, &got, sizeof want) && (((uint8_t *)&want.k.blend[0])[7] = 0, !memcmp(&want, &got, sizeof want)));
        t("from_state: samples beyond 255 are refused", nvn_pipe_from_state(&d, &s, vk, 0, 0, NULL, 0, cf, 0, 0, 256) == -1);
    }

    // ---------- Every R row of every profile ----------
    int non_row_checks = total, non_row_fail = fail;
    if (find_repo(argv[0], repo, sizeof repo)) { t("the repository is found (set DM_REPO to its root)", 0); }
    else {
        printf("repository: %s\n", repo);
        char gd[4200]; snprintf(gd, sizeof gd, "%s/dm/games", repo);
        DIR *dir = opendir(gd); char *names[512]; int nn = 0;
        if (dir) { struct dirent *de; while ((de = readdir(dir)) && nn < 512) if (de->d_name[0] != '.') names[nn++] = dupstr(de->d_name); closedir(dir); }
        qsort(names, (size_t)nn, sizeof *names, cmp_str);
        int profiles = 0, empty = 0;
        for (int i = 0; i < nn; i++) {
            char p[4400]; snprintf(p, sizeof p, "%s/%s/pso_manifest.tsv", gd, names[i]);
            FILE *f = fopen(p, "r"); if (!f) { free(names[i]); continue; } fclose(f);
            profiles++; long before = rows_all;
            manifest(p, names[i]);
            empty += rows_all == before;
            free(names[i]);
        }
        printf("%d profiles with a pso_manifest.tsv, %d without R rows\n", profiles, empty);
        printf("not representable, by reason:%s\n", nreasons ? "" : " none");
        for (int i = 0; i < nreasons; i++) printf("  %s: %d (%s)\n", reasons[i].reason, reasons[i].n, reasons[i].where);
        printf("attachments decoded from an ambiguous Metal format (nvn_fmt_from_mtl's first candidate):");
        for (int i = 0; i < 10; i++) if (guess_slots[i]) printf(" %s%d: %ld", i < 8 ? "c" : i == 8 ? "depth" : "stencil", i < 8 ? i : 0, guess_slots[i]);
        printf("\nlongest P row: %ld bytes (NVN_PIPE_ROW_MAX %d)\n", p_max, NVN_PIPE_ROW_MAX);
        t("the manifests hold R rows", rows_all > 0);
        char nm[160]; snprintf(nm, sizeof nm, "every representable R row round-trips, every other one has an expected reason (%ld rows failed)", row_fail);
        t(nm, row_fail == 0);
    }
    printf("(%d checks before the manifests, %d failed)\n", non_row_checks, non_row_fail);
    printf("== t_pipe: %d checks, %d failed; R rows: %ld round-tripped, %ld not representable ==\n", total, fail, rows_ok, rows_nr);
    return fail != 0;
}
