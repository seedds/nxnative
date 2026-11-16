// GLSLC at run time. Fire Emblem: Three Houses (KTGL, SDK 6.5.0) compiles 9 built-in GLSL shaders at boot with the game's own runtime compiler (subsdk0, glslcCompile called through a function
// table from main+0x9c89f8; the sources are plain strings in main, e.g. "in vec3 ktglPosition0; …"). Their outputs are in no game file, and shaders are translated offline only, so the
// outputs are captured once on the Mac: DM_GLSLC_CAPTURE=<dir> writes every successful compile's GLSLC output (magic 0x19866891, the whole output's length at +0x44) to
// <dir>/<FNV-1a of the output>.glslc. shader_split.py reads that folder like any other source of GLSLC outputs (the NSP script makes it for a game whose main holds GLSL sources and imports glslcCompile).
// The compile object's first word points at its last results {GLSLCcompilationStatus *, GLSLCoutput *}; the status has "success" at +0xc and "allocation error" at +0xd, which is what the
// game itself reads after the call (main+0x9c8a04)
// Play: a game may also compile when a scene needs a shader its files do not hold. Catherine: Full Body loads each material's shaders from data/shaders_binary/<flags>.vs / .ps, a pre-compiled
// cache that lacks many combinations, and compiles the missing ones (21 when a save at Night 2 loads), past the boot the NSP script's capture runs. Without DM_GLSLC_CAPTURE, an output is saved to
// glslc_play/ beside the save (DM_SAVE's parent: the app's Documents/glslc_play, on the Mac beside save/, as pso_user.tsv; DM_GLSLC_PLAY=<dir> another folder, off none) when the shader map the
// renderer loads lacks the shader key of one of its stages (nvn_shkey.h, the key shader_split.py skey gives and nvn_mtl.m looks up; mtl_shmap_has). The key decides, not the output's file name:
// the split gives a play output a row only for a key no other source has, so a game that compiles a combination of stages its files hold already (Portal and Portal 2 combine shaders of
// game.zip) needs nothing, and saving it would only bring the same red line and the same file at every run. Each combination of stages with a missing key is saved and reported once a run. Its draws are missing in this
// build (shaders are translated offline only); the NSP script takes the folder into the next build (gather_glslc_play)
#include "hle.h"
#include "../gfx/nvn/nvn_shkey.h"
#include "../gfx/nvn/nvn_mtl.h"
#include <sys/stat.h>
#include <unistd.h>

