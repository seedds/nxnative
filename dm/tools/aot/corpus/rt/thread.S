// The thread and synchronization SVCs of rt/thread.h, as libnx's svc.s wraps them: arguments in x0..x5, the result in w0, an output in w1
// stored through the pointer the caller passed in x0 (saved on the stack across the svc). The host answers them (src/rt/host.cpp; on the Switch,
// Horizon's kernel: svcCreateThread 0x08, StartThread 0x09, ExitThread 0x0A, SleepThread 0x0B, CloseHandle 0x16, WaitSynchronization 0x18,
// ArbitrateLock 0x1A, ArbitrateUnlock 0x1B, WaitForAddress 0x34, SignalToAddress 0x35)
    .text

    .global rt_svc_create_thread
    .type   rt_svc_create_thread, %function
    .align  2
rt_svc_create_thread:                // x0 = &handle, x1 = entry, x2 = argument, x3 = stack top, w4 = priority, w5 = core
    str     x0, [sp, #-16]!
    svc     #0x8
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret
    .size   rt_svc_create_thread, . - rt_svc_create_thread

    .global rt_svc_start_thread
    .type   rt_svc_start_thread, %function
    .align  2
rt_svc_start_thread:                 // w0 = handle
    svc     #0x9
    ret
    .size   rt_svc_start_thread, . - rt_svc_start_thread

    .global rt_svc_exit_thread
    .type   rt_svc_exit_thread, %function
    .align  2
rt_svc_exit_thread:
    svc     #0xA
    brk     #0xA
    .size   rt_svc_exit_thread, . - rt_svc_exit_thread

    .global rt_svc_sleep_thread
    .type   rt_svc_sleep_thread, %function
    .align  2
rt_svc_sleep_thread:                 // x0 = nanoseconds
    svc     #0xB
    ret
    .size   rt_svc_sleep_thread, . - rt_svc_sleep_thread

    .global rt_svc_close_handle
    .type   rt_svc_close_handle, %function
    .align  2
rt_svc_close_handle:                 // w0 = handle
    svc     #0x16
    ret
    .size   rt_svc_close_handle, . - rt_svc_close_handle

    .global rt_svc_wait_synchronization
    .type   rt_svc_wait_synchronization, %function
    .align  2
rt_svc_wait_synchronization:         // x0 = &index, x1 = handles, w2 = count, x3 = timeout
    str     x0, [sp, #-16]!
    svc     #0x18
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret
    .size   rt_svc_wait_synchronization, . - rt_svc_wait_synchronization

    .global rt_svc_arbitrate_lock
    .type   rt_svc_arbitrate_lock, %function
    .align  2
rt_svc_arbitrate_lock:               // w0 = owner handle, x1 = tag location, w2 = this thread's tag
    svc     #0x1A
    ret
    .size   rt_svc_arbitrate_lock, . - rt_svc_arbitrate_lock

    .global rt_svc_arbitrate_unlock
    .type   rt_svc_arbitrate_unlock, %function
    .align  2
rt_svc_arbitrate_unlock:             // x0 = tag location
    svc     #0x1B
    ret
    .size   rt_svc_arbitrate_unlock, . - rt_svc_arbitrate_unlock

    .global rt_svc_wait_for_address
    .type   rt_svc_wait_for_address, %function
    .align  2
rt_svc_wait_for_address:             // x0 = address, w1 = arbitration type, w2 = value, x3 = timeout
    svc     #0x34
    ret
    .size   rt_svc_wait_for_address, . - rt_svc_wait_for_address

    .global rt_svc_signal_to_address
    .type   rt_svc_signal_to_address, %function
    .align  2
rt_svc_signal_to_address:            // x0 = address, w1 = signal type, w2 = value, w3 = count
    svc     #0x35
    ret
    .size   rt_svc_signal_to_address, . - rt_svc_signal_to_address
