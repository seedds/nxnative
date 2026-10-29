// What the Vulkan backend's GPU tests share (docs/design/vulkan-backend.md 10 items 2 and 3): each test drives the real NVN frontend (its entry points as C functions, h_<name>,
// as dm/gfx/tests/t_frontend.c does, and command records it emits itself, as the Metal tests do) over the Vulkan backend (dm/gfx/hostvk), headless, and compares pixels it reads
// back with expected values. Provided here:
// - the host functions the frontend calls, which the tests do not link (the stubs of t_frontend.c), and the front end's Vulkan query (no window);
// - the page-fault hook of the dirty-page tracker (dm/gfx/nvn/nvn_dirty.c) wired to SIGSEGV, as the host's crash handler does on Apple: an upload write-protects a texture's storage;
// - the device rule of dm/gfx/hostvk/tests/t_vk_common.h: no Vulkan at all skips (exit 0), unless VK_ICD_FILENAMES / VK_DRIVER_FILES / VK_LOADER_DRIVERS_SELECT names a driver
//   (CTest on Linux sets lavapipe): then it fails; the validation layer is asked for (DM_VK_VALIDATION=1) with synchronization validation, must be on when VK_INSTANCE_LAYERS names
//   it, and every message it counts fails the test;
// - a shader library in a temporary folder as the backend reads one (DM_MSL_DIR/vk-<DMSH_CHAIN_VK>/<key>.spv and .json, the shader map DM_NVN_SHMAP): a test's GLSL, compiled by
//   dm/gfx/hostvk/shaders/gen_spirv.sh into committed arrays, with a hand-written reflection in the Vulkan form dmsh_build_vk.py writes, under the key the frontend computes for a
//   program's shader data (prog_key), so a draw loads its shaders the way a game's do;
// - records (emit), submissions through nvnQueueSubmitCommands, and readbacks of a texture's subresource through the backend's own image (vk_texture.c hvk_test_image);
// - the pipeline switches of phase P5 kept inside the test: the disk cache in the test's temporary folder (DM_VK_PSO_CACHE) and no prewarm list (DM_PSO_MANIFEST=off), unless the
//   test sets them first; and a second start of the backend, for the tests of the disk cache and the prewarm: the test runs itself again as a child process with a phase argument
//   (hv_spawn), which reports its numbers through a results file (hv_res_put / hv_res_get).
#pragma once
#include "../../nvn/nvn_obj.h"
#include "../../nvn/nvn_protos.h"
#include "../../nvn/nvn_pools.h"
#include "../../nvn/nvn_state.h"
#include "../../nvn/nvn_be.h"
#include "../../nvn/dmsh_chain_vk.h"
#include "../../../host/dm.h"
#include "vk_device.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#else
#include <process.h>
#endif

