// Stubs for t_condvar: host facilities of hle_thread.c that the unit test does not use
#include "../dm.h"
struct dm_flags g_flag;
uint64_t g_t0_ns;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
// The notification scheduler test supplies a C entry for real thread SVCs.
#ifndef DM_TEST_THREAD_ENTER
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
#endif
void *guest_sym(const char *name) { (void)name; return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
uint64_t now_ns(void) { return plat_now_ns(); }   // hle_os.c's, the host's monotonic clock
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
__thread _Atomic int dm_host_work;   // referenced by hle_thread.c; in the host it is defined in hle_nvn.c
pthread_key_t g_key18;   // read by hle_thread.c's SVC entry and pause signal; in the host it is defined in loader.c (never created here: those paths do not run)
#if !DM_NATIVE_EXEC
#include "../aot.h"
// The x86-64 execution engine's side of hle_thread.c (aot_thread.c), which this test does not link: no thread here runs recompiled guest code
dm_guest_state *dm_aot_thread_begin(void) { return NULL; }
int dm_aot_thread_run(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
void dm_aot_thread_end(void) {}
void dm_aot_thread_exiting(void) {}
void dm_aot_thread_poke(dm_guest_state *st) { (void)st; }
int dm_aot_stop_status(void) { return -1; }   // the process never stops here
#endif
