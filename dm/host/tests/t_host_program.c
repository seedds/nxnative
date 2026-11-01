// The host program, nxhost-run (dm/host/main_x86.c), run as a child process as a player's launcher or the NSP script would run it, on the synthetic game module of t_aot_module
// (dm/tools/tests/aot_synth.py, packed by dm/tools/aot/tests/build_packs.sh into <packs>/module; no game files): the module has no SDK, so it runs from its own entry
// (DM_AOT_RAW=1). SDL's dummy video and audio drivers (no display, no sound card) and the headless Vulkan presenter (DM_VK_HEADLESS=1). Runs:
//   1. DM_WINDOW=1: the front end opens (SDL's dummy driver gives a window without Vulkan) and the guest runs on its own thread; the program loads the module through the loader's
//      game-module path, runs it through the engine and exits with the module's status: 0 (ExitProcess). Its log: the start line, the module mapped and its pack loaded, the entry,
//      the module's two imports answered by named stubs (the program has no t_aot_mod_combine: t_aot_module's test entries are not in it), the module's OutputDebugString
//      (DM_DBG=1), the end line with status 0;
//   2. no DM_WINDOW: the same on the main thread;
//   3. without DM_AOT_RAW: the SDK start refuses a module set without nnMain with a [RED] line naming the switch, and a nonzero status, before any guest code;
//   4. no arguments: the usage and status 2.
// The program is the one built beside this test (its folder: argv[0]'s); the packs come from NXAOT_PACKS (the environment, else the CMake option); without them runs 1 to 3 are
// skipped with a note (the Windows runner, a tree built without the packs)
//   t_host_program
#include "plat/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#define EXE ".exe"
#else
#include <sys/wait.h>
#define EXE ""
#endif

#ifndef T_AOT_PACKS
#define T_AOT_PACKS ""
#endif

static int checks, fails;
static void verdict(int ok, const char *what) { checks++; fails += !ok; printf("%-5s %s\n", ok ? "ok" : "FAIL", what); }

typedef struct { int status; char *out; } result;
static void run(const char *cmd, result *r) {
    memset(r, 0, sizeof *r);
    FILE *f = popen(cmd, "r");
    if (!f) { r->status = -1; r->out = calloc(1, 1); return; }
    size_t cap = 0, n = 0; char line[8192];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        if (n + len + 1 > cap) { cap = (n + len + 1) * 2; r->out = realloc(r->out, cap); if (!r->out) abort(); }
        memcpy(r->out + n, line, len + 1); n += len;
    }
    int st = pclose(f);
#if defined(_WIN32)
    r->status = st;
#else
    r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
    if (!r->out) r->out = calloc(1, 1);
}
static const char *line_with(const char *out, const char *needle) {   // the whole log line holding needle, for the report
    static char buf[600];
    const char *p = strstr(out, needle);
    if (!p) return "(no such line)";
    const char *b = p; while (b > out && b[-1] != '\n') b--;
    snprintf(buf, sizeof buf, "%.*s", (int)strcspn(b, "\n"), b);
    return buf;
}
static void env(const char *k, const char *v) {
#if defined(_WIN32)
    char b[512]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}
static void quote(char *out, size_t n, const char *prog, const char *args) {
#if defined(_WIN32)
    snprintf(out, n, "\"\"%s\" %s 2>&1\"", prog, args);
#else
    snprintf(out, n, "'%s' %s 2>&1", prog, args);
#endif
}

int main(int argc, char **argv) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    char prog[1100], dir[1024], cmd[4096];
    snprintf(dir, sizeof dir, "%s", argv[0]);
    char *slash = strrchr(dir, '/');
