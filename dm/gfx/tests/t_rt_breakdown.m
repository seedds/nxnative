// A snapshot records GPU writes without changing pool residency or retaining dead entries.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(int argc, char **argv) { @autoreleasepool {
    int on = argc == 1; setenv("DM_NVN_TEX_BREAKDOWN", on ? "1" : "0", 1);
    mtl_init();
    pool_t p = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS, .size = 1 << 20};
    tex_t t = {.pool = &p, .serial = 998000, .off = 512};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width = 64, .height = 64, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    alias_key k = tex_alias_key(&t);
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:tex_desc(&t, 0)];
    hm_put_id(&g_alias, &k, mt);
    atomic_store(&g_cur_frame, 10);
    mtl_tex_gpu_written(&t);
    CHECK((uintptr_t)nvn_hm_get(&g_alias_written, &k) == (on ? 11 : 0), "only the enabled diagnostic stamps GPU writes");
    rt_breakdown();
    CHECK(g_npool == 0, "a diagnostic does not create pool buffers");
    mtl_alias_drop_pool(&p);
    CHECK(g_alias_written.n == 0, "pool finalization removes recorded writes");
    printf("%d checks, %d failures\n", checks, failed); return failed != 0;
} }
