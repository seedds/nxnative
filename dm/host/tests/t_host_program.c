// The host program, nxhost-run (dm/host/main_x86.c), run as a child process as a player's launcher or the NSP script would run it, on the synthetic game module of t_aot_module
// (dm/tools/tests/aot_synth.py, packed by dm/tools/aot/tests/build_packs.sh into <packs>/module; no game files): the module has no SDK, so it runs from its own entry
// (DM_AOT_RAW=1). SDL's dummy video and audio drivers (no display, no sound card) and the headless Vulkan presenter (DM_VK_HEADLESS=1). Runs:
//   1. DM_WINDOW=1: the front end opens (SDL's dummy driver gives a window without Vulkan) and the guest runs on its own thread; the program loads the module through the loader's
//      game-module path, runs it through the engine and exits with the module's status: 0 (ExitProcess). Its log: the start line, the module mapped and its pack loaded, the entry,
//      the module's two imports answered by named stubs (the program has no t_aot_mod_combine: t_aot_module's test entries are not in it), the module's OutputDebugString
//      (DM_DBG=1), the end line with status 0;
//   2. no DM_WINDOW: the same on the main thread;
//   3. without DM_AOT_RAW: the SDK start refuses a module set without nnMain with a [RED] line naming the switch, and a nonzero status, before any guest code;
//   4. no arguments: the usage and status 2;
//   5. a copy of the module's folder whose image has one byte changed (the manifest's SHA-256 with it): its pack is another image's, so the program refuses it before any guest
//      code with the reason's [RED] line, the repack line and status 8 (aot.h DM_AOT_REPACK), which the launcher answers with the repack.
// The program is the one built beside this test (its folder: argv[0]'s); the packs come from NXAOT_PACKS (the environment, else the CMake option); without them runs 1 to 3 are
// skipped with a note (the Windows runner, a tree built without the packs)
//   t_host_program
#if defined(_WIN32)
#include <direct.h>   // _mkdir; before plat.h, whose mkdir(path, mode) macro would break direct.h's declaration of mkdir
#else
#include <sys/stat.h>
#endif
#include "plat/plat.h"
#include <stdint.h>
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
    // 5. A pack this host refuses, which a repack cures (aot.h DM_AOT_REPACK): the module's folder copied beside this test with one byte of its image changed and the manifest's
    //    SHA-256 with it, so that its pack is another image's: the reason's [RED] line, the repack line and status 8, before any guest code
    {
        char rdir[1200], src[1300], dst[1300];
        snprintf(rdir, sizeof rdir, "%s/t_host_program.refused", dir);
#if defined(_WIN32)
        _mkdir(rdir);
        const char *pack_name = "main.aot.dll";
#else
        mkdir(rdir, 0755);
        const char *pack_name = "main.aot.so";
#endif
        const char *files[] = {"main.image.bin", "main.aot.tsv", pack_name};
        int copied = 1;
        for (int k = 0; k < 3 && copied; k++) {
            snprintf(src, sizeof src, "%s/%s", module, files[k]); snprintf(dst, sizeof dst, "%s/%s", rdir, files[k]);
            FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
            char buf[65536]; size_t got;
            while (out && (got = fread(buf, 1, sizeof buf, in)) > 0) copied = copied && fwrite(buf, 1, got, out) == got;
            copied = copied && in && out;
            if (in) fclose(in);
            if (out) fclose(out);
        }
        // the image's last byte changed, and the manifest's first line given that image's SHA-256 (its last field)
        snprintf(dst, sizeof dst, "%s/main.image.bin", rdir);
        FILE *img = fopen(dst, "rb+");
        long len = 0; uint8_t *bytes = NULL;
        if (img && !fseek(img, 0, SEEK_END) && (len = ftell(img)) > 0 && (bytes = malloc((size_t)len)) && !fseek(img, 0, SEEK_SET) && fread(bytes, 1, (size_t)len, img) == (size_t)len) {
            bytes[len - 1] ^= 0x5a;
            fseek(img, 0, SEEK_SET); copied = copied && fwrite(bytes, 1, (size_t)len, img) == (size_t)len;
        } else copied = 0;
        if (img) fclose(img);
        uint8_t sha[32]; char hex[65];
        if (bytes) plat_sha256(bytes, (size_t)len, sha);
        free(bytes);
        for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", sha[k]);
        snprintf(dst, sizeof dst, "%s/main.aot.tsv", rdir);
        FILE *mf = fopen(dst, "rb"); char text[65536]; size_t tn = mf ? fread(text, 1, sizeof text - 1, mf) : 0;
        if (mf) fclose(mf);
        text[tn] = 0;
        char *eol = strchr(text, '\n'), *tab = NULL;
        for (char *p = text; eol && p < eol; p++) if (*p == '\t') tab = p;
        if (tab && eol - tab == 65) memcpy(tab + 1, hex, 64); else copied = 0;
        mf = fopen(dst, "wb");
        copied = copied && mf && fwrite(text, 1, tn, mf) == tn;
        if (mf) fclose(mf);
#if defined(_WIN32)
        snprintf(args, sizeof args, "\"%s\" main", rdir);
#else
        snprintf(args, sizeof args, "'%s' main", rdir);
#endif
        quote(cmd, sizeof cmd, prog, args);
        env("DM_WINDOW", NULL); env("DM_AOT_RAW", "1");
        run(cmd, &r);
        printf("      %s\n", line_with(r.out, "compiled from another image"));
        printf("      %s\n", line_with(r.out, "DM_AOT_REPACK"));
        verdict(copied && r.status == 8 && strstr(r.out, "compiled from another image") && strstr(r.out, "[RED] aot: this host refuses the game's pack") && !strstr(r.out, "[guest]"),
                "a pack of another image: the reason's [RED] line, the repack line and status 8 (DM_AOT_REPACK), no guest code ran");
        free(r.out);
    }
    printf("t_host_program: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
