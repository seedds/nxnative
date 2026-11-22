// Exercise the real HLE, forced-takeover and stub indexes with synthetic Mach-O sections; no game files required.
#include "../hle.h"
#include <mach-o/getsect.h>
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static hle_ent entries[4];
static hle_force_ent forces[3];
static hle_force_ent *force_sec = forces; static unsigned long force_sec_n = 3;   // the alias phase swaps in its own table
static int have_sections;
static uint8_t *test_section(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; assert(!strcmp(seg, "__DATA"));
    *size = 0;
    if (!have_sections) return NULL;
    if (!strcmp(sect, "swhle")) { *size = sizeof entries; return (uint8_t *)entries; }
    assert(!strcmp(sect, "swforce")); *size = force_sec_n * sizeof *force_sec; return (uint8_t *)force_sec;
}
#define getsectiondata test_section
#include "../hle.c"
#include "../loader.c"
void mtl_present_screen(uint32_t w, uint32_t h) { (void)w; (void)h; }   // loader.c: a screen taller than 16:9; no renderer here
#undef getsectiondata

struct dm_flags g_flag;
uint64_t unimpl_stubs[MAX_STUB];
uint64_t now_ns(void) { return 0; }
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return 0; }
void guest_clock_read_wait(void) {}
void hlecount_seal(void) {}
int svc_nro_deactivate(module_t *m) { m->loaded = 0; return 1; }   // no memory mappings in this fixture
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
static const char *wrap_kind, *wrap_name;
static unsigned wraps;
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    wrap_kind = kind; wrap_name = name; wraps++; return fn;
}

static void reset_sections(void) {
    free(g_hle_idx); g_hle_idx = NULL; g_hle_mask = 0; g_hle_sec = NULL;
    free(g_fidx); g_fidx = NULL; g_fmask = 0;
    free(g_force_hits); g_force_hits = NULL;
}

// Failures run in a child, so duplicate rejection and the hard stub limit are checked without killing the suite.
static void expect_abort(int duplicate) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
        freopen("/dev/null", "w", stderr);
        if (duplicate == 2) force_init();   // the alias table set up by the caller: a target that is itself taken over
        else if (duplicate) { forces[2] = forces[0]; force_init(); }
        else resolve_import(NULL, "one_more_missing_symbol");
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

