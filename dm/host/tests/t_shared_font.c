// Shared-font ownership and LoadFont forwarding through the real forced registration; no game module or font file is needed.
#include "../hle_sdk16.c"
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
int dm_main_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void hle_signal_system_event(void *ev) { (void)ev; }
uint64_t hle_system_event_binding(void *ev) { (void)ev; return 1; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 1; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(plat_qos_t q) { (void)q; }
PLAT_TABLE_DEFINE(swforce)   // the real forced registrations (on Apple: getsectiondata of __DATA,swforce)
#if !defined(__APPLE__)
// The port's last stand-in for the standard font, a font of the system's (hle_sdk16.c font_system; docs/reference/services.md, "Shared fonts on Linux and Windows"): in
// a child, since the host looks the fonts up once per process. data: the XDG data directory the child is given (XDG_DATA_DIRS, XDG_DATA_HOME), with no DM_SHARED_FONT
// or DM_FONT_DIR. Its exit status: 0 when the load state, the address and the size are as want_loaded says (the file's bytes behind the BFTTF header, its size + 8);
// with no font the host stops the process at the first request (hle_sdk16.c no_font: Horizon always has its shared fonts, so a game would wait for ever), status
// DM_AOT_NO_FONT, after one [RED] line on standard error that names the fonts and the folders looked in (err: the file it goes to)
static const uint8_t g_ttf[] = "t_shared_font: a stand-in TrueType file's bytes";
static int font_child(const char *data, int want_loaded, const char *err) {
    const pid_t child = fork();
    if (child < 0) return -1;
    if (!child) {
        if (err && !freopen(err, "w", stderr)) _exit(5);
        setenv("XDG_DATA_DIRS", data, 1); setenv("XDG_DATA_HOME", data, 1);
        unsetenv("DM_SHARED_FONT"); unsetenv("DM_FONT_DIR");
        const int state = h_pl_LoadState(0);
        const uint8_t *p = h_pl_Addr(0);
        const size_t n = h_pl_Size(0);
        if (!want_loaded) _exit(state == 0 && !p && n == 0 ? 0 : 1);
        uint32_t marker = 0;
        if (p) memcpy(&marker, p, 4);
        _exit(state == 1 && p && marker == 0x18029a7fu && n == sizeof g_ttf + 8 && !memcmp(p + 8, g_ttf, sizeof g_ttf) ? 0 : 2);
    }
    int s = 0;
    waitpid(child, &s, 0);
    return WIFEXITED(s) ? WEXITSTATUS(s) : 128 + (WIFSIGNALED(s) ? WTERMSIG(s) : 0);
}
static void system_font_checks(void) {
    char cwd[900], dir[1100], file[1300];
    if (!getcwd(cwd, sizeof cwd)) { CHECK(0, "the working folder"); return; }
    snprintf(dir, sizeof dir, "%s/t_shared_font.tmp", cwd);
    const char *parts[] = {"", "/empty", "/share", "/share/fonts", "/share/fonts/truetype", "/share/fonts/truetype/dejavu"};
    for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++) { snprintf(file, sizeof file, "%s%s", dir, parts[i]); mkdir(file, 0755); }
    snprintf(file, sizeof file, "%s/share/fonts/truetype/dejavu/DejaVuSans.ttf", dir);
    FILE *f = fopen(file, "wb");
    const int wrote = f && fwrite(g_ttf, 1, sizeof g_ttf, f) == sizeof g_ttf;
    if (f) fclose(f);
    CHECK(wrote, "the stand-in font file written");
    snprintf(file, sizeof file, "%s/share", dir);
    CHECK(font_child(file, 1, NULL) == 0, "a system font in an XDG data directory is the standard font's stand-in: loaded, BFTTF header + the file, its size + 8");
    snprintf(file, sizeof file, "%s/empty", dir);
    char err[1400]; snprintf(err, sizeof err, "%s/no_font.txt", dir);
    CHECK(font_child(file, 0, err) == SDK16_NO_FONT_STATUS, "no system font: the process stops at the first request with status DM_AOT_NO_FONT (10), not a wait for ever");
    FILE *e = fopen(err, "r"); char line[4096] = ""; int reds = 0, named = 0;
    while (e && fgets(line, sizeof line, e)) if (strstr(line, "[RED] ")) { reds++; named += strstr(line, "DejaVu Sans") && strstr(line, "/empty/fonts") && strstr(line, "status 10"); }
    if (e) fclose(e);
    CHECK(reds == 1 && named == 1, "... after one [RED] line that names the fonts looked for, the folder and the status");
}
#endif
int main(void) {
#if !defined(__APPLE__)
    system_font_checks();
#endif
    size_t bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)plat_table_swforce(&bytes);
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
    uint8_t standard[64] = {0}, extension[64] = {0}, unrelated[64] = {0};
    uint32_t marker = 0x18029a7fu;
    memcpy(standard, &marker, 4); memcpy(extension, &marker, 4); memcpy(unrelated, &marker, 4);
    char name[50] = {0}; int self;
    const void *before[] = {NULL, unrelated, standard, extension};
    for (unsigned i = 0; i < sizeof before / sizeof *before; i++) {
        status = 403; int n = calls;
        CHECK(load(&self, name, before[i], 17, sizeof name) == status, "uninitialized fonts preserve result");
        CHECK(calls == n + 1 && seen_data == before[i], "uninitialized fonts leave all data unchanged");
    }
    std_f = (font_t){standard, sizeof standard}; ext_f = (font_t){extension, sizeof extension};
    const void *input[] = {standard, extension, standard + 8, extension + 8, standard + 1, extension + 9, unrelated, NULL};
    const void *expect[] = {standard + 8, extension + 8, standard + 8, extension + 8, standard + 1, extension + 9, unrelated, NULL};
    const uint32_t results[] = {0, 403, 301, 0x80000001u, ~0u};
    for (unsigned r = 0; r < sizeof results / sizeof *results; r++) for (unsigned i = 0; i < sizeof input / sizeof *input; i++) {
        int n = calls; status = results[r];
        CHECK(load(&self, name, input[i], ~0u, sizeof name) == status, "SDK status preserved");
        CHECK(calls == n + 1 && seen_data == expect[i], "only exact host bases skip the header, once");
        CHECK(seen_self == &self && seen_name == name && seen_index == ~0u && seen_capacity == sizeof name, "all other arguments preserved");
    }
    ext_f = std_f;
    CHECK(load(&self, name, standard, 0, 0) == status && seen_data == standard + 8, "extension fallback to standard");
    CHECK(seen_index == 0 && seen_capacity == 0, "zero unsigned arguments preserved");
    CHECK(lookups == 1, "original resolved once");
    CHECK(!memcmp(standard, unrelated, sizeof standard) && !memcmp(extension, unrelated, sizeof extension), "input bytes unchanged");
    printf("shared font: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
