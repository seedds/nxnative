// Public NVN image pool (NVN_RB_IMGP) GPU tests:
// 1. 3D image pool indexed by a handle loaded from a storage buffer (The Duskbloods' GXFogVolume_*), with an unrelated 2D IMAGE texture in the same pool (must map to the write sink, no [RED]).
// 2. Dynamic c2-picked 2D image pool without tex_dyn_slots, including a RegisterTexture-only texture bound via BindImages (img_pool_note_bound + image_prepare_shader upgrade).
// 3. Unaligned 1D buffer image pool (Persona 3 Reload's cp_ib_cb0_40000000_) using dm_buffer_write / dm_buffer_read / dm_buffer_width with p->bias and the negative-coordinate guard.
// 4. g_psnap_gen invalidation when img_pool_gpu rebuilds after a new registration.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#include <unistd.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void crash_install(void);

#define P(name) ((NVNT_##name)nvn_lookup(#name))
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void require(int ok, const char *label) { CHECK(ok, label); if (!ok) exit(2); }

enum {
    POOL_BYTES    = 0x200000,
    COMMAND_BYTES = 0x20000,
    DESCRIPTORS   = 0x20000,
    SSBO_OFF      = 0x30000,
    READBACK_OFF  = 0x40000,
    TEX3D_OFF     = 0x60000,
    TEX2D_A_OFF   = 0x80000,
    TEX2D_B_OFF   = 0xA0000,
    TEX2D_C_OFF   = 0xC0000,
    BUF_IMG_OFF   = 0xE0000,
    POOL_IDS      = 384,
};

static NVNdevice device;
static NVNqueue queue;
static NVNcommandBuffer command;
static NVNmemoryPool pool;
static NVNtexturePool textures;
static NVNbufferAddress base;
static uint8_t *memory;
static NSMutableArray *keep_shaders;

static void submit_and_wait(NVNcommandHandle h) {
    NVNsync sync; P(nvnSyncInitialize)(&sync, &device);
    P(nvnQueueSubmitCommands)(&queue, 1, &h);
    P(nvnQueueFenceSync)(&queue, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNsyncWaitResult r = P(nvnSyncWait)(&sync, 5000000000ull);
    require(r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "queue fence completes");
    CHECK(!mtl_last_commit_error(), "no Metal execution error");
    P(nvnSyncFinalize)(&sync);
}

static void make_program(NVNprogram *prog, id<MTLLibrary> lib, NSString *fn_name, const char *key, nvn_refl rf) {
    require(P(nvnProgramInitialize)(prog, &device), "program initializes");
    NvShader *sh = [NvShader new];
    [keep_shaders addObject:sh];
    sh->stage = 5;
    sh->fn = [lib newFunctionWithName:fn_name];
    require(sh->fn != nil, "shader function compiles");
    sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0];
    require(sh->ae != nil, "argument encoder builds");
    sh->arg_len = sh->ae.encodedLength;
    strlcpy(sh->key, key, sizeof sh->key);
    sh->rf = rf;
    NvProg *np = [NvProg new];
    np->sh[5] = sh;
    prog_t *p = (prog_t *)prog;
    p->serial = nvn_new_serial();
    serial_put(p->serial, np);
}

