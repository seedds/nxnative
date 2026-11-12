// The error applet (hle_err.c): each nn::err Show call is looked up by its mangled name in the real swforce registrations and called as the game would; a front_message
// stand-in records the dialog it would show. Checks the code each form shows (the console's "%04u-%04u", from a Result's module and description), the ApplicationErrorArg
// fields at the offsets of Ocarina of Time's sdk, messages that are empty, too long or not UTF-8, and that scripted input or no window shows nothing but still logs
#include "../hle.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <unistd.h>

int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }   // hle_bigstack.c: no guest stack here, the stand-ins run on this one
static int applets;   // hle_oe.c: open library applets; each Show call holds the game out of focus while it runs
void dm_applet_enter(const char *name) { (void)name; applets++; }
void dm_applet_leave(void) { applets--; }

static int calls, reply = 1;
static char got_title[256], got_text[8192];
int front_message(const char *title, const char *text) {
    calls++;
    snprintf(got_title, sizeof got_title, "%s", title); snprintf(got_text, sizeof got_text, "%s", text);
    return reply;
}

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) if (!strcmp(e[i].name, name)) fn = e[i].fn;
    CHECK(fn, "%s is taken over", name);
    return fn;
}

// The log of one call: stderr goes to a file meanwhile
static char logbuf[16384];
static FILE *log_file; static int saved_err;
static void log_begin(void) { fflush(stderr); saved_err = dup(2); log_file = tmpfile(); dup2(fileno(log_file), 2); calls = 0; got_title[0] = got_text[0] = 0; }
static void log_end(void) {
    fflush(stderr); dup2(saved_err, 2); close(saved_err);
    rewind(log_file); size_t n = fread(logbuf, 1, sizeof logbuf - 1, log_file); logbuf[n] = 0; fclose(log_file);
}

static void (*show_result)(Result), (*show_code)(uint64_t), (*show_variant)(const void *), (*show_app)(const void *), (*aoc_version)(void), (*app_version)(void);
static uint64_t code(uint32_t category, uint32_t number) { return category | (uint64_t)number << 32; }
static void expect_dialog(const char *what, const char *title, const char *text) {
    CHECK(calls == 1, "%s: one dialog (%d)", what, calls);
    CHECK(!strcmp(got_title, title), "%s: title '%s', expected '%s'", what, got_title, title);
    if (text) CHECK(!strcmp(got_text, text), "%s: text '%.80s', expected '%.80s'", what, got_text, text);
    CHECK(applets == 0, "%s: the applet closed again (%d open)", what, applets);
}
static void result_case(Result r, const char *title) { log_begin(); show_result(r); log_end(); expect_dialog("ShowError(Result)", title, NULL); }
static void code_case(uint64_t c, const char *title) { log_begin(); show_code(c); log_end(); expect_dialog("ShowError(ErrorCode)", title, NULL); }

// ApplicationErrorArg as the SDK's constructor and setters write it: version 2 at +0, the code number at +8, the language code at +0xc, the dialog message in 0x800 bytes at
// +0x14 and the full-screen message in 0x800 bytes at +0x814 (0x1014 bytes)
static uint8_t app[0x1014];
static void app_arg(uint8_t version, uint32_t number, const char *dialog, const char *full) {
    memset(app, 0, sizeof app); app[0] = version; memcpy(app + 8, &number, 4); memcpy(app + 0xc, "en-US", 5);
    if (dialog) memcpy(app + 0x14, dialog, strlen(dialog));
    if (full) memcpy(app + 0x814, full, strlen(full));
}
static void app_case(const char *what, const char *title, const char *text) { log_begin(); show_app(app); log_end(); expect_dialog(what, title, text); }

