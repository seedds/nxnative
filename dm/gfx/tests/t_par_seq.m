// Parallel translation's sequencer (nvn_rec.h, stage P2): jobs translated in any order reach the encode thread in the order the planner numbered them, a job's blocks in the order
// they were recorded, and a drain waits for every numbered job. Each recorded call here is a block the encode thread runs (RO_Q_BLOCK) that appends its tag to a log
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); count++; printf("%s expected %llu → got %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w == g ? "✓" : "✗"); fail += w != g; } while (0)
static int g_log[4096], g_nlog;   // written by the encode thread only, read after a drain
static void put_tag(int tag) { rec_t *r = rec_put(RO_Q_BLOCK, 1); r->a[0] = rec_obj([^{ g_log[g_nlog++] = tag; } copy]); }
static void job(uint64_t id, int first, int n) { rec_job_begin(); for (int i = 0; i < n; i++) put_tag(first + i); rec_job_end(id); }
static int in_order(int n) { for (int i = 0; i < n; i++) if (g_log[i] != i) return 0; return g_nlog == n; }
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0); setenv("DM_NVN_ENC_THREAD", "1", 1); setenv("DM_NVN_PAR", "1", 1); mtl_init();
    EXPECT("deferred encoding is on", 1, rec_on());
    // Two jobs finished in reverse order: 0..9 then 10..19
    g_nlog = 0; uint64_t a = jseq_issue(), b = jseq_issue();
    job(b, 10, 10); jseq_push(0);
    EXPECT("the later job is held while the earlier one runs", 0, g_jseq_pushed);
    job(a, 0, 10);
    rec_drain(); EXPECT("a later job waits for the earlier one", 1, in_order(20));
    // A job larger than one block (REC_BLK records) keeps its order, between two smaller ones finished last-first
    g_nlog = 0; uint64_t c = jseq_issue(), d = jseq_issue(), e = jseq_issue();
    job(e, 5 + 600, 7); job(d, 5, 600); job(c, 0, 5);
    rec_drain(); EXPECT("three jobs, one of three blocks, finished last-first", 1, in_order(612));
    // Blocks recorded outside a job go to the queue directly, as with DM_NVN_PAR=0
    g_nlog = 0; for (int i = 0; i < 3; i++) put_tag(i); rec_drain(); EXPECT("records outside jobs", 1, in_order(3));
    // More jobs than slots: numbering waits for pushes, and the order holds
    g_nlog = 0; for (int i = 0; i < JSEQ_N + 40; i++) job(jseq_issue(), i, 1);
    rec_drain(); EXPECT("more jobs than sequencer slots", 1, in_order(JSEQ_N + 40));
    printf("== par seq: %d checks, %d failed ==\n", count, fail);
    return fail != 0;
} }
