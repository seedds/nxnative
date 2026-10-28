// Stage-2 feasibility check: one binary, several guest memory layouts chosen at run time from DM_PROFILE.
// Each layout runs in its own process (mem_init is once per process, as in the host), so this test re-execs itself
// with LAYOUT set, boots the production dispatcher (hle_svc.c mem_init via GetInfo), and prints what it built.
// Build: clang -O1 -I.. -I../gen/<game> -o build/t_layout_runtime t_layout_runtime.c ../hle_svc.c ../hle_profile.c
#include "../dm.h"
#include "../hle_profile.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_guest_write_prepare(void *addr, size_t size) { (void)addr; (void)size; }

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c
static uint64_t info(int which) { uint64_t regs[31] = {0}; regs[1] = (uint64_t)which; svc_dispatch(0x29, regs, 0); return regs[1]; }   // GetInfo(out, id): id in x1, value out in x1

typedef struct { const char *name, *profile; uint64_t reserve, alias, stack, pool, used, sysres; int limit; } case_t;
static const case_t CASES[] = {
    // Tears of the Kingdom: 5 GB, 2.375 GB alias
    {"totk", "layout_totk.tsv", 0x140000000ull, 0x98000000ull, 0x10000000ull, 0xCD500000ull, 0x4000000ull, 0x2000000ull, 0},
    // Animal Crossing: 4 GB, 256 MB alias, no system resource
    {"acnh", "layout_acnh.tsv", 0x100000000ull, 0x10000000ull, 0x10000000ull, 0xCD500000ull, 0x7a00000ull, 0x0ull, 0},
    // Super Mario Galaxy: 1 GB total (the smallest profile in the repository)
    {"smg",  "layout_smg.tsv",  0x40000000ull, 0x10000000ull, 0x10000000ull, 0xCD500000ull, 0x4000000ull, 0ull, 1},
    // Aegis Rim: alias 2.875 GB with the map limit on
    {"13s",  "layout_13s.tsv",  0x140000000ull, 0x118000000ull, 0x10000000ull, 0x40000000ull, 0x4000000ull, 0x1000000ull, 1},
};
#define NCASES ((int)(sizeof CASES / sizeof CASES[0]))

static int child(const case_t *c) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", getenv("LAYOUT_DIR") ?: ".", c->profile);
    setenv("DM_PROFILE", path, 1);
    const struct dm_profile *p = dm_profile();
    fprintf(stderr, "[child] %s: profile %s reserve %#llx alias %#llx stack %#llx pool %#llx used %#llx sysres %#llx limit %d\n",
            c->name, p->source, (unsigned long long)p->mem_reserve, (unsigned long long)p->mem_alias,
            (unsigned long long)p->mem_stack, (unsigned long long)p->app_pool, (unsigned long long)p->mem_used,
            (unsigned long long)p->sys_resource, p->mem_map_limit);
    // what the host reports to the guest, built by the production dispatcher
    uint64_t aslr = info(12), aslr_size = info(13), heap_addr = info(4), heap_size = info(5);
    uint64_t alias_addr = info(2), alias_size = info(3), stack_addr = info(14), stack_size = info(15);
    uint64_t total = info(6), used = info(7), sysres = info(16);
    printf("%-6s reserve %#-12llx alias %#-12llx stack %#-12llx heap_region %#-12llx | got aslr %#llx+%#llx alias %#llx+%#llx stack %#llx+%#llx heap@%#llx total %#llx used %#llx sysres %#llx\n",
           c->name, (unsigned long long)aslr_size, (unsigned long long)alias_size, (unsigned long long)stack_size,
           (unsigned long long)heap_size, (unsigned long long)aslr, (unsigned long long)aslr_size,
           (unsigned long long)alias_addr, (unsigned long long)alias_size, (unsigned long long)stack_addr,
           (unsigned long long)stack_size, (unsigned long long)heap_addr, (unsigned long long)total,
           (unsigned long long)used, (unsigned long long)sysres);
    int ok = aslr_size == c->reserve && alias_size == c->alias && stack_size == c->stack && heap_size == c->reserve - 0x10000000ull - c->alias - c->stack
             && alias_addr == aslr + 0x10000000ull && stack_addr == alias_addr + c->alias
             && heap_addr == stack_addr + c->stack && total == c->pool && sysres == c->sysres;
    if (used != c->used) { printf("  [RED] used memory %#llx, profile says %#llx\n", (unsigned long long)used, (unsigned long long)c->used); ok = 0; }
    // SetHeapSize must succeed inside the runtime region
    { uint64_t regs[31] = {0}; regs[1] = heap_size / 2; svc_dispatch(0x01, regs, 0); if (regs[0]) { printf("  [RED] SetHeapSize(%#llx) failed %#llx\n", (unsigned long long)(heap_size / 2), (unsigned long long)regs[0]); ok = 0; } }
    // an explicit-profile mismatch must be visible, not silently ignored
    printf("  %s\n", ok ? "OK" : "[RED] MISMATCH");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1) {   // child mode: LAYOUT names the case
        for (int i = 0; i < NCASES; i++) if (!strcmp(argv[1], CASES[i].name)) return child(&CASES[i]);
        return 2;
    }
    // Write each case's profile where the child will find it (the profile loader takes a path in
    // DM_PROFILE and reads game.tsv's key/value form). Generated here rather than committed: the
    // values live in CASES above, and a fixture that can drift from them is a test that lies.
    for (int i = 0; i < NCASES; i++) {
        char path[256]; snprintf(path, sizeof path, "layout_%s.tsv", CASES[i].name);
        FILE *f = fopen(path, "w");
        if (!f) { printf("[RED] cannot write %s (errno %d)\n", path, errno); return 1; }
        fprintf(f, "# generated by t_layout_runtime: the case table above is the source of truth\n");
        fprintf(f, "name %s (runtime layout test)\n", CASES[i].name);
        fprintf(f, "mem_reserve %#llx\n", (unsigned long long)CASES[i].reserve);
        fprintf(f, "mem_alias %#llx\n", (unsigned long long)CASES[i].alias);
        fprintf(f, "mem_stack %#llx\n", (unsigned long long)CASES[i].stack);
        fprintf(f, "app_pool %#llx\n", (unsigned long long)CASES[i].pool);
        fprintf(f, "mem_used %#llx\n", (unsigned long long)CASES[i].used);
        fprintf(f, "sys_resource %#llx\n", (unsigned long long)CASES[i].sysres);
        fprintf(f, "mem_map_limit %d\n", CASES[i].limit);
        fclose(f);
    }
    int bad = 0;
    for (int i = 0; i < NCASES; i++) {
        pid_t pid = fork();
        if (!pid) { execl(argv[0], argv[0], CASES[i].name, (char *)NULL); _exit(3); }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("%-6s FAILED (%s %d)\n", CASES[i].name, WIFSIGNALED(st) ? "signal" : "status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st)); bad++; }
    }
    printf(bad ? "[RED] %d layout(s) failed\n" : "all %d runtime layouts OK\n", bad ? bad : NCASES);
    return bad ? 1 : 0;
}
