// The front end's GPU-lost hook for t_gpu_lost: a strong definition in a compilation unit of its own replaces nvn_mtl.m's weak default (a FATAL line and an abort) at link time, as front_mac.m's does
#include <stdatomic.h>
_Atomic int test_gpu_lost_calls;
void front_gpu_lost(void) { atomic_fetch_add(&test_gpu_lost_calls, 1); }
