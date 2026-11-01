// Status notices: the rules of dm_notices.h.
#include "dm_notices.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dm_notices_init(dm_notices_t *n, const char *env, int thermal_cap) {
    memset(n, 0, sizeof *n);
    n->enabled = !(env && !strcmp(env, "0"));
    n->thermal_cap = thermal_cap; n->prog_pct = -1;
}

// Copies s into out[cap], cut at a character boundary (a controller's name is UTF-8 that a cut must not split).
static void put(char *out, size_t cap, const char *s) {
    size_t n = strlen(s); if (n >= cap) { n = cap - 1; while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    memcpy(out, s, n); out[n] = 0;
}

int dm_notices_for(dm_notices_t *n, const dm_event_t *ev, dm_notice_t *out) {
    memset(out, 0, sizeof *out);
    if (!n->enabled || !ev) return 0;
    switch (ev->kind) {
    case DM_EV_SHADERS: {
        if (!ev->phase || strcmp(ev->phase, "background") || !ev->state) return 0;   // the prepare step has its own screen
        if (!strcmp(ev->state, "done")) { if (!n->prog_on) return 0; n->prog_on = 0; n->prog_pct = -1; out->act = DM_NOTICE_CLEAR_PROGRESS; return 1; }
        int pct = ev->total > 0 ? (int)(100 * (ev->done < ev->total ? ev->done : ev->total) / ev->total) : -1;
        if (n->prog_on && pct == n->prog_pct) return 0;   // the line would read the same
        n->prog_on = 1; n->prog_pct = pct; out->act = DM_NOTICE_PROGRESS;
        if (pct >= 0) snprintf(out->text, sizeof out->text, "Preparing shaders\xE2\x80\xA6 %d%%", pct); else put(out->text, sizeof out->text, "Preparing shaders\xE2\x80\xA6");
        return 1;
    }
    case DM_EV_CONTROLLER: {
        int up = ev->state && !strcmp(ev->state, "connected");
        if (!up && !(ev->state && !strcmp(ev->state, "disconnected"))) return 0;
        out->act = DM_NOTICE_SHOW; out->seconds = 3;
        if (!up) { put(out->text, sizeof out->text, "Controller disconnected"); return 1; }
        char who[64]; put(who, sizeof who, ev->pad_name ? ev->pad_name : "");
        if (*who) snprintf(out->text, sizeof out->text, "Controller connected: %s (player %d)", who, ev->player);
        else snprintf(out->text, sizeof out->text, "Controller connected (player %d)", ev->player);
        return 1;
    }
    case DM_EV_THERMAL: {
        int was = n->thermal_level; n->thermal_level = ev->level;
        if (!n->thermal_cap || ev->level < 2 || ev->level == was) return 0;   // nothing to say below serious, nor where no cap applies, nor again for the same state
        out->act = DM_NOTICE_SHOW; out->seconds = 6;
        put(out->text, sizeof out->text, "The device is hot; the game may hold 30 fps");
        return 1;
    }
    default: return 0;
    }
}

void dm_notice_view_apply(dm_notice_view_t *v, const dm_notice_t *n, double now) {
    switch (n->act) {
    case DM_NOTICE_SHOW: put(v->msg, sizeof v->msg, n->text); v->msg_until = now + n->seconds; break;
    case DM_NOTICE_PROGRESS: put(v->prog, sizeof v->prog, n->text); v->prog_on = 1; break;
    case DM_NOTICE_CLEAR_PROGRESS: v->prog_on = 0; break;
    default: break;
    }
}
const char *dm_notice_view_text(const dm_notice_view_t *v, double now) {
    if (v->msg_until > now) return v->msg;
    return v->prog_on ? v->prog : NULL;
}

static struct { dm_notices_t n; void (*show)(const dm_notice_t *); } g_inst;   // written once before the hook is set, then read by the writer thread alone
static void hook(const dm_event_t *ev, void *user) {
    (void)user; dm_notice_t out;
    if (dm_notices_for(&g_inst.n, ev, &out)) g_inst.show(&out);
}
void dm_notices_install(int thermal_cap, int thermal_now, void (*show)(const dm_notice_t *n)) {
    dm_notices_init(&g_inst.n, getenv("DM_NOTICES"), thermal_cap); g_inst.show = show;
    if (!g_inst.n.enabled) return;
    dm_events_set_hook(hook, NULL);
    dm_event_t start = {.kind = DM_EV_THERMAL, .level = thermal_now > 0 ? thermal_now : 0};   // the state the run starts in, as if it were the first thermal event
    dm_notice_t out; if (dm_notices_for(&g_inst.n, &start, &out)) show(&out);
}
