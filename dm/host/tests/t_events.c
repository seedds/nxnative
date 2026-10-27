// Runtime events (dm_events.c): the stream (a path, fd:N), the JSON a line holds, a stream that fails, the typed hook, the rate limit, and that an unset DM_EVENTS does nothing.
// Each scenario runs in a child process of its own: the module is configured once per process, as the host does it.
#include "../dm_events.c"
#include <signal.h>
#include <sys/wait.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "t_events: %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static char *slurp(const char *path) {
    static char b[1 << 16]; FILE *f = fopen(path, "r"); size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0; if (f) fclose(f); b[n] = 0; return b;
}
static int count(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p++) n++; return n; }
static const char *dir;
static char *tmp(const char *name) { static char p[8][512]; static int i; snprintf(p[i % 8], sizeof p[0], "%s/%s", dir, name); return p[i++ % 8]; }
static void to_log(const char *name) { int fd = open(tmp(name), O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) _exit(2); dup2(fd, 2); close(fd); }

static void s_path(void) {   // every event kind, one line each, in order; the path is created
    char *p = tmp("path.jsonl"); unlink(p);
    CHECK(dm_events_configure(p) == 1);
    dm_event_frame_loop(); dm_event_frame_loop();   // the second does nothing
    dm_event_shaders("prewarm", "progress", 3, 10, 1.5);
    dm_event_end("terminated", "SIGTERM");
    dm_event_exit(-1, 11, "signal");
    dm_event_exit(7, 0, "exited");
    dm_events_flush(2000);
    char *s = slurp(p);
    CHECK(count(s, "\n") == 5);
    CHECK(count(s, "{\"ev\":\"frame_loop\",\"t\":") == 1 && strstr(s, "\"elapsed\":"));
    CHECK(strstr(s, "\"ev\":\"shaders\",\"t\":") && strstr(s, ",\"phase\":\"prewarm\",\"state\":\"progress\",\"done\":3,\"total\":10,\"elapsed\":1.500}\n"));
    CHECK(strstr(s, ",\"reason\":\"terminated\",\"text\":\"SIGTERM\"}\n"));
    CHECK(strstr(s, ",\"code\":null,\"signal\":11,\"reason\":\"signal\"}\n"));
    CHECK(strstr(s, ",\"code\":7,\"signal\":null,\"reason\":\"exited\"}\n"));
    CHECK(strstr(s, "\"ev\":\"frame_loop\"") < strstr(s, "\"ev\":\"shaders\"") && strstr(s, "\"ev\":\"shaders\"") < strstr(s, "\"ev\":\"end\""));   // in the order emitted
}
static void s_controller_thermal_save(void) {   // the three events of the front ends and the save commit: fields, escaping, and thermal reports only a change
    char *p = tmp("new.jsonl"); unlink(p); CHECK(dm_events_configure(p));
    dm_event_controller(1, 1, "Pro \"X\"", "Switch Pro Controller", "pro");
    dm_event_controller(0, 2, NULL, NULL, "handheld");
    dm_event_thermal(0); dm_event_thermal(0); dm_event_thermal(2); dm_event_thermal(2); dm_event_thermal(0); dm_event_thermal(7); dm_event_thermal(-1);
    dm_event_save("save_device", "Common\"Save", 3);
    dm_event_shaders("background", "progress", 4, 9, 0.5);
    dm_events_flush(2000);
    char *s = slurp(p);
    CHECK(count(s, "\n") == 7);   // 2 controller, 3 thermal (0, 2, 0), 1 save, 1 shaders
    CHECK(strstr(s, ",\"state\":\"connected\",\"player\":1,\"name\":\"Pro \\\"X\\\"\",\"kind\":\"Switch Pro Controller\",\"type\":\"pro\"}\n"));
    CHECK(strstr(s, ",\"state\":\"disconnected\",\"player\":2,\"name\":\"\",\"kind\":\"\",\"type\":\"handheld\"}\n"));
    CHECK(count(s, "\"ev\":\"thermal\"") == 3 && strstr(s, ",\"state\":\"nominal\",\"level\":0}\n") && strstr(s, ",\"state\":\"serious\",\"level\":2}\n"));
    CHECK(strstr(s, "\"ev\":\"save\",\"t\":") && strstr(s, ",\"save\":\"save_device\",\"mount\":\"Common\\\"Save\",\"commit\":3}\n"));
    CHECK(strstr(s, ",\"phase\":\"background\",\"state\":\"progress\",\"done\":4,\"total\":9"));
}
static void s_append(void) {   // a second process appends to the file the first one made
    char *p = tmp("path.jsonl"); CHECK(dm_events_configure(p)); dm_event_end("x", NULL); dm_events_flush(2000);
    CHECK(count(slurp(p), "\n") == 6);
}
static void s_red_escaping(void) {   // quotes, backslashes, newlines, control bytes and invalid UTF-8 in a [RED] text; the log line itself stays whole
    char *p = tmp("red.jsonl"); unlink(p); to_log("red.log");
    CHECK(dm_events_configure(p));
    dm_log_red("[vk] ", "[RED] say \"hi\"\nnext \\ \x01 \xff caf\xc3\xa9 %d%%", 5);
    dm_events_flush(2000);
    char *s = slurp(p);
    CHECK(count(s, "\n") == 1);   // one line though the text has a newline
    CHECK(strstr(s, "{\"ev\":\"red\",\"t\":"));
    CHECK(strstr(s, ",\"text\":\"[vk] [RED] say \\\"hi\\\"\\nnext \\\\ \\u0001 \\ufffd caf\xc3\xa9 5%\"}\n"));
    fflush(stderr); char *log = slurp(tmp("red.log"));
    CHECK(strstr(log, "[dm] [vk] [RED] say \"hi\"\nnext \\ \x01 \xff caf\xc3\xa9 5%\n"));
}
static void s_long_text(void) {   // a text longer than the event's room is cut in the event, never in the log
    char *p = tmp("long.jsonl"); unlink(p); to_log("long.log");
    char big[3000]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
    CHECK(dm_events_configure(p)); dm_log_red("", "[RED] %s", big); dm_events_flush(2000);
    CHECK(count(slurp(p), "\n") == 1);
    fflush(stderr); CHECK(strlen(slurp(tmp("long.log"))) > 3000);
}
static void s_fd(void) {
    int pr[2]; CHECK(!pipe(pr));
    char spec[32]; snprintf(spec, sizeof spec, "fd:%d", pr[1]);
    CHECK(dm_events_configure(spec) == 1);
    dm_event_shaders("prebuild", "start", 0, 5, 0); dm_event_frame_loop(); dm_events_flush(2000);
    char b[1024]; ssize_t n = read(pr[0], b, sizeof b - 1); b[n > 0 ? n : 0] = 0;
    CHECK(count(b, "\n") == 2 && strstr(b, "\"phase\":\"prebuild\",\"state\":\"start\""));
}
static void s_bad_specs(void) {   // nothing valid: nothing on, and each bad value says so once in the log
    to_log("bad.log");
    CHECK(dm_events_configure(NULL) == 0 && dm_events_configure("") == 0);   // unset: silent
    CHECK(dm_events_configure("fd:") == 0 && dm_events_configure("fd:abc") == 0 && dm_events_configure("fd:1") == 0 && dm_events_configure("fd:2") == 0 && dm_events_configure("fd:900") == 0);
    CHECK(dm_events_configure("/nonexistent-dir/x.jsonl") == 0);
    dm_event_frame_loop(); dm_events_flush(100);
    fflush(stderr); CHECK(count(slurp(tmp("bad.log")), "[RED] DM_EVENTS") == 6);
    CHECK(!atomic_load(&g_on) && !g_thread);   // no queue, no writer thread
}
static void s_closed_reader(void) {   // the reader went away: EPIPE, no SIGPIPE, one [RED] line, and later events are dropped quietly
    to_log("closed.log");
    int pr[2]; CHECK(!pipe(pr)); close(pr[0]);
    char spec[32]; snprintf(spec, sizeof spec, "fd:%d", pr[1]); CHECK(dm_events_configure(spec));
    for (int i = 0; i < 5; i++) { dm_event_end("x", NULL); dm_events_flush(2000); }
    fflush(stderr); char *log = slurp(tmp("closed.log"));
    CHECK(count(log, "[RED] DM_EVENTS: write failed") == 1);
}
static void s_full_pipe(void) {   // a reader that does not read and a pipe that is full (the write would block): off, once
    to_log("full.log");
    int pr[2]; CHECK(!pipe(pr)); fcntl(pr[1], F_SETFL, O_NONBLOCK);
    char junk[4096] = {0}; while (write(pr[1], junk, sizeof junk) > 0) {}
    char spec[32]; snprintf(spec, sizeof spec, "fd:%d", pr[1]); CHECK(dm_events_configure(spec));
    for (int i = 0; i < 3; i++) { dm_event_end("x", NULL); dm_events_flush(2000); }
    fflush(stderr); char *log = slurp(tmp("full.log"));
    CHECK(count(log, "[RED] DM_EVENTS: write failed") == 1);
}
static int g_hits; static pthread_t g_hook_thread; static dm_ev_kind g_kinds[8];
static void hook(const dm_event_t *e, void *u) { CHECK(u == &g_hits);
    if (e->kind == DM_EV_CONTROLLER) CHECK(e->player == 2 && !strcmp(e->state, "connected") && !strcmp(e->pad_name, "Pad") && !strcmp(e->pad_kind, "DualSense") && !strcmp(e->type, "pro"));
    if (e->kind == DM_EV_THERMAL) CHECK(e->level == 3 && !strcmp(e->state, "critical"));
    if (e->kind == DM_EV_SAVE) CHECK(!strcmp(e->save, "save") && !strcmp(e->mount, "save") && e->commit == 1); g_kinds[g_hits++ & 7] = e->kind; g_hook_thread = pthread_self(); if (e->kind == DM_EV_RED) CHECK(!strcmp(e->text, "[RED] typed") && !strcmp(e->name, "red")); }