int main(void) { @autoreleasepool {
    alarm(90);
    require(!setvbuf(stdout, NULL, _IONBF, 0), "unbuffered stdout");
    crash_install();
    setenv("DM_PSO_ARCHIVE", "off", 1);
    setenv("DM_PSO_PREBUILD", "0", 1);
    setenv("DM_PSO_USER", "/dev/null", 1);
    setenv("DM_PSO_RECORD", "/dev/null", 1);
    mtl_init();
    keep_shaders = [NSMutableArray array];

    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "template<typename P> static inline __attribute__((always_inline)) auto dm_buffer_read(constant P* p, uint i, uint c)\n"
        "{\n    uint b = reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(p) + p->bias)[i];\n"
        "    uint m = c + b;\n    return (&p->e[0])[i].read(m < b ? c : m);\n}\n"
        "template<typename P, typename V> static inline __attribute__((always_inline)) void dm_buffer_write(constant P* p, uint i, V v, uint c)\n"
        "{\n    uint b = reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(p) + p->bias)[i];\n"
        "    uint m = c + b;\n    (&p->e[0])[i].write(v, m < b ? c : m);\n}\n"
        "template<typename P> static inline __attribute__((always_inline)) uint dm_buffer_width(constant P* p, uint i)\n"
        "{\n    uint b = reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(p) + p->bias)[i];\n"
        "    return (&p->e[0])[i].get_width() - b;\n}\n"
        "struct spvPool_3d { uint n; ulong seen; array<texture3d<uint, access::read_write>, 1> e; };\n"
        "struct Args3d { constant spvPool_3d* img [[id(0)]]; device uint* ssbo [[id(1)]]; };\n"
        "kernel void write_3d_from_ssbo(constant Args3d& d [[buffer(0)]], uint tid [[thread_position_in_grid]]) {\n"
        "    uint h0 = d.ssbo[0] & 0xfffffu;\n"
        "    uint h1 = d.ssbo[1] & 0xfffffu;\n"
        "    uint i0 = min(h0, d.img->n);\n"
        "    uint i1 = min(h1, d.img->n);\n"
        "    if (tid == 0) {\n"
        "        (&d.img->e[0])[i0].write(uint4(0x55aa1234u, 0, 0, 0), uint3(2, 1, 1));\n"
        "        (&d.img->e[0])[i1].write(uint4(0xdeadbeefu, 0, 0, 0), uint3(0, 0, 0));\n"
        "    }\n"
        "}\n"
        "struct spvPool_2d { uint n; ulong seen; array<texture2d<uint, access::read_write>, 1> e; };\n"
        "struct C2 { uint4 _m0[256]; };\n"
        "struct Args2d { constant spvPool_2d* img [[id(0)]]; constant C2* c2 [[id(1)]]; };\n"
        "kernel void write_2d_from_c2(constant Args2d& d [[buffer(0)]], uint tid [[thread_position_in_grid]]) {\n"
        "    // DMSH_C2_IMG = 0x120 (vec4 index 18), stride 8 bytes: slot 0 in _m0[18].x, slot 1 in _m0[18].z\n"
        "    uint h = (tid == 0) ? d.c2->_m0[18].x : d.c2->_m0[18].z;\n"
        "    uint i = min(h & 0xfffffu, d.img->n);\n"
        "    (&d.img->e[0])[i].write(uint4(tid == 0 ? 0x11112222u : 0x33334444u, 0, 0, 0), uint2(1, 1));\n"
        "}\n"
        "struct spvPool_buf { uint n; uint bias; ulong seen; array<texture_buffer<uint, access::read_write>, 1> e; };\n"
        "struct ArgsBuf { constant spvPool_buf* img [[id(0)]]; device uint* ssbo [[id(1)]]; };\n"
        "kernel void rw_buf_from_ssbo(constant ArgsBuf& d [[buffer(0)]], uint tid [[thread_position_in_grid]]) {\n"
        "    uint i = min(d.ssbo[0] & 0xfffffu, d.img->n);\n"
        "    dm_buffer_write(d.img, i, uint4(0xdead0000u + tid, 0, 0, 0), tid);\n"
        "    if (tid == 0) {\n"
        "        dm_buffer_write(d.img, i, uint4(0xbadbadu, 0, 0, 0), 0xffffffffu);\n"
        "        d.ssbo[1] = dm_buffer_width(d.img, i);\n"
        "    }\n"
        "    threadgroup_barrier(mem_flags::mem_texture);\n"
        "    d.ssbo[2 + tid] = dm_buffer_read(d.img, i, tid).x;\n"
        "}\n"
        options:nil error:&err];
    require(lib != nil, "test Metal shaders compile");

    NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device, &db), "device initializes");
    NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device);
    require(P(nvnQueueInitialize)(&queue, &qb), "queue initializes");
    require(!posix_memalign((void **)&memory, (size_t)getpagesize(), POOL_BYTES), "aligned pool storage");
    memset(memory, 0, POOL_BYTES);
    NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24);
    require(P(nvnMemoryPoolInitialize)(&pool, &pb), "memory pool initializes");
    base = P(nvnMemoryPoolGetBufferAddress)(&pool); require(base != 0, "pool GPU address");
    require(P(nvnTexturePoolInitialize)(&textures, &pool, DESCRIPTORS, POOL_IDS), "texture pool initializes");
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initializes");
    P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, COMMAND_BYTES);

    // 1. 3D image pool with handle from SSBO + unrelated 2D IMAGE texture in same pool
    NVNtexture tex3d, tex2d_unrelated;
    {
        NVNtextureBuilder tb = {0};
        P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
        P(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_IMAGE);
        P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_3D);
        P(nvnTextureBuilderSetSize3D)(&tb, 4, 4, 2);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32UI);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, TEX3D_OFF);
        require(P(nvnTextureInitialize)(&tex3d, &tb), "3D image initializes");
        P(nvnTexturePoolRegisterImage)(&textures, 300, &tex3d, NULL);

        P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
        P(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_IMAGE);
        P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
        P(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32UI);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, TEX2D_A_OFF);
        require(P(nvnTextureInitialize)(&tex2d_unrelated, &tb), "unrelated 2D image initializes");
        P(nvnTexturePoolRegisterImage)(&textures, 301, &tex2d_unrelated, NULL);
    }
    NVNprogram prog3d;
    {
        nvn_refl rf = {0}; rf.nb = 2; rf.bindless = 1; rf.pool = 1; rf.tg[0] = rf.tg[1] = rf.tg[2] = 1;
        rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMGP, .rw = 1, .dtt = MTLTextureType3D, .dpf = NVN_DPF_UINT, .aid = 0};
        rf.b[1] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 1};
        make_program(&prog3d, lib, @"write_3d_from_ssbo", "imgp3d0000000001", rf);
    }
    ((uint32_t *)(memory + SSBO_OFF))[0] = 300;
    ((uint32_t *)(memory + SSBO_OFF))[1] = 301;   // 2D image ID: must hit 3D write sink, never touch tex2d_unrelated
    P(nvnMemoryPoolFlushMappedRange)(&pool, SSBO_OFF, 64);
    uint8_t red157_before = g_red_once[157], red32_before = g_red_once[32], red106_before = g_red_once[106];
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetTexturePool)(&command, &textures);
    P(nvnCommandBufferBindProgram)(&command, &prog3d, NVN_SHADER_STAGE_COMPUTE_BIT);
    P(nvnCommandBufferBindStorageBuffer)(&command, NVN_SHADER_STAGE_COMPUTE, 0, base + SSBO_OFF, 64);
    P(nvnCommandBufferDispatchCompute)(&command, 1, 1, 1);
    const NVNcopyRegion reg3d = {0, 0, 0, 4, 4, 2};
    const NVNcopyRegion reg2d = {0, 0, 0, 4, 4, 1};
    P(nvnCommandBufferCopyTextureToBuffer)(&command, &tex3d, NULL, &reg3d, base + READBACK_OFF, 0);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, &tex2d_unrelated, NULL, &reg2d, base + READBACK_OFF + 0x1000, 0);
    submit_and_wait(P(nvnCommandBufferEndRecording)(&command));
    uint32_t *rb3d = (uint32_t *)(memory + READBACK_OFF);
    uint32_t *rb2d = (uint32_t *)(memory + READBACK_OFF + 0x1000);
    CHECK(rb3d[1 * 16 + 1 * 4 + 2] == 0x55aa1234u, "3D img_pool wrote voxel (2,1,1) via handle from SSBO");
    CHECK(rb2d[0] == 0, "unrelated 2D image ID in 3D img_pool hit write sink, leaving 2D texture untouched");
    CHECK(g_red_once[157] == red157_before && g_red_once[32] == red32_before && g_red_once[106] == red106_before, "3D img_pool skipped unrelated 2D image with zero [RED] mismatch logs");

    // 2. c2-picked 2D image pool (no tex_dyn_slots): one texture registered with RegisterTexture (no IMAGE flag!) and one with RegisterImage
    NVNtexture tex2d_regtex, tex2d_regimg;
    {
        NVNtextureBuilder tb = {0};
        P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
        P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
        P(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32UI);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, TEX2D_B_OFF);
        require(P(nvnTextureInitialize)(&tex2d_regtex, &tb), "RegisterTexture-only 2D texture initializes");
        P(nvnTexturePoolRegisterTexture)(&textures, 310, &tex2d_regtex, NULL);   // deliberately NO IMAGE flag and RegisterTexture

        P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
        P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
        P(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32UI);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, TEX2D_C_OFF);
        require(P(nvnTextureInitialize)(&tex2d_regimg, &tb), "RegisterImage 2D texture initializes");
        P(nvnTexturePoolRegisterImage)(&textures, 311, &tex2d_regimg, NULL);
    }
    NVNprogram prog2d;
    {
        nvn_refl rf = {0}; rf.nb = 2; rf.bindless = 1; rf.pool = 1; rf.drv_read[2] = 1; rf.tg[0] = 2; rf.tg[1] = rf.tg[2] = 1;
        rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMGP, .rw = 1, .dtt = MTLTextureType2D, .dpf = NVN_DPF_UINT, .aid = 0};
        rf.b[1] = (nvn_rbind){.kind = NVN_RB_DRV2, .aid = 1};
        make_program(&prog2d, lib, @"write_2d_from_c2", "imgp2d0000000002", rf);
    }
    NVNimageHandle ihs[2] = { P(nvnDeviceGetImageHandle)(&device, 310), P(nvnDeviceGetImageHandle)(&device, 311) };
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetTexturePool)(&command, &textures);
    P(nvnCommandBufferBindProgram)(&command, &prog2d, NVN_SHADER_STAGE_COMPUTE_BIT);
    P(nvnCommandBufferBindImages)(&command, NVN_SHADER_STAGE_COMPUTE, 0, 2, ihs);
    P(nvnCommandBufferDispatchCompute)(&command, 1, 1, 1);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, &tex2d_regtex, NULL, &reg2d, base + READBACK_OFF + 0x2000, 0);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, &tex2d_regimg, NULL, &reg2d, base + READBACK_OFF + 0x3000, 0);
    submit_and_wait(P(nvnCommandBufferEndRecording)(&command));
    CHECK(((uint32_t *)(memory + READBACK_OFF + 0x2000))[1 * 4 + 1] == 0x11112222u, "c2-picked img_pool wrote RegisterTexture-only bound image (slot 0)");
    CHECK(((uint32_t *)(memory + READBACK_OFF + 0x3000))[1 * 4 + 1] == 0x33334444u, "c2-picked img_pool wrote RegisterImage bound image (slot 1)");

    // 3. Unaligned 1D buffer image pool (p->bias + dm_buffer_write / read / width + negative-coord guard)
    NVNtexture texbuf;
    {
        NVNtextureBuilder tb = {0};
        P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
        P(nvnTextureBuilderSetFlags)(&tb, NVN_TEXTURE_FLAGS_IMAGE);
        P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_BUFFER);
        P(nvnTextureBuilderSetSize1D)(&tb, 8);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R32UI);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, BUF_IMG_OFF + 4);   // 4-byte unaligned offset -> lead >= 1
        require(P(nvnTextureInitialize)(&texbuf, &tb), "unaligned buffer image initializes");
        P(nvnTexturePoolRegisterImage)(&textures, 320, &texbuf, NULL);
    }
    NVNprogram progbuf;
    {
        nvn_refl rf = {0}; rf.nb = 2; rf.bindless = 1; rf.pool = 1; rf.tg[0] = 8; rf.tg[1] = rf.tg[2] = 1;
        rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMGP, .rw = 1, .dtt = MTLTextureTypeTextureBuffer, .dpf = NVN_DPF_UINT, .aid = 0};
        rf.b[1] = (nvn_rbind){.kind = NVN_RB_SBUFP, .reg = 0, .aid = 1, .rw = 1};
        make_program(&progbuf, lib, @"rw_buf_from_ssbo", "imgpbf0000000003", rf);
    }
    memset(memory + BUF_IMG_OFF, 0, 64);
    memset(memory + SSBO_OFF, 0, 64);
    ((uint32_t *)(memory + SSBO_OFF))[0] = 320;
    P(nvnMemoryPoolFlushMappedRange)(&pool, BUF_IMG_OFF, 64);
    P(nvnMemoryPoolFlushMappedRange)(&pool, SSBO_OFF, 64);
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetTexturePool)(&command, &textures);
    P(nvnCommandBufferBindProgram)(&command, &progbuf, NVN_SHADER_STAGE_COMPUTE_BIT);
    P(nvnCommandBufferBindStorageBuffer)(&command, NVN_SHADER_STAGE_COMPUTE, 0, base + SSBO_OFF, 64);
    P(nvnCommandBufferDispatchCompute)(&command, 1, 1, 1);
    submit_and_wait(P(nvnCommandBufferEndRecording)(&command));
    uint32_t *buf_words = (uint32_t *)(memory + BUF_IMG_OFF);
    uint32_t *ssbo_words = (uint32_t *)(memory + SSBO_OFF);
    CHECK(buf_words[0] == 0, "negative-coordinate dm_buffer_write did not clobber guard word before unaligned buffer image");
    CHECK(ssbo_words[1] == 8u, "dm_buffer_width returned logical width (8), not aligned view width");
    int buf_ok = 1, rd_ok = 1;
    for (int i = 0; i < 8; i++) {
        buf_ok &= (buf_words[1 + i] == 0xdead0000u + (uint32_t)i);
        rd_ok &= (ssbo_words[2 + i] == 0xdead0000u + (uint32_t)i);
    }
    if (!buf_ok || !rd_ok) fprintf(stderr, "  buf[0..4]=%#x %#x %#x %#x %#x  rd[0..3]=%#x %#x %#x %#x\n", buf_words[0], buf_words[1], buf_words[2], buf_words[3], buf_words[4], ssbo_words[2], ssbo_words[3], ssbo_words[4], ssbo_words[5]);
    CHECK(buf_ok, "unaligned buffer img_pool dm_buffer_write wrote every element at offset +4");
    CHECK(rd_ok, "unaligned buffer img_pool dm_buffer_read read back every element at offset +4");

    // 4. g_psnap_gen increments when img_pool_gpu rebuilds after a new registration
    uint64_t pgen_before = g_psnap_gen;
    P(nvnTexturePoolRegisterImage)(&textures, 330, &tex3d, NULL);
    os_unfair_lock_lock(&g_rmu);
    nvn_rbind rb_probe = {.kind = NVN_RB_IMGP, .rw = 1, .dtt = MTLTextureType3D, .dpf = NVN_DPF_UINT};
    __unsafe_unretained id<MTLBuffer> pb_out = nil;
    uint64_t gpu_after = img_pool_gpu(&rb_probe, 0, &pb_out);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(gpu_after != 0 && pb_out != nil && g_psnap_gen > pgen_before, "img_pool_gpu rebuild bumps g_psnap_gen for argument-block invalidation");

    P(nvnQueueFinish)(&queue);
    P(nvnProgramFinalize)(&prog3d); P(nvnProgramFinalize)(&prog2d); P(nvnProgramFinalize)(&progbuf);
    P(nvnTextureFinalize)(&tex3d); P(nvnTextureFinalize)(&tex2d_unrelated); P(nvnTextureFinalize)(&tex2d_regtex);
    P(nvnTextureFinalize)(&tex2d_regimg); P(nvnTextureFinalize)(&texbuf);
    P(nvnCommandBufferFinalize)(&command); P(nvnTexturePoolFinalize)(&textures);
    P(nvnMemoryPoolFinalize)(&pool); P(nvnQueueFinalize)(&queue); P(nvnDeviceFinalize)(&device);
    free(memory);
    printf("public NVN image pool: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