// The stages of one whole GLSLC output read as shader_split.py glslc_shaders reads an NVN output (NVN2 outputs carry no length at +0x44, so capture() takes none of them): one control section
// as the output's section header says (glslc_section), else with its code area right after it (control +0x20 = control length); several stages compiled as one program are read by their section
// headers (glslc_program). A stage the split would not keep (glslc_stage, check) is not keyed here either. Returns the number of keys, 0 when the split would read none
#define GLSLC_MAX_STAGES 16
typedef struct { const uint8_t *ctl, *code; uint32_t nctl, ncode; } glslc_sec;
static const uint8_t CTL_MAGIC[4] = {0x34, 0x12, 0x76, 0x98}, GFX_MAGIC[4] = {0x78, 0x56, 0x34, 0x12}, CS_MAGIC[4] = {0x79, 0x56, 0x34, 0x12};
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int glslc_stage(const glslc_sec *s) {   // 1..5 vertex, hull, domain, geometry, pixel by the SPH; 6 compute; 0 none (shader_split.py glslc_stage, sph_stage)
    if (s->nctl >= NVN2_KEY_CTL && nvn_shkey_nvn2(s->ctl)) return 0;   // an NVN2 section: not one of this reader's
    int gfx = s->ncode >= 4 && !memcmp(s->code, GFX_MAGIC, 4), cs = s->ncode >= 4 && !memcmp(s->code, CS_MAGIC, 4);
    if (cs || (!gfx && s->nctl >= 0x754 && rd32(s->ctl + 0x748) && rd32(s->ctl + 0x74c) && rd32(s->ctl + 0x750))) return 6;   // compute: its header, or none and a thread group (SDK 6)
    if (!gfx || s->ncode < 0x34) return 0;
    uint32_t w = rd32(s->code + 0x30), t = w >> 10 & 0xf;
    return t == 0 && (w & 0x1f) == 1 ? 1 : t >= 1 && t <= 5 ? (int)t : 0;   // Catherine's GLSLC writes type 0 in its vertex shaders
}
static int glslc_check(const glslc_sec *s, int st) {   // shader_split.py check: control and code consistent
    if (s->nctl < 0x760 || memcmp(s->ctl, CTL_MAGIC, 4)) return 0;   // shorter than any key's control bytes (ctl_len: 0x760 at least)
    uint32_t nkey, ncode; nvn_shkey_lens(s->ctl, &nkey, &ncode);
    if (s->nctl < nkey) return 0;
    int gfx = s->ncode >= 4 && !memcmp(s->code, GFX_MAGIC, 4);
    if (st == 6 ? gfx : !gfx) return 0;
    return ncode == s->ncode;
}
static int glslc_sections(const uint8_t *d, uint32_t size, glslc_sec *out) {   // GPU code sections by the section headers; -1: headers that do not fit (shader_split.py glslc_section)
    uint32_t n = size >= 0x50 ? rd32(d + 0x4c) : 0; int k = 0;
    if (!n || n > GLSLC_MAX_STAGES || 0x90 + 0x90ull * n > size) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *s = d + 0x90 + 0x90 * i;
        uint32_t ssz = rd32(s), soff = rd32(s + 4), co = rd32(s + 0x30), ko = rd32(s + 0x34), kn = rd32(s + 0x38), cn = rd32(s + 0x3c);
        if (rd32(s + 8)) continue;   // not GPU code
        if ((uint64_t)soff + ssz > size || (uint64_t)co + cn > ssz || (uint64_t)ko + kn > ssz) return -1;
        out[k++] = (glslc_sec){d + soff + co, d + soff + ko, cn, kn};
    }
    return k;
}
static int glslc_keys(const uint8_t *d, uint32_t size, uint8_t keys[GLSLC_MAX_STAGES][8]) {
    glslc_sec sec[GLSLC_MAX_STAGES]; int n = 0;
    const uint8_t *c = size > 0x760 ? memmem(d, size, CTL_MAGIC, 4) : NULL;
    if (!c) return 0;
    uint32_t co = (uint32_t)(c - d);
    if (memmem(c + 4, size - co - 4, CTL_MAGIC, 4)) {   // several stages
        int k = glslc_sections(d, size, sec); unsigned seen = 0;
        if (k < 2) return 0;
        for (int i = 0; i < k; i++) { int st = glslc_stage(&sec[i]); if (!st || seen >> st & 1 || !glslc_check(&sec[i], st)) return 0; seen |= 1u << st; }
        n = k;
    } else {
        // The output's own section header first, as shader_split.py glslc_shaders reads it (SDK 4's control lengths are short, SDK 21's are long); without a usable one, the code area right after
        // the control section's +0x20 length
        int st = 0;
        if (glslc_sections(d, size, sec) != 1 || !(st = glslc_stage(&sec[0])) || !glslc_check(&sec[0], st)) {
            uint32_t nctl = 0, ncode = 0;
            if (co + 0x708 <= size) { nctl = rd32(c + 0x20); ncode = rd32(c + 0x704); }
            sec[0] = (glslc_sec){c, c + nctl, nctl, ncode};
            st = nctl >= 0x760 && !(nctl & 3) && (uint64_t)co + nctl + ncode <= size ? glslc_stage(&sec[0]) : 0;
            if (!st || !glslc_check(&sec[0], st)) return 0;
        }
        n = 1;
    }
    for (int i = 0; i < n; i++) { uint32_t nkey, ncode; nvn_shkey_lens(sec[i].ctl, &nkey, &ncode); nvn_shkey(sec[i].ctl, nkey, sec[i].code, sec[i].ncode, keys[i]); }
    return n;
}

