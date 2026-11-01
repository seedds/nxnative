// Runtime events: see dm_events.h and docs/frontends.md (Runtime events). Self-contained (no dm.h): the Mac launcher includes this file.
#define DM_EVENTS_IMPL
#include "dm_events.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define QN 256            // queued events; more are dropped
#define RED_PER_SEC 20    // [RED] lines become events at most this often; the rest count as dropped
typedef struct { dm_ev_kind kind; double t, elapsed; long done, total; int code, sig; unsigned dropped; int player, level; long commit; char phase[16], state[16], reason[32], text[400], pad_name[64], pad_kind[64], type[16], save[16], mount[48]; } slot_t;
static slot_t g_q[QN];
static unsigned g_head, g_tail, g_lost;   // under g_mu: pop at g_head, push at g_tail (QN slots), events lost since the last pop
static int g_busy, g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_idle = PTHREAD_COND_INITIALIZER;
static _Atomic int g_on;            // a stream or a hook is set
static int g_fd = -1;               // the stream (-1: none or turned off)
static void (*g_hook)(const dm_event_t *, void *); static void *g_hook_user;
static double g_t0;                 // process start, CLOCK_MONOTONIC seconds
static double g_red_sec; static int g_red_n;
static _Atomic int g_frame_loop;
static _Atomic int g_thermal = -1;   // the last thermal level reported

static double mono(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + ts.tv_nsec / 1e9; }
static double wall(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (double)ts.tv_sec + ts.tv_nsec / 1e9; }

static void off_note(const char *why, int err) {   // the one line a failed stream leaves in the log
    fprintf(stderr, "[dm] [RED] DM_EVENTS: %s (%s): runtime events are off for this run\n", why, err ? strerror(err) : "invalid value");
}

int dm_events_configure(const char *spec) {
    int fd = -1;
    if (!spec || !*spec) return 0;
    if (!strncmp(spec, "fd:", 3)) {
        char *end; long n = strtol(spec + 3, &end, 10);
        if (!spec[3] || *end || n < 3 || n > 1 << 20 || fcntl((int)n, F_GETFD) < 0) { off_note("fd:N needs an open descriptor above 2", errno); return 0; }
        fd = (int)n;
    } else {
        fd = open(spec, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) { off_note("cannot open the file", errno); return 0; }
    }
    fcntl(fd, F_SETNOSIGPIPE, 1);   // a reader that went away gives EPIPE, not a signal that kills the game
    pthread_mutex_lock(&g_mu); g_fd = fd; pthread_mutex_unlock(&g_mu);
    atomic_store(&g_on, 1);
    return 1;
}
// The host reads DM_EVENTS when it loads. The launcher (DM_EVENTS_MANUAL) calls dm_events_configure itself once its log and its env files are in place.
#ifndef DM_EVENTS_MANUAL
__attribute__((constructor)) static void events_init(void) { g_t0 = mono(); dm_events_configure(getenv("DM_EVENTS")); }
#endif

void dm_events_set_hook(void (*fn)(const dm_event_t *, void *), void *user) {
    pthread_mutex_lock(&g_mu); g_hook = fn; g_hook_user = user; pthread_mutex_unlock(&g_mu);
    if (fn) atomic_store(&g_on, 1);
}

// JSON string body (without quotes) of s into out: escapes quotes, backslashes and controls; a byte that is not valid UTF-8 becomes U+FFFD. Returns the length written (always terminated).
static size_t json_str(char *out, size_t n, const char *s) {
    size_t o = 0; out[0] = 0;   // an empty string leaves an empty body
#define PUT(...) do { int w_ = snprintf(out + o, n - o, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= n - o) { out[o] = 0; return o; } o += (size_t)w_; } while (0)
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p;
        if (c == '"') { PUT("\\\""); p++; }
        else if (c == '\\') { PUT("\\\\"); p++; }
        else if (c == '\n') { PUT("\\n"); p++; }
        else if (c == '\r') { PUT("\\r"); p++; }
        else if (c == '\t') { PUT("\\t"); p++; }
        else if (c < 0x20 || c == 0x7f) { PUT("\\u%04x", c); p++; }
        else if (c < 0x80) { PUT("%c", (int)c); p++; }
        else {
            int len = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0, ok = len && c < 0xf5;
            for (int i = 1; ok && i < len; i++) if ((p[i] & 0xc0) != 0x80) ok = 0;
            if (ok && len == 3 && ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0))) ok = 0;
            if (ok && len == 4 && ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90))) ok = 0;
            if (ok) { PUT("%.*s", len, (const char *)p); p += len; } else { PUT("\\ufffd"); p++; }
        }
    }
