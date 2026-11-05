// Native fault injection at the real jit.tsv loader's four realloc sites.
#include "../hle.h"
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>

static void *test_realloc(void *p, size_t size);
static void test_die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#undef die
#define die test_die
#define realloc test_realloc
#include "../hle_jit.c"
#undef realloc

module_t *g_mod;
int g_nmod;
static int calls, fail_at;
static jit_req *saved_req;
static int saved_nreq;
static jit_req saved_rows[2];

static void *test_realloc(void *p, size_t size) {
    if (++calls == fail_at) {
        assert(g_nreq == (fail_at - 1) / 7);
        assert((p == g_req) == ((fail_at - 1) % 7 == 0));
        saved_req = g_req;
        saved_nreq = g_nreq;
        // Also snapshot the in-progress row (not yet counted as a complete request).
        int n = g_nreq + ((fail_at - 1) % 7 != 0);
        assert(n >= 0 && n <= 2);
        assert(n == 0 || g_req);
        for (int i = 0; i < n; i++) saved_rows[i] = g_req[i];
        return NULL;
    }
    return realloc(p, size);
}
static void check_rows(int n) {
    assert(n >= 0 && n <= 2);
    assert(n == 0 || g_req);
    for (int i = 0; i < n; i++) {
        const jit_req *r = &g_req[i];
        assert(r->cmd == (uint64_t)(i + 1));
        assert(r->nrel == 0 || (r->rel_at && r->rel_off && r->rel_mod));
        for (uint32_t j = 0; j < r->nrel; j++) {
            assert(r->rel_at[j] == 8 * j);
            assert(r->rel_off[j] == 0x10 + j);
            assert(r->rel_mod[j] == -1);
        }
    }
}
static void test_die(const char *fmt, ...) {
    char msg[1400]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    assert(fail_at && calls == fail_at);
    assert(strstr(msg, "jit.tsv") && strstr(msg, "allocat"));
    assert(g_req == saved_req && g_nreq == saved_nreq);
    assert(g_nreq == (fail_at - 1) / 7);   // only fully loaded rows are published
    int n = g_nreq + ((fail_at - 1) % 7 != 0);
    assert(n >= 0 && n <= 2);
    assert(n == 0 || g_req);
    for (int i = 0; i < n; i++) {
        assert(g_req[i].nrel == saved_rows[i].nrel);
        if (i == g_nreq) assert(g_req[i].nrel == (uint32_t)(((fail_at - 1) % 7 - 1) / 3));
        assert(g_req[i].rel_at == saved_rows[i].rel_at);
        assert(g_req[i].rel_off == saved_rows[i].rel_off);
        assert(g_req[i].rel_mod == saved_rows[i].rel_mod);
    }
    check_rows(n);
    _exit(0);
}
int main(void) {
    char dir[] = "/private/tmp/dm-jit-test-XXXXXX", path[256];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/jit.tsv", dir);
    FILE *f = fopen(path, "w"); assert(f);
    for (int i = 1; i <= 2; i++)
        fprintf(f, "req\tkey%d\t%d\t0\t20\t0\t0\t0\t0\t0\t0=jit_rx+10,8=jit_rx+11\n", i, i);
    assert(!fclose(f));
    snprintf(path, sizeof path, "%s/jit.out", dir);
    f = fopen(path, "w"); assert(f); assert(!fclose(f));
    // Each row grows the request list once and each relocation array twice.
    for (int failure = 1; failure <= 14; failure++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { fail_at = failure; jit_serve_load(dir); _exit(1); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    jit_serve_load(dir);
    assert(calls == 14 && g_nreq == 2 && !g_out_len);
    check_rows(2);
    assert(jit_find("key1") == &g_req[0] && jit_find("key2") == &g_req[1]);
    for (int i = 0; i < g_nreq; i++) { assert(g_req[i].nrel == 2); free(g_req[i].rel_at); free(g_req[i].rel_off); free(g_req[i].rel_mod); }
    free(g_req);
    assert(!unlink(path));
    snprintf(path, sizeof path, "%s/jit.tsv", dir); assert(!unlink(path)); assert(!rmdir(dir));
    puts("jit: 14 allocation failures and successful multi-row load passed");
}
