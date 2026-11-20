// nn::jit (code generation at run time). On Horizon the jit service loads the game's JIT plugin, an NRO the game reads from its own files, into a process of its own, maps the
// code memory the game gave it read-write there and read + execute in the game, and calls the plugin's entry points for GenerateCode and Control (yuzu's service/jit has the same
// calling conventions). Super Mario 64 (Super Mario 3D All-Stars, program 1) translates the N64 code its precompiled NRO lacks this way; its main imports nn::jit::Initialize,
// CreateJitEnvironment, GenerateCode, Control, GetJitEnvironmentInfo, DestroyJitEnvironment and Finalize, and the SDK's Initialize reached for the service and crashed (sdk+0x2e5414).
// The host has no service process, so it takes over the whole API: the plugin is one of the rewritten NROs (dm/tools/nro_collect.py finds it by the hash the game's NRR registers),
// loaded alone (loader.c nro_load_alone: on Horizon it is not linked with the game), and its entry points run on the calling thread. Its state lives in the transfer memory the game
// gives it (the work buffer's last part), as on Horizon. On the Mac the code memory is a MAP_JIT mapping at one address for the plugin and the game (yuzu maps it the same way), writable
// only while the plugin runs on that thread (pthread_jit_write_protect_np). iOS runs no code made at run time: there GenerateCode is served from a capture (below).
#include "hle.h"
#include <TargetConditionals.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef struct { uint64_t offset, size; } jit_range;   // nn::jit::CodeRange: offset and size in the code (rx) or data (ro) memory
// The plugin's view of its memory (the JITConfiguration yuzu names): the game's code and data memory, the transfer memory, and the service's own mappings of the first two.
// Here sys_* are user_*: one mapping (the plugin writes its code through sys_rx: EmuSystemJITPlugin's GenerateCode keeps +0x30, and Control(0) hands +0 to the code it writes)
typedef struct { jit_range user_rx, user_ro, tmem, sys_rx, sys_ro; } jit_config;
// The SDK's environment (GetImpl(handle) is the handle itself, sdk+0x2e5680): +0 the service object, +8 code memory address, +0x10 its size, +0x18 data memory address, +0x20 its size;
// GetJitEnvironmentInfo copies +8..+0x28 (sdk+0x2e69e0). The host's handle points to the same layout, then its own fields
typedef struct {
    void *svc; uint64_t rx, rx_size, ro, ro_size;
    jit_config cfg;
    module_t *plugin;
    uint64_t (*gen)(int *ret, jit_range *c0_out, jit_range *c1_out, jit_config *cfg, uint64_t cmd, const void *in, size_t in_size, const jit_range *c0_in,
                    const jit_range *c1_in, const void *data, size_t data_size, void *out, size_t out_size);
    uint64_t (*ctl)(int *ret, jit_config *cfg, uint64_t cmd, const void *in, size_t in_size, void *out, size_t out_size);
    pthread_mutex_t mu;   // one request at a time, as the service's session serves them
    uint64_t ncalls[8], nbytes[8], ns[8];   // GenerateCode per command (0-7)
} jit_env;

