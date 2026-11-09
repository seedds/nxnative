// Compute shared memory in a device buffer (dmsh_build.py tg_spill, docs/reference/shaders.md "Compute shared memory past 32 KB"): dispatch binds a slice of DMSH_TG_SLOT for every
// workgroup of the grid. 1,024 workgroups of 48 KB each fill their slice with their own index and, after the barrier, check a neighbour thread's part; with slices shared modulo 512 some
// saw another workgroup's values. The indirect case takes its counts from a GPU fill earlier in the submission while the CPU's copy still names one workgroup: the slices must follow the
// GPU's counts (the shape of pull request #1242's test)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { GROUPS = 1024, THREADS = 64, WORDS = 12288, SLICE = WORDS * 4 };

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *out [[id(0)]]; };\n"
        "kernel void spilled(constant Args &a [[buffer(0)]], device uchar *dm_tg [[buffer(%d)]], uint3 g3 [[threadgroup_position_in_grid]], uint3 n3 [[threadgroups_per_grid]],\n"
        "                    uint t [[thread_index_in_threadgroup]]) {\n"
        " ulong base = ((ulong(g3.z) * n3.y + g3.y) * n3.x + g3.x) * %dul;\n"
        " device uint *mem = (device uint *)(dm_tg + base); uint g = (g3.z * n3.y + g3.y) * n3.x + g3.x;\n"
        " for (uint i = 0; i < %du; i++) mem[t * %du + i] = g * 1000u + i;\n"
        " threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);\n"
        " uint nb = (t + 1u) %% %du, bad = 0;\n"
        " for (uint i = 0; i < %du; i++) bad += mem[nb * %du + i] != g * 1000u + i;\n"
        " a.out[g * %du + t] = bad; }\n", DMSH_TG_SLOT, SLICE, WORDS / THREADS, WORDS / THREADS, THREADS, WORDS / THREADS, WORDS / THREADS, THREADS];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "spilled kernel compiles"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    prog_t pg = {.serial = 961001}; NvProg *program = [NvProg new]; NvShader *cs = [NvShader new];
    program->sh[5] = cs; cs->stage = 5; cs->fn = [lib newFunctionWithName:@"spilled"];
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = THREADS; cs->rf.tg[1] = cs->rf.tg[2] = 1;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 0};
    cs->rf.tg_spill = SLICE;
    serial_put(pg.serial, program);
    const size_t out_bytes = (size_t)GROUPS * THREADS * 4, pool = out_bytes + 0x4000;
    uint8_t *mem = NULL; if (posix_memalign((void **)&mem, getpagesize(), pool)) return 1;
    memset(mem, 0, pool); mtl_pool_add(mem, pool);
    uint32_t *args = (uint32_t *)(mem + out_bytes);
    for (int indirect = 0; indirect < 2; indirect++) {
        for (int run = 0; run < 3; run++) {
            mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
            S.cprog = &pg; S.ssbo[5][0] = (p_bind_buf){.addr = (uint64_t)mem, .size = out_bytes};
            mtl_sub_fill((uint64_t)mem, out_bytes, 0xffffffffu);
            p_dispatch dp = {.x = GROUPS, .y = 1, .z = 1};
            if (indirect) {   // the CPU's copy says one workgroup; the GPU writes 1,024 before the dispatch
                args[0] = args[1] = args[2] = 1;
                mtl_sub_fill((uint64_t)args, 4, GROUPS); mtl_sub_fill((uint64_t)args + 4, 8, 1);
                dp = (p_dispatch){.x = 99, .y = 99, .z = 99, .ind = (uint64_t)args};
            }
            dispatch(&dp);
            uint64_t fence = mtl_sub_commit(); CHECK(mtl_wait(fence, 5000000000ull), "dispatch completes"); mtl_drain();
            CHECK(!mtl_last_commit_error(), "no GPU error");
            const uint32_t *out = (const uint32_t *)mem; long bad = 0, missing = 0;
            for (size_t i = 0; i < (size_t)GROUPS * THREADS; i++) { missing += out[i] == 0xffffffffu; bad += out[i] != 0xffffffffu && out[i]; }
            if (bad || missing) fprintf(stderr, "%s run %d: %ld threads saw another workgroup's data, %ld did not run\n", indirect ? "indirect" : "direct", run, bad, missing);
            CHECK(!missing, indirect ? "every indirect workgroup ran" : "every direct workgroup ran");
            CHECK(!bad, indirect ? "indirect workgroups keep their own slices" : "direct workgroups keep their own slices");
        }
    }
    serial_del(pg.serial);
    printf("t_tg_spill: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
