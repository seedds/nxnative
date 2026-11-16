// Shared-font ownership and LoadFont forwarding through the real forced registration; no game module or font file is needed.
#include "../hle_sdk16.c"
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop stand-ins hold the game out of focus
void dm_applet_leave(void) {}
#include <mach-o/getsect.h>
#include <sys/wait.h>
#include <sys/resource.h>

static int checks, fails, calls, lookups, missing;
static void *seen_self;
static char *seen_name;
static const void *seen_data;
static unsigned seen_index, seen_capacity;
static uint32_t status;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static uint32_t original(void *self, char *name, const void *data, unsigned index, unsigned capacity) {
    calls++; seen_self = self; seen_name = name; seen_data = data;
    seen_index = index; seen_capacity = capacity;
    return status;
}
void *guest_sym(const char *name) {
    lookups++;
    CHECK(!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj"), "original symbol lookup");
    return missing ? NULL : original;
}
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_sdk_major(void) { return 20; }
int dm_game_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void hle_signal_system_event(void *ev) { (void)ev; }
uint64_t hle_system_event_binding(void *ev) { (void)ev; return 1; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 1; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
extern const struct mach_header_64 _mh_execute_header;
// One font per type from DM_FONT_DIR (a child each, as the fonts are opened once): a type's own file, else the standard font; DM_SHARED_FONT is one font for types 0-4
static int fonts_from_folder(const char *dir, const char *one) {
    pid_t child = fork();
    if (child) { int s = 0; waitpid(child, &s, 0); return WIFEXITED(s) ? WEXITSTATUS(s) : 99; }
    setenv("DM_FONT_DIR", dir, 1); unsetenv("DM_SHARED_FONT_EXT");
    if (one) setenv("DM_SHARED_FONT", one, 1); else unsetenv("DM_SHARED_FONT");
    int bad = 0; font_t f[FONT_TYPES];
    for (int t = 0; t < FONT_TYPES; t++) f[t] = font_get(t);
    const char *want[FONT_TYPES] = {one ? "one" : "std", "std", "std", "std", "kor", "ext"};
    if (one) want[1] = want[2] = want[3] = want[4] = "one";
    for (int t = 0; t < FONT_TYPES; t++) bad += !f[t].p || f[t].n != 8 + 3 || memcmp(f[t].p + 8, want[t], 3);
    bad += f[1].p != f[0].p || f[2].p != f[0].p;                   // no file of its own: the standard font's mapping itself
    bad += font_get(-1).p != f[0].p || font_get(FONT_TYPES).p != f[0].p;   // an unknown type: the standard font
    bad += font_payload(f[4].p) != f[4].p + 8 || font_payload(f[5].p + 8) != f[5].p + 8;   // every type's base skips the header, once
    _exit(bad);
}
static void put(const char *dir, const char *file, const char *text) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); }
}
int main(void) {
    char dir[] = "/tmp/t_shared_font.XXXXXX";
    if (mkdtemp(dir)) {
        put(dir, "standard.ttf", "std"); put(dir, "korean.ttf", "kor"); put(dir, "nintendo_ext.ttf", "ext");
        char one[1100]; snprintf(one, sizeof one, "%s/one.ttf", dir); put(dir, "one.ttf", "one");
        CHECK(fonts_from_folder(dir, NULL) == 0, "a type's own file, else the standard font");
        CHECK(fonts_from_folder(dir, one) == 0, "DM_SHARED_FONT: one font for types 0-4, the extension font still its own");
        const char *files[] = {"standard.ttf", "korean.ttf", "nintendo_ext.ttf", "one.ttf"};
        for (unsigned i = 0; i < 4; i++) { char p[1100]; snprintf(p, sizeof p, "%s/%s", dir, files[i]); unlink(p); }
        rmdir(dir);
    } else CHECK(0, "temporary font folder");
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    uint32_t (*load)(void *, char *, const void *, unsigned, unsigned) = NULL;
    unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) {
        if (strcmp(e[i].name, "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj")) continue;
        load = e[i].fn; count++;
        CHECK(e[i].why && e[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && load, "exact LoadFont forced registration exists once");
    if (!load) return 1;
    // A missing SDK implementation must fail loudly rather than fabricate a successful font load.
    pid_t child = fork();
    CHECK(child >= 0, "missing-original child starts");
    if (!child) { struct rlimit r = {0, 0}; setrlimit(RLIMIT_CORE, &r); missing = 1; load(NULL, NULL, NULL, 0, 0); _exit(1); }
    if (child > 0) { int s; waitpid(child, &s, 0); CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGABRT, "missing original aborts"); }
    uint8_t standard[64] = {0}, extension[64] = {0}, traditional[64] = {0}, unrelated[64] = {0};
    uint32_t marker = 0x18029a7fu;
    memcpy(standard, &marker, 4); memcpy(extension, &marker, 4); memcpy(traditional, &marker, 4); memcpy(unrelated, &marker, 4);
    char name[50] = {0}; int self;
    const void *before[] = {NULL, unrelated, standard, extension};
    for (unsigned i = 0; i < sizeof before / sizeof *before; i++) {
        status = 403; int n = calls;
        CHECK(load(&self, name, before[i], 17, sizeof name) == status, "uninitialized fonts preserve result");
        CHECK(calls == n + 1 && seen_data == before[i], "uninitialized fonts leave all data unchanged");
    }
    font_by_type[0] = (font_t){standard, sizeof standard}; font_by_type[5] = (font_t){extension, sizeof extension};
    const void *input[] = {standard, extension, standard + 8, extension + 8, standard + 1, extension + 9, unrelated, NULL};
    const void *expect[] = {standard + 8, extension + 8, standard + 8, extension + 8, standard + 1, extension + 9, unrelated, NULL};
    const uint32_t results[] = {0, 403, 301, 0x80000001u, ~0u};
    for (unsigned r = 0; r < sizeof results / sizeof *results; r++) for (unsigned i = 0; i < sizeof input / sizeof *input; i++) {
        int n = calls; status = results[r];
        CHECK(load(&self, name, input[i], ~0u, sizeof name) == status, "SDK status preserved");
        CHECK(calls == n + 1 && seen_data == expect[i], "only exact host bases skip the header, once");
        CHECK(seen_self == &self && seen_name == name && seen_index == ~0u && seen_capacity == sizeof name, "all other arguments preserved");
    }
    font_by_type[5] = font_by_type[0];
    CHECK(load(&self, name, standard, 0, 0) == status && seen_data == standard + 8, "extension fallback to standard");
    CHECK(seen_index == 0 && seen_capacity == 0, "zero unsigned arguments preserved");
    CHECK(lookups == 1, "original resolved once");
    CHECK(!memcmp(standard, unrelated, sizeof standard) && !memcmp(extension, unrelated, sizeof extension) && !memcmp(traditional, unrelated, sizeof traditional), "input bytes unchanged");
    // GetBuiltInFontDataSet (Switch 2 types): one entry per type asked, in order, at most 6; the address and size are GetBuiltInFontData's
    for (int t = 0; t < FONT_TYPES; t++) (void)font_get(t);   // the fonts are opened once: what follows sets them (types 1, 2 and 4 as having no file of their own)
    for (int t = 0; t < FONT_TYPES; t++) font_by_type[t] = (font_t){standard, sizeof standard};
    font_by_type[3] = (font_t){traditional, sizeof traditional}; font_by_type[5] = (font_t){extension, sizeof extension};
    CHECK(load(&self, name, traditional, 0, sizeof name) == status && seen_data == traditional + 8, "every type's own base skips the header");
    pl_builtin_font_t set[8]; memset(set, 0xa5, sizeof set);
    const uint8_t types[8] = {5, 0, 3, 5, 1, 2, 4, 0};
    int got = -1;
    CHECK(h_pl_BuiltInSetOunce(&got, set, types, 3, 0, 0) == 1 && got == 3, "font set: three types, three entries");
    CHECK(set[0].type == 5 && set[0].address == extension + font_skip() && set[0].size == sizeof extension - font_skip(), "font set: the extension font for type 5");
    CHECK(set[1].type == 0 && set[1].address == standard + font_skip() && set[2].type == 3 && set[2].address == traditional + font_skip(), "font set: each type's own font, in order");
    pl_builtin_font_t one; CHECK(h_pl_BuiltInOunce(&one, 3, 0) == 1 && !memcmp(&one, &set[2], sizeof one), "font set: an entry as GetBuiltInFontData fills it");
    CHECK(set[3].type == 0xa5, "font set: nothing past the count");
    memset(set, 0, sizeof set);
    CHECK(h_pl_BuiltInSetOunce(&got, set, types, 8, 0, 0) == 1 && got == 6 && set[5].type == 2 && set[6].type == 0 && set[6].address == NULL, "font set: at most 6, as the SDK sends");
    // The Switch 1 overload: at most 7, the same entries
    memset(set, 0, sizeof set);
    CHECK(h_pl_BuiltInSetNx(&got, set, types, 8, 0, 0) == 1 && got == 7 && set[6].type == 4 && set[6].address == standard + font_skip() && set[7].address == NULL, "font set (Switch 1 types): at most 7");
    memset(font_by_type, 0, sizeof font_by_type);
    CHECK(h_pl_BuiltInSetOunce(&got, set, types, 3, 0, 0) == 0 && got == 0, "font set: no fonts, no entries");
    got = -1;
    CHECK(h_pl_BuiltInSetNx(&got, set, types, 3, 0, 0) == 0 && got == -1, "font set (Switch 1 types): no fonts, count left as it was");
    printf("shared font: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