#if defined(_WIN32)
    char *bs = strrchr(dir, '\\'); if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    snprintf(prog, sizeof prog, "%s/nxhost-run" EXE, dir);
    FILE *pf = fopen(prog, "rb");
    if (!pf) { printf("FAIL: no host program at %s (the CMake target nxhost-run, NXHOST_PROGRAM)\n", prog); return 1; }
    fclose(pf);
    printf("t_host_program: %s\n", prog);
    // The environment every run shares: no display, no sound card, no window for Vulkan, no watchdog left from the caller
    env("SDL_VIDEO_DRIVER", "dummy"); env("SDL_AUDIO_DRIVER", "dummy"); env("DM_VK_HEADLESS", "1"); env("DM_DBG", "1");
    result r;
    // 4. The usage
    env("DM_WINDOW", NULL); env("DM_AOT_RAW", NULL);
    quote(cmd, sizeof cmd, prog, "");
    run(cmd, &r);
    printf("      %s\n", line_with(r.out, "usage:"));
    verdict(r.status == 2 && strstr(r.out, "usage: nxhost-run <dir>"), "no arguments: the usage, status 2");
    free(r.out);
    const char *packs = getenv("NXAOT_PACKS");
    if (!packs || !*packs) packs = T_AOT_PACKS;
    if (!*packs) {
        printf("note: no packs (NXAOT_PACKS unset; dm/tools/aot/tests/build_packs.sh makes the synthetic module's in the nxnative-aot image): the program's runs of a module are skipped\n");
        printf("t_host_program: %d checks, %d failed\n", checks, fails);
        return fails != 0;
    }
    char module[1100], probe[1200], args[1300];
    snprintf(module, sizeof module, "%s/module", packs);
    snprintf(probe, sizeof probe, "%s/main.aot.tsv", module);
    pf = fopen(probe, "rb");
    if (!pf) { printf("FAIL: no %s (dm/tools/aot/tests/build_packs.sh writes it)\n", probe); return 1; }
    fclose(pf);
#if defined(_WIN32)
    snprintf(args, sizeof args, "\"%s\" main", module);
#else
    snprintf(args, sizeof args, "'%s' main", module);
#endif
    quote(cmd, sizeof cmd, prog, args);
    // 1. With the window path, 2. without
    for (int windowed = 1; windowed >= 0; windowed--) {
        env("DM_WINDOW", windowed ? "1" : NULL); env("DM_AOT_RAW", "1");
        run(cmd, &r);
        fputs(r.out, stdout);
        char what[300];
        snprintf(what, sizeof what, "%s: the synthetic module ran to ExitProcess and the program exited with its status 0 (status %d)", windowed ? "DM_WINDOW=1" : "no window", r.status);
        verdict(r.status == 0, what);
        verdict(strstr(r.out, "nxhost-run: modules from") != NULL, "the start line (nxhost-run: modules from ..., the engine without or with the JIT fallback)");
        verdict(strstr(r.out, "main: base") && strstr(r.out, "x86-64 engine)") && strstr(r.out, "aot: 1 modules from"), "the module mapped by the loader's game-module path and its pack loaded");
        verdict(strstr(r.out, ">>> entry main+") && strstr(r.out, "DM_AOT_RAW=1"), "the module's own entry (DM_AOT_RAW=1)");
        verdict(strstr(r.out, "UNIMPL t_aot_mod_combine") && strstr(r.out, "UNIMPL t_aot_mod_missing"), "the module's two imports answered by named stubs (UNIMPL lines)");
        verdict(strstr(r.out, "[guest] aot module: done") != NULL, "the module's OutputDebugString line ([guest], DM_DBG=1)");
        verdict(strstr(r.out, "guest code ended: status 0 (the process ended)") != NULL, "the end line: status 0, the process ended");
        if (windowed) verdict(strstr(r.out, "video driver dummy") && strstr(r.out, "no Vulkan window"), "the SDL3 front end opened on the dummy driver (a window without Vulkan)");
        printf("      %s\n", line_with(r.out, "guest code ended"));
        free(r.out);
    }
    // 3. The SDK start without DM_AOT_RAW: refused before any guest code
    env("DM_WINDOW", NULL); env("DM_AOT_RAW", NULL);
    run(cmd, &r);
    printf("      %s\n", line_with(r.out, "missing symbol nnMain"));
    verdict(r.status != 0 && strstr(r.out, "[RED] missing symbol nnMain") && strstr(r.out, "DM_AOT_RAW=1") && !strstr(r.out, "[guest]"),
            "without DM_AOT_RAW: the SDK start refuses a module set without nnMain ([RED] line naming the switch), no guest code ran, nonzero status");
    free(r.out);
    printf("t_host_program: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