int main(void) {
    unsetenv("DM_KEYS"); unsetenv("DM_CTL");
    show_result = hook("_ZN2nn3err9ShowErrorENS_6ResultE");
    show_code = hook("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE");
    show_variant = hook("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE");
    show_app = hook("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE");
    aoc_version = hook("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv");
    app_version = hook("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv");
    if (fails) { printf("t_err: %d of %d checks failed\n", fails, checks); return 1; }

    printf("1 ShowError(Result): category 2000 + module, number = description\n");
    result_case(0x2a2, "Error code 2162-0001");                       // RES_FAIL: module 162, description 1
    result_case(2 | 1234u << 9, "Error code 2002-1234");
    result_case(0x1ff | 0x1fffu << 9 | 0xffc00000u, "Error code 2511-8191");   // the largest module and description; bits 22-31 are not part of the code
    printf("2 ShowError(ErrorCode): the console's form, and 2999-9999 for a code it cannot write\n");
    code_case(code(2306, 1234), "Error code 2306-1234");
    code_case(code(2999, 9999), "Error code 2999-9999");
    code_case(code(2000, 0), "Error code 2000-0000");
    code_case(code(1999, 5), "Error code 2999-9999");
    code_case(code(3000, 5), "Error code 2999-9999");
    code_case(code(2100, 10000), "Error code 2999-9999");
    printf("3 ShowError(ErrorResultVariant): {state, Result or ErrorCode at +4}\n");
    uint8_t v[12] = {0}; uint32_t state = 1, r = 0x2a2; memcpy(v, &state, 4); memcpy(v + 4, &r, 4);
    log_begin(); show_variant(v); log_end(); expect_dialog("variant with a Result", "Error code 2162-0001", NULL);
    uint64_t c = code(2124, 3); state = 2; memcpy(v, &state, 4); memcpy(v + 4, &c, 8);
    log_begin(); show_variant(v); log_end(); expect_dialog("variant with an ErrorCode", "Error code 2124-0003", NULL);
    state = 0; memcpy(v, &state, 4);
    log_begin(); show_variant(v); log_end();
    CHECK(calls == 0 && strstr(logbuf, "[RED]"), "an empty variant: no dialog, a red line (%d dialogs)", calls);
    printf("4 the version errors: the Results the SDK hands the applet\n");
    log_begin(); aoc_version(); log_end(); expect_dialog("add-on content version", "Error code 2162-0101", NULL);
    log_begin(); app_version(); log_end(); expect_dialog("game version", "Error code 2162-0100", NULL);

    printf("5 ShowApplicationError: the game's code number and messages\n");
    app_arg(2, 12345, "Save data is damaged.", "The save data could not be read. Start a new game.");
    app_case("both messages", "Error code 12345", "Save data is damaged.\n\nThe save data could not be read. Start a new game.");
    app_arg(2, 7, "Only the dialog message.", NULL); app_case("dialog message only", "Error code 7", "Only the dialog message.");
    app_arg(2, 7, NULL, "Only the full-screen message."); app_case("full-screen message only", "Error code 7", "Only the full-screen message.");
    app_arg(2, 7, "Same.", "Same."); app_case("the same message twice", "Error code 7", "Same.");
    app_arg(2, 0, NULL, NULL); app_case("no message", "Error code 0", "The game reported an error.");
    app_arg(2, 1, "%s%n%x", NULL); app_case("a format string is text", "Error code 1", "%s%n%x");
    app_arg(2, 1, "\xE5\x90\x8D \xC3\x9C", NULL); app_case("valid UTF-8 kept", "Error code 1", "\xE5\x90\x8D \xC3\x9C");
    static const char *const bad[] = {"\xC3\x28", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "ok \xE5\x90", "\xFF", "\x80"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        app_arg(2, 9, bad[i], "Shown instead.");
        app_case("an invalid dialog message is left out", "Error code 9", "Shown instead.");
        CHECK(strstr(logbuf, "[RED]") != NULL, "invalid UTF-8 case %zu: a red line", i);
    }
    app_arg(2, 3, NULL, "Full"); memset(app + 0x14, 'A', 0x800);   // no 0 in the dialog field: at most 0x7ff bytes of it
    log_begin(); show_app(app); log_end();
    CHECK(calls == 1 && strspn(got_text, "A") == 0x7ff && !strcmp(got_text + 0x7ff, "\n\nFull"), "a dialog message without its 0: cut at 0x7ff bytes (%zu)", strspn(got_text, "A"));
    memset(app + 0x814, 'B', 0x800);
    log_begin(); show_app(app); log_end();
    CHECK(calls == 1 && strlen(got_text) == 0x7ff * 2 + 2 && !memcmp(got_text + 0x801, "BBBB", 4), "both fields without their 0: each cut at 0x7ff bytes (%zu)", strlen(got_text));
    app_arg(1, 5, "Unknown layout", NULL);
    log_begin(); show_app(app); log_end();
    expect_dialog("another version of the argument", "Error", "The game reported an error.");
    CHECK(strstr(logbuf, "[RED]") != NULL, "another version of the argument: a red line");

    printf("6 nobody to close it: scripted input or no window shows nothing, the log line stays and the call returns\n");
    setenv("DM_KEYS", "1:A", 1);
    log_begin(); show_result(0x2a2); log_end();
    CHECK(calls == 0 && strstr(logbuf, "2162-0001") && applets == 0, "DM_KEYS: no dialog (%d), the code logged", calls);
    unsetenv("DM_KEYS"); setenv("DM_CTL", "/tmp/ctl", 1);
    app_arg(2, 42, "Logged.", NULL);
    log_begin(); show_app(app); log_end();
    CHECK(calls == 0 && strstr(logbuf, "Error code 42") && strstr(logbuf, "Logged."), "DM_CTL: no dialog (%d), code and message logged", calls);
    unsetenv("DM_CTL"); reply = -1;
    log_begin(); show_code(code(2306, 1)); log_end();
    CHECK(calls == 1 && strstr(logbuf, "2306-0001") && strstr(logbuf, "no dialog") && applets == 0, "no window: the front end shows nothing, the log says so");

    printf("t_err: %d of %d checks failed\n", fails, checks);
    return fails != 0;
}
