// Game language default unit test (hle_oe.c): a macOS language identifier mapped to a Switch language code, the choice among the Mac's preferred languages against a game's
// control data, and the whole initialisation (DM_LANG set from the choice) with a control.nacp written for the test. The expected codes and choices are hand-written from the
// language table (dm/tools/nacp.py LANGUAGES, hle_sdk16.c dm_lang_code) and the mapping examples, not computed the way the code computes them.
// hle_oe.c is included directly: the language functions are static there. The macOS list itself (lang_mac.m) is replaced by g_pref.
#include "../hle_oe.c"
#include <sys/stat.h>
#include <unistd.h>

// What "macOS" answers: language identifiers, space-separated in preference order, filled by the test (lang_mac.m is AppKit code and cannot be driven from a unit test)
static const char *g_pref = "";
int dm_mac_preferred_languages(char *out, size_t n) {
    if (out && n) out[0] = 0;
    if (!g_pref || !*g_pref) return 0;
    snprintf(out, n, "%s", g_pref);
    int c = 1;
    for (const char *s = g_pref; *s; s++) if (*s == ' ') c++;
    return c;
}
// Stubs for the rest of hle_oe.c (the notification queue and the scripted background round trip), which this test does not use
uint64_t now_ns(void) { return 0; }
void dm_audio_suspend(int on) { (void)on; }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
void dm_gfx_suspend(int on) { (void)on; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
void hle_clear_system_event(void *g) { (void)g; }
void dm_thread_native_wait(int entering) { (void)entering; }   // the blocking pop's scheduler hand-off (hle.h); the language choice never pops a notification

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", s); } while (0)
static int is(const char *got, const char *want) { return want ? got && !strcmp(got, want) : !got; }

// The game's languages as a list of codes, in the order nacp.py lists them (the order of the log line)
static const char *const k_ffcc[] = {"en-US", "en-GB", "ja", "fr", "de", "es", "it"};

// The code the choice yields for a preferred list against a game's languages (NULL: unknown)
static const char *choose(const char *const *pref, int n, const char *const *have, int hn) { return lang_choose(pref, n, have, hn); }

// A minimal control.nacp: 0x3030 zero bytes with the SupportedLanguageFlag at 0x302C, as the host reads it
static int write_nacp(const char *path, unsigned flag) {
    unsigned char b[0x3030] = {0};
    for (int i = 0; i < 4; i++) b[NACP_LANG_OFF + i] = (unsigned char)(flag >> 8 * i);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    int ok = fwrite(b, 1, sizeof b, f) == sizeof b;
    if (!ok) perror(path);
    fclose(f);
    return ok;
}

int main(void) {
    // The macOS identifiers (the shapes [NSLocale preferredLanguages] returns) → the Switch code, or none
    static const struct { const char *tag, *want; } k_map[] = {
        {"it-IT", "it"}, {"it", "it"},
        {"en-US", "en-US"}, {"en", "en-US"}, {"en-GB", "en-GB"}, {"en-AU", "en-GB"}, {"en-CA", "en-GB"}, {"EN-us", "en-US"},
        {"fr-FR", "fr"}, {"fr", "fr"}, {"fr-BE", "fr"}, {"fr-CA", "fr-CA"},
        {"es-ES", "es"}, {"es", "es"}, {"es-MX", "es-419"}, {"es-419", "es-419"}, {"es-AR", "es-419"}, {"es-US", "es-419"},
        {"pt-BR", "pt-BR"}, {"pt-PT", "pt"}, {"pt", "pt"},
        {"zh-Hans", "zh-Hans"}, {"zh-Hans-CN", "zh-Hans"}, {"zh-CN", "zh-Hans"}, {"zh-Hans-SG", "zh-Hans"},
        {"zh-Hant", "zh-Hant"}, {"zh-Hant-TW", "zh-Hant"}, {"zh-TW", "zh-Hant"}, {"zh-HK", "zh-Hant"},
        {"ja-JP", "ja"}, {"de-AT", "de"}, {"ko-KR", "ko"}, {"nl-BE", "nl"}, {"ru-RU", "ru"},
        {"sv-SE", NULL}, {"pl", NULL}, {"tr-TR", NULL}, {"C", NULL}, {"", NULL}, {"-", NULL}, {"it--IT", "it"},
    };
    for (unsigned i = 0; i < sizeof k_map / sizeof *k_map; i++) {
        const char *got = lang_map(k_map[i].tag);
        char msg[128];
        snprintf(msg, sizeof msg, "macOS %-11s → %-8s", k_map[i].tag, got ? got : "none");
        CHECK(is(got, k_map[i].want), msg);
        CHECK(!got || lang_code(got), "the mapping only names codes the settings table has");
    }
    // The choice: the first preferred language the game supports, a region mate counting as supported
    {
        static const char *const it_then_en[] = {"it-IT", "en-US"};
        static const char *const sv_then_ja[] = {"sv-SE", "ja-JP"};
        static const char *const en_au[] = {"en-AU"};
        static const char *const fr_ca[] = {"fr-CA"};
        static const char *const es_mx[] = {"es-MX"};
        static const char *const pt_br[] = {"pt-BR"};
        static const char *const zh_tw[] = {"zh-Hant-TW"};
        static const char *const en_gb_only[] = {"en-GB"};
        static const char *const en_only[] = {"en-US"};
        static const char *const ja_only[] = {"ja"};
        static const char *const fr_only[] = {"fr"};
        static const char *const es_only[] = {"es"};
        static const char *const pt_only[] = {"pt"};
        static const char *const zh_hans_only[] = {"zh-Hans"};
        CHECK(!strcmp(choose(it_then_en, 2, k_ffcc, 7), "it"), "Italian Mac, game has Italian: it");
        CHECK(!strcmp(choose(sv_then_ja, 2, k_ffcc, 7), "ja"), "Swedish first: the Switch has no code, the next supported language wins");
        CHECK(!strcmp(choose(it_then_en, 2, NULL, 0), "it"), "no control data: the first preferred language the Switch has a code for");
        CHECK(!strcmp(choose(en_au, 1, en_only, 1), "en-US"), "en-AU on an American-English-only game: en-US stands in");
        CHECK(!strcmp(choose(en_gb_only, 1, en_only, 1), "en-US"), "en-GB on an American-English-only game: en-US");
        CHECK(!strcmp(choose(en_only, 1, en_gb_only, 1), "en-GB"), "en-US on a British-English-only game: en-GB");
        CHECK(!strcmp(choose(fr_ca, 1, fr_only, 1), "fr"), "fr-CA on a French-only game: fr");
        CHECK(!strcmp(choose(es_mx, 1, es_only, 1), "es"), "es-MX on a Spain-Spanish-only game: es");
        CHECK(!strcmp(choose(pt_br, 1, pt_only, 1), "pt"), "pt-BR on a Portuguese-only game: pt");
        CHECK(!strcmp(choose(zh_tw, 1, zh_hans_only, 1), "zh-Hans"), "Traditional Chinese on a Simplified-only game: no stand-in and no en-US, the game's first language (zh-Hans)");
        CHECK(!strcmp(choose(ja_only, 1, fr_only, 1), "fr"), "Japanese Mac on a French-only game: no en-US, the game's first language (fr)");
        CHECK(!strcmp(choose(ja_only, 1, en_only, 1), "en-US"), "Japanese Mac on an English-only game: en-US, which the game has");
        CHECK(!strcmp(choose(NULL, 0, k_ffcc, 7), "en-US"), "no preferred language at all: en-US");
        CHECK(!strcmp(choose(sv_then_ja, 1, k_ffcc, 7), "en-US"), "nothing preferred is supported and nothing maps: en-US");
        CHECK(!strcmp(choose(en_only, 1, k_ffcc, 7), "en-US"), "en-US on a game with en-US: en-US");
        CHECK(!strcmp(choose(zh_hans_only, 1, k_ffcc, 7), "en-US"), "Simplified Chinese on a game without it: en-US");
    }

    // The whole initialisation: control.nacp beside the version's RomFS folder (DM_ROMFS), as the extraction writes it, and DM_LANG set from the choice
    const char *tmp = getenv("TMPDIR");
    char base[512], ver[600], nacp[700], romfs[700], none[700];
    snprintf(base, sizeof base, "%s/t_lang_tmp", tmp && *tmp ? tmp : ".");
    snprintf(ver, sizeof ver, "%s/v0", base);
    snprintf(nacp, sizeof nacp, "%s/control.nacp", ver);
    snprintf(romfs, sizeof romfs, "%s/RomFS", ver);
    snprintf(none, sizeof none, "%s/no-nacp/RomFS", base);
    mkdir(base, 0777); mkdir(ver, 0777); mkdir(romfs, 0777);
    // FFCC's languages: American and British English, Japanese, French, German, Spanish, Italian (flags 0,1,2,3,4,6,7 of the NACP order)
    CHECK(write_nacp(nacp, 0xDF), "a control.nacp for the test was written");
    setenv("DM_ROMFS", romfs, 1);
    unsetenv("DM_LANG");
    g_pref = "it-IT en-US";
    lang_default_init();
    CHECK(is(getenv("DM_LANG"), "it"), "initialisation: Italian Mac, game has Italian → DM_LANG=it");
    {   // an explicit DM_LANG wins, whatever the Mac says
        setenv("DM_LANG", "en-US", 1);
        g_pref = "it-IT en-US";
        lang_default_init();
        CHECK(is(getenv("DM_LANG"), "en-US"), "initialisation: an explicit DM_LANG is not replaced");
    }
    {   // no control.nacp reachable: the first preferred language that maps
        unsetenv("DM_LANG");
        setenv("DM_ROMFS", none, 1);
        g_pref = "fr-FR it-IT";
        lang_default_init();
        CHECK(is(getenv("DM_LANG"), "fr"), "initialisation: no control data → the first preferred language with a Switch code (fr)");
        unsetenv("DM_ROMFS");
        unsetenv("DM_ROOT");
        unsetenv("DM_LANG");
        lang_default_init();
        CHECK(is(getenv("DM_LANG"), "fr"), "initialisation: no DM_ROMFS either → DM_ROOT, then still the first mapped language");
    }
    {   // a Mac whose languages the Switch does not know at all keeps the old default
        unsetenv("DM_LANG");
        setenv("DM_ROMFS", romfs, 1);
        g_pref = "sv-SE da-DK";
        lang_default_init();
        CHECK(is(getenv("DM_LANG"), "en-US"), "initialisation: nothing maps → en-US, the host's old default");
    }
    {   // and a Mac that lists no language at all does nothing
        unsetenv("DM_LANG");
        g_pref = "";
        lang_default_init();
        CHECK(!getenv("DM_LANG"), "initialisation: no macOS list → DM_LANG stays unset");
    }
    {   // the platform-independent defaults of the two queries themselves, with the language derived above
        setenv("DM_LANG", "it", 1);
        uint64_t v = h_oe_GetLang();
        char code[9] = {0};
        memcpy(code, &v, 8);
        CHECK(!strcmp(code, "it"), "GetDesiredLanguage returns the chosen code as an 8-byte LanguageCode");
    }
    unlink(nacp); rmdir(romfs); rmdir(ver); rmdir(base);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
