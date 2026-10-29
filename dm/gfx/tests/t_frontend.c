// The NVN frontend linked over the null backend (dm/gfx/nvn/nvn_null.c; docs/design/vulkan-backend.md, step M6) on the hosts without Metal: the interface layer's entry points
// called as the C functions they are without native execution (h_<name>, external there: hle.h HLE_LINKAGE), a device, a queue and a memory pool made, a command list of a few
// records recorded into the pool and submitted, the frontend's decoder (nvn_replay_exec) handing its records to the null backend, which says what it does not do and completes the
// submission at once. It checks the link and the call path, not drawing: which backend functions were reached, the pool table through the backend, the fence of a submission, the
// CPU-visible effect of an event the command list signals, and that nothing crashed. dm/host/CMakeLists.txt builds it (CTest; the Windows runners read host_tests.tsv); on Apple
// nvn_be.h's functions are the Metal backend's, so there is no Mac build of it
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_protos.h"
#include "../nvn/nvn_pools.h"
#include "../../host/dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The entry points under test, declared with their own types: each NVNT_<name> (nvn_protos.h) is the type its h_<name> is checked against in hle_nvn.c
#define DECL(name) extern __typeof__(*(NVNT_##name)0) h_##name
DECL(nvnDeviceBuilderSetDefaults); DECL(nvnDeviceBuilderSetFlags); DECL(nvnDeviceInitialize); DECL(nvnDeviceGetInteger); DECL(nvnDeviceFinalize);
DECL(nvnQueueBuilderSetDevice); DECL(nvnQueueBuilderSetDefaults); DECL(nvnQueueInitialize); DECL(nvnQueueSubmitCommands); DECL(nvnQueueFenceSync); DECL(nvnQueueFinalize);
DECL(nvnMemoryPoolBuilderSetDefaults); DECL(nvnMemoryPoolBuilderSetDevice); DECL(nvnMemoryPoolBuilderSetStorage); DECL(nvnMemoryPoolBuilderSetFlags);
DECL(nvnMemoryPoolInitialize); DECL(nvnMemoryPoolFinalize); DECL(nvnMemoryPoolGetBufferAddress);
DECL(nvnCommandBufferInitialize); DECL(nvnCommandBufferAddCommandMemory); DECL(nvnCommandBufferBeginRecording); DECL(nvnCommandBufferEndRecording);
DECL(nvnCommandBufferSetScissor); DECL(nvnCommandBufferClearColor); DECL(nvnCommandBufferDrawArrays); DECL(nvnCommandBufferSignalEvent); DECL(nvnCommandBufferFinalize);
DECL(nvnEventBuilderSetDefaults); DECL(nvnEventBuilderSetStorage); DECL(nvnEventInitialize); DECL(nvnEventGetValue);
DECL(nvnSyncInitialize); DECL(nvnSyncWait);
void *h_nvnBootstrapLoader(const char *name);   // hle_nvn.c's HLE_FORCE entry
const void *plat_table_swnvn(size_t *bytes);    // hle_nvn.c's registration table (PLAT_TABLE_DEFINE)
// What the null backend was asked to do (nvn_null.c)
int nvn_null_reached(const char *fn);
int nvn_null_functions(void);
uint64_t nvn_null_calls(void);

// ---- What the frontend needs of the host, which this test does not link (the stubs other host tests use, e.g. dm/host/tests/t_fs.c) ----
uint64_t g_t0_ns;
uint64_t now_ns(void) { return plat_now_ns(); }
uint64_t guest_ns(void) { return plat_now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
int front_screen_max_fps(void) { return 60; }
static int (*g_fault_hook)(void *, int, void *);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_fault_hook = fn; }   // nvn_dirty.c registers its page-fault hook at the first pool
static unsigned g_preps;
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { (void)fn; }
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; g_preps++; }   // the null backend calls it before it writes an event's value

