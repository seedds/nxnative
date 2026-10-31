// Guest threads for the corpus (rt/thread.c, rt/thread.S): Horizon's thread and synchronization SVCs as libnx's wrappers call them (the minimal
// set a self-checking threaded program needs: CreateThread, StartThread, ExitThread, SleepThread, CloseHandle, WaitSynchronization on a thread,
// ArbitrateLock / ArbitrateUnlock behind a libnx-style mutex, WaitForAddress / SignalToAddress), a thread object with its own stack and TLS
// block, and the register-invariant loop of rt/pause.S that the pause test checks (docs/design/aot-recompiler.md section 6.2).
#ifndef NXAOT_CORPUS_THREAD_H
#define NXAOT_CORPUS_THREAD_H

#include "rt.h"

#ifdef __cplusplus
extern "C" {
#endif

// The SVCs (rt/thread.S), with libnx's signatures; the results are Horizon result codes (0 = success)
uint32_t rt_svc_create_thread(uint32_t *handle, void (*entry)(void *), void *arg, void *stack_top, int32_t priority, int32_t core);
uint32_t rt_svc_start_thread(uint32_t handle);
__attribute__((noreturn)) void rt_svc_exit_thread(void);
void rt_svc_sleep_thread(int64_t nanoseconds);
uint32_t rt_svc_close_handle(uint32_t handle);
uint32_t rt_svc_wait_synchronization(int32_t *index, const uint32_t *handles, int32_t count, int64_t timeout);
uint32_t rt_svc_arbitrate_lock(uint32_t owner, volatile uint32_t *tag_location, uint32_t self_tag);
uint32_t rt_svc_arbitrate_unlock(volatile uint32_t *tag_location);
uint32_t rt_svc_wait_for_address(volatile uint32_t *address, uint32_t type, int32_t value, int64_t timeout);
uint32_t rt_svc_signal_to_address(volatile uint32_t *address, uint32_t type, int32_t value, int32_t count);

enum { RT_ARB_WAIT_IF_LESS_THAN = 0, RT_ARB_DECREMENT_AND_WAIT_IF_LESS_THAN = 1, RT_ARB_WAIT_IF_EQUAL = 2 };
enum { RT_SIGNAL = 0, RT_SIGNAL_AND_INCREMENT_IF_EQUAL = 1 };

// A thread: its handle, function, argument, stack (16-byte aligned, the caller's) and TLS block (as rt.c's for the main thread)
typedef struct rt_thread {
    uint32_t handle;
    void (*fn)(struct rt_thread *self, void *arg);
    void *arg;
    uint8_t tls[256] __attribute__((aligned(16)));
} rt_thread;

uint32_t rt_thread_start(rt_thread *t, void (*fn)(rt_thread *, void *), void *arg, void *stack, size_t stack_size);
void rt_thread_join(rt_thread *t);  // WaitSynchronization on its handle, then CloseHandle

// A mutex as libnx's: 0 free, else the owner's handle, with the waiters bit 0x40000000 when a thread waits in ArbitrateLock
void rt_mutex_lock(volatile uint32_t *m, uint32_t self);
void rt_mutex_unlock(volatile uint32_t *m, uint32_t self);

// rt/pause.S: n iterations of a loop that keeps x20 = 2 * x19 and x21 = ~x19 at its head (the label nxaot_pause_loop_head, which the host's
// pause test knows by its symbol); returns x19 at the end (n)
uint64_t nxaot_pause_loop(uint64_t n);

#ifdef __cplusplus
}
#endif

#endif
