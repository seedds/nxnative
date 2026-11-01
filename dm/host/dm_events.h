// Runtime events (docs/frontends.md, Runtime events): the host and the Mac launcher say what happens while a game runs, as one JSON object per line in the envelope of the NSP script's
// build events (`ev`, `t`), for a front end outside the app, and as typed callbacks for the app's own front end (front_mac.m, front_ios.m), which never parse JSON.
//   DM_EVENTS=<path>   appended to (created); DM_EVENTS=fd:<N>   written to descriptor N (above 2: 1 and 2 are the log), which a child inherits. Unset: every call below is one atomic load.
// Delivery is off the caller's critical path: a call copies the event into a small queue under a mutex held for a few instructions; a background thread writes each line with one write() and runs the hook.
// The queue drops what does not fit (counted into the next line's `dropped`); a failed write turns the stream off with one [RED] log line. Nothing here can stop or crash the game.
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef enum { DM_EV_FRAME_LOOP = 1, DM_EV_SHADERS, DM_EV_RED, DM_EV_END, DM_EV_EXIT } dm_ev_kind;
typedef struct {
    dm_ev_kind kind;
    const char *name;     // the `ev` of the line
    double t;             // Unix time, seconds
    double elapsed;       // seconds: since the process started (frame_loop), since the phase started (shaders)
    const char *phase;    // shaders: "prewarm" or "prebuild"
    const char *state;    // shaders: "start", "progress" or "done"
    long done, total;     // shaders
    const char *text;     // red: the log line without its "[dm] " prefix; end: a detail ("" when none)
    const char *reason;   // end, exit
    int code, sig;        // exit: the exit code (-1 when the host died on a signal) and the signal (0 when it did not)
    unsigned dropped;     // events lost before this one (queue full, or [RED] lines over the rate limit)
} dm_event_t;

#ifdef __cplusplus
extern "C" {
#endif
// Sets the stream from a DM_EVENTS value ("" or NULL: none). The environment is read once at start-up; call this only before the first event (tests). 1 when a stream is on.
int dm_events_configure(const char *spec);
// The typed hook: called on the writer thread, in order, for every event, with or without a stream. It must not block for long; there is one.
void dm_events_set_hook(void (*fn)(const dm_event_t *ev, void *user), void *user);
// Waits until everything queued is written and handed to the hook, at most `ms`: before an exit that does not return to main (never on the render path).
void dm_events_flush(int ms);
void dm_event_frame_loop(void);   // the first present; later calls do nothing
void dm_event_shaders(const char *phase, const char *state, long done, long total, double elapsed);
void dm_event_end(const char *reason, const char *text);   // the host knows why it is ending
void dm_event_exit(int code, int sig, const char *reason);   // the supervising launcher: how the host ended
void dm_log_red(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   // a [RED] log line: "[dm] <tag><text>\n" on stderr, and the event
#ifdef __cplusplus
}
#endif

// The unit tests and tools that compile a few host files without dm_events.c get these (weak, overridden by the real ones).
#ifndef DM_EVENTS_IMPL
__attribute__((weak)) void dm_event_frame_loop(void) {}
__attribute__((weak)) void dm_event_shaders(const char *phase, const char *state, long done, long total, double elapsed) { (void)phase; (void)state; (void)done; (void)total; (void)elapsed; }
__attribute__((weak)) void dm_event_end(const char *reason, const char *text) { (void)reason; (void)text; }
__attribute__((weak)) void dm_events_flush(int ms) { (void)ms; }
__attribute__((weak)) void dm_log_red(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[dm] %s", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
#endif

// LOG-style macros: a format that starts with "[RED] " (after at most one "[vk] ", "[gl] ", "[nvn] " or "[nvn-mtl] " tag) also becomes a `red` event. The test is made on the literal at compile time.
#define DM_LOG_FMT1(f, ...) f
#define DM_LOG_IS_RED(f) (!__builtin_strncmp(f, "[RED] ", 6) || !__builtin_strncmp(f, "[vk] [RED] ", 11) || !__builtin_strncmp(f, "[gl] [RED] ", 11) || \
                          !__builtin_strncmp(f, "[nvn] [RED] ", 12) || !__builtin_strncmp(f, "[nvn-mtl] [RED] ", 16))
// The message and its newline under one stream lock: written apart, another thread's line could land between them and two messages share a log line
#define DM_LOG_PLAIN(tag, ...) do { flockfile(stderr); fprintf(stderr, "[dm] " tag __VA_ARGS__); fputc('\n', stderr); funlockfile(stderr); } while (0)
#ifdef __clang_analyzer__
#define DM_LOG_TAGGED DM_LOG_PLAIN   // the static analyzer sees the plain line: the red branch changes its path-sensitive results in files that never log a red line
#else
#define DM_LOG_TAGGED(tag, ...) do { \
        if (DM_LOG_IS_RED(DM_LOG_FMT1(__VA_ARGS__, 0))) dm_log_red(tag, __VA_ARGS__); \
        else DM_LOG_PLAIN(tag, __VA_ARGS__); } while (0)
#endif
