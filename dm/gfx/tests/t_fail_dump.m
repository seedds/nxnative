// The ring that names the passes of a command buffer that failed (nvn_mtl.m fail_dump; issue #1326). A GPU hang, a page fault or a revoked access used to print the fence, the
// error and a few counters, so three reports in that issue agree on the shape of the failure and none can say which pass or which shader the GPU died in. Checks that a command
// buffer's pass range is the one it encoded, that the dump names the passes of that range and leaves the others out, that each pass carries its size and the fragment key of its
// first draw, and that a range the ring no longer holds says so instead of printing nothing. Real Metal device for the attachments, no window, no game files
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)

static MTL4RenderPassDescriptor *pass_desc(int w, int h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
    d.colorAttachments[0].texture = [g_dev newTextureWithDescriptor:td];
    return d;
}
// One pass encoded: the host bumps g_stat_pass and records it, then its draws name it (rp_begin_ex, draw)
static void encode_pass(MTL4RenderPassDescriptor *d, const char *fskey, int draws) {
    g_stat_pass++; fail_note_pass(d, 1);
    for (int i = 0; i < draws; i++) fail_note_draw(fskey);
}
static char t_capbuf[16384];
static int t_saved; static FILE *t_tmpf;
static void cap_begin(void) { fflush(stderr); t_saved = dup(fileno(stderr)); t_tmpf = tmpfile(); dup2(fileno(t_tmpf), fileno(stderr)); }
static const char *cap_end(void) {
    fflush(stderr); dup2(t_saved, fileno(stderr)); close(t_saved);
    rewind(t_tmpf); size_t n = fread(t_capbuf, 1, sizeof t_capbuf - 1, t_tmpf); t_capbuf[n] = 0; fclose(t_tmpf); return t_capbuf;
}
static int lines_with(const char *s, const char *needle) { int n = 0; for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle)) n++; return n; }

int main(void) { @autoreleasepool {
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();

    // A command buffer's range starts after the passes already encoded
    g_stat_pass = 100; atomic_store(&g_cur_frame, 7); g_seg = 2;
    fail_cb_begin();
    CHECK("a command buffer's range starts at the next pass", g_cb_pass0 == 101);

    MTL4RenderPassDescriptor *big = pass_desc(1920, 1080), *small = pass_desc(256, 128);
    encode_pass(big, "09ab31c69af710c9", 3);
    encode_pass(small, "47f9b38490dcf9a2", 1);
    encode_pass(big, NULL, 0);
    uint64_t p0 = g_cb_pass0, p1 = g_stat_pass;
    CHECK("the range covers the passes it encoded", p0 == 101 && p1 == 103);

    cap_begin(); fail_dump("submission command buffer", 42, 7, 2, p0, p1); const char *out = cap_end();
    CHECK("the failure names the command buffer", strstr(out, "the submission command buffer that failed: fence 42, frame 7, segment 2, passes 101..103 (3 passes)") != NULL);
    CHECK("each pass of the range is printed", lines_with(out, "]   pass ") == 3);
    CHECK("a pass carries its size, its draws and its first fragment shader", strstr(out, "pass 101 1920x1080 colors 1 depth 0 draws 3 first fragment shader 09ab31c6") != NULL);
    CHECK("the key is the first draw's, cut to eight digits", strstr(out, "pass 102 256x128 colors 1 depth 0 draws 1 first fragment shader 47f9b384") != NULL);
    CHECK("a pass with no draw says so", strstr(out, "pass 103 1920x1080 colors 1 depth 0 draws 0 first fragment shader -") != NULL);

    // The next command buffer's failure must not print the previous one's passes
    fail_cb_begin();
    encode_pass(small, "645d3029b5948342", 2);
    cap_begin(); fail_dump("segment command buffer", 43, 7, 3, g_cb_pass0, g_stat_pass); out = cap_end();
    CHECK("only the failing command buffer's passes are printed", lines_with(out, "]   pass ") == 1 && strstr(out, "pass 104 256x128") != NULL);

    // The #1326 logs are full of failed submissions with "passes +0": an upload, copy or compute command buffer
    fail_cb_begin();
    cap_begin(); fail_dump("submission command buffer", 45, 7, 4, g_cb_pass0, g_stat_pass); out = cap_end();
    CHECK("a command buffer with no render pass says what it held", lines_with(out, "]   pass ") == 0 && strstr(out, "no render pass was encoded into it") != NULL);

    // A range the ring has lost: say it, rather than print an empty list
    for (int i = 0; i < FAIL_RING + 8; i++) encode_pass(small, "0000000000000000", 1);
    cap_begin(); fail_dump("submission command buffer", 44, 7, 2, 101, 103); out = cap_end();
    CHECK("a range the ring no longer holds says so", lines_with(out, "]   pass ") == 0 && strstr(out, "none of its passes is still in the ring of the last 256") != NULL);

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
} }
