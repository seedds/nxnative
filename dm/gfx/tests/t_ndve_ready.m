// The production first-network readiness gate, without a Metal device.
#include "../nvn/nvn_mtl.m"

module_t *g_mod;
int g_nmod;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
void *guest_sym(const char *name) { (void)name; return NULL; }
void svc_thread_dump(void) {}
void dm_guest_clock_advanced(void) {}

static unsigned checks, failures;
#define CHECK(label, condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static _Atomic int returned, result;
static void *wait_first(void *unused) {
    (void)unused;
    result = ndve_ready_for_network(0, 3ull * NSEC_PER_SEC);
    returned = 1;
    return NULL;
}
static void pending(void) {
    g_nd_ready = dispatch_group_create();
    dispatch_group_enter(g_nd_ready);
    g_nd.state = 1; g_nd_waited = 0; returned = 0; result = -1;
}
static void finish_first(int state) {
    pending();
    pthread_t thread;
    int created = pthread_create(&thread, NULL, wait_first, NULL);
    CHECK("waiter created", created == 0);
    if (created) { dispatch_group_leave(g_nd_ready); return; }
    uint64_t until = bp_now() + NSEC_PER_SEC;
    while (!g_nd_waited && bp_now() < until) sched_yield();
    CHECK("first network entered readiness gate", g_nd_waited == 1);
    CHECK("no decision before initializer completes", returned == 0);
    g_nd.state = state;
    dispatch_group_leave(g_nd_ready);
    pthread_join(thread, NULL);
    CHECK("decision follows published initialization result", result == (state == 2));
}
int main(void) {
    alarm(10);
    finish_first(2);
    finish_first(-1);
    pending();
    CHECK("comparison mode keeps original path without waiting", ndve_ready_for_network(1, 0) == 0 && g_nd_waited == 0);
    CHECK("timeout keeps original path", ndve_ready_for_network(0, 0) == 0 && g_nd_waited == 1);
    uint64_t start = bp_now();
    CHECK("timeout is not imposed again", ndve_ready_for_network(0, 3ull * NSEC_PER_SEC) == 0 && bp_now() - start < NSEC_PER_SEC);
    g_nd.state = 2; dispatch_group_leave(g_nd_ready);
    CHECK("later readiness remains usable", ndve_ready_for_network(0, 0) == 1);
    g_nd.state = -1;
    CHECK("failed initialization retains fallback", ndve_ready_for_network(0, 0) == 0);
    alarm(0);
    printf("NDVE readiness: %u checks, %u failed\n", checks, failures);
    return failures != 0;
}
