// Exercise the public size -> caller allocation -> both SDK open routes -> close boundary.
// The service requirement must not be inflated by the local backend's different command representation.
#include "../hle.h"
#include <assert.h>
#include <errno.h>

static void *backing;
static size_t backing_size;
static int allocations, frees, fail_alloc;
static int work_alloc(void **out, size_t align, size_t size) {
    if (fail_alloc) return ENOMEM;
    int r = posix_memalign(out, align, size);
    if (!r) { assert(!backing); backing = *out; backing_size = size; allocations++; }
    return r;
}
static void work_free(void *p) {
    if (p) { assert(p == backing); backing = NULL; backing_size = 0; frees++; }
    free(p);
}
#define posix_memalign work_alloc
#define free work_free
#include "../hle_audren.c"
#undef posix_memalign
#undef free

enum { SDK_WORK = 0x760000, SERVICE_WORK = 0x61d000 };
static int opens, starts, joins, closes, fail_open, nested, multi_open, expect_game_event;
static int event_destroys, event_binds, attaches, event_in_handle;
static int sdk_updates, sdk_impl_execs, sdk_ars_execs;
static void *sdk_event, *game_event;
static void *sdk_work;
static int acquire_during_destroy;
static uint64_t notify_binding; static int notify_signals;
static char switch_event[64]; static uint64_t switch_binding; static int switch_signals;
static char notify_event[64];   // the output device notification's event (SDK 21+ names)
static int notify_binds, notify_destroys;
static void *map_key, *map_value;
static size_t sdk_size(const uint8_t *param) {
    assert(*(int32_t *)(param + P_DEVICE) == 1 && *(int32_t *)(param + P_MODE) == 1);
    assert((*(uint32_t *)(param + 0x34) & 0xffffff) == 0x564552 || (*(uint32_t *)(param + 0x38) & 0xffffff) == 0x564552);
    return SDK_WORK;
}
static Result sdk_open(void **handle, const uint8_t *param, void *work, size_t size) {
    opens++;
    size_t need = h_ar_WorkSize(param);   // SDK opens can query the already converted parameter through the PLT
    assert(need == SDK_WORK);
    if (!work || ((uintptr_t)work & (WORK_PAGE - 1)) || size < need) return RES_FAIL;
    if (fail_open) return RES_FAIL;
    assert(!backing || (work == backing && size == backing_size));
    sdk_work = work;
    memset(work, 0x5a, need);   // touch the entire area, not just the handle
    handle[0] = work; handle[1] = (uint8_t *)work + 0x80;
    return RES_OK;
}
static Result sdk_open_ev(void **handle, void *ev, const uint8_t *param, void *work, size_t size) {
    if (expect_game_event)
        assert(ev == game_event);   // explicit CPU + manual keeps the game's own event, as on Horizon
    else
        assert(ev && ev != game_event);   // the SDK's CPU / manual open keeps this object and signals it on every render: the renderer's, never the game's
    sdk_event = ev;
    Result r = nested ? h_ar_Open0(handle, param, work, size) : sdk_open(handle, param, work, size);
    if (!r && event_in_handle) handle[1] = ev;   // SDK 3 to 12 keep the event in the handle (SDK 3 sdk+0x17dffc, SDK 12 sdk+0x4d274); SDK 18 keeps it at *handle[1]
    return r;
}
static Result sdk_update(void *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    (void)sys; (void)o; (void)on; (void)p; (void)pn; (void)in; (void)inn;
    sdk_updates++; return RES_OK;
}
static Result sdk_impl_exec(void *impl) { (void)impl; sdk_impl_execs++; return RES_OK; }
static Result sdk_ars_exec(void *sys) { (void)sys; sdk_ars_execs++; return RES_OK; }
static void sdk_destroy_event(void *ev) {
    if (ev == notify_event) {
        if (acquire_during_destroy) assert(h_ad_Notify21(notify_event) == 0x499);
        notify_destroys++; notify_binding = 0; return;
    }
    assert(ev == sdk_event); event_destroys++;
}
static uint32_t sdk_readable(const void *ev) { assert(ev == sdk_event && event_binds); return 0x1002; }
static void sdk_attach(void *ev, uint32_t h, int managed, int clear) {   // the game's object gets the copied read end, managed by the game, auto-clear: what the DSP open attaches
    assert(ev == game_event && h == 0x1003 && managed == 1 && clear == 1);
    attaches++;
}
static void sdk_close(void *obj, void *state) {
    if (!multi_open) assert(obj == sdk_work && state == (event_in_handle ? sdk_event : (uint8_t *)sdk_work + 0x80));
    assert(((uint8_t *)obj)[SDK_WORK - 1] == 0x5a);   // storage is alive through SDK teardown
    closes++;
}
void *guest_sym(const char *name) {
    if (!strcmp(name, hle_force_h_ar_WorkSize.name)) return sdk_size;
    if (!strcmp(name, hle_force_h_ar_Open.name)) return sdk_open_ev;
    if (!strcmp(name, hle_force_h_ar_Open0.name)) return sdk_open;
    if (!strcmp(name, hle_force_h_ar_Close.name)) return sdk_close;
    if (!strcmp(name, hle_force_h_ars_Update.name)) return sdk_update;
    if (!strcmp(name, hle_force_h_ar_ImplExec.name)) return sdk_impl_exec;
    if (!strcmp(name, hle_force_h_ars_Exec.name)) return sdk_ars_exec;
    if (!strcmp(name, "_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE")) return sdk_close;   // an SDK with the renderer in the process (from SDK 3); never called here
    if (!strcmp(name, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE")) return sdk_destroy_event;
    if (!strcmp(name, "_ZN2nn2os30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return sdk_readable;
    if (!strcmp(name, "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE")) return sdk_attach;
    return NULL;
}
void *objmap_get(const void *key) { return key == map_key ? map_value : NULL; }
void objmap_put(const void *key, void *value) { assert(!map_key); map_key = (void *)key; map_value = value; }
void *objmap_del(const void *key) {
    if (key != map_key) return NULL;
    void *value = map_value; map_key = map_value = NULL; return value;
}
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) {
    (void)fn; (void)prio; (void)stack; starts++; return arg;
}
void dm_guest_thread_join(void *tt) {
    ar_t *a = tt;
    assert(a >= g_ar && a < g_ar + AR_MAX && !atomic_load(&a->open));
    assert(((uint8_t *)a->handle[0])[SDK_WORK - 1] == 0x5a);   // storage also survives the render thread
    joins++;
}
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
uint64_t now_ns(void) { return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *dm_pcmout_open_sink(void) { return NULL; }
int dm_audio_output_channels(void) { return 2; }   // the renderer tests here take the stereo route
int dm_audio_route(void) { return 2; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; return 0; }
void *dm_pcmout_released(void *k) { (void)k; return NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
void hle_bind_system_event(void *ev, int ac) {
    if (ev == notify_event) { assert(ac == 0); notify_binds++; notify_binding = 0x7000 + (unsigned)notify_binds; return; }
    if (ev == switch_event) { assert(ac == 0); switch_binding++; return; }
    assert(ev == sdk_event && ac == 1 && event_destroys == 2 * event_binds + 1);   // after the open destroys the SDK's local event (each earlier Close destroyed one more)
    event_binds++;
}
static int event_clears;
static void *last_cleared_ev;
void hle_clear_system_event(void *ev) { event_clears++; last_cleared_ev = ev; }
uint32_t hle_dup_readable_handle(uint32_t h) { assert(h == 0x1002); return 0x1003; }

int main(void) {
    uint8_t param[P_SIZE] = {0}, saved[P_SIZE];
    const uint32_t words[] = {48000, 240, 132, 19, 138, 1, 16, 0, 0, 85, 1800, 0, 0, 0x38564552};
    memcpy(param, words, sizeof words);
    memcpy(saved, param, sizeof param);
    // Expected results were obtained by executing Horizon audio build 3a75b4a6's sizing routine,
    // independently of this implementation. A changed count must change the service requirement.
    const struct { int revision, word, value; size_t expected; } cases[] = {
        {8, 4, 138, 0x61d000}, {8, 4, 137, 0x61b000}, {8, 3, 18, 0x5f7000},
        {8, 10, 1799, 0x61c000}, {8, 7, 2, 0x622000}, {9, 4, 138, 0x61e000},
        {12, 4, 138, 0x5f7000}, {14, 4, 138, 0x621000}, {15, 4, 138, 0x6a4000}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        memcpy(param, saved, sizeof param);
        *(uint32_t *)(param + 0x34) = 0x30564552 + ((uint32_t)cases[i].revision << 24);
        ((uint32_t *)param)[cases[i].word] = cases[i].value;
        size_t got = h_ar_WorkSize(param);
        if (got != cases[i].expected) { fprintf(stderr, "service size case %zu: expected %#zx got %#zx\n", i, cases[i].expected, got); return 1; }
    }
    memcpy(param, saved, sizeof param);
    *(uint32_t *)(param + 0x34) = 0x34564552;
    *(uint32_t *)(param + 0x24) = *(uint32_t *)(param + 0x28) = 0;
    assert(h_ar_WorkSize(param) == 0x308000);   // fixed command buffer and performance format 1
    // The longer SDK 8 public layout, padding bytes, and unknown future revisions.
    memcpy(param, saved, sizeof param);
    *(uint32_t *)(param + 0x34) = 0;
    *(uint32_t *)(param + 0x38) = 0x38564552;
    param[0x21] = param[0x22] = param[0x23] = 0xff;
    assert(h_ar_WorkSize(param) == SERVICE_WORK);
    memcpy(param, saved, sizeof param);
    *(uint32_t *)(param + 0x34) = 0x40564552;
    assert(h_ar_WorkSize(param) == SDK_WORK);
    memcpy(param, saved, sizeof param);
    assert(h_ar_WorkSize(param) == SERVICE_WORK);
    void *work; assert(!posix_memalign(&work, WORK_PAGE, SDK_WORK));
    memset(work, 0xa5, SDK_WORK);
    void *handle[2], *event[8]; game_event = event;
    for (int route = 0; route < 4; route++) {   // no event, event, nested open, event kept in the handle (SDK 3 to 12)
        nested = route == 2; event_in_handle = route == 3;
        int a = allocations, s = starts, c = closes;
        Result r = route ? h_ar_Open(handle, event, param, work, SERVICE_WORK) : h_ar_Open0(handle, param, work, SERVICE_WORK);
        assert(!r && allocations == a + 1 && starts == s + 1);
        assert(handle[0] != work && map_value == backing);
        assert(((uint8_t *)work)[0] == 0xa5 && ((uint8_t *)work)[SERVICE_WORK - 1] == 0xa5);
        assert(!memcmp(param, saved, sizeof param));
        if (route) {
            assert((uint8_t *)sdk_event >= (uint8_t *)g_rev && (uint8_t *)sdk_event < (uint8_t *)(g_rev + 4));
            assert(g_ar[0].handle[1] == (event_in_handle ? sdk_event : (uint8_t *)sdk_work + 0x80));
            memset(event, 0xee, sizeof event);   // the game may reuse its object at once (LEGO Star Wars: on its stack; Little Nightmares): nothing refers to it
        }
        int d = event_destroys;
        h_ar_Close(handle[0], handle[1]);
        assert(!backing && !map_key && allocations == frees && closes == c + 1 && joins == starts);
        assert(event_destroys == d + !!route && !rev_find(handle[0]));   // Close destroys the renderer's event and frees its slot
    }
    event_in_handle = nested = 0;
    assert(event_destroys == 6 && event_binds == 3 && attaches == 3);   // each event open replaces the SDK's local event once, attaches the game's, and Close destroys it
    fail_open = 1;
    int s = starts;
    assert(h_ar_Open0(handle, param, work, SERVICE_WORK));
    assert(!backing && !map_key && allocations == frees && starts == s);
    fail_open = 0; fail_alloc = 1;
    int o = opens;
    assert(h_ar_Open0(handle, param, work, SERVICE_WORK));
    assert(opens == o && starts == s && !backing);
    fail_alloc = 0;
    // Invalid caller storage is still rejected, without giving it a host allocation.
    int a = allocations;
    assert(h_ar_Open0(handle, param, NULL, WORK_PAGE));
    assert(h_ar_Open0(handle, param, (uint8_t *)work + 1, WORK_PAGE));
    assert(h_ar_Open0(handle, param, work, WORK_PAGE - 1));
    assert(h_ar_Open0(handle, param, work, SERVICE_WORK - WORK_PAGE));
    assert(allocations == a && starts == s);
    // An existing full-size buffer and an explicitly CPU renderer retain caller ownership.
    for (int cpu = 0; cpu < 2; cpu++) {
        *(int32_t *)(param + P_DEVICE) = cpu;
        if (cpu) assert(h_ar_WorkSize(param) == SDK_WORK);
        assert(!h_ar_Open0(handle, param, work, SDK_WORK));
        assert(handle[0] == work && allocations == a && !map_key);
        h_ar_Close(handle[0], handle[1]);
    }
    // Explicit CPU + manual renderer (device 1, mode 1): driven by the game — no host render thread, no event replacement,
    // no DeviceSink capture, no event clearing in Update, and ExecuteAudioRendererRendering forwards straight to the SDK.
    *(int32_t *)(param + P_DEVICE) = 1; *(int32_t *)(param + P_MODE) = 1;
    assert(h_ar_WorkSize(param) == SDK_WORK);
    int s_before = starts, j_before = joins, d_before = event_destroys, b_before = event_binds, at_before = attaches, cl_before = event_clears;
    expect_game_event = 1;
    assert(!h_ar_Open(handle, event, param, work, SDK_WORK));
    expect_game_event = 0;
    assert(handle[0] == work && allocations == a && !map_key);
    assert(starts == s_before && !g_ar[0].render_tt && g_ar[0].manual && !g_ar[0].ev && !g_ar[0].ev_kernel);
    assert(event_destroys == d_before && event_binds == b_before && attaches == at_before && !rev_find(handle[0]));
    uint8_t manual_impl[0x1100] = {0};   // impl_sys_off() decodes a 12-bit imm (<= 0xfff); keep manual_impl + offset in bounds under sanitizers
    uint8_t *manual_sys = manual_impl + impl_sys_off();
    for (int i = 0; i < 6; i++) assert(h_ars_Update(manual_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK);
    assert(sdk_updates == 6 && event_clears == cl_before && atomic_load(&g_ar[0].renders) == 0);
    assert(h_ar_ImplExec(manual_impl) == RES_OK && sdk_impl_execs == 1);
    assert(h_ars_Exec(manual_sys) == RES_OK && sdk_ars_execs == 1);
    assert(atomic_load(&g_ar[0].renders) == 0);
    h_ar_Close(handle[0], handle[1]);
    assert(joins == j_before && !g_ar[0].handle[0] && !g_ar[0].manual && event_destroys == d_before);
    // Coexistence: explicit CPU+manual renderer opened beside an unbound DSP renderer still binds and forwards cleanly,
    // and the DSP renderer records and clears its own bound event on Update.
    void *work2; assert(!posix_memalign(&work2, WORK_PAGE, SDK_WORK));
    void *handle2[2];
    multi_open = 1;
    assert(!h_ar_Open0(handle, param, work, SDK_WORK));   // slot 0: manual, no render thread
    assert(starts == s_before && g_ar[0].manual && !g_ar[0].sys);
    *(int32_t *)(param + P_DEVICE) = 0; *(int32_t *)(param + P_MODE) = 0;
    assert(!h_ar_Open(handle2, event, param, work2, SDK_WORK));   // slot 1: DSP with event, starts render thread
    assert(starts == s_before + 1 && !g_ar[1].manual && !g_ar[1].sys && g_ar[1].ev == sdk_event && g_ar[1].ev_kernel);
    assert(h_ars_Exec(manual_sys) == RES_OK && sdk_ars_execs == 2);
    assert(g_ar[0].sys == manual_sys && !g_ar[1].sys);
    assert(h_ars_Update(manual_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK && sdk_updates == 7 && event_clears == cl_before);
    uint8_t dsp_sys = 0;
    assert(h_ars_Update(&dsp_sys, NULL, 0, NULL, 0, NULL, 0) == RES_OK && sdk_updates == 8);
    assert(g_ar[1].sys == &dsp_sys && event_clears == cl_before + 1 && last_cleared_ev == sdk_event);
    h_ar_Close(handle[0], handle[1]);
    assert(joins == j_before && !g_ar[0].handle[0] && !g_ar[0].manual);
    h_ar_Close(handle2[0], handle2[1]);
    assert(joins == j_before + 1 && !g_ar[1].handle[0] && !g_ar[1].ev && !g_ar[1].ev_kernel);
    multi_open = 0;
    free(work2);
    free(work);
    // The output device notification: a second acquire is refused until a release, which destroys the event as the SDK's does (Nintendo Switch Sports Resort, main+0x3af119c)
    h_ad_NotifyRelease21(notify_event);   // never acquired: nothing to destroy
    assert(notify_destroys == 0);
    assert(h_ad_Notify21(notify_event) == RES_OK);
    assert(h_ad_Notify21(notify_event) == 0x499 && notify_binds == 1);   // the second acquire, before a release, is refused
    dm_audio_device_changed(); assert(notify_signals == 1);
    acquire_during_destroy = 1;
    h_ad_NotifyRelease21(notify_event);
    acquire_during_destroy = 0;
    dm_audio_device_changed(); assert(notify_signals == 1);
    assert(notify_destroys == 1);
    assert(h_ad_Notify21(notify_event) == RES_OK && notify_binds == 2);
    dm_audio_device_changed(); assert(notify_signals == 2);
    h_ad_NotifyRelease21(notify_event);
    assert(notify_destroys == 2);
    switch_binding = 0x8000; h_ad_Notify7(switch_event);
    dm_audio_device_changed(); assert(switch_signals == 1);
    // Destroy/rebind the same storage as an unrelated event: the old audio subscriber cannot signal it.
    switch_binding++;
    dm_audio_device_changed(); assert(switch_signals == 1 && notify_signals == 2);
    h_ad_Notify7(switch_event);
    dm_audio_device_changed(); assert(switch_signals == 2);
    puts("audren work buffer: Horizon sizing fixtures, both opens, nested open, failures and close lifetime passed");
    return 0;
}

uint64_t hle_system_event_binding(void *ev) { assert(ev == notify_event || ev == switch_event); return ev == notify_event ? notify_binding : switch_binding; }
int hle_signal_system_event_binding(uint64_t binding) {
    if (!binding) return 0;
    if (binding == notify_binding) { notify_signals++; return 1; }
    if (binding == switch_binding) { switch_signals++; return 1; }
    return 0;
}
