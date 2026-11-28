// Dispatch valid synthetic gates across multiple NSO/NRO-sized batches, without game code.
#include "../hle.c"
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

struct dm_flags g_flag;
pthread_key_t g_key18;
uint64_t unimpl_stubs[MAX_STUB];
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
uint64_t guest_ticks(void) { return 123456789; }
void guest_clock_read_wait(void) {}
void guest_applet_wait(void) {}
static uint64_t entered, spun_ret, spun_orig;
static unsigned enters, leaves;
void thr_svc_enter(uint64_t *regs, uint64_t orig) { (void)regs; entered = orig; enters++; }
void thr_svc_leave(void) { leaves++; }
void dm_spin_wait(uint64_t ret, uint64_t orig) { spun_ret = ret; spun_orig = orig; }
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig) { regs[0] = orig + (unsigned)num; return regs[0]; }
static void observe(uint64_t *regs, int param) { regs[0] = (unsigned)param; }

static uint64_t address(unsigned n) { return 0x100000000ull + (uint64_t)(n / 6251) * 0x10000000 + (n % 6251) * 16 + 4; }
static const char *kinds[] = {"svc", "cntpct", "cntvct", "obs", "spin"};
static int argument(unsigned n) { return n % 5 == 3 ? (int)(n << 8) | 1 : (int)(n % 18); }

// Reject unknown and duplicate gates in a child process so the suite keeps running.
static void expect_abort(int mode, uint64_t addr) {
    fflush(NULL);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
        freopen("/dev/null", "w", stderr);
        uint64_t regs[30] = {0};
        if (mode == 0) gate_dispatch(addr, regs);
        else gate_register(addr, "svc", 1, addr - 4);
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

static void check(unsigned count) {
    for (unsigned n = 0; n < count; n++) {
        uint64_t regs[30] = {0}, ret = address(n), orig = ret - 4;
        unsigned before = enters;
        uint64_t result = gate_dispatch(ret, regs);
        switch (n % 5) {
            case 0: assert(result == orig + (unsigned)argument(n) && regs[0] == result); break;
            case 1: case 2: assert(result == guest_ticks() && regs[argument(n)] == result); break;
            case 3: assert(result == 0 && regs[0] == n); break;
            case 4: assert(result == 0 && spun_ret == ret && spun_orig == orig); break;
        }
        assert(enters == before + (n % 5 == 0 || n % 5 == 4));
        assert(enters == leaves);
        if (enters != before) assert(entered == orig);
    }
}
int main(void) {
    // Dispatch before any gate is registered must abort cleanly instead of dereferencing NULL.
    expect_abort(0, address(0));

    gate_obs_set(1, observe);
    const unsigned batches[] = {6251, 12502, 22502};
    unsigned start = 0;
    for (unsigned b = 0; b < sizeof batches / sizeof *batches; b++) {
        for (unsigned n = start; n < batches[b]; n++)
            gate_register(address(n), kinds[n % 5], argument(n), address(n) - 4);
        check(batches[b]);
        assert(gate_obs_present(1));
        printf("gates: %u registered and dispatched across module addresses\n", batches[b]);
        start = batches[b];
    }

    // Duplicate registration and unknown dispatch must still abort after rehashing.
    expect_abort(1, address(0));
    expect_abort(1, address(22501));
    expect_abort(0, address(22502));
    return 0;
}