static int save(const char *dir, uint64_t h, const uint8_t *out, uint32_t n, char *path, size_t np) {   // 1: written now, 0: there already, -1: failed
    snprintf(path, np, "%s/%016llx.glslc", dir, (unsigned long long)h);
    struct stat st; if (!stat(path, &st)) return 0;
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.partial", path);   // a whole file or none: the NSP script takes only whole outputs
    FILE *f = fopen(tmp, "wb"); if (!f) { LOG("[RED] glslc: cannot write %s", tmp); return -1; }
    int ok = fwrite(out, 1, n, f) == n; ok &= !fclose(f);
    if (!ok || rename(tmp, path)) { LOG("[RED] glslc: cannot write %s", path); unlink(tmp); return -1; }
    return 1;
}

static const char *play_dir(void) {   // glslc_play/ beside the save, made at the first output it gets; NULL: off
    static char dir[1024]; static int init;
    if (!init) {
        init = 1; const char *e = getenv("DM_GLSLC_PLAY"), *s = getenv("DM_SAVE");
        if (e && (!strcmp(e, "off") || !strcmp(e, "0") || !*e)) dir[0] = 0;
        else if (e) snprintf(dir, sizeof dir, "%s", e);
        else if (s && *s) { snprintf(dir, sizeof dir, "%s", s); size_t n = strlen(dir); while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
            char *sl = strrchr(dir, '/'); if (sl) snprintf(sl + 1, sizeof dir - (size_t)(sl + 1 - dir), "glslc_play"); else snprintf(dir, sizeof dir, "glslc_play"); }
    }
    return dir[0] ? dir : NULL;
}

// The stage combinations this run has saved and reported (all of an output's stage keys, as one hash), sorted: the same shader compiled again (another material with the same stages, a scene
// loaded again, an output that differs only outside its stages) is neither saved nor reported again. A program that pairs a missing stage with other stages than an output saved before is saved
// as well: the split takes its variation pair (var_pairs: shader_pairs.tsv) only from an output it reads, and dmsh_build pads the vertex shader for the pixel shaders paired with it (vs_pad)
static uint64_t *g_run; static size_t g_nrun, g_caprun;
static int u64_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int run_add(uint64_t k) {   // 1: new this run
    if (bsearch(&k, g_run, g_nrun, sizeof k, u64_cmp)) return 0;
    if (g_nrun == g_caprun) { uint64_t *r = realloc(g_run, (g_caprun = g_caprun ? g_caprun * 2 : 64) * sizeof *r); if (!r) return 1; g_run = r; }
    size_t i = g_nrun; while (i && g_run[i - 1] > k) { g_run[i] = g_run[i - 1]; i--; }
    g_run[i] = k; g_nrun++;
    return 1;
}