static void s_hook(void) {   // the typed hook runs without a stream, in order, off the caller's thread
    to_log("hook.log"); CHECK(!dm_events_configure(NULL));
    dm_events_set_hook(hook, &g_hits);
    dm_event_frame_loop(); dm_log_red("", "[RED] typed"); dm_event_end("terminated", "x");
    dm_event_controller(1, 2, "Pad", "DualSense", "pro"); dm_event_thermal(3); dm_event_save("save", "save", 1); dm_events_flush(2000);
    CHECK(g_hits == 6 && g_kinds[0] == DM_EV_FRAME_LOOP && g_kinds[1] == DM_EV_RED && g_kinds[2] == DM_EV_END && g_kinds[3] == DM_EV_CONTROLLER && g_kinds[4] == DM_EV_THERMAL && g_kinds[5] == DM_EV_SAVE);
    CHECK(!pthread_equal(g_hook_thread, pthread_self()));
}
static void s_rate_limit(void) {   // a flood of [RED] lines: at most RED_PER_SEC a second become events (two seconds at most here), the rest are counted into the next line
    char *p = tmp("rate.jsonl"); unlink(p); to_log("rate.log");
    CHECK(dm_events_configure(p));
    for (int i = 0; i < 1000; i++) dm_log_red("", "[RED] line %d", i);
    dm_events_flush(2000);
    int reds = count(slurp(p), "\"ev\":\"red\"");
    CHECK(reds >= 1 && reds <= 2 * RED_PER_SEC);
    dm_event_end("x", NULL); dm_events_flush(2000);
    CHECK(strstr(slurp(p), "\"reason\":\"x\",\"dropped\":"));
}
static void s_overflow_does_not_block(void) {   // a reader that never reads (a full pipe): the caller is never held up, whatever the queue holds
    int pr[2]; CHECK(!pipe(pr)); fcntl(pr[1], F_SETFL, O_NONBLOCK);
    char spec[32]; snprintf(spec, sizeof spec, "fd:%d", pr[1]); CHECK(dm_events_configure(spec)); to_log("block.log");
    double t0 = mono();
    for (int i = 0; i < 100000; i++) dm_event_shaders("prewarm", "progress", i, 100000, 0);
    CHECK(mono() - t0 < 2.0);
}

static void run(const char *name, void (*fn)(void)) {
    pid_t pid = fork();
    if (!pid) { g_fail = 0; fn(); fflush(stderr); _exit(g_fail); }
    int st; waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "t_events: scenario %s failed (status %#x)\n", name, st); g_fail = 1; }
}
int main(void) {
    const char *base = getenv("TMPDIR"); char t[512]; snprintf(t, sizeof t, "%s/t_events.XXXXXX", base && *base ? base : "/tmp");
    dir = mkdtemp(t); if (!dir) { perror("mkdtemp"); return 1; }
    run("path", s_path); run("new_events", s_controller_thermal_save); run("append", s_append); run("red_escaping", s_red_escaping); run("long_text", s_long_text); run("fd", s_fd); run("bad_specs", s_bad_specs);
    run("closed_reader", s_closed_reader); run("full_pipe", s_full_pipe); run("hook", s_hook); run("rate_limit", s_rate_limit); run("overflow", s_overflow_does_not_block);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {}   // NOLINT(bugprone-command-processor): a fixed rm of the folder mkdtemp made above, no outside input
    puts(g_fail ? "t_events: FAILED" : "t_events: all passed");
    return g_fail;
}
