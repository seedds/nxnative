// GLSLC at run time. Fire Emblem: Three Houses (KTGL, SDK 6.5.0) compiles 9 built-in GLSL shaders at boot with the game's own runtime compiler (subsdk0, glslcCompile called through a function
// table from main+0x9c89f8; the sources are plain strings in main, e.g. "in vec3 ktglPosition0; …"). Their outputs are in no game file, and shaders are translated offline only, so the
// outputs are captured once on the Mac: DM_GLSLC_CAPTURE=<dir> writes every successful compile's GLSLC output (magic 0x19866891, the whole output's length at +0x44) to
// <dir>/<FNV-1a of the output>.glslc. shader_split.py reads that folder like any other source of GLSLC outputs (the NSP script makes it for a game whose main holds GLSL sources and imports glslcCompile).
// The compile object's first word points at its last results {GLSLCcompilationStatus *, GLSLCoutput *}; the status has "success" at +0xc and "allocation error" at +0xd, which is what the
// game itself reads after the call (main+0x9c8a04)
#include "hle.h"
// A guest function this file calls (docs/design/aot-recompiler.md 2.4): the plain call where the guest's code runs natively (Apple), or where a unit test supplies it
// as a C function (DM_TEST_GUEST_C); on the x86-64 engine a guest call on the calling thread (aot.h DM_GUEST_CALL: nested inside the host call it is answering)
#if DM_NATIVE_EXEC || defined(DM_TEST_GUEST_C)
#define GUEST_CALL(f, ...) f(__VA_ARGS__)
#else
#include "aot.h"
#define GUEST_CALL DM_GUEST_CALL
#endif
#include <sys/stat.h>

static void capture(const void *obj) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_GLSLC_CAPTURE"); if (dir) { mkdir(dir, 0755); LOG("glslc: run-time compile outputs are saved to %s", dir); } }
    if (!dir) return;
    const uint8_t *const *res = *(const uint8_t *const *const *)obj;
    if (!res || !res[0] || !res[1] || !res[0][0xc] || res[0][0xd]) return;
    const uint8_t *out = res[1]; uint32_t n = *(const uint32_t *)(out + 0x44);
    if (*(const uint32_t *)out != 0x19866891 || n < 0x48 || n > (64u << 20)) { LOG("[RED] glslc: unknown output header (magic %#x length %#x), not saved", *(const uint32_t *)out, n); return; }
    uint64_t h = 0xcbf29ce484222325ull; for (uint32_t i = 0; i < n; i++) h = (h ^ out[i]) * 0x100000001b3ull;
    char path[1024]; snprintf(path, sizeof path, "%s/%016llx.glslc", dir, (unsigned long long)h);
    struct stat st; if (!stat(path, &st)) return;
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] glslc: cannot write %s", path); return; }
    fwrite(out, 1, n, f); fclose(f);
    LOG("glslc: saved a run-time compile output %s (%u bytes)", path, n);
}

// The GPU code version the game's own GLSLC writes (glslcGetVersion returns GLSLCversion {apiMajor, apiMinor, gpuCodeVersionMajor, gpuCodeVersionMinor, package, …}, 0x34 bytes, through x8),
// 0 when the game carries no GLSLC. nvnDeviceGetInteger reports at least this as the driver's newest supported GPU code version (hle_nvn.c): a game's driver, in the same SDK, runs that code
int dm_glslc_gpu_code(int *major, int *minor) {
    static int v[2] = {-1, -1};
    if (v[0] < 0) {
        typedef struct { uint32_t w[16]; } glslc_version_t;
        glslc_version_t (*get)(void) = (glslc_version_t (*)(void))guest_sym("glslcGetVersion");
        v[0] = v[1] = 0;
#if DM_NATIVE_EXEC || defined(DM_TEST_GUEST_C)
        if (get) { glslc_version_t r = get(); v[0] = (int)r.w[2]; v[1] = (int)r.w[3]; LOG("glslc: GLSLC API %u.%u, GPU code version %d.%d", r.w[0], r.w[1], v[0], v[1]); }
#else   // the struct comes back through x8, an indirect result address (AAPCS64): guest memory for it, x8 set before the guest call, which leaves it as it is
        if (get) {
            glslc_version_t *rp = dm_guest_alloc(sizeof *rp);   // zero: a call that left reads as version 0
            dm_aot_host_thread_begin((uint64_t)(uintptr_t)get)->x[8] = (uint64_t)(uintptr_t)rp;   // the calling thread's block (made for a host thread, as dm_gcall makes it)
            dm_gcall((const void *)get, 0);
            glslc_version_t r = *rp; dm_guest_free(rp, sizeof *rp);
            v[0] = (int)r.w[2]; v[1] = (int)r.w[3]; LOG("glslc: GLSLC API %u.%u, GPU code version %d.%d", r.w[0], r.w[1], v[0], v[1]);
        }
#endif
    }
    *major = v[0]; *minor = v[1];
    return v[0] > 0;
}

HLE_FORCE("glslcCompile", "run-time GLSL compile (the game's own GLSLC compiles as usual; with DM_GLSLC_CAPTURE the outputs are saved to disk for offline translation)", uint8_t, h_glslc_Compile, void *obj) {
    static uint8_t (*orig)(void *);
    if (!orig && !(orig = guest_sym("glslcCompile"))) die("glslcCompile missing");
    uint8_t ok = GUEST_CALL(orig, obj);
    if (ok) capture(obj);
    return ok;
}
