// Synthetic modules exercise real relocation and nn::ro calls; no game files or executable guest code required.
#include "../loader.c"
void dm_thread_service_connect(void) {}   // threads started so far have nothing to wait for here
#include "../hle_ro.c"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>
void mtl_present_screen(uint32_t w, uint32_t h) { (void)w; (void)h; }   // loader.c: a screen taller than 16:9; no renderer here
// 40 fps (loader.c, compiled here): only a stub that takes ticks of 1/120 s (the FRAME_STEP_120 capability in its fixups table) gets it; the loader writes the step's unit (+0x38)
// for it, 120, at every start, and 0 for every other stub. Elsewhere DM_FPS_TARGET=40 holds 30
static float page_target(const uint8_t *page) { float t; memcpy(&t, page, 4); return t; }
static uint32_t page_unit(const uint8_t *page) { uint32_t u; memcpy(&u, page + FSTEP_UNIT_OFF, 4); return u; }
static void fps_40(void) {
    _Alignas(8) uint8_t page[0x100] = {0};
    module_t m = {.base = page, .size = sizeof page};   // a stub in vsyncs (Tears of the Kingdom's, Breath of the Wild's)
    strcpy(m.name, "main");
    unsetenv("DM_RENDER_SCALE"); unsetenv("DM_DYNAMIC_RESOLUTION"); unsetenv("DM_SCREEN_ASPECT");
    install_fps_state(&m);
    setenv("DM_FPS_TARGET", "40", 1); dm_fps_state_apply();
    assert(page_target(page) == 30 && page_unit(page) == 0);   // no tick stub: 30, the page stays in vsyncs
    m.fstep_t120 = 1; install_fps_state(&m);   // Ocarina of Time's: FRAME_STEP_120
    dm_fps_state_apply();
    assert(page_target(page) == 40 && page_unit(page) == 120);
    setenv("DM_FPS_TARGET", "60", 1); dm_fps_state_apply(); assert(page_target(page) == 60 && page_unit(page) == 120);   // the unit at every target
    setenv("DM_FPS_TARGET", "30", 1); dm_fps_state_apply(); assert(page_target(page) == 30 && page_unit(page) == 120);
    setenv("DM_FPS_TARGET", "0", 1); dm_fps_state_apply(); assert(page_target(page) == 0 && page_unit(page) == 120);
    setenv("DM_FPS_TARGET", "45", 1); dm_fps_state_apply(); assert(page_target(page) == 0);   // not a frame rate it takes: the game's own step
    m.fstep_t120 = 0; install_fps_state(&m); setenv("DM_FPS_TARGET", "60", 1); dm_fps_state_apply(); assert(page_unit(page) == 0);   // back to a stub in vsyncs: 0 again
    unsetenv("DM_FPS_TARGET"); dm_fps_state = NULL; g_fps_mod = NULL; g_fstep_t120 = 0;
    puts("40 fps only for a stub in ticks of 1/120 s (FRAME_STEP_120), the step's unit in the state page: OK");
}

// ALIAS_CUT rows in fixups.tsv: restored at load on a 64 GB Mac alias region, kept under a smaller DM_ALIAS_SIZE or DM_KEEP_ALIAS_CUTS=1, and fail-closed on byte mismatch.
static void alias_cuts(void) {
    char dir[] = "/tmp/dm_alias_cut_XXXXXX";
    assert(mkdtemp(dir));
    char path[256]; snprintf(path, sizeof path, "%s/test.fixups.tsv", dir);
    FILE *f = fopen(path, "w"); assert(f);
    fputs("# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n"
          "# TEXT_END\t0x4000\n# RO_OFF\t0x4000\n# DATA_OFF\t0x4000\n"
          "# KEY_K18_IDX\t0\n# KEY_KTLS_IDX\t1\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t3\n"
          "# ALIAS_CUT\t0x10\te001c0d2\t2000c0d2\n", f);
    fclose(f);
    module_t m = {0}; strcpy(m.name, "test");
    read_layout(&m, dir);
    unlink(path); rmdir(dir);
    assert(m.n_alias_cut == 1 && m.alias_cut[0].off == 0x10 && m.alias_cut[0].len == 4);
    uint8_t img[0x40] = {0}; const uint8_t orig[4] = {0xe0, 0x01, 0xc0, 0xd2}, cut[4] = {0x20, 0x00, 0xc0, 0xd2};
    m.base = img;
    // Default Mac alias region (64 GB): restores original bytes
    memcpy(img + 0x10, cut, 4); unsetenv("DM_ALIAS_SIZE"); unsetenv("DM_KEEP_ALIAS_CUTS");
    undo_alias_cuts(&m);
    assert(memcmp(img + 0x10, orig, 4) == 0);
    // Small alias region (iPhone layout): keeps cut bytes
    memcpy(img + 0x10, cut, 4); setenv("DM_ALIAS_SIZE", "0x118000000", 1);
    undo_alias_cuts(&m);
    assert(memcmp(img + 0x10, cut, 4) == 0);
    // Explicit DM_KEEP_ALIAS_CUTS=1 (region_check): keeps cut bytes even at 64 GB
    unsetenv("DM_ALIAS_SIZE"); setenv("DM_KEEP_ALIAS_CUTS", "1", 1);
    undo_alias_cuts(&m);
    assert(memcmp(img + 0x10, cut, 4) == 0);
    // Byte mismatch at cut site fails closed (aborts)
    unsetenv("DM_KEEP_ALIAS_CUTS"); memset(img + 0x10, 0xaa, 4);
    fflush(stdout); fflush(stderr);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { freopen("/dev/null", "w", stderr); undo_alias_cuts(&m); _exit(0); }
    int st; assert(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st));
    puts("ALIAS_CUT read_layout and undo_alias_cuts (restore at 64 GB, keep on small alias / DM_KEEP_ALIAS_CUTS, mismatch abort): OK");
}

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
static uint64_t missing;
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; return (uint64_t)&missing; }
int hle_implements(const char *name) { return !strcmp(name, "vkGetInstanceProcAddr") || !strcmp(name, "nvnBootstrapLoader"); }
int svc_nro_deactivate(module_t *m) { m->loaded = 0; return 1; }   // no memory mappings in this fixture
void dm_exl_sync_all(void) {}

