// Settings language comparisons through the host hooks. All codes are synthetic;
// no game module, control data, graphics device or font file is needed.
#include "../hle_sdk16.c"
#include <limits.h>

module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
static int g_major = 20, g_ounce;   // the SDK the list is asked for
int dm_sdk_major(void) { return g_major; }
int dm_sdk_ounce(void) { return g_ounce; }
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
static const char *const ounce[] = {"ja", "en-US", "fr", "de", "it", "es", "ko", "nl", "pt", "ru", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR", "pl", "th"};   // a Switch 2 SDK's own table
static void ounce_list(int major, int want) {   // a Switch 2 SDK's list: its own table's first `want` codes, whatever room the game gives
    g_ounce = 1; g_major = major;
    char out[20 * 8]; memset(out, 0xa5, sizeof out);
    CHECK(h_set_AvailLangN() == want, "Switch 2 SDK: count of its own table");
    CHECK(h_set_AvailLang(out, 20) == want, "Switch 2 SDK: its own table, whole");
    for (int i = 0; i < want; i++) {
        char expected[8] = {0}; memcpy(expected, ounce[i], strlen(ounce[i]));
        CHECK(!memcmp(out + i * 8, expected, 8), "Switch 2 SDK: order and spelling of its own table");
    }
    for (int i = 0; i < 8; i++) CHECK((unsigned char)out[want * 8 + i] == 0xa5, "Switch 2 SDK: the entry after the list is left alone");
    memset(out, 0xa5, sizeof out);
    CHECK(h_set_AvailLang(out, 3) == 3 && !memcmp(out + 16, "fr\0\0\0\0\0\0", 8) && (unsigned char)out[24] == 0xa5, "Switch 2 SDK: a short buffer takes the first codes");
    g_ounce = 0; g_major = 20;
}

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
    // A Switch 2 SDK reports its own table: 16 codes before SDK 21, 18 from it (Polish and Thai), 18 when the version is not known
    ounce_list(20, 16); ounce_list(21, 18); ounce_list(22, 18); ounce_list(0, 18);
    CHECK(h_set_AvailLangN() == 18, "a Switch SDK's count is unchanged after the Switch 2 cases");
    printf("language: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
