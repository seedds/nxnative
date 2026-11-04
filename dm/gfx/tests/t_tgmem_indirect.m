// Indirect compute spill sizes follow the current GPU-written dispatch arguments.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) {
    return NULL;
}
module_t *module_of(const void *p) {
    (void)p;
    return NULL;
}
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {
}
uint64_t now_ns(void) {
    return bp_now();
}
static int checks, failed;
#define CHECK(c, label)                                                                                                \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failed++;                                                                                                  \
            fprintf(stderr, "FAIL: %s\n", label);                                                                      \
        }                                                                                                              \
    } while (0)

static void check_indirect_spill(uint8_t *arguments) {
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\n"
                        "using namespace metal;\n"
                        "kernel void spilled(device uint* storage [[buffer(30)]],\n"
                        "                    uint3 group [[threadgroup_position_in_grid]]) {\n"
                        "    storage[4 + group.x * 12288] = group.x + 1;\n"
                        "}\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    CHECK(library != nil, "indirect spill fixture compiles");
    if (!library) {
        return;
    }
    NvShader *shader = [NvShader new];
    shader->stage = 5;
    shader->fn = [library newFunctionWithName:@"spilled"];
    shader->rf.tg[0] = shader->rf.tg[1] = shader->rf.tg[2] = 1;
    shader->rf.tgmem = 49152;
    NvProg *program = [NvProg new];
    program->sh[5] = shader;
    prog_t guest = {.serial = UINT64_MAX - 1};
    serial_put(guest.serial, program); // NOLINT(clang-analyzer-osx.cocoa.RetainCount): serial_del releases the table's retain.
    S.cprog = &guest;
    // The CPU's stale arguments name no work. Queued GPU fills replace them with 1024 groups; the dispatch must allocate for those current counts.
    mtl_sub_fill((uint64_t)(uintptr_t)arguments, 12, 0);
    CHECK(mdi_wait(), "initial argument clear completes");
    mtl_sub_fill((uint64_t)(uintptr_t)arguments, 4, 1024);
    mtl_sub_fill((uint64_t)(uintptr_t)arguments + 4, 8, 1);
    p_dispatch dispatch_args = {.ind = (uint64_t)(uintptr_t)arguments};
    dispatch(&dispatch_args);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "indirect spilled dispatch completes");
    CHECK(!mtl_last_commit_error(), "indirect spilled dispatch has no GPU error");
    CHECK(g_tgm_buf.length >= DMSH_TGMEM_HEAD + 1024ull * 49152,
          "spill allocation covers GPU-written counts before dispatch");
    uint32_t *words = g_tgm_buf.contents;
    int wrong_outputs = 0;
    for (uint32_t g = 0; g < 1024; g++) {
        wrong_outputs += words[DMSH_TGMEM_HEAD / sizeof(uint32_t) + g * 12288] != g + 1;
    }
    CHECK(!wrong_outputs, "all indirectly dispatched groups have separate storage");
    serial_del(guest.serial);
    S.cprog = NULL;
    mtl_sub_begin();
}

int main(void) {
    @autoreleasepool {
        mtl_init();
        size_t size = 0x4000;
        uint8_t *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (memory == MAP_FAILED) {
            abort();
        }
        pool_t pool = {0};
        pool_builder_t pool_builder = {.mem = memory,
                                       .size = size,
                                       .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
        CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pool_builder),
              "argument pool initialized");
        state_reset();
        memset(&R, 0, sizeof R);
        mtl_sub_begin();
        check_indirect_spill(memory);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completes");
        h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
        defer_drain();
        munmap(memory, size);
        printf("t_tgmem_indirect: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
}