typedef struct {
    Elf64_Sym sym[4];
    char names[32];
    Elf64_Dyn dyn[3];
    Elf64_Rela rel[3];
    uint64_t slots[3], value, bss;
} image;
static image images[3];
static uint8_t raw[2][128];
static ro_module handles[2];
static int inits, finis;
static void init(void) { assert(guest_sym("shared")); inits++; }
static void fini(void) { assert(guest_sym("shared")); finis++; }

static void fixture(int i) {
    image *im = &images[i]; module_t *m = &g_mod[i];
    m->base = (uint8_t *)im; m->size = sizeof *im;
    m->guest_size = offsetof(image, bss);
    snprintf(m->name, sizeof m->name, "test%d", i);
    memcpy(im->names, "\0shared\0late\0absent\0", 20);
    m->symtab = im->sym; m->strtab = im->names; m->nsym = 4; m->dyn = im->dyn;
    im->sym[1] = (Elf64_Sym){.st_name = 1, .st_info = 1 << 4, .st_shndx = i ? 1 : 0, .st_value = offsetof(image, value)};
    im->sym[2] = (Elf64_Sym){.st_name = 8, .st_info = STB_WEAK << 4, .st_shndx = i == 1 ? 1 : 0, .st_value = offsetof(image, value)};
    im->sym[3] = (Elf64_Sym){.st_name = 13, .st_info = STB_WEAK << 4};
    im->dyn[0] = (Elf64_Dyn){.d_tag = DT_RELA, .d_un.d_ptr = offsetof(image, rel)};
    im->dyn[1] = (Elf64_Dyn){.d_tag = DT_RELASZ, .d_un.d_val = sizeof im->rel};
    for (int k = 0; k < 3; k++) im->rel[k] = (Elf64_Rela){.r_offset = offsetof(image, slots) + 8 * k, .r_info = (uint64_t)(k + 1) << 32 | R_AARCH64_ABS64, .r_addend = k == 0 ? 8 : 0};
    build_symhash(m);
    if (!i) { m->loaded = 1; g_loaded[g_nloaded++] = m; return; }
    m->nro = 1; m->data_off = offsetof(image, slots); m->pristine_len = offsetof(image, bss) - m->data_off; m->key_page_off = sizeof *im;
    im->value = 123;
    uint8_t *h = raw[i - 1]; memcpy(h + 0x10, "NRO0", 4); *(uint32_t *)(h + 0x18) = sizeof raw[0]; h[0x40] = (uint8_t)i;
    *(uint32_t *)(h + 0x38) = 32; m->nro_size = sizeof raw[0]; CC_SHA256(h, sizeof raw[0], m->sha256);
    m->init = (uintptr_t)init - (uintptr_t)m->base; m->fini = (uintptr_t)fini - (uintptr_t)m->base;
}
static void load(int i) { assert(h_ro_LoadModule(&handles[i - 1], raw[i - 1], NULL, 32, 1) == 0); }
static void unload(int i) { h_ro_UnloadModule(&handles[i - 1]); assert(!g_mod[i].loaded); }