static void check_language_bindings(void) {
    uint8_t image[0x100] = {0};
    char names[sizeof k_language_list + sizeof k_language_count];
    memcpy(names, k_language_list, sizeof k_language_list);
    memcpy(names + sizeof k_language_list, k_language_count, sizeof k_language_count);
    Elf64_Sym symbols[] = {
        {0}, {.st_name = 0, .st_shndx = 1, .st_value = 4},
        {.st_name = sizeof k_language_list, .st_shndx = 1, .st_value = 8},
    };
    module_t sdk = {.name = "sdk", .base = image, .size = sizeof image, .ro_off = 16,
                    .symtab = symbols, .strtab = names, .nsym = 3};
    module_t *loaded[] = {&sdk};
    g_mod = &sdk; g_nmod = 1; g_loaded = loaded; g_nloaded = 1;
    int list_fallback, count_fallback;
    hle_force_ent hooks[] = {
        {k_language_list, &list_fallback, "test"}, {k_language_count, &count_fallback, "test"},
    };
    force_sec = hooks; force_sec_n = 2;
    const struct { const char *stamp; unsigned symbols; size_t data_off; int native; } cases[] = {
        {"FS_ACCESS: { sdk_version: 20.5.25, spec: Ounce }", 3, sizeof image, 1},
        {"FS_ACCESS: { sdk_version: 22.2.10, spec: Ounce }\n", 3, sizeof image, 1},
        {"FS_ACCESS: { sdk_version: 22.2.11, spec: Ounce }", 3, sizeof image, 0},
        {"FS_ACCESS: { sdk_version: 22.2.10, spec: NX }", 3, sizeof image, 0},
        {"FS_ACCESS: { sdk_version: 22.2.10, spec: Ounce }", 2, sizeof image, 0},
        {"FS_ACCESS: { sdk_version: 22.2.10, spec: Ounce }", 3, 0, 0},
        {"", 3, sizeof image, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        strcpy((char *)image + sdk.ro_off, cases[i].stamp);
        sdk.nsym = cases[i].symbols;
        sdk.data_off = cases[i].data_off;
        force_init();
        int other, weak0;
        Elf64_Sym list_import = {.st_name = symbols[1].st_name};
        Elf64_Sym count_import = {.st_name = symbols[2].st_name};
        assert(bind_sym(&sdk, &list_import, &other, &weak0) == (uint64_t)(cases[i].native ? (void *)(image + 4) : &list_fallback));
        assert(bind_sym(&sdk, &count_import, &other, &weak0) == (uint64_t)(cases[i].native ? (void *)(image + 8) : &count_fallback));
        force_check();
        reset_sections();
    }
    g_mod = NULL; g_nmod = 0; g_loaded = NULL; g_nloaded = 0;
    force_sec = forces; force_sec_n = 3;
}

int main(void) {
    // Empty/missing sections must still give misses after lazy initialization.
    assert(!hle_implements("absent")); assert(!hle_implements("absent"));
    force_init(); assert(!force_lookup("absent")); reset_sections();

    // Three different names in the same initial bucket exercise probing and a duplicate behind a collision.
    char names[3][32]; unsigned found = 0;
    for (unsigned i = 0; found < 3; i++) {
        char candidate[32]; snprintf(candidate, sizeof candidate, "symbol_%u", i);
        if ((name_h(candidate) & 15) == 15) strcpy(names[found++], candidate);
    }
    int values[5];   // [4]: the alias phase's taken-over function
    entries[0] = (hle_ent){names[0], &values[0]};
    entries[1] = (hle_ent){names[1], &values[1]};
    entries[2] = (hle_ent){names[1], &values[2]};
    entries[3] = (hle_ent){names[2], &values[3]};
    for (unsigned i = 0; i < 3; i++) forces[i] = (hle_force_ent){names[i], &values[i], "test"};
    have_sections = 1; force_init();
    for (unsigned i = 0; i < 3; i++) {
        unsigned before = wraps;
        assert(hle_implements(names[i]) && wraps == before);
        uint64_t want = (uint64_t)&values[i == 2 ? 3 : i];
        assert(resolve_import(NULL, names[i]) == want);
        assert(!strcmp(wrap_kind, "HLE") && !strcmp(wrap_name, names[i]));
        assert(force_lookup(names[i]) == &values[i]);
        assert(!strcmp(wrap_kind, "FORCE") && !strcmp(wrap_name, names[i]));
        assert(g_force_hits[i] == 1);
    }
    assert(!hle_implements("absent") && !force_lookup("absent") && !g_nstub);
    assert(force_lookup(names[0]) == &values[0] && g_force_hits[0] == 2);
    expect_abort(1);

    // Fill through every growth boundary, then query in reverse using different pointers to the same names.
    static char missing[MAX_STUB][32];
    for (unsigned i = 0; i < MAX_STUB; i++) {
        snprintf(missing[i], sizeof missing[i], "missing_%u", i);
        assert(resolve_import(NULL, missing[i]) == (uint64_t)&unimpl_stubs[i]);
        assert(!strcmp(wrap_kind, "STUB") && !strcmp(wrap_name, missing[i]));
    }
    for (unsigned i = MAX_STUB; i-- > 0;) {
        char copy[32]; strcpy(copy, missing[i]);
        assert(resolve_import(NULL, copy) == (uint64_t)&unimpl_stubs[i]);
    }
    assert(g_nstub == MAX_STUB); expect_abort(0);
    reset_sections();

    // Guest aliases (hle.h HLE_FORCE_ALIAS): a reference to the hardware member binds to the loaded module's software member, even where the module defines the hardware one itself
    // (as the sdk does); an alias whose target no module defines leaves the binding as it was; an alias to a taken-over name stops loading
    static uint8_t image[0x100];
    static const char strtab[] = "\0hw_member\0sw_member\0";
    static Elf64_Sym symtab[3];
    symtab[1] = (Elf64_Sym){.st_name = 1, .st_shndx = 1, .st_value = 0x80};
    symtab[2] = (Elf64_Sym){.st_name = 11, .st_shndx = 1, .st_value = 0x40};
    static module_t mod; mod.base = image; mod.symtab = symtab; mod.strtab = strtab; mod.nsym = 3; build_symhash(&mod);
    static module_t *loaded[1]; loaded[0] = &mod; g_loaded = loaded; g_nloaded = 1;
    static hle_force_ent aliases[3];
    aliases[0] = (hle_force_ent){"hw_member", NULL, "test", "sw_member"};
    aliases[1] = (hle_force_ent){"hw_orphan", NULL, "test", "sw_orphan"};
    aliases[2] = (hle_force_ent){"forced_fn", &values[4], "test", NULL};
    force_sec = aliases; force_sec_n = 3; have_sections = 1; force_init();
    assert(force_lookup("hw_member") == image + 0x40 && !strcmp(wrap_kind, "ALIAS") && !strcmp(wrap_name, "hw_member") && g_force_hits[0] == 1);
    int other, weak0;
    assert(bind_sym(&mod, &symtab[1], &other, &weak0) == (uint64_t)(image + 0x40) && !other && !weak0);   // the module's own hw_member loses to the alias
    assert(bind_sym(&mod, &symtab[2], &other, &weak0) == (uint64_t)(image + 0x40));                      // the software member itself is untouched
    assert(!force_lookup("hw_orphan") && g_force_hits[1] == 1);
    assert(force_lookup("forced_fn") == &values[4]);
    aliases[1] = (hle_force_ent){"hw_bad", NULL, "test", "forced_fn"};
    expect_abort(2);
    g_loaded = NULL; g_nloaded = 0; force_sec = forces; force_sec_n = 3;
    reset_sections();
    check_language_bindings();
    puts("Language list and count: verified SDK exports and paired host fallback: OK");
    puts("Guest aliases: bound to the loaded definition over the module's own, orphan left as bound, alias to a taken-over name rejected: OK");
    puts("Symbol indexes: collisions, duplicate precedence/rejection, misses, wrappers and 4096 reusable stubs: OK");
    return 0;
}