static int g_checks, g_failed;
static void check(const char *what, long long want, long long got) {
    g_checks++;
    if (want != got) { g_failed++; printf("  %-64s expected %lld, got %lld FAILED\n", what, want, got); }
}
#define REACHED(fn) check("null backend reached " #fn, 1, nvn_null_reached(#fn))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // The registration table and the name table: the constructor (nvn_nops_register) matched every implementation to a name, or the program would have stopped before main
    size_t tb = 0; const void *tab = plat_table_swnvn(&tb);
    check("registration table present", 1, tab != NULL);
    check("registration table holds whole entries", 0, (long long)(tb % (2 * sizeof(void *))));
    check("registration table: more than 400 implementations", 1, tb / (2 * sizeof(void *)) > 400);
    printf("NVN implementations registered: %zu\n", tb / (2 * sizeof(void *)));
    check("no guest entry points without native execution: nvnBootstrapLoader answers NULL", 1, h_nvnBootstrapLoader("nvnDeviceGetProcAddress") == NULL);

    // Device and queue
    NVNdeviceBuilder db; NVNdevice dev; h_nvnDeviceBuilderSetDefaults(&db); h_nvnDeviceBuilderSetFlags(&db, 0);
    check("DeviceInitialize", 1, h_nvnDeviceInitialize(&dev, &db));
    int v = 0; h_nvnDeviceGetInteger(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v); check("uniform buffer alignment", 256, v);
    REACHED(be_set_origin_lower_left); REACHED(be_set_depth_zero);
    NVNqueueBuilder qb; NVNqueue q; h_nvnQueueBuilderSetDevice(&qb, &dev); h_nvnQueueBuilderSetDefaults(&qb);
    check("QueueInitialize", 1, h_nvnQueueInitialize(&q, &qb));

    // A memory pool, through the backend into the shared pool table (nvn_pools.c)
    enum { POOL = 1 << 16 };
    uint8_t *mem = plat_vm_alloc(POOL); check("pool storage", 1, mem != NULL); if (!mem) return 1;
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    h_nvnMemoryPoolBuilderSetDefaults(&pb); h_nvnMemoryPoolBuilderSetDevice(&pb, &dev); h_nvnMemoryPoolBuilderSetStorage(&pb, mem, POOL);
    h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    check("MemoryPoolInitialize", 1, h_nvnMemoryPoolInitialize(&pool, &pb));
    REACHED(be_pool_add); check("pool table: one pool", 1, nvn_pool_count());
    check("pool's buffer address is its storage", (long long)(uintptr_t)mem, (long long)h_nvnMemoryPoolGetBufferAddress(&pool));
    check("dirty-page tracker registered its fault hook", 1, g_fault_hook != NULL);

    // An event in the pool, and a command list recorded into the pool's first 16 KB: a scissor (a state record), a clear, a draw, the event's signal
    NVNeventBuilder eb; NVNevent ev; h_nvnEventBuilderSetDefaults(&eb); h_nvnEventBuilderSetStorage(&eb, &pool, 0x8000);
    check("EventInitialize", 1, h_nvnEventInitialize(&ev, &eb)); check("event starts at 0", 0, h_nvnEventGetValue(&ev));
    NVNcommandBuffer cb; check("CommandBufferInitialize", 1, h_nvnCommandBufferInitialize(&cb, &dev));
    h_nvnCommandBufferAddCommandMemory(&cb, &pool, 0, 0x4000);
    h_nvnCommandBufferBeginRecording(&cb);
    h_nvnCommandBufferSetScissor(&cb, 0, 0, 16, 16);
    float rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f}; h_nvnCommandBufferClearColor(&cb, 0, rgba, 0xf);
    h_nvnCommandBufferDrawArrays(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    h_nvnCommandBufferSignalEvent(&cb, &ev, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_TOP, 0, 7);
    NVNcommandHandle hd = h_nvnCommandBufferEndRecording(&cb);
    check("the handle is the first record, in the pool", (long long)(uintptr_t)mem, (long long)hd);

    // Submitted: the decoder walks the list and hands every record to the null backend; the submission's fence is complete at once
    h_nvnQueueSubmitCommands(&q, 1, &hd);
    REACHED(be_queue_activate); REACHED(be_sub_begin); REACHED(be_exec); REACHED(be_sub_commit);
    REACHED(be_list_begin); REACHED(be_rec_pre); REACHED(be_state_dirty); REACHED(be_clear_color); REACHED(be_draw); REACHED(be_signal_event); REACHED(be_end);
    check("not reached: no texture copy was recorded", 0, nvn_null_reached("be_copy_t2t"));
    check("the event holds the signalled value", 7, h_nvnEventGetValue(&ev));
    check("the host's pre-write hook ran before the event was written", 1, g_preps >= 1);
    NVNsync sync; check("SyncInitialize", 1, h_nvnSyncInitialize(&sync, &dev));
    h_nvnQueueFenceSync(&q, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    check("the fence of the submission is complete", NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED, h_nvnSyncWait(&sync, 0));
    REACHED(be_last_fence); REACHED(be_wait);

    // A second submission of the same list: another fence, the event written again
    *(volatile uint32_t *)(mem + 0x8000) = 0;
    h_nvnQueueSubmitCommands(&q, 1, &hd);
    check("resubmitted list signals again", 7, h_nvnEventGetValue(&ev));

    // The pool goes: out of the table
    h_nvnCommandBufferFinalize(&cb);
    h_nvnMemoryPoolFinalize(&pool);
    REACHED(be_pool_del); check("pool table: empty again", 0, nvn_pool_count());
    h_nvnQueueFinalize(&q); REACHED(be_queue_forget);
    h_nvnDeviceFinalize(&dev);
    plat_vm_release(mem, POOL);

    printf("null backend: %d functions reached (one [RED] line each), %llu calls\n", nvn_null_functions(), (unsigned long long)nvn_null_calls());
    check("each function reached said so once: more than 15 red lines", 1, nvn_null_functions() > 15);
    printf("== t_frontend: %d checks, %d failed ==\n", g_checks, g_failed);
    return g_failed != 0;
}
