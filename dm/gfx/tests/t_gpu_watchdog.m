// The per-submission pipeline record (subrec_*) and the two reports that print it: the GPU hang watchdog (wd_check), when no fence completes for DM_NVN_GPU_WATCHDOG seconds, and
// a failed command buffer (subrec_failed), when macOS aborts a shader that ran too long and the fence completes with an error (issue #1326), so the watchdog never fires.
// Checks the record's contents (pairs once per command buffer, pass offsets, the overflow count), its text, and that a failure prints it once per fence in either handler order
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static NSError *error(NSString *text) { return [NSError errorWithDomain:@"t" code:1 userInfo:@{NSLocalizedDescriptionKey: text}]; }
static NSString *const HANG = @"Impacting Interactivity (00000002:kIOGPUCommandBufferCallbackErrorHang)";   // macOS aborting a command buffer that ran too long
static NSString *const VICTIM = @"Discarded (victim of GPU error/recovery) (00000005:kIOGPUCommandBufferCallbackErrorInnocentVictim)";
static int has(const char *s, const char *part) { return strstr(s, part) != NULL; }
// A record as subrec_fmt writes it, into buf
static const char *text(const subrec_t *r, uint32_t failed_cbs, char *buf, size_t size) { buf[0] = 0; subrec_fmt(buf, size, 0, r, failed_cbs); return buf; }
// Begins the record of a fence the way mtl_sub_begin and mtl_sub_commit bracket it, the commit leaving g_fence at that fence
static subrec_t *begin(uint64_t fence) { g_fence = fence - 1; subrec_begin(fence); return g_subrec_cur; }
static uint32_t commit(void) { uint32_t cb = g_subrec_cur->cb; g_fence++; g_subrec_cur = NULL; return cb; }
static int A, B, C, D;   // stand-ins for programs and compute shaders: only their addresses matter (subrec_add's who)
int main(void) { @autoreleasepool {
    char buf[SUBREC_LINE];
    // The record: a run of draws of one program costs one entry; a pair seen again in the same command buffer is not added twice; the next command buffer lists it again
    g_stat_pass = 1000; subrec_t *r = begin(10);
    subrec_add("aaaa", "bbbb", &A, 0); subrec_add("aaaa", "bbbb", &A, 0); subrec_add("cccc", "dddd", &B, 0);
    g_stat_pass += 2; subrec_add("aaaa", "bbbb", &A, 0); subrec_add("-", "eeee", &C, 1);
    CHECK(r->n == 3 && r->draws == 4 && r->dispatches == 1, "pairs once per command buffer, every draw and dispatch counted");
    CHECK(r->p[2].pass == 2 && r->p[0].pass == 0 && r->p[2].cb == 0, "each pair carries the pass it first ran in, from the submission's first");
    uint32_t cb0 = subrec_cut();
    g_stat_pass += 3; subrec_add("aaaa", "bbbb", &A, 0); subrec_add("ffff", "gggg", &D, 0);
    CHECK(cb0 == 0 && r->cb == 1 && r->n == 5 && r->p[3].cb == 1 && r->p[3].pass == 5, "a segment cut starts the next command buffer's list, a pair it repeats included");
    uint32_t last = commit();
    subrec_add("zzzz", "zzzz", &A, 0);
    CHECK(last == 1 && r->n == 5 && r->draws == 6, "draws after the commit leave the record alone");
    text(r, 1u << 1, buf, sizeof buf);
    CHECK(has(buf, " fence 10 (frame 0, 6 draws, 1 dispatches, 2 command buffers, from pass 1000):"), "the header names the fence, its counts and its first pass");
    CHECK(has(buf, " cb 0: pass +0 aaaa/bbbb cccc/dddd pass +2 -/eeee | cb 1 FAILED: pass +5 aaaa/bbbb ffff/gggg;"), "pipelines per command buffer in order, the failed one marked");

    // Overflow: past SUBREC_PAIRS only a count; the line stays within its buffer
    r = begin(11);
    static int who[SUBREC_PAIRS + 7];
    for (int i = 0; i < SUBREC_PAIRS + 7; i++) { char v[17], f[17]; snprintf(v, sizeof v, "%016x", i); snprintf(f, sizeof f, "%016x", ~i); subrec_add(v, f, &who[i], 0); }
    commit();
    CHECK(r->n == SUBREC_PAIRS && r->n_more == 7, "a full record counts the rest");
    text(r, 0, buf, sizeof buf);
    CHECK(has(buf, "(+7 more);") && strlen(buf) < sizeof buf - 1, "a full record fits one line and says how many more");
    char small[64]; small[0] = 0; int o = subrec_fmt(small, sizeof small, 0, r, 0);
    CHECK(o == (int)sizeof small - 1 && strlen(small) == sizeof small - 1, "a line too short for the record is cut at its end, not past it");

    // The error path: a failed command buffer prints the record once its submission is closed, once per fence
    r = begin(20); subrec_add("aaaa", "bbbb", &A, 0); uint32_t s0 = subrec_cut(); subrec_add("cccc", "dddd", &B, 0); last = commit();
    CHECK(!subrec_failed(20, s0, error(HANG), 0), "a failed segment waits for its submission's last command buffer");
    CHECK(subrec_failed(20, last, nil, 1), "the last command buffer's handler prints it, though it succeeded");
    CHECK(!subrec_failed(20, last, error(HANG), 1) && !subrec_failed(20, s0, error(HANG), 0), "once per fence");
    r = begin(30); subrec_add("aaaa", "bbbb", &A, 0); subrec_cut(); commit();
    text(r, 1u << 1, buf, sizeof buf);
    CHECK(has(buf, " cb 0: pass +0 aaaa/bbbb | cb 1 FAILED: none listed;"), "a failed command buffer with no draw of its own is named too");

    r = begin(21); subrec_add("aaaa", "bbbb", &A, 0); s0 = subrec_cut(); subrec_add("cccc", "dddd", &B, 0); last = commit();
    CHECK(!subrec_failed(21, last, nil, 1), "a submission whose command buffers all succeeded prints nothing");
    CHECK(subrec_failed(21, s0, error(HANG), 0), "a segment whose handler runs after the last one prints itself");

    r = begin(22); subrec_add("aaaa", "bbbb", &A, 0); last = commit();
    CHECK(!subrec_failed(22, last, error(VICTIM), 1), "a victim of another's fault ran nothing of its own");
    CHECK(!subrec_failed(23, 0, error(HANG), 1), "a fence with no record prints nothing");

    r = begin(24); subrec_add("aaaa", "bbbb", &A, 0); last = commit();
    begin(24 + SUBREC_RING); commit();
    CHECK(!subrec_failed(24, last, error(HANG), 1), "a record the ring has rewritten is not printed under the old fence");

    int printed = 0;
    for (int i = 0; i < SUBREC_REPORTS + 4; i++) { uint64_t f = 100 + (uint64_t)i; begin(f); subrec_add("aaaa", "bbbb", &A, 0); last = commit(); printed += subrec_failed(f, last, error(HANG), 1); }
    CHECK(printed == SUBREC_REPORTS - 2, "reports stop after SUBREC_REPORTS fences (two printed above)");

    // The watchdog: no fence completed for longer than DM_NVN_GPU_WATCHDOG seconds with work in flight names the oldest fence in flight, once
    setenv("DM_NVN_GPU_WATCHDOG", "1", 1);
    begin(200); subrec_add("aaaa", "bbbb", &A, 0); commit(); begin(201); commit();
    g_done = 199;
    uint64_t t0 = ws_t0() - 2000000000ull;
    CHECK(wd_check(201, WS_ALLOC, &t0, 199) == 199 && atomic_load(&g_wd_reported) == 200, "a stuck GPU is reported, naming the oldest fence in flight");
    atomic_store(&g_wd_reported, 0); g_done = 201; t0 = ws_t0() - 2000000000ull;
    wd_check(201, WS_ALLOC, &t0, 201);
    CHECK(atomic_load(&g_wd_reported) == 0, "nothing in flight: no report");
    printf("GPU watchdog and failed submission pipelines: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
