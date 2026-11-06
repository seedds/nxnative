// Failed GPU work, named (nvn_mtl.m cbn_close / cbn_failed; issue #1326). One submission of three segments (DM_NVN_CUT=1 cuts at each target change): pass 1 draws program A
// twice, pass 2 dispatches compute shader C and draws program B, pass 3 draws A again. DM_TEST_GPU_HANG=2 makes the second main command buffer report a hang in the shape
// of #1326's log (its work still runs). Checks that the log names it f7 s1 p2-2 with C's and B's keys, lists the segment before it with A's keys and not the one after it;
// that an innocent victim and a command buffer with no error print nothing; that a page fault prints only entries not printed before; that an ignored command buffer
// (GPU access revoked) prints once; that a hang reported after the ring has gone round is still named; and that a command buffer gets its label. Then the cost when
// nothing fails: the per-draw note and the per-command-buffer close, timed. Real Metal 4 queue, no window, no game files; run with and without DM_NVN_ENC_THREAD
#include "../nvn/nvn_mtl.m"
#include <sys/stat.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
#define KEY_A_V "a0a0a0a0a0a0a0a1"
#define KEY_A_F "a0a0a0a0a0a0a0a2"
#define KEY_B_V "b0b0b0b0b0b0b0b1"
#define KEY_B_F "b0b0b0b0b0b0b0b2"
#define KEY_C "c0c0c0c0c0c0c0c3"
#define FRAME 7
#define HANG_AT "2"   // the main command buffer that reports the hang: the second segment
#define DUMP_WAIT_NS 5000000000ull

// The host's log (stderr) goes to a file while the cases run, so the checks can read it
static FILE *g_log; static int g_stderr;
static void log_capture(void) { fflush(stderr); g_log = tmpfile(); if (!g_log) abort(); g_stderr = dup(2); dup2(fileno(g_log), 2); }
static char *log_text(void) {   // all of it so far (pread: the shared offset stderr writes at is left alone)
    fflush(stderr); struct stat st; if (fstat(fileno(g_log), &st)) abort();
    char *b = calloc((size_t)st.st_size + 1, 1); if (!b || pread(fileno(g_log), b, (size_t)st.st_size, 0) != st.st_size) abort();
    return b;
}
static void log_release(void) { fflush(stderr); dup2(g_stderr, 2); close(g_stderr); char *b = log_text(); fputs(b, stdout); free(b); fclose(g_log); }
static int count(const char *text, const char *what) { int n = 0; for (const char *p = text; (p = strstr(p, what)); p++) n++; return n; }
static size_t log_size(void) { fflush(stderr); struct stat st; return fstat(fileno(g_log), &st) ? 0 : (size_t)st.st_size; }

typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(void) { chain c = {0}; if (posix_memalign((void **)&c.p, getpagesize(), 65536)) abort(); memset(c.p, 0, 65536); mtl_pool_add(c.p, 65536); return c; }
static void emit(chain *c, unsigned op, const void *a, size_t n) {
    size_t sz = (8 + n + 7) & ~7ull; if (c->n + sz > 65536) abort();
    op_hdr h = {op, (uint32_t)sz}; memcpy(c->p + c->n, &h, 8); if (n) memcpy(c->p + c->n + 8, a, n); c->n += sz;
}
static tex_t target(uint64_t serial) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .off = (int64_t)(serial - 97000) << 20};   // own storage: two targets of one description at one offset are one texture
    t.d.width = t.d.height = 8; t.d.depth = t.d.samples = t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8; return t;
}
static void rt(chain *c, const tex_t *t) {
    p_set_rt r = {.n = 1}; r.color[0] = t; emit(c, OP_SET_RT, &r, sizeof r);
    p_viewports v = {.n = 1}; v.v[0][2] = v.v[0][3] = 8; emit(c, OP_VIEWPORTS, &v, sizeof v);
    p_scissor sc = {0, 0, 8, 8}; emit(c, OP_SCISSOR, &sc, sizeof sc);
}
static void bind(chain *c, const prog_t *p, int stages) { p_bind_program b = {p, stages}; emit(c, OP_BIND_PROGRAM, &b, sizeof b); }
static void triangle(chain *c) { p_draw d = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3, .instances = 1}; emit(c, OP_DRAW, &d, sizeof d); }
static NvShader *shader(id<MTLLibrary> lib, NSString *fn, int stage, const char *key) {
    NvShader *s = [NvShader new]; s->fn = [lib newFunctionWithName:fn]; s->stage = stage; strlcpy(s->key, key, sizeof s->key);
    if (!s->fn) { fprintf(stderr, "no function %s\n", fn.UTF8String); exit(2); }
    return s;
}
// NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): serial_put retains the program in g_by_serial through hm_put_id's CFBridgingRetain (the leak is reported at the closing brace)
static void program(prog_t *p, NvShader *v, NvShader *f, NvShader *k) {
    NvProg *o = [NvProg new]; o->sh[0] = v; o->sh[1] = f; o->sh[5] = k; serial_put(p->serial, o);
}
// NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
static NSError *nested(NSString *domain, NSInteger code, NSString *text, NSError *under) {   // as Metal 4 reports a command buffer's error: the cause under a timeout with no text
    NSMutableDictionary *info = [NSMutableDictionary dictionary]; if (text) info[NSLocalizedDescriptionKey] = text;
    if (under) { info[NSUnderlyingErrorKey] = under; info[NSMultipleUnderlyingErrorsKey] = @[under]; }
    return [NSError errorWithDomain:domain code:code userInfo:info];
}
static NSError *metal_error(NSInteger code, const char *text, NSInteger iogpu) {
    return nested(MTL4CommandQueueErrorDomain, MTL4CommandQueueErrorTimeout, nil, nested(MTL4CommandQueueErrorDomain, code, @(text), nested(@"IOGPUCommandQueueErrorDomain", iogpu, nil, nil)));
}