// ---------- The entry points the tests call (each NVNT_<name> of nvn_protos.h is the type hle_nvn.c checks its h_<name> against) ----------
#define DECL(name) extern __typeof__(*(NVNT_##name)0) h_##name
DECL(nvnDeviceBuilderSetDefaults); DECL(nvnDeviceBuilderSetFlags); DECL(nvnDeviceInitialize); DECL(nvnDeviceFinalize); DECL(nvnDeviceSetWindowOriginMode); DECL(nvnDeviceSetDepthMode);
DECL(nvnQueueBuilderSetDevice); DECL(nvnQueueBuilderSetDefaults); DECL(nvnQueueInitialize); DECL(nvnQueueSubmitCommands); DECL(nvnQueueFinish); DECL(nvnQueueFinalize);
DECL(nvnQueueFenceSync); DECL(nvnSyncInitialize); DECL(nvnSyncWait); DECL(nvnQueueFlush);
DECL(nvnMemoryPoolBuilderSetDefaults); DECL(nvnMemoryPoolBuilderSetDevice); DECL(nvnMemoryPoolBuilderSetStorage); DECL(nvnMemoryPoolBuilderSetFlags);
DECL(nvnMemoryPoolInitialize); DECL(nvnMemoryPoolFinalize);
DECL(nvnProgramInitialize); DECL(nvnProgramSetShaders); DECL(nvnProgramFinalize);
DECL(nvnTextureBuilderSetDefaults); DECL(nvnTextureBuilderSetDevice); DECL(nvnTextureBuilderSetTarget); DECL(nvnTextureBuilderSetFormat); DECL(nvnTextureBuilderSetSize2D);
DECL(nvnTextureBuilderSetWidth); DECL(nvnTextureBuilderSetHeight); DECL(nvnTextureBuilderSetDepth); DECL(nvnTextureBuilderSetLevels); DECL(nvnTextureBuilderSetFlags);
DECL(nvnTextureBuilderSetStorage); DECL(nvnTextureBuilderSetPackagedTextureLayout); DECL(nvnTextureBuilderSetSwizzle); DECL(nvnTextureBuilderGetStorageSize);
DECL(nvnTextureInitialize); DECL(nvnTextureFinalize); DECL(nvnTextureReadTexels); DECL(nvnTextureWriteTexels);
DECL(nvnTexturePoolInitialize); DECL(nvnTexturePoolRegisterTexture); DECL(nvnTexturePoolRegisterImage); DECL(nvnSamplerPoolInitialize); DECL(nvnSamplerPoolRegisterSampler);
DECL(nvnSamplerBuilderSetDefaults); DECL(nvnSamplerInitialize); DECL(nvnSamplerBuilderSetMinMagFilter); DECL(nvnSamplerBuilderSetWrapMode); DECL(nvnSamplerBuilderSetBorderColor);
DECL(nvnCommandBufferInitialize); DECL(nvnCommandBufferAddCommandMemory); DECL(nvnCommandBufferBeginRecording); DECL(nvnCommandBufferEndRecording); DECL(nvnCommandBufferFinalize);
DECL(nvnCommandBufferSetRenderTargets); DECL(nvnCommandBufferClearColor); DECL(nvnCommandBufferClearDepthStencil); DECL(nvnCommandBufferClearTexture);
DECL(nvnCommandBufferCopyTextureToBuffer); DECL(nvnCommandBufferCopyBufferToTexture); DECL(nvnCommandBufferCopyTextureToTexture); DECL(nvnCommandBufferCopyBufferToBuffer);
DECL(nvnCommandBufferSetCopyRowStride); DECL(nvnCommandBufferFenceSync);
#undef DECL
void *h_nvnBootstrapLoader(const char *name);
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]);   // hle_nvn.c

// ---------- What the frontend needs of the host (t_frontend.c's stubs) ----------
uint64_t g_t0_ns;
uint64_t now_ns(void) { return plat_now_ns(); }
uint64_t guest_ns(void) { return plat_now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
int front_screen_max_fps(void) { return 60; }
int front_vk_instance_extensions(const char **names, unsigned *n) { (void)names; if (n) *n = 0; return 0; }   // no window
static int (*g_fault_hook)(void *, int, void *);
static void (*g_prep_hook)(void *, size_t);
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep_hook = fn; }
void dm_guest_write_prepare(void *addr, size_t len) { if (g_prep_hook) g_prep_hook(addr, len); }   // the backend calls it before it writes guest memory from the CPU
#if !defined(_WIN32)
static struct sigaction g_old_segv;
static void on_segv(int sig, siginfo_t *si, void *uc) {   // a write to a page the dirty tracker protected: it unprotects and stamps it, and the write runs again
    if (g_fault_hook && g_fault_hook(si->si_addr, 1, uc)) return;
    sigaction(SIGSEGV, &g_old_segv, NULL); (void)sig;   // not the tracker's: crash as before
}
void dm_fault_hook_register(int (*fn)(void *, int, void *)) {
    g_fault_hook = fn;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
}
#else
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_fault_hook = fn; }   // Windows: the tests skip without a Vulkan driver before any upload (a vectored handler is the CPU port's)
#endif

