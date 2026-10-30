// nn::jit (run-time code generation through the jit:u sysmodule): Super Mario 3D All-Stars' Super Mario 64 hands its JIT plugin (RomFS nro/EmuSystemJITPlugin.nro)
// to the sysmodule, which loads it with nn::ro, calls its nnjitplugin* functions and lets it write code through a RW view of two code memories the game sees R-X and R--.
// The host has no sysmodule: the public nn::jit functions are taken over, the plugin NRO is loaded as nn::ro loads one (loader.c nro_find / nro_load: the rewritten copy
// the NSP script collected by its SHA-256, which the game's NRR registers) and called directly, on a host thread of its own with a large stack as the sysmodule's. The game's work buffer stays the plugin's RW view; the game's view is a second mapping
// of the same pages (mach_vm_remap), R-X for the first code memory and R-- for the second. Call order and argument layout per switchbrew.org/wiki/JIT_services.
// The code the plugin writes is not rewritten, so it must need no rewrite: no svc, no tpidr, no x18 (Super Mario 64's has none; its plugin and N64 code NRO use x18).
#include "hle.h"
#include <CommonCrypto/CommonDigest.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>

typedef struct { uint64_t off, size; } code_range;
typedef struct { uint64_t rx_addr, rx_size, ro_addr, ro_size, tmem_addr, tmem_size, sys_rx, sys_ro; } jit_state;   // the plugin's view (OnPrepared / Control / GenerateCode x3)
typedef struct {
    int used; jit_state st;
    int (*control)(int32_t *, jit_state *, uint64_t, const void *, uint64_t, void *, uint64_t);
    int (*generate)(int32_t *, code_range *, code_range *, jit_state *, uint64_t, const void *, uint64_t, const code_range *, const code_range *, const void *, uint64_t, void *, uint64_t);
} jit_env;
static jit_env g_env[4];

// ---------- Running plugin code on a big host stack (the sysmodule's thread; a game thread's stack can be small) ----------
typedef struct { void (*fn)(void *); void *arg; } jit_call;
static void *jit_thread(void *p) { jit_call *c = p; c->fn(c->arg); return NULL; }
static void on_big_stack(void (*fn)(void *), void *arg) {
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    jit_call c = {fn, arg}; pthread_t t;
    if (pthread_create(&t, &a, jit_thread, &c)) die("jit: pthread_create");
    pthread_join(t, NULL); pthread_attr_destroy(&a);
}

