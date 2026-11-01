// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include "hle.h"

module_t *g_mod;
int g_nmod;
// Stable module addresses live in g_mod; lookup order follows activation, including unload/reload.
static module_t **g_loaded; static int g_nloaded, g_mod_capacity;
static pthread_rwlock_t g_modules_mu = PTHREAD_RWLOCK_INITIALIZER;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);
#if !DM_NATIVE_EXEC
// The x86-64 execution engine (aot.h): module images one to one in its arena below 2^39 (design 5.1, 5.2), never executable (3.2: their code runs from the
// pack's library), and guest functions the host calls entered through dm_guest_call (2.4)
void *dm_guest_alloc(size_t n);
uint8_t *dm_aot_image_map(const uint8_t *bytes, size_t len, size_t bss, uint64_t ro_end, size_t *mapped);
struct dm_guest_state;
struct dm_guest_state *dm_aot_self_state(void);
uint64_t dm_guest_call(struct dm_guest_state *st, uint64_t addr, int nargs, ...);
// What an import of a module the engine mapped binds to: a host function's guest-visible slot, an unimplemented import's stub slot (aot_arena.c, design 2.4)
uint64_t dm_aot_bind(const module_t *m, const char *name, uint64_t v);
#if defined(_WIN32)   // aot.h's DM_AOT_PACK_SUFFIX (this file declares what it needs of the engine instead of including aot.h: its unit test t_nro.c links no engine)
#define AOT_PACK_SUFFIX ".aot.dll"
#else
#define AOT_PACK_SUFFIX ".aot.so"
#endif
#endif

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    default: return g_keygate;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("cannot create the host key");
    LOG("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", (unsigned long)g_key18, (unsigned long)g_keytls, (unsigned long)g_keytlr, (unsigned long)g_keygate);
}

// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
static void tsd_check(void) {
#if DM_NATIVE_EXEC
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD layout mismatch");
    pthread_setspecific(g_key18, 0);
#endif   // the rewritten code reads its thread slots through Apple's TSD (tpidrro_el0); the x86-64 execution engine (Phase 2) keeps the guest's thread registers itself
}

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_FINI: m->fini = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // The same name already exists with a smaller index: keep that one
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("[RED] DM_SYMCHECK: %s in %s hash %p ≠ linear %p", name, m->name, r, l); } }
    return r;
}
static void *loaded_sym(const char *name) {   // caller holds g_modules_mu, or is doing startup relocation before guest threads run
    for (int i = 0; i < g_nloaded; i++) { void *p = module_sym(g_loaded[i], name); if (p) return p; }
    return NULL;
}
void *guest_sym(const char *name) {
    pthread_rwlock_rdlock(&g_modules_mu); void *p = loaded_sym(name); pthread_rwlock_unlock(&g_modules_mu);
    return p;
}
// For modules with the patch area before the base (patch_off < 0) addresses in the patch area count too, as for modules whose patch area follows the image (crash reports, the GC pause's "in guest code" test)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

// Whether main imports a symbol whose name starts with prefix (an undefined dynamic symbol: one main calls but another module defines), for behaviour that follows what the game uses
int dm_main_imports(const char *prefix) {
    size_t n = strlen(prefix);
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        module_t *m = &g_mod[i];
        for (unsigned k = 1; k < m->nsym; k++) if (!m->symtab[k].st_shndx && !strncmp(m->strtab + m->symtab[k].st_name, prefix, n)) return 1;
    }
    return 0;
}

// SDK major version, from the version string the SDK's libraries keep in the sdk module's read-only data ("NintendoSDK_nnSdk-10_4_0"; SDK 12 spells it NintendoSdk_).
// Behaviour that follows the SDK version is decided by it (the NVN API version, the shared-font layout), not by the game profile. 0 = no sdk module or no version string
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;   // read-only data, data and bss
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7)
            if (p - b >= 11 && !strncasecmp(p - 11, "NintendoSDK", 11) && p[7] >= '0' && p[7] <= '9') {
                major = atoi(p + 7); LOG("sdk: NintendoSDK nnSdk-%.12s (major %d)", p + 7, major); break; }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module; SDK-dependent behaviour takes the newer SDKs' side");
    return major;
}

// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    void *p = loaded_sym(name); if (p) return (uint64_t)p;
    return resolve_import(m, name);
}

// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
PLAT_TABLE_DEFINE(swforce)   // plat_table_swforce
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static unsigned *g_fidx, g_fmask;   // name hash over the table: force_lookup runs for every named relocation, and each lookup used to strcmp its way through all 722 entries (the swhle table's scan is hashed the same way, hle.c)
static void force_init(void) {
    size_t sz; g_force = (const hle_force_ent *)plat_table_swforce(&sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    unsigned cap = 16; while (cap < g_nforce * 2) cap <<= 1;
    g_fidx = calloc(cap, sizeof *g_fidx); g_fmask = cap - 1;
    for (unsigned i = 0; i < g_nforce; i++)   // insertion doubles as the duplicate check (a wrong list must still fail loudly, at once)
        for (unsigned j = sym_hash(g_force[i].name) & g_fmask;; j = (j + 1) & g_fmask) {
            if (!g_fidx[j]) { g_fidx[j] = i + 1; break; }
            if (!strcmp(g_force[g_fidx[j] - 1].name, g_force[i].name)) die("duplicate entry in the forced takeover list: %s", g_force[i].name);
        }
}
static void *force_lookup(const char *name) {
    if (!g_fmask) return NULL;
    for (unsigned j = sym_hash(name) & g_fmask; g_fidx[j]; j = (j + 1) & g_fmask) {
        unsigned i = g_fidx[j] - 1;
        if (!strcmp(g_force[i].name, name)) {
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  (%s)", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // A counting stub when DM_HLECOUNT is set
        }
    }
    return NULL;
}
// Generic host: the list serves several games, so entries a game does not reference are only noted; with DM_FORCE_STRICT=1 it still fails loudly (for checking a mistyped list)
static void force_check(void) {
    unsigned unused = 0; int strict = getenv("DM_FORCE_STRICT") != NULL;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { if (strict || g_flag.imports) LOG("forced takeover entry with no reference at all: %s", g_force[i].name); unused++; }
    if (unused && strict) die("forced takeover list is wrong");
    LOG("force: %u entries hit, %u not referenced by this game", g_nforce - unused, unused);
}

// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
    char line[256], key[32]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // may be negative (patch area before the base)
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "FPS_STATE_OFF")) m->fps_state_off = v;
        else if (!strcmp(key, "BUILD_ID")) sscanf(line, "# %*s %32s", m->build_id);
        else for (int k = 0; k < KEY_N; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != (1u << KEY_N) - 1)
        die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong", path);
}