// The backend's test hooks (dm/gfx/hostvk): its device, a full shutdown, and the image behind a texture object for readbacks
const hostvk_dev *hvk_device(void);
void hvk_shutdown(void);
VkImage hvk_test_image(const tex_t *t, VkFormat *fmt, VkImageAspectFlags *aspect);
int hvk_oneoff_begin(VkCommandBuffer *cb);
int hvk_oneoff_end(VkCommandBuffer cb);
VkResult hvk_buffer_make(VkDeviceSize n, VkBufferUsageFlags usage, int host_visible, VkBuffer *buf, VkDeviceMemory *mem, void **map);
void hvk_buffer_free(VkBuffer buf, VkDeviceMemory mem);
void hvk_full_barrier(VkCommandBuffer cb);

// ---------- Checks ----------
static int g_fail, g_total, g_quiet;
static void t(const char *name, int ok) {
    g_total++; g_fail += !ok;
    if (!ok || !g_quiet) printf("%-110s %s\n", name, ok ? "\xE2\x9C\x93" : "\xE2\x9C\x97");
}
static int env_set(const char *v) { const char *e = getenv(v); return e && *e; }
static int vk_required(void) { return env_set("VK_ICD_FILENAMES") || env_set("VK_DRIVER_FILES") || env_set("VK_LOADER_DRIVERS_SELECT"); }
static int validation_required(void) { const char *e = getenv("VK_INSTANCE_LAYERS"); return e && strstr(e, "VK_LAYER_KHRONOS_validation"); }
static const char *g_test;
static void hv_library(void);
static void hv_library_remove(void);
// The backend's device, brought up with validation asked for and the test's shader library in place (the backend finds its library when it starts, as the host's does);
// no device: skip (exit 0) or fail, as in t_vk_common.h
static const hostvk_dev *hv_up(const char *test) {
    g_test = test;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!getenv("DM_VK_VALIDATION")) setenv("DM_VK_VALIDATION", "1", 1);
    hv_library();
    be_init();
    const hostvk_dev *d = hvk_device();
    if (!d) {
        hv_library_remove();
        if (!vk_required()) { printf("note: no Vulkan device: skipped\n== %s: 0 checks, 0 failed (skipped: no Vulkan device) ==\n", test); exit(0); }
        t("the Vulkan backend has a device (VK_ICD_FILENAMES / VK_DRIVER_FILES / VK_LOADER_DRIVERS_SELECT names a driver)", 0);
        printf("== %s: %d checks, %d failed ==\n", test, g_total, g_fail); exit(1);
    }
    printf("device '%s' (%s %s), level %c, validation %s\n", d->props.deviceName, d->driver_name, d->driver_info, d->caps.level, d->validation ? "on (synchronization validation)" : "off");
    return d;
}
// The end of a test: the backend shut down (its validation messages are counted until then), the check that there were none, the summary line, the library removed
static int hv_finish(void) {
    const hostvk_dev *d = hvk_device();   // the structure outlives the shutdown with its counters (hostvk_destroy)
    hvk_shutdown();
    char name[200];
    if (d && d->validation) { snprintf(name, sizeof name, "no validation message: %u errors, %u warnings", (unsigned)d->val_errors, (unsigned)d->val_warnings); t(name, d->val_errors == 0 && d->val_warnings == 0); }
    else if (validation_required()) t("validation layer on", 0);
    else printf("note: VK_LAYER_KHRONOS_validation is not installed: validation not checked\n");
    printf("== %s: %d checks, %d failed ==\n", g_test, g_total, g_fail);
    hv_library_remove();
    return g_fail != 0;
}

