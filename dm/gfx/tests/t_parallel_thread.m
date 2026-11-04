// Real worker replay preserves nested return stacks and the remaining submission handles.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
typedef struct { uint8_t bytes[2048]; size_t n; } commands;
static const uint8_t *emit(commands *c, uint32_t op, const void *data, size_t n) {
    uint8_t *p = c->bytes + c->n; op_hdr h = {op, (uint32_t)(8 + n)};
    memcpy(p, &h, 8); if (n) memcpy(p + 8, data, n); c->n += h.size; return p;
}
static void event(commands *c, volatile uint32_t *dst, uint32_t value) {
    uint8_t args[16]; uint64_t addr = (uintptr_t)dst; uint32_t mode = 0;
    memcpy(args, &addr, 8); memcpy(args + 8, &mode, 4); memcpy(args + 12, &value, 4); emit(c, OP_SIGNAL_EVENT, args, sizeof args);
}
static void call(commands *c, commands *child) { uint64_t p = (uintptr_t)child->bytes; emit(c, OP_CALL, &p, 8); }
typedef struct { int context, failed; dispatch_semaphore_t ready, go; } context_probe;
static void *probe_context(void *arg) {
    context_probe *p = arg; rx_select(p->context);
    dispatch_semaphore_signal(p->ready); dispatch_semaphore_wait(p->go, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < 10000; i++) {
        if (g_rxi != p->context || g_rx != &g_rxs[p->context] || &S != &S_rx[p->context] || &R != &R_rx[p->context]) p->failed = 1;
        rx_select(p->context);
    }
    return NULL;
}
static void check_context_ownership(void) {
    context_probe p[2]; pthread_t threads[2]; int owner = g_rxi, started = 0;
    for (int i = 0; i < 2; i++) {
        p[i] = (context_probe){.context = i + 1, .ready = dispatch_semaphore_create(0), .go = dispatch_semaphore_create(0)};
        int err = pthread_create(&threads[i], NULL, probe_context, &p[i]);
        CHECK(!err, "start context isolation probe"); if (err) break; started++;
    }
    for (int i = 0; i < started; i++) dispatch_semaphore_wait(p[i].ready, DISPATCH_TIME_FOREVER);
    CHECK(g_rxi == owner && g_rx == &g_rxs[owner], "worker selection does not change submitter context");
    for (int i = 0; i < started; i++) dispatch_semaphore_signal(p[i].go);
    for (int i = 0; i < started; i++) { pthread_join(threads[i], NULL); CHECK(!p[i].failed, "overlapping threads retain their own selected context"); }
}
static void select_position(par_pos pos) {
    mtl_exec_rest(pos.rest, pos.nrest); g_ret_n = pos.depth;
    memcpy(g_ret, pos.ret, (size_t)pos.depth * sizeof *g_ret); g_exec_cur = pos.op;
}
static void check_planner_positions(void) {
    commands leaf = {0}, root = {0}, other = {0}; p_set_rt rt = {0}; p_draw draw = {0};
    emit(&leaf, OP_SET_RT, &rt, sizeof rt); emit(&leaf, OP_DRAW, &draw, sizeof draw); emit(&leaf, OP_END, NULL, 0);
    p_scissor sc = {.x = 3, .w = 8, .h = 8}; emit(&root, OP_SCISSOR, &sc, sizeof sc); call(&root, &leaf);
    sc.x = 7; emit(&root, OP_SCISSOR, &sc, sizeof sc); call(&root, &leaf); emit(&root, OP_END, NULL, 0);
    sc.x = 11; emit(&other, OP_SCISSOR, &sc, sizeof sc); call(&other, &leaf); emit(&other, OP_END, NULL, 0);
    uint64_t handles[] = {(uintptr_t)root.bytes, (uintptr_t)other.bytes, (uintptr_t)root.bytes};
    par_walk walk = par_walk_begin(handles, 3); par_pos all[32], targets[5], pos; int count = 0, nt = 0;
    op_hdr h; const void *args;
    while (par_walk_next(&walk, &pos, &h, &args)) {
        if (count < 32) all[count] = pos; count++;
        if (h.op == OP_SET_RT && nt < 5) targets[nt++] = pos;
    }
    CHECK(!walk.bad && count == 15 && nt == 5, "planner walks repeated handles and nested calls exactly once");
    int distinct = 1; for (int i = 0; i < nt; i++) for (int j = 0; j < i; j++) if (par_pos_equal(&targets[i], &targets[j])) distinct = 0;
    CHECK(distinct, "identical command addresses have distinct invocation positions");
    par_scan(handles, 3);
    CHECK(g_parc_n == 5 && g_parf_n == 4, "scan produces only the boundaries following the draw threshold");
    const int expected[] = {3, 7, 11, 3, 7};
    for (int i = 0; i < nt; i++) {
        select_position(targets[i]); const parf_snap *f = parf_at(targets[i].op);
        CHECK(i ? f && f->r.sc.x == expected[i] && par_pos_equal(&f->pos, &targets[i]) : !f,
              "snapshot lookup never substitutes another invocation of the same list");
    }
    // Skip earlier boundaries: check mode must compare the actual invocation, not the first unused address.
    parc_snap *last = &g_parc[4]; select_position(last->pos); S = last->s; R = last->r; pend_reset();
    memset(&g_pck, 0, sizeof g_pck); parc_check();
    CHECK(last->used && !g_parc[0].used && g_pck.same == 1 && !g_pck.differ, "contract check selects the correct invocation after skipped boundaries");
    const int cuts[] = {0, 4, 7, 10, 13, 15}; int covered = 0, ordered = 1;
    for (int range = 0; range < 5; range++) {
        walk = par_walk_from(all[cuts[range]]);
        while (par_walk_next(&walk, &pos, &h, &args)) {
            if (cuts[range + 1] < count && par_pos_equal(&pos, &all[cuts[range + 1]])) break;
            if (covered >= count || !par_pos_equal(&pos, &all[covered])) ordered = 0;
            covered++;
        }
        if (walk.bad) ordered = 0;
    }
    CHECK(ordered && covered == count, "independently resumed ranges cover commands in order without overlap or gaps");
    const uint32_t unsupported_ops[] = {OP_MULTI_DRAW, OP_COPY_T2B, OP_TF_BIND, OP_TF_STATE}; uint64_t one;
    for (unsigned i = 0; i < sizeof unsupported_ops / sizeof *unsupported_ops; i++) {
        commands unsupported = {0}; call(&unsupported, &root); emit(&unsupported, unsupported_ops[i], NULL, 0); emit(&unsupported, OP_END, NULL, 0);
        one = (uintptr_t)unsupported.bytes; par_scan(&one, 1);
        CHECK(!g_parc_n && !g_parf_n, "unsupported tail invalidates the whole plan, including earlier snapshots");
    }
    commands deep[PAR_CALLS + 2] = {0};
    for (int i = 0; i <= PAR_CALLS; i++) call(&deep[i], &deep[i + 1]);
    for (int i = 0; i <= PAR_CALLS; i++) emit(&deep[i], OP_END, NULL, 0);
    emit(&deep[PAR_CALLS + 1], OP_SET_RT, &rt, sizeof rt); emit(&deep[PAR_CALLS + 1], OP_END, NULL, 0);
    one = (uintptr_t)deep[0].bytes; walk = par_walk_begin(&one, 1);
    while (par_walk_next(&walk, &pos, &h, &args)) {}
    CHECK(walk.bad, "excessive call depth declines planning before writing past the return stack");
    one = (uintptr_t)deep[1].bytes; walk = par_walk_begin(&one, 1); count = 0;
    while (par_walk_next(&walk, &pos, &h, &args)) { count++; CHECK(pos.depth == PAR_CALLS, "planner supports the replay's full call depth"); }
    CHECK(!walk.bad && count == 1, "maximum supported nesting completes normally");
    commands cycle = {0}; one = (uintptr_t)cycle.bytes; emit(&cycle, OP_JUMP, &one, sizeof one);
    walk = par_walk_begin(&one, 1); walk.left = 4;
    CHECK(!par_walk_next(&walk, &pos, &h, &args) && walk.bad, "cyclic command flow exhausts the planning budget");
    op_hdr malformed = {OP_BEGIN, 0}; memcpy(cycle.bytes, &malformed, sizeof malformed); walk = par_walk_begin(&one, 1);
    CHECK(!par_walk_next(&walk, &pos, &h, &args) && walk.bad, "zero-sized command cannot stall the planner");
    malformed = (op_hdr){OP_CALL, 8}; memcpy(cycle.bytes, &malformed, sizeof malformed); walk = par_walk_begin(&one, 1);
    CHECK(!par_walk_next(&walk, &pos, &h, &args) && walk.bad, "truncated call cannot read a target outside its command");
    mtl_exec_rest(NULL, 0); g_ret_n = 0; g_exec_cur = NULL; g_parc_n = g_parf_n = 0;
    memset(&g_pck, 0, sizeof g_pck); state_reset(); memset(&R, 0, sizeof R);
}
static void check_bounded_replay(void) {
    commands leaf = {0}, root = {0}; p_scissor sc = {.w = 8, .h = 8};
    sc.x = 2; emit(&leaf, OP_SCISSOR, &sc, sizeof sc);
    sc.x = 3; emit(&leaf, OP_SCISSOR, &sc, sizeof sc); emit(&leaf, OP_END, NULL, 0);
    sc.x = 1; emit(&root, OP_SCISSOR, &sc, sizeof sc); call(&root, &leaf);
    sc.x = 4; emit(&root, OP_SCISSOR, &sc, sizeof sc); call(&root, &leaf);
    sc.x = 5; emit(&root, OP_SCISSOR, &sc, sizeof sc); emit(&root, OP_END, NULL, 0);
    uint64_t handles[] = {(uintptr_t)root.bytes, (uintptr_t)root.bytes};
    par_walk walk = par_walk_begin(handles, 2); par_pos positions[14], pos; op_hdr h; const void *args; int n = 0;
    while (par_walk_next(&walk, &pos, &h, &args)) { if (n < 14) positions[n] = pos; n++; }
    CHECK(!walk.bad && n == 14, "bounded fixture has repeated calls and repeated submission handles");
    if (n != 14) return;
    state_reset(); R.sc.x = -1;
    CHECK(par_continue(positions[0], &positions[0]) == 1 && R.sc.x == -1, "empty bounded range executes no command");
    par_pos absent = positions[1]; absent.nrest = 99;
    CHECK(par_continue(positions[0], &absent) == -1 && R.sc.x == -1, "unreachable endpoint rejects the range before any state changes");
    const int expected[] = {1, 2, 3, 4, 2, 3, 5, 1, 2, 3, 4, 2, 3, 5};
    for (int i = 0; i < 13; i++) {
        CHECK(par_continue(positions[i], &positions[i + 1]) == 1 && R.sc.x == expected[i], "replay executes exactly the state command before its exclusive endpoint");
        par_pos actual = par_here(g_exec_cur);
        CHECK(par_pos_equal(&actual, &positions[i + 1]), "stopping preserves the endpoint call stack and remaining handles");
    }
    CHECK(par_continue(positions[13], NULL) == 0 && R.sc.x == 5 && !g_ret_n && !g_nrest,
          "final range finishes its caller and all remaining handles");
    CHECK(!t_par_stop && !t_par_stopped, "range stop state never leaks into later replay");
    g_exec_cur = NULL; state_reset();
}
static void attach_snapshot_positions(const uint64_t *handles, int n) {
    par_walk walk = par_walk_begin(handles, n); par_pos pos; op_hdr h; const void *args;
    int assigned[PARF_MAX] = {0};
    while (par_walk_next(&walk, &pos, &h, &args)) {
        for (int i = 0; i < g_parf_n; i++) if (!assigned[i] && g_parf[i].op == pos.op) { g_parf[i].pos = pos; assigned[i] = 1; break; }
    }
    CHECK(!walk.bad, "fixture command ranges are complete");
    for (int i = 0; i < g_parf_n; i++) CHECK(assigned[i], "fixture snapshot names an exact invocation");
}
static void check_large_write_sets(void) {
    // Populate the table directly to test every representable size, including its full capacity.
    for (int count = 6800; count <= SBW_N; count += SBW_N - 6800) {
        pend_reset(); WT_;
        for (int i = 0; i < count; i++) { g_sbwp[i].page = (uint64_t)i + 1; g_sbwp[i].gen = g_pend_gen; g_sbwp[i].src = SBW_GPU; }
        g_nsbw = count; g_parc_n = 0; parc_put(NULL, NULL); g_exec_cur = NULL;
        CHECK(g_parc[0].w.npg == count, "snapshot retains the full write table");
        g_sbwp[count - 1].gen = 0; g_nsbw--;
        memset(&g_pck, 0, sizeof g_pck); parc_check();
        CHECK(g_pck.extra == 1 && !g_pck.differ, "large scan superset is accepted");
        // Equal counts with a different page must still fail; hashes alone cannot prove inclusion.
        g_sbwp[0].page = SBW_N + 1; g_parc[0].used = 0;
        g_pck.wlogged = 16; parc_check();
        CHECK(g_pck.differ == 1 && g_pck.wf[5] == 1, "missing replay page is rejected above the old limit");
    }
    pend_reset(); g_parc_n = 0; memset(&g_pck, 0, sizeof g_pck);
}
typedef struct { void *mem; int rep; volatile uint32_t value; } submit_probe;
static void *run_submission(void *arg) { @autoreleasepool {
    submit_probe *job = arg; void *mem = job->mem; int rep = job->rep;
        commands root = {0}, child = {0}, leaf = {0}, tail = {0}, last = {0}; p_set_rt rt = {0}; rt.dview.base_level = rep + 1;
        const uint8_t *split = emit(&leaf, OP_SET_RT, &rt, sizeof rt);
        p_clear_buf fill = {.addr = (uintptr_t)mem, .size = 4, .value = 0x12340000u + rep};
        emit(&leaf, OP_CLEAR_BUF, &fill, sizeof fill);
        event(&leaf, &job->value, 3); emit(&leaf, OP_END, NULL, 0);
        event(&child, &job->value, 2); call(&child, &leaf); event(&child, &job->value, 4); emit(&child, OP_END, NULL, 0);
        event(&root, &job->value, 1); call(&root, &child); event(&root, &job->value, 5); emit(&root, OP_END, NULL, 0);
        const uint8_t *split2 = emit(&tail, OP_SET_RT, &rt, sizeof rt);
        fill.addr += 4; fill.value = 0x56780000u + rep; emit(&tail, OP_CLEAR_BUF, &fill, sizeof fill);
        event(&tail, &job->value, 6); emit(&tail, OP_END, NULL, 0);
        event(&last, &job->value, 7); emit(&last, OP_END, NULL, 0);
        uint64_t handles[] = {(uintptr_t)root.bytes, (uintptr_t)tail.bytes, (uintptr_t)last.bytes};
        mtl_sub_begin();
        // Supply known boundary snapshots directly: this tests worker continuation, independently of scan heuristics.
        if (!g_parf) g_parf = calloc(PARF_MAX, sizeof *g_parf);
        g_parf_n = 2;
        for (int i = 0; i < 2; i++) { g_parf[i].op = i ? split2 : split; g_parf[i].s = S; g_parf[i].r = R; g_parf[i].w = g_wt_rx[g_rxi]; }
        g_parf[1].r.rt = rt; g_parf[1].r.has_rt = 1;
        attach_snapshot_positions(handles, 3);
        pthread_t owner = g_sub_thread; uint64_t chunks = g_parw_chunks, boundaries = g_par_nb;
        for (int i = 0; i < 3; i++) { mtl_exec_rest(handles + i + 1, 2 - i); mtl_exec((const uint8_t *)handles[i]); }
        mtl_exec_rest(NULL, 0);
        CHECK(g_par_handoff && RX.npost == 2 && g_ret_n == 2, "submitter stops inside nested call before remaining events");
        CHECK(R.rt.dview.base_level != rt.dview.base_level, "render target operation is not applied before handoff");
        par_chunks_run();
        CHECK(!g_par_handoff && g_ret_n == 0, "worker resumes both callers and completes handles");
        CHECK(g_parw_chunks == chunks + 2, "two chunks ran on the actual worker");
        CHECK(g_par_nb == boundaries + 2, "each resumed boundary is counted only once");
        CHECK(pthread_equal(owner, g_sub_thread) && !t_par_worker, "submitter identity remains unchanged");
        CHECK(RX.npost == 7, "all seven events recorded exactly once");
        for (int i = 0; i < RX.npost; i++) CHECK(RX.post[i].v == (uint64_t)i + 1, "events preserve nested and handle order");
        mtl_sub_commit(); mtl_drain();
        CHECK(((uint32_t *)mem)[0] == 0x12340000u + rep && ((uint32_t *)mem)[1] == 0x56780000u + rep, "both worker chunks execute GPU writes");
        CHECK(job->value == 7 && !mtl_last_commit_error(), "ordered events complete with no GPU error");
    return NULL;
} }
static void check_allocator_wrap(void *mem) {
    enum { N = 9 }; commands list[N] = {0}; uint64_t handles[N];
    mtl_sub_begin(); g_parf_n = N;
    typeof(R) previous = R;
    for (int i = 0; i < N; i++) {
        p_set_rt rt = {0}; rt.dview.base_level = i + 20;
        const uint8_t *split = emit(&list[i], OP_SET_RT, &rt, sizeof rt);
        p_clear_buf fill = {.addr = (uintptr_t)mem + (i + 2) * 4, .size = 4, .value = 0xabc00000u + i};
        emit(&list[i], OP_CLEAR_BUF, &fill, sizeof fill); emit(&list[i], OP_END, NULL, 0);
        handles[i] = (uintptr_t)list[i].bytes;
        g_parf[i].op = split; g_parf[i].s = S; g_parf[i].r = previous; g_parf[i].w = g_wt_rx[RXI];
        previous.rt = rt; previous.has_rt = 1;
    }
    attach_snapshot_positions(handles, N);
    uint64_t chunks = g_parw_chunks;
    for (int i = 0; i < N; i++) { mtl_exec_rest(handles + i + 1, N - i - 1); mtl_exec((const uint8_t *)handles[i]); }
    mtl_exec_rest(NULL, 0); par_chunks_run();
    CHECK(g_parw_chunks == chunks + N, "worker cycles through contexts more than twice in one submission");
    int reserved[RX_CHUNKS];
    for (int i = 0; i < RX_CHUNKS; i++) {
        reserved[i] = g_sub_alloc[i];
        CHECK(reserved[i] >= i * ALLOC_MAX && reserved[i] < (i + 1) * ALLOC_MAX, "each context uses its own allocator pool");
        CHECK(sub_allocator(i) == reserved[i], "context reuse keeps the reserved allocator without resetting it");
        for (int j = 0; j < i; j++) CHECK(g_alloc4[reserved[i]] != g_alloc4[reserved[j]], "context allocators are distinct Metal objects");
    }
    uint64_t fence = mtl_sub_commit();
    for (int i = 0; i < RX_CHUNKS; i++) CHECK(g_alloc_fence[reserved[i]] == fence, "all chunk allocators retire at the final fence");
    mtl_drain();
    for (int i = 0; i < N; i++) CHECK(((uint32_t *)mem)[i + 2] == 0xabc00000u + i, "GPU writes survive context and allocator reuse");
    CHECK(!mtl_last_commit_error(), "allocator wrap has no GPU validation failure");
}
static void check_query_handoff(void) {
    mtl_sub_begin(); vis_clear(); g_vis_on = g_vis_counting = 1;
    id<MTLBuffer> samples = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    ((uint64_t *)samples.contents)[0] = 3; ((uint64_t *)samples.contents)[1] = 5;
    // Known visibility slot values isolate query accumulation and buffer ownership from rasterization.
    free(g_vs); g_vs = malloc(2 * sizeof *g_vs); g_vscap = 2;
    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(samples), 0};
    uint64_t early[2] = {0}, late[2] = {0};
    post((post_op){.kind = 3, .addr = (uintptr_t)early, .vis = vis_snapshot()});
    int previous = RXI, ring = g_vis_ring_i;
    RX.cut_copy = 1; sub_cut_to((RXI + 1) % RX_CHUNKS); par_fresh_switch();
    CHECK(!g_vis_rx[previous].slots && !g_vis_rx[previous].n, "previous context relinquishes query slots");
    CHECK(g_nvs == 1 && g_vis_on && g_vis_counting && g_vis_ring_i == ring, "active query and retirement slot follow the handoff");
    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(samples), 1};
    post((post_op){.kind = 3, .addr = (uintptr_t)late, .vis = vis_snapshot()});
    vis_clear(); CHECK(!g_nvs, "reset empties only the live query");
    uint64_t fence = mtl_sub_commit(); mtl_drain();
    CHECK(early[0] == 3 && late[0] == 8, "reports retain the exact prefix through a handoff and later reset");
    CHECK(g_vis_ring[ring].fence == fence, "transferred query buffer retires with the final submission");
    CHECK(!mtl_last_commit_error(), "query handoff has no GPU error");
    g_vis_on = g_vis_counting = 0;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_PAR", "thread1", 1); setenv("DM_NVN_PAR_DRAWS", "1", 1); mtl_init(); state_reset(); check_planner_positions(); check_bounded_replay(); check_large_write_sets(); check_context_ownership();
    void *mem = NULL; CHECK(!posix_memalign(&mem, 16384, 16384), "allocate shared GPU pool");
    if (!mem) return 1; memset(mem, 0, 16384); mtl_pool_add(mem, 16384);
    submit_probe job = {.mem = mem};
    for (int rep = 0; rep < 6; rep++) {
        job.rep = rep;
        if (!(rep & 1)) run_submission(&job);
        else { pthread_t submitter; int err = pthread_create(&submitter, NULL, run_submission, &job);
            if (!err) pthread_join(submitter, NULL);
            CHECK(!err, "start alternate submitting thread");
            if (err) run_submission(&job);
        }
    }
    rx_bind_serial();
    check_allocator_wrap(mem);
    check_query_handoff();
    mtl_pool_del(mem); mtl_drain(); free(mem);
    printf("parallel worker: %d checks, %d failures\n", checks, failed); return failed != 0;
} }
