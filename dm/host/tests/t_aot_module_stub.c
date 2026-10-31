// Stubs for t_aot_module: host facilities the synthetic module does not reach (it runs on the host clock, has no Unreal allocator), and the graphics backend, whose
// view of guest memory (be_guest_map / be_guest_unmap, called by hle_svc.c's memory SVCs) is counted here. The test links the engine (aot_*.c) and the host's real
// loader, import resolution and gate table (hle.c), SVC dispatcher and thread layer
#include "../dm.h"
#include <stdatomic.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;   // hle_thread.c's host-work count; in the host it is defined in hle_nvn.c
uint64_t now_ns(void) { return plat_now_ns(); }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void guest_clock_read_wait(void) {}
uint64_t dm_entropy64(void) { return ((uint64_t)plat_random32() << 32) | plat_random32(); }
void hlecount_svc(int num) { (void)num; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }
void hlecount_seal(void) {}
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; }

// The stub backend: every call counted, the last of each kind kept
_Atomic uint64_t g_t_guest_maps, g_t_guest_unmaps;
uint64_t g_t_guest_map_a, g_t_guest_map_n;
void be_guest_map(void *mem, uint64_t size) { g_t_guest_map_a = (uint64_t)(uintptr_t)mem; g_t_guest_map_n = size; g_t_guest_maps++; }
void be_guest_unmap(void *mem, uint64_t size) { (void)mem; (void)size; g_t_guest_unmaps++; }