int main(void) { @autoreleasepool {
    alarm(120); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("DM_TEST_GPU_HANG", HANG_AT, 1);
    setenv("DM_NVN_CUT", "1", 1);   // a segment at each target change after a draw
    setenv("DM_NVN_TBDR", "0", 1);   // no pass merged across the target changes
    mtl_init(); mtl_set_frame(FRAME);
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n [[vertex_id]]) { float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(p[n], 0, 1); }\n"
        "fragment float4 red() { return float4(1, 0, 0, 1); }\nfragment float4 blue() { return float4(0, 0, 1, 1); }\n"
        "kernel void k(uint i [[thread_position_in_grid]]) {}\n" options:nil error:&err];
    if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 2; }
    static prog_t pa = {.serial = 97001}, pb = {.serial = 97002}, pc = {.serial = 97003};
    NvShader *k = shader(lib, @"k", 5, KEY_C); k->rf.tg[0] = k->rf.tg[1] = k->rf.tg[2] = 1;
    program(&pa, shader(lib, @"v", 0, KEY_A_V), shader(lib, @"red", 1, KEY_A_F), nil);
    program(&pb, shader(lib, @"v", 0, KEY_B_V), shader(lib, @"blue", 1, KEY_B_F), nil);
    program(&pc, nil, nil, k);
    tex_t t1 = target(97101), t2 = target(97102);
    chain ch = chain_new();
    rt(&ch, &t1); bind(&ch, &pa, 3); triangle(&ch); triangle(&ch);   // segment 0, pass 1
    rt(&ch, &t2); bind(&ch, &pc, 32); p_dispatch d = {.x = 1, .y = 1, .z = 1}; emit(&ch, OP_DISPATCH, &d, sizeof d); bind(&ch, &pb, 3); triangle(&ch);   // segment 1, pass 2: the hang
    rt(&ch, &t1); bind(&ch, &pa, 3); triangle(&ch);   // the last segment, pass 3
    emit(&ch, OP_END, NULL, 0);

    log_capture();
    uint64_t cuts = mtl_cut_count();
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(ch.p);
    uint64_t fence = mtl_sub_commit(); int done = mtl_wait(fence, DUMP_WAIT_NS);
    char *text = NULL; uint64_t t0 = now_ns();
    do { free(text); text = log_text(); if (strstr(text, "<- failed")) break; usleep(10000); } while (now_ns() - t0 < DUMP_WAIT_NS);
    size_t first_dump = log_size();
    int segments = (int)(mtl_cut_count() - cuts);

    char want[512];
    snprintf(want, sizeof want, "main command buffer #2 f%d s1 p2-2 caused a GPU hang (imitated by DM_TEST_GPU_HANG)", FRAME); int header = strstr(text, want) != NULL;
    snprintf(want, sizeof want, "#1 f%d s0 p1-1 (segment of fence %llu): draws 2, dispatches 0; v:" KEY_A_V " f:" KEY_A_F "\n", FRAME, (unsigned long long)fence); int before = strstr(text, want) != NULL;
    snprintf(want, sizeof want, "#2 f%d s1 p2-2 (segment of fence %llu): draws 1, dispatches 1; c:" KEY_C " v:" KEY_B_V " f:" KEY_B_F "  <- failed", FRAME, (unsigned long long)fence); int hung = strstr(text, want) != NULL;
    int dumps = count(text, "failed GPU work:"), after = strstr(text, "   #3 ") != NULL;
    free(text);

    // Errors as the #1326 logs nest them, given to the last segment's feedback (#3) by hand
    cbn_entry e2 = g_cbn_ring[2 % CBN_RING], e3 = g_cbn_ring[3 % CBN_RING];   // as cbn_close gave them to the feedback handlers
    cbn_failed(&e3, nil);
    cbn_failed(&e3, metal_error(MTL4CommandQueueErrorTimeout, "Discarded (victim of GPU error/recovery) (00000005:kIOGPUCommandBufferCallbackErrorInnocentVictim)", 5));
    text = log_text(); int quiet = count(text, "failed GPU work:") == 1; free(text);
    cbn_failed(&e3, metal_error(MTL4CommandQueueErrorNotPermitted, "Caused GPU Address Fault Error (0000000b:kIOGPUCommandBufferCallbackErrorPageFault)", 11));
    text = log_text();
    snprintf(want, sizeof want, "main command buffer #3 f%d s2 p3-3 caused a GPU page fault;", FRAME); int fault = strstr(text, want) != NULL;
    snprintf(want, sizeof want, "   #3 f%d s2 p3-3 (last segment of fence %llu): draws 1, dispatches 0; v:" KEY_A_V " f:" KEY_A_F "  <- failed", FRAME, (unsigned long long)fence); int fault_entry = strstr(text, want) != NULL;
    int once = count(text, "   #1 f") == 1 && count(text, "   #2 f") == 1;
    free(text);
    NSError *ignored = metal_error(MTL4CommandQueueErrorAccessRevoked, "Ignored (for causing prior/excessive GPU errors) (00000004:kIOGPUCommandBufferCallbackErrorSubmissionsIgnored)", 4);
    cbn_failed(&e3, ignored); cbn_failed(&e3, ignored); cbn_failed(&e2, ignored);
    text = log_text();
    snprintf(want, sizeof want, "main command buffer #3 f%d s2 p3-3 (listed above) was ignored: macOS revoked the GPU access", FRAME); int revoked_header = strstr(text, want) != NULL;
    int revoked_once = count(text, "failed GPU work:") == 3 && count(text, "   #3 f") == 1;
    free(text);

    // The label, read back on the command buffer it went to: with deferred encoding both blocks run on the encode thread, in order. Then the cost of a close (label, ring copy)
    enum { DRAWS = 10000000, CLOSES = 20000 };
    __block NSString *label = nil;
    mtl_sub_begin(); cbn_close('s', 4, 99); rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) { label = cb.label; });
    uint64_t c0 = now_ns(); for (int i = 0; i < CLOSES; i++) @autoreleasepool { cbn_close('s', 0, 0); } double per_close = (double)(now_ns() - c0) / CLOSES;
    int closed = mtl_wait(mtl_sub_commit(), DUMP_WAIT_NS);
    // A hang reported after the ring has gone round: the failed command buffer is named from the entry its handler kept, the ones before it are gone
    cbn_failed(&e3, metal_error(MTL4CommandQueueErrorTimeout, "Caused GPU Hang Error (00000003:kIOGPUCommandBufferCallbackErrorHang)", 3));
    text = log_text();
    snprintf(want, sizeof want, "main command buffer #3 f%d s2 p3-3 caused a GPU hang;", FRAME); int late = strstr(text, want) != NULL && count(text, "   #3 f") == 2;
    int late_gone = strstr(text, "   #2: no longer in the ring") != NULL;
    free(text);
    log_release();

    printf("segments cut %d, fence %llu, SyncWait %s; dump %zu bytes, %d headers\n", segments, (unsigned long long)fence, done ? "returned" : "timed out", first_dump, dumps);
    CHECK("the submission was cut into three segments, and completed", segments == 2 && done && !mtl_last_commit_error());
    CHECK("the imitated hang is named: main command buffer #2, frame, segment and pass", header);
    CHECK("the failed segment lists its dispatch's and its draw's shader keys, each once", hung);
    CHECK("the segment before it is listed with its own keys", before);
    CHECK("the segment after it is not listed", !after);
    CHECK("one dump for the hang", dumps == 1);
    CHECK("no error and an innocent victim print nothing", quiet);
    CHECK("a page fault prints its header and its own entry", fault && fault_entry);
    CHECK("an entry is printed once", once);
    CHECK("an ignored command buffer prints once, its entry already listed", revoked_header && revoked_once);
    snprintf(want, sizeof want, "f%d s4 no pass", FRAME);
    CHECK("a hang reported after the ring went round still names its command buffer, and says the ones before it are gone", late && late_gone);
    CHECK("a command buffer is labelled with its frame, segment and passes", closed && label && !strcmp(label.UTF8String, want));

    // The cost when nothing fails: a draw notes its program's shaders (already noted: the common case, or the program's first draw in the command buffer)
    __unsafe_unretained NvProg *a = prog_get(&pa);
    c0 = now_ns(); for (int i = 0; i < DRAWS; i++) { cbn_prog(a); __asm__ volatile("" ::: "memory"); } double per_draw = (double)(now_ns() - c0) / DRAWS;
    c0 = now_ns(); for (int i = 0; i < DRAWS / 10; i++) { a->sh[0]->cbn_seen = a->sh[1]->cbn_seen = 0; g_cbn.nkeys = 0; cbn_prog(a); } double per_new = (double)(now_ns() - c0) / (DRAWS / 10);
    printf("cost: %.2f ns a draw whose shaders are noted, %.1f ns a program's first draw in a command buffer, %.0f ns a command buffer closed (label and ring copy%s)\n", per_draw, per_new, per_close, rec_on() ? ", the label recorded for the encode thread" : "");
    printf("t_gpu_hang_names: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
