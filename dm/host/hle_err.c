// nn::err: the error applet. On a Switch the game's Show call starts the applet, which shows the error code (and, for an application error, the game's own message) until
// the player closes it, and the call returns then: ShowError(ErrorCode) and ShowApplicationError in Ocarina of Time's sdk (build ID 1643a838814f9c10c696f930ed681e7fa3fb9af8)
// run StartLibraryApplet, JoinLibraryApplet and CloseLibraryApplet (sdk+0x405510). The host shows the same in a dialog of the front end (front_message), and the calling game
// thread waits until the player closes it. Without a window, or with scripted input (DM_KEYS, DM_CTL: nobody is there to close it), nothing is shown and the call returns at
// once, as the controller applet does (hle_hid.c). The log line is written either way
#include "hle.h"
#include "front.h"

// The front end's dialog (front.h). This default is for a front end that has none: nothing shown
__attribute__((weak)) int front_message(const char *title, const char *text) { (void)title; (void)text; return -1; }

// The code as the console writes it: nn::err::GetErrorCodeString (sdk+0x405100) prints category and number with "%04u-%04u" when the category is 2000 to 2999 and the
// number at most 9999, and "2999-9999" for any other code. An ErrorCode is {u32 category, u32 number}, passed in one register (MakeErrorCode, sdk+0x403c00)
#define ERR_CATEGORY_MIN 2000u
#define ERR_CATEGORY_MAX 2999u
#define ERR_NUMBER_MAX 9999u
#define ERR_CODE_LEN 16
void err_code_string(char out[ERR_CODE_LEN], uint64_t code) {
    uint32_t category = (uint32_t)code, number = (uint32_t)(code >> 32);
    if (category < ERR_CATEGORY_MIN || category > ERR_CATEGORY_MAX || number > ERR_NUMBER_MAX) { category = ERR_CATEGORY_MAX; number = ERR_NUMBER_MAX; }
    snprintf(out, ERR_CODE_LEN, "%04u-%04u", category, number);
}
// A Result's code: category 2000 + module (bits 0-8), number = description (bits 9-21): ConvertResultToErrorCode (sdk+0x4050e0)
uint64_t err_code_of_result(Result r) { return (ERR_CATEGORY_MIN + (r & 0x1ff)) | (uint64_t)((r >> 9) & 0x1fff) << 32; }

// Text the game wrote: well-formed UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF) in n bytes. It is passed on as data, never as a format string
int err_utf8_valid(const char *s, size_t n) {
    const uint8_t *p = (const uint8_t *)s, *end = p + n;
    while (p < end) {
        uint8_t c = *p++;
        if (c < 0x80) continue;
        int more = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
        if (more < 0 || end - p < more) return 0;
        uint8_t lo = 0x80, hi = 0xbf;   // the bounds of the second byte that rule out overlong forms, surrogates and code points past U+10FFFF
        if (c == 0xe0) lo = 0xa0; else if (c == 0xed) hi = 0x9f; else if (c == 0xf0) lo = 0x90; else if (c == 0xf4) hi = 0x8f;
        if (p[0] < lo || p[0] > hi) return 0;
        for (int i = 1; i < more; i++) if ((p[i] & 0xc0) != 0x80) return 0;
        p += more;
    }
    return 1;
}

// nn::err::ApplicationErrorArg, as Ocarina of Time's sdk (build ID 1643a838814f9c10c696f930ed681e7fa3fb9af8) lays it out: its constructor (sdk+0x405580) writes 2 at +0;
// SetApplicationErrorCodeNumber (sdk+0x4058b0) the u32 at +8; GetLanguageCode (sdk+0x405be0) reads 8 bytes at +0xc; SetDialogMessage (sdk+0x4058c0) and SetFullScreenMessage
// (sdk+0x4059d0) copy at most 0x7ff bytes and a 0 into 0x800 bytes at +0x14 and +0x814; ShowApplicationError (sdk+0x404260) hands the applet 0x1014 bytes. Other versions
// of the argument were not seen: their fields are not read
#define APP_ARG_VERSION 2
#define APP_ARG_NUMBER 0x8
#define APP_ARG_DIALOG 0x14
#define APP_ARG_FULL_SCREEN 0x814
#define APP_ARG_MESSAGE_CAP 0x800
// A message field as text: up to its first 0 within the field; NULL when it is empty or not valid UTF-8 (the game wrote the field itself, so a 0 inside it is not assured)
static const char *app_message(const uint8_t *arg, size_t off, char out[APP_ARG_MESSAGE_CAP]) {
    size_t n = strnlen((const char *)arg + off, APP_ARG_MESSAGE_CAP - 1);
    if (!n) return NULL;
    if (!err_utf8_valid((const char *)arg + off, n)) { LOG("[RED] err: the application error's message at +%#zx is not valid UTF-8; shown without it", off); return NULL; }
    memcpy(out, arg + off, n); out[n] = 0;
    return out;
}
// The dialog of an application error: the game's code number and its messages, the dialog message first and the full-screen one (the console's details page) after it
void err_app_dialog(const void *arg_, char *title, size_t title_cap, char *text, size_t text_cap) {
    const uint8_t *arg = arg_;
    if (arg[0] != APP_ARG_VERSION) {
        LOG("[RED] err: ApplicationErrorArg version %u, whose layout is not known: shown without its code number and messages", arg[0]);
        snprintf(title, title_cap, "Error"); snprintf(text, text_cap, "The game reported an error.");
        return;
    }
    uint32_t number; memcpy(&number, arg + APP_ARG_NUMBER, 4);
    // Open: how the console's applet writes an application's code number is not in the game's code; it is shown here as the number the game set
    snprintf(title, title_cap, "Error code %u", number);
    static char dialog[APP_ARG_MESSAGE_CAP], full[APP_ARG_MESSAGE_CAP];   // one call at a time: callers hold g_show
    const char *d = app_message(arg, APP_ARG_DIALOG, dialog), *f = app_message(arg, APP_ARG_FULL_SCREEN, full);
    if (d && f && strcmp(d, f)) snprintf(text, text_cap, "%s\n\n%s", d, f);
    else if (d || f) snprintf(text, text_cap, "%s", d ? d : f);
    else snprintf(text, text_cap, "The game reported an error.");
}

