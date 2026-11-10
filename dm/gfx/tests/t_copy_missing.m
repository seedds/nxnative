// Missing recorded texture objects must not crash enabled pass-dump diagnostics,
// change the surviving texture, or prevent the next valid copy in that submission.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void finish(void) {
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completes");
    CHECK(!mtl_last_commit_error(), "GPU has no error");
}
int main(int argc, char **argv) { @autoreleasepool {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    if (mode < 0 || mode > 2) return 2;
    CHECK(!setenv("DM_NVN_PASSDUMP", "0", 1), "enable pass dump for frame zero");
    mtl_init(); CHECK(passdump(), "diagnostics actually enabled");
    tex_t src = {.magic=MAGIC('T','E','X',0), .serial=987601};
    src.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    tex_t dst = src; dst.serial++;
    MTLTextureDescriptor *desc = tex_desc(&src, 0); desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> s = [g_dev newTextureWithDescriptor:desc], d = [g_dev newTextureWithDescriptor:desc];
    CHECK(s && d, "test textures allocate"); if (!s || !d) return 2;
    uint8_t input[64], sentinel[64], got[64];
    for (unsigned i = 0; i < sizeof input; i++) input[i] = (uint8_t)(i * 3 + 7);
    memset(sentinel, 0x5a, sizeof sentinel);
    [s replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:input bytesPerRow:16];
    [d replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:sentinel bytesPerRow:16];
    res_add(s); res_add(d); mtl_tex_init(&src); mtl_tex_init(&dst); serial_put(src.serial, s); serial_put(dst.serial, d);
    p_copy_t2t invalid = {.src=mode == 2 ? &src : NULL, .dst=mode == 1 ? &dst : NULL, .sr={0,0,0,4,4,1}, .dr={0,0,0,4,4,1}};
    mtl_sub_begin();
    CHECK(copy_t2t_enc(&invalid) == nil, "missing object rejects the copy");
    finish();
    [s getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    CHECK(!memcmp(got, input, sizeof got), "rejected copy preserves source bytes");
    [d getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    CHECK(!memcmp(got, sentinel, sizeof got), "rejected copy preserves destination bytes");
    mtl_sub_begin(); copy_t2t(&invalid);
    p_copy_t2t valid = {.src=&src, .dst=&dst, .sr={0,0,0,4,4,1}, .dr={0,0,0,4,4,1}};
    copy_t2t(&valid); finish();
    [d getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    CHECK(!memcmp(got, input, sizeof got), "a following valid copy still copies every byte");
    mtl_tex_release(&src); mtl_tex_release(&dst); mtl_drain();
    printf("t_copy_missing mode=%d: %d checks, %d failed\n", mode, checks, failed);
    return failed != 0;
} }
