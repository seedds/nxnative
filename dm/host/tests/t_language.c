// Settings language comparisons through the host hooks. All codes are synthetic;
// no game module, control data, graphics device or font file is needed.
#include "../hle_sdk16.c"
#include <limits.h>

module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
static int sdk_major = 20;
int dm_sdk_major(void) { return sdk_major; }
int dm_game_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *guest_sym(const char *name) { (void)name; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop stand-ins in hle_sdk16.c hold the game out of focus
void dm_applet_leave(void) {}
void hle_signal_system_event(void *ev) { (void)ev; }
uint64_t hle_system_event_binding(void *ev) { (void)ev; return 1; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 1; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR", "pl", "th"};

int main(void) {
    // Check the full matrix: both Chinese spellings match their own script;
    // different scripts, region variants and every unrelated language stay distinct.
    for (int32_t lang = 0; lang < 20; lang++) for (int i = 0; i < 20; i++) {
        char code[8] = {0}; memcpy(code, codes[i], strlen(codes[i]));
        unsigned want = lang == i || (lang == 6 && i == 15) || (lang == 15 && i == 6)
                                 || (lang == 11 && i == 16) || (lang == 16 && i == 11);
        CHECK(h_set_LangEq(&lang, code) == want, "Language == LanguageCode matrix");
        CHECK(h_set_LangEq2(code, &lang) == want, "LanguageCode == Language matrix");
    }
    const int32_t invalid[] = {-1, INT_MIN, 20, INT_MAX};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        CHECK(!h_set_LangEq(&invalid[i], "zh-Hans"), "invalid Language is not equal");
        CHECK(!h_set_LangEq2("zh-Hans", &invalid[i]), "invalid Language is not equal (reversed)");
        CHECK(h_set_LangMake(invalid[i]) == 0, "invalid Make still returns an empty code");
    }
    // A LanguageCode is exactly eight bytes; a missing terminator must not
    // accept an alias prefix or read past the supplied object.
    const char malformed[][8] = {"", "zh", "zh-hans", "zh-HansX", "zh-HantX", "zh-CNxyz", "zh-TWxyz"};
    const int32_t chinese[] = {6, 11, 15, 16};
    for (unsigned i = 0; i < sizeof malformed / sizeof *malformed; i++)
        for (unsigned j = 0; j < sizeof chinese / sizeof *chinese; j++) {
            CHECK(!h_set_LangEq(&chinese[j], malformed[i]), "malformed code is not an alias");
            CHECK(!h_set_LangEq2(malformed[i], &chinese[j]), "malformed code is not an alias (reversed)");
        }
    // Matching aliases must not collapse the system table: newer games use its
    // indices, and callers of Make must continue to receive the original spelling.
    // Polish (18) and Thai (19) have codes, but a Switch game's list stays 18 long.
    char available[19 * 8]; memset(available, 0xa5, sizeof available);
    CHECK(h_set_AvailLangN() == 18, "available language count stays 18");
    CHECK(h_set_AvailLang(available, 19) == 18, "available language list keeps all 18 entries");
    for (int i = 0; i < 20; i++) {
        char expected[8] = {0}; memcpy(expected, codes[i], strlen(codes[i]));
        uint64_t code = h_set_LangMake(i);
        CHECK(!memcmp(&code, expected, 8), "Make preserves each exact spelling");
        if (i < 18) CHECK(!memcmp(available + i * 8, expected, 8), "available language list preserves order and spelling");
    }
    const unsigned char *tail = (const unsigned char *)available + 18 * 8;
    for (int i = 0; i < 8; i++) CHECK(tail[i] == 0xa5, "available language list leaves the unused entry alone");
    memset(available, 0xa5, sizeof available);
    CHECK(h_set_AvailLang(available, 3) == 3 && !memcmp(available + 16, "fr\0\0\0\0\0\0", 8)
          && (unsigned char)available[24] == 0xa5, "a short buffer takes the first codes");
    CHECK(!h_set_AvailLang(NULL, 0), "an empty buffer writes nothing");

    // SDK 1-3 use settings commands 1/3: a fixed 15-code list. SDK 4+
    // use the extended commands 5/6 and retain the existing fallback list.
    const int legacy_sdks[] = {1, 2, 3};
    for (unsigned i = 0; i < sizeof legacy_sdks / sizeof *legacy_sdks; i++) {
        sdk_major = legacy_sdks[i]; memset(available, 0xa5, sizeof available);
        CHECK(h_set_AvailLangN() == 15, "legacy settings count is 15");
        CHECK(h_set_AvailLang(available, 19) == 15, "legacy settings list stops at 15");
        for (int n = 0; n < 15; n++) {
            char expected[8] = {0}; memcpy(expected, codes[n], strlen(codes[n]));
            CHECK(!memcmp(available + n * 8, expected, 8), "legacy list preserves the original codes");
        }
        for (unsigned n = 15 * 8; n < sizeof available; n++)
            CHECK((unsigned char)available[n] == 0xa5, "legacy list leaves the tail untouched");
        CHECK(h_set_AvailLang(available, 3) == 3, "legacy list respects a smaller buffer");
        CHECK(!h_set_AvailLang(NULL, 0), "legacy empty list writes nothing");
    }
    const int modern_sdks[] = {0, 4, 10, 20};
    for (unsigned i = 0; i < sizeof modern_sdks / sizeof *modern_sdks; i++) {
        sdk_major = modern_sdks[i];
        CHECK(h_set_AvailLangN() == 18, "modern or unknown SDK retains the fallback count");
        CHECK(h_set_AvailLang(available, 19) == 18, "modern or unknown SDK retains the fallback list");
    }
    sdk_major = 20;

    // nn::oe::GetDisplayVersion: reads the exact 16-byte DisplayVersion field from control.nacp (+0x3060),
    // preserving suffixes like "1.0.2 b723"; falls back to DM_GAME_DISPLAY_VERSIONS / "1.0.0" when absent.
    {
        char tmp[] = "/tmp/t_dispver.XXXXXX", nacp_path[256], rom_path[256], out[16];
        CHECK(mkdtemp(tmp) != NULL, "mkdtemp for display version");
        snprintf(nacp_path, sizeof nacp_path, "%s/control.nacp", tmp);
        snprintf(rom_path, sizeof rom_path, "%s/romfs.img", tmp);
        uint8_t nacp[0x4000] = {0};
        memcpy(nacp + 0x3060, "1.0.2 b723", 10);
        FILE *f = fopen(nacp_path, "wb"); CHECK(f && fwrite(nacp, 1, sizeof nacp, f) == sizeof nacp, "write control.nacp"); if (f) fclose(f);
        unsetenv("DM_CONTROL_NACP"); setenv("DM_ROMFS", rom_path, 1);
        memset(out, 0xaa, sizeof out); h_oe_DispVer(out);
        CHECK(!strcmp(out, "1.0.2 b723") && out[15] == 0, "GetDisplayVersion reads exact DisplayVersion beside DM_ROMFS");
        // 16 non-NUL bytes in control.nacp must NUL-terminate at byte 15 without reading past the buffer.
        memset(nacp + 0x3060, '9', 16);
        f = fopen(nacp_path, "wb"); CHECK(f && fwrite(nacp, 1, sizeof nacp, f) == sizeof nacp, "rewrite full-width control.nacp"); if (f) fclose(f);
        setenv("DM_CONTROL_NACP", nacp_path, 1);
        memset(out, 0xaa, sizeof out); h_oe_DispVer(out);
        CHECK(!memcmp(out, "999999999999999\0", 16), "GetDisplayVersion NUL-terminates a full 16-byte NACP field");
        // Missing control.nacp falls back to DM_GAME_DISPLAY_VERSIONS by main build ID, else "1.0.0".
        unlink(nacp_path); rmdir(tmp);
        memset(out, 0xaa, sizeof out); h_oe_DispVer(out);
        CHECK(!strcmp(out, "1.0.0"), "GetDisplayVersion falls back to 1.0.0 without control.nacp or matching build");
        const struct { const char *build, *ver; } k[] = {DM_GAME_DISPLAY_VERSIONS};
        if (k[0].build) {
            module_t fake = {0}; strlcpy(fake.name, "main", sizeof fake.name); strlcpy(fake.build_id, k[0].build, sizeof fake.build_id);
            g_mod = &fake; g_nmod = 1;
            memset(out, 0xaa, sizeof out); h_oe_DispVer(out);
            CHECK(!strcmp(out, k[0].ver), "GetDisplayVersion falls back to profile build row without control.nacp");
            g_mod = NULL; g_nmod = 0;
        }
        unsetenv("DM_CONTROL_NACP"); unsetenv("DM_ROMFS");
    }

    printf("language: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
