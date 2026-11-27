// The host services hle_net.c and hle_net_backend.c call, for tests that link it without the thread and fault layers (hle_thread.c, crash.c)
#include <stddef.h>
void dm_thread_native_wait(int entering) { (void)entering; }
void dm_guest_write_prepare(void *addr, size_t len) { (void)addr; (void)len; }
void *guest_sym(const char *name) { (void)name; return NULL; }   // a game without __errno_location: the host keeps the only errno
