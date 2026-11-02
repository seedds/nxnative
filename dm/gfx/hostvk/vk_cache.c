// The Vulkan backend's pipeline cache on disk (docs/design/vulkan-backend.md 3.4; phase P5).
// One file per game and device: <folder>/<vendorID>-<deviceID>-<driverVersion>-<pipelineCacheUUID>-<DMSH_CHAIN_VK>-v<HVK_CACHE_VERSION>.bin (hexadecimal IDs), so a new driver, a
// new shader chain or a new version of what this backend puts into its pipelines starts a new file; at start, the cleanup below deletes the files of the
// same vendor and device under another name and those of another chain or backend version (a laptop's two GPUs keep one each). The folder: DM_VK_PSO_CACHE=<folder> (off: no cache), else vk_pipeline_cache/ beside the save (the parent of DM_SAVE, as the Metal
// backend's pso_user.tsv and pso_archive/), else vk_pipeline_cache/ in the platform layer's data folder (plat_data_path).
// The file: 8 bytes "nxvkpc1\0", the data's length (8 bytes, little-endian) and SHA-256 (32), then vkGetPipelineCacheData's bytes. It is read at bring-up and checked: the
// framing, the hash, and the driver's header (VkPipelineCacheHeaderVersionOne: its size and version, this device's vendor and device IDs and pipelineCacheUUID), since drivers
// validate the header but not always what follows; a file that fails is ignored with one red line and replaced at the next write.
// Threads: every thread that compiles (the submit thread, the background workers, the prepare workers) has a VkPipelineCache of its own, made on its first compile from the
// file's data and held by the thread for the length of each creation (hvk_cache_begin / end). The writer makes an empty cache, merges each thread's into it, holding that thread's
// lock for its merge only, so it never reads a cache object a compile is using, and writes the merge to <file>.tmp and renames it over the file (copy and rename: the Metal
// archive's history of crashes on a half-written file, shaders.md). A thread that ends merges its cache into a shared one first. Written after the prepare step, 10 s after the
// last of a burst of new pipelines (the optimizing worker's tick), and at shutdown; not when the merge gives the bytes already on disk.
// The folder's upkeep (decision D8, docs/design/vulkan-backend.md, step D8): each .bin file is an entry; index.tsv beside them holds each entry's size and last use, so a start reads one
// small file and a readdir instead of a stat per entry (only an entry the index does not list is stat'ed once, its modification time as its last use, and this game's own entry for
// its true size). The index is written to index.tsv.tmp and renamed over (a crash leaves the previous one); this game's own entry is marked used at each start and each write.
// At start, with the cleanup on (DM_VK_PSO_CACHE_CLEANUP=1, the default) the stale entries are removed: those of another chain or backend version (any device), those of this
// device under another driver (the driver version or pipelineCacheUUID in the name) and those unused for 90 days, with writes cut short (.bin.tmp) of other names; =0 keeps them
// all. The size limit (DM_VK_PSO_CACHE_LIMIT=<MiB>, default 512, 0 none) is checked at start and after each write: over it, entries are removed until the folder fits, those of
// another chain or backend version first, then other devices' entries of this one, each group least recently used first. This game's own entry (the one in use) is never removed by it.
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
#include <time.h>
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

