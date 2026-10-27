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
static int opens, starts, joins, closes, fail_open, nested;
static int event_destroys, event_binds;
static void *sdk_event;
static void *sdk_work;
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
    assert(ev);
    sdk_event = ev;
    return nested ? h_ar_Open0(handle, param, work, size) : sdk_open(handle, param, work, size);
}
static void sdk_destroy_event(void *ev) { assert(ev == sdk_event); event_destroys++; }
static void sdk_close(void *obj, void *state) {
    assert(obj == sdk_work && state == (uint8_t *)sdk_work + 0x80);
    assert(((uint8_t *)obj)[SDK_WORK - 1] == 0x5a);   // storage is alive through SDK teardown
    closes++;
}
// A pre-SdkMutex SDK: Update adds its OS-mutex offset to this before calling
// LockMutex. The bytecode is inspected, never executed.
static uint32_t old_update[24];
static uint32_t old_impl[24];
static void *expected_sys;
static uint8_t empty_commands[8];
static void *expected_mutex;
static int held, old_locks, old_unlocks, old_processes, sdk_mutex_queries;
static void old_lock(void *mutex) { assert(mutex == expected_mutex && !held); held = 1; old_locks++; }
static void old_unlock(void *mutex) { assert(mutex == expected_mutex && held); held = 0; old_unlocks++; }
static int old_manual_cpu(void *sys) { assert(sys == expected_sys); return 1; }
static uint8_t *old_command_buffer(void *sys) { (void)sys; return empty_commands; }
static size_t old_command_size(void *sys) { (void)sys; return sizeof empty_commands; }
static size_t old_generate(void *sys, void *commands, size_t size) {
    (void)sys; assert(held && commands == empty_commands && size == sizeof empty_commands); return 0;
}
static void old_time_max(void *proc, uint32_t value) { (void)proc; assert(value == UINT32_MAX); }
static void old_setup(void *proc, const void *commands, size_t size) { (void)proc; assert(!held && commands == empty_commands && size == 0); }
static void old_process(void *proc) { (void)proc; assert(!held); old_processes++; }
void *guest_sym(const char *name) {
    if (!strcmp(name, hle_force_h_ar_WorkSize.name)) return sdk_size;
    if (!strcmp(name, hle_force_h_ar_Open.name)) return sdk_open_ev;
    if (!strcmp(name, hle_force_h_ar_Open0.name)) return sdk_open;
    if (!strcmp(name, hle_force_h_ar_Close.name)) return sdk_close;
    if (!strcmp(name, hle_force_h_ar_ImplExec.name)) return old_impl;
    if (!strcmp(name, "_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm")) return old_update;
    if (!strcmp(name, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE")) return old_lock;
    if (!strcmp(name, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE")) return old_unlock;
    if (!strcmp(name, "_ZN2nn2os12SdkMutexType4LockEv")) { sdk_mutex_queries++; return NULL; }
    if (!strcmp(name, "_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv")) return old_manual_cpu;
    if (!strcmp(name, "_ZNK2nn5audio6server17AudioRenderSystem18GetRenderingDeviceEv")) return old_manual_cpu;
    if (!strcmp(name, "_ZNK2nn5audio6server17AudioRenderSystem26GetCommandBufferWorkBufferEv")) return old_command_buffer;
    if (!strcmp(name, "_ZNK2nn5audio6server17AudioRenderSystem30GetCommandBufferWorkBufferSizeEv")) return old_command_size;
    if (!strcmp(name, "_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvm")) return old_generate;
    if (!strcmp(name, "_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj")) return old_time_max;
    if (!strcmp(name, "_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm")) return old_setup;
    if (!strcmp(name, "_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv")) return old_process;
    if (!strcmp(name, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE")) return sdk_destroy_event;
    return NULL;
}
void *objmap_get(const void *key) { return key == map_key ? map_value : NULL; }
void objmap_put(const void *key, void *value) { assert(!map_key); map_key = (void *)key; map_value = value; }
void *objmap_del(const void *key) {
    if (key != map_key) return NULL;
    void *value = map_value; map_key = map_value = NULL; return value;
}
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) {
    (void)fn; (void)arg; (void)prio; (void)stack; starts++; return (void *)1;
}
void dm_guest_thread_join(void *tt) {
    assert(tt == (void *)1 && !atomic_load(&g_open));
    assert(((uint8_t *)sdk_work)[SDK_WORK - 1] == 0x5a);   // storage also survives the render thread
    joins++;
}
uint64_t now_ns(void) { return 0; }
void *dm_pcmout_open(int ch) { (void)ch; return NULL; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; return 0; }
void *dm_pcmout_released(void *k) { (void)k; return NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
void hle_bind_system_event(void *ev, int ac) {
    assert(ev == sdk_event && ac == 1 && event_destroys == event_binds + 1);
    event_binds++;
}

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
    void *handle[2], *event[8];
    for (int route = 0; route < 3; route++) {
        nested = route == 2;
        int a = allocations, s = starts, c = closes;
        Result r = route ? h_ar_Open(handle, event, param, work, SERVICE_WORK) : h_ar_Open0(handle, param, work, SERVICE_WORK);
        assert(!r && allocations == a + 1 && starts == s + 1);
        assert(handle[0] != work && map_value == backing);
        assert(((uint8_t *)work)[0] == 0xa5 && ((uint8_t *)work)[SERVICE_WORK - 1] == 0xa5);
        assert(!memcmp(param, saved, sizeof param));
        h_ar_Close(handle[0], handle[1]);
        assert(!backing && !map_key && allocations == frees && closes == c + 1 && joins == starts);
    }
    assert(event_destroys == 2 && event_binds == 2);   // event opens, including the nested route, replace the local event once
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
    free(work);
    uint8_t old_sys[0x300] = {0};
    expected_sys = old_sys;
    expected_mutex = old_sys + 0x1f8;
    old_update[0] = 0x9107e000;   // add x0, x0, #0x1f8
    intptr_t branch = (intptr_t)(void *)old_lock - (intptr_t)&old_update[1];
    assert(!(branch & 3) && branch >= -(1 << 27) && branch < (1 << 27));
    old_update[1] = 0x94000000u | ((uint32_t)(branch / 4) & 0x3ffffffu);
    assert(render_once(old_sys, NULL, NULL) == RES_OK);
    assert(old_locks == 1 && old_unlocks == 1 && old_processes == 1 && !held);
    assert(sdk_mutex_queries == 1);   // optional detection only; never required
    old_impl[0] = 0x91014013;   // add x19, x0, #0x50
    old_impl[1] = 0xaa1303e0;   // mov x0, x19
    branch = (intptr_t)(void *)old_manual_cpu - (intptr_t)&old_impl[2];
    assert(!(branch & 3) && branch >= -(1 << 27) && branch < (1 << 27));
    old_impl[2] = 0x94000000u | ((uint32_t)(branch / 4) & 0x3ffffffu);
    assert(impl_system_offset() == 0x50);
    old_impl[0] = 0x91018013;   // same call with the later +0x60 member
    assert(impl_system_offset() == 0x60);
    old_impl[0] = 0x91014013;
    uint32_t mode_call = old_impl[2];
    branch = (intptr_t)(void *)old_process - (intptr_t)&old_impl[2];
    assert(!(branch & 3) && branch >= -(1 << 27) && branch < (1 << 27));
    old_impl[2] = 0x94000000u | ((uint32_t)(branch / 4) & 0x3ffffffu);
    assert(impl_system_offset() == 0x60);   // an unrelated call proves no offset
    old_impl[2] = mode_call;
    uint32_t saved_impl[24]; memcpy(saved_impl, old_impl, sizeof old_impl);
    old_impl[0] = 0x91400413;   // add x19, x0, #1, lsl #12
    assert(impl_system_offset() == 0x1000);
    old_impl[0] = saved_impl[0]; old_impl[1] = 0xd2800013;   // mov x19, #0: invalidates the derived pointer
    old_impl[2] = saved_impl[1];
    branch = (intptr_t)(void *)old_manual_cpu - (intptr_t)&old_impl[3];
    old_impl[3] = 0x94000000u | ((uint32_t)(branch / 4) & 0x3ffffffu);
    assert(impl_system_offset() == 0x60);
    old_impl[1] = 0xb4000040;   // cbz x0, +8: do not infer across control flow
    assert(impl_system_offset() == 0x60);
    old_impl[1] = 0x2a1303f3;   // mov w19, w19: truncates the pointer before it reaches x0
    assert(impl_system_offset() == 0x60);
    // Skyrim's complete straight-line prologue, including stack saves and
    // unrelated MOVZ/MOVK instructions, still recovers the member.
    uint32_t prologue[] = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd,
                          0x91014013, 0x52a00094, 0xaa1303e0, 0x72805334};
    memcpy(old_impl, prologue, sizeof prologue);
    branch = (intptr_t)(void *)old_manual_cpu - (intptr_t)&old_impl[8];
    old_impl[8] = 0x94000000u | ((uint32_t)(branch / 4) & 0x3ffffffu);
    assert(impl_system_offset() == 0x50);
    memcpy(old_impl, saved_impl, sizeof old_impl);
    uint8_t impl[0x400] = {0}; expected_sys = impl + 0x50; expected_mutex = (uint8_t *)expected_sys + 0x1f8;
    assert(h_ar_ImplExec(impl) == RES_OK);
    assert(old_locks == 2 && old_unlocks == 2 && old_processes == 2 && !held);
    // SDK 3.4 begins with a single command whose size and ID are both20.
    // The modern walk used to accept it and permanently select the wrong layout.
    uint8_t cl[0x20 + 0x14 + 0x130] = {0}; int16_t pcm[FRAME_MAX * 2];
    int32_t mix[480]; for (int i = 0; i < 240; i++) { mix[i] = i; mix[240 + i] = -i; }
    *(uint32_t *)(cl + 4) = 1; *(int32_t **)(cl + 8) = mix;
    *(uint32_t *)(cl + 0x14) = 240; *(uint32_t *)(cl + 0x18) = 48000;
    uint8_t *cmd = cl + 0x20;
    *(uint32_t *)cmd = 0xcafebabe; *(uint16_t *)(cmd + 4) = 0x14; *(uint16_t *)(cmd + 6) = 0x14; cmd[0x10] = 1;
    assert(capture(cl, 0x34, pcm) == 0 && g_hdr6 && !g_hdr21);
    cmd += 0x14; *(uint32_t *)cmd = 0xcafebabe; *(uint16_t *)(cmd + 4) = 0x130;
    *(uint16_t *)(cmd + 6) = 0x0f; cmd[0x10] = 1;
    *(uint32_t *)(cmd + 0x120) = 2; cmd[0x124] = 0; cmd[0x125] = 1;
    *(uint32_t *)(cl + 4) = 2;
    // Candidate detection inspects a full list; the first command may repeat.
    *(uint32_t *)(cl + 4) = 5;
    uint8_t full[0x20 + 4 * 0x14 + 0x130] = {0}; memcpy(full, cl, 0x20);
    for (int i = 0; i < 4; i++) memcpy(full + 0x20 + i * 0x14, cl + 0x20, 0x14);
    memcpy(full + 0x20 + 4 * 0x14, cmd, 0x130);
    assert(capture(full, sizeof full, pcm) == 240 && g_sink4 && g_sink_id == 0x0f);
    for (int i = 0; i < 240; i++) assert(pcm[2 * i] == i && pcm[2 * i + 1] == -i);
    assert(!cmd_ok(full + sizeof full - 2, full + sizeof full));
    puts("audren work buffer: Horizon sizing fixtures, both opens, nested open, failures and close lifetime passed");
    puts("audren old SDK: OS mutex selected and rendering works with SdkMutex symbols absent");
    return 0;
}
