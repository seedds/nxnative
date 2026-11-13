// nn::web offline HTML pages: the pages a game ships in its HtmlDocument NCA (nx_extract --html), shown by the web applet over the game. On the host a web view of the front end
// (web_apple.m: WKWebView on the Mac and the iPhone) shows them over the game picture, with the Switch browser's JavaScript extension (window.nx: content messages both ways, the
// footer's buttons) and the controller as the page reads it (navigator.getGamepads and key events). The page ends when it goes to its callback URL (http://localhost/ by default:
// exit reason 3 with that URL as the last URL), when the default B goes back from its first page (reason 1), when it calls nx.endApplet (reason 0) or when the game asks (reason 2).
// While a page is shown it holds the input (dm_input_hold): the game reads nothing, as an application out of focus.
// Pages: in DM_HTML (default: the folder "html" beside the RomFS folder), in its html-document folder or in html<n>/html-document (one folder per HtmlDocument NCA, as nx_extract
// writes them). Without the page, or without a web view (a windowless run), the stand-in of before answers: the page loads at once, sends one empty content message and closes when
// it appears (exit reason 0, no last URL).
// ShowOfflineHtmlPageArg (0x2000 bytes): u16 entry count at +0, entries from +8, each {u16 type, u16 size, u32 0} and its data. OfflineHtmlPageReturnValue: u64 exit reason at +0,
// the last URL at +8 (0x1000 bytes), its size at +0x1008; its getters stay in the SDK
#include "hle.h"
#include "front.h"
#include "dm_input.h"
#include <pthread.h>
#include <sys/stat.h>

enum { TLV_CALLBACK_URL = 0x3, TLV_DOCUMENT_PATH = 0x6, TLV_KEY_REPEAT_DELAY = 0x1c, TLV_KEY_REPEAT_PULSE = 0x1d, TLV_BOOT_MODE = 0x41 };
enum { ARG_SIZE = 0x2000, URL_SIZE = 0x1000, MSG_MAX = 32 };
enum { BOOT_ALL_FOREGROUND = 0, BOOT_INITIALLY_HIDDEN = 1 };   // nn::web::WebSessionBootMode

// The value of entry `type`, NULL when the argument has none; *size = its size
static const uint8_t *web_tlv(const uint8_t *arg, unsigned type, unsigned *size) {
    unsigned n = *(const uint16_t *)arg; size_t o = 8;
    for (unsigned i = 0; i < n && o + 8 <= ARG_SIZE; i++) {
        unsigned t = *(const uint16_t *)(arg + o), sz = *(const uint16_t *)(arg + o + 2);
        if (o + 8 + sz > ARG_SIZE) break;
        if (t == type) { if (size) *size = sz; return arg + o + 8; }
        o += 8 + sz;
    }
    return NULL;
}
static void web_tlv_str(const uint8_t *arg, unsigned type, char *out, size_t n) {
    unsigned sz = 0; const uint8_t *v = web_tlv(arg, type, &sz);
    out[0] = 0; if (v) snprintf(out, n, "%.*s", (int)strnlen((const char *)v, sz), (const char *)v);
}
static uint32_t web_tlv_u32(const uint8_t *arg, unsigned type, uint32_t def) { unsigned sz = 0; const uint8_t *v = web_tlv(arg, type, &sz); return v && sz >= 4 ? *(const uint32_t *)v : def; }

// The folder that holds `path` (the document path, its query cut off): DM_HTML, its html-document and html<n>/html-document folders. 0 = not found
static int web_find(const char *path, char *dir, size_t n) {
    char file[1024], root[1024]; snprintf(file, sizeof file, "%.*s", (int)strcspn(path, "?#"), path);
    if (!*file || *file == '/' || strstr(file, "..")) return 0;   // a path inside the document only
    const char *e = getenv("DM_HTML"), *rom = getenv("DM_ROMFS"), *r = getenv("DM_ROOT");
    if (e) snprintf(root, sizeof root, "%s", e);
    else if (rom) {   // beside the RomFS folder
        int k = (int)strlen(rom); while (k > 1 && rom[k - 1] == '/') k--;
        while (k > 0 && rom[k - 1] != '/') k--;
        if (k > 1) snprintf(root, sizeof root, "%.*shtml", k, rom); else snprintf(root, sizeof root, "%shtml", k ? "/" : "");
    } else snprintf(root, sizeof root, "%s%s/html", r ?: getenv("HOME") ?: "", r ? "" : "/dm-work");   // as the RomFS's default, $DM_ROOT/romfs
    for (int k = -2; k < 16; k++) {
        char d[1200], f[2300]; struct stat st;
        if (k == -2) snprintf(d, sizeof d, "%s/html-document", root); else if (k == -1) snprintf(d, sizeof d, "%s", root); else snprintf(d, sizeof d, "%s/html%d/html-document", root, k);
        snprintf(f, sizeof f, "%s/%s", d, file);
        if (!stat(f, &st) && S_ISREG(st.st_mode)) { snprintf(dir, n, "%s", d); return 1; }
    }
    return 0;
}

