// The kernel's NTP discipline is a read-only source for the network clock. A
// user/fixed clock is not evidence of synchronization. No daemon, system clock
// setting, entitlement or trust configuration is changed here.
#include "hle.h"
#include <errno.h>
#include <pthread.h>
#include <sys/timex.h>

// Contended service locks release the caller's guest core, including during
// SDK callbacks below. HLE_FORCE_WAIT cannot be used around those callbacks.
void dm_host_lock(pthread_mutex_t *mutex);

// PosixTime has whole-second resolution. Require both kernel error estimates
// to fit within one second; reject unsynchronized/leap/error states as well.
// XNU grows maxerror as its synchronization ages, so stale precision fails too.
#define NETWORK_CLOCK_ERROR_US 1000000L
static Result network_clock_read(int64_t *out) {
    if (!out) return RES_FAIL;
    struct ntptimeval value = {0};
    int saved_errno = errno;
    int status = ntp_gettime(&value);
    errno = saved_errno;
    if (status != TIME_OK || value.time_state != TIME_OK || value.time.tv_sec <= 0 ||
        value.time.tv_nsec < 0 || value.time.tv_nsec >= 1000000000L ||
        value.maxerror < 0 || value.esterror < 0 ||
        value.maxerror > NETWORK_CLOCK_ERROR_US || value.esterror > NETWORK_CLOCK_ERROR_US) return RES_FAIL;
    *out = (int64_t)value.time.tv_sec;
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "network clock from the synchronized kernel NTP discipline with checked precision", Result, h_time_NetNow, int64_t *out) {
    return network_clock_read(out);
}
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", "accuracy follows the same kernel NTP state and error bounds as the network time getter", unsigned, h_time_NetAccurate, void) {
    int64_t value;
    return network_clock_read(&value) == RES_OK;
}

// SDK task sizes differ, so the guest object is an opaque registry key. Each
// task owns a separate SDK-created SystemEvent. All task operations serialize;
// the kernel query completes inline, without a made-up delay or network request.
typedef struct network_clock_task {
    const void *object;
    struct network_clock_task *next;
    _Alignas(16) uint8_t event[0x100];
    uint64_t binding;
    Result result;
    int initialized, started, mode;
} network_clock_task;
static pthread_mutex_t clock_tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static network_clock_task *clock_tasks;
static unsigned clock_task_count;
#define NETWORK_CLOCK_MAX_TASKS 256

static network_clock_task *clock_task_find(const void *object) {
    for (network_clock_task *task = clock_tasks; task; task = task->next) if (task->object == object) return task;
    return NULL;
}
static network_clock_task *clock_task_remove(const void *object) {
    network_clock_task **link = &clock_tasks;
    while (*link && (*link)->object != object) link = &(*link)->next;
    network_clock_task *task = *link;
    if (task) { *link = task->next; clock_task_count--; }
    return task;
}
static void clock_task_release(network_clock_task *task) {
    if (!task) return;
    if (task->initialized) {
        // Match the SDK that created the event; DestroySystemEvent closes its
        // handles and removes the host binding before storage can be reused.
        void (*destroy)(void *) = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        destroy(task->event);
    }
    free(task);
}
static void clock_task_construct(void *object) {
    if (!object) return;
    dm_host_lock(&clock_tasks_lock);
    network_clock_task *old = clock_task_remove(object);
    if (clock_task_count < NETWORK_CLOCK_MAX_TASKS) {
        network_clock_task *task = calloc(1, sizeof *task);
        if (task) { task->object = object; task->result = RES_FAIL; task->next = clock_tasks; clock_tasks = task; clock_task_count++; }
    }
    pthread_mutex_unlock(&clock_tasks_lock);
    clock_task_release(old);
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "owned network clock task and completion event", void, h_time_NcCtor, void *self) { clock_task_construct(self); }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC2Ev", "owned network clock task and completion event, base constructor", void, h_time_NcCtor2, void *self) { clock_task_construct(self); }
static void clock_task_destroy(void *object) {
    dm_host_lock(&clock_tasks_lock);
    network_clock_task *task = clock_task_remove(object);
    pthread_mutex_unlock(&clock_tasks_lock);
    clock_task_release(task);
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "destroy only this task's SDK event and registry lifetime", void, h_time_NcDtor, void *self) { clock_task_destroy(self); }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD2Ev", "destroy only this task's SDK event, base destructor", void, h_time_NcDtor2, void *self) { clock_task_destroy(self); }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "initialize an unsignaled per-task SDK event with the requested clear mode", Result, h_time_NcInit, void *self, int mode) {
    if (mode != 0 && mode != 1) return RES_FAIL;
    dm_host_lock(&clock_tasks_lock);
    network_clock_task *task = clock_task_find(self);
    Result result = RES_FAIL;
    if (task) {
        if (!task->initialized) {
            hle_bind_system_event(task->event, mode);
            task->binding = hle_system_event_binding(task->event);
            task->initialized = task->binding != 0;
            task->mode = mode;
        }
        if (task->initialized && task->mode == mode) result = RES_OK;
    }
    pthread_mutex_unlock(&clock_tasks_lock);
    return result;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "complete after an actual kernel synchronization/precision query, retaining its real result", Result, h_time_NcStart, void *self) {
    dm_host_lock(&clock_tasks_lock);
    network_clock_task *task = clock_task_find(self);
    Result result = RES_FAIL;
    if (task && task->initialized) {
        hle_clear_system_event(task->event);
        int64_t value;
        task->result = network_clock_read(&value);
        task->started = 1;
        // The captured binding identifies this event lifetime, not its address.
        if (hle_signal_system_event_binding(task->binding)) result = RES_OK;
        else task->result = RES_FAIL;
    }
    pthread_mutex_unlock(&clock_tasks_lock);
    return result;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "return this task's event without changing its completion state", void *, h_time_NcEvent, void *self) {
    dm_host_lock(&clock_tasks_lock);
    network_clock_task *task = clock_task_find(self);
    void *event = task && task->initialized ? task->event : NULL;
    pthread_mutex_unlock(&clock_tasks_lock);
    return event;
}
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "actual completed kernel query result; failure before starting or after destruction", Result, h_time_NcResult, void *self) {
    dm_host_lock(&clock_tasks_lock);
    network_clock_task *task = clock_task_find(self);
    Result result = task && task->initialized && task->started ? task->result : RES_FAIL;
    pthread_mutex_unlock(&clock_tasks_lock);
    return result;
}
