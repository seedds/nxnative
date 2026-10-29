// The Vulkan backend's pipeline cache on disk (docs/design/vulkan-backend.md 3.4; phase P5).
// One file per game and device: <folder>/<vendorID>-<deviceID>-<driverVersion>-<pipelineCacheUUID>-<DMSH_CHAIN_VK>-v<HVK_CACHE_VERSION>.bin (hexadecimal IDs), so a new driver, a
// new shader chain or a new version of what this backend puts into its pipelines starts a new file; at start, the files of the same vendor and device under another name are
// deleted (a laptop's two GPUs keep one each). The folder: DM_VK_PSO_CACHE=<folder> (off: no cache), else vk_pipeline_cache/ beside the save (the parent of DM_SAVE, as the Metal
// backend's pso_user.tsv and pso_archive/), else vk_pipeline_cache/ in the platform layer's data folder (plat_data_path).
// The file: 8 bytes "nxvkpc1\0", the data's length (8 bytes, little-endian) and SHA-256 (32), then vkGetPipelineCacheData's bytes. It is read at bring-up and checked: the
// framing, the hash, and the driver's header (VkPipelineCacheHeaderVersionOne: its size and version, this device's vendor and device IDs and pipelineCacheUUID), since drivers
// validate the header but not always what follows; a file that fails is ignored with one red line and replaced at the next write.
// Threads: every thread that compiles (the submit thread, the background workers, the prepare workers) has a VkPipelineCache of its own, made on its first compile from the
// file's data and held by the thread for the length of each creation (hvk_cache_begin / end). The writer makes an empty cache, merges each thread's into it, holding that thread's
// lock for its merge only, so it never reads a cache object a compile is using, and writes the merge to <file>.tmp and renames it over the file (copy and rename: the Metal
// archive's history of crashes on a half-written file, shaders.md). A thread that ends merges its cache into a shared one first. Written after the prepare step, 10 s after the
// last of a burst of new pipelines (the optimizing worker's tick), and at shutdown; not when the merge gives the bytes already on disk.
// What the driver keeps varies: lavapipe (Mesa 25.2.8) returns only the 32-byte header (it caches nothing), so there a warm start compiles as a cold one does; the counts of creations
// and of creations the driver answered from the cache (VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) say which kind of driver it is.
#include "plat/plat.h"
#include "vk_pso.h"
#include "../nvn/dmsh_chain_vk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#define HVK_CACHE_VERSION 1   // the backend's own: bump it when what a key makes changes (the libraries, the layouts, the state a pipeline bakes), so old files are dropped
static const char k_magic[8] = "nxvkpc1";
enum { HEAD = 48 };          // magic, length, SHA-256

typedef struct hvk_tcache { VkPipelineCache c; pthread_mutex_t mu; struct hvk_tcache *next; } hvk_tcache;
static _Thread_local hvk_tcache *t_tc;          // the calling thread's
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;   // the registry and the retired cache
static hvk_tcache *g_reg;
static VkPipelineCache g_retired;               // what the threads that ended had
static void *g_data; static size_t g_ndata;      // the driver data the file held (each thread's cache starts from it)
static int g_on; static char g_dir[900], g_path[1100], g_name[200];
static uint8_t g_disk_sha[32]; static int g_disk_known;   // the data on disk now
static _Atomic uint64_t g_new_ns;               // the last creation since the last write (0: none)
static _Atomic uint64_t s_loaded, s_written, s_writes, s_created, s_hits;