// No front end (a windowless run): no web view, the stand-in answers. web_apple.m defines these (and the unit test, DM_TEST_WEB_FRONT)
#ifndef DM_TEST_WEB_FRONT
__attribute__((weak)) int front_web_open(const char *dir, const char *path, const char *callback, int hidden, const dm_web_opts *o) { (void)dir; (void)path; (void)callback; (void)hidden; (void)o; return -1; }
__attribute__((weak)) void front_web_show(void) {}
__attribute__((weak)) void front_web_send(const char *msg, size_t n) { (void)msg; (void)n; }
__attribute__((weak)) void front_web_close(void) {}
__attribute__((weak)) void front_web_shot(const char *path) { (void)path; }
#endif

// One page at a time, as the applet. state: 0 none, 1 started (loading or loaded, hidden), 2 shown, 3 ended (the exit event is signalled, WaitForExit takes the result)
static _Alignas(16) uint8_t g_ev_web[0x100];
static pthread_mutex_t g_web_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_web_cv = PTHREAD_COND_INITIALIZER;
static int g_web_state, g_web_real, g_web_held;   // g_web_real: a web view shows the page (0: the stand-in); g_web_held: the page holds the input
static uint32_t g_web_reason; static char g_web_url[URL_SIZE];
static char *g_web_msg[MSG_MAX]; static size_t g_web_msg_n[MSG_MAX]; static int g_web_msg_h, g_web_msg_c;   // messages from the page, oldest at g_web_msg_h

static void msg_clear(void) { while (g_web_msg_c) { free(g_web_msg[g_web_msg_h]); g_web_msg_h = (g_web_msg_h + 1) % MSG_MAX; g_web_msg_c--; } g_web_msg_h = 0; }
static void msg_push(const char *m, size_t n) {   // under g_web_mu
    if (g_web_msg_c == MSG_MAX) { LOG("[RED] web: %d content messages from the page not taken by the game; the oldest is dropped", MSG_MAX); free(g_web_msg[g_web_msg_h]); g_web_msg_h = (g_web_msg_h + 1) % MSG_MAX; g_web_msg_c--; }
    int i = (g_web_msg_h + g_web_msg_c++) % MSG_MAX;
    g_web_msg[i] = malloc(n + 1); memcpy(g_web_msg[i], m, n); g_web_msg[i][n] = 0; g_web_msg_n[i] = n;
}
static void web_end_locked(uint32_t reason, const char *url) {   // the page ends: hidden, the input given back, the exit event fired
    if (g_web_state == 0 || g_web_state == 3) return;
    g_web_reason = reason; snprintf(g_web_url, sizeof g_web_url, "%s", url ?: "");
    g_web_state = 3;
    if (g_web_held) { g_web_held = 0; dm_input_hold(0); }
    hle_signal_system_event(g_ev_web);
    pthread_cond_broadcast(&g_web_cv);
}

// Front end reports (main thread)
void dm_web_message(const char *msg, size_t n) {
    pthread_mutex_lock(&g_web_mu);
    static int logged; if (logged++ < 16) LOG("web: content message from the page: \"%.*s\"", (int)(n > 80 ? 80 : n), msg);
    if (g_web_state == 1 || g_web_state == 2) msg_push(msg, n);
    pthread_mutex_unlock(&g_web_mu);
}
void dm_web_ended(uint32_t reason, const char *url) {
    pthread_mutex_lock(&g_web_mu);
    if (g_web_state == 1 || g_web_state == 2) LOG("web: the page ended: exit reason %u, last URL %s", reason, url && *url ? url : "(none)");
    web_end_locked(reason, url);
    pthread_mutex_unlock(&g_web_mu);
}
void dm_web_failed(const char *why) {   // the page did not load: the stand-in takes over (its empty ready message, closed when it appears)
    pthread_mutex_lock(&g_web_mu);
    if (g_web_real && (g_web_state == 1 || g_web_state == 2)) {
        LOG("[RED] web: the page could not be shown (%s); the stand-in answers", why ?: "?");
        g_web_real = 0;
        if (g_web_state == 1) msg_push("", 0); else web_end_locked(0, NULL);
    }
    pthread_mutex_unlock(&g_web_mu);
}

