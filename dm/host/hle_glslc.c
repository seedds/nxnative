// GLSLC at run time. Fire Emblem: Three Houses (KTGL, SDK 6.5.0) compiles 9 built-in GLSL shaders at boot with the game's own runtime compiler (subsdk0, glslcCompile called through a function
// table from main+0x9c89f8; the sources are plain strings in main, e.g. "in vec3 ktglPosition0; …"). Their outputs are in no game file, and shaders are translated offline only, so the
// outputs are captured once on the Mac: DM_GLSLC_CAPTURE=<dir> writes every successful compile's GLSLC output (magic 0x19866891, the whole output's length at +0x44) to
// <dir>/<FNV-1a of the output>.glslc. shader_split.py reads that folder like any other source of GLSLC outputs (the NSP script makes it for a game whose main holds GLSL sources and imports glslcCompile).
// The same switch can capture shaders compiled after boot. The NSP script takes that folder with --glslc-play.
// The compile object's first word points at its last results {GLSLCcompilationStatus *, GLSLCoutput *}; the status has "success" at +0xc and "allocation error" at +0xd, which is what the
// game itself reads after the call (main+0x9c8a04)
#include "hle.h"
#include <sys/stat.h>
#include <unistd.h>

static const char *capture_dir;
static uint8_t (*glslc_compile)(void *);
static void glslc_init(void) {
    if (!(glslc_compile = guest_sym("glslcCompile"))) die("glslcCompile missing");
    const char *dir = getenv("DM_GLSLC_CAPTURE");
    if (dir && *dir) {
        capture_dir = dir;
        LOG("glslc: run-time compile outputs are saved to %s", dir);
    }
}

static void capture(const void *obj) {
    const uint8_t *const *res = *(const uint8_t *const *const *)obj;
    if (!res || !res[0] || !res[1] || !res[0][0xc] || res[0][0xd]) return;
    const uint8_t *out = res[1]; uint32_t n = *(const uint32_t *)(out + 0x44);
    if (*(const uint32_t *)out != 0x19866891 || n < 0x48 || n > (64u << 20)) { LOG("[RED] glslc: unknown output header (magic %#x length %#x), not saved", *(const uint32_t *)out, n); return; }
    uint64_t h = 0xcbf29ce484222325ull; for (uint32_t i = 0; i < n; i++) h = (h ^ out[i]) * 0x100000001b3ull;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%016llx.glslc", capture_dir, (unsigned long long)h) >= (int)sizeof path) {
        LOG("[RED] glslc: capture path is too long");
        return;
    }
    struct stat st; if (!stat(path, &st)) return;
    mkdir(capture_dir, 0755);
    // A capture can run on several game threads. Give each writer its own file and publish it only once it is complete.
    char tmp[sizeof path + sizeof ".partial.XXXXXX"];
    snprintf(tmp, sizeof tmp, "%s.partial.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { LOG("[RED] glslc: cannot write %s", path); return; }
    FILE *f = fdopen(fd, "wb");
    int ok = 0;
    if (f) {
        ok = fwrite(out, 1, n, f) == n;
        if (fclose(f)) ok = 0;
    } else close(fd);
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        LOG("[RED] glslc: cannot write %s", path);
        return;
    }
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
        if (get) { glslc_version_t r = get(); v[0] = (int)r.w[2]; v[1] = (int)r.w[3]; LOG("glslc: GLSLC API %u.%u, GPU code version %d.%d", r.w[0], r.w[1], v[0], v[1]); }
    }
    *major = v[0]; *minor = v[1];
    return v[0] > 0;
}

HLE_FORCE("glslcCompile", "run-time GLSL compile (the game's own GLSLC compiles as usual; with DM_GLSLC_CAPTURE the outputs are saved to disk for offline translation)", uint8_t, h_glslc_Compile, void *obj) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, glslc_init);
    uint8_t ok = glslc_compile(obj);
    if (ok && capture_dir) capture(obj);
    return ok;
}
