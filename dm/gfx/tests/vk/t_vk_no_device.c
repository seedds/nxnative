// No Vulkan device for the game (vk_backend.c, hvk_require_device; aot.h DM_AOT_NO_VULKAN): the game program's path, which nxhost-run asks for with a window and no
// DM_VK_HEADLESS (main_x86.c), stops at the device's bring-up with one [RED] line and exit status 9 instead of running with nothing drawn; without that request (the
// tests, a headless or diagnostic run) the null backend's forms answer, as before. Each case runs in a child of this program with DM_VK_DEVICE=999, an index no machine's
// loader lists, so the result is the same with or without a Vulkan device here (no loader at all gives the same two answers).
#include "t_vk_harness.h"   // the stand-ins of the host functions the frontend and the backend call (no device is made through it here)
#include "vk_present.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

int hvk_up(void);   // vk_backend.c: the device comes up (1) or the null forms answer (0)

static int run(const char *self, const char *mode, char *out, size_t n) {   // the child's exit status, its output (both streams) in out
    char cmd[1200];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --child %s 2>&1\"", self, mode);
#else
    snprintf(cmd, sizeof cmd, "'%s' --child %s 2>&1", self, mode);
#endif
    FILE *f = popen(cmd, "r"); size_t got = 0;
    if (!f) return -1;
    while (got + 1 < n) { size_t r = fread(out + got, 1, n - 1 - got, f); if (!r) break; got += r; }
    out[got] = 0;
    int st = pclose(f);
#if defined(_WIN32)
    return st;
#else
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
#endif
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--child")) {
        setenv("DM_VK_DEVICE", "999", 1);
        if (!strcmp(argv[2], "require")) hvk_require_device(9);
        printf("child: up %d\n", hvk_up());
        fflush(stdout);
        return 0;
    }
    static char out[65536];
    int fails = 0, st = run(argv[0], "require", out, sizeof out);
    int ok = st == 9 && strstr(out, "[RED] [hostvk] no Vulkan device for the game") && strstr(out, "exits with status 9") && !strstr(out, "child: up");
    fails += !ok;
    printf("%-5s the game program's path without a device: one [RED] line and exit status 9 (status %d)\n", ok ? "ok" : "FAIL", st);
    st = run(argv[0], "plain", out, sizeof out);
    ok = st == 0 && strstr(out, "child: up 0") && strstr(out, "the null backend's forms answer") && !strstr(out, "exits with status");
    fails += !ok;
    printf("%-5s without the request (a test, a headless run): the null forms answer and the program goes on (status %d)\n", ok ? "ok" : "FAIL", st);
    printf("== t_vk_no_device: 2 checks, %d failed ==\n", fails);
    return fails != 0;
}