static void capture(const void *obj) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_GLSLC_CAPTURE"); if (dir) { mkdir(dir, 0755); LOG("glslc: run-time compile outputs are saved to %s", dir); } }
    const uint8_t *const *res = *(const uint8_t *const *const *)obj;
    if (!res || !res[0] || !res[1] || !res[0][0xc] || res[0][0xd]) return;
    const uint8_t *out = res[1]; uint32_t n = *(const uint32_t *)(out + 0x44);
    if (*(const uint32_t *)out != 0x19866891 || n < 0x48 || n > (64u << 20)) { LOG("[RED] glslc: unknown output header (magic %#x length %#x), not saved", *(const uint32_t *)out, n); return; }
    uint64_t h = 0xcbf29ce484222325ull; for (uint32_t i = 0; i < n; i++) h = (h ^ out[i]) * 0x100000001b3ull;
    char path[1024];
    if (dir) {   // the NSP script's capture run: every output
        if (save(dir, h, out, n, path, sizeof path) > 0) LOG("glslc: saved a run-time compile output %s (%u bytes)", path, n);
        return;
    }
    uint8_t keys[GLSLC_MAX_STAGES][8]; int nk = glslc_keys(out, n, keys), nmiss = 0; char miss[17] = ""; uint64_t combo = 0xcbf29ce484222325ull;
    for (int i = 0; i < nk; i++) {
        for (int b = 0; b < 8; b++) combo = (combo ^ keys[i][b]) * 0x100000001b3ull;
        char hex[17]; for (int b = 0; b < 8; b++) snprintf(hex + 2 * b, 3, "%02x", keys[i][b]);
        if (mtl_shmap_has(hex) == 1) continue;   // in this build's shader tables (no map at all: every key is missing)
        if (!nmiss) memcpy(miss, hex, sizeof hex);
        nmiss++;
    }
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static unsigned seen, unread;   // the game may compile on several threads
    if (!nk) {   // the split would read no shader out of it either: saved, it would only stop the next build's split
        pthread_mutex_lock(&mu); unsigned u = run_add(h) ? ++unread : 0; pthread_mutex_unlock(&mu);
        if (u == 1) LOG("[RED] glslc: the game compiled an output whose stages this host cannot read (output %016llx, %u bytes): not saved, its draws may be missing", (unsigned long long)h, n);
        return;
    }
    if (!nmiss) return;   // every stage in the tables: a shader the game files hold, or one an earlier play capture brought in
    pthread_mutex_lock(&mu);
    if (!run_add(combo)) { pthread_mutex_unlock(&mu); return; }   // these stages were saved and reported this run already
    const char *pd = play_dir(); int w = 0;
    if (pd) { mkdir(pd, 0755); w = save(pd, h, out, n, path, sizeof path); }
    unsigned k = ++seen;
    pthread_mutex_unlock(&mu);
    if (k == 1) LOG("[RED] glslc: the game compiled a shader that this build's shader map does not have (key %s, %d of the output's %d stages; output %016llx, %u bytes): its draws are missing "
                    "(shaders are translated offline only). %s", miss, nmiss, nk, (unsigned long long)h, n,
                    pd ? "Such outputs are saved to glslc_play/ beside the save: put that folder into the NSP folder and build the app again" : "DM_GLSLC_PLAY=off: not saved");
    else if (k <= 64) LOG("glslc: shader %s is not in the shader map either (output %016llx, %u so far this run)%s", miss, (unsigned long long)h, k, w > 0 ? ", saved" : "");
    else if (k == 65) LOG("glslc: more outputs not in the shader map are saved without a line");
}

// The GPU code version the game's own GLSLC writes (glslcGetVersion returns GLSLCversion {apiMajor, apiMinor, gpuCodeVersionMajor, gpuCodeVersionMinor, package, …}, 0x34 bytes, through x8),
// 0 when the game carries no GLSLC. nvnDeviceGetInteger reports at least this as the driver's newest supported GPU code version (hle_nvn.c): a game's driver, in the same SDK, runs that code
int dm_glslc_gpu_code(int *major, int *minor) {
    static int v[2] = {-1, -1};
    if (v[0] < 0) {
        typedef struct { uint32_t w[16]; } glslc_version_t;
        glslc_version_t (*get)(void) = (glslc_version_t (*)(void))guest_sym("glslcGetVersion");
        v[0] = v[1] = 0;
        if (get) { glslc_version_t r = get(); v[0] = (int)r.w[2]; v[1] = (int)r.w[3]; LOG("glslc: GLSLC API %u.%u, GPU code version %d.%d", r.w[0], r.w[1], v[0], v[1]); }
    }
    *major = v[0]; *minor = v[1];
    return v[0] > 0;
}

HLE_FORCE("glslcCompile", "run-time GLSL compile (the game's own GLSLC compiles as usual; with DM_GLSLC_CAPTURE the outputs are saved to disk for offline translation, without it those with a stage the shader map lacks go to glslc_play/)", uint8_t, h_glslc_Compile, void *obj) {
    static uint8_t (*orig)(void *);
    if (!orig && !(orig = guest_sym("glslcCompile"))) die("glslcCompile missing");
    uint8_t ok = orig(obj);
    if (ok) capture(obj);   // DM_GLSLC_CAPTURE: every output; else those with a stage whose key the shader map lacks, to glslc_play/
    return ok;
}