// ---------- Plugin NRO ----------
// What the plugin asks of the sysmodule's diagnostics: reported here (the sysmodule's own functions; the game's sdk ones would run on a thread the host made, without a guest context)
static void plg_panic(const char *msg) { die("jit: the plugin panicked: %s", msg ? msg : "(no message)"); }
static void plg_abort(const char *cond, const char *fn, const char *file, int line) { die("jit: the plugin aborted: %s in %s (%s:%d)", cond ? cond : "-", fn ? fn : "-", file ? file : "-", line); }
static void plg_unexpected(const char *fn, const char *file, int line) { die("jit: the plugin hit an unexpected default: %s (%s:%d)", fn ? fn : "-", file ? file : "-", line); }
static void plg_vlog(const void *meta, const char *fmt, const void *va) { (void)meta; (void)va; LOG("jit plugin log: %s", fmt ? fmt : ""); }   // Horizon's va_list is not Apple's: the format as is
static const char *g_sym_asked[64]; static int g_nsym_asked;
static void *jit_lookup(const char *name) {   // nnjitpluginResolveBasicSymbols' lookup (nn::ro::LookupSymbol in the sysmodule): the C library functions a plugin may want
    static const struct { const char *n; void *f; } t[] = {{"memcpy", (void *)memcpy}, {"memmove", (void *)memmove}, {"memset", (void *)memset}, {"memcmp", (void *)memcmp},
                                                           {"strlen", (void *)strlen}, {"strcmp", (void *)strcmp}, {"strncmp", (void *)strncmp}, {"abort", (void *)abort},
                                                           {"PanicForPlugin", (void *)plg_panic}, {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", (void *)plg_abort},
                                                           {"_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i", (void *)plg_unexpected},
                                                           {"_ZN2nn4diag6detail8VLogImplERKNS0_11LogMetaDataEPKcSt9__va_list", (void *)plg_vlog}};
    void *f = NULL;
    for (size_t i = 0; name && i < sizeof t / sizeof *t; i++) if (!strcmp(t[i].n, name)) f = t[i].f;
    if (g_nsym_asked < 64) { g_sym_asked[g_nsym_asked++] = name; LOG("%sjit: plugin looks up %s → %s", f ? "" : "[RED] ", name ? name : "(null)", f ? "host" : "none"); }
    return f;
}

// The plugin's rewritten copy (by the SHA-256 of the NRO the game hands over), loaded as nn::ro would load it; NULL when the pack has none
static module_t *plugin_module(const uint8_t *nro, size_t size) {
    if (size < 0x80 || memcmp(nro + 0x10, "NRO0", 4)) { LOG("[RED] jit: the plugin is not an NRO"); return NULL; }
    uint32_t n = *(const uint32_t *)(nro + 0x18); uint8_t sha[32]; CC_SHA256(nro, n, sha);
    module_t *m = nro_find(sha);
    if (!m) { LOG("[RED] jit: no rewritten copy of the plugin NRO (%u bytes) in the patched modules' nro.tsv", n); return NULL; }
    if (!m->loaded) { nro_load(m); if (m->init) ((void (*)(void))(m->base + m->init))(); }
    return m;
}

// ---------- Views of the game's work buffer ----------
static uint64_t alias_of(void *rw, uint64_t size, int prot) {   // a second mapping of the same pages for the game, with the game's permissions
    if (!size) return 0;
    mach_vm_address_t a = 0; vm_prot_t cur, max;
    if (mach_vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)rw, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) die("jit: mach_vm_remap %#llx", (unsigned long long)size);
    if (mprotect((void *)a, size, prot)) die("jit: mprotect code memory view");
    return a;
}

typedef struct { jit_env *e; const uint8_t *nro; size_t nro_size; int rc; } load_args;
static void plugin_prepare(void *p) {
    load_args *a = p; jit_env *e = a->e;
    module_t *m = plugin_module(a->nro, a->nro_size);
    if (!m) { a->rc = -1; return; }
    uint32_t (*ver)(void) = module_sym(m, "nnjitpluginGetVersion");
    void (*resolve)(void *) = module_sym(m, "nnjitpluginResolveBasicSymbols");
    void (*diag)(int, void *) = module_sym(m, "nnjitpluginSetupDiagnostics");
    void (*configure)(uint32_t *) = module_sym(m, "nnjitpluginConfigure");
    void (*prepared)(jit_state *) = module_sym(m, "nnjitpluginOnPrepared");
    e->control = module_sym(m, "nnjitpluginControl"); e->generate = module_sym(m, "nnjitpluginGenerateCode");
    if (!ver || !configure || !prepared || !e->control || !e->generate) { LOG("[RED] jit: the plugin lacks a required nnjitplugin function"); a->rc = -1; return; }
    uint32_t v = ver(); LOG("jit: plugin version %u", v);
    if (v > 1) { LOG("[RED] jit: plugin version %u is newer than 1", v); a->rc = -1; return; }
    if (resolve) resolve((void *)jit_lookup);
    if (diag) { void *lk = (void *)jit_lookup; diag(1, &lk); }
    configure(NULL);   // not the debug mode: no memory security mode outputs
    jit_state st = e->st; st.sys_rx = st.sys_ro = 0;   // +0x30: cleared for OnPrepared
    prepared(&st);
    a->rc = 0;
}

