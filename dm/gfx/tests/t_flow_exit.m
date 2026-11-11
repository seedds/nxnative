// Exercise the production shutdown hook, then bypass every atexit handler.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int main(void) {
    if (!nvn_flow_on()) return 3;
    nvn_flow_event("lifecycle probe", 123, 0, 1, 2);
    mtl_arch_save(0, 1);
    _Exit(0);
}