static void mkdirs(const char *path) {   // every component of path
    char p[1100]; snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) if (*c == '/' || *c == '\\') { char k = *c; *c = 0; mkdir(p, 0755); *c = k; }
    mkdir(p, 0755);
}
static void folder(char *out, size_t n) {
    const char *e = getenv("DM_VK_PSO_CACHE"), *save = getenv("DM_SAVE");
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    if (save && *save) {   // beside the save: its parent
        char s[900]; snprintf(s, sizeof s, "%s", save);
        size_t l = strlen(s); while (l > 1 && (s[l - 1] == '/' || s[l - 1] == '\\')) s[--l] = 0;
        char *slash = strrchr(s, '/'), *bs = strrchr(s, '\\'); if (bs && (!slash || bs > slash)) slash = bs;
        if (slash) { *slash = 0; snprintf(out, n, "%s/vk_pipeline_cache", slash == s ? "/" : s); return; }
        snprintf(out, n, "vk_pipeline_cache"); return;
    }
    plat_data_path(out, n, "vk_pipeline_cache");
}
static void name_of(char *out, size_t n) {
    const VkPhysicalDeviceProperties *p = &hvk.dev.props;
    char uuid[33]; for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", p->pipelineCacheUUID[i]);
    snprintf(out, n, "%04x-%04x-%08x-%s-%s-v%d.bin", p->vendorID, p->deviceID, p->driverVersion, uuid, DMSH_CHAIN_VK, HVK_CACHE_VERSION);
}
// The files of this vendor and device under another name (another driver, chain or backend version): deleted
static void remove_others(void) {
    char prefix[32]; snprintf(prefix, sizeof prefix, "%04x-%04x-", hvk.dev.props.vendorID, hvk.dev.props.deviceID);
    DIR *d = opendir(g_dir); if (!d) return;
    struct dirent *e; char p[1400];
    while ((e = readdir(d))) {
        const char *nm = e->d_name; size_t l = strlen(nm);
        if (strncmp(nm, prefix, strlen(prefix)) || !strcmp(nm, g_name)) continue;
        if (!(l > 4 && !strcmp(nm + l - 4, ".bin")) && !(l > 8 && !strcmp(nm + l - 8, ".bin.tmp"))) continue;
        snprintf(p, sizeof p, "%s/%s", g_dir, nm);
        if (!remove(p)) hostvk_log("[hostvk] pipeline cache of another driver, chain or backend version removed: %s", p);
    }
    closedir(d);
}
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// The file's driver data, or why it is not usable
static const char *check(const uint8_t *b, size_t n, const uint8_t **data, size_t *nd) {
    if (n < HEAD) return "shorter than its header";
    if (memcmp(b, k_magic, 8)) return "not a pipeline cache file of this backend";
    uint64_t len = rd64(b + 8);
    if (len != n - HEAD) return "its length does not match its header (a write cut short)";
    uint8_t sha[32]; plat_sha256(b + HEAD, (size_t)len, sha);
    if (memcmp(sha, b + 16, 32)) return "its SHA-256 does not match (corrupt)";
    const uint8_t *v = b + HEAD;
    if (len < 32) return "the driver's header is missing";
    uint32_t hsize = rd32(v), hver = rd32(v + 4), vendor = rd32(v + 8), device = rd32(v + 12);
    if (hsize < 32 || hsize > len || hver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return "the driver's header is not VkPipelineCacheHeaderVersionOne";
    if (vendor != hvk.dev.props.vendorID || device != hvk.dev.props.deviceID || memcmp(v + 16, hvk.dev.props.pipelineCacheUUID, 16)) return "it was written for another device or driver";
    *data = v; *nd = (size_t)len;
    return NULL;
}
void hvk_cache_init(void) {
    const char *e = getenv("DM_VK_PSO_CACHE");
    if (e && !strcmp(e, "off")) { hostvk_log("[hostvk] pipeline cache on disk off (DM_VK_PSO_CACHE=off)"); return; }
    folder(g_dir, sizeof g_dir); name_of(g_name, sizeof g_name);
    snprintf(g_path, sizeof g_path, "%s/%s", g_dir, g_name);
    mkdirs(g_dir);
    remove_others();
    g_on = 1;
    FILE *f = fopen(g_path, "rb");
    if (!f) { hostvk_log("[hostvk] pipeline cache %s: none yet (a first start on this driver and chain)", g_path); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    size_t got = b ? fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    const uint8_t *data = NULL; size_t nd = 0;
    const char *why = !b || got != (size_t)len ? "it could not be read" : check(b, got, &data, &nd);
    if (why) { hostvk_log("[RED] [hostvk] pipeline cache %s: %s: ignored, a new one is written", g_path, why); free(b); return; }
    if ((g_data = malloc(nd))) { memcpy(g_data, data, nd); g_ndata = nd; }
    plat_sha256(data, nd, g_disk_sha); g_disk_known = 1;
    atomic_store(&s_loaded, nd);
    hostvk_log("[hostvk] pipeline cache %s: %zu bytes of driver data read", g_path, nd);
    free(b);
}
VkPipelineCache hvk_cache_begin(void) {
    if (!g_on) return VK_NULL_HANDLE;
    if (!t_tc) {
        hvk_tcache *c = calloc(1, sizeof *c);
        if (!c) return VK_NULL_HANDLE;
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, g_ndata, g_data};
        if (vkCreatePipelineCache(hvk.d, &ci, NULL, &c->c)) { free(c); HVK_RED_ONCE("vkCreatePipelineCache failed: this thread compiles without the cache"); return VK_NULL_HANDLE; }
        pthread_mutex_init(&c->mu, NULL);
        pthread_mutex_lock(&g_reg_mu); c->next = g_reg; g_reg = c; pthread_mutex_unlock(&g_reg_mu);
        t_tc = c;
    }
    pthread_mutex_lock(&t_tc->mu);
    return t_tc->c;
}
void hvk_cache_end(const VkPipelineCreationFeedback *fb, int made) {
    if (made) { atomic_fetch_add(&s_created, 1); atomic_store(&g_new_ns, plat_now_ns()); }
    if (made && fb && (fb->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) && (fb->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) atomic_fetch_add(&s_hits, 1);
    if (g_on && t_tc) pthread_mutex_unlock(&t_tc->mu);
}
void hvk_cache_retire_thread(void) {
    hvk_tcache *c = t_tc;
    if (!c) return;
    t_tc = NULL;
    pthread_mutex_lock(&g_reg_mu);
    for (hvk_tcache **pp = &g_reg; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    if (!g_retired) { VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; if (vkCreatePipelineCache(hvk.d, &ci, NULL, &g_retired)) g_retired = VK_NULL_HANDLE; }
    if (g_retired) vkMergePipelineCaches(hvk.d, g_retired, 1, &c->c);
    pthread_mutex_unlock(&g_reg_mu);
    vkDestroyPipelineCache(hvk.d, c->c, NULL); pthread_mutex_destroy(&c->mu); free(c);
}
static int rename_over(const char *from, const char *to) {
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}
int hvk_cache_flush(const char *why) {
    if (!g_on) return 0;
    uint64_t mark = atomic_load(&g_new_ns);
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache dst;
    if (vkCreatePipelineCache(hvk.d, &ci, NULL, &dst)) { hostvk_log("[RED] [hostvk] pipeline cache: vkCreatePipelineCache failed: not written"); return -1; }
    pthread_mutex_lock(&g_reg_mu);
    for (hvk_tcache *c = g_reg; c; c = c->next) { pthread_mutex_lock(&c->mu); vkMergePipelineCaches(hvk.d, dst, 1, &c->c); pthread_mutex_unlock(&c->mu); }
    if (g_retired) vkMergePipelineCaches(hvk.d, dst, 1, &g_retired);
    pthread_mutex_unlock(&g_reg_mu);
    size_t n = 0; uint8_t *b = NULL; int rc = -1;
    if (vkGetPipelineCacheData(hvk.d, dst, &n, NULL) == VK_SUCCESS && (b = malloc(HEAD + n)) && vkGetPipelineCacheData(hvk.d, dst, &n, b + HEAD) == VK_SUCCESS) rc = 0;
    vkDestroyPipelineCache(hvk.d, dst, NULL);
    if (rc) { free(b); hostvk_log("[RED] [hostvk] pipeline cache: vkGetPipelineCacheData failed: not written"); return -1; }
    uint8_t sha[32]; plat_sha256(b + HEAD, n, sha);
    if (g_disk_known && !memcmp(sha, g_disk_sha, 32)) { free(b); atomic_compare_exchange_strong(&g_new_ns, &mark, 0); return 0; }
    memcpy(b, k_magic, 8); for (int i = 0; i < 8; i++) b[8 + i] = (uint8_t)((uint64_t)n >> (8 * i)); memcpy(b + 16, sha, 32);
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    mkdirs(g_dir);
    FILE *f = fopen(tmp, "wb");
    size_t put = f ? fwrite(b, 1, HEAD + n, f) : 0;
    int closed = f ? fclose(f) : -1;
    free(b);
    if (!f || put != HEAD + n || closed || rename_over(tmp, g_path)) { remove(tmp); hostvk_log("[RED] [hostvk] pipeline cache: writing %s failed: the old file stays", g_path); return -1; }
    memcpy(g_disk_sha, sha, 32); g_disk_known = 1;
    atomic_fetch_add(&s_written, n); atomic_fetch_add(&s_writes, 1);
    atomic_compare_exchange_strong(&g_new_ns, &mark, 0);
    hostvk_log("[hostvk] pipeline cache written (%s): %zu bytes of driver data, %s", why, n, g_path);
    return 1;
}
void hvk_cache_tick(uint64_t now) {
    uint64_t t = atomic_load(&g_new_ns);
    if (g_on && t && now > t && now - t >= 10000000000ull) hvk_cache_flush("10 s after new pipelines");
}
void hvk_cache_stats_get(hvk_cache_stats *s) {
    memset(s, 0, sizeof *s);
    s->loaded = s_loaded; s->written = s_written; s->writes = s_writes; s->created = s_created; s->hits = s_hits; s->on = g_on;
    snprintf(s->path, sizeof s->path, "%s", g_path);
}
void hvk_cache_shutdown(void) {
    if (!g_on) return;
    hvk_cache_flush("shutdown");
    pthread_mutex_lock(&g_reg_mu);
    while (g_reg) { hvk_tcache *c = g_reg; g_reg = c->next; vkDestroyPipelineCache(hvk.d, c->c, NULL); pthread_mutex_destroy(&c->mu); free(c); }
    if (g_retired) vkDestroyPipelineCache(hvk.d, g_retired, NULL);
    g_retired = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_reg_mu);
    t_tc = NULL;
    free(g_data); g_data = NULL; g_ndata = 0; g_on = 0; g_disk_known = 0;
}
