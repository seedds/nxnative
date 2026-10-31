// Stubs for t_aot_host: host facilities of hle_svc.c, hle_thread.c and loader.c that the corpus does not reach (it imports nothing, observes nothing, and runs
// on the host clock); the test links the engine (aot_*.c) and the host's real loader, SVC dispatcher and thread layer
#include "../dm.h"
#include <stdatomic.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;   // hle_thread.c's host-work count; in the host it is defined in hle_nvn.c
uint64_t now_ns(void) { return plat_now_ns(); }   // hle_os.c's: the host's monotonic clock
uint64_t guest_ns(void) { return now_ns(); }      // the guest clock as hle_os.c gives it when nothing froze it and DM_DETCLOCK is off
int dm_detclock_enabled(void) { return 0; }
void guest_clock_read_wait(void) {}               // the app is never in the background here
uint64_t dm_entropy64(void) { return ((uint64_t)plat_random32() << 32) | plat_random32(); }
void hlecount_svc(int num) { (void)num; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param)) { (void)id; (void)fn; }   // no observation gates: the corpus has no manifest rows
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }   // no Unreal allocator
// The loader's binding of what no loaded module defines: the corpus defines everything it uses
uint64_t resolve_import(module_t *m, const char *name) { die("t_aot_host: %s imports %s, which the corpus never does", m->name, name); }
int hle_implements(const char *name) { (void)name; return 0; }
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig) { (void)ret_addr; (void)kind; (void)arg; (void)orig; die("t_aot_host: a rewriter gate"); }