HLE_FORCE("_ZN2nn3jit10InitializeEv", "connect to the jit:u sysmodule; the host runs JIT plugins itself", void, h_jit_Initialize, void) { LOG("init takeover: jit::Initialize (no-op)"); }
HLE_FORCE("_ZN2nn3jit8FinalizeEv", "disconnect from jit:u", void, h_jit_Finalize, void) {}

HLE_FORCE("_ZN2nn3jit20CreateJitEnvironmentEPNS0_9JitHandleERKNS0_27JitEnvironmentConfigurationERKNS0_13JitPluginInfoEPvm",
          "JIT environment: the plugin NRO loaded and called by the host, code memory as a second mapping of the work buffer", Result, h_jit_Create,
          uint64_t *out, const uint64_t *cfg, const uint64_t *plugin, uint8_t *work, uint64_t work_size) {
    // cfg: R-X code memory size, R-- code memory size; plugin: NRR (address, size), NRO (address, size), work memory size. The work buffer holds the R-X code memory, the R-- one, then the work memory
    uint64_t rx = cfg[0], ro = cfg[1], tm = plugin[4];
    if (((rx | ro | tm | (uint64_t)work) & 0x1FFFFF) || rx + ro + tm > work_size) { LOG("[RED] jit: CreateJitEnvironment sizes rx %#llx ro %#llx work %#llx in %#llx", (unsigned long long)rx, (unsigned long long)ro, (unsigned long long)tm, (unsigned long long)work_size); return 0x1AF; }
    int k = 0; while (k < 4 && g_env[k].used) k++;
    if (k == 4) { LOG("[RED] jit: more than 4 JIT environments"); return 0x1AF; }
    jit_env *e = &g_env[k]; memset(e, 0, sizeof *e); e->used = 1;
    e->st.rx_addr = alias_of(work, rx, PROT_READ | PROT_EXEC); e->st.rx_size = rx;
    e->st.ro_addr = alias_of(work + rx, ro, PROT_READ); e->st.ro_size = ro;
    e->st.tmem_addr = (uint64_t)(work + rx + ro); e->st.tmem_size = tm;
    e->st.sys_rx = (uint64_t)work; e->st.sys_ro = (uint64_t)(work + rx);
    memset(work + rx + ro, 0, tm);
    LOG("jit: environment %d: code R-X %#llx (%#llx bytes, written at %p), R-- %#llx (%#llx bytes), work memory %p (%#llx bytes); plugin NRO %#llx bytes", k,
        (unsigned long long)e->st.rx_addr, (unsigned long long)rx, (void *)work, (unsigned long long)e->st.ro_addr, (unsigned long long)ro, (void *)(work + rx + ro), (unsigned long long)tm, (unsigned long long)plugin[3]);
    load_args a = {e, (const uint8_t *)plugin[2], plugin[3], -1};
    on_big_stack(plugin_prepare, &a);
    if (a.rc) { e->used = 0; return 0x1AF; }
    *out = (uint64_t)k + 1;
    return 0;
}
static jit_env *env_of(uint64_t h) { return h >= 1 && h <= 4 && g_env[h - 1].used ? &g_env[h - 1] : NULL; }

HLE_FORCE("_ZN2nn3jit21DestroyJitEnvironmentENS0_9JitHandleE", "JIT environment: the host keeps the plugin and the views until exit", void, h_jit_Destroy, uint64_t h) {
    jit_env *e = env_of(h); if (e) e->used = 0;
}

typedef struct { uint64_t rx_addr, rx_size, ro_addr, ro_size; } jit_info;
HLE_FORCE("_ZN2nn3jit21GetJitEnvironmentInfoENS0_9JitHandleE", "the game's addresses of the two code memories", jit_info, h_jit_Info, uint64_t h) {
    jit_env *e = env_of(h); jit_info i = {0};
    if (e) { i.rx_addr = e->st.rx_addr; i.rx_size = e->st.rx_size; i.ro_addr = e->st.ro_addr; i.ro_size = e->st.ro_size; }
    return i;
}

