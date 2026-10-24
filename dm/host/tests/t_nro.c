// Synthetic modules exercise real relocation and nn::ro calls; no game files or executable guest code required.
#include "../loader.c"
#include "../hle_ro.c"
#include <assert.h>
#include <stddef.h>

struct dm_flags g_flag;
uint64_t now_ns(void) { return 0; }
void gate_register(uint64_t a, const char *k, int n, uint64_t o) { (void)a; (void)k; (void)n; (void)o; }
void hlecount_seal(void) {}
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
static uint64_t missing;
uint64_t resolve_import(module_t *m, const char *name) { (void)m; (void)name; return (uint64_t)&missing; }
int hle_implements(const char *name) { (void)name; return 0; }

typedef struct {
    Elf64_Sym sym[4];
    char names[32];
    Elf64_Dyn dyn[3];
    Elf64_Rela rel[3];
    uint64_t slots[3], value;
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
    m->nro = 1; m->data_off = offsetof(image, slots); m->pristine_len = sizeof *im - m->data_off;
    im->value = 123; m->pristine = malloc(m->pristine_len); memcpy(m->pristine, m->base + m->data_off, m->pristine_len);
    uint8_t *h = raw[i - 1]; memcpy(h + 0x10, "NRO0", 4); *(uint32_t *)(h + 0x18) = sizeof raw[0]; h[0x40] = (uint8_t)i;
    *(uint32_t *)(h + 0x38) = 32; m->nro_size = sizeof raw[0]; CC_SHA256(h, sizeof raw[0], m->sha256);
    m->init = (uintptr_t)init - (uintptr_t)m->base; m->fini = (uintptr_t)fini - (uintptr_t)m->base;
}
static void load(int i) { assert(h_ro_LoadModule(&handles[i - 1], raw[i - 1], NULL, 32, 1) == 0); }
static void unload(int i) { h_ro_UnloadModule(&handles[i - 1]); assert(!g_mod[i].loaded); }

// Wide output (loader.c output_aspect): the height stays, the width is height × the aspect rounded to a multiple of 8, never narrower than 16:9; only for a profile with aspect any
static uint64_t aspect_out(const char *aspect, uint64_t preset) {
    static uint8_t page[0x60]; module_t m = {.name = "main", .out_w = 1280, .out_h = 720};
    memset(page, 0, sizeof page); memcpy(page + 0x40, &preset, 8); dm_fps_state = page;
    if (aspect) setenv("DM_ASPECT", aspect, 1); else unsetenv("DM_ASPECT");
    output_aspect(&m); uint64_t o; memcpy(&o, page + 0x40, 8); dm_fps_state = NULL; return o;
}
static void aspect_test(void) {
#define WH(w, h) ((uint64_t)(h) << 32 | (w))
    assert(aspect_out(NULL, 0) == 0);   // wide by the profile, but no front end aspect (front_screen_aspect's weak 0): the game's size
    assert(aspect_out("16:9", 0) == 0 && aspect_out("4:3", 0) == 0);   // never narrower than 16:9
#if DM_GAME_ASPECT_ANY
    assert(aspect_out("2868:1320", 0) == WH(1568, 720));   // iPhone 17 Pro Max at the game's 720 lines
    assert(aspect_out("2868:1320", WH(1920, 1080)) == WH(2344, 1080));   // DM_OUTPUT_SIZE's height
    assert(aspect_out("2556:1179", 0) == WH(1560, 720));   // iPhone 15 / 16
    puts("wide output: 1568x720 at 2868:1320, DM_OUTPUT_SIZE's height kept, never below 16:9: OK");
#else
    assert(aspect_out("2868:1320", 0) == 0);   // a profile without aspect any keeps its size
    puts("wide output: off without the profile key aspect any: OK");
#endif
}
int main(void) {
    g_nmod = 3; for (int i = 0; i < 3; i++) fixture(i);
    relocate(&g_mod[0]);
    assert(images[0].slots[0] == (uint64_t)&missing + 8);
    assert(images[0].slots[1] == 0 && images[0].slots[2] == 0);
    assert(!guest_sym("shared"));
    size_t size = 0; assert(h_ro_GetBufferSize(&size, raw[0]) == 0 && size == 32);

    load(2); load(1);   // deliberately opposite to manifest/hash order
    uint64_t second = (uint64_t)&images[2].value, first = (uint64_t)&images[1].value;
    assert((uint64_t)guest_sym("shared") == second);
    assert(images[0].slots[0] == second + 8 && images[1].slots[0] == second + 8);
    assert(images[0].slots[1] == first && images[2].slots[1] == first);
    assert(images[0].slots[2] == 0);
    uintptr_t p; assert(h_ro_LookupModuleSymbol(&p, &handles[0], "shared") == 0 && p == first);
    assert(h_ro_LoadModule(&handles[0], raw[0], NULL, 32, 1) != 0);

    unload(2);
    assert((uint64_t)guest_sym("shared") == first && images[0].slots[0] == first + 8);
    assert(h_ro_LookupModuleSymbol(&p, &handles[1], "shared") != 0);
    images[2].value = 999; load(2);
    assert(images[2].value == 123 && images[2].slots[0] == first + 8);
    assert((uint64_t)guest_sym("shared") == first);
    unload(1);
    assert(images[0].slots[0] == second + 8 && images[0].slots[1] == 0 && images[2].slots[1] == 0);
    unload(2);
    assert(!guest_sym("shared") && images[0].slots[0] == (uint64_t)&missing + 8);
    assert(inits == 3 && finis == 3);

    memcpy(g_refused[0], g_mod[1].sha256, 32); g_nrefused = 1;
    uint8_t nrr[0x380] = {0}; memcpy(nrr, "NRR0", 4); *(uint32_t *)(nrr + 0x340) = 0x350; *(uint32_t *)(nrr + 0x344) = 1;
    memcpy(nrr + 0x350, g_mod[1].sha256, 32);
    assert(h_ro_RegisterModuleInfo(NULL, nrr) != 0);
    assert(h_ro_LoadModule(&handles[0], raw[0], NULL, 32, 1) != 0 && !g_mod[1].loaded);
    for (int i = 0; i < 3; i++) { free(g_mod[i].symh); free(g_mod[i].pristine); }
    puts("NRO relocation, load order, lifecycle, weak symbols and refusal: OK");
    aspect_test();
    return 0;
}
