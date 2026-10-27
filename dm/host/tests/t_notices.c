// Status notices (dm_notices.c): which event shows which notice, the throttling, the off switch, the view's timing, and that the installed hook hands notices to the front end.
#include "../dm_notices.c"
#include <assert.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "t_notices: %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

// dm_events.c is not linked: the hook the module installs is kept here and called by hand, as the events' writer thread would
static void (*g_hook_fn)(const dm_event_t *, void *); static void *g_hook_user;
void dm_events_set_hook(void (*fn)(const dm_event_t *, void *), void *user) { g_hook_fn = fn; g_hook_user = user; }

static dm_event_t shaders(const char *phase, const char *state, long done, long total) { return (dm_event_t){.kind = DM_EV_SHADERS, .phase = phase, .state = state, .done = done, .total = total}; }
static dm_event_t pad(const char *state, int player, const char *name) { return (dm_event_t){.kind = DM_EV_CONTROLLER, .state = state, .player = player, .pad_name = name}; }
static dm_event_t heat(int level) { return (dm_event_t){.kind = DM_EV_THERMAL, .level = level}; }

static void t_shaders(void) {
    dm_notices_t n; dm_notices_init(&n, NULL, 0); dm_notice_t o;
    // the prepare step has its own progress screen
    dm_event_t e = shaders("prewarm", "progress", 5, 10); CHECK(!dm_notices_for(&n, &e, &o) && o.act == DM_NOTICE_NONE);
    e = shaders("prebuild", "start", 0, 10); CHECK(!dm_notices_for(&n, &e, &o));
    e = shaders("prebuild", "done", 10, 10); CHECK(!dm_notices_for(&n, &e, &o));
    // the background prewarm: a line while it runs, one per percent, gone at done
    e = shaders("background", "start", 0, 200); CHECK(dm_notices_for(&n, &e, &o) && o.act == DM_NOTICE_PROGRESS && !strcmp(o.text, "Preparing shaders\xE2\x80\xA6 0%"));
    e = shaders("background", "progress", 1, 200); CHECK(!dm_notices_for(&n, &e, &o));   // still 0%
    e = shaders("background", "progress", 90, 200); CHECK(dm_notices_for(&n, &e, &o) && !strcmp(o.text, "Preparing shaders\xE2\x80\xA6 45%"));
    e = shaders("background", "progress", 90, 200); CHECK(!dm_notices_for(&n, &e, &o));   // the same again
    e = shaders("background", "progress", 999, 200); CHECK(dm_notices_for(&n, &e, &o) && strstr(o.text, "100%"));   // never above 100
    e = shaders("background", "done", 200, 200); CHECK(dm_notices_for(&n, &e, &o) && o.act == DM_NOTICE_CLEAR_PROGRESS);
    e = shaders("background", "done", 200, 200); CHECK(!dm_notices_for(&n, &e, &o));   // nothing to clear
    e = shaders("background", "start", 0, 0); CHECK(dm_notices_for(&n, &e, &o) && !strcmp(o.text, "Preparing shaders\xE2\x80\xA6"));   // no total: no percentage
}
static void t_controller(void) {
    dm_notices_t n; dm_notices_init(&n, NULL, 0); dm_notice_t o;
    dm_event_t e = pad("connected", 2, "Pro Controller"); CHECK(dm_notices_for(&n, &e, &o) && o.act == DM_NOTICE_SHOW && o.seconds >= 2 && o.seconds <= 3 && !strcmp(o.text, "Controller connected: Pro Controller (player 2)"));
    e = pad("connected", 1, NULL); CHECK(dm_notices_for(&n, &e, &o) && !strcmp(o.text, "Controller connected (player 1)"));
    e = pad("connected", 1, ""); CHECK(dm_notices_for(&n, &e, &o) && !strcmp(o.text, "Controller connected (player 1)"));
    e = pad("disconnected", 1, "Pro Controller"); CHECK(dm_notices_for(&n, &e, &o) && !strcmp(o.text, "Controller disconnected"));
    // a long name is cut on a character boundary, never in the middle of a UTF-8 sequence
    char name[200]; memset(name, 'a', 60); name[60] = 0; strcat(name, "\xC3\xA9\xC3\xA9\xC3\xA9");
    e = pad("connected", 1, name); CHECK(dm_notices_for(&n, &e, &o)); CHECK(strlen(o.text) < sizeof o.text);
    for (const unsigned char *p = (const unsigned char *)o.text; *p; p++) if (*p >= 0x80) CHECK(*p == 0xC3 ? p[1] == 0xA9 : *p == 0xA9);
    e = pad("unknown", 1, "x"); CHECK(!dm_notices_for(&n, &e, &o));
}
static void t_thermal(void) {
    dm_notices_t n; dm_notices_init(&n, NULL, 1); dm_notice_t o;
    dm_event_t e;
    e = heat(0); CHECK(!dm_notices_for(&n, &e, &o));
    e = heat(1); CHECK(!dm_notices_for(&n, &e, &o));   // fair: nothing
    e = heat(2); CHECK(dm_notices_for(&n, &e, &o) && o.act == DM_NOTICE_SHOW && !strcmp(o.text, "The device is hot; the game may hold 30 fps") && o.seconds > 3);
    e = heat(2); CHECK(!dm_notices_for(&n, &e, &o));   // once per change
    e = heat(3); CHECK(dm_notices_for(&n, &e, &o));    // critical is a change
    e = heat(1); CHECK(!dm_notices_for(&n, &e, &o));
    e = heat(2); CHECK(dm_notices_for(&n, &e, &o));    // hot again
    // no thermal cap (the Mac): the frame rate is not held, so there is nothing to say
    dm_notices_init(&n, NULL, 0); e = heat(3); CHECK(!dm_notices_for(&n, &e, &o));
}
static void t_other_and_off(void) {
    dm_notices_t n; dm_notices_init(&n, NULL, 1); dm_notice_t o;
    dm_event_t red = {.kind = DM_EV_RED, .text = "[RED] x"}, save = {.kind = DM_EV_SAVE, .save = "save", .mount = "m", .commit = 1}, fl = {.kind = DM_EV_FRAME_LOOP}, end = {.kind = DM_EV_END, .reason = "terminated"};
    CHECK(!dm_notices_for(&n, &red, &o) && !dm_notices_for(&n, &save, &o) && !dm_notices_for(&n, &fl, &o) && !dm_notices_for(&n, &end, &o) && !dm_notices_for(&n, NULL, &o));
    const char *off[] = {"0"}, *on[] = {NULL, "", "1", "yes"};
    dm_notices_init(&n, off[0], 1); dm_event_t c = pad("connected", 1, "x"), h = heat(3), s = shaders("background", "start", 0, 1);
    CHECK(!dm_notices_for(&n, &c, &o) && !dm_notices_for(&n, &h, &o) && !dm_notices_for(&n, &s, &o));
    for (unsigned i = 0; i < sizeof on / sizeof *on; i++) { dm_notices_init(&n, on[i], 1); CHECK(dm_notices_for(&n, &c, &o)); }
}
static void t_view(void) {
    dm_notice_view_t v; memset(&v, 0, sizeof v); dm_notice_t p = {.act = DM_NOTICE_PROGRESS}, m = {.act = DM_NOTICE_SHOW, .seconds = 3}, clr = {.act = DM_NOTICE_CLEAR_PROGRESS};
    snprintf(p.text, sizeof p.text, "Preparing shaders 10%%"); snprintf(m.text, sizeof m.text, "Controller disconnected");
    CHECK(!dm_notice_view_text(&v, 0));
    dm_notice_view_apply(&v, &p, 10); CHECK(!strcmp(dm_notice_view_text(&v, 10), "Preparing shaders 10%"));
    dm_notice_view_apply(&v, &m, 11); CHECK(!strcmp(dm_notice_view_text(&v, 12), "Controller disconnected"));   // a message wins while it lasts
    CHECK(!strcmp(dm_notice_view_text(&v, 14.5), "Preparing shaders 10%"));   // then the progress line is back
    dm_notice_view_apply(&v, &clr, 15); CHECK(!dm_notice_view_text(&v, 15));
    dm_notice_view_apply(&v, &m, 20); dm_notice_view_apply(&v, &clr, 21); CHECK(dm_notice_view_text(&v, 21) != NULL && !dm_notice_view_text(&v, 23.5));
}
static int g_shown; static dm_notice_t g_last;
static void show(const dm_notice_t *n) { g_shown++; g_last = *n; }
static void t_install(void) {
    // DM_NOTICES=0: no hook at all
    setenv("DM_NOTICES", "0", 1); g_hook_fn = NULL; dm_notices_install(1, 0, show); CHECK(!g_hook_fn && !g_shown);
    unsetenv("DM_NOTICES"); dm_notices_install(1, 0, show); CHECK(g_hook_fn && !g_shown);
    if (!g_hook_fn) return;
    dm_event_t c = pad("connected", 1, "Pad"); g_hook_fn(&c, g_hook_user); CHECK(g_shown == 1 && !strcmp(g_last.text, "Controller connected: Pad (player 1)"));
    dm_event_t r = {.kind = DM_EV_RED, .text = "[RED] x"}; g_hook_fn(&r, g_hook_user); CHECK(g_shown == 1);
    // a start on a hot device says so at once, and the first thermal event of the same state does not repeat it
    g_shown = 0; dm_notices_install(1, 2, show); CHECK(g_shown == 1 && strstr(g_last.text, "hot"));
    dm_event_t h = heat(2); g_hook_fn(&h, g_hook_user); CHECK(g_shown == 1);
    h = heat(3); g_hook_fn(&h, g_hook_user); CHECK(g_shown == 2);
    g_shown = 0; dm_notices_install(0, 3, show); CHECK(g_shown == 0);   // no cap: no heat notice
}
int main(void) {
    t_shaders(); t_controller(); t_thermal(); t_other_and_off(); t_view(); t_install();
    if (!g_fail) puts("t_notices: ok");
    return g_fail;
}