#undef PUT
    return o;
}

static const char *const NAMES[] = {NULL, "frame_loop", "shaders", "red", "end", "exit", "controller", "thermal", "save"};   // the `ev` of each dm_ev_kind (docs/frontends.md lists them; test_runtime_events.py checks)

static size_t format_line(const slot_t *s, char *b, size_t n) {   // one JSON object and a newline
    char a[1024], r[128], k[128], ty[32];
    int o = snprintf(b, n, "{\"ev\":\"%s\",\"t\":%.3f", NAMES[s->kind], s->t);
    switch (s->kind) {
    case DM_EV_FRAME_LOOP: o += snprintf(b + o, n - o, ",\"elapsed\":%.3f", s->elapsed); break;
    case DM_EV_SHADERS: o += snprintf(b + o, n - o, ",\"phase\":\"%s\",\"state\":\"%s\",\"done\":%ld,\"total\":%ld,\"elapsed\":%.3f", s->phase, s->state, s->done, s->total, s->elapsed); break;
    case DM_EV_RED: json_str(a, sizeof a, s->text); o += snprintf(b + o, n - o, ",\"text\":\"%s\"", a); break;
    case DM_EV_END: json_str(r, sizeof r, s->reason); json_str(a, sizeof a, s->text);
        o += snprintf(b + o, n - o, ",\"reason\":\"%s\"", r); if (*s->text) o += snprintf(b + o, n - o, ",\"text\":\"%s\"", a); break;
    case DM_EV_EXIT: json_str(r, sizeof r, s->reason);
        o += snprintf(b + o, n - o, ",\"code\":"); o += s->code >= 0 ? snprintf(b + o, n - o, "%d", s->code) : snprintf(b + o, n - o, "null");
        o += s->sig ? snprintf(b + o, n - o, ",\"signal\":%d", s->sig) : snprintf(b + o, n - o, ",\"signal\":null");
        o += snprintf(b + o, n - o, ",\"reason\":\"%s\"", r); break;
    case DM_EV_CONTROLLER: json_str(a, sizeof a, s->pad_name); json_str(k, sizeof k, s->pad_kind); json_str(ty, sizeof ty, s->type);
        o += snprintf(b + o, n - o, ",\"state\":\"%s\",\"player\":%d,\"name\":\"%s\",\"kind\":\"%s\",\"type\":\"%s\"", s->state, s->player, a, k, ty); break;
    case DM_EV_THERMAL: o += snprintf(b + o, n - o, ",\"state\":\"%s\",\"level\":%d", s->state, s->level); break;
    case DM_EV_SAVE: json_str(a, sizeof a, s->mount);
        o += snprintf(b + o, n - o, ",\"save\":\"%s\",\"mount\":\"%s\",\"commit\":%ld", s->save, a, s->commit); break;
    }
    if (s->dropped) o += snprintf(b + o, n - o, ",\"dropped\":%u", s->dropped);
    o += snprintf(b + o, n - o, "}\n");
    return (size_t)o < n ? (size_t)o : n - 1;
}

static void *writer(void *arg) {
    (void)arg; pthread_setname_np("dm.events");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_head == g_tail) { g_busy = 0; pthread_cond_broadcast(&g_idle); pthread_cond_wait(&g_cv, &g_mu); }
        slot_t s = g_q[g_head % QN]; g_head++; g_busy = 1; s.dropped += g_lost; g_lost = 0;
        int fd = g_fd; void (*hook)(const dm_event_t *, void *) = g_hook; void *user = g_hook_user;
        pthread_mutex_unlock(&g_mu);
        if (hook) {
            dm_event_t e = {.kind = s.kind, .name = NAMES[s.kind], .t = s.t, .elapsed = s.elapsed, .phase = s.phase, .state = s.state, .done = s.done, .total = s.total,
                            .text = s.text, .reason = s.reason, .code = s.code, .sig = s.sig, .player = s.player, .level = s.level, .pad_name = s.pad_name, .pad_kind = s.pad_kind, .type = s.type,
                            .save = s.save, .mount = s.mount, .commit = s.commit, .dropped = s.dropped};
            hook(&e, user);
        }
        if (fd >= 0) {
            char line[4096]; size_t n = format_line(&s, line, sizeof line); ssize_t w;
            do w = write(fd, line, n); while (w < 0 && errno == EINTR);
            if (w != (ssize_t)n) {   // a full disk, a closed or full pipe, a bad descriptor: off, once
                int err = w < 0 ? errno : EIO;
                pthread_mutex_lock(&g_mu); g_fd = -1; pthread_mutex_unlock(&g_mu);
                off_note("write failed", err);
            }
        }
    }
    return NULL;
}