// Start: the page is looked up and opened; hidden until Appear when the argument boots it initially hidden (forced off for the synchronous form).
// These path buffers and web_find's take about 11 KB, more than an 8 KB guest stack: the entry points run on the host stack, as every HLE_FORCE take-over (issue #2541).
static void web_start(const uint8_t *arg, int sync) {
    char path[0xc00], cb[0x400], dir[1024];
    web_tlv_str(arg, TLV_DOCUMENT_PATH, path, sizeof path); web_tlv_str(arg, TLV_CALLBACK_URL, cb, sizeof cb);
    if (!*cb) snprintf(cb, sizeof cb, "http://localhost/");
    int hidden = !sync && web_tlv_u32(arg, TLV_BOOT_MODE, BOOT_ALL_FOREGROUND) == BOOT_INITIALLY_HIDDEN;
    dm_web_opts o = {web_tlv_u32(arg, TLV_KEY_REPEAT_DELAY, 0), web_tlv_u32(arg, TLV_KEY_REPEAT_PULSE, 0)};
    pthread_mutex_lock(&g_web_mu);
    if (g_web_state) { front_web_close(); if (g_web_held) dm_input_hold(0); }
    msg_clear(); g_web_held = 0; g_web_reason = 0; g_web_url[0] = 0; g_web_state = 1;
    g_web_real = web_find(path, dir, sizeof dir) && front_web_open(dir, path, cb, hidden, &o) == 0;
    static int n; if (n++ < 8) {
        if (g_web_real) LOG("web: offline page %s from %s (%s, callback URL %s)", path, dir, hidden ? "hidden until it appears" : "shown", cb);
        else LOG("web: offline page %s: %s; the stand-in answers (it loads at once and closes when it appears)", path, web_find(path, dir, sizeof dir) ? "no web view (windowless)" : "not found (DM_HTML)");
    }
    if (!g_web_real) msg_push("", 0);   // the stand-in's ready message: the game waits for any message before Appear (the pause menu sends end_sound_load)
    if (g_web_real && !hidden) { g_web_state = 2; g_web_held = 1; dm_input_hold(1); }
    pthread_mutex_unlock(&g_web_mu);
}
static void web_return(void *rv) {   // the ended page's result into the return value; the session is over
    pthread_mutex_lock(&g_web_mu);
    if (rv) { *(uint64_t *)rv = g_web_reason; snprintf((char *)rv + 8, URL_SIZE, "%s", g_web_url); *(uint64_t *)((char *)rv + 8 + URL_SIZE) = g_web_url[0] ? strlen(g_web_url) + 1 : 0; }
    if (g_web_real) front_web_close();
    msg_clear(); g_web_state = 0; g_web_real = 0; hle_clear_system_event(g_ev_web);
    pthread_mutex_unlock(&g_web_mu);
}
static void web_wait_end(void) { pthread_mutex_lock(&g_web_mu); while ((g_web_state == 1 || g_web_state == 2)) pthread_cond_wait(&g_web_cv, &g_web_mu); pthread_mutex_unlock(&g_web_mu); }