// ---------- Guest memory: pools through the frontend ----------
typedef struct { NVNmemoryPool pool; uint8_t *mem; size_t size; } hv_pool;
static NVNdevice g_dev;
static void hv_device(int origin_upper_left) {
    NVNdeviceBuilder db; h_nvnDeviceBuilderSetDefaults(&db); h_nvnDeviceBuilderSetFlags(&db, 0); h_nvnDeviceInitialize(&g_dev, &db);
    h_nvnDeviceSetWindowOriginMode(&g_dev, origin_upper_left ? NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT : NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
}
static int hv_pool_make(hv_pool *p, size_t size) {
    memset(p, 0, sizeof *p);
    p->mem = plat_vm_alloc(size); p->size = size;
    if (!p->mem) return 0;
    NVNmemoryPoolBuilder b; h_nvnMemoryPoolBuilderSetDefaults(&b); h_nvnMemoryPoolBuilderSetDevice(&b, &g_dev); h_nvnMemoryPoolBuilderSetStorage(&b, p->mem, size);
    h_nvnMemoryPoolBuilderSetFlags(&b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    return h_nvnMemoryPoolInitialize(&p->pool, &b);
}
static void hv_pool_free(hv_pool *p) { h_nvnMemoryPoolFinalize(&p->pool); plat_vm_release(p->mem, p->size); memset(p, 0, sizeof *p); }

// ---------- The shader library ----------
static char g_lib[512], g_libv[600], g_map[600], g_pcache[600]; static int g_own_pcache;
static void hv_library(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_lib, sizeof g_lib, "%s/t_vk_lib_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_lib)) { perror("mkdtemp"); exit(2); }
    snprintf(g_libv, sizeof g_libv, "%s/vk-%s", g_lib, DMSH_CHAIN_VK); mkdir(g_libv, 0755);
    snprintf(g_map, sizeof g_map, "%s/shader_map.tsv", g_lib);
    FILE *f = fopen(g_map, "w"); if (!f) exit(2);
    fprintf(f, "package\tindex\tstage\tdm_ctl_sha16\tsource\n"); fclose(f);
    char p[700]; snprintf(p, sizeof p, "%s/inputs.tsv", g_libv); f = fopen(p, "w"); if (f) { fprintf(f, "game\ttest\ninputs\ttest\n"); fclose(f); }
    setenv("DM_MSL_DIR", g_lib, 1); setenv("DM_NVN_SHMAP", g_map, 1);
    snprintf(g_pcache, sizeof g_pcache, "%s/vk_pipeline_cache", g_lib);
    if (!getenv("DM_VK_PSO_CACHE")) { setenv("DM_VK_PSO_CACHE", g_pcache, 1); g_own_pcache = 1; }   // not the user's data folder
    if (!getenv("DM_PSO_MANIFEST")) setenv("DM_PSO_MANIFEST", "off", 1);
}
static void rm_dir(const char *dir) {   // a folder of files
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char p[1400];
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); remove(p); }
    closedir(d); rmdir(dir);
}
static void hv_library_remove(void) { if (g_lib[0]) { rm_dir(g_libv); if (g_own_pcache) rm_dir(g_pcache); rm_dir(g_lib); g_lib[0] = 0; } }
// ---------- A second start: the test run again as a child process (a new process: a new device, the backend's state and the disk cache read anew) ----------
// self: argv[0]; phase: the child's argument. The child inherits the environment as it is when called. Returns its exit status (-1: it could not be started)
static int hv_spawn(const char *self, const char *phase) {
    fflush(stdout); fflush(stderr);
#if defined(_WIN32)
    const char *argv[] = {self, phase, NULL};
    return (int)_spawnv(_P_WAIT, self, argv);
#else
    pid_t pid = fork();
    if (pid == 0) { execl(self, self, phase, (char *)NULL); _exit(127); }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
#endif
}
// A child's numbers: "name value" lines appended to a file, read back by name (the last line of that name; def when none)
static void hv_res_put(const char *path, const char *name, double v) { FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s %.9g\n", name, v); fclose(f); } }
static double hv_res_get(const char *path, const char *name, double def) {
    FILE *f = fopen(path, "r"); if (!f) return def;
    char line[256], n[128]; double v, out = def;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%127s %lf", n, &v) == 2 && !strcmp(n, name)) out = v;
    fclose(f);
    return out;
}
// A shader of a program: control section and code made unique by name, the program's data in pool cp at offset off; its module (code, bytes) and its Vulkan reflection
// (bindings_json: the "bindings" array's text, vin_json: the "vertex_inputs" array's, both may be NULL) are written under the key the frontend gives it
typedef struct { const char *name; int stage; const uint32_t *code; size_t bytes; const char *bindings; const char *vin; } hv_shader;
static uint8_t g_ctl[6][0x880];
static void hv_program(NVNprogram *prog, hv_pool *cp, size_t off, const hv_shader *sh, int n) {
    static const char *const stage_name[6] = {"vertex", "pixel", "geometry", "hull", "domain", "compute"};
    static const char *const refl_stage[6] = {"vs", "ps", "gs", "hs", "ds", "cs"};
    NVNshaderData sd[6]; memset(sd, 0, sizeof sd);
    for (int i = 0; i < n; i++) {
        memset(g_ctl[i], 0, sizeof g_ctl[i]);
        snprintf((char *)g_ctl[i] + 0x100, 0x80, "nxnative test shader %s", sh[i].name);
        uint32_t ncode = 64; memcpy(g_ctl[i] + 0x704, &ncode, 4);
        uint8_t *code = cp->mem + off + (size_t)i * 256;
        memset(code, 0, 64); snprintf((char *)code, 64, "code of %s", sh[i].name);
        sd[i].data = (NVNbufferAddress)(uintptr_t)code; sd[i].control = g_ctl[i];
    }
    h_nvnProgramInitialize(prog, &g_dev);
    h_nvnProgramSetShaders(prog, n, sd);
    FILE *m = fopen(g_map, "a");
    for (int i = 0; i < n; i++) {
        uint8_t md[8]; char key[17]; prog_key((const prog_t *)prog, (unsigned)i, md);
        for (int b = 0; b < 8; b++) snprintf(key + 2 * b, 3, "%02x", md[b]);
        char p[800]; snprintf(p, sizeof p, "%s/%s.spv", g_libv, key);
        FILE *f = fopen(p, "wb"); if (f) { fwrite(sh[i].code, 1, sh[i].bytes, f); fclose(f); }
        snprintf(p, sizeof p, "%s/%s.json", g_libv, key);
        if ((f = fopen(p, "w"))) {
            fprintf(f, "{\"key\":\"%s\",\"target\":\"vk\",\"chain\":\"%s\",\"stage\":\"%s\",\"entry\":\"main\",\"bindings\":%s,\"vertex_inputs\":%s}", key, DMSH_CHAIN_VK,
                    refl_stage[sh[i].stage], sh[i].bindings ? sh[i].bindings : "[]", sh[i].vin ? sh[i].vin : "[]");
            fclose(f);
        }
        if (m) fprintf(m, "test\t%d\t%s\t%s\t%s\n", i, stage_name[sh[i].stage], key, sh[i].name);
    }
    if (m) fclose(m);
}

// ---------- Records ----------
typedef struct { uint8_t *base; size_t used, cap; } hv_cmds;
static void hv_emit(hv_cmds *c, unsigned op, const void *p, size_t n) {
    size_t size = (8 + n + 7) & ~(size_t)7; op_hdr h = {op, (uint32_t)size};
    if (c->used + size + 8 > c->cap) { fprintf(stderr, "command buffer full\n"); exit(2); }
    memcpy(c->base + c->used, &h, 8); if (n) memcpy(c->base + c->used + 8, p, n); c->used += size;
}
static void hv_submit(NVNqueue *q, hv_cmds *c) {   // the records so far, ended, submitted on q and waited for
    hv_emit(c, OP_END, NULL, 0);
    NVNcommandHandle h = (NVNcommandHandle)(uintptr_t)c->base;
    h_nvnQueueSubmitCommands(q, 1, &h); h_nvnQueueFinish(q);
    c->used = 0;
}
static tex_t hv_tex(int target, int format, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, int flags) {   // a texture object as a record snapshots one (GPU-only storage)
    tex_t t; memset(&t, 0, sizeof t);
    t.magic = MAGIC('T', 'E', 'X', 0); t.serial = nvn_new_serial();
    t.d = (nvn_texdesc){.target = target, .format = format, .flags = flags, .samples = 1, .width = w, .height = h, .depth = depth, .levels = levels};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
// Subresources of a texture read back from the backend's image: (x, y) w×h of level, layers layer to layer + n - 1, aspect (VK_IMAGE_ASPECT_*), bpp bytes per texel; rows of
// w × bpp bytes, layer after layer, into out
static int hv_read_n(const tex_t *t, VkImageAspectFlags aspect, unsigned level, unsigned layer, unsigned n, int x, int y, uint32_t w, uint32_t h, uint32_t bpp, void *out) {
    VkFormat f; VkImageAspectFlags a; VkImage img = hvk_test_image(t, &f, &a);
    if (!img) return 0;
    VkBuffer b; VkDeviceMemory m; void *map; size_t bytes = (size_t)w * h * bpp * n;
    if (hvk_buffer_make((VkDeviceSize)bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &b, &m, &map)) return 0;
    VkCommandBuffer cb; int ok = 0;
    if (hvk_oneoff_begin(&cb)) {
        hvk_full_barrier(cb);
        VkBufferImageCopy r = {0, 0, 0, {aspect ? aspect : a, level, layer, n}, {x, y, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
        hvk_full_barrier(cb);
        if ((ok = hvk_oneoff_end(cb))) memcpy(out, map, bytes);
    }
    hvk_buffer_free(b, m);
    return ok;
}
static int hv_read(const tex_t *t, VkImageAspectFlags aspect, unsigned level, unsigned layer, int x, int y, uint32_t w, uint32_t h, uint32_t bpp, void *out) {
    return hv_read_n(t, aspect, level, layer, 1, x, y, w, h, bpp, out);
}

// ---------- Records the tests share, in the payload formats of dm/gfx/nvn/nvn_obj.h ----------
static void hv_rt(hv_cmds *c, int n, const tex_t *const *color, const tex_t *depth) {   // render targets with no views
    p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = n;
    for (int i = 0; i < n; i++) rt.color[i] = color[i];
    rt.depth = depth; hv_emit(c, OP_SET_RT, &rt, sizeof rt);
}
static void hv_scissor(hv_cmds *c, int x, int y, int w, int h) { p_scissor s = {x, y, w, h}; hv_emit(c, OP_SCISSOR, &s, sizeof s); }
static void hv_state(hv_cmds *c, int kind, const void *st, size_t n) {   // kind: 0 blend 1 color 2 channel mask 3 multisample 4 polygon 5 depth-stencil
    p_bind_state bs; memset(&bs, 0, sizeof bs); bs.kind = kind; memcpy(bs.bytes, st, n); hv_emit(c, OP_BIND_STATE, &bs, sizeof bs);
}
static void hv_depth(hv_cmds *c, int test, int write, int func) { ds_st ds; memset(&ds, 0, sizeof ds); ds.dt = (unsigned)test; ds.dw = (unsigned)write; ds.df = (unsigned)func; hv_state(c, 5, &ds, sizeof ds); }
static void hv_cull(hv_cmds *c, int cull) { poly_st p; memset(&p, 0, sizeof p); p.cull = (unsigned)cull; p.front = NVN_FRONT_FACE_CCW; p.mode = NVN_POLYGON_MODE_FILL; hv_state(c, 4, &p, sizeof p); }
static void hv_bind(hv_cmds *c, const NVNprogram *p, int stages) { p_bind_program b = {(const prog_t *)p, stages}; hv_emit(c, OP_BIND_PROGRAM, &b, sizeof b); }
static void hv_attrs(hv_cmds *c, int n, const va_st *a) { p_bind_vattr b; memset(&b, 0, sizeof b); b.n = n; memcpy(b.st, a, 4 * (size_t)n); hv_emit(c, OP_BIND_VATTR, &b, 8 + 4 * (size_t)n); }
static void hv_streams(hv_cmds *c, int n, const vs_st *s) { p_bind_vstream b; memset(&b, 0, sizeof b); b.n = n; memcpy(b.st, s, 8 * (size_t)n); hv_emit(c, OP_BIND_VSTREAM, &b, 8 + 8 * (size_t)n); }
static void hv_vbuf(hv_cmds *c, uint32_t index, const void *addr, uint32_t size) { p_bind_vbuf1 b = {index, size, (uint64_t)(uintptr_t)addr}; hv_emit(c, OP_BIND_VBUF1, &b, sizeof b); }
static void hv_buf(hv_cmds *c, unsigned op, int stage, int index, const void *addr, uint64_t size) { p_bind_buf b = {stage, index, (uint64_t)(uintptr_t)addr, size}; hv_emit(c, op, &b, sizeof b); }
static void hv_draw(hv_cmds *c, int prim, int first, int count, int instances) {   // a direct draw, recorded as the frontend records one (rec_draw: no indirect fields)
    p_draw d; memset(&d, 0, sizeof d); d.prim = prim; d.first = first; d.count = count; d.instances = instances; hv_emit(c, OP_DRAW, &d, offsetof(p_draw, ind));
}
static void hv_draw_idx(hv_cmds *c, int prim, int itype, int count, const void *ibuf, int base_vertex, int instances) {
    p_draw_idx d; memset(&d, 0, sizeof d); d.prim = prim; d.itype = itype; d.count = count; d.ibuf = (uint64_t)(uintptr_t)ibuf; d.base_vertex = base_vertex; d.instances = instances;
    hv_emit(c, OP_DRAW_IDX, &d, offsetof(p_draw_idx, ind));
}
// A texture's subresource region into guest memory at dst (the copy engine's layout: rows of w texels, tightly packed unless a row stride is set)
static void hv_t2b(hv_cmds *c, const tex_t *t, const view_t *v, void *dst, int x, int y, int z, int w, int h, int d) {
    p_copy_b2t r; memset(&r, 0, sizeof r); r.src = (uint64_t)(uintptr_t)dst; r.tex = t; if (v) { r.view = *v; r.has_view = 1; }
    r.r = (NVNcopyRegion){x, y, z, w, h, d}; hv_emit(c, OP_COPY_T2B, &r, sizeof r);
}
static void hv_b2b(hv_cmds *c, const void *src, void *dst, uint64_t n) { uint64_t a[3] = {(uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)dst, n}; hv_emit(c, OP_COPY_B2B, a, sizeof a); }
static void hv_clear_color(hv_cmds *c, int index, int mask, float r, float g, float b, float a) { p_clear_color cc = {index, mask, {r, g, b, a}}; hv_emit(c, OP_CLEAR_COLOR, &cc, sizeof cc); }
// An array of 32-bit words from a GLSL source's SPIR-V header: its byte size (the generated arrays are static)
#define HV_SPV(name) name, sizeof name
// The records so far, ended and submitted on q without waiting for the GPU (the replay encodes them during the call, so the command memory can be reused at once)
static void hv_submit_nowait(NVNqueue *q, hv_cmds *c) {
    hv_emit(c, OP_END, NULL, 0);
    NVNcommandHandle h = (NVNcommandHandle)(uintptr_t)c->base;
    h_nvnQueueSubmitCommands(q, 1, &h); h_nvnQueueFlush(q);
    c->used = 0;
}
// The pool finalized and initialized again over the same storage, as a game replaces a pool (the backend imports the memory anew)
static int hv_pool_replace(hv_pool *p) {
    h_nvnMemoryPoolFinalize(&p->pool);
    NVNmemoryPoolBuilder b; h_nvnMemoryPoolBuilderSetDefaults(&b); h_nvnMemoryPoolBuilderSetDevice(&b, &g_dev); h_nvnMemoryPoolBuilderSetStorage(&b, p->mem, p->size);
    h_nvnMemoryPoolBuilderSetFlags(&b, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    return h_nvnMemoryPoolInitialize(&p->pool, &b);
}
static void hv_update(hv_cmds *c, void *dst, const void *data, uint32_t n) {   // nvnCommandBufferUpdateUniformBuffer's record: the bytes travel in the command memory
    uint8_t rec[8 + 8 + 256]; uint64_t d = (uint64_t)(uintptr_t)dst, nn = n;
    memcpy(rec, &d, 8); memcpy(rec + 8, &nn, 8); memcpy(rec + 16, data, n);
    hv_emit(c, OP_UPDATE_UB, rec, 16 + n);
}
static void hv_fill(hv_cmds *c, void *dst, uint64_t n, uint32_t value) { p_clear_buf f = {(uint64_t)(uintptr_t)dst, n, value, 0}; hv_emit(c, OP_CLEAR_BUF, &f, sizeof f); }
