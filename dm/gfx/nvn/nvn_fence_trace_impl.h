// Included after the frame/fence state is declared. CPU and GPU clocks stay separate.
static ft_buffer g_fence_trace_buffer;
static pthread_once_t g_ft_once = PTHREAD_ONCE_INIT;
static _Atomic int g_ft_state, g_ft_clock;
static _Atomic uint64_t g_ft_next_sync;
static uint64_t g_ft_first, g_ft_last;
static char g_ft_path[1024];
static _Thread_local uint64_t g_ft_sync, g_ft_sync_start;
static void ft_configure(void) {
    const char *e = getenv("DM_NVN_FENCE_TRACE");
    unsigned long long first, last;
    if (!e) return;
    if (sscanf(e, "%llu-%llu:%1023[^\n]", &first, &last, g_ft_path) != 3 || last < first) {
        MLOG("[RED] DM_NVN_FENCE_TRACE requires first-last:path"); return;
    }
    const size_t capacity = 262144;
    ft_event *events = calloc(capacity, sizeof *events);
    if (!events) { MLOG("[RED] fence trace allocation failed"); return; }
    ft_init(&g_fence_trace_buffer, events, capacity); g_ft_first = first; g_ft_last = last;
    atomic_store(&g_ft_state, 1);
}
static int ft_enabled(void) {
    pthread_once(&g_ft_once, ft_configure);
    return atomic_load_explicit(&g_ft_state, memory_order_relaxed) == 1;
}
static int ft_selected(void) {
    if (!ft_enabled()) return 0;
    uint64_t f = atomic_load(&g_cur_frame); return f >= g_ft_first && f <= g_ft_last;
}
static void ft_emit(uint32_t kind, uint64_t sync, uint64_t fence, uint32_t site, uint64_t value, double gpu0, double gpu1) {
    if (!ft_enabled()) return;
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    ft_event e = {.cpu_ns=ws_t0(), .frame=atomic_load(&g_cur_frame), .thread=tid,
        .sync=sync, .fence=fence, .latest=atomic_load(&g_fence), .done=atomic_load(&g_done),
        .value=value, .gpu_begin_s=gpu0, .gpu_end_s=gpu1, .kind=kind, .site=site};
    ft_append(&g_fence_trace_buffer, &e);
}
static void ft_commit_edge(uint64_t fence, int finish) {
    if (fence) ft_emit(finish ? FT_COMMIT_END : FT_COMMIT_BEGIN, 0, fence, 0, 0, 0, 0);
}
uint64_t mtl_sync_trace_begin(uint64_t fence, uint64_t timeout, int fenced) {
    if (!ft_selected()) return 0;
    g_ft_sync_start = ws_t0();
    uint64_t id = atomic_fetch_add(&g_ft_next_sync, 1) + 1; g_ft_sync = id;
    ft_emit(FT_SYNC_BEGIN, id, fence, (uint32_t)fenced, timeout, 0, 0); return id;
}
uint64_t mtl_sync_trace_remaining(uint64_t id, uint64_t timeout) {
    if (!id || timeout == UINT64_MAX || !timeout) return timeout;
    uint64_t elapsed = ws_t0() - g_ft_sync_start;
    return elapsed < timeout ? timeout - elapsed : 0;
}
void mtl_sync_trace_end(uint64_t id, uint64_t fence, int result) {
    if (!id) return;
    ft_emit(FT_SYNC_END, id, fence, 0, (uint64_t)result, 0, 0); g_ft_sync = 0;
}
static void ft_wait_event(uint32_t kind, uint64_t fence, int site, uint64_t value) {
    if (g_ft_sync) ft_emit(kind, g_ft_sync, fence, (uint32_t)site, value, 0, 0);
}
static void ft_frame(uint64_t frame) {
    if (!ft_enabled() || frame < g_ft_first) return;
    if (!atomic_exchange(&g_ft_clock, 1)) {
        // Bracket the CA clock sample in CPU raw uptime. It is calibration evidence,
        // not permission to subtract GPU timestamps without checking their domain.
        uint64_t a = ws_t0(); double ca = CACurrentMediaTime(); uint64_t b = ws_t0();
        ft_event e = {.kind=FT_CLOCK_PAIR,.frame=frame,.cpu_ns=a,.value=b,.gpu_begin_s=ca};
        ft_append(&g_fence_trace_buffer, &e);
    }
    if (frame <= g_ft_last) return;
    int expected = 1;
    if (!atomic_compare_exchange_strong(&g_ft_state, &expected, 2)) return;
    ft_cutoff cutoff = ft_close(&g_fence_trace_buffer); uint64_t cutoff_ns = ws_t0();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        char tmp[1056]; snprintf(tmp, sizeof tmp, "%s.tmp", g_ft_path);
        FILE *f = fopen(tmp, "w"); if (!f) { MLOG("[RED] fence trace open failed: %s", tmp); return; }
        int bad = fprintf(f, "# fence_trace_v1 CPU=CLOCK_UPTIME_RAW_ns GPU=Metal_seconds; cutoff=%llu first=%llu last=%llu records=%zu dropped=%llu; cutoff is NOT completion, missing joins remain unknown\n",
            (unsigned long long)cutoff_ns, (unsigned long long)g_ft_first, (unsigned long long)g_ft_last, cutoff.used, (unsigned long long)cutoff.dropped) < 0;
        bad |= fprintf(f, "kind\tcpu_ns\tframe\tthread\tsync\tfence\tlatest\tdone\tsite\tvalue\tgpu_begin_s\tgpu_end_s\n") < 0;
        for (size_t i=0; i<cutoff.used; i++) {
            ft_event e = g_fence_trace_buffer.events[i];
            bad |= fprintf(f, "%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%u\t%llu\t%.9f\t%.9f\n", e.kind,
                (unsigned long long)e.cpu_ns, (unsigned long long)e.frame, (unsigned long long)e.thread,
                (unsigned long long)e.sync, (unsigned long long)e.fence, (unsigned long long)e.latest,
                (unsigned long long)e.done, e.site, (unsigned long long)e.value, e.gpu_begin_s, e.gpu_end_s) < 0;
        }
        bad |= fclose(f) != 0;
        if (!bad) bad = rename(tmp, g_ft_path) != 0;
        MLOG("%sfence trace cutoff exported %zu events, dropped %llu -> %s", bad ? "[RED] " : "", cutoff.used, (unsigned long long)cutoff.dropped, g_ft_path);
    });
}
