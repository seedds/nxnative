// Exercise the real forced wrappers with a synthetic SDK engine. No game or font assets.
#include "../hle.h"
static void *work_malloc(size_t size);
static void work_free(void *ptr);
#define malloc work_malloc
#define free work_free
#include "../hle_sdk16.c"
#undef malloc
#undef free
#include <mach-o/getsect.h>
#include <sys/resource.h>
#include <sys/wait.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
// Count actual live backing bytes, independently of the registry's representation.
static struct { void *ptr; size_t size; } allocations[512];
static size_t live_bytes, peak_bytes;
static int fail_after = -1;
static void *work_malloc(size_t size) {
    if (!fail_after) return NULL;
    if (fail_after > 0) fail_after--;
    void *p = malloc(size);
    if (p) {
        unsigned i = 0;
        while (i < 512 && allocations[i].ptr) i++;
        if (i == 512) abort();
        allocations[i].ptr = p; allocations[i].size = size;
        live_bytes += size;
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    }
    return p;
}
static void work_free(void *ptr) {
    if (!ptr) return;
    for (unsigned i = 0; i < 512; i++) if (allocations[i].ptr == ptr) {
        live_bytes -= allocations[i].size; allocations[i].ptr = NULL;
        free(ptr); return;
    }
    CHECK(0, "only host-owned allocations are freed");
}
static int live_host(void *ptr) {
    for (unsigned i = 0; i < 512; i++) if (allocations[i].ptr == ptr) return 1;
    return 0;
}
typedef struct { void *work; unsigned size; int init_result, fini_result, expects_host, finalizes; } engine_t;
static int sdk_init(void *self, void *buf, unsigned size) {
    engine_t *e = self;
    // The SDK stores its work pointer even on an initialization error.
    e->work = buf; e->size = size;
    if (e->expects_host) {
        CHECK(size >= 0x80000 && live_host(buf), "every small engine receives live enlarged memory");
        if (size >= 0x80000 && live_host(buf)) {
            ((unsigned char *)buf)[0] = 0x36;
            ((unsigned char *)buf)[0x7ffff] = 0x92;
        }
    }
    return e->init_result;
}
static int sdk_fini(void *self) {
    engine_t *e = self;
    e->finalizes++;
    if (e->expects_host) {
        CHECK(live_host(e->work), "backing memory remains live throughout SDK Finalize");
        if (live_host(e->work)) {
            CHECK(((unsigned char *)e->work)[0] == 0x36 && ((unsigned char *)e->work)[0x7ffff] == 0x92,
                  "Finalize can read both ends of its work memory");
        }
    }
    // Like the SDK, leave the work-pointer accessor's value intact after Finalize.
    return e->fini_result;
}
static void *sdk_get(void *self) { return ((engine_t *)self)->work; }
static const char *missing;
void *guest_sym(const char *name) {
    if (missing && !strcmp(missing, name)) return NULL;
    if (!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj")) return sdk_init;
    if (!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine8FinalizeEv")) return sdk_fini;
    if (!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv")) return sdk_get;
    CHECK(0, "only the lifecycle originals are resolved"); return NULL;
}
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_sdk_major(void) { return 20; }
int dm_game_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void hle_signal_system_event(void *ev) { (void)ev; }
uint64_t hle_system_event_binding(void *ev) { (void)ev; return 1; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 1; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
extern const struct mach_header_64 _mh_execute_header;
static void *forced(const char *name, void *original) {
    unsigned long bytes = 0; unsigned n = 0; void *fn = original;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) if (!strcmp(entries[i].name, name)) {
        fn = entries[i].fn; n++;
    }
    CHECK(n <= 1, "a lifecycle symbol is never forced twice");
    // An unhooked symbol goes to the SDK, as the loader does on main.
    return fn;
}
static int (*initialize)(void *, void *, unsigned);
static int (*finalize)(void *);
static void *(*get_work)(void *);
static void many_engines(void) {
    engine_t engines[80] = {0}; unsigned char game[80][64] = {{0}};
    for (unsigned i = 0; i < 80; i++) {
        engines[i].expects_host = 1;
        CHECK(initialize(&engines[i], game[i], sizeof game[i]) == 0, "Initialize preserves success status");
        CHECK(get_work(&engines[i]) == game[i], "game recovers its original pointer for every live engine");
        for (unsigned j = 0; j < i; j++) CHECK(engines[i].work != engines[j].work, "live engines never share backing memory");
    }
    CHECK(peak_bytes >= 80 * 0x80000, "more than sixteen simultaneous engines are protected");
    for (unsigned i = 80; i-- > 0;) {
        void *original = get_work(&engines[i]);
        CHECK(finalize(&engines[i]) == 0 && engines[i].finalizes == 1, "Finalize calls SDK once and returns its status");
        CHECK(original == game[i] && get_work(&engines[i]) == original, "original pointer remains recoverable after Finalize");
        CHECK(get_work(&engines[i]) == original, "post-Finalize getters do not consume pointer ownership");
    }
    CHECK(live_bytes < 80 * 128, "Finalize releases backing memory; only small pointer records may remain");
}
static void reinitialize(void) {
    engine_t e = {.expects_host = 1}; unsigned char a[64] = {0}, b[64] = {0};
    void *large = malloc(0x90000);
    initialize(&e, a, sizeof a);
    void *old_host = e.work;
    initialize(&e, b, sizeof b);
    CHECK(get_work(&e) == b, "small reinitialization replaces the original game pointer");
    CHECK(e.work == old_host || !live_host(old_host), "small reinitialization cannot leak replaced backing memory");
    e.expects_host = 0;
    initialize(&e, large, 0x90000);
    CHECK(e.work == large && e.size == 0x90000 && get_work(&e) == large, "large reinitialization removes stale pointer override");
    CHECK(!live_host(old_host), "large reinitialization releases the previous host buffer");
    CHECK(finalize(&e) == 0 && get_work(&e) == large, "large buffers stay game-owned through Finalize");
    e.expects_host = 1;
    for (unsigned i = 0; i < 40; i++) {
        initialize(&e, a, sizeof a);
        CHECK(get_work(&e) == a, "Finalize then reinitialize reactivates pointer mapping");
        CHECK(finalize(&e) == 0, "repeated lifetime completes");
        CHECK(!live_host(e.work), "every completed lifetime releases its enlarged buffer");
    }
    // A null game pointer must not be confused with an absent registry entry.
    initialize(&e, NULL, 0);
    CHECK(get_work(&e) == NULL, "null original pointer is preserved exactly");
    finalize(&e);
    CHECK(get_work(&e) == NULL, "null pointer remains null after Finalize");
    free(large);
}
static void failures(void) {
    engine_t e = {.expects_host = 1}; unsigned char game[64] = {0};
    const int results[] = {201, -1, 0x12345678};
    for (unsigned i = 0; i < sizeof results / sizeof *results; i++) {
        e.init_result = results[i];
        CHECK(initialize(&e, game, sizeof game) == results[i], "Initialize preserves SDK failure status bits");
        CHECK(get_work(&e) == game && live_host(e.work), "failed initialization retains memory until SDK cleanup");
        e.fini_result = 201;
        CHECK(finalize(&e) == 201 && live_host(e.work), "failed Finalize cannot release memory the SDK may still use");
        e.fini_result = 0;
        CHECK(finalize(&e) == 0 && !live_host(e.work), "successful cleanup of failed initialization releases backing memory");
        CHECK(get_work(&e) == game, "failure cleanup preserves original pointer ownership");
    }
}
static void failed_large_reinit(void) {
    engine_t e = {.expects_host = 1}; unsigned char game[64] = {0};
    void *large = malloc(0x90000);
    initialize(&e, game, sizeof game);
    void *old_host = e.work;
    e.expects_host = 0; e.init_result = 201;
    CHECK(initialize(&e, large, 0x90000) == 201, "large reinitialization preserves failure status");
    CHECK(get_work(&e) == large, "failed replacement still reports its new game pointer");
    CHECK(live_host(old_host), "failed replacement retains old backing until SDK cleanup");
    CHECK(finalize(&e) == 0 && !live_host(old_host), "Finalize releases backing retained by failed replacement");
    CHECK(get_work(&e) == large, "replacement failure cleanup preserves game ownership");
    free(large);
}
static void missing_originals(void) {
    const char *symbols[] = {
        "_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj",
        "_ZN2nn6fontll18ScalableFontEngine8FinalizeEv",
        "_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv",
    };
    for (unsigned i = 0; i < 3; i++) {
        pid_t child = fork(); CHECK(child >= 0, "missing-original child starts");
        if (!child) {
            struct rlimit r = {0, 0}; setrlimit(RLIMIT_CORE, &r);
            engine_t e = {0}; unsigned char game[64] = {0}; missing = symbols[i];
            if (i == 0) initialize(&e, game, sizeof game);
            if (i == 1) finalize(&e);
            if (i == 2) get_work(&e);
            _exit(1);
        }
        if (child > 0) { int status = 0; waitpid(child, &status, 0);
            CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "missing original fails explicitly"); }
    }
}
static int abort_pipe;
static void allocation_abort(int sig) {
    (void)sig;
    // Only write an already computed counter in this test's fatal-path signal handler.
    ssize_t n = write(abort_pipe, &live_bytes, sizeof live_bytes);
    _exit(n == sizeof live_bytes ? 0 : 1);
}
static void allocation_failures(void) {
    for (int i = 0; i < 2; i++) {
        int fds[2]; if (pipe(fds)) { CHECK(0, "allocation-failure pipe opens"); return; }
        pid_t child = fork(); CHECK(child >= 0, "allocation-failure child starts");
        if (!child) {
            close(fds[0]); abort_pipe = fds[1]; signal(SIGABRT, allocation_abort);
            engine_t e = {0}; unsigned char game[64] = {0}; fail_after = i;
            initialize(&e, game, sizeof game); _exit(1);
        }
        close(fds[1]);
        size_t retained = SIZE_MAX; ssize_t n = read(fds[0], &retained, sizeof retained); close(fds[0]);
        if (child > 0) { int status = 0; waitpid(child, &status, 0);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "allocation failure follows explicit fatal path"); }
        CHECK(n == sizeof retained && !retained, "failed allocation leaves no partial host record or backing");
    }
}
static void run_case(void (*fn)(void), const char *name) {
    pid_t child = fork(); CHECK(child >= 0, "test child starts");
    if (!child) { checks = fails = 0; fn(); printf("%s: %d checks, %d failed\n", name, checks, fails); fflush(stdout); _exit(fails != 0); }
    if (child > 0) { int s = 0; waitpid(child, &s, 0); CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0, name); }
}
int main(void) {
    initialize = forced("_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj", sdk_init);
    finalize = forced("_ZN2nn6fontll18ScalableFontEngine8FinalizeEv", sdk_fini);
    get_work = forced("_ZN2nn6fontll18ScalableFontEngine22GetPointerToWorkBufferEv", sdk_get);
    run_case(missing_originals, "missing originals");
    run_case(allocation_failures, "allocation failures");
    run_case(many_engines, "many engines");
    run_case(reinitialize, "reinitialize");
    run_case(failures, "SDK failure cleanup");
    run_case(failed_large_reinit, "failed large reinitialization");
    printf("font work: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