// The page as a session the game drives while it runs (Super Mario 3D All-Stars' Sunshine: its launcher pause menu, launcher_pause_menu.htdocs, started hidden at boot and shown with
// Appear when the player suspends; Super Mario 64 imports the same). The SDK's own Start creates a library applet (null read in applet::StartLibraryAppletImmediately)
HLE_FORCE("_ZN2nn3web17OfflineWebSessionC1Ev", "offline HTML session (host web view, no applet): nothing to construct", void, h_webs_Ctor, void *s) { (void)s; }
HLE_FORCE("_ZN2nn3web17OfflineWebSessionD1Ev", "offline HTML session (host web view)", void, h_webs_Dtor, void *s) { (void)s; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession17GetWorkBufferSizeEv", "offline HTML session work buffer: the SDK's size (0xd0), unused by the host", size_t, h_webs_WorkSize, void) { return 0xd0; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession10InitializeEPvm", "offline HTML session (host web view): the work buffer is not used", void, h_webs_Init, void *s, void *buf, size_t n) { (void)s; (void)buf; (void)n; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession8FinalizeEv", "offline HTML session: a page still open is closed", void, h_webs_Fini, void *s) {
    (void)s; pthread_mutex_lock(&g_web_mu); int open = g_web_state == 1 || g_web_state == 2; pthread_mutex_unlock(&g_web_mu);
    if (open) { dm_web_ended(2, NULL); web_return(NULL); }
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession5StartEPPNS_2os15SystemEventTypeERKNS0_22ShowOfflineHtmlPageArgE", "start an offline HTML page in the host's web view (the stand-in without the page)",
          Result, h_webs_Start, void *s, void **ev, const uint8_t *arg) {
    (void)s;
    hle_bind_system_event(g_ev_web, 0); hle_clear_system_event(g_ev_web);
    if (ev) *ev = g_ev_web;
    web_start(arg, 0);
    return RES_OK;
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession6AppearEv", "show the offline HTML page (the stand-in closes at once)", unsigned, h_webs_Appear, void *s) {
    (void)s;
    pthread_mutex_lock(&g_web_mu);
    if (g_web_state == 1 && g_web_real) {
        static int n; if (n++ < 8) LOG("web::OfflineWebSession::Appear: the page is shown and holds the input");
        g_web_state = 2; g_web_held = 1; dm_input_hold(1); front_web_show();
    }
    else if (g_web_state == 1 || g_web_state == 2) {
        static int n; if (n++ < 8) LOG("web::OfflineWebSession::Appear: the stand-in closes at once (no page shown)");
        g_web_state = 2; web_end_locked(0, NULL);
    }
    pthread_mutex_unlock(&g_web_mu);
    return 1;
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession21TrySendContentMessageEPKcm", "content message to the offline HTML page", unsigned, h_webs_Send, void *s, const char *msg, size_t n) {
    (void)s;
    pthread_mutex_lock(&g_web_mu);
    if (g_web_real && (g_web_state == 1 || g_web_state == 2)) front_web_send(msg, msg ? strnlen(msg, n) : 0);
    pthread_mutex_unlock(&g_web_mu);
    return 1;
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession24TryReceiveContentMessageEPmPcm", "content message from the offline HTML page, oldest first (size without the terminating zero)",
          unsigned, h_webs_Recv, void *s, size_t *out, char *buf, size_t n) {
    (void)s;
    pthread_mutex_lock(&g_web_mu);
    int got = g_web_msg_c > 0;
    if (got) {
        int i = g_web_msg_h; size_t k = g_web_msg_n[i] < n ? g_web_msg_n[i] : n ? n - 1 : 0;
        if (buf && n) { memcpy(buf, g_web_msg[i], k); buf[k] = 0; }
        if (out) *out = k;
        free(g_web_msg[i]); g_web_msg_h = (i + 1) % MSG_MAX; g_web_msg_c--;
    }
    pthread_mutex_unlock(&g_web_mu);
    return got;
}
HLE_FORCE("_ZNK2nn3web17OfflineWebSession16NeedsWaitForExitEv", "whether the offline HTML page was started and not waited for", unsigned, h_webs_Needs, const void *s) {
    (void)s; pthread_mutex_lock(&g_web_mu); int r = g_web_state != 0; pthread_mutex_unlock(&g_web_mu); return r; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession11RequestExitEv", "close the offline HTML page at the game's request (exit reason 2; the exit event fires)", void, h_webs_ReqExit, void *s) {
    (void)s; LOG("web::OfflineWebSession::RequestExit"); dm_web_ended(2, NULL); }
HLE_FORCE("_ZN2nn3web17OfflineWebSession11WaitForExitEPNS0_26OfflineHtmlPageReturnValueE", "wait for the offline HTML page's end: its exit reason and last URL", Result, h_webs_Wait, void *s, void *rv) {
    (void)s; web_wait_end(); web_return(rv); return RES_OK; }

// The page as an applet the calling thread waits for (Octopath Traveler 0 imports it): shown at once; without the page the cut of before (fails)
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "offline HTML page in the host's web view, waited for (fails without the page)",
          Result, h_web_Offline, void *rv, const uint8_t *arg) {
    hle_bind_system_event(g_ev_web, 0); hle_clear_system_event(g_ev_web);
    web_start(arg, 1);
    pthread_mutex_lock(&g_web_mu); int real = g_web_real; pthread_mutex_unlock(&g_web_mu);
    if (!real) { web_return(NULL); LOG("web::ShowOfflineHtmlPage: no page shown (cut)"); return RES_FAIL; }
    web_wait_end(); web_return(rv);
    return RES_OK;
}