// The dialog, or with nobody to close it nothing. 1 = shown and closed, 0 = not shown (no window, scripted input)
static pthread_mutex_t g_show = PTHREAD_MUTEX_INITIALIZER;   // the static message buffers: two game threads may report errors at once
static int scripted_input(void) { const char *keys = getenv("DM_KEYS"), *ctl = getenv("DM_CTL"); return (keys && *keys) || (ctl && *ctl); }
static int show(const char *title, const char *text) {
    LOG("err: the game shows an error: %s: %s", title, text);
    int r = scripted_input() ? -1 : front_message(title, text);   // blocks until the player closes it; -1: no window
    LOG("err: %s", r > 0 ? "the player closed the error dialog" : "no dialog (no window or scripted input), the call returns");
    return r > 0;
}
#define ERR_TEXT_CAP (2 * APP_ARG_MESSAGE_CAP + 2)
int err_show_code(uint64_t code, const char *text) {
    char title[ERR_CODE_LEN + 16], c[ERR_CODE_LEN]; err_code_string(c, code);
    snprintf(title, sizeof title, "Error code %s", c);
    pthread_mutex_lock(&g_show); int r = show(title, text); pthread_mutex_unlock(&g_show);
    return r;
}
int err_show_app(const void *arg) {
    static char title[64], text[ERR_TEXT_CAP];
    pthread_mutex_lock(&g_show);
    err_app_dialog(arg, title, sizeof title, text, sizeof text);
    int r = show(title, text);
    pthread_mutex_unlock(&g_show);
    return r;
}
// nn::err::ErrorResultVariant: {u32 state; then a Result (state 1) or an ErrorCode (state 2) at +4} (its constructors, sdk+0x405c00 / sdk+0x405c20; GetState sdk+0x405ca0).
// The SDK's ShowError of it (sdk+0x403f30) shows the Result's or the ErrorCode's error and aborts on any other state; the host logs that one
int err_show_variant(const void *v_) {
    const uint8_t *v = v_; uint32_t state; memcpy(&state, v, 4);
    if (state == 1) { Result r; memcpy(&r, v + 4, 4); return err_show_code(err_code_of_result(r), "The game reported this error."); }
    if (state == 2) { uint64_t code; memcpy(&code, v + 4, 8); return err_show_code(code, "The game reported this error."); }
    LOG("[RED] err: ShowError of an ErrorResultVariant in state %u, which holds no error (the SDK aborts on it); nothing shown", state);
    return 0;
}

// The stand-ins run on the big host stack (HLE_FORCE_BIG): the dialog's texts and the log lines are formatted on the caller's thread, which can be a game thread with little
// stack left (memory.md, Host calls on small guest stacks)
// The Result codes of the two version errors are the ones the SDK hands the applet: 0xc8a2 (sdk+0x404058) and 0xcaa2 (sdk+0x4040c8), module 162, descriptions 100 and 101
#define RES_APP_VERSION 0xc8a2u
#define RES_AOC_VERSION 0xcaa2u
HLE_FORCE_BIG("_ZN2nn3err9ShowErrorENS_6ResultE", "error applet: a dialog with the Result's code (front_message)", void, h_err_ShowError, Result r) {
    DM_APPLET("error"); LOG("err::ShowError Result=%#x (module %u, description %u)", r, r & 0x1ff, (r >> 9) & 0x1fff);
    err_show_code(err_code_of_result(r), "The game reported this error.");
}
HLE_FORCE_BIG("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "error applet: a dialog with the error code (front_message)", void, h_err_ShowCode, uint64_t code) {
    DM_APPLET("error"); err_show_code(code, "The game reported this error.");
}
HLE_FORCE_BIG("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "error applet: a dialog with the variant's code (front_message)", void, h_err_ShowVar, const void *v) { DM_APPLET("error"); err_show_variant(v); }
HLE_FORCE_BIG("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", "add-on content version error applet: a dialog with its code (front_message)", void, h_err_AocVer, void) {
    DM_APPLET("error"); err_show_code(err_code_of_result(RES_AOC_VERSION), "The add-on content cannot be used with this version of the game.");
}
HLE_FORCE_BIG("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "game version error applet: a dialog with its code (front_message)", void, h_err_AppVer, void) {
    DM_APPLET("error"); err_show_code(err_code_of_result(RES_APP_VERSION), "This version of the game cannot be used.");
}
// Pokémon Legends: Z-A (SDK 19): the application's own error screen (an error code number and two messages in the argument) goes through the error applet as well
HLE_FORCE_BIG("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet: a dialog with the game's code number and message (front_message)", void, h_err_ShowApp, const void *arg) {
    DM_APPLET("error"); err_show_app(arg);
}
// The network service form (ShowNetworkServiceError) stays a log line: hle_offline.c
