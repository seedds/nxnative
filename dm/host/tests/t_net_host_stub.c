// The two host services hle_net_backend.c calls, for tests that link it without the thread and fault layers (hle_thread.c, crash.c)
#include <stddef.h>
void dm_thread_native_wait(int entering) { (void)entering; }
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }
