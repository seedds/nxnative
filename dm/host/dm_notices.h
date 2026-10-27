// Status notices (docs/frontends.md, Runtime events, "The app's own notices"): which runtime event shows which small notice in the game's own app, as pure functions, so the rules are tested
// without a window (tests/t_notices.c). front_mac.m and front_ios.m install it (dm_notices_install) and only draw what it hands them: one pill at the top of the game's view.
//   shaders  phase "background" only (the prewarm that runs while the game does): "Preparing shaders... 45%" while it runs, gone at done. The prepare step before the first frame
//            (phases prewarm, prebuild) has its own progress screen (front_progress), so the events of those phases show nothing here.
//   controller  "Controller connected: <name> (player N)" / "Controller disconnected", 3 s.
//   thermal  serious or critical, once per change into it, 6 s, and only where the thermal cap holds the frame step at 30 fps (the iPhone, unless DM_THERMAL_CAP=0); nominal and fair show nothing.
//   red, save, frame_loop, end, exit  nothing: [RED] lines are diagnostics for the log, not for a player; games show their own save icon.
// DM_NOTICES=0 turns every notice off (the settings screens' "Status notices").
#pragma once
#include "dm_events.h"

typedef enum { DM_NOTICE_NONE = 0, DM_NOTICE_SHOW, DM_NOTICE_PROGRESS, DM_NOTICE_CLEAR_PROGRESS } dm_notice_act;
typedef struct { dm_notice_act act; char text[112]; double seconds; } dm_notice_t;   // SHOW: text for `seconds`; PROGRESS: text until CLEAR_PROGRESS

// The rules' state, touched by one thread (the events' writer thread).
typedef struct { int enabled, thermal_cap, thermal_level, prog_on, prog_pct; } dm_notices_t;
void dm_notices_init(dm_notices_t *n, const char *env, int thermal_cap);   // env: DM_NOTICES ("0" turns the notices off)
int dm_notices_for(dm_notices_t *n, const dm_event_t *ev, dm_notice_t *out);   // 1 when `out` says what to do for this event

// What the view shows now: a message for its time, else the progress line, else nothing. Main thread only.
typedef struct { char msg[112], prog[112]; double msg_until; int prog_on; } dm_notice_view_t;
void dm_notice_view_apply(dm_notice_view_t *v, const dm_notice_t *n, double now);
const char *dm_notice_view_text(const dm_notice_view_t *v, double now);   // NULL: show nothing

// Installs the hook that turns events into notices and calls `show` for each (on the events' writer thread: the front end switches to its main thread and must not block).
// thermal_cap: 1 where the frame step holds 30 fps while the device is hot; thermal_now: the thermal state at this moment (0 to 3, NSProcessInfo.thermalState), so a start on a hot device
// says so at once and only a change after that does again. Reads DM_NOTICES once. Call after the launcher applied the settings and before the first event.
void dm_notices_install(int thermal_cap, int thermal_now, void (*show)(const dm_notice_t *n));