// Frame step state (the page after the key page, read-write; layout in patch_nso.py FRAME_STEP): the rewriter's stubs step the game by the host's whole vsyncs capped at this
// target, 0 = the game's own interval logic. The profile's fps_target, overridden by DM_FPS_TARGET (0, 30 or 60); the graphics layer decides the steps and paces presents
// (hle_nvn.c fstep_tick)
uint8_t *dm_fps_state;
static const char *g_fps_mod;   // the module holding the state page (for the log)
static void install_fps_state(module_t *m) {
    if (m->fps_state_off + 0x40 > m->size || m->fps_state_off < m->key_page_off + m->key_page_size) die("%s: frame step state page out of range", m->name);
    dm_fps_state = m->base + m->fps_state_off; g_fps_mod = m->name;
}
// The settings in the state page, written when the game starts (main.c, after host loading): on iOS the loading runs at app launch, before the launcher has set DM_FPS_TARGET and
// DM_RENDER_SCALE (Launcher.swift), and nothing reads the page before the game runs
void dm_fps_state_apply(void) {
    if (!dm_fps_state) return;
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; using the game's own frame step", e); t = 0; }
    float f = (float)t; memcpy(dm_fps_state, &f, sizeof f);
    // Render scale of the 3D views (the stubs at Tears of the Kingdom's main+0xd5cc0, Breath of the Wild's main+0x1266880): 1.0 keeps the game's own size and dynamic resolution;
    // another factor fixes the size at that fraction (DM_RENDER_SCALE, 0.5 to 1.0). Independent of the frame step: written at target 0 too
    const char *rs = getenv("DM_RENDER_SCALE"); float sc = 1.0f;
    if (rs) { char *end; sc = strtof(rs, &end); if (end == rs || *end || !(sc >= 0.5f && sc <= 1.0f)) { LOG("[RED] DM_RENDER_SCALE=%s: must be 0.5 to 1.0; keeping the game's size", rs); sc = 1.0f; } else LOG("%s: 3D views rendered at %.2f of the game's size (DM_RENDER_SCALE)", g_fps_mod, sc); }
    memcpy(dm_fps_state + 0x10, &sc, sizeof sc);
    char cap[32]; snprintf(cap, sizeof cap, ", capped at %d fps", t);
    LOG("%s: frame step %s%s (%s)", g_fps_mod, !t ? "as the game sets it" : "in whole vsyncs decided by the host", t ? cap : "", e ? "DM_FPS_TARGET" : "game profile fps_target");
}
static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: key page out of range", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;
    for (int k = 0; k < KEY_N; k++) if (!page[m->key_idx[k]]) die("%s: key page entry %d is 0 (host key not created) -- the game would read TSD slot 0", m->name, k);
    if (plat_vm_protect(m->base + m->key_page_off, m->key_page_size, PLAT_PROT_READ)) die("%s key page mprotect", m->name);
    if (m->fps_state_off) install_fps_state(m);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, (long long)m->patch_off, (unsigned long)g_key18, (unsigned long)g_keytls, (unsigned long)g_keytlr, (unsigned long)g_keygate);
}

// DT_RELR (from SDK 20; all three modules of The Legend of Zelda: Breath of the Wild 1.9.0 have it): a compact table of relative relocations with the addend in the slot (*slot += base).
// Each 8-byte entry: even = a new start offset (relocate it, the pointer moves to the next slot); odd = a bitmap whose bits 1-63 stand for the 63 slots after the pointer, which then moves on by 63 slots.
// Without it main's GOT and vtables hold module offsets (main's RELA keeps only symbol relocations)
static unsigned relr_apply(module_t *m, uint64_t relr, uint64_t relrsz, uint64_t delta) {
    const uint64_t *e = (const uint64_t *)(m->base + relr); uint64_t *where = NULL; unsigned n = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { where = (uint64_t *)(m->base + v); *where++ += delta; n++; continue; }
        if (!where) die("%s: DT_RELR bitmap entry with no start before it", m->name);
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += delta; n++; }
        where += 63;
    }
    return n;
}

// The address a symbol relocation of module m binds to, as Horizon's rtld would with the modules loaded now (forced take-over, then the loaded modules in load order, then HLE / stub).
// *other: the module's own definition was taken over by an earlier module's; *weak0: an undefined weak symbol nothing defines, bound to 0
static uint64_t bind_sym(module_t *m, const Elf64_Sym *s, int *other, int *weak0) {
    uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
    *other = *weak0 = 0;
    // Symbols the module defines itself are also looked up globally in load order (the ELF default; Horizon's rtld does the same): the definition of an earlier-loaded module overrides a later one; only local / non-default visibility binds to itself.
    // Engage's main defines its own operator new / delete (Unity's memory manager) and sdk has a copy too (forwarding to malloc / free): previously sdk bound its own delete,
    // so blocks from main's new were freed by sdk's libc++ string code into the SDK heap, and CentralHeap::Free reported NotNMallocPtr
    if (!v && s->st_shndx) {
        uint64_t self = (uint64_t)m->base + s->st_value;
        v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
        if (!v) v = self;
        *other = v != self;
    }
    // An undefined weak symbol that no module defines and the host does not implement is 0, as Horizon's rtld binds it: the code tests its address before calling it.
    // Octopath Traveler 0's main imports the thread_local init function _ZTHN13LowLevelTasks5FTask10ActiveTaskE weakly; a named stub made it look defined and the game called it
    const char *nm = m->strtab + s->st_name;
    *weak0 = !v && !s->st_shndx && ELF64_ST_BIND(s->st_info) == STB_WEAK && !loaded_sym(nm) && !hle_implements(nm);
    if (!v && !*weak0) v = resolve_sym(m, nm);
#if !DM_NATIVE_EXEC
    v = dm_aot_bind(m, nm, v);   // x86-64 engine: a host function or stub as its guest-visible address, so a compiled blr through the GOT reaches it through the dispatcher
#endif
    return v;
}

typedef struct { const Elf64_Rela *tab[2]; uint64_t n[2]; uint64_t relr, relrsz; } relocs_t;
static relocs_t relocs_of(const module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0; relocs_t r = {0};
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: r.relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: r.relrsz = d->d_un.d_val; break;
    }
    r.tab[0] = (const Elf64_Rela *)(m->base + rela); r.tab[1] = (const Elf64_Rela *)(m->base + jmprel);
    r.n[0] = relasz / sizeof(Elf64_Rela); r.n[1] = pltsz / sizeof(Elf64_Rela);
    return r;
}

static void relocate(module_t *m) {
    relocs_t rl = relocs_of(m);
    int cnt[3] = {0}, nint = 0, nweak0 = 0;
    unsigned nrelr = rl.relr ? relr_apply(m, rl.relr, rl.relrsz, (uint64_t)m->base) : 0;
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < rl.n[t]; i++) {
            const Elf64_Rela *r = &rl.tab[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si]; int other, weak0;
            uint64_t v = bind_sym(m, s, &other, &weak0);
            if (other && nint++ < 8) LOG("%s: %s bound to an earlier module's definition (%s)", m->name, m->strtab + s->st_name, module_of((void *)v) ? module_of((void *)v)->name : "?");
            if (weak0 && nweak0++ < 8) LOG("%s: undefined weak %s bound to 0", m->name, m->strtab + s->st_name);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    if (!m->nro || m->loads <= 1)
        LOG("%s: reloc relative=%d relr=%u abs/glob=%d jump=%d%s", m->name, cnt[0], nrelr, cnt[1], cnt[2], nint ? " (some bound to an earlier module's definition)" : "");
    if (nint > 8) LOG("%s: %d bound to an earlier module's definition in all", m->name, nint);
}

