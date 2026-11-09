// A historically read texture ID keeps its mark across streaming replacements.
// Zero ordinary walk room exercises first-table materialization and GPU pixels.
// Finalize-before-replace, unread IDs and unwritten render targets stay distinct.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (%d)\n", msg, __LINE__); } } while (0)
static uint64_t entry(id<MTLBuffer> b, int i) { return ((uint64_t *)((uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }
static uint64_t dummy(id<MTLBuffer> b) { return entry(b, *(uint32_t *)b.contents); }
static id<MTLBuffer> table(void) {
    mtl_sub_begin();
    // Exhaust the ordinary per-frame allowance, without allocating a working-set-sized buffer.
    g_walk_frame = atomic_load(&g_cur_frame); g_walk_spent = POOL_FRAME_BYTES; g_walk_req = 0;
    __unsafe_unretained id<MTLBuffer> b = nil;
    os_unfair_lock_lock(&g_rmu);
    CHECK(pool_room() == 0, "ordinary walk has zero room");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b);
    os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> held = b;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completed");
    CHECK(!mtl_last_commit_error(), "submission has no GPU error");
    return held;
}
static void read_mark(int i) { pool_seen_gpu(); atomic_fetch_or((_Atomic uint32_t *)g_seen_buf.contents + i / 32, 1u << (i % 32)); }
static int pixel_is(int i, uint8_t v) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)g_treg[i].view;
    if (!t) return 0; // Do not resolve here: that would hide a failed pool walk.
    id<MTLBuffer> b = [g_dev newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
    if (!b) return 0;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
              toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:16384];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return 0;
    for (size_t n = 0; n < 16384; n++) if (((uint8_t *)b.contents)[n] != v) return 0;
    return 1;
}
int main(void) { @autoreleasepool {
    mtl_init();
    const char *e = getenv("DM_NVN_POOL_SEEN_KEEP"); int keep = !(e && !strcmp(e, "0"));
    enum { LEN = 1 << 20, N = 6 };
    uint8_t *mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    pool_t p = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=LEN, .flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, LEN);
    tex_t t[N] = {0};
    for (int n=0; n<N; n++) {
        t[n].magic=MAGIC('T','E','X',0); t[n].has_layout=1; t[n].serial=990100+n; t[n].pool=&p; t[n].off=n*65536; t[n].mem=mem+t[n].off;
        t[n].d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=64,.height=64,.depth=1,.levels=1,.samples=1};
        if (n==5) { t[n].has_layout=0; t[n].d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE; }
        nvn_tex_layout(&t[n].d,&t[n].L); memset(t[n].mem,0x11*(n+1),t[n].L.size); mtl_tex_init(&t[n]);
    }
    mtl_reg_tex(10,&t[0],NULL); mtl_reg_tex(20,&t[3],NULL); read_mark(10);
    id<MTLBuffer> old=table(); uint64_t original=entry(old,10);
    CHECK(original!=dummy(old) && pixel_is(10,0x11),"historically read texture has original pixels");
    CHECK(entry(old,20)==dummy(old),"never-read ID remains dummy at zero room");
    mtl_reg_tex(10,&t[1],NULL);
    id<MTLBuffer> direct=table();
    CHECK(pool_seen(10)==keep,"live replacement retains history only with fix enabled");
    CHECK((entry(direct,10)!=dummy(direct))==keep,"live replacement materialized in its first table");
    if (keep) CHECK(pixel_is(10,0x22),"live replacement table names new pixels");
    CHECK(entry(old,10)==original,"previous table retains its original entry");
    // Establish a made, read ID in either mode before the finalize/re-register case.
    read_mark(10); (void)table();
    mtl_tex_release(&t[1]);
    CHECK(g_treg[10].retired && !g_treg[10].view,"Finalize drops cached view while descriptor stays retired");
    mtl_reg_tex(10,&t[2],NULL);
    id<MTLBuffer> retired=table();
    CHECK(pool_seen(10)==keep,"finalize-then-replace retains history only with fix enabled");
    CHECK((entry(retired,10)!=dummy(retired))==keep,"retired replacement materialized in first table");
    if (keep) CHECK(pixel_is(10,0x33),"retired replacement has replacement pixels");
    mtl_reg_tex(20,&t[4],NULL); id<MTLBuffer> unread=table();
    CHECK(!pool_seen(20) && entry(unread,20)==dummy(unread),"replacing unread ID does not invent a read mark");
    read_mark(10); (void)table(); mtl_reg_tex(10,&t[5],NULL); id<MTLBuffer> target=table();
    CHECK(entry(target,10)==dummy(target),"unwritten render target remains dummy despite old read history");
    mtl_reg_tex(10,NULL,NULL); CHECK(!pool_seen(10),"emptying an ID clears its history");
    mtl_reg_tex(10,&t[0],NULL); id<MTLBuffer> reset=table();
    CHECK(!pool_seen(10),"empty then re-register does not resurrect history");
    (void)reset;
    mtl_reg_tex(10,NULL,NULL); mtl_reg_tex(20,NULL,NULL);
    for(int n=0;n<N;n++) if(n!=1) mtl_tex_release(&t[n]);
    mtl_pool_del(mem); mtl_drain(); munmap(mem,LEN);
    fprintf(stderr,"pool_seen_replace: %d checks, %d failed\n",checks,failed);
    return failed ? 1 : 0;
} }
