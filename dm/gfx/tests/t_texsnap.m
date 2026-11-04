// TEXSNAP uses the production render pass and the ordered Metal 4 copy encoder.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; printf("%s: %s\n", (c) ? "PASS" : "FAIL", label); if (!(c)) failed++; } while (0)
static void clear(MTLClearColor c) { R.clear_c[0] = 1; R.cc[0] = c; CHECK(rp_begin() != nil, "render pass opens"); rp_end(); }
int main(void) { @autoreleasepool {
    mtl_init(); state_reset(); memset(&R, 0, sizeof R);
    CHECK(ts_format(MTLPixelFormatRG11B10Float), "DKB packed HDR format is supported");
    CHECK(!ts_format(MTLPixelFormatRGBA8Uint), "integer format is excluded from float dump");
    int hdr = getenv("TEXSNAP_TEST_HDR") != NULL;
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=99871};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=hdr ? NVN_FORMAT_R11G11B10F : NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *d = tex_desc(&t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:d]; res_add(target); mtl_tex_init(&t); serial_put(t.serial, target);
    R.has_rt=1; R.rt.n=1; R.rt.color[0]=&t;
    g_td_list=[NSMutableArray array]; g_td_frame=100; g_ts_n=0; g_ts_all=1; mtl_set_frame(99); mtl_sub_begin();
    clear(MTLClearColorMake(0,0,0,1)); CHECK(g_ts_count==0, "nonmatching frame does not capture");
    mtl_set_frame(100); clear(MTLClearColorMake(1,0.5,0.25,1));
    CHECK(g_ts_count==1 && g_td_list.count>=1, "matching frame captures output");
    id<MTLTexture> snap = nil; for (NSArray *e in g_td_list) if (e.count > 4) snap=e[0];
    g_ts_all=0; clear(MTLClearColorMake(0,1,0,1)); CHECK(g_ts_count==1, "selection off does not capture");
    g_ts_all=1; g_ts_count=TS_MAX_COUNT; clear(MTLClearColorMake(0,1,0,1)); CHECK(g_ts_truncated==1, "count limit reports truncation"); g_ts_count=1; g_ts_bytes=TS_MAX_BYTES; clear(MTLClearColorMake(0,1,0,1));
    CHECK(g_ts_truncated==2 && g_ts_count==1, "byte limit rejects another allocation"); g_ts_all=0;
    id<MTLBuffer> out=[g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared]; res_add(out);
    if (snap) enc_b_copy_texture_buffer(blit(), snap, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), out, 0, 256, 1024);
    enc_b_copy_texture_buffer(blit(), target, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), out, 1024, 256, 1024);
    g_td_frame=101; mtl_set_frame(101); g_ts_all=1; clear(MTLClearColorMake(0,1,0,1));
    CHECK(g_ts_count==1 && g_ts_truncated==0 && g_ts_bytes<TS_MAX_BYTES, "new explicit frame resets capture budget"); g_ts_all=0;
    mtl_sub_commit(); mtl_finish();
    uint8_t a[4]={255,128,64,255}, b[4]={0,255,0,255};
    if (hdr) { uint32_t av=(15u<<6) | ((14u<<6)<<11) | ((13u<<5)<<22), bv=(15u<<6)<<11; memcpy(a,&av,4); memcpy(b,&bv,4); }
    int ok1=!!snap, ok2=1;
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) { ok1 &= !memcmp((uint8_t *)out.contents+y*256+x*4,a,4); ok2 &= !memcmp((uint8_t *)out.contents+1024+y*256+x*4,b,4); }
    CHECK(ok1, "snapshot retains original asymmetric color"); CHECK(ok2, "target contains later overwrite");
    g_ts_all=1; ts_summary(); g_td_frame=102; ts_summary(); g_ts_all=0;
    printf("texsnap: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