// The folder's entries (index.tsv), under g_idx_mu: the writer updates this game's entry and applies the limit
enum { E_CUR, E_OTHER_DEVICE, E_STALE };        // this game's entry on this device; another device's of this chain and backend version; another version, or this device's other driver
typedef struct { char name[200]; uint64_t size; int64_t used; int cls; } hvk_entry;
static pthread_mutex_t g_idx_mu = PTHREAD_MUTEX_INITIALIZER;
static hvk_entry *g_ent; static int g_nent, g_cap_ent;
static uint64_t g_limit;                        // bytes (0: no limit)
static char g_suffix[64];                       // "-<DMSH_CHAIN_VK>-v<HVK_CACHE_VERSION>.bin": this chain and backend version
#define HVK_CACHE_LIMIT_MIB 512                 // decision D8's defaults (step D8)
#define HVK_CACHE_STALE_DAYS 90
#define MIB(b) ((double)(b) / 1048576.0)

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
static int rename_over(const char *from, const char *to) {
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}
static int ends_with(const char *s, const char *t) { size_t a = strlen(s), b = strlen(t); return a >= b && !strcmp(s + a - b, t); }
static hvk_entry *ent_find(const char *name) { for (int i = 0; i < g_nent; i++) if (!strcmp(g_ent[i].name, name)) return &g_ent[i]; return NULL; }
static hvk_entry *ent_add(const char *name) {
    if (g_nent == g_cap_ent) { int c = g_cap_ent ? 2 * g_cap_ent : 16; hvk_entry *e = realloc(g_ent, (size_t)c * sizeof *e); if (!e) return NULL; g_ent = e; g_cap_ent = c; }
    hvk_entry *e = &g_ent[g_nent++]; memset(e, 0, sizeof *e); snprintf(e->name, sizeof e->name, "%s", name);
    return e;
}
static int ent_class(const char *name) {
    if (!strcmp(name, g_name)) return E_CUR;
    if (!ends_with(name, g_suffix)) return E_STALE;   // another chain or backend version
    char prefix[32]; snprintf(prefix, sizeof prefix, "%04x-%04x-", hvk.dev.props.vendorID, hvk.dev.props.deviceID);
    return strncmp(name, prefix, strlen(prefix)) ? E_OTHER_DEVICE : E_STALE;   // this device under another driver version or cache UUID
}
// The index: a comment line, then "<entry>\t<size in bytes>\t<last use, Unix seconds>" per entry
static void index_path(char *out, size_t n, int tmp) { snprintf(out, n, "%s/index.tsv%s", g_dir, tmp ? ".tmp" : ""); }
static void index_save(void) {
    char p[1100], t[1100]; index_path(p, sizeof p, 0); index_path(t, sizeof t, 1);
    FILE *f = fopen(t, "wb");
    int ok = f && fprintf(f, "# nxnative Vulkan pipeline cache index 1: entry, size in bytes, last use (Unix seconds)\n") > 0;
    for (int i = 0; ok && i < g_nent; i++) ok = fprintf(f, "%s\t%llu\t%lld\n", g_ent[i].name, (unsigned long long)g_ent[i].size, (long long)g_ent[i].used) > 0;
    if (f && fclose(f)) ok = 0;
    if (!ok || rename_over(t, p)) { remove(t); hostvk_log("[RED] [hostvk] pipeline cache: writing %s failed: the previous index stays", p); }
}
// The folder's entries now: the .bin files a readdir finds, with what the index says of them (an entry it does not list: one stat)
typedef struct { char name[200]; uint64_t size; int64_t used; } hvk_row;
static void index_scan(int64_t now) {
    hvk_row *idx = NULL; int nidx = 0, cap = 0;
    char p[1100]; index_path(p, sizeof p, 0);
    FILE *f = fopen(p, "rb");
    char line[400]; hvk_row r;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long sz; long long used;
        if (line[0] == '#' || sscanf(line, "%199[^\t]\t%llu\t%lld", r.name, &sz, &used) != 3) continue;
        r.size = sz; r.used = used;
        if (nidx == cap) { cap = cap ? 2 * cap : 16; hvk_row *q = realloc(idx, (size_t)cap * sizeof *q); if (!q) break; idx = q; }
        idx[nidx++] = r;
    }
    if (f) fclose(f);
    g_nent = 0;
    DIR *d = opendir(g_dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const char *nm = e->d_name;
        if (!ends_with(nm, ".bin") || strlen(nm) >= sizeof r.name) continue;
        hvk_entry *x = ent_add(nm); if (!x) break;
        x->cls = ent_class(nm);
        int listed = 0;
        for (int i = nidx - 1; i >= 0 && !listed; i--) if (!strcmp(idx[i].name, nm)) { x->size = idx[i].size; x->used = idx[i].used; listed = 1; }   // a name's last row
        if (!listed || x->cls == E_CUR) {   // this game's own entry: its true size (one stat), as a crash between its rename and the index's would leave the index behind
            struct stat st; char fp[1400]; snprintf(fp, sizeof fp, "%s/%s", g_dir, nm);
            if (!stat(fp, &st)) { x->size = (uint64_t)st.st_size; if (!listed) x->used = (int64_t)st.st_mtime; }
        }
        if (x->cls == E_CUR) x->used = now;
    }
    if (d) closedir(d);
    free(idx);
}
static int ent_remove(int i) {   // 1: the file is gone (removed, or already missing) and the entry dropped
    char fp[1400]; snprintf(fp, sizeof fp, "%s/%s", g_dir, g_ent[i].name);
    struct stat st;
    if (remove(fp) && !stat(fp, &st)) return 0;
    g_ent[i] = g_ent[--g_nent];
    return 1;
}
// At start, with the cleanup on: the stale entries removed (logged once)
static void cleanup(int64_t now) {
    int64_t old = now - (int64_t)HVK_CACHE_STALE_DAYS * 86400;
    int n0 = g_nent, k_ver = 0, k_old = 0, k_tmp = 0; uint64_t bytes = 0;
    for (int i = g_nent - 1; i >= 0; i--) {
        uint64_t sz = g_ent[i].size;
        int stale = g_ent[i].cls == E_STALE, unused = g_ent[i].cls != E_CUR && g_ent[i].used < old;
        if ((stale || unused) && ent_remove(i)) { bytes += sz; if (stale) k_ver++; else k_old++; }
    }
    DIR *d = opendir(g_dir); struct dirent *e; char fp[1400], own[220]; snprintf(own, sizeof own, "%s.tmp", g_name);
    while (d && (e = readdir(d))) {   // a write cut short under another name (this game's own is overwritten at its next write)
        if (!ends_with(e->d_name, ".bin.tmp") || !strcmp(e->d_name, own)) continue;
        snprintf(fp, sizeof fp, "%s/%s", g_dir, e->d_name);
        if (!remove(fp)) k_tmp++;
    }
    if (d) closedir(d);
    hostvk_log("[hostvk] pipeline cache cleanup: %d entries, removed %d (%.1f MiB): %d of another driver, chain or backend version, %d unused for %d days%s", n0, k_ver + k_old,
               MIB(bytes), k_ver, k_old, HVK_CACHE_STALE_DAYS, k_tmp ? ", and unfinished writes" : "");
}
// The size limit: over it, entries removed until the folder fits, another version's first, then other devices', least recently used first (logged once per cleanup)
static void apply_limit(void) {
    if (!g_limit) return;
    uint64_t total = 0; for (int i = 0; i < g_nent; i++) total += g_ent[i].size;
    if (total <= g_limit) return;
    int n0 = g_nent, k = 0, stuck = 0; uint64_t t0 = total, bytes = 0;
    char *skip = calloc((size_t)g_nent + 1, 1);   // files that could not be removed: passed over
    while (total > g_limit && skip) {
        int pick = -1;
        for (int i = 0; i < g_nent; i++) {
            if (g_ent[i].cls == E_CUR || skip[i]) continue;
            int si = g_ent[i].cls == E_STALE, sp = pick < 0 ? 0 : g_ent[pick].cls == E_STALE;
            if (pick < 0 || si > sp || (si == sp && g_ent[i].used < g_ent[pick].used)) pick = i;
        }
        if (pick < 0) break;
        uint64_t sz = g_ent[pick].size;
        if (!ent_remove(pick)) { skip[pick] = 1; stuck++; continue; }
        skip[pick] = skip[g_nent];   // ent_remove moved the last entry into pick
        total -= sz; bytes += sz; k++;
    }
    free(skip);
    hostvk_log("[hostvk] pipeline cache: %d entries, %.1f MiB over %.0f MiB: removed %d (%.1f MiB)%s%s", n0, MIB(t0), MIB(g_limit), k, MIB(bytes),
               stuck ? "; some could not be removed" : "", total > g_limit ? "; still over: this game's own entry is kept" : "");
}
static void options(void) {
    const char *l = getenv("DM_VK_PSO_CACHE_LIMIT");
    unsigned long long mib = HVK_CACHE_LIMIT_MIB;
    if (l && *l) {
        char *end; unsigned long long v = strtoull(l, &end, 10);
        if (*end || end == l || l[0] == '-' || v > (1ull << 40)) hostvk_log("[RED] [hostvk] DM_VK_PSO_CACHE_LIMIT=%s is not a size in MiB (0: no limit): %d MiB", l, HVK_CACHE_LIMIT_MIB);
        else mib = v;
    }
    g_limit = (uint64_t)mib << 20;
}
static int cleanup_on(void) { const char *c = getenv("DM_VK_PSO_CACHE_CLEANUP"); return !(c && !strcmp(c, "0")); }
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
    snprintf(g_suffix, sizeof g_suffix, "-%s-v%d.bin", DMSH_CHAIN_VK, HVK_CACHE_VERSION);
    mkdirs(g_dir);
    options();
    int64_t now = (int64_t)time(NULL);
    pthread_mutex_lock(&g_idx_mu);   // the folder's upkeep (decision D8): the entries, the cleanup, the limit, the index
    index_scan(now);
    if (cleanup_on()) cleanup(now);
    else hostvk_log("[hostvk] pipeline cache cleanup off (DM_VK_PSO_CACHE_CLEANUP=0): %d entries kept, those of other drivers and versions too", g_nent);
    apply_limit();
    index_save();
    pthread_mutex_unlock(&g_idx_mu);
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
    pthread_mutex_lock(&g_idx_mu);   // this game's entry: its size and last use; then the limit
    hvk_entry *x = ent_find(g_name); if (!x && (x = ent_add(g_name))) x->cls = E_CUR;
    if (x) { x->size = HEAD + n; x->used = (int64_t)time(NULL); }
    apply_limit();
    index_save();
    pthread_mutex_unlock(&g_idx_mu);
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
    pthread_mutex_lock(&g_idx_mu); free(g_ent); g_ent = NULL; g_nent = g_cap_ent = 0; pthread_mutex_unlock(&g_idx_mu);
}
