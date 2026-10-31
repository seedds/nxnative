// Exercise the real getenv takeover and its once-only initializer. Allocation
// tracking checks that unused backing storage is freed; accepted values must
// stay readable for the process lifetime (also checked under ASan).
#include "../hle.h"
#undef DM_GAME_ENV
#define DM_GAME_ENV "PROFILE=profile"
static char *env_strdup(const char *s);
static void env_free(void *p);
#define strdup env_strdup
#define free env_free
#include "../hle_os.c"
#undef free
#undef strdup

module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;
void crash_watch_frame(void) {}
void dm_guest_clock_advanced(void) {}
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
static char *sdk_getenv(const char *name) { return name && !strcmp(name, "SDK_ONLY") ? "sdk" : NULL; }
void *guest_sym(const char *name) { return !strcmp(name, "getenv") ? sdk_getenv : NULL; }

static char *live_duplicate;
static int duplicate_calls;
static char *env_strdup(const char *s) {
    duplicate_calls++;
    live_duplicate = strdup(s);
    return live_duplicate;
}
static void env_free(void *p) {
    if (p == live_duplicate) live_duplicate = NULL;
    free(p);
}

static int checks, fails;
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "PASS" : "FAIL", text); } while (0)
static int value_is(const char *name, const char *want) {
    const char *value = g_getenv(name);
    return value && !strcmp(value, want);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int valid = !strcmp(argv[1], "valid"), profile = !strcmp(argv[1], "profile");
    if (profile) unsetenv("DM_GAME_ENV");
    else if (valid) setenv("DM_GAME_ENV", "ignored ONE=first TWO=second=tail EMPTY= ignored-again", 1);
    else if (!strcmp(argv[1], "empty")) setenv("DM_GAME_ENV", "", 1);
    else if (!strcmp(argv[1], "invalid")) setenv("DM_GAME_ENV", "tokens without assignments", 1);
    else if (!strcmp(argv[1], "whitespace")) setenv("DM_GAME_ENV", "   ", 1);
    else return 2;

    CHECK(value_is("SDK_ONLY", "sdk"), "unknown names reach the SDK getenv");
    if (valid) {
        CHECK(value_is("ONE", "first"), "accepted assignment remains readable");
        CHECK(value_is("TWO", "second=tail"), "the value keeps additional equals signs");
        CHECK(value_is("EMPTY", ""), "an empty value is an accepted assignment");
        CHECK(g_getenv("ON") == NULL, "a prefix does not match an accepted name");
        CHECK(g_getenv("ignored") == NULL, "tokens without assignments stay absent");
        CHECK(live_duplicate != NULL, "accepted assignments retain their backing allocation");
    } else if (profile) {
        CHECK(value_is("PROFILE", "profile"), "unset DM_GAME_ENV uses the profile value");
        CHECK(live_duplicate != NULL, "the profile value retains its backing allocation");
    } else {
        CHECK(live_duplicate == NULL, "no accepted assignments leave no backing allocation");
        CHECK(g_getenv("PROFILE") == NULL, "an explicit empty or invalid override hides the profile");
        CHECK(g_getenv("tokens") == NULL, "invalid input creates no guest value");
    }
    int calls = duplicate_calls;
    setenv("DM_GAME_ENV", "ONE=changed PROFILE=changed SDK_ONLY=changed", 1);
    CHECK(value_is("SDK_ONLY", "sdk"), "later environment changes do not rerun initialization");
    CHECK(duplicate_calls == calls, "repeated lookups allocate no new backing storage");
    CHECK(valid ? value_is("ONE", "first") : g_getenv("ONE") == NULL, "a cached assignment keeps its original value or absence");
    if (profile) CHECK(value_is("PROFILE", "profile"), "the cached profile value stays readable");
    printf("== game env %s: %d checks, %d failed ==\n", argv[1], checks, fails);
    return fails != 0;
}
