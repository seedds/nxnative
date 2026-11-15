// The game shaders a failed command buffer encoded, and those common to all such failures (cbs_note, cbs_take, cbs_feedback): a GPU fault's error names no pass or shader. A victim of another's
// fault and a submission refused after one ran nothing of their own and do not count (Splatoon Raiders, issue #1289)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static NvShader *shader(const char *key) { NvShader *s = [NvShader new]; strlcpy(s->key, key, sizeof s->key); return s; }
static NSError *error(NSString *text) { return [NSError errorWithDomain:@"t" code:1 userInfo:@{NSLocalizedDescriptionKey: text}]; }
static NSString *const FAULT = @"Caused GPU Address Fault Error (0000000b:kIOGPUCommandBufferCallbackErrorPageFault)";
static const cbs_tally_ent *tally(const char *key) { for (uint32_t i = 0; i < g_cbs_tally_n; i++) if (!strcmp(g_cbs_tally[i].key, key)) return &g_cbs_tally[i]; return NULL; }
static int count(const char *key, uint32_t failed, uint32_t runs) { const cbs_tally_ent *t = tally(key); return t && t->failed == failed && t->runs == runs; }
static NSString *const VICTIM = @"Discarded (victim of GPU error/recovery) (00000005:kIOGPUCommandBufferCallbackErrorInnocentVictim)";
int main(void) { @autoreleasepool {
    NvShader *a = shader("aaaa_b"), *b = shader("bbbb_b"), *c = shader("cccc_b"), *d = shader("dddd_b");
    cbs_note(a); cbs_note(a); cbs_note(b); cbs_note(a); cbs_note(c);
    cbs_list *l = cbs_take();
    CHECK(l && l->n == 3 && !strcmp(l->e[0].key, "aaaa_b") && !strcmp(l->e[2].key, "cccc_b"), "a command buffer lists each shader once, in order");
    cbs_feedback(l, nil, "ok");
    CHECK(g_cbs_failures == 0, "a command buffer that succeeded counts for nothing");

    cbs_note(a); l = cbs_take();
    CHECK(l && l->n == 1 && l->e[0].runs == 2, "the next command buffer lists a shader the last one had, and counts its runs");
    cbs_feedback(l, error(VICTIM), "victim");
    cbs_note(a); cbs_feedback(cbs_take(), error(@"Ignored (for causing prior/excessive GPU errors) (00000004:kIOGPUCommandBufferCallbackErrorSubmissionsIgnored)"), "ignored");
    CHECK(g_cbs_failures == 0 && g_cbs_tally_n == 0, "a victim and a refused submission ran nothing of their own");

    cbs_note(a); cbs_note(b); cbs_note(c); cbs_feedback(cbs_take(), error(FAULT), "first");   // a: 4 runs, b and c: 2
    CHECK(g_cbs_failures == 1 && g_cbs_tally_n == 3 && count("aaaa_b", 1, 4) && count("bbbb_b", 1, 2) && count("cccc_b", 1, 2), "the first fault's shaders are all suspects");
    cbs_note(d); cbs_note(c); cbs_note(a); cbs_feedback(cbs_take(), error(FAULT), "second");
    CHECK(g_cbs_failures == 2 && g_cbs_tally_n == 4 && count("aaaa_b", 2, 5) && count("cccc_b", 2, 3) && count("bbbb_b", 1, 2) && count("dddd_b", 1, 1), "a later fault raises the counts of the shaders it ran");
    CHECK(!strcmp(g_cbs_tally[0].key, "cccc_b") && !strcmp(g_cbs_tally[1].key, "aaaa_b"), "most often failed first, then the rarer");
    cbs_note(a); cbs_feedback(cbs_take(), error(VICTIM), "victim");
    CHECK(g_cbs_failures == 2 && count("aaaa_b", 2, 5), "a victim does not count, though the shader ran again");
    cbs_note(c); cbs_feedback(cbs_take(), error(@"Impacting Interactivity (00000002:kIOGPUCommandBufferCallbackErrorHang)"), "hang");
    CHECK(g_cbs_failures == 3 && count("cccc_b", 3, 4), "a hang counts, as this command buffer caused it");
    cbs_note(d); cbs_feedback(cbs_take(), error(FAULT), "another cause");
    CHECK(count("cccc_b", 3, 4) && count("dddd_b", 2, 2), "a failure with another cause leaves the earlier counts");
    cbs_feedback(cbs_take(), error(FAULT), "empty");
    CHECK(g_cbs_failures == 5, "a failure with no game shader counts too");

    enum { MANY = CBS_SHOW + 50 };   // a list past what one line shows, and the failures past CBS_REPORTS
    for (int i = 0; i < MANY; i++) { char k[24]; snprintf(k, sizeof k, "%016x_b", i); cbs_note(shader(k)); }
    cbs_list *many = cbs_take(); CHECK(many && many->n == MANY, "the list grows past its first allocation");
    cbs_feedback(many, error(FAULT), "many");
    for (int i = 0; i < CBS_REPORTS + 4; i++) { cbs_note(a); cbs_feedback(cbs_take(), error(FAULT), "repeat"); }
    CHECK(g_cbs_failures == 6 + CBS_REPORTS + 4 && tally("aaaa_b")->failed == 2 + CBS_REPORTS + 4, "failures go on being counted after the reports stop");
    // DM_RES_CHECK: an object retired behind an earlier fence than a binding's goes while that binding is on the GPU
    mtl_init(); g_res_check_on = 1;
    id<MTLBuffer> early = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared], fine = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(early); res_add(fine);
    res_check(fine, "fine_b", 0); defer_obj_release(fine); fine = nil;   // bound, then retired behind the same submission
    defer_obj_release(early); g_fence++; res_check(early, "late_b", 0); early = nil;   // retired, then bound by the next submission
    pthread_mutex_lock(&g_mu); g_done = g_fence; pthread_mutex_unlock(&g_mu);
    defer_drain();
    CHECK(g_res_late == 1, "the object bound after its fence is reported, the other not");
    printf("failed command buffers' shaders: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
