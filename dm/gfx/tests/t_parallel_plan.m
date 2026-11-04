// The coordinator's first slice (DM_NVN_PAR=plan, nvn_mtl.m par_plan_*): the scan plans up to RX_CHUNKS chunks; each after the first is translated from the scan's
// snapshot into its own command buffer, allocator and arenas. Events and GPU writes must come out exactly as the serial replay's, and a boundary whose state is not the
// snapshot's must finish the submission serially, still exactly.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
typedef struct { uint8_t bytes[1024]; size_t n; } commands;
static void emit(commands *c, uint32_t op, const void *data, size_t n) {
    uint8_t *p = c->bytes + c->n; op_hdr h = {op, (uint32_t)(8 + n)};
    memcpy(p, &h, 8); if (n) memcpy(p + 8, data, n); c->n += h.size;
}
static void event(commands *c, volatile uint32_t *dst, uint32_t value) {
    uint8_t args[16]; uint64_t addr = (uintptr_t)dst; uint32_t mode = 0;
    memcpy(args, &addr, 8); memcpy(args + 8, &mode, 4); memcpy(args + 12, &value, 4); emit(c, OP_SIGNAL_EVENT, args, sizeof args);
}
enum { LISTS = 6 };
static volatile uint32_t g_value;
// One submission: LISTS lists, the last two called from a parent list (a split inside a call), each changing the render target, drawing, filling 4 bytes and signalling
static uint64_t submit(void *mem, int rep, int spoil) {
    static commands list[LISTS], parent; uint64_t handles[LISTS];
    memset(list, 0, sizeof list); memset(&parent, 0, sizeof parent);
    int nh = 0;
    for (int i = 0; i < LISTS; i++) {
        p_set_rt rt = {0}; rt.dview.base_level = 10 * rep + i + 1; p_draw draw = {0};
        p_clear_buf fill = {.addr = (uintptr_t)mem + (uint64_t)(16 * rep + i) * 4, .size = 4, .value = 0x5a000000u + (uint32_t)(rep << 8) + (uint32_t)i};
        emit(&list[i], OP_SET_RT, &rt, sizeof rt); emit(&list[i], OP_DRAW, &draw, sizeof draw); emit(&list[i], OP_CLEAR_BUF, &fill, sizeof fill);
        event(&list[i], &g_value, (uint32_t)i + 1); emit(&list[i], OP_END, NULL, 0);
        if (i < LISTS - 2) handles[nh++] = (uintptr_t)list[i].bytes;
    }
    for (int i = LISTS - 2; i < LISTS; i++) { uint64_t p = (uintptr_t)list[i].bytes; emit(&parent, OP_CALL, &p, 8); }
    emit(&parent, OP_END, NULL, 0); handles[nh++] = (uintptr_t)parent.bytes;
    uint64_t chunks = g_planst.chunks, fb = g_planst.fb_state;
    mtl_sub_begin(); mtl_sub_index(handles, nh);
    CHECK(g_plan_n == RX_CHUNKS - 1 && g_plan_active, "the scan plans a split near each quarter of the draws");
    if (spoil && g_plan_n > 1) { typeof(S_rx[0]) *s = (void *)&g_plan[1]->s; s->blend_color[0] = 123.0f; }   // the second split's snapshot no longer matches the replay
    for (int i = 0; i < nh; i++) { mtl_exec_rest(handles + i + 1, nh - i - 1); mtl_exec((const uint8_t *)handles[i]); }
    mtl_exec_rest(NULL, 0);
    CHECK(t_par_stopped && RX.npost == 1, "the submitting thread stops before the first split, after the first chunk's event");
    int first = g_rxi; void *pool = RX.sub_pool;
    uint64_t fence = mtl_sub_commit();
    CHECK(!g_plan_active && !t_par_stop && !t_par_stopped, "the plan leaves no stop behind");
    if (!spoil) CHECK(g_planst.chunks == chunks + RX_CHUNKS - 1 && g_planst.fb_state == fb, "every planned chunk is translated from its snapshot");
    else CHECK(g_planst.chunks == chunks + 1 && g_planst.fb_state == fb + 1, "a snapshot that differs finishes the submission serially at that split");
    CHECK(g_rxi != first || spoil, "the commit happens in the last chunk's context");
    CHECK(pool != NULL && RX.sub_pool == NULL, "the submission's autorelease pool is popped once");
    for (int c = 0; c < RX_CHUNKS; c++) CHECK(!g_rxs[c].sub_arenas.count, "every chunk's arenas retired with the submission");
    mtl_drain();
    for (int i = 0; i < LISTS; i++) CHECK(((uint32_t *)mem)[16 * rep + i] == 0x5a000000u + (uint32_t)(rep << 8) + (uint32_t)i, "each chunk's GPU write lands");
    CHECK(g_value == LISTS && !mtl_last_commit_error(), "the last event completes with no GPU error");
    return fence;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_PAR", "plan", 1); setenv("DM_NVN_PAR_DRAWS", "1", 1); mtl_init(); state_reset();
    void *mem = NULL; CHECK(!posix_memalign(&mem, 16384, 16384), "allocate shared GPU pool");
    if (!mem) return 1; memset(mem, 0, 16384); mtl_pool_add(mem, 16384);
    for (int rep = 0; rep < 4; rep++) {
        g_value = 0; submit(mem, rep, rep == 2);
        // allocators: the chunks of one submission take distinct ones
        int distinct = 1; for (int i = 0; i < RX_CHUNKS; i++) for (int j = 0; j < i; j++) if (g_sub_alloc[i] >= 0 && g_sub_alloc[i] == g_sub_alloc[j]) distinct = 0;
        CHECK(distinct, "chunks take allocators from their own pools");
    }
    mtl_pool_del(mem); mtl_drain(); free(mem);
    printf("parallel plan: %d checks, %d failures\n", checks, failed); return failed != 0;
} }