typedef struct { jit_env *e; uint64_t cmd; const void *in; uint64_t in_size; void *out; uint64_t out_size; int32_t res; int rc;
                 code_range *orx, *oro; const code_range *irx, *iro; uint8_t s32[0x20]; uint64_t s32_size; } call_args;
static void plugin_control(void *p) {
    call_args *a = p; jit_state st = a->e->st;
    a->rc = a->e->control(&a->res, &st, a->cmd, a->in, a->in_size, a->out, a->out_size);
}
HLE_FORCE("_ZN2nn3jit7ControlENS0_9JitHandleEmPKvmPvm", "JIT control command: the plugin's nnjitpluginControl", int, h_jit_Control, uint64_t h, uint64_t cmd, const void *in, uint64_t in_size, void *out, uint64_t out_size) {
    jit_env *e = env_of(h); if (!e) return 0;
    call_args a = {.e = e, .cmd = cmd, .in = in, .in_size = in_size, .out = out, .out_size = out_size};
    on_big_stack(plugin_control, &a);
    static int n; if (n++ < 20 || a.rc) LOG("%sjit: Control %llu (in %#llx, out %#llx bytes) → %d, rc %d", a.rc ? "[RED] " : "", (unsigned long long)cmd, (unsigned long long)in_size, (unsigned long long)out_size, a.res, a.rc);
    return a.rc ? 0 : a.res;
}

static void plugin_generate(void *p) {
    call_args *a = p; jit_state st = a->e->st;
    a->rc = a->e->generate(&a->res, a->orx, a->oro, &st, a->cmd, a->in, a->in_size, a->irx, a->iro, a->s32, a->s32_size, a->out, a->out_size);
}
static int range_ok(const code_range *r, uint64_t size) { return !(r->off & 3) && !(r->size & 3) && r->size <= size && r->off <= size - r->size; }
HLE_FORCE("_ZN2nn3jit12GenerateCodeEPNS0_9CodeRangeES2_NS0_9JitHandleEmPKvmRKS1_S7_S5_mPvm", "JIT code generation: the plugin's nnjitpluginGenerateCode writes through the RW view",
          int, h_jit_Generate, code_range *out_rx, code_range *out_ro, uint64_t h, uint64_t cmd, const void *in, uint64_t in_size,
          const code_range *rx, const code_range *ro, const void *data, uint64_t data_size, void *out, uint64_t out_size) {
    jit_env *e = env_of(h); if (!e) return 0;
    if (!range_ok(rx, e->st.rx_size) || !range_ok(ro, e->st.ro_size)) { LOG("[RED] jit: GenerateCode ranges out of the code memories"); return 0; }
    code_range orx = {rx->off, 0}, oro = {ro->off, 0};
    call_args a = {.e = e, .cmd = cmd, .in = in, .in_size = in_size, .out = out, .out_size = out_size, .orx = &orx, .oro = &oro, .irx = rx, .iro = ro, .s32_size = data_size};
    memcpy(a.s32, data, data_size > 0x20 ? 0x20 : data_size);
    on_big_stack(plugin_generate, &a);
    int ok = !a.rc && orx.off >= rx->off && rx->size >= orx.size && orx.off - rx->off <= rx->size - orx.size && oro.off >= ro->off && ro->size >= oro.size && oro.off - ro->off <= ro->size - oro.size;
    static int n; if (n++ < 20 || !ok)
        LOG("%sjit: GenerateCode %llu (in %#llx bytes) → %d, rc %d; code R-X +%#llx %#llx bytes, R-- +%#llx %#llx bytes", ok ? "" : "[RED] ", (unsigned long long)cmd, (unsigned long long)in_size, a.res, a.rc,
            (unsigned long long)orx.off, (unsigned long long)orx.size, (unsigned long long)oro.off, (unsigned long long)oro.size);
    if (!ok) return 0;
    if (orx.size) sys_icache_invalidate((void *)(e->st.rx_addr + orx.off), orx.size);
    *out_rx = orx; *out_ro = oro;
    return a.res;
}