int main(void) {
    fps_40();
    alias_cuts();
    FILE *manifest = tmpfile(); assert(manifest);
    fputs("# NRO manifest\n\nrefused 00 128\n", manifest);
    for (int i = 0; i < 1432; i++) fprintf(manifest, "nro%d 00 128\n", i);
    rewind(manifest); allocate_modules(3, manifest);
    assert(g_mod_capacity == 1435 && ftell(manifest) == 0); fclose(manifest);
    g_nmod = 3; for (int i = 0; i < 3; i++) fixture(i);
    force_init();
    bool (*module_range)(uintptr_t *, size_t *, uintptr_t) =
        (bool (*)(uintptr_t *, size_t *, uintptr_t))force_lookup("_ZN2nn2ro21GetModuleAddressRangeEPmS1_m");
    assert(module_range);
    Result (*lookup_at_base)(uintptr_t *, uintptr_t, const char *) =
        (Result (*)(uintptr_t *, uintptr_t, const char *))force_lookup("_ZN2nn2ro18LookupModuleSymbolEPmmPKc");
    assert(lookup_at_base);
    uintptr_t base = 0, main_base = (uintptr_t)&images[0], nro_base = (uintptr_t)&images[1];
    size_t span = 0;
    assert(module_range(&base, &span, main_base + 1) && base == main_base && span == offsetof(image, bss));
    assert(!module_range(&base, &span, main_base + span) && base == main_base && span == offsetof(image, bss));
    assert(!module_range(&base, &span, nro_base));
    uintptr_t found = 1;
    assert(lookup_at_base(&found, nro_base, "shared") == 0xa16 && found == 1);
    relocate(&g_mod[0]);
    assert(images[0].slots[0] == (uint64_t)&missing + 8);
    assert(images[0].slots[1] == 0 && images[0].slots[2] == 0);
    assert(!guest_sym("shared"));
    size_t size = 0; assert(h_ro_GetBufferSize(&size, raw[0]) == 0 && size == 32);

    assert(!g_mod[1].pristine && !g_mod[2].pristine);
    load(2); load(1);   // deliberately opposite to manifest/hash order
    assert(module_range(&base, &span, nro_base) && base == nro_base && span == offsetof(image, bss));
    assert(g_mod[1].pristine && g_mod[2].pristine);
    uint64_t second = (uint64_t)&images[2].value, first = (uint64_t)&images[1].value;
    assert(lookup_at_base(&found, nro_base, "shared") == 0 && found == first);
    assert(lookup_at_base(&found, nro_base + 1, "shared") == 0xa16 && found == first);
    assert(lookup_at_base(&found, main_base, "shared") == 0xa16 && found == first);
    assert((uint64_t)guest_sym("shared") == second);
    assert(images[0].slots[0] == second + 8 && images[1].slots[0] == second + 8);
    assert(images[0].slots[1] == first && images[2].slots[1] == first);
    assert(images[0].slots[2] == 0);
    uintptr_t p; assert(h_ro_LookupModuleSymbol(&p, &handles[0], "shared") == 0 && p == first);
    assert(h_ro_LookupSymbol(&p, "shared") == 0 && p == second);
    assert(h_ro_LookupSymbol(&p, "vkGetInstanceProcAddr") == 0 && p == (uintptr_t)&missing);
    assert(h_ro_LookupSymbol(&p, "vkNotImplemented") != 0 && p == (uintptr_t)&missing);
    assert(h_ro_LookupSymbol(&p, "nvnBootstrapLoader") != 0);
    assert(h_ro_LoadModule(&handles[0], raw[0], NULL, 32, 1) != 0);

    unload(2);
    assert((uint64_t)guest_sym("shared") == first && images[0].slots[0] == first + 8);
    assert(h_ro_LookupModuleSymbol(&p, &handles[1], "shared") != 0);
    images[2].value = 999; images[2].bss = 456; load(2);
    assert(images[2].value == 123 && images[2].bss == 0 && images[2].slots[0] == first + 8);
    assert((uint64_t)guest_sym("shared") == first);
    unload(1);
    assert(!module_range(&base, &span, nro_base));
    assert(lookup_at_base(&found, nro_base, "shared") == 0xa16 && found == first);
    assert(images[0].slots[0] == second + 8 && images[0].slots[1] == 0 && images[2].slots[1] == 0);
    unload(2);
    assert(!guest_sym("shared") && images[0].slots[0] == (uint64_t)&missing + 8);
    assert(inits == 3 && finis == 3);

    // The last manifest slot must support the same lookup, relocation and unload lifecycle as the first ones.
    module_t *first_slot = &g_mod[1];
    int last = g_mod_capacity - 1;
    g_mod[last] = g_mod[2]; memset(&g_mod[2], 0, sizeof g_mod[2]); g_nmod = g_mod_capacity;
    load(2);
    assert(handles[1].module == &g_mod[last] && &g_mod[1] == first_slot);
    assert(nro_find(g_mod[last].sha256) == &g_mod[last]);
    assert((uint64_t)guest_sym("shared") == second && images[0].slots[0] == second + 8);
    h_ro_UnloadModule(&handles[1]); assert(!g_mod[last].loaded && !guest_sym("shared"));

    memcpy(g_refused[0], g_mod[1].sha256, 32); g_nrefused = 1;
    uint8_t nrr[0x380] = {0}; memcpy(nrr, "NRR0", 4); *(uint32_t *)(nrr + 0x340) = 0x350; *(uint32_t *)(nrr + 0x344) = 1;
    memcpy(nrr + 0x350, g_mod[1].sha256, 32);
    assert(h_ro_RegisterModuleInfo(NULL, nrr) != 0);
    assert(h_ro_LoadModule(&handles[0], raw[0], NULL, 32, 1) != 0 && !g_mod[1].loaded);

    // A JIT plugin is loaded alone (hle_jit.c), and a second CreateJitEnvironment after DestroyJitEnvironment loads it again: from its data as rewritten and a zero bss,
    // relocated against itself only. Start from a module never loaded, as the plugin is.
    module_t *alone = &g_mod[1]; free(alone->pristine); alone->pristine = NULL; alone->loads = 0; alone->loaded = 0;
    memset(images[1].slots, 0, sizeof images[1].slots); images[1].value = 123; images[1].bss = 0;
    uint64_t own = (uint64_t)&images[1].value;
    nro_load_alone(alone);
    assert(alone->pristine && alone->loads == 1 && alone->loaded);
    assert(images[1].slots[0] == own + 8 && images[1].slots[1] == own && images[1].slots[2] == 0);
    assert(!guest_sym("shared") && alone->alone);
    // Not linked with the game: a linked NRO loaded and unloaded meanwhile binds nothing into it, even a weak import the NRO defines (the main module's import is bound)
    image *lk = &images[2]; module_t *linked = &g_mod[last];
    lk->sym[3] = (Elf64_Sym){.st_name = 13, .st_info = STB_WEAK << 4, .st_shndx = 1, .st_value = offsetof(image, value)}; free(linked->symh); build_symhash(linked);
    load(2);
    assert(images[0].slots[2] == (uint64_t)&images[2].value && images[1].slots[2] == 0);
    h_ro_UnloadModule(&handles[1]);
    assert(images[0].slots[2] == 0 && images[1].slots[2] == 0 && alone->loaded);
    // Loaded again while it is loaded (a second environment of the plugin before the first is destroyed): refused, its state not reset under it
    fflush(stdout); fflush(stderr);
    pid_t child = fork();
    if (!child) { nro_load_alone(alone); _exit(0); }
    int st; waitpid(child, &st, 0); assert(WIFSIGNALED(st) && images[1].value == 123);
    images[1].value = 999; images[1].bss = 456; images[1].slots[0] = images[1].slots[1] = images[1].slots[2] = 7;
    nro_unload_alone(alone);   // as h_jit_Destroy ends it, after the plugin's fini
    assert(!alone->loaded && !alone->alone);
    nro_load_alone(alone);
    assert(alone->loads == 2 && images[1].value == 123 && images[1].bss == 0);
    assert(images[1].slots[0] == own + 8 && images[1].slots[1] == own && images[1].slots[2] == 0);
    assert(!guest_sym("shared"));
    nro_unload_alone(alone);
    // No initialized data at all: the first load snapshots nothing, a reload zeroes the bss
    size_t data_off = alone->data_off, plen = alone->pristine_len; free(alone->pristine); alone->pristine = NULL;
    alone->data_off = offsetof(image, bss); alone->pristine_len = 0;
    nro_load_alone(alone); images[1].bss = 5; nro_unload_alone(alone);
    nro_load_alone(alone); assert(images[1].bss == 0 && alone->pristine); nro_unload_alone(alone);
    alone->data_off = data_off; alone->pristine_len = plen;

    for (int i = 0; i < g_nmod; i++) { free(g_mod[i].symh); free(g_mod[i].pristine); }
    free(g_mod); free(g_loaded);
    free(g_force_hits); free(g_fidx);
    puts("NRO relocation, load order, lifecycle, weak symbols, refusal and loading alone (reload, isolation from linked loads, no second load, no initialized data): OK");
    return 0;
}