static int jit_log_all(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_JIT_LOG"); v = e && atoi(e); } return v; }

// The plugin's helpers, which it looks up by name (nnjitpluginResolveBasicSymbols; on Horizon the service's own). Called from the plugin, so plain host functions
static void jit_panic(const char *msg) { LOG("[RED] JIT plugin PanicForPlugin: %s", msg ? msg : "(null)"); die("JIT plugin panic"); }
static void jit_abort(const char *cond, const char *fn, const char *file, int line) { LOG("[RED] JIT plugin abort: %s in %s (%s:%d)", cond ? cond : "", fn ? fn : "", file ? file : "", line); die("JIT plugin abort"); }
static void jit_unexpected(const char *fn, const char *file, int line) { LOG("[RED] JIT plugin unexpected default: %s (%s:%d)", fn ? fn : "", file ? file : "", line); die("JIT plugin abort"); }
static void *jit_resolve(const char *name) {
    static const struct { const char *name; void *fn; } t[] = {
        {"memcpy", (void *)memcpy}, {"memmove", (void *)memmove}, {"memset", (void *)memset}, {"PanicForPlugin", (void *)jit_panic},
        {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", (void *)jit_abort}, {"_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i", (void *)jit_unexpected},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (name && !strcmp(name, t[i].name)) return t[i].fn;
    LOG("[RED] JIT plugin asks for %s, which the host does not provide", name ? name : "(null)");
    return NULL;
}

static int range_ok(const jit_range *r, uint64_t size) { return r->offset <= size && r->size <= size - r->offset; }

// ---- Capture (Mac, DM_JIT_CAPTURE=<dir>): what the plugin made, for a host that runs no code made at run time (iOS) ----
// A request is known by its key: the command, the data with the words that differ from run to run masked (the code offset the game asks for, host addresses: Super Mario 64's
// command 4 passes the block's PC, its MIPS word count, the address of those words and the offset), and the SHA-256 of the input buffer (command 4: the block's MIPS words;
// a larger input only its size, in_sha). Each request is written once, as <dir>/<key>.jit: the request, the layout it was made in (every module, the code,
// data and transfer memory: the code holds absolute addresses into them), the code and the output buffer. dm/tools/jit_capture.py builds the image the iOS app serves from
int dm_guest_as_contains(uintptr_t a);
static const char *jit_capture_dir(void) { static const char *d; static int set; if (!set) { d = getenv("DM_JIT_CAPTURE"); set = 1; } return d; }
// A host address: in a module, the guest block, or the code / data / transfer memory of the environment (masked in the data words)
static jit_env *g_jit_env;
static int host_addr(uint64_t v) {
    jit_env *e = g_jit_env;
    if (e && ((v >= e->rx && v - e->rx < e->rx_size) || (v >= e->ro && v - e->ro < e->ro_size) || (v >= e->cfg.tmem.offset && v - e->cfg.tmem.offset < e->cfg.tmem.size))) return 1;
    return module_of((void *)(uintptr_t)v) || dm_guest_as_contains((uintptr_t)v);
}
// The input's part of the key: its SHA-256 up to 1 MB (Super Mario 64's command 4: the block's MIPS words). A larger input is the bulk request at boot (command 3: the N64 state with
// the precompiled block list, 140 MB), which holds a table of 4,235 pointers into the precompiled NRO, different in every run; its key is the command, the data and the size.
// Masking host addresses by value does not make it stable: about 92,000 other words fall inside the host's module range by chance ({x, 1} pairs of 32-bit values), and which ones
// depends on where the modules were mapped
enum { JIT_KEY_IN_MAX = 1 << 20 };
static void in_sha(const void *in, size_t n, uint8_t out[32]) {
    if (n > JIT_KEY_IN_MAX) { memset(out, 0, 32); memcpy(out, &n, sizeof n); return; }
    CC_SHA256(in, (CC_LONG)n, out);
}
// The data words that differ from run to run, masked: the offset asked for and host addresses. masked[] is what the capture stores
static void data_mask(uint8_t masked[32], const jit_range *c0_in, const void *in, size_t in_size, const uint8_t d32[32], size_t data_size) {
    uint64_t w[4]; memcpy(w, d32, 32);
    for (int k = 0; k < 4; k++) {
        uintptr_t v = (uintptr_t)w[k];
        if (8 * (size_t)k >= data_size) w[k] = 0;
        else if (w[k] == c0_in->offset || (in && v >= (uintptr_t)in && v - (uintptr_t)in < in_size) || host_addr(w[k])) w[k] = ~0ull;
    }
    memcpy(masked, w, 32);
}
// The key: SHA-256 of the command, the data size, its data and the input's hash, first 16 bytes in hex (jit_capture.py builds the same keys). The data: when the image's
// keydata row names a command's request bytes, those bytes as they are (Super Mario 64's command 4 struct leaves 4 padding bytes uninitialised, which differ when the plugin does not
// run, and masking by value took one such word, the count with 1 above it, for a host address); otherwise the data with run-dependent words masked (data_mask)
static uint8_t g_keydata[8][32]; static uint8_t g_keydata_set[8];
// outcode rows: where a command's output lists the code's entry points (u32 offsets of entries 1 … count − 1 from the code's start), their count and the code size. The game takes
// the code to be where it asked (Super Mario 64: entry = its own code pointer + offset, main+0x5256c, and it advances that pointer by the size, main+0x52640), so a request served
// elsewhere in the image gets its entry offsets moved by the difference and a size of 0: the game then always asks at the same offset, below every such request in the image
static struct { int set; uint32_t entries, count, size; } g_outcode[8];
static void jit_key(char hx[33], uint64_t cmd, const jit_range *c0_in, const void *in, size_t in_size, const uint8_t d32[32], size_t data_size, const uint8_t ish[32]) {
    uint8_t w[32];
    if (cmd < 8 && g_keydata_set[cmd]) for (int k = 0; k < 32; k++) w[k] = (k < (int)data_size ? d32[k] : 0) & g_keydata[cmd][k];
    else data_mask(w, c0_in, in, in_size, d32, data_size);
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, &cmd, 8); uint64_t ds = data_size; CC_SHA256_Update(&c, &ds, 8); CC_SHA256_Update(&c, w, 32); CC_SHA256_Update(&c, ish, 32);
    uint8_t h[32]; CC_SHA256_Final(h, &c);
    for (int k = 0; k < 16; k++) snprintf(hx + 2 * k, 3, "%02x", h[k]);
}
typedef struct { char name[16]; uint64_t base, size, pre; } jit_region;

// ---- Serving (iOS; the Mac with DM_JIT_SERVE=<dir>): GenerateCode answered from the image dm/tools/jit_capture.py built, the plugin never runs ----
// jit.img is the code memory's first part (mapped read + execute at the layout's base, loader.c; on iOS the executable's __TEXT,__dm<tag>_jit section, as the modules are embedded),
// jit.tsv the requests by key (where each one's code lies in the image, its output and the output's host addresses to relocate) and jit.out the outputs
typedef struct { char key[33]; uint64_t cmd, code_off, code_size, ro_off, ro_size, out_off, out_size; int ret; uint32_t nrel; uint64_t *rel_at, *rel_off; int *rel_mod; } jit_req;
static jit_req *g_req; static int g_nreq; static const uint8_t *g_out; static size_t g_out_len; static uint64_t g_img_size, g_layout_rx, g_layout_ro;
static void *jit_grow(void *p, size_t n) { void *q = realloc(p, n); if (!q) die("jit: out of memory"); return q; }
static void jit_serve_load(const char *dir) {
    char path[1100]; snprintf(path, sizeof path, "%s/jit.tsv", dir);
    FILE *f = fopen(path, "r"); if (!f) die("jit: no %s", path);
    size_t cap = 0; char *line = NULL; ssize_t n;
    while ((n = getline(&line, &cap, f)) > 0) {
        unsigned long long a, b, c;
        if (sscanf(line, "layout\t%llx\t%llx\t%llx", &a, &b, &c) == 3) { g_layout_rx = a; g_layout_ro = b; g_img_size = c; continue; }
        unsigned kc; char km[65];
        if (sscanf(line, "keydata\t%u\t%64s", &kc, km) == 2 && kc < 8) { for (int k = 0; k < 32; k++) sscanf(km + 2 * k, "%2hhx", &g_keydata[kc][k]); g_keydata_set[kc] = 1; continue; }
        unsigned oe, oc, os2;
        if (sscanf(line, "outcode\t%u\t%x\t%x\t%x", &kc, &oe, &oc, &os2) == 4 && kc < 8) { g_outcode[kc].set = 1; g_outcode[kc].entries = oe; g_outcode[kc].count = oc; g_outcode[kc].size = os2; continue; }
        if (strncmp(line, "req\t", 4)) continue;
        g_req = jit_grow(g_req, (size_t)(g_nreq + 1) * sizeof *g_req); jit_req *r = &g_req[g_nreq++]; memset(r, 0, sizeof *r);
        char rel[16]; unsigned long long co, cs, roo, ros, oo, os; int off = 0;
        if (sscanf(line, "req\t%32s\t%llu\t%llx\t%llx\t%llx\t%llx\t%d\t%llx\t%llx\t%n", r->key, (unsigned long long *)&r->cmd, &co, &cs, &roo, &ros, &r->ret, &oo, &os, &off) < 9 || !off) die("%s: bad row", path);
        r->code_off = co; r->code_size = cs; r->ro_off = roo; r->ro_size = ros; r->out_off = oo; r->out_size = os;
        (void)rel;
        for (const char *q = line + off; *q && *q != '\n' && *q != '-';) {   // <out offset>=<region>+<offset>, comma-separated
            unsigned long long at, ro2; char nm[17]; int k = 0;
            if (sscanf(q, "%llx=%16[^+]+%llx%n", &at, nm, &ro2, &k) != 3) die("%s: bad relocation in %s", path, r->key);
            int mi = -1; if (strcmp(nm, "jit_rx")) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, nm)) mi = i; if (mi < 0) die("%s: relocation into %s, which is not loaded", path, nm); }
            r->rel_at = jit_grow(r->rel_at, (r->nrel + 1) * 8); r->rel_off = jit_grow(r->rel_off, (r->nrel + 1) * 8); r->rel_mod = jit_grow(r->rel_mod, (r->nrel + 1) * sizeof(int));
            r->rel_at[r->nrel] = at; r->rel_off[r->nrel] = ro2; r->rel_mod[r->nrel] = mi; r->nrel++;
            q += k; if (*q == ',') q++;
        }
    }
    free(line); fclose(f);
    snprintf(path, sizeof path, "%s/jit.out", dir);
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) die("jit: no %s", path);
    g_out_len = (size_t)st.st_size; g_out = g_out_len ? mmap(NULL, g_out_len, PROT_READ, MAP_PRIVATE, fd, 0) : NULL; close(fd);
    if (g_out == MAP_FAILED) die("jit: cannot map %s", path);
    LOG("jit: serving %d requests from %s (image %#llx bytes, outputs %.1f MB): the plugin does not run", g_nreq, dir, (unsigned long long)g_img_size, g_out_len / 1048576.0);
}
static const jit_req *jit_find(const char *key) { for (int i = 0; i < g_nreq; i++) if (!strcmp(g_req[i].key, key)) return &g_req[i]; return NULL; }
// The image at the layout's base: the Mac maps jit.img read + execute, iOS remaps the executable's signed __TEXT,__dm<tag>_jit pages there (as loader.c remaps the modules)
#if TARGET_OS_IPHONE
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif
static void jit_map_image(uint8_t *at, const char *dir) {
    if (!g_img_size) return;
#if TARGET_OS_IPHONE
    (void)dir; unsigned long z = 0; char sn[17]; const char *tg = getenv("DM_EMBED_TAG");   // an app of several programs tags each one's sections (gen_embed.py --tag)
    snprintf(sn, sizeof sn, "__dm%s_jit", tg ? tg : "");
    const uint8_t *sec = getsectiondata(&_mh_execute_header, "__TEXT", sn, &z);
    if (!sec || z < g_img_size || ((uintptr_t)sec & 0x3fff)) die("jit: the image is not in the executable (__TEXT,%s), or smaller than jit.tsv says", sn);
    vm_address_t a = (vm_address_t)at; vm_prot_t cur = 0, max = 0;
    if (vm_remap(mach_task_self(), &a, g_img_size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)sec, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS || a != (vm_address_t)at || !(cur & VM_PROT_EXECUTE))
        die("jit: cannot remap the image to %p", at);
#else
    char path[1100]; snprintf(path, sizeof path, "%s/jit.img", dir);
    // As loader.c maps the modules on the Mac: copied into anonymous pages, then sealed read + execute (macOS refuses a fixed executable mapping of the file)
    int fd = open(path, O_RDONLY); if (fd < 0) die("jit: no %s", path);
    if (mmap(at, g_img_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != at) die("jit: cannot map %s at %p", path, at);
    if (read(fd, at, g_img_size) != (ssize_t)g_img_size) die("jit: cannot read %s", path);
    close(fd);
    if (mprotect(at, g_img_size, PROT_READ | PROT_EXEC)) die("jit: cannot seal the image at %p", at);
    sys_icache_invalidate(at, g_img_size);
#endif
}   // pre: a module's patch area in front of its base (patch_off < 0)
static void jit_capture(jit_env *e, uint64_t cmd, const void *in, size_t in_size, const jit_range *c0_in, const uint8_t d32[32], size_t data_size, const void *out, size_t out_size,
                        int ret, const jit_range *o0, const jit_range *o1) {
    uint8_t ish[32]; in_sha(in, in_size, ish);
    char key[33]; jit_key(key, cmd, c0_in, in, in_size, d32, data_size, ish);
    uint8_t md[32]; data_mask(md, c0_in, in, in_size, d32, data_size);
    char p[1024], t[1100]; snprintf(p, sizeof p, "%s/%s.jit", jit_capture_dir(), key);
    if (access(p, F_OK) == 0) return;
    jit_region r[g_nmod + 3]; uint32_t nr = 0;
    for (int i = 0; i < g_nmod; i++) { strlcpy(r[nr].name, g_mod[i].name, 16); r[nr].base = (uint64_t)g_mod[i].base; r[nr].size = g_mod[i].size; r[nr].pre = g_mod[i].patch_off < 0 ? (uint64_t)-g_mod[i].patch_off : 0; nr++; }
    strlcpy(r[nr].name, "jit_rx", 16); r[nr].base = e->rx; r[nr].size = e->rx_size; r[nr].pre = 0; nr++;
    strlcpy(r[nr].name, "jit_ro", 16); r[nr].base = e->ro; r[nr].size = e->ro_size; r[nr].pre = 0; nr++;
    strlcpy(r[nr].name, "jit_tmem", 16); r[nr].base = e->cfg.tmem.offset; r[nr].size = e->cfg.tmem.size; r[nr].pre = 0; nr++;
    snprintf(t, sizeof t, "%s.%d.tmp", p, getpid());
    FILE *f = fopen(t, "wb"); if (!f) { static int red; if (!red++) LOG("[RED] jit capture cannot write to %s", jit_capture_dir()); return; }
    uint32_t c32 = (uint32_t)cmd; uint64_t hd[4] = {c0_in->offset, c0_in->size, in_size, data_size};
    int ok = fwrite("DMJ5", 1, 4, f) == 4 && fwrite(&c32, 4, 1, f) && fwrite(hd, 8, 4, f) == 4 && fwrite(ish, 1, 32, f) == 32 && fwrite(md, 1, 32, f) == 32 && fwrite(d32, 1, 32, f) == 32;
    uint64_t os = out_size; int32_t rv = ret;
    ok = ok && fwrite(&os, 8, 1, f) && fwrite(&rv, 4, 1, f) && fwrite(o0, 16, 1, f) && fwrite(o1, 16, 1, f) && fwrite(&nr, 4, 1, f) && fwrite(r, sizeof *r, nr, f) == nr;
    if (o0->size) ok = ok && fwrite((void *)(e->rx + o0->offset), 1, o0->size, f) == o0->size;
    if (o1->size) ok = ok && fwrite((void *)(e->ro + o1->offset), 1, o1->size, f) == o1->size;
    if (out_size) ok = ok && fwrite(out, 1, out_size, f) == out_size;
    if (in_size <= JIT_KEY_IN_MAX) ok = ok && fwrite(in, 1, in_size, f) == in_size;   // small inputs whole: the image keys on the part a request used
    ok &= fclose(f) == 0;
    if (ok) rename(t, p); else unlink(t);   // whole files only: a capture run may be killed at any time
    static int n; if (n++ < 64) LOG("jit capture: command %llu → %s (%llu bytes of code)", (unsigned long long)cmd, key, (unsigned long long)o0->size);
}

HLE_FORCE("_ZN2nn3jit10InitializeEv", "connects to the jit service; the host runs the plugin itself", void, h_jit_Initialize, void) { LOG("init takeover: jit::Initialize (no-op)"); }
HLE_FORCE("_ZN2nn3jit8FinalizeEv", "Initialize opened no session", void, h_jit_Finalize, void) { }

// Create, Destroy, Control and GenerateCode run the game's JIT plugin, the game's own code: on the caller's stack (hle.h HLE_FORCE_DIRECT)
// CreateJitEnvironment(&handle, config, plugin, work, work_size): config = {code memory size, data memory size}, plugin = {NRR buffer, NRO buffer, transfer memory size} (sizes 2 MB
// multiples, as the SDK checks), work = code memory | data memory | transfer memory (2 MB aligned). Aborts as the SDK does on a failure (it has no error return the game checks)
HLE_FORCE_DIRECT("_ZN2nn3jit20CreateJitEnvironmentEPNS0_9JitHandleERKNS0_27JitEnvironmentConfigurationERKNS0_13JitPluginInfoEPvm",
          "the jit service: load the plugin NRO into the host and give it code memory the game can run", Result, h_jit_Create,
          void **out, const uint64_t *cfg, const uint64_t *plug, uint8_t *work, uint64_t work_size) {
    uint64_t rx_size = cfg[0], ro_size = cfg[1], tmem_size = plug[4];
    if (((uintptr_t)work | rx_size | ro_size | tmem_size) & 0x1fffff || rx_size + ro_size + tmem_size > work_size) die("jit::CreateJitEnvironment: sizes %#llx / %#llx / %#llx, work %p + %#llx (the SDK aborts)",
        (unsigned long long)rx_size, (unsigned long long)ro_size, (unsigned long long)tmem_size, work, (unsigned long long)work_size);
    const char *serve = dm_jit_serve_dir();
    if (serve) {
        if (!g_nreq) jit_serve_load(serve);
        uint8_t *base = dm_jit_layout_base();
        if (!base || rx_size != g_layout_rx || ro_size != g_layout_ro) die("jit: the game asks for %#llx bytes of code memory and %#llx of data memory, the image was built for %#llx / %#llx",
            (unsigned long long)rx_size, (unsigned long long)ro_size, (unsigned long long)g_layout_rx, (unsigned long long)g_layout_ro);
        jit_env *e = calloc(1, sizeof *e); pthread_mutex_init(&e->mu, NULL);
        jit_map_image(base, serve);
        uint8_t *ro = mmap(NULL, ro_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (ro == MAP_FAILED) die("jit: data memory of %#llx bytes", (unsigned long long)ro_size);
        e->rx = (uint64_t)base; e->rx_size = rx_size; e->ro = (uint64_t)ro; e->ro_size = ro_size;
        e->cfg = (jit_config){{e->rx, rx_size}, {e->ro, ro_size}, {(uint64_t)(work + rx_size + ro_size), tmem_size}, {e->rx, rx_size}, {e->ro, ro_size}};
        *out = e; g_jit_env = e;
        LOG("jit::CreateJitEnvironment: served from %s: code memory %p + %#llx (the image's first %#llx bytes), data memory %p + %#llx", serve, base, (unsigned long long)rx_size,
            (unsigned long long)g_img_size, ro, (unsigned long long)ro_size);
        return 0;
    }
    // The NRO is one of the two buffers (the NRR the other; Super Mario 64 passes the NRR first)
    const uint8_t *nro = NULL; uint64_t nro_size = 0;
    for (int k = 0; k < 2; k++) { const uint8_t *b = (const uint8_t *)plug[2 * k]; if (b && plug[2 * k + 1] >= 0x80 && !memcmp(b + 0x10, "NRO0", 4)) { nro = b; nro_size = plug[2 * k + 1]; } }
    if (!nro) die("jit::CreateJitEnvironment: no NRO among the plugin buffers");
    uint32_t n = *(const uint32_t *)(nro + 0x18); if (n > nro_size) n = (uint32_t)nro_size;
    uint8_t sha[32]; CC_SHA256(nro, n, sha);
    char hx[17]; for (int k = 0; k < 8; k++) snprintf(hx + 2 * k, 3, "%02x", sha[k]);
    module_t *m = nro_find(sha);
    if (!m) die("jit::CreateJitEnvironment: no rewritten NRO with SHA-256 %s… (%u bytes): the plugin is not in the patched modules' nro.tsv", hx, n);
    jit_env *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->mu, NULL);
    e->plugin = m;
    uint8_t *rx = NULL;
#if TARGET_OS_IPHONE
    die("jit::CreateJitEnvironment: iOS runs no code generated at run time");
#else
    rx = mmap(NULL, rx_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (rx == MAP_FAILED) die("jit: MAP_JIT mapping of %#llx bytes failed", (unsigned long long)rx_size);
#endif
    uint8_t *ro = mmap(NULL, ro_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ro == MAP_FAILED) die("jit: data memory of %#llx bytes", (unsigned long long)ro_size);
    e->rx = (uint64_t)rx; e->rx_size = rx_size; e->ro = (uint64_t)ro; e->ro_size = ro_size;
    // On Horizon the work buffer's code and data parts become code memory the game no longer reaches, and the transfer memory part the plugin's state
    e->cfg = (jit_config){{e->rx, rx_size}, {e->ro, ro_size}, {(uint64_t)(work + rx_size + ro_size), tmem_size}, {e->rx, rx_size}, {e->ro, ro_size}};
    nro_load_alone(m);
    // As the service loads a plugin (yuzu LoadPlugin): its _init, GetVersion (1), ResolveBasicSymbols, SetupDiagnostics (off), Configure, OnPrepared
    void *gv = module_sym(m, "nnjitpluginGetVersion"), *rb = module_sym(m, "nnjitpluginResolveBasicSymbols"), *sd = module_sym(m, "nnjitpluginSetupDiagnostics"),
         *cf = module_sym(m, "nnjitpluginConfigure"), *op = module_sym(m, "nnjitpluginOnPrepared");
    e->gen = module_sym(m, "nnjitpluginGenerateCode"); e->ctl = module_sym(m, "nnjitpluginControl");
    if (!gv || !cf || !e->gen || !op) die("jit: %s lacks the plugin entry points (GetVersion, Configure, GenerateCode, OnPrepared)", m->name);
    if (m->init) ((void (*)(void))(m->base + m->init))();
    uint64_t ver = ((uint64_t (*)(void))gv)();
    if (ver != 1) die("jit: %s is plugin version %llu, the service takes 1", m->name, (unsigned long long)ver);
    if (rb) ((void (*)(void *))rb)((void *)jit_resolve);
    void *res = (void *)jit_resolve;
    if (sd) ((void (*)(uint32_t, void **))sd)(0, &res);
    uint32_t flags = 0;
    ((void (*)(uint32_t *))cf)(&flags);
    ((void (*)(jit_config *))op)(&e->cfg);
    *out = e; g_jit_env = e;
    LOG("jit::CreateJitEnvironment: plugin %s (SHA-256 %s…, %u bytes) loaded; code memory %p + %#llx (MAP_JIT), data memory %p + %#llx, plugin state %p + %#llx (the work buffer's last part)",
        m->name, hx, n, rx, (unsigned long long)rx_size, ro, (unsigned long long)ro_size, (void *)e->cfg.tmem.offset, (unsigned long long)tmem_size);
    return 0;
}

HLE_FORCE_DIRECT("_ZN2nn3jit21DestroyJitEnvironmentENS0_9JitHandleE", "unload the plugin and free its code memory", void, h_jit_Destroy, jit_env *e) {
    if (!e) return;
    LOG("jit::DestroyJitEnvironment: GenerateCode %llu / %llu / %llu / %llu calls (commands 1-4)", (unsigned long long)e->ncalls[1], (unsigned long long)e->ncalls[2],
        (unsigned long long)e->ncalls[3], (unsigned long long)e->ncalls[4]);
    if (e->plugin) {
        if (e->plugin->fini) ((void (*)(void))(e->plugin->base + e->plugin->fini))();
        nro_unload_alone(e->plugin);   // the loader's: under its locks, with its check of stacks borrowing the plugin's data
        munmap((void *)e->rx, e->rx_size);
    }   // a served image stays mapped in the layout
    munmap((void *)e->ro, e->ro_size); if (g_jit_env == e) g_jit_env = NULL;
    pthread_mutex_destroy(&e->mu); free(e);
}

typedef struct { uint64_t rx, rx_size, ro, ro_size; } jit_info;
HLE_FORCE("_ZN2nn3jit21GetJitEnvironmentInfoENS0_9JitHandleE", "where the code and data memory are mapped in the game", jit_info, h_jit_Info, jit_env *e) {
    return (jit_info){e->rx, e->rx_size, e->ro, e->ro_size};
}

static int jit_serve(jit_env *e, jit_range *c0_out, jit_range *c1_out, uint64_t cmd, const void *in, size_t in_size, const jit_range *c0_in, const uint8_t d32[32], size_t data_size,
                     void *out, size_t out_size) {
    uint8_t ish[32]; in_sha(in, in_size, ish);
    char key[33]; jit_key(key, cmd, c0_in, in, in_size, d32, data_size, ish);
    const jit_req *r = jit_find(key);
    static _Atomic unsigned long nserved, nmiss;
    if (!r) {
        unsigned long m = ++nmiss;
        LOG("[RED] jit::GenerateCode command %llu: request %s (data %08x %08x) is not in the image (miss %lu): capture it on the Mac (DM_JIT_CAPTURE) and rebuild the image",
            (unsigned long long)cmd, key, *(const uint32_t *)d32, *(const uint32_t *)(d32 + 4), m);
        const char *md = getenv("DM_JIT_MISS_DUMP");   // the missed request's input, for comparing with the captures
        if (md && in && in_size <= JIT_KEY_IN_MAX) { char q[1024]; snprintf(q, sizeof q, "%s/miss_%s.in", md, key); FILE *g = fopen(q, "wb"); if (g) { fwrite(in, 1, in_size, g); fclose(g); } }
        if (c0_out) *c0_out = (jit_range){c0_in->offset, 0};
        if (c1_out) *c1_out = (jit_range){0, 0};
        return -1;
    }
    if (r->out_size > out_size || r->out_off + r->ro_size + r->out_size > g_out_len || r->ro_off + r->ro_size > e->ro_size) die("jit: request %s does not fit the buffers the game gave", key);
    if (r->ro_size) memcpy((void *)(e->ro + r->ro_off), g_out + r->out_off, r->ro_size);
    if (r->out_size) memcpy(out, g_out + r->out_off + r->ro_size, r->out_size);
    for (uint32_t i = 0; i < r->nrel; i++) {
        uint64_t v = (r->rel_mod[i] < 0 ? e->rx : (uint64_t)g_mod[r->rel_mod[i]].base) + r->rel_off[i];
        memcpy((uint8_t *)out + r->rel_at[i], &v, 8);
    }
    if (cmd < 8 && g_outcode[cmd].set) {
        int64_t delta = (int64_t)r->code_off - (int64_t)c0_in->offset; uint32_t cnt = 0;
        if (delta < 0) die("jit: request %s lies at %#llx in the image, below the offset the game asks for (%#llx)", key, (unsigned long long)r->code_off, (unsigned long long)c0_in->offset);
        if (g_outcode[cmd].count + 4 <= r->out_size) memcpy(&cnt, (uint8_t *)out + g_outcode[cmd].count, 4);
        for (uint32_t i = 1; i < cnt && g_outcode[cmd].entries + 4 * i <= r->out_size; i++) {
            uint32_t v; memcpy(&v, (uint8_t *)out + g_outcode[cmd].entries + 4 * (i - 1), 4); v += (uint32_t)delta; memcpy((uint8_t *)out + g_outcode[cmd].entries + 4 * (i - 1), &v, 4);
        }
        if (g_outcode[cmd].size + 4 <= r->out_size) memset((uint8_t *)out + g_outcode[cmd].size, 0, 4);
    } else if (r->code_off != c0_in->offset) die("jit: request %s lies at %#llx in the image, not at %#llx where the game asks, and the image has no outcode row for command %llu", key,
                                                (unsigned long long)r->code_off, (unsigned long long)c0_in->offset, (unsigned long long)cmd);
    unsigned long k = ++nserved;
    if (k <= 16 || !(k & (k - 1)) || jit_log_all())
        LOG("jit::GenerateCode #%lu command %llu: served %s, code %#llx + %#llx (asked at %#llx), %u relocations", k, (unsigned long long)cmd, key, (unsigned long long)r->code_off,
            (unsigned long long)r->code_size, (unsigned long long)c0_in->offset, r->nrel);
    (void)data_size;
    if (c0_out) *c0_out = (jit_range){r->code_off, r->code_size};
    if (c1_out) *c1_out = (jit_range){r->ro_off, r->ro_size};
    return r->ret;
}

// Control(handle, command, in, in_size, out, out_size): the plugin's return value (the SDK aborts when the service fails; the plugin's wrapper fails only without a Control)
HLE_FORCE_DIRECT("_ZN2nn3jit7ControlENS0_9JitHandleEmPKvmPvm", "the plugin's nnjitpluginControl", int, h_jit_Control, jit_env *e, uint64_t cmd, const void *in, size_t in_size, void *out, size_t out_size) {
    if (!e->plugin) { LOG("jit::Control command %llu: served (the plugin does not run; its state is in the image)", (unsigned long long)cmd); (void)in; (void)in_size; (void)out; (void)out_size; return 0; }
    if (!e->ctl) die("jit::Control: the plugin has no nnjitpluginControl");
    int ret = -1;
    pthread_mutex_lock(&e->mu);
    uint64_t w = e->ctl(&ret, &e->cfg, cmd, in, in_size, out, out_size);
    pthread_mutex_unlock(&e->mu);
    LOG("jit::Control command %llu, in %p + %#zx, out %p + %#zx: %d%s", (unsigned long long)cmd, in, in_size, out, out_size, ret, w ? " (the plugin's wrapper failed)" : "");
    if (w) die("jit::Control: the plugin's wrapper failed (the SDK aborts)");
    return ret;
}

// GenerateCode(&code_out, &data_out, handle, command, in, in_size, code_in, data_in, data, data_size, out, out_size): the ranges are offsets in the code and data memory;
// data (at most 32 bytes) goes to the plugin zero-padded to 32, as the SDK sends it. After the plugin wrote code, the SDK invalidates the instruction cache over code_out and cleans the
// data cache over data_out (sdk+0x2e6bcc); here the plugin wrote through the same mapping, so invalidating the code range is all
HLE_FORCE_DIRECT("_ZN2nn3jit12GenerateCodeEPNS0_9CodeRangeES2_NS0_9JitHandleEmPKvmRKS1_S7_S5_mPvm", "the plugin's nnjitpluginGenerateCode, writing into the MAP_JIT code memory",
          int, h_jit_Generate, jit_range *c0_out, jit_range *c1_out, jit_env *e, uint64_t cmd, const void *in, size_t in_size, const jit_range *c0_in, const jit_range *c1_in,
          const void *data, size_t data_size, void *out, size_t out_size) {
    uint8_t d32[32] = {0}; memcpy(d32, data, data_size < 32 ? data_size : 32);
    jit_range o0 = {0}, o1 = {0};
    int ret = -1;
    if (!e->plugin) return jit_serve(e, c0_out, c1_out, cmd, in, in_size, c0_in, d32, data_size, out, out_size);
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&e->mu);
#if !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(0);
#endif
    uint64_t w = e->gen(&ret, &o0, &o1, &e->cfg, cmd, in, in_size, c0_in, c1_in, d32, data_size, out, out_size);
#if !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(1);
#endif
    if (!w && !ret && o0.size) {
        if (!range_ok(&o0, e->rx_size)) die("jit::GenerateCode: the plugin returned code range %#llx + %#llx beyond the code memory", (unsigned long long)o0.offset, (unsigned long long)o0.size);
        sys_icache_invalidate((void *)(e->rx + o0.offset), o0.size);
    }
    uint64_t dt = now_ns() - t0; unsigned c = cmd < 8 ? (unsigned)cmd : 0;
    uint64_t k = ++e->ncalls[c]; e->nbytes[c] += o0.size; e->ns[c] += dt;
    pthread_mutex_unlock(&e->mu);
    if (k <= 16 || jit_log_all() || !(k & (k - 1)))
        LOG("jit::GenerateCode #%llu command %llu: in %p + %#zx, code in %#llx + %#llx, data %zu bytes %016llx %016llx, out %#zx → %d, code %#llx + %#llx, data %#llx + %#llx, %.2f ms (command %llu so far: %llu bytes, %.1f ms)",
            (unsigned long long)k, (unsigned long long)cmd, in, in_size, (unsigned long long)c0_in->offset, (unsigned long long)c0_in->size, data_size,
            (unsigned long long)__builtin_bswap64(*(uint64_t *)d32), (unsigned long long)__builtin_bswap64(*(uint64_t *)(d32 + 8)), out_size, ret,
            (unsigned long long)o0.offset, (unsigned long long)o0.size, (unsigned long long)o1.offset, (unsigned long long)o1.size, dt / 1e6,
            (unsigned long long)cmd, (unsigned long long)e->nbytes[c], e->ns[c] / 1e6);
    if (w) { LOG("[RED] jit::GenerateCode: the plugin's wrapper failed (command %llu)", (unsigned long long)cmd); die("jit::GenerateCode failed (the SDK aborts)"); }
    if (ret) LOG("[RED] jit::GenerateCode command %llu: the plugin returned %d (the service fails the request)", (unsigned long long)cmd, ret);
    if (jit_capture_dir()) jit_capture(e, cmd, in, in_size, c0_in, d32, data_size, out, out_size, ret, &o0, &o1);
    if (c0_out) *c0_out = o0;
    if (c1_out) *c1_out = o1;
    return ret;
}
