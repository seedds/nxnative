// Dispatch-first resets must follow preceding GPU writes and precede every append.
// Exercise the production Metal 4 path with four words and GPU-written indirect counts.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
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
int main(void) {
    @autoreleasepool {
        mtl_init();
        state_reset();
        memset(&R, 0, sizeof R);
        NSError *error = nil;
        NSString *source =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "struct Args { device atomic_uint *words [[id(0)]]; };\n"
             "kernel void append(constant Args &args [[buffer(0)]]) {\n"
             "    uint count = atomic_fetch_add_explicit(args.words, 1u, memory_order_relaxed);\n"
             "    atomic_fetch_max_explicit(args.words + 1, (count + 64u) / 64u, memory_order_relaxed);\n"
             "}\n";
        id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
        CHECK(library != nil, "append fixture compiles");
        if (!library) {
            return 1;
        }
        NvShader *shader = [NvShader new];
        shader->stage = 5;
        shader->fn = [library newFunctionWithName:@"append"];
        shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0];
        shader->rf.tg[0] = 64;
        shader->rf.tg[1] = shader->rf.tg[2] = 1;
        shader->rf.nb = 1;
        shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 2, .aid = 0};
        shader->rf.nfirst = 4;
        const uint16_t reset_words[] = {0, 2, 3, 1};
        const uint32_t reset_values[] = {0, 1, 1, 0};
        for (int i = 0; i < 4; i++) {
            shader->rf.first[i].reg = 2;
            shader->rf.first[i].word = reset_words[i];
            shader->rf.first[i].value = reset_values[i];
        }
        NvProg *program = [NvProg new];
        program->sh[5] = shader;
        prog_t guest = {.serial = UINT64_MAX - 8};
        serial_put(guest.serial, program); // NOLINT(clang-analyzer-osx.cocoa.RetainCount): serial_del releases the table's retain.
        S.cprog = &guest;
        void *memory = NULL;
        size_t memory_size = (size_t)getpagesize();
        if (posix_memalign(&memory, memory_size, memory_size)) {
            abort();
        }
        memset(memory, 0, memory_size);
        mtl_pool_add(memory, memory_size);
        uint64_t address = (uint64_t)(uintptr_t)memory;
        S.ssbo[5][2] = (p_bind_buf){.addr = address, .size = 64};
        for (int run = 0; run < 20; run++) {
            mtl_sub_begin();
            mtl_sub_fill(address, 20, 0x12345678);
            mtl_sub_fill(address + 256, 4, 128);
            mtl_sub_fill(address + 260, 8, 1);
            p_dispatch dispatch_args = {.x = 128, .y = 1, .z = 1};
            if (run & 1) {
                dispatch_args.ind = address + 256;
            }
            dispatch(&dispatch_args);
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "append dispatch completes");
            CHECK(!mtl_last_commit_error(), "append has no GPU error");
            const uint32_t *output = memory;
            CHECK(output[0] == 8192 && output[1] == 128, "all workgroups append after the reset");
            CHECK(output[2] == 1 && output[3] == 1, "both nonzero indirect words survive");
            CHECK(output[4] == 0x12345678, "adjacent storage remains intact");
        }
        mtl_drain();
        serial_del(guest.serial);
        S.cprog = NULL;
        mtl_pool_del(memory);
        free(memory);
        printf("t_first_store: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
}