static void push(const slot_t *s, int is_red) {
    pthread_mutex_lock(&g_mu);
    if (is_red) {
        double m = mono();
        if (m - g_red_sec >= 1) { g_red_sec = m; g_red_n = 0; }
        if (++g_red_n > RED_PER_SEC) { g_lost++; pthread_mutex_unlock(&g_mu); return; }
    }
    if (g_tail - g_head >= QN) { g_lost++; pthread_mutex_unlock(&g_mu); return; }
    if (!g_thread) {
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        g_thread = pthread_create(&th, &at, writer, NULL) == 0 ? 1 : -1; pthread_attr_destroy(&at);
        if (g_thread < 0) { g_lost++; pthread_mutex_unlock(&g_mu); return; }
    }
    g_q[g_tail % QN] = *s; g_tail++; g_busy = 1;
    pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}

static void emit(slot_t *s, int is_red) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    s->t = wall(); push(s, is_red);
}
static void copy(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s ? s : ""); }

void dm_events_flush(int ms) {
    if (!atomic_load(&g_on)) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    while (g_thread > 0 && (g_head != g_tail || g_busy)) if (pthread_cond_timedwait(&g_idle, &g_mu, &ts)) break;
    pthread_mutex_unlock(&g_mu);
}

void dm_event_frame_loop(void) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed) || atomic_exchange(&g_frame_loop, 1)) return;
    slot_t s = {.kind = DM_EV_FRAME_LOOP, .elapsed = mono() - g_t0}; emit(&s, 0);
}
void dm_event_shaders(const char *phase, const char *state, long done, long total, double elapsed) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    slot_t s = {.kind = DM_EV_SHADERS, .done = done, .total = total, .elapsed = elapsed}; copy(s.phase, sizeof s.phase, phase); copy(s.state, sizeof s.state, state); emit(&s, 0);
}
void dm_event_end(const char *reason, const char *text) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    slot_t s = {.kind = DM_EV_END}; copy(s.reason, sizeof s.reason, reason); copy(s.text, sizeof s.text, text); emit(&s, 0);
}
void dm_event_exit(int code, int sig, const char *reason) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    slot_t s = {.kind = DM_EV_EXIT, .code = code, .sig = sig}; copy(s.reason, sizeof s.reason, reason); emit(&s, 0);
}
void dm_event_controller(int connected, int player, const char *name, const char *kind, const char *type) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    slot_t s = {.kind = DM_EV_CONTROLLER, .player = player}; copy(s.state, sizeof s.state, connected ? "connected" : "disconnected");
    copy(s.pad_name, sizeof s.pad_name, name); copy(s.pad_kind, sizeof s.pad_kind, kind); copy(s.type, sizeof s.type, type); emit(&s, 0);
}
void dm_event_thermal(int level) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed) || level < 0 || level > 3 || atomic_exchange(&g_thermal, level) == level) return;
    static const char *const N[] = {"nominal", "fair", "serious", "critical"};
    slot_t s = {.kind = DM_EV_THERMAL, .level = level}; copy(s.state, sizeof s.state, N[level]); emit(&s, 0);
}
void dm_event_save(const char *save, const char *mount, long commit) {
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) return;
    slot_t s = {.kind = DM_EV_SAVE, .commit = commit}; copy(s.save, sizeof s.save, save); copy(s.mount, sizeof s.mount, mount); emit(&s, 0);
}
void dm_log_red(const char *tag, const char *fmt, ...) {
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    flockfile(stderr); fprintf(stderr, "[dm] %s", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr); funlockfile(stderr);   // the log line, whole, under one stream lock (as LOG on main)
    va_end(ap);
    if (atomic_load_explicit(&g_on, memory_order_relaxed)) {
        slot_t s = {.kind = DM_EV_RED}; int n = snprintf(s.text, sizeof s.text, "%s", tag);
        if ((size_t)n < sizeof s.text) vsnprintf(s.text + n, sizeof s.text - (size_t)n, fmt, ap2);
        for (size_t l = strlen(s.text); l && s.text[l - 1] == '\n';) s.text[--l] = 0;
        emit(&s, 1);
    }
    va_end(ap2);
}
