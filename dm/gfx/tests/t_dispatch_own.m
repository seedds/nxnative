// A compute binding's format-view upgrade must finish before its dispatch.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { texture2d<uint, access::read> src [[id(0)]]; device uint *out [[id(1)]]; constant uint *c0 [[id(2)]]; };\n"
        "kernel void copy_pixel(constant Args &a [[buffer(0)]], uint3 p [[thread_position_in_grid]], uint3 n [[threadgroups_per_grid]]) {\n"
        " uint i = 4 * ((p.z * n.y + p.y) * n.x + p.x); a.out[i] = a.src.read(uint2(0)).r;\n"
        " for (uint j = 0; j < 3; ++j) a.out[i + j + 1] = a.c0[0x200 / 4 + j]; }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic compute shader compiles"); if (!lib) return 1;
    prog_t pg = {.serial=960001}; NvProg *program = [NvProg new]; NvShader *cs = [NvShader new];
    program->sh[5] = cs; cs->stage = 5; cs->fn = [lib newFunctionWithName:@"copy_pixel"];
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = cs->rf.tg[1] = cs->rf.tg[2] = 1;
    cs->rf.nb = 3; cs->rf.drv_read[0] = 1;
    cs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .dtt=MTLTextureType2D, .dpf=NVN_DPF_UINT, .reg=0, .aid=0};
    cs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=0, .aid=1};
    cs->rf.b[2] = (nvn_rbind){.kind=NVN_RB_DRV0, .aid=2};
    serial_put(pg.serial, program);
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), getpagesize())) return 1;
    memset(mem, 0, getpagesize()); mtl_pool_add(mem, getpagesize());
    // Both direct and indirect first uses need an upgrade. Repeat each with the
    // cached view and different group counts to catch stale argument tables/c0.
    for (int indirect = 0; indirect < 2; indirect++) {
        uint64_t serial = 960002 + indirect;
        tex_t texture = {.magic=MAGIC('T','E','X',0), .serial=serial, .off=serial << 20};
        texture.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8,
            .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width=8, .height=8, .depth=1, .levels=1, .samples=1};
        nvn_tex_layout(&texture.d, &texture.L); mtl_tex_init(&texture);
        id<MTLTexture> old = tex_mtl(&texture);
        CHECK(old && !(old.usage & MTLTextureUsagePixelFormatView), "source needs a format-view upgrade");
        id<MTLCommandBuffer> seed = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = old; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(1,0,0,1);
        [[seed renderCommandEncoderWithDescriptor:rp] endEncoding]; [seed commit]; [seed waitUntilCompleted];
        CHECK(seed.status == MTLCommandBufferStatusCompleted, "source pixels seeded");
        view_t view = {.set_format=1, .format=NVN_FORMAT_RGBA8UI}; mtl_reg_tex(601 + indirect, &texture, &view);
        for (int warm = 0; warm < 2; warm++) {
            uint32_t counts[3] = {warm ? 2 : 3, warm ? 1 : 2, warm ? 2 : 1};
            memcpy((uint8_t *)mem + 2048, counts, sizeof counts);
            mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
            S.cprog = &pg; S.tex[5][0] = 601 + indirect;
            S.ssbo[5][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=1024};
            mtl_sub_fill((uint64_t)mem, 1024, 0);
            p_dispatch dp = {.x=(int)counts[0], .y=(int)counts[1], .z=(int)counts[2]};
            if (indirect) { dp.ind = (uint64_t)mem + 2048; dp.x = dp.y = dp.z = 99; }
            dispatch(&dp);
            uint64_t fence = mtl_sub_commit(); CHECK(mtl_wait(fence, 2000000000ull), "dispatch completes"); mtl_drain();
            CHECK(!mtl_last_commit_error(), "no GPU error");
            const uint32_t *out = mem; int pixels = 1, dimensions = 1;
            for (unsigned i = 0; i < counts[0] * counts[1] * counts[2]; i++) {
                pixels &= out[4 * i] == 255;
                dimensions &= !memcmp(out + 4 * i + 1, counts, sizeof counts);
            }
            CHECK(pixels, "dispatch reads the pixels preserved by the format-view upgrade");
            CHECK(dimensions, "every workgroup sees this dispatch's driver group counts");
        }
    }
    printf("t_dispatch_own: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