// ---- NRO modules (nn::ro, hle_ro.c) ----
// A game may load modules at run time: nn::ro::LoadModule hands over an NRO image it read from its files (Super Smash Bros. Ultimate: the fighters' code, 98 NROs in data.arc, whose
// exports main imports: 1,045 lua2cpp::create_agent_* functions). iOS runs no code loaded at run time, so the NROs are rewritten offline like the other modules (dm/tools/nro_collect.py,
// patch_nso.py) and listed in <patched dir>/nro.tsv with their SHA-256. All are mapped at start (code read-only + execute from then on); none takes part in symbol lookups until the game
// loads it. A load restores the NRO's data and bss as rewritten (after the first), relocates it against the modules loaded then, in load order (Horizon's rtld does the same), and binds the
// symbol relocations of the modules loaded before it that it defines and nothing loaded defined (Horizon: BindVariables, and functions bound lazily at their first call): main's lua2cpp
// imports are stubs until then. Only data is written, so the same works with code embedded in the iOS executable. An unload binds every relocation that pointed into the NRO again
static void map_image(module_t *m, const char *dir);
static uint8_t g_refused[16][32]; static int g_nrefused;   // NROs the pack refused (online SDKs, nro.tsv rows named "refused")
int nro_refused(const uint8_t sha256[32]) {
    for (int i = 0; i < g_nrefused && i < 16; i++) if (!memcmp(g_refused[i], sha256, 32)) return 1;
    return 0;
}
static FILE *open_nros(const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/nro.tsv", dir);
    const char *off = getenv("DM_NRO");
    if (off && !atoi(off)) { LOG("NRO: loading off (DM_NRO=0): every LoadModule fails"); return NULL; }
    return fopen(path, "r");
}
// Final Fantasy X/X-2 registers 1,432 NROs, exceeding the old 128-slot array before nnMain. Size both tables from the actual manifest before any module pointer escapes;
// never realloc them, since nn::ro handles and the activation list retain module_t pointers. Refused SDK rows need no slot.
static void allocate_modules(int n, FILE *nros) {
    char line[512], name[16];
    if (n <= 0) die("no modules to load");
    if (nros) {
        while (fgets(line, sizeof line, nros)) {
            if (line[0] == '#' || sscanf(line, "%15s", name) != 1 || !strcmp(name, "refused")) continue;
            if (n == INT_MAX) die("too many modules");
            n++;
        }
        if (ferror(nros) || fseek(nros, 0, SEEK_SET)) die("cannot read NRO manifest");
    }
    g_mod_capacity = n;
    g_mod = calloc((size_t)n, sizeof *g_mod); g_loaded = calloc((size_t)n, sizeof *g_loaded);
    if (!g_mod || !g_loaded) die("cannot allocate %d module slots", n);
}
static void preload_nros(const char *dir, FILE *f) {
    char path[1024], line[512]; snprintf(path, sizeof path, "%s/nro.tsv", dir);
    if (!f) return;
    int n0 = g_nmod; size_t data = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[16], hex[65]; unsigned size;
        if (sscanf(line, "%15s %64s %u", name, hex, &size) != 3 || strlen(hex) != 64) die("%s: bad row %s", path, line);
        if (!strcmp(name, "refused")) {   // an online SDK the pack left out (dm/tools/nro_collect.py ONLINE_SDKS): LoadModule refuses its hash
            if (g_nrefused < (int)(sizeof g_refused / sizeof *g_refused)) for (int k = 0; k < 32; k++) sscanf(hex + 2 * k, "%2hhx", &g_refused[g_nrefused][k]);
            g_nrefused++; continue;
        }
        if (g_nmod >= g_mod_capacity) die("%s: NRO manifest grew while loading", path);
        module_t *m = &g_mod[g_nmod];
        strlcpy(m->name, name, sizeof m->name); m->nro = 1; m->nro_size = size;
        for (int k = 0; k < 32; k++) sscanf(hex + 2 * k, "%2hhx", &m->sha256[k]);
        read_layout(m, dir); map_image(m, dir); parse_dynamic(m); register_gates(m, dir);
        // The NRO header gives initialized data separately from bss. Keep only those bytes: bss is known zero before the first load, even for FFX's 925 MB static heap.
        m->pristine_len = *(const uint32_t *)(m->base + 0x34);
        if (*(const uint32_t *)(m->base + 0x30) != m->data_off || m->pristine_len > m->key_page_off - m->data_off) die("%s: NRO data layout mismatch", m->name);
        data += m->pristine_len;
        g_nmod++;
    }
    fclose(f);
    LOG("NRO: %d modules mapped (%s), loaded when the game asks; initialized data %.1f MB, snapshotted on first load", g_nmod - n0, path, data / 1048576.0);
}

module_t *nro_find(const uint8_t sha256[32]) {
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].nro && !memcmp(g_mod[i].sha256, sha256, 32)) return &g_mod[i];
    return NULL;
}

// Bind the symbol relocations of module k again: those whose symbol nm defines (load) or whose slot points into nm (unload). Returns how many changed
static unsigned rebind(module_t *k, module_t *nm, int load) {
    relocs_t rl = relocs_of(k); unsigned n = 0;
    uint64_t lo = (uint64_t)nm->base, hi = lo + nm->size;
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < rl.n[t]; i++) {
            const Elf64_Rela *r = &rl.tab[t][i];
            unsigned type = ELF64_R_TYPE(r->r_info);
            if (type == R_AARCH64_RELATIVE) continue;
            const Elf64_Sym *s = &k->symtab[ELF64_R_SYM(r->r_info)];
            uint64_t *where = (uint64_t *)(k->base + r->r_offset), add = type == R_AARCH64_JUMP_SLOT ? 0 : (uint64_t)r->r_addend, cur = *where - add;
            if (load ? s->st_shndx || !module_sym(nm, k->strtab + s->st_name) : !(cur >= lo && cur < hi)) continue;
            int other, weak0; uint64_t v = bind_sym(k, s, &other, &weak0);
            if (v != cur) { *where = v + add; n++; }
        }
    return n;
}

void nro_load(module_t *m) {
    pthread_rwlock_wrlock(&g_modules_mu);
    // No guest code can modify an inactive NRO before its first load. Copy here, before relocation/init: eagerly copying FFX/X-2's 1,432 modules committed 5.8 GB of mostly unused bss.
    if (!m->pristine) {
        if (!(m->pristine = malloc(m->pristine_len ? m->pristine_len : 1))) die("%s: pristine data copy", m->name);
        memcpy(m->pristine, m->base + m->data_off, m->pristine_len);
    }
    if (m->loads) {
        memcpy(m->base + m->data_off, m->pristine, m->pristine_len);
        memset(m->base + m->data_off + m->pristine_len, 0, m->key_page_off - m->data_off - m->pristine_len);
    }
    m->loads++; m->loaded = 1; g_loaded[g_nloaded++] = m;
    relocate(m);
    unsigned n = 0;
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].loaded && &g_mod[i] != m) n += rebind(&g_mod[i], m, 1);
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s loaded (load %u, base %p): %u imports of the modules loaded before bound to it", m->name, m->loads, m->base, n);
}

