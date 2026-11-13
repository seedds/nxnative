// Exercise real diagnostic workers with settings supplied after process startup.
#include <assert.h>
#include <sys/wait.h>
#include <time.h>
#include "../memstat.m"

module_t *g_mod;
int g_nmod;
void svc_guest_range(uint8_t **base, size_t *size) { *base = NULL; *size = 0; }
int thr_name_of(pthread_t thread, char *name, size_t size) { (void)thread; (void)name; (void)size; return 0; }
uint64_t now_ns(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

static int contains(const char *path, const char *text) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0; fclose(f);
    return strstr(buf, text) != NULL;
}

static int child(void) {
    // Launch settings must not create workers (or a Metal device) before env.txt
    // and launcher settings have been applied. Let premature workers flush.
    for (int i = 0; i < 100; i++) {
        if (access("early-memory.tsv", F_OK) == 0 || access("early-cpu.tsv", F_OK) == 0) {
            fprintf(stderr, "diagnostic output appeared before explicit host startup\n");
            return 1;
        }
        usleep(10000);
    }
    unsetenv("DM_MEMSTAT"); unsetenv("DM_THRCPU");
    memstat_start(); thrcpu_start();
    setenv("DM_MEMSTAT", "", 1); setenv("DM_THRCPU", "", 1);
    memstat_start(); thrcpu_start();

    // This is when iOS supplies env.txt values. Test actual TSV samples, not
    // just a thread-creation stub; no guest modules or game inputs are needed.
    setenv("DM_MEMSTAT", "memory.tsv", 1);
    setenv("DM_THRCPU", "cpu.tsv,1", 1);
    memstat_start(); thrcpu_start();
    for (int i = 0; i < 500; i++) {
        if (contains("memory.tsv", "\n0\t") && contains("cpu.tsv", "\t-\ttotal\t")) {
            assert(contains("memory.tsv", "phys_footprint"));
            assert(contains("cpu.tsv", "thread ID\tthread name\tCPU%"));
            assert(access("early-memory.tsv", F_OK) != 0);
            assert(access("early-cpu.tsv", F_OK) != 0);
            return 0;
        }
        usleep(10000);
    }
    fprintf(stderr, "diagnostic samples missing after loading environment settings\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2) return child();
    char exe[4096]; assert(realpath(argv[0], exe));
    char dir[] = "/tmp/nxnative-memstat-XXXXXX"; assert(mkdtemp(dir));
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        assert(chdir(dir) == 0);
        setenv("DM_MEMSTAT", "early-memory.tsv", 1);
        setenv("DM_THRCPU", "early-cpu.tsv,1", 1);
        unsetenv("DM_MEMSTAT_DUMP");
        execl(exe, exe, "--child", (char *)NULL);
        _exit(127);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    const char *files[] = {"early-memory.tsv", "early-cpu.tsv", "memory.tsv", "cpu.tsv"};
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, files[i]); unlink(path);
    }
    assert(rmdir(dir) == 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    puts("memstat: deferred memory and CPU sampling passed");
    return 0;
}
