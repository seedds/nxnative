// The one rule of the environment (dm/host/plat/plat.h): an empty value is unset, on every platform. Windows' C runtime cannot even hold one (setenv(name, "", 1) is
// _putenv_s, which removes the variable), so the platform layer removes empty variables before main, in every program that links it, and plat_env_scrub removes the
// ones set later. Checked in this process (a variable set to nothing, then scrubbed; a variable with a value kept) and in a child started with an empty variable in its
// environment (unset there before its main). The switches' readers then never see "set but empty" (docs/env_switches.tsv; docs/settings.md, "Settings in a file").
#include "plat/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

static int checks, fails;
static void check(int ok, const char *what) { checks++; fails += !ok; printf("%-5s %s\n", ok ? "ok" : "FAIL", what); }

static void set_empty(const char *name) {   // a variable set to nothing, as a parent or a shell can make one
#if defined(_WIN32)
    SetEnvironmentVariableA(name, "");   // the process's block holds an empty value; the C runtime's setenv cannot make one
#else
    setenv(name, "", 1);
#endif
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--child")) {   // started with T_ENV_EMPTY set to nothing and T_ENV_SET=1
        const char *e = getenv("T_ENV_EMPTY"), *s = getenv("T_ENV_SET");
        printf("child: T_ENV_EMPTY %s, T_ENV_SET %s\n", e ? (*e ? "set" : "empty") : "unset", s ? s : "unset");
        return 0;
    }
    // 1. In this process: a variable set to nothing after the start is removed by plat_env_scrub; one with a value stays
    set_empty("T_ENV_EMPTY");
    setenv("T_ENV_SET", "1", 1);
    plat_env_scrub();
    check(!getenv("T_ENV_EMPTY"), "a variable set to nothing after the start: unset after plat_env_scrub");
#if defined(_WIN32)
    char buf[8];
    check(GetEnvironmentVariableA("T_ENV_EMPTY", buf, sizeof buf) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND, "... and gone from the process's block too");
#endif
    check(getenv("T_ENV_SET") && !strcmp(getenv("T_ENV_SET"), "1"), "a variable with a value stays");
    // 2. A child started with a variable set to nothing in its environment: unset there before its main (the platform layer's constructor)
    set_empty("T_ENV_EMPTY");
    char cmd[1200], line[256] = "";
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --child\"", argv[0]);
#else
    snprintf(cmd, sizeof cmd, "'%s' --child", argv[0]);
#endif
    FILE *f = popen(cmd, "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; pclose(f); }
    printf("      %s%s", line, strchr(line, '\n') ? "" : "\n");
    check(strstr(line, "child: T_ENV_EMPTY unset, T_ENV_SET 1") != NULL, "a child started with a variable set to nothing: unset before its main");
    printf("t_env_rule: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