void nro_unload(module_t *m) {
    pthread_rwlock_wrlock(&g_modules_mu);
    if (!svc_nro_deactivate(m)) die("NRO %s: cannot unload while a mapped stack borrows its data", m->name);
    for (int i = 0; i < g_nloaded; i++) if (g_loaded[i] == m) { memmove(g_loaded + i, g_loaded + i + 1, (size_t)(--g_nloaded - i) * sizeof *g_loaded); break; }
    unsigned n = 0;
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].loaded) n += rebind(&g_mod[i], m, 0);
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s unloaded: %u imports that pointed into it bound again", m->name, n);
}

// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
            case DT_RELR: relr = d->d_un.d_ptr; break; case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
        // DT_RELR slots: subtracting the base makes them address-independent (done on the copy, reusing relr_apply's walk)
        if (relr) { uint8_t *b = m->base; m->base = cp; relr_apply(m, relr, relrsz, -(uint64_t)b); m->base = b; }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        plat_sha256_ctx c; plat_sha256_init(&c); plat_sha256_update(&c, cp, m->size);
        for (int t = 0; t < 2; t++)                                // Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                plat_sha256_update(&c, &tail, 8);
            }
        uint8_t d[32]; plat_sha256_final(&c, d); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s size %#zx relocation slots %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

#if TARGET_OS_IPHONE
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx (%#lx bytes) failed: %d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s permissions after vm_remap %#x, missing %#x", m->name, cur, need);
}
#define IMAGE_MH (&_mh_execute_header)
// The NRO modules (loader.c "NRO modules") are packed instead of getting sections of their own: a Mach-O file has at most 255 sections, and Super Smash Bros. Ultimate's 98 NROs would
// take 294. gen_embed.py puts their pieces one after another, each 16K aligned, in __TEXT,__dm_nro_x0 / __DM_NRO_W,__img / __TEXT,__dm_nro_x1, and __DATA,__dm_nro_tab lists them:
// 32 bytes of name, then the offset and length of its x0, W and x1 pieces in their sections (u64 each). A module name never holds an underscore, so these names meet no module's
typedef struct { char name[32]; uint64_t off[3][2]; } nro_ent;
static int nro_pieces(const char *name, const uint8_t **x0, unsigned long *z0, const uint8_t **w, unsigned long *zw, const uint8_t **x1, unsigned long *z1) {
    unsigned long zt, s0, sw, s1;
    const nro_ent *t = (const nro_ent *)getsectiondata(IMAGE_MH, "__DATA", "__dm_nro_tab", &zt);
    const uint8_t *b0 = getsectiondata(IMAGE_MH, "__TEXT", "__dm_nro_x0", &s0), *bw = getsectiondata(IMAGE_MH, "__DM_NRO_W", "__img", &sw), *b1 = getsectiondata(IMAGE_MH, "__TEXT", "__dm_nro_x1", &s1);
    if (!t || !b0 || !bw || !b1) return 0;
    for (unsigned long i = 0; i < zt / sizeof *t; i++) if (!strncmp(t[i].name, name, sizeof t[i].name)) {
        *x0 = b0 + t[i].off[0][0]; *z0 = t[i].off[0][1]; *w = bw + t[i].off[1][0]; *zw = t[i].off[1][1]; *x1 = b1 + t[i].off[2][0]; *z1 = t[i].off[2][1];
        return 1;
    }
    return 0;
}
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; snprintf(sx0, sizeof sx0, "__dm%sx0", m->name); snprintf(sx1, sizeof sx1, "__dm%sx1", m->name); snprintf(dm, sizeof dm, "__DM%sW", m->name);
    unsigned long z0 = 0, zw = 0, z1 = 0;
    const uint8_t *x0 = NULL, *w = NULL, *x1 = NULL;
    if (m->nro) { if (!nro_pieces(m->name, &x0, &z0, &w, &zw, &x1, &z1)) die("NRO %s: not in the embedded NRO table (__DATA,__dm_nro_tab)", m->name); }
    else { x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0); w = getsectiondata(IMAGE_MH, dm, "__img", &zw); x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1); }
    if (!x0 || !w || !x1) die("embedded sections %s / %s / %s missing", sx0, dm, sx1);
    // Patch area before the base (patch_off < 0): W ends at the end of data (FILE_IMAGE_LEN); bss and the key page are zero pages from vm_allocate and are not in the executable
    int front = m->patch_off < 0;
    if (z0 + zw != (front ? m->file_image_len : (uint64_t)m->patch_off) || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s embedded section layout mismatch (x0 %#lx w %#lx x1 %#lx, PATCH_OFF %#llx)", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = front ? m->image_size : m->patch_off + z1;
    uint64_t pre = front ? (uint64_t)-m->patch_off : 0;
    if (front) m->patch_len = z1;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, pre + m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s failed to reserve %#llx bytes of address space", m->name, (unsigned long long)(pre + m->size));
    base += pre;
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    // A module with nothing to rewrite has an empty patch area (87 of Super Smash Bros. Ultimate's 98 NROs: no x18, SVC or TLS register use); vm_remap refuses 0 bytes
    // (KERN_INVALID_ARGUMENT), which stopped the app at launch on an iPhone at the first such NRO (found in PR #94)
    if (z1) remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
#else
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // Patch area before the base: file = the first part of the image (text + ro + data) + patch area; bss and the key page are anonymous zero pages. The gap from the patch area's end to the base is left inaccessible (PROT_NONE)
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len;
        if (sz < fl) die("%s is shorter than FILE_IMAGE_LEN %#llx", path, (unsigned long long)fl);
        m->size = m->image_size; m->patch_len = (sz - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s patch area of %#llx bytes does not fit in the %#llx before the base", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
#if DM_NATIVE_EXEC
        uint8_t *a = plat_vm_alloc(pre + m->size);
#else
        uint8_t *a = dm_guest_alloc(pre + m->size);   // x86-64 engine: in the arena
#endif
        if (!a) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - fl); free(raw); install_key_page(m);
        uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
#if DM_NATIVE_EXEC
        if (plat_vm_protect(m->base, rx, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect text", m->name);
        if (plat_vm_protect(a, m->patch_len, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect patch", m->name);
#else
        if (plat_vm_protect(m->base, rx, PLAT_PROT_READ)) die("%s mprotect text", m->name);   // x86-64 engine: readable, never executable (design 3.2)
        if (plat_vm_protect(a, m->patch_len, PLAT_PROT_READ)) die("%s mprotect patch", m->name);
#endif
        if (pre > m->patch_len && plat_vm_protect(a + m->patch_len, pre - m->patch_len, PLAT_PROT_NONE)) die("%s mprotect gap", m->name);
        return;
    }
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
#if DM_NATIVE_EXEC
    m->base = plat_vm_alloc(m->size);
#else
    m->base = dm_guest_alloc(m->size);   // x86-64 engine: in the arena
#endif
    if (!m->base) die("%s mmap", m->name);
    memcpy(m->base, raw, sz); free(raw); install_key_page(m);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
#if DM_NATIVE_EXEC
    if (plat_vm_protect(m->base, rx, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect text", m->name);
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (plat_vm_protect(m->base + po, m->size - po, PLAT_PROT_READ | PLAT_PROT_EXEC)) die("%s mprotect patch", m->name);
#else
    if (plat_vm_protect(m->base, rx, PLAT_PROT_READ)) die("%s mprotect text", m->name);   // x86-64 engine: readable, never executable (design 3.2)
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (plat_vm_protect(m->base + po, m->size - po, PLAT_PROT_READ)) die("%s mprotect patch", m->name);
#endif
}
#endif

#if DM_NATIVE_EXEC   // the x86-64 engine's load_modules takes the recompiler's input instead (below, "The game-module path")
void load_modules(const char *dir, const char **names, int n) {
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    FILE *nros = open_nros(dir);
    allocate_modules(n, nros);
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        m->loaded = 1; g_loaded[g_nloaded++] = m; g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    preload_nros(dir, nros);   // mapped now, relocated when the game loads them (hle_ro.c)
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK: %lu symbol lookups at load time, hash and linear disagree %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① breakdown: mapping (read + copy + seal pages) %.1f ms, dynamic section parsing %.1f ms, gate registration %.1f ms, relocation + forced takeover %.1f ms, total %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}
#endif

// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT in forward order of the load list.
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->init || m->nro) continue;   // an NRO's init runs when the game loads it (hle_ro.c)
        LOG("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, m->init, m->init_arraysz / 8);
#if DM_NATIVE_EXEC
        ((void (*)(void))(m->base + m->init))();
#else
        dm_guest_call(dm_aot_self_state(), (uint64_t)(uintptr_t)(m->base + m->init), 0);   // x86-64 engine: guest code from the host (design 2.4)
#endif
    }
}
#if !DM_NATIVE_EXEC

// ---- The x86-64 execution engine's module images ----
// A module image as it is: an NRO file (a game's modules come through load_modules below, from their manifests), whose code runs from the pack the caller loads
// for it (aot_arena.c dm_aot_pack_load). Mapped one to one in the arena at a base the engine chooses (the pack's code is base-relative: 3.3), [0, ro_off)
// readable and never executable, the rest writable, bss zero; its dynamic section parsed; loaded (in the symbol lookups) and relocated and bound as
// load_modules relocates every module (relocations are data: 9.4). Modules added this way share g_mod with load_modules, at a fixed capacity, since
// module_t pointers never move
#define AOT_MAX_MODULES 64
// The image one to one in the arena (aot_arena.c dm_aot_image_map: [0, ro_off) readable and never executable, the rest readable and writable, bss zero) and the module's
// layout fields as load_modules' mapping sets them on Apple. key_page_off is the end of the mapping, where Apple's key page would begin: the end of the module's data and
// bss for dm_sdk_major's search and QueryMemory's read-only runs (none here)
static void aot_place(module_t *m, const uint8_t *bytes, size_t len, size_t bss, uint64_t text_end, uint64_t ro_off, uint64_t data_off) {
    size_t mapped = 0;
    m->base = dm_aot_image_map(bytes, len, bss, ro_off, &mapped);
    m->size = mapped; m->text_end = text_end; m->ro_off = ro_off; m->data_off = data_off; m->image_size = len; m->file_image_len = len;
    m->key_page_off = mapped; m->key_page_size = 0; m->patch_off = 0;
}
module_t *loader_add_image(const char *name, const uint8_t *bytes, size_t len, size_t bss, uint64_t text_end, uint64_t ro_off, uint64_t data_off) {
    pthread_rwlock_wrlock(&g_modules_mu);
    if (!g_mod) {
        g_mod_capacity = AOT_MAX_MODULES;
        g_mod = calloc((size_t)g_mod_capacity, sizeof *g_mod); g_loaded = calloc((size_t)g_mod_capacity, sizeof *g_loaded);
        if (!g_mod || !g_loaded) die("cannot allocate %d module slots", g_mod_capacity);
    }
    if (g_nmod >= g_mod_capacity) die("%s: no room for another module (%d)", name, g_mod_capacity);
    module_t *m = &g_mod[g_nmod];
    strlcpy(m->name, name, sizeof m->name);
    aot_place(m, bytes, len, bss, text_end, ro_off, data_off);
    parse_dynamic(m);
    m->loaded = 1; g_loaded[g_nloaded++] = m; g_nmod++;
    if (!g_force_hits) force_init();
    relocate(m);
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("%s: base %p size %#zx (image %#zx, bss %#zx; x86-64 engine)", name, (void *)m->base, m->size, len, bss);
    return m;
}

// hle.c's named stub for name, as resolve_import makes one for an import nothing implements (&unimpl_stubs[k], or the import table's function when it has the name),
// for a host function handed to the guest at run time (aot_arena.c dm_guest_fnptr): under the module lock, which every other stub allocation after the start holds too
// (an NRO's relocation). name must live as long as the process
uint64_t loader_named_stub(const char *name) {
    pthread_rwlock_wrlock(&g_modules_mu);
    const uint64_t v = resolve_import(NULL, name);
    pthread_rwlock_unlock(&g_modules_mu);
    return v;
}

// ---- The game-module path of the x86-64 execution engine (docs/design/aot-recompiler.md 1.1, 1.2, 9.4) ----
// load_modules' folder holds the recompiler's input, as it holds the rewritten modules on Apple (dm/tools/aot/README.md, "A game module in the host"). Per module:
//   <name>.aot.tsv    the manifest patch_nso.py --aot wrote: the module, the NSO build ID and the SHA-256 of the image; layout, aux, data, obs, spin, cut and oracle rows
//                     (patch_nso.py, "The recompiler's input"; nxaot reads the same text: dm/tools/aot/src/module/manifest.cpp)
//   <name>.image.bin  the behaviour image: the NSO's segments as the rewriter decompressed them (it reads the LZ4 of the NSO, as for Apple's <name>.patched.bin: the host decompresses
//                     nothing), with the cuts and the frame step's branches, none of Apple's rewrites
//   <name>.aot.so     the pack `nxaot build` made from the two (.aot.dll on Windows; aot.h DM_AOT_PACK_SUFFIX)
// The image is checked against its manifest (size, layout, SHA-256: refused with both hashes) and mapped one to one in the arena as loader_add_image maps one; the build ID
// goes to module_t.build_id, as fixups.tsv's on Apple, for the host's per-build behaviour (dm_main_build); the aux row places the frame step's state page; the obs and spin rows
// go into hle.c's gate table at their guest addresses, which on Apple holds the gate blocks' return addresses (dm_aot_gate dispatches them: aot_run.c). Then every module is
// relocated and bound (dm_aot_bind: host functions and unimplemented imports at their guest-visible slots), and its pack loaded with the image's identity (aot_arena.c)
int dm_aot_load_module_pack(module_t *m, const char *dir, const uint8_t sha256[32], uint64_t image_size, uint64_t text_end, uint64_t bss, char *err, size_t errn);
// aot.h's (this file is compiled into unit tests that include it without the engine's headers): the manifest's gate rows against the pack's gate sites
#ifndef DM_AOT_GATE_ROW
#define DM_AOT_GATE_ROW
typedef struct dm_aot_gate_row { uint64_t offset, arg; uint32_t kind; } dm_aot_gate_row;
#endif
enum { AOT_GATE_OBS = 1, AOT_GATE_SPIN = 2 };   // include/nxaot/aot_module.h NXAOT_GATE_OBS, NXAOT_GATE_SPIN
int dm_aot_pack_check_gates(const module_t *m, const dm_aot_gate_row *rows, int n, int *unserved, char *err, size_t errn);
typedef struct { uint64_t off, param, word; uint32_t num; } aot_obs_t;
typedef struct { uint64_t head, edge; } aot_edge_t;
typedef struct {
    char module[64], build_id[72], sha256[65];
    uint64_t text_end, ro_off, data_off, bss, image_size, aux_off, aux_len, fps_state;
    int layout, aux, rows[4];   // rows: data, cut, oracle (T, J and W), comments
    aot_obs_t *obs; int nobs;
    aot_edge_t *edge; int nedge, nspin;
    uint8_t sha[32];            // the image's, computed
} aot_manifest_t;

static uint8_t *aot_read(const char *path, size_t *n) {   // a whole file, NUL-terminated; NULL when it cannot be read
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *p = NULL; size_t cap = 0, len = 0, got;
    do {
        if (len + 65536 + 1 > cap) { cap = (len + 65536 + 1) * 2; uint8_t *q = realloc(p, cap); if (!q) { free(p); fclose(f); return NULL; } p = q; }
        got = fread(p + len, 1, 65536, f); len += got;
    } while (got);
    int bad = ferror(f); fclose(f);
    if (bad) { free(p); return NULL; }
    p[len] = 0; *n = len;
    return p;
}
static int aot_hex(const char *s, uint64_t *v) {   // hex with or without 0x, as nxaot's reader takes it
    char *e;
    if (!*s || *s == '-' || *s == '+' || *s == ' ') return 0;
    errno = 0; *v = strtoull(s, &e, 16);
    return !*e && !errno;
}
static int aot_dec(const char *s, uint64_t *v) {
    char *e;
    if (*s < '0' || *s > '9') return 0;
    errno = 0; *v = strtoull(s, &e, 10);
    return !*e && !errno;
}
static int aot_hexbytes(const char *s) { size_t n = strlen(s); if (!n || n % 2) return 0; for (; *s; s++) if (!strchr("0123456789abcdefABCDEF", *s)) return 0; return 1; }

// The manifest's rows into t, every row checked as nxaot's reader checks it; 0, or 1 with the reason in err
static int aot_parse(char *text, aot_manifest_t *t, const char *path, char *err, size_t errn) {
    int line = 0, header = 0;
    for (char *s = text, *next; s && *s; s = next) {
        next = strchr(s, '\n');
        if (next) *next++ = 0;
        line++;
        size_t ln = strlen(s);
        if (ln && s[ln - 1] == '\r') s[--ln] = 0;
        if (!ln) continue;
        char *f[16]; int nf = 0;
        f[nf++] = s;
        for (char *p = s; *p && nf < 16; p++) if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        uint64_t a = 0, b = 0, c = 0, d = 0;
        if (!header) {
            if (nf != 4 || strcmp(f[0], "# nxaot-manifest 1") || !*f[1] || strlen(f[1]) >= sizeof t->module || !*f[2] || strlen(f[2]) >= sizeof t->build_id ||
                strlen(f[3]) != 64 || !aot_hexbytes(f[3]) || !aot_hexbytes(f[2])) {
                snprintf(err, errn, "%s line %d: not a manifest of version 1 ('# nxaot-manifest 1', the module, the build ID in hex, the SHA-256 of the image)", path, line); return 1;
            }
            snprintf(t->module, sizeof t->module, "%s", f[1]); snprintf(t->build_id, sizeof t->build_id, "%s", f[2]); snprintf(t->sha256, sizeof t->sha256, "%s", f[3]);
            header = 1; continue;
        }
        const char *k = f[0], *bad = NULL;
        if (k[0] == '#') { t->rows[3]++; continue; }
        if (!strcmp(k, "layout")) {
            if (nf != 11 || strcmp(f[1], "text_end") || strcmp(f[3], "ro_off") || strcmp(f[5], "data_off") || strcmp(f[7], "bss") || strcmp(f[9], "image_size") ||
                !aot_hex(f[2], &t->text_end) || !aot_hex(f[4], &t->ro_off) || !aot_hex(f[6], &t->data_off) || !aot_hex(f[8], &t->bss) || !aot_hex(f[10], &t->image_size))
                bad = "layout: text_end <hex> ro_off <hex> data_off <hex> bss <hex> image_size <hex>";
            else if (t->layout++) bad = "a second layout row";
            else if (!(t->text_end <= t->ro_off && t->ro_off <= t->data_off && t->data_off <= t->image_size)) bad = "layout: text_end <= ro_off <= data_off <= image_size does not hold";
        } else if (!strcmp(k, "aux")) {
            if (nf != 5 || strcmp(f[3], "fps_state") || !aot_hex(f[1], &t->aux_off) || !aot_hex(f[2], &t->aux_len) || !aot_hex(f[4], &t->fps_state)) bad = "aux: <offset> <length> fps_state <offset>";
            else if (t->aux++) bad = "a second aux row";
        } else if (!strcmp(k, "data") || !strcmp(k, "oracle-W")) {
            if (nf != 3 || !aot_hex(f[1], &a) || !aot_hex(f[2], &b) || b < a) bad = "data / oracle-W: <start> <end> with start <= end";
            else t->rows[k[0] == 'd' ? 0 : 2]++;
        } else if (!strcmp(k, "obs")) {
            if (nf != 5 || !aot_hex(f[1], &a) || !aot_dec(f[2], &b) || b > 0xFF || !aot_hex(f[3], &c) || !aot_hex(f[4], &d) || d > 0xFFFFFFFFu)
                bad = "obs: <offset> <number (decimal, below 256)> <parameter> <original instruction word>";
            else {
                aot_obs_t *o = realloc(t->obs, (size_t)(t->nobs + 1) * sizeof *o);
                if (!o) die("aot: no memory for a manifest");
                t->obs = o; t->obs[t->nobs++] = (aot_obs_t){a, c, d, (uint32_t)b};
            }
        } else if (!strcmp(k, "spin") || !strcmp(k, "oracle-T")) {
            if (nf != 3 || !aot_hex(f[1], &a)) bad = "spin / oracle-T: <offset> <offset>[,<offset>...]";
            for (char *p = nf == 3 ? f[2] : NULL, *q; !bad && p; p = q) {
                q = strchr(p, ',');
                if (q) *q++ = 0;
                if (!aot_hex(p, &b)) { bad = "spin / oracle-T: <offset> <offset>[,<offset>...]"; break; }
                if (k[0] == 's') {
                    aot_edge_t *e = realloc(t->edge, (size_t)(t->nedge + 1) * sizeof *e);
                    if (!e) die("aot: no memory for a manifest");
                    t->edge = e; t->edge[t->nedge++] = (aot_edge_t){a, b};
                }
            }
            if (!bad) { if (k[0] == 's') t->nspin++; else t->rows[2]++; }
        } else if (!strcmp(k, "cut")) {
            if (nf < 5 || !aot_hex(f[1], &a) || !aot_hexbytes(f[2]) || !aot_hexbytes(f[3]) || strlen(f[2]) != strlen(f[3])) bad = "cut: <offset> <old hex> <new hex> <reason>";
            else t->rows[1]++;
        } else if (!strcmp(k, "oracle-J")) {
            if (nf != 4 || !aot_hex(f[1], &a) || !aot_hex(f[2], &b) || !aot_dec(f[3], &c) || (c != 1 && c != 2)) bad = "oracle-J: <br offset> <base> <width: 1 or 2>";
            else t->rows[2]++;
        } else bad = "an unknown row kind";
        if (bad) { snprintf(err, errn, "%s line %d: %s", path, line, bad); return 1; }
    }
    if (!header) { snprintf(err, errn, "%s: empty, not a manifest", path); return 1; }
    if (!t->layout) { snprintf(err, errn, "%s: no layout row", path); return 1; }
    return 0;
}

static void aot_hexstr(const uint8_t *b, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }

// Module m's manifest and image from dir: checked, mapped, its gates registered, its dynamic section parsed. 0, or 1 with the reason in err
static int aot_map_module(module_t *m, const char *dir, aot_manifest_t *t, char *err, size_t errn) {
    char path[1024], ipath[1024];
    snprintf(path, sizeof path, "%s/%s.aot.tsv", dir, m->name);
    snprintf(ipath, sizeof ipath, "%s/%s.image.bin", dir, m->name);
    size_t n = 0, len = 0;
    char *text = (char *)aot_read(path, &n);
    if (!text) { snprintf(err, errn, "%s: cannot read it (errno %d): rewrite the module with dm/tools/patch_nso.py <NSO> <out> %s --aot %s", path, errno, m->name, dir); return 1; }
    int bad = aot_parse(text, t, path, err, errn);
    free(text);
    if (bad) return 1;
    if (strcmp(t->module, m->name)) { snprintf(err, errn, "%s is the manifest of module %s, not of %s", path, t->module, m->name); return 1; }
    uint8_t *img = aot_read(ipath, &len);
    if (!img) { snprintf(err, errn, "%s: cannot read the image %s names (errno %d)", ipath, path, errno); return 1; }
    char have[65], want[65];
    plat_sha256(img, len, t->sha);
    aot_hexstr(t->sha, 32, have);
    snprintf(want, sizeof want, "%s", t->sha256);
    for (char *c = want; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
    const uint64_t end = t->image_size + t->bss;
    const char *why = NULL;
    if (len != t->image_size) why = "its size is not the layout's image_size";
    else if (strcmp(have, want)) why = "its SHA-256 is not the manifest's";
    else if (t->text_end & 3 || !t->text_end) why = "the layout's text_end is no whole number of instructions";
    else if (t->aux && (t->fps_state < t->data_off || t->fps_state + 0x40 > end || t->aux_off < t->data_off || t->aux_len > t->image_size || t->aux_off > t->image_size - t->aux_len))
        why = "its aux row lies outside the image's data";
    for (int i = 0; !why && i < t->nobs; i++) {
        uint32_t w;
        if (t->obs[i].off >= t->text_end || t->obs[i].off & 3 || !t->obs[i].num) { why = "an obs row names no instruction of the text, or number 0"; break; }
        memcpy(&w, img + t->obs[i].off, 4);
        if (w != t->obs[i].word) { why = "an obs row's original word is not the image's (the gate's instruction must be the original one)"; break; }
    }
    for (int i = 0; !why && i < t->nedge; i++)
        if (t->edge[i].head >= t->text_end || t->edge[i].edge >= t->text_end || (t->edge[i].head | t->edge[i].edge) & 3) why = "a spin row names no instruction of the text";
    if (why) {
        snprintf(err, errn, "%s does not match %s (%s): image %zu bytes, SHA-256 %s; manifest image_size %llu, SHA-256 %s. Rewrite the module again with patch_nso.py --aot",
                 ipath, path, why, len, have, (unsigned long long)t->image_size, want);
        free(img); return 1;
    }
    aot_place(m, img, len, (size_t)t->bss, t->text_end, t->ro_off, t->data_off);
    free(img);
    snprintf(m->build_id, sizeof m->build_id, "%.32s", t->build_id);   // the first 16 bytes, as fixups.tsv's BUILD_ID on Apple
    if (t->aux) { m->fps_state_off = t->fps_state; dm_fps_state = m->base + t->fps_state; g_fps_mod = m->name; }   // the frame step's state page (install_fps_state on Apple)
    parse_dynamic(m);
    for (int i = 0; i < t->nobs; i++) gate_register((uint64_t)(uintptr_t)m->base + t->obs[i].off, "obs", (int)(t->obs[i].num | t->obs[i].param << 8), t->obs[i].off);
    for (int i = 0; i < t->nedge; i++) gate_register((uint64_t)(uintptr_t)m->base + t->edge[i].edge, "spin", 0, t->edge[i].head);
    // The gates are the host's observers and the spin waits' back-off (Apple's gates.tsv): the pack's code calls them (nxaot_rt_gate, design 1.1) at the sites its
    // descriptor lists, which load_modules checks against these rows once the pack is loaded (aot_arena.c dm_aot_pack_check_gates)
    LOG("%s: base %p size %#zx (behaviour image %#zx, bss %#llx; build %s; %d data blocks, %d cuts, %d oracle rows%s; x86-64 engine)", m->name, (void *)m->base, m->size, len,
        (unsigned long long)t->bss, m->build_id, t->rows[0], t->rows[1], t->rows[2], t->aux ? ", frame step state page" : "");
    return 0;
}

// ---- NROs on the game-module path (nn::ro, hle_ro.c) ----
// The folder's nro.tsv has Apple's rows (name, SHA-256, size; "refused" rows for the online SDKs the pack leaves out: nro_refused). The NRO itself is the image the game
// hands ro::LoadModule (as Horizon makes the game's buffer the module): at its first load it is mapped one to one in the arena from those bytes, as loader_add_image maps
// an image, and its pack <dir>/<name>.aot.so loaded with their identity (nxaot build <name>.nro -o <object> --shared <dir>/<name>.aot.so: the pack of the NRO file,
// whose SHA-256 is the one nro.tsv lists). From then on it is an NRO module as on Apple: nro_load relocates it and binds the earlier modules' imports, nro_unload undoes
// that, a later load restores its data. g_mod has room for every row (allocate_modules counts them), so module pointers never move
typedef struct { char name[16]; uint8_t sha[32]; uint32_t size; int no_pack; } aot_nro_t;   // no_pack: its pack failed to load once (reported then; later loads fail at once)
static aot_nro_t *g_aot_nro; static int g_aot_nnro;
static char *g_aot_dir;   // load_modules' folder: the packs of the NROs lie there
static void aot_read_nros(const char *dir, FILE *f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[16], hex[65]; unsigned size;
        if (sscanf(line, "%15s %64s %u", name, hex, &size) != 3 || strlen(hex) != 64) die("[RED] aot: %s/nro.tsv: bad row %s", dir, line);
        uint8_t sha[32];
        for (int k = 0; k < 32; k++) sscanf(hex + 2 * k, "%2hhx", &sha[k]);
        if (!strcmp(name, "refused")) {   // an online SDK the pack left out (dm/tools/nro_collect.py ONLINE_SDKS): LoadModule refuses its hash
            if (g_nrefused < (int)(sizeof g_refused / sizeof *g_refused)) memcpy(g_refused[g_nrefused], sha, 32);
            g_nrefused++; continue;
        }
        aot_nro_t *r = realloc(g_aot_nro, (size_t)(g_aot_nnro + 1) * sizeof *r);
        if (!r) die("aot: no memory for nro.tsv");
        g_aot_nro = r;
        snprintf(r[g_aot_nnro].name, sizeof r[g_aot_nnro].name, "%s", name); memcpy(r[g_aot_nnro].sha, sha, 32); r[g_aot_nnro].size = size;
        g_aot_nnro++;
    }
    fclose(f);
    LOG("aot: %s/nro.tsv lists %d NROs (mapped from the game's image at their first ro::LoadModule, each with its pack <name>" AOT_PACK_SUFFIX "), %d refused", dir, g_aot_nnro,
        g_nrefused);
}

module_t *loader_nro_map(const uint8_t *image, uint32_t n, const uint8_t sha[32], char *err, size_t errn) {
    char hx[65];
    aot_hexstr(sha, 32, hx);
    aot_nro_t *row = NULL;
    for (int i = 0; i < g_aot_nnro; i++) if (!memcmp(g_aot_nro[i].sha, sha, 32)) { row = &g_aot_nro[i]; break; }
    if (!row) {
        snprintf(err, errn, "the NRO with SHA-256 %s (%u bytes) has no row in %s/nro.tsv, so it has no pack: list it there (<name> <SHA-256> <size>) and pack it beside it "
                 "(nxaot build <name>.nro -o <object> --shared %s/<name>" AOT_PACK_SUFFIX ")", hx, n, g_aot_dir ? g_aot_dir : "the modules' folder", g_aot_dir ? g_aot_dir : "<folder>");
        return NULL;
    }
    if (row->no_pack) { snprintf(err, errn, "%s (SHA-256 %s): its pack did not load (the reason is above)", row->name, hx); return NULL; }
    uint32_t hdr[8];   // the NRO header from +0x18: size, flags, text, read-only data and data (offset, size), bss size
    if (n < 0x80 || memcmp(image + 0x10, "NRO0", 4)) { snprintf(err, errn, "%s (SHA-256 %s): not an NRO", row->name, hx); return NULL; }
    memcpy(hdr, image + 0x18, sizeof hdr);
    const uint64_t text_end = hdr[3], ro_off = hdr[4], data_off = hdr[6], data_size = hdr[7];
    uint32_t bss; memcpy(&bss, image + 0x38, 4);
    if (hdr[2] || text_end > ro_off || ro_off > data_off || data_off + data_size > n || hdr[0] != n) {
        snprintf(err, errn, "%s (SHA-256 %s): the NRO header's segments do not fit its %u bytes", row->name, hx, n); return NULL;
    }
    pthread_rwlock_wrlock(&g_modules_mu);
    if (g_nmod >= g_mod_capacity) die("[RED] aot: no module slot for NRO %s (%d)", row->name, g_mod_capacity);
    module_t *m = &g_mod[g_nmod];   // filled here, counted (g_nmod) once its pack loaded
    memset(m, 0, sizeof *m);
    strlcpy(m->name, row->name, sizeof m->name);
    m->nro = 1; m->nro_size = n; memcpy(m->sha256, sha, 32);
    aot_place(m, image, n, bss, text_end, ro_off, data_off);
    parse_dynamic(m);
    m->pristine_len = data_size;   // initialized data (nro_load keeps a copy at the first load, restores it at the next)
    pthread_rwlock_unlock(&g_modules_mu);
    if (dm_aot_load_module_pack(m, g_aot_dir, sha, n, text_end, bss, err, errn)) {   // the slot stays unused; the image stays mapped, once (no_pack)
        row->no_pack = 1;
        free(m->symh); memset(m, 0, sizeof *m);
        return NULL;
    }
    pthread_rwlock_wrlock(&g_modules_mu);
    g_nmod++;
    pthread_rwlock_unlock(&g_modules_mu);
    LOG("NRO %s mapped from the game's image at %p (%u bytes, bss %#x; x86-64 engine)", m->name, (void *)m->base, n, bss);
    return m;
}

void load_modules(const char *dir, const char **names, int n) {
    const uint64_t t0 = now_ns();
    char err[1200];
    FILE *nros = open_nros(dir);
    allocate_modules(n, nros);   // room for the NROs nro.tsv lists too
    g_aot_dir = strdup(dir);
    aot_manifest_t *man = calloc((size_t)n, sizeof *man);
    if (!man) die("aot: no memory for %d manifests", n);
    for (int i = 0; i < n; i++) {   // every module mapped and parsed first, then relocated (cross-module symbols must all be in place), as on Apple
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        if (aot_map_module(m, dir, &man[i], err, sizeof err)) die("[RED] aot: %s", err);
        m->loaded = 1; g_loaded[g_nloaded++] = m; g_nmod++;
    }
    force_init();
    for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]);
    force_check();
    if (nros) aot_read_nros(dir, nros);   // mapped when the game loads them (hle_ro.c, loader_nro_map)
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        if (dm_aot_load_module_pack(m, dir, man[i].sha, man[i].image_size, man[i].text_end, man[i].bss, err, sizeof err)) die("[RED] aot: %s", err);
        // The manifest's gates against the sites the pack's code calls (design 1.1): a pack of another manifest is refused; a row it does not serve is a [RED] line
        dm_aot_gate_row *rows = calloc((size_t)(man[i].nobs + man[i].nedge) + 1, sizeof *rows);
        if (!rows) die("aot: no memory for %s's gate rows", m->name);
        int nrows = 0, unserved = 0;
        for (int k = 0; k < man[i].nobs; k++) rows[nrows++] = (dm_aot_gate_row){man[i].obs[k].off, man[i].obs[k].num | man[i].obs[k].param << 8, AOT_GATE_OBS};
        for (int k = 0; k < man[i].nedge; k++) rows[nrows++] = (dm_aot_gate_row){man[i].edge[k].edge, man[i].edge[k].head, AOT_GATE_SPIN};
        if (dm_aot_pack_check_gates(m, rows, nrows, &unserved, err, sizeof err)) die("[RED] aot: %s", err);
        if (nrows) LOG("aot: %s: %d gates (%d observation gates, %d spin wait back-edges) called by its pack, %d not", m->name, nrows - unserved, man[i].nobs, man[i].nedge, unserved);
        free(rows);
        free(man[i].obs); free(man[i].edge);
    }
    free(man);
    { void hlecount_seal(void); hlecount_seal(); }
    LOG("aot: %d modules from %s mapped, relocated and their packs loaded in %.1f ms", n, dir, (now_ns() - t0) / 1e6);
}
#endif
