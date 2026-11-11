// NVN null device unit tests: look up addresses from nvnBootstrapLoader the way the game does and go all the way to present; all expected values are hand-calculated, without calling the implementation under test.
// For build and run see dm/gfx/tests/Makefile (make -C dm/gfx/tests).
#include "../nvn/hle_nvn.c"
#include "../tex/nvn_deswizzle.h"
#include <time.h>
void *front_metal_layer(void) { return NULL; }   // no window in unit tests: present only counts
module_t *module_of(const void *p) { (void)p; return NULL; }   // no guest modules in unit tests
module_t *g_mod; int g_nmod;                            // host symbols that crash.c depends on
void svc_thread_dump(void) {}
void crash_install(void);
void nvn_present_config(int hz, int display_mode);
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }   // the host's clock (hle_os.c: Mach absolute time), which Metal's timestamps are on

static int g_fail, g_n;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); g_n++;                 \
    printf("%-44s expected %-12lld → got %-12lld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); if (w_ != g_) g_fail++; } while (0)

#define P(name) ((NVNT_##name)proc(#name))
static PFNNVNDEVICEGETPROCADDRESSPROC gpa; static NVNdevice dev;
static void *proc(const char *n) { void *p = (void *)gpa(&dev, n); if (!p) { printf("address lookup failed %s\n", n); exit(1); } return p; }
// CRC-16 with the reflected polynomial 0xA001, from 0: the hash Super Mario Odyssey's sampler cache takes of a sampler's bytes 8 on (main+0x96e400)
static uint16_t crc16_a001(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1); }
    return c;
}

// A recording's control list entry (nvn_obj.h ctl_e) at a handle, and whether it is one piece of n record bytes at p, the last entry
static uint64_t ctl_at(NVNcommandHandle h, int i) { uint64_t v; memcpy(&v, (const uint8_t *)(uintptr_t)h + 8 * i, 8); return v; }
// A recording's records as the queue submits them (compact ones expanded); kept until the next call
static uint8_t *lin(NVNcommandHandle h) { static uint8_t *b; free(b); return b = nvn_rec_linear(h, NULL); }
static int one_piece(NVNcommandHandle h, const void *p, size_t n) { uint64_t v = ctl_at(h, 0); return ctl_kind(v) == CTL_CHUNK && (v & CTL_LAST) && ctl_addr(v) == p && ctl_len(v) == n; }

// Command memory callback: hands out a new 256-byte memory segment each time (forces a recording of many pieces); control memory it leaves to the host's own blocks
static uint8_t g_cmdmem[64 * 1024]; static size_t g_cmdoff; static int g_cb_calls; static NVNmemoryPool g_cmdpool;
static void mem_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent ev, size_t minsz, void *data) {
    (void)data; g_cb_calls++;
    if (ev != NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY) return;
    size_t n = minsz > 256 ? minsz : 256;
    P(nvnCommandBufferAddCommandMemory)(c, &g_cmdpool, (ptrdiff_t)g_cmdoff, n); g_cmdoff += n;
}

// Counts the out-of-memory callbacks of a command buffer that should need none (the copy strides test)
static int g_nocb_calls;
static void no_mem_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent ev, size_t minsz, void *data) { (void)c; (void)ev; (void)minsz; (void)data; g_nocb_calls++; }

// Control memory callback for the list-overflow test: hands out 32-byte control blocks in turn; command memory from g_cmdpool as mem_cb
static uint8_t g_ctlmem[4096] __attribute__((aligned(8))); static size_t g_ctloff; static int g_ctl_calls;
static void ctl_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent ev, size_t minsz, void *data) {
    if (ev == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY) { mem_cb(c, ev, minsz, data); return; }
    g_ctl_calls++; size_t n = minsz > 32 ? minsz : 32;
    P(nvnCommandBufferAddControlMemory)(c, g_ctlmem + g_ctloff, n); g_ctloff += n;
}

// Suspend unit test: do the submit on another thread
static _Atomic int g_sstarted, g_sdone; static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_scv = PTHREAD_COND_INITIALIZER;
static NVNqueue *g_sq; static NVNcommandHandle g_sh; static NVNT_nvnQueueSubmitCommands g_sub;
static void *susp_thr(void *a) { (void)a; pthread_mutex_lock(&g_smu); atomic_store(&g_sstarted, 1); pthread_cond_broadcast(&g_scv); pthread_mutex_unlock(&g_smu); g_sub(g_sq, 1, &g_sh); atomic_store(&g_sdone, 1); return NULL; }
static int wait_started(void) { struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 10; pthread_mutex_lock(&g_smu); while (!atomic_load(&g_sstarted) && pthread_cond_timedwait(&g_scv, &g_smu, &dl) == 0) {} int ok = atomic_load(&g_sstarted); pthread_mutex_unlock(&g_smu); return ok; }

static size_t tex_size3(int target, int fmt, int w, int h, int depth) {   // a layered or 3D texture's storage size
    NVNtextureBuilder tb;
    P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetTarget)(&tb, target);
    P(nvnTextureBuilderSetSize3D)(&tb, w, h, depth); P(nvnTextureBuilderSetFormat)(&tb, fmt);
    return P(nvnTextureBuilderGetStorageSize)(&tb);
}
static size_t tex_size(int fmt, int w, int h, int flags, size_t *align) {
    NVNtextureBuilder tb;
    P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetFlags)(&tb, flags); P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    P(nvnTextureBuilderSetWidth)(&tb, w); P(nvnTextureBuilderSetHeight)(&tb, h); P(nvnTextureBuilderSetFormat)(&tb, fmt);
    if (align) *align = P(nvnTextureBuilderGetStorageAlignment)(&tb);
    return P(nvnTextureBuilderGetStorageSize)(&tb);
}

// 5d's recorder: records small draws into one command buffer, re-adding the same memory well before it is full (so it never needs a host-lent segment), until told to stop
#define CMDSIZE_MARGIN 512   // room left for one more draw record and the END, whatever their sizes
typedef struct { NVNcommandBuffer *c; NVNmemoryPool *mp; size_t n; _Atomic int stop; _Atomic long recs; } cmdsize_rec_t;
static void *cmdsize_recorder(void *a) {
    cmdsize_rec_t *r = a;
    while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
        P(nvnCommandBufferAddCommandMemory)(r->c, r->mp, 0, r->n); P(nvnCommandBufferBeginRecording)(r->c);
        while (P(nvnCommandBufferGetCommandMemoryFree)(r->c) >= CMDSIZE_MARGIN) {
            P(nvnCommandBufferDrawElementsBaseVertex)(r->c, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, 0x10000, 5);
            atomic_fetch_add_explicit(&r->recs, 1, memory_order_relaxed);
        }
        P(nvnCommandBufferEndRecording)(r->c);
    }
    return NULL;
}
int main(void) {
    setenv("DM_NVN_COUNT", "1", 1);   // the call-count checks below need the counting trampolines (off by default)
    {   // A missing control section must not publish uninitialized bytes or poison a serial's key cache.
        uint8_t control[0x880] = {0}, key[8];
        const uint8_t expected[8] = {0x56, 0x26, 0xd9, 0xa8, 0xf1, 0xd7, 0x36, 0x4b}; // SHA-256 of 0x880 zero bytes
        prog_t p = {.nstage=1};
        for (int cached = 0; cached < 2; cached++) {
            p.serial = cached ? nvn_new_serial() : 0; p.sd[0].control = NULL;
            memset(key, 0xa5, sizeof key);
            EXPECT("missing shader control has no key", 0, prog_key(&p, 0, key));
            EXPECT("a failed key leaves the output untouched", 0xa5, key[0]);
            p.sd[0].control = control;
            EXPECT("supplying control after failure hashes it", 1, prog_key(&p, 0, key));
            EXPECT("a failed key was not cached as zero", 0, memcmp(key, expected, sizeof key));
        }
    }
    // Engine-native limiters have no FPS state page. Their launcher cap must
    // still pace presents and select the display-link rate in display mode.
    { win_t w={.interval=1};nvn_present_config(60,1);dm_fps_state=NULL;
      setenv("DM_FPS_TARGET","30",1);g_present_cap=-1;
      EXPECT("native 30 cap: display interval",2,present_itv(&w));
      EXPECT("native 30 cap: period ns",33333333,present_period(&w));
      EXPECT("native 30 cap: display-link target",30,nvn_game_fps());
      EXPECT("native cap does not enable frame-step stubs",0,fstep_on());
      setenv("DM_FPS_TARGET","60",1);g_present_cap=-1;
      EXPECT("native 60 cap: period ns",16666666,present_period(&w));
      EXPECT("native 60 cap: display-link target",60,nvn_game_fps());
      setenv("DM_FPS_TARGET","0",1);g_present_cap=-1;w.interval=2;
      EXPECT("no cap preserves display pacing",1,present_itv(&w));
      EXPECT("display mode: the game reads back the interval the window presents at",1,window_interval(&w));
      setenv("DM_FPS_TARGET","30",1);g_present_cap=-1;
      EXPECT("display mode under a 30 cap: the game reads back 2",2,window_interval(&w));
      setenv("DM_FPS_TARGET","0",1);g_present_cap=-1;nvn_present_config(60,0);
      EXPECT("no cap preserves game pacing",2,present_itv(&w));
      EXPECT("game mode: the game reads back the interval it set",2,window_interval(&w));
      // the 30 Hz link only for even intervals: interval 3 (20 fps) has no whole number of 30 Hz ticks and stays on the 60 Hz link
      w.interval=1;present_period(&w);w.interval=2;present_period(&w);g_itv2_since=(now_ns()-2000000000ull)|1;
      EXPECT("interval 2 held a second: 30 Hz link",30,nvn_game_fps());
      w.interval=3;present_period(&w);
      EXPECT("interval 3: 60 Hz link",60,nvn_game_fps());
      w.interval=4;present_period(&w);g_itv2_since=(now_ns()-2000000000ull)|1;
      EXPECT("interval 4 held a second: 30 Hz link",30,nvn_game_fps());
      w.interval=2;present_period(&w);
      EXPECT("4 to 2 keeps the run",30,nvn_game_fps());
      w.interval=3;present_period(&w);w.interval=2;present_period(&w);
      EXPECT("back to 2 from 3 waits a second again",60,nvn_game_fps());
      _Alignas(8) uint8_t state[64]={0};float cap=60;memcpy(state,&cap,4);dm_fps_state=state;
      EXPECT("stub state takes precedence over env",60,fps_present_cap());
      *(uint32_t *)(state+4)=1;
      EXPECT("the state page's movie flag is the host's copy, not read for the cap",60,fps_present_cap());
      dm_fps_state=NULL;unsetenv("DM_FPS_TARGET");g_present_cap=-1;nvn_present_config(0,0);
    }
    // No frame step page and no engine limiter the host writes (a main the profile's
    // stubs or limiter rows do not know): nothing makes the game follow the cap, so
    // presentation keeps the game's own interval (Tears of the Kingdom 1.4.2 at 2x).
    { win_t w={.interval=2};nvn_present_config(60,0);dm_fps_state=NULL;g_engine_stub=0;
      setenv("DM_FPS_TARGET","60",1);g_present_cap=-1;
      EXPECT("no limiter: the 60 cap is not applied",0,fps_present_cap());
      EXPECT("no limiter: the game's present interval stays",2,present_itv(&w));
      setenv("DM_FPS_TARGET","30",1);g_present_cap=-1;w.interval=1;
      EXPECT("no limiter: the 30 cap is not applied",1,present_itv(&w));
      _Alignas(8) uint8_t state[64]={0};float cap=60;memcpy(state,&cap,4);dm_fps_state=state;
      EXPECT("a frame step page needs no limiter",60,fps_present_cap());
      dm_fps_state=NULL;unsetenv("DM_FPS_TARGET");g_present_cap=-1;g_engine_stub=1;nvn_present_config(0,0);
    }
    // Display mode with no engine limiter (Red Dead Redemption: interval 2 set once, steps by time): the cap is the present interval alone
    { win_t w={.interval=2};nvn_present_config(60,1);dm_fps_state=NULL;g_engine_stub=0;
      setenv("DM_FPS_TARGET","60",1);g_present_cap=-1;
      EXPECT("display mode, no limiter: the 60 cap is applied",60,fps_present_cap());
      EXPECT("display mode, no limiter: one vsync a frame",1,present_itv(&w));
      EXPECT("display mode, no limiter: the game reads back 1",1,window_interval(&w));
      setenv("DM_FPS_TARGET","30",1);g_present_cap=-1;
      EXPECT("display mode, no limiter: the 30 cap is applied",30,fps_present_cap());
      EXPECT("display mode, no limiter: two vsyncs a frame",2,present_itv(&w));
      EXPECT("display mode, no limiter: 30 Hz display-link target",30,nvn_game_fps());
      setenv("DM_FPS_TARGET","0",1);g_present_cap=-1;
      EXPECT("display mode, no limiter, no cap: one frame per tick",1,present_itv(&w));
      nvn_present_config(-1,0);setenv("DM_PRESENT_INTERVAL","display",1);setenv("DM_FPS_TARGET","30",1);g_present_cap=-1;
      EXPECT("the cap asked for before the window's first interval: display mode from the environment",30,fps_present_cap());
      unsetenv("DM_PRESENT_INTERVAL");unsetenv("DM_FPS_TARGET");g_present_cap=-1;g_engine_stub=1;nvn_present_config(0,0);
    }
    crash_install();   // the same SIGSEGV / SIGBUS handling as production (the page-fault hook is dispatched from within it)
    // 1. Bootstrap address lookup: bootstrap gets GetProcAddress, then uses it to get all the names
    PFNNVNBOOTSTRAPLOADERPROC boot = (PFNNVNBOOTSTRAPLOADERPROC)h_nvnBootstrapLoader;
    gpa = (PFNNVNDEVICEGETPROCADDRESSPROC)boot("nvnDeviceGetProcAddress");
    EXPECT("bootstrap GetProcAddress is non-null", 1, gpa != NULL);
    int nonnull = 0; for (unsigned i = 0; i < NVN_NFUNC; i++) nonnull += gpa(NULL, nvn_names[i]) != NULL;
    EXPECT("every name in the name table resolves (NVN_NFUNC)", NVN_NFUNC, nonnull);   // used to be hard-coded 523 and kept failing once the name table grew to 536
    EXPECT("unknown name resolves to null", 0, gpa(NULL, "nvnNoSuchFunction") != NULL);

    // 2. Device
    NVNdeviceBuilder db; P(nvnDeviceBuilderSetDefaults)(&db); P(nvnDeviceBuilderSetFlags)(&db, 0);
    EXPECT("DeviceInitialize succeeds", 1, P(nvnDeviceInitialize)(&dev, &db));
    int v; P(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v);
    EXPECT("uniform buffer alignment (256 measured on X1)", 256, v);
    P(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE, &v); EXPECT("texture descriptor size (32)", 32, v);
    P(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_SIZE, &v); EXPECT("queue command memory minimum (65536)", 65536, v);
    // Precompiled shaders need not match the linked SDK: ASTRAL CHAIN (SDK 6.4)
    // ships GPU code 1.15. Advertise the translator's supported range even with no SDK.
    P(nvnDeviceGetInteger)(&dev, (NVNdeviceInfo)0x2E, &v); EXPECT("newest GPU code major", 1, v);
    P(nvnDeviceGetInteger)(&dev, (NVNdeviceInfo)0x30, &v); EXPECT("newest GPU code minor", 16, v);
    P(nvnDeviceGetInteger)(&dev, (NVNdeviceInfo)0x2F, &v); EXPECT("oldest GPU code major", 1, v);
    P(nvnDeviceGetInteger)(&dev, (NVNdeviceInfo)0x31, &v); EXPECT("oldest GPU code minor", 5, v);
    // The Switch 2 table (NVN2's own constants: the newest seen, SDK 21.4.4's, as an older sdk's own switch answers its older versions through dm_sdk_nvn2_item): its GPU code range and Ampere's limits, the host's 16 vertex slots, and the run-time items
    EXPECT("NVN2: API 231.39", 231 * 100 + 39, g_devinfo2[0] * 100 + g_devinfo2[1]);
    EXPECT("NVN2: GPU code 201.0 to 201.20", 1, g_devinfo2[0x2E] == 201 && g_devinfo2[0x30] == 20 && g_devinfo2[0x2F] == 201 && g_devinfo2[0x31] == 0);
    EXPECT("NVN2: 1,024 compute threads, an unbounded dispatch width", 1, g_devinfo2[0x1F] == 1024 && g_devinfo2[0x20] == 2147483647);
    EXPECT("NVN2: the host's 16 vertex bindings, not Ampere's 32, and its 32 attributes", 1, g_devinfo2[0x06] == 16 && g_devinfo2[0x0E] == 32);
    EXPECT("NVN2: the memory pool page size is read at run time", DEVINFO_RT, g_devinfo2[NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE]);
    EXPECT("NVN2: items up to 0xA9", 0xAA, (int)(sizeof g_devinfo2 / sizeof *g_devinfo2));

    NVNsamplerBuilder sb; NVNsampler sampler;
    P(nvnSamplerBuilderSetDefaults)(&sb);
    const uint32_t opaque_black[4] = {0, 0, 0, 1};
    P(nvnSamplerBuilderSetBorderColorui)(&sb, opaque_black);
    P(nvnSamplerInitialize)(&sampler, &sb);
    EXPECT("unsigned integer border alpha is numeric", 1, ((samp_t *)&sampler)->p.border[3] == 1.f);
    P(nvnSamplerFinalize)(&sampler);
    const int opaque_white[4] = {1, 1, 1, 1};
    P(nvnSamplerBuilderSetBorderColori)(&sb, opaque_white);
    P(nvnSamplerInitialize)(&sampler, &sb);
    EXPECT("signed integer border RGB is numeric", 1, ((samp_t *)&sampler)->p.border[0] == 1.f && ((samp_t *)&sampler)->p.border[2] == 1.f);
    P(nvnSamplerFinalize)(&sampler);
    {   // Games tell samplers apart by their bytes from 8 on: Super Mario Odyssey's sampler cache keys entries by a CRC-16 (polynomial 0xA001) of them alone (nvn_obj.h samp_t)
        NVNsampler s2;
        #define SAMP_CRC(s) crc16_a001((const uint8_t *)&(s) + 8, sizeof(NVNsampler) - 8)
        P(nvnSamplerBuilderSetDefaults)(&sb);
        memset(&sampler, 0xA5, sizeof sampler); memset(&s2, 0x5A, sizeof s2);   // object memory with different old bytes
        P(nvnSamplerInitialize)(&sampler, &sb); P(nvnSamplerInitialize)(&s2, &sb);
        EXPECT("equal samplers: equal bytes from 8 on", 1, !memcmp((const uint8_t *)&sampler + 8, (const uint8_t *)&s2 + 8, sizeof sampler - 8));
        uint16_t base = SAMP_CRC(s2);   // the same object for the base and the changes, so only the state differs
        P(nvnSamplerBuilderSetMinMagFilter)(&sb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST); P(nvnSamplerInitialize)(&s2, &sb);
        EXPECT("filter alone changes the bytes from 8 on", 1, SAMP_CRC(s2) != base);
        P(nvnSamplerBuilderSetDefaults)(&sb); P(nvnSamplerBuilderSetWrapMode)(&sb, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_MIRRORED_REPEAT, NVN_WRAP_MODE_REPEAT);
        P(nvnSamplerInitialize)(&s2, &sb);
        EXPECT("wrap alone changes the bytes from 8 on", 1, SAMP_CRC(s2) != base);
        #undef SAMP_CRC
        P(nvnSamplerFinalize)(&sampler); P(nvnSamplerFinalize)(&s2);
    }

    // 3. Memory pool: GPU address = the storage address given by the game
    static uint8_t poolmem[1 << 20] __attribute__((aligned(4096)));
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    P(nvnMemoryPoolBuilderSetDevice)(&pb, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&pb);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, poolmem, sizeof poolmem); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24);
    EXPECT("MemoryPoolInitialize succeeds", 1, P(nvnMemoryPoolInitialize)(&pool, &pb));
    EXPECT("pool GPU address = storage address", (long long)(uintptr_t)poolmem, P(nvnMemoryPoolGetBufferAddress)(&pool));

    // Engines may retarget a ring buffer with raw stores instead of SetStorage.
    // These SDK offsets come from the guest writes reported in issue #723;
    // deliberately do not use buf_t or its offsets to construct the writes.
    {
        NVNbufferBuilder bb = {0};
        struct { uint64_t before; NVNbuffer buffer; uint64_t after; } guarded = {
            .before = UINT64_C(0x1122334455667788), .after = UINT64_C(0x8877665544332211)
        };
        NVNbuffer *buffer = &guarded.buffer;
        P(nvnBufferBuilderSetDevice)(&bb, &dev); P(nvnBufferBuilderSetDefaults)(&bb);
        P(nvnBufferBuilderSetStorage)(&bb, &pool, 0x100, 0x200);
        EXPECT("ring buffer initializes", 1, P(nvnBufferInitialize)(buffer, &bb));
        EXPECT("initial buffer address", (uintptr_t)poolmem + 0x100, P(nvnBufferGetAddress)(buffer));
        EXPECT("initial buffer size", 0x200, P(nvnBufferGetSize)(buffer));
        uint64_t initial_offset, initial_size;
        memcpy(&initial_offset, (uint8_t *)buffer + 0x18, 8);
        memcpy(&initial_size, (uint8_t *)buffer + 0x20, 8);
        EXPECT("initialized guest offset field", 0x100, initial_offset);
        EXPECT("initialized guest size field", 0x200, initial_size);
        const struct { uint64_t offset, size; } allocations[] = {{0x4000, 0x100}, {0x8000, 0x300}, {0, 0}};
        for (unsigned i = 0; i < sizeof allocations / sizeof allocations[0]; i++) {
            memcpy((uint8_t *)buffer + 0x18, &allocations[i].offset, 8);
            memcpy((uint8_t *)buffer + 0x20, &allocations[i].size, 8);
            EXPECT("guest ring store changes address", (uintptr_t)poolmem + allocations[i].offset, P(nvnBufferGetAddress)(buffer));
            EXPECT("guest ring store changes map", (uintptr_t)poolmem + allocations[i].offset, (uintptr_t)P(nvnBufferMap)(buffer));
            EXPECT("guest ring store changes offset", allocations[i].offset, P(nvnBufferGetMemoryOffset)(buffer));
            EXPECT("guest ring store changes size", allocations[i].size, P(nvnBufferGetSize)(buffer));
            EXPECT("guest ring store preserves pool", (uintptr_t)&pool, (uintptr_t)P(nvnBufferGetMemoryPool)(buffer));
        }
        EXPECT("ring stores preserve builder offset", 0x100, P(nvnBufferBuilderGetMemoryOffset)(&bb));
        EXPECT("ring stores preserve builder size", 0x200, P(nvnBufferBuilderGetSize)(&bb));
        P(nvnBufferFinalize)(buffer);
        EXPECT("buffer leaves preceding object intact", UINT64_C(0x1122334455667788), guarded.before);
        EXPECT("buffer leaves following object intact", UINT64_C(0x8877665544332211), guarded.after);

        static uint8_t private_mem[4096] __attribute__((aligned(4096)));
        NVNmemoryPoolBuilder private_pb = {0}; NVNmemoryPool private_pool;
        P(nvnMemoryPoolBuilderSetDevice)(&private_pb, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&private_pb);
        P(nvnMemoryPoolBuilderSetStorage)(&private_pb, private_mem, sizeof private_mem);
        P(nvnMemoryPoolBuilderSetFlags)(&private_pb, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        EXPECT("GPU-only pool initializes", 1, P(nvnMemoryPoolInitialize)(&private_pool, &private_pb));
        P(nvnBufferBuilderSetStorage)(&bb, &private_pool, 0x100, 0x200);
        EXPECT("GPU-only buffer initializes", 1, P(nvnBufferInitialize)(buffer, &bb));
        uint64_t offset = 0x400, size = 0x80;
        memcpy((uint8_t *)buffer + 0x18, &offset, 8); memcpy((uint8_t *)buffer + 0x20, &size, 8);
        EXPECT("GPU-only retargeted address", (uintptr_t)private_mem + 0x400, P(nvnBufferGetAddress)(buffer));
        EXPECT("GPU-only retargeted size", 0x80, P(nvnBufferGetSize)(buffer));
        EXPECT("GPU-only buffer stays unmappable", 0, (uintptr_t)P(nvnBufferMap)(buffer));
        P(nvnBufferFinalize)(buffer); P(nvnMemoryPoolFinalize)(&private_pool);
    }

    // 4. Texture storage size (block-linear: GOB = 64 B × 8 rows; block height from the GOB count of "height × 1.5": ≥16→16, ≥8→8 ...)
    //    256×256 RGBA8: 1024 B per row = 16 GOBs; height 256 rows = 32 GOB rows, block height 16 GOBs → 2 blocks; 16×2×(512×16) = 262144
    size_t al; EXPECT("256x256 RGBA8 size", 262144, tex_size(NVN_FORMAT_RGBA8, 256, 256, 0, &al));
    EXPECT("block-linear alignment", 512, al);
    //    100×100 RGBA8: 400 B → 7 GOBs wide; height 100 → 13 GOB rows, ×1.5 = 150 rows = 19 GOBs → block height 16 → 1 block; 7×1×8192 = 57344
    EXPECT("100x100 RGBA8 size", 57344, tex_size(NVN_FORMAT_RGBA8, 100, 100, 0, NULL));
    //    128×128 DXT1: 32×32 blocks, 8 B/block → 256 B = 4 GOBs wide; 32 rows → ×1.5 = 48 rows = 6 GOBs → block height 4 → 4 GOB rows = 1 block; 4×1×2048 = 8192
    EXPECT("128x128 DXT1 size", 8192, tex_size(NVN_FORMAT_RGBA_DXT1, 128, 128, 0, NULL));
    //    LINEAR 100×100 RGBA8: row pitch 400 aligned to 32 = 416; 416×100 = 41600, aligned to 32
    EXPECT("linear 100x100 RGBA8 size", 41600, tex_size(NVN_FORMAT_RGBA8, 100, 100, NVN_TEXTURE_FLAGS_LINEAR, &al));
    EXPECT("linear alignment", 32, al);
    // A cube array's depth counts layer-faces, as in OpenGL: 12 = two cubes, the size of a 12-layer 2D array (it used to be read as 12 cubes, six times the storage)
    EXPECT("64x64 RGBA8 cube array of depth 12 = 2D array of 12 layers", tex_size3(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 64, 64, 12),
           tex_size3(NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, NVN_FORMAT_RGBA8, 64, 64, 12));

    // A channel change on the same storage must survive the game's descriptor
    // deduplication. Exercise the public builder/init/compare API, including
    // default versus explicitly specified identity channel selection.
    {
        NVNtextureBuilder tb; NVNtexture original = {0}, replacement = {0};
        P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
        NVNtextureSwizzle swz[4] = {0};
        P(nvnTextureBuilderGetSwizzle)(&tb, &swz[0], &swz[1], &swz[2], &swz[3]);
        EXPECT("TextureBuilderGetSwizzle default identity", 1, swz[0] == 2 && swz[1] == 3 && swz[2] == 4 && swz[3] == 5);
        P(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R8);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
        P(nvnTextureInitialize)(&original, &tb);
        P(nvnTextureBuilderSetSwizzle)(&tb, 2, 3, 4, 5);
        P(nvnTextureInitialize)(&replacement, &tb);
        EXPECT("TextureCompare implicit/explicit identity", 1, P(nvnTextureCompare)(&original, &replacement));
        P(nvnTextureFinalize)(&replacement);
        P(nvnTextureBuilderSetSwizzle)(&tb, 1, 1, 1, 2);
        P(nvnTextureBuilderGetSwizzle)(&tb, &swz[0], &swz[1], &swz[2], &swz[3]);
        EXPECT("TextureBuilderGetSwizzle reads what was set", 1, swz[0] == 1 && swz[1] == 1 && swz[2] == 1 && swz[3] == 2);
        P(nvnTextureInitialize)(&replacement, &tb);
        EXPECT("TextureCompare A8 channel replacement", 0, P(nvnTextureCompare)(&original, &replacement));
        EXPECT("TextureCompare channel replacement symmetric", 0, P(nvnTextureCompare)(&replacement, &original));
        P(nvnTextureFinalize)(&original); P(nvnTextureFinalize)(&replacement);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGB10A2);
        P(nvnTextureBuilderSetSwizzle)(&tb, 2, 3, 4, 0);
        P(nvnTextureInitialize)(&original, &tb);
        P(nvnTextureBuilderSetSwizzle)(&tb, 2, 3, 4, 1);
        P(nvnTextureInitialize)(&replacement, &tb);
        EXPECT("TextureCompare capture alpha zero/one", 0, P(nvnTextureCompare)(&original, &replacement));
        EXPECT("TextureCompare same capture descriptor", 1, P(nvnTextureCompare)(&replacement, &replacement));
        P(nvnTextureFinalize)(&original); P(nvnTextureFinalize)(&replacement);
    }

    // Flushed ranges (nvn_flushed): a texture's storage flushed before its first use is filled from memory (tex_init_fill). Spans merge when they overlap or touch, and go with their pool
    {
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x1000, 0x100);
        EXPECT("flushed: the flushed range", 1, nvn_flushed(poolmem + 0x1000, 0x100));
        EXPECT("flushed: its last byte", 1, nvn_flushed(poolmem + 0x10ff, 1));
        EXPECT("flushed: a range ending at its start", 0, nvn_flushed(poolmem + 0xf00, 0x100));
        EXPECT("flushed: a range starting at its end", 0, nvn_flushed(poolmem + 0x1100, 0x100));
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x1100, 0x100);
        EXPECT("flushed: a touching flush merges (1 span)", 1, (long long)g_nfl);
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x3000, 0x10);
        EXPECT("flushed: a separate flush (2 spans)", 2, (long long)g_nfl);
        EXPECT("flushed: the gap between them", 0, nvn_flushed(poolmem + 0x1200, 0x1e00));
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x1080, 0x40);
        EXPECT("flushed: a flush inside a span (still 2)", 2, (long long)g_nfl);
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0, 0x4000);
        EXPECT("flushed: a flush over both (1 span)", 1, (long long)g_nfl);
        fl_drop(poolmem + 0x1000, 0x1000);   // a pool inside the span: split
        EXPECT("flushed: a pool dropped from inside a span (2 spans)", 2, (long long)g_nfl);
        EXPECT("flushed: the dropped part", 0, nvn_flushed(poolmem + 0x1000, 0x1000));
        EXPECT("flushed: before it", 1, nvn_flushed(poolmem + 0xfff, 1));
        EXPECT("flushed: after it", 1, nvn_flushed(poolmem + 0x2000, 1));
        static uint8_t smem[0x2000] __attribute__((aligned(4096))); NVNmemoryPool sp;
        P(nvnMemoryPoolBuilderSetStorage)(&pb, smem, sizeof smem);
        P(nvnMemoryPoolInitialize)(&sp, &pb);
        P(nvnMemoryPoolFlushMappedRange)(&sp, 0x800, 0x800);
        EXPECT("flushed: a second pool's flush", 1, nvn_flushed(smem + 0x800, 1));
        P(nvnMemoryPoolFinalize)(&sp);
        EXPECT("flushed: gone with its pool", 0, nvn_flushed(smem, sizeof smem));
        fl_drop(poolmem, sizeof poolmem);
        EXPECT("flushed: the first pool's spans dropped", 0, (long long)g_nfl);
        P(nvnMemoryPoolBuilderSetStorage)(&pb, poolmem, sizeof poolmem);
    }

    // 5. Command buffer: record uniform updates / events / counters / buffer copies / nested calls, with memory in 256 B segments to force multi-segment linking
    NVNcommandBuffer cb; P(nvnCommandBufferInitialize)(&cb, &dev);
    NVNmemoryPoolBuilder cpb; P(nvnMemoryPoolBuilderSetDefaults)(&cpb); P(nvnMemoryPoolBuilderSetStorage)(&cpb, g_cmdmem, sizeof g_cmdmem);
    P(nvnMemoryPoolInitialize)(&g_cmdpool, &cpb);
    P(nvnCommandBufferSetMemoryCallback)(&cb, mem_cb); P(nvnCommandBufferSetMemoryCallbackData)(&cb, NULL);
    NVNeventBuilder eb; NVNevent ev; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb, &pool, 0x1000);
    P(nvnEventInitialize)(&ev, &eb);
    EXPECT("EventGetMemoryPool: the storage pool", 1, ((NVNmemoryPool *(*)(const NVNevent *))proc("nvnEventGetMemoryPool"))(&ev) == &pool);
    EXPECT("EventGetMemoryOffset: the storage offset", 0x1000, ((int64_t (*)(const NVNevent *))proc("nvnEventGetMemoryOffset"))(&ev));
    uint8_t *ub = poolmem + 0x2000, *cnt = poolmem + 0x3000, *src = poolmem + 0x4000, *dst = poolmem + 0x5000;
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 3 + 1);
    uint32_t data[16];

    P(nvnCommandBufferBeginRecording)(&cb);
    EXPECT("recording", 1, P(nvnCommandBufferIsRecording)(&cb));
    for (int k = 0; k < 20; k++) {                        // 20 updates to different offsets of the same buffer, 64 B of data each
        for (int i = 0; i < 16; i++) data[i] = k * 100 + i;
        P(nvnCommandBufferUpdateUniformBuffer)(&cb, (NVNbufferAddress)ub, 4096, k * 64, 64, data);
        memset(data, 0xEE, sizeof data);                  // modify the original data after recording: the submit must use the values copied in at record time
    }
    P(nvnCommandBufferSignalEvent)(&cb, &ev, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 7);
    P(nvnCommandBufferSignalEvent)(&cb, &ev, NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 5);
    P(nvnCommandBufferReportCounter)(&cb, NVN_COUNTER_TYPE_TIMESTAMP, (NVNbufferAddress)cnt);
    NVNcommandHandle h1 = P(nvnCommandBufferEndRecording)(&cb);
    EXPECT("not recording after EndRecording", 0, P(nvnCommandBufferIsRecording)(&cb));
    P(nvnCommandBufferBeginRecording)(&cb);
    P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)src, (NVNbufferAddress)dst, 64, 0);
    P(nvnCommandBufferCopyCommands)(&cb, 1, &h1);         // nested: execute h1 once more (event 7 → +5 → write 7 again → +5)
    NVNcommandHandle h2 = P(nvnCommandBufferEndRecording)(&cb);
    printf("(command memory callback called %d times, %zu B used)\n", g_cb_calls, g_cmdoff);
    EXPECT("command memory callback at least twice (multi-segment)", 1, g_cb_calls >= 2);
    EXPECT("event value still 0 before submit", 0, P(nvnEventGetValue)(&ev));

    NVNqueueBuilder qb; NVNqueue q; static uint8_t qmem[1 << 19];
    P(nvnQueueBuilderSetDevice)(&qb, &dev); P(nvnQueueBuilderSetDefaults)(&qb);
    size_t qsz = P(nvnQueueBuilderGetQueueMemorySize)(&qb);
    EXPECT("queue memory = 64K+256K+16K rounded to 4K", 65536 + 262144 + 16384, qsz);
    P(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    EXPECT("QueueInitialize succeeds", 1, P(nvnQueueInitialize)(&q, &qb));
    NVNcommandHandle hs[2] = {h1, h2};
    uint64_t counter_before = now_ns();
    P(nvnQueueSubmitCommands)(&q, 2, hs);
    NVNsync sf; P(nvnSyncInitialize)(&sf, &dev); P(nvnQueueFenceSync)(&q, &sf, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    int wr = P(nvnSyncWait)(&sf, 5000000000ull);
    uint64_t counter_after = now_ns();
    EXPECT("fence wait after submit succeeds (0 or 1)", 1, wr == 0 || wr == 1);
    int ub_ok = 1;
    for (int k = 0; k < 20; k++) for (int i = 0; i < 16; i++) { uint32_t w; memcpy(&w, ub + k * 64 + i * 4, 4); ub_ok &= w == (uint32_t)(k * 100 + i); }
    EXPECT("uniform buffer 20×16 words equal the recorded values", 1, ub_ok);
    EXPECT("event value: h1(7,+5), then h2 copies and runs h1(7,+5) again = 12", 12, P(nvnEventGetValue)(&ev));
    EXPECT("buffer copy 64 B identical", 0, memcmp(src, dst, 64));
    uint64_t ts[2]; memcpy(ts, cnt, 16);
    EXPECT("counter timestamp written (non-zero)", 1, ts[1] != 0);
    EXPECT("its counter field: the time, 0 with the profile's render_cond (as on the GPU)", DM_GAME_RENDER_COND ? 0 : ts[1], ts[0]);
    NVNcounterData fixed_counter = {0, 614400};
    EXPECT("614400 raw counter ticks = 1 ms", 1000000, P(nvnDeviceGetTimestampInNanoseconds)(&dev, &fixed_counter));
    NVNcounterData reported_counter; memcpy(&reported_counter, cnt, sizeof reported_counter);
    uint64_t counter_ns = P(nvnDeviceGetTimestampInNanoseconds)(&dev, &reported_counter);
    EXPECT("report converted to ns lies inside submit/wait", 1, counter_ns + 2 >= counter_before && counter_ns <= counter_after);

    // 5c. Copy strides are command buffer state (NVN): setting them takes no command memory, Get reads them back, and a copy recorded after them carries them.
    //     Super Smash Bros. Ultimate records one upload in 256 bytes of command memory and 256 of control memory (main+0x3664960): SetCopyRowStride, SetCopyImageStride,
    //     CopyBufferToTexture, on a command buffer whose out-of-memory handler is null; recorded as two 24-byte records, the copy overflowed and the game's callback jumped to 0
    {
        static uint8_t m[256] __attribute__((aligned(4096))), ctl[256]; NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c3;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        NVNtextureBuilder tb; NVNtexture tx = {0}; P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
        P(nvnTextureBuilderSetSize2D)(&tb, 8, 8); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetStorage)(&tb, &pool, 0); P(nvnTextureInitialize)(&tx, &tb);
        P(nvnCommandBufferInitialize)(&c3, &dev); P(nvnCommandBufferSetMemoryCallback)(&c3, no_mem_cb); P(nvnCommandBufferSetMemoryCallbackData)(&c3, NULL); g_nocb_calls = 0;
        EXPECT("copy row stride starts at 0", 0, P(nvnCommandBufferGetCopyRowStride)(&c3));
        P(nvnCommandBufferAddControlMemory)(&c3, ctl, sizeof ctl);
        P(nvnCommandBufferAddCommandMemory)(&c3, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c3);
        P(nvnCommandBufferSetCopyRowStride)(&c3, 64); P(nvnCommandBufferSetCopyImageStride)(&c3, 512);
        EXPECT("setting the copy strides takes no command memory", 0, P(nvnCommandBufferGetCommandMemoryUsed)(&c3));
        EXPECT("GetCopyRowStride reads the row stride back", 64, P(nvnCommandBufferGetCopyRowStride)(&c3));
        EXPECT("GetCopyImageStride reads the image stride back", 512, P(nvnCommandBufferGetCopyImageStride)(&c3));
        NVNcopyRegion rg = {0, 0, 0, 8, 8, 1};
        P(nvnCommandBufferCopyBufferToTexture)(&c3, 0x10000, &tx, NULL, &rg, 0);
        P(nvnCommandBufferEndRecording)(&c3);
        EXPECT("one upload with strides fits 256 bytes without the out-of-memory callback", 0, g_nocb_calls);
        op_hdr rh; memcpy(&rh, m, sizeof rh); const p_copy_strides *cs = copy_strides(m + 8, rh.size);
        EXPECT("the copy carries the strides: 64 per row, 512 per image", 1, rh.op == OP_COPY_B2T && cs && cs->row == 64 && cs->image == 512);
        P(nvnCommandBufferSetCopyRowStride)(&c3, 0); P(nvnCommandBufferSetCopyImageStride)(&c3, 0);
        P(nvnCommandBufferAddControlMemory)(&c3, ctl, sizeof ctl);
        P(nvnCommandBufferAddCommandMemory)(&c3, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c3);
        P(nvnCommandBufferCopyBufferToTexture)(&c3, 0x10000, &tx, NULL, &rg, 0); P(nvnCommandBufferEndRecording)(&c3);
        memcpy(&rh, m, sizeof rh);
        EXPECT("a copy with the default strides keeps its size and has no strides", 1, rh.size == 8 + COPY_STRIDES_OFF && !copy_strides(m + 8, rh.size));
        P(nvnTextureFinalize)(&tx); P(nvnCommandBufferFinalize)(&c3);
    }

    // 5d. GetCommandMemorySize is the size of the memory the game last added, stable while another thread records into the buffer (GitHub issue #768): Super Smash Bros. Ultimate
    //     sorts its pool of command buffers by it while its other render dispatch thread records into buffers of that pool. Summed per call, a reader saw sizes torn by the records
    {
        static uint8_t m[4096] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c4;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c4, &dev);
        P(nvnCommandBufferAddCommandMemory)(&c4, &mp, 0, sizeof m);
        EXPECT("size after AddCommandMemory: the memory added", sizeof m, P(nvnCommandBufferGetCommandMemorySize)(&c4));
        P(nvnCommandBufferBeginRecording)(&c4);
        P(nvnCommandBufferDrawElementsBaseVertex)(&c4, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, 0x10000, 5);
        EXPECT("size unchanged by a record", sizeof m, P(nvnCommandBufferGetCommandMemorySize)(&c4));
        EXPECT("size = used + free in the memory the game added", 1, P(nvnCommandBufferGetCommandMemorySize)(&c4) == P(nvnCommandBufferGetCommandMemoryUsed)(&c4) + P(nvnCommandBufferGetCommandMemoryFree)(&c4));
        // Past the memory added (no callback): the recording goes on in a host-lent segment, which is not the game's memory and leaves the size as the game set it
        for (int i = 0; i < 200; i++) P(nvnCommandBufferDrawElementsBaseVertex)(&c4, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, 0x10000, 5);
        EXPECT("in a host-lent segment: size is still the memory added", 1, P(nvnCommandBufferGetCommandMemorySize)(&c4) == sizeof m && P(nvnCommandBufferGetCommandMemoryUsed)(&c4) > sizeof m);
        P(nvnCommandBufferEndRecording)(&c4);
        // Memory added at an offset the host aligns past: the size is the bytes the game passed, not the host's segment
        P(nvnCommandBufferAddCommandMemory)(&c4, &mp, 4, sizeof m - 4);
        EXPECT("memory added at offset 4: size is the bytes passed", sizeof m - 4, P(nvnCommandBufferGetCommandMemorySize)(&c4));
        // Memory added in the middle of a recording (a game's out-of-memory callback): the size is the new memory's
        P(nvnCommandBufferBeginRecording)(&c4);
        P(nvnCommandBufferDrawElementsBaseVertex)(&c4, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, 0x10000, 5);
        P(nvnCommandBufferAddCommandMemory)(&c4, &mp, 0, sizeof m);
        EXPECT("memory added during a recording: size is that memory", sizeof m, P(nvnCommandBufferGetCommandMemorySize)(&c4));
        P(nvnCommandBufferEndRecording)(&c4);
        // Another thread records into the same memory over and over: every size read here is the memory added
        cmdsize_rec_t r = { .c = &c4, .mp = &mp, .n = sizeof m }; pthread_t th; long torn = 0;
        P(nvnCommandBufferAddCommandMemory)(&c4, &mp, 0, sizeof m);
        EXPECT("the recorder thread starts", 0, pthread_create(&th, NULL, cmdsize_recorder, &r));
        for (uint64_t w = now_ns(); !atomic_load_explicit(&r.recs, memory_order_relaxed) && now_ns() - w < 2000000000ull; ) {}
        for (uint64_t t0 = now_ns(); now_ns() - t0 < 200000000ull; )
            for (int i = 0; i < 4096; i++) torn += P(nvnCommandBufferGetCommandMemorySize)(&c4) != sizeof m;
        atomic_store_explicit(&r.stop, 1, memory_order_relaxed); pthread_join(th, NULL);
        EXPECT("the recorder recorded while the sizes were read", 1, atomic_load_explicit(&r.recs, memory_order_relaxed) > 0);
        EXPECT("size reads during another thread's recording: none torn", 0, torn);
        P(nvnCommandBufferFinalize)(&c4); P(nvnMemoryPoolFinalize)(&mp);
    }

    // 5a. Command memory per recording, as GetCommandMemoryUsed reports it (games size prerecorded command blocks by it: Breath of the Wild records one scratch draw per mesh):
    //     no BEGIN record and no END in command memory, a plain indexed draw without the indirect / multi-draw fields; the handle is the recording's control list, in the control
    //     memory the game added (one 8-byte entry for a recording in one piece); an indirect draw keeps its address
    {
        static uint8_t m[4096] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c2;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c2, &dev);
        static uint8_t ctl[512] __attribute__((aligned(8)));
        EXPECT("control free before AddControlMemory", 0, P(nvnCommandBufferGetControlMemoryFree)(&c2));
        P(nvnCommandBufferAddControlMemory)(&c2, ctl, sizeof ctl);
        EXPECT("control block size after grant", sizeof ctl, P(nvnCommandBufferGetControlMemorySize)(&c2));
        EXPECT("control block free after grant", sizeof ctl, P(nvnCommandBufferGetControlMemoryFree)(&c2));
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferDrawElementsBaseVertex)(&c2, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, 0x10000, 5);
        NVNcommandHandle hd = P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("plain indexed draw recording: 24 bytes (the driver's 48-72), no END", 24, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        EXPECT("its control list: one entry", 8, P(nvnCommandBufferGetControlMemoryUsed)(&c2));
        EXPECT("control free = size - used", sizeof ctl - 8, P(nvnCommandBufferGetControlMemoryFree)(&c2));
        op_hdr rh; memcpy(&rh, m, sizeof rh); int compact = rec_compact(rh.op);
        uint8_t *L = lin(hd); memcpy(&rh, L, sizeof rh); p_draw_idx dt; const p_draw_idx *d = draw_idx_rec(L + 8, rh.size, &dt);
        EXPECT("its handle is its control list, one piece: the compact draw, expanded to OP_DRAW_IDX", 1, (const uint8_t *)hd == ctl && one_piece(hd, m, 24) && compact && rh.op == OP_DRAW_IDX);
        P(nvnCommandBufferAddControlMemory)(&c2, ctl, 128);
        EXPECT("new control block replaces capacity", 128, P(nvnCommandBufferGetControlMemoryFree)(&c2));
        EXPECT("draw read back: count, base vertex, no indirect address", 1, d->count == 36 && d->base_vertex == 5 && d->ibuf == 0x10000 && !d->ind && !d->mdi_cnt);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2); NVNcommandHandle he = P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("empty recording: no command memory", 0, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        EXPECT("empty recording: one control entry, an empty piece", 1, P(nvnCommandBufferGetControlMemoryUsed)(&c2) == 8 && (const uint8_t *)he == ctl && one_piece(he, NULL, 0));
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferDrawElementsIndirect)(&c2, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 0x10000, 0x20000);
        NVNcommandHandle hi = P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("indirect indexed draw: 24 bytes (the driver's 36)", 24, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        L = lin(hi); memcpy(&rh, L, sizeof rh); d = draw_idx_rec(L + 8, rh.size, &dt);
        EXPECT("indirect draw keeps its address", 0x20000, d->ind);
        // A single texture bind keeps its handle 16 bytes into the command, where a game that patches recorded commands writes a new one (Luigi's Mansion 3: base + GetCommandMemoryUsed + 0x10)
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        size_t at = P(nvnCommandBufferGetCommandMemoryUsed)(&c2);
        P(nvnCommandBufferBindTexture)(&c2, NVN_SHADER_STAGE_FRAGMENT, 3, (NVNtextureHandle)(0x1ff00000u | 1234));
        P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("single texture bind recording, first pools: 0x18", 0x18, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        uint32_t patched = 0x1ff00000u | 1235; memcpy(m + at + 0x10, &patched, 4);
        memcpy(&rh, m, sizeof rh); p_bind_handles bt; const p_bind_handles *bh = bind_rec(rh.op, m + 8, &bt);
        EXPECT("the bind reads the handle patched at its start + 0x10: slot 3, texture 1235, sampler 511", 1, rh.op == OP_BIND_TEX1 && bh->n == 1 && bh->first == 3 && bh->stage == NVN_SHADER_STAGE_FRAGMENT && bh->h[0] == patched);
        // With another pool bound the record keeps the pool bases after the handle, and the read adds them
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        ((cmd_t *)&c2)->tex_base = 7; ((cmd_t *)&c2)->samp_base = 3;
        P(nvnCommandBufferBindTexture)(&c2, NVN_SHADER_STAGE_VERTEX, 2, (NVNtextureHandle)(5u << 20 | 1234));
        P(nvnCommandBufferEndRecording)(&c2); ((cmd_t *)&c2)->tex_base = ((cmd_t *)&c2)->samp_base = 0;
        EXPECT("single texture bind recording, other pools: 0x20", 0x20, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        memcpy(&rh, m, sizeof rh); bh = bind_rec(rh.op, m + 8, &bt);
        EXPECT("the bind adds the pool bases: stage, slot 2, texture 1241, sampler 8", 1, bh->stage == NVN_SHADER_STAGE_VERTEX && bh->first == 2 && bh->h[0] == (8u << 20 | 1241));
        // State binds carry only the state object's bytes: 16 bytes for a 4-byte state, 24 for an 8-byte one or the multisample state
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        NVNpolygonState ps; P(nvnPolygonStateSetDefaults)(&ps); P(nvnPolygonStateSetCullFace)(&ps, NVN_FACE_BACK); P(nvnCommandBufferBindPolygonState)(&c2, &ps);
        NVNdepthStencilState dss; P(nvnDepthStencilStateSetDefaults)(&dss); P(nvnDepthStencilStateSetDepthFunc)(&dss, NVN_DEPTH_FUNC_GREATER); P(nvnCommandBufferBindDepthStencilState)(&c2, &dss);
        NVNmultisampleState mss; P(nvnMultisampleStateSetDefaults)(&mss); P(nvnMultisampleStateSetSamples)(&mss, 4); P(nvnCommandBufferBindMultisampleState)(&c2, &mss);
        NVNcommandHandle hs3 = P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("polygon, depth-stencil and multisample binds: 8 + 16 + 24 (compact; the driver's 20-24, 8 and 80)", 48, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        {   const uint8_t *L3 = lin(hs3); op_hdr h1, h2, h3; memcpy(&h1, L3, 8); memcpy(&h2, L3 + 16, 8); memcpy(&h3, L3 + 40, 8);   // expanded: the canonical 16 + 24 + 24
            const p_bind_state *s1 = (const p_bind_state *)(L3 + 8), *s2 = (const p_bind_state *)(L3 + 24), *s3 = (const p_bind_state *)(L3 + 48);
            poly_st pr; ds_st dr; ms_st mr; memcpy(&pr, s1->bytes, sizeof pr); memcpy(&dr, s2->bytes, sizeof dr); memcpy(&mr, s3->bytes, sizeof mr);
            EXPECT("the state records read back: kinds, cull, depth function, samples", 1, h1.op == OP_BIND_STATE && h1.size == 16 && s1->kind == 4 && pr.cull == NVN_FACE_BACK
                   && h2.op == OP_BIND_STATE && h2.size == 24 && s2->kind == 5 && dr.df == NVN_DEPTH_FUNC_GREATER && h3.op == OP_BIND_STATE && h3.size == 24 && s3->kind == 3 && mr.samples == 4); }
        P(nvnCommandBufferFinalize)(&c2); P(nvnMemoryPoolFinalize)(&mp);
    }
    // Uniform bindings are measured into resource budgets too. A bounded binding needs
    // 8 header bytes plus 16 payload bytes; keep the full record for larger ranges.
    {
        static uint8_t m[4096] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c2;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c2, &dev);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferBindUniformBuffer)(&c2, NVN_SHADER_STAGE_FRAGMENT, 13, UINT64_C(0x123456789abc0000), 65536);
        NVNcommandHandle first = P(nvnCommandBufferEndRecording)(&c2);
        size_t first_size = P(nvnCommandBufferGetCommandMemoryUsed)(&c2);
        EXPECT("maximum uniform range: 24-byte bind", 24, first_size);
        op_hdr rh; memcpy(&rh, m, sizeof rh);
        EXPECT("bounded uniform record is 24 bytes", 24, rh.size);
        p_bind_buf decoded; const p_bind_buf *b = bind_buf_rec(rh.op, m + 8, &decoded);
        EXPECT("bounded uniform decode keeps stage, index, address, size", 1,
               b->stage == NVN_SHADER_STAGE_FRAGMENT && b->index == 13 && b->addr == UINT64_C(0x123456789abc0000) && b->size == 65536);
        EXPECT("uniform bind recording: one piece of 24 bytes", 1, one_piece(first, m, 24));
        uint8_t saved[64]; memcpy(saved, m, first_size);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, (ptrdiff_t)first_size, sizeof m - first_size); P(nvnCommandBufferBeginRecording)(&c2);
        NVNbufferRange ranges[] = {{UINT64_C(0xfedcba9876540000), 256}, {UINT64_C(0x1122334455660000), 0}};
        P(nvnCommandBufferBindUniformBuffers)(&c2, NVN_SHADER_STAGE_VERTEX, 2, 2, ranges);
        NVNcommandHandle plural = P(nvnCommandBufferEndRecording)(&c2);
        memset(ranges, 0, sizeof ranges);
        EXPECT("plural uniform binds: one compact record, 8 + 2 x 12", 32, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        EXPECT("independent uniform recording preserves predecessor", 0, memcmp(saved, m, first_size));
        const uint8_t *L = lin(plural); size_t pos = 0;   // expanded: one OP_BIND_UBO_SMALL a binding, as when recorded one by one
        memcpy(&rh, L + pos, sizeof rh); b = bind_buf_rec(rh.op, L + pos + 8, &decoded);
        EXPECT("plural first uniform keeps copied range and slot", 1,
               b->stage == NVN_SHADER_STAGE_VERTEX && b->index == 2 && b->addr == UINT64_C(0xfedcba9876540000) && b->size == 256);
        pos += rh.size;
        memcpy(&rh, L + pos, sizeof rh); b = bind_buf_rec(rh.op, L + pos + 8, &decoded);
        EXPECT("plural second uniform preserves zero range and next slot", 1,
               b->stage == NVN_SHADER_STAGE_VERTEX && b->index == 3 && b->addr == UINT64_C(0x1122334455660000) && b->size == 0);
        EXPECT("plural uniform binds: one piece", 1, one_piece(plural, m + first_size, 32));
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 512, sizeof m - 512); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferBindUniformBuffer)(&c2, NVN_SHADER_STAGE_COMPUTE, 3, UINT64_C(0xffff123456780000), UINT64_C(0x100000001));
        P(nvnCommandBufferEndRecording)(&c2);
        memcpy(&rh, m + 512, sizeof rh);
        EXPECT("wide uniform range keeps full record", 32, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        EXPECT("wide uniform range keeps full opcode and size", 1, rh.op == OP_BIND_UBO && rh.size == 32);
        b = bind_buf_rec(rh.op, m + 520, &decoded);
        EXPECT("wide uniform decode preserves all 64 bits", 1,
               b->stage == NVN_SHADER_STAGE_COMPUTE && b->index == 3 && b->addr == UINT64_C(0xffff123456780000) && b->size == UINT64_C(0x100000001));
        P(nvnCommandBufferFinalize)(&c2); P(nvnMemoryPoolFinalize)(&mp);
    }
    // One vertex range needs 8 header bytes plus 16 payload bytes; arrays and
    // ranges wider than 32 bits retain the general representation.
    {
        static uint8_t m[4096] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c2;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c2, &dev);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferBindVertexBuffer)(&c2, 15, UINT64_C(0xfedcba9876540000), UINT32_MAX);
        NVNcommandHandle hv = P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("single vertex buffer: 24-byte bind", 24, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        op_hdr rh; memcpy(&rh, m, sizeof rh);
        EXPECT("single vertex binding record is 24 bytes", 24, rh.size);
        EXPECT("single vertex binding recording: one piece", 1, one_piece(hv, m, 24));
        p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(rh.op, m + 8, &tmp);
        EXPECT("single vertex decode preserves slot, full address, maximum size", 1,
               b->first == 15 && b->n == 1 && b->r[0].address == UINT64_C(0xfedcba9876540000) && b->r[0].size == UINT32_MAX);
        NVNbufferRange ranges[] = {{UINT64_C(0x1122334455660000), 0}, {UINT64_C(0x9988776655440000), 256}};
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 128, sizeof m - 128); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferBindVertexBuffers)(&c2, 3, 1, ranges); P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("one-element vertex array: 24-byte bind", 24, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        memcpy(&rh, m + 128, sizeof rh); b = vbuf_rec(rh.op, m + 136, &tmp);
        EXPECT("one-element vertex array decode preserves zero range", 1,
               b->first == 3 && b->n == 1 && b->r[0].address == UINT64_C(0x1122334455660000) && b->r[0].size == 0);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 256, sizeof m - 256); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferBindVertexBuffers)(&c2, 4, 2, ranges); NVNcommandHandle hv2 = P(nvnCommandBufferEndRecording)(&c2);
        memset(ranges, 0, sizeof ranges);
        EXPECT("two vertex ranges: one compact record, 8 + 2 x 12 (the driver's 40)", 32, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        const uint8_t *L = lin(hv2); memcpy(&rh, L, sizeof rh);
        EXPECT("two vertex ranges expand to the array record", OP_BIND_VBUF, rh.op);
        b = vbuf_rec(rh.op, L + 8, &tmp);
        EXPECT("vertex array decode preserves both copied ranges", 1,
               b->first == 4 && b->n == 2 && b->r[0].address == UINT64_C(0x1122334455660000) && !b->r[0].size &&
               b->r[1].address == UINT64_C(0x9988776655440000) && b->r[1].size == 256);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 384, sizeof m - 384); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferBindVertexBuffer)(&c2, 7, UINT64_C(0xabcd123456780000), UINT64_C(0x100000001)); P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("wide vertex range retains full record", 32, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        memcpy(&rh, m + 384, sizeof rh);
        EXPECT("wide vertex range retains array opcode", OP_BIND_VBUF, rh.op);
        b = vbuf_rec(rh.op, m + 392, &tmp);
        EXPECT("wide vertex range decode preserves all 64 bits", 1,
               b->first == 7 && b->n == 1 && b->r[0].address == UINT64_C(0xabcd123456780000) && b->r[0].size == UINT64_C(0x100000001));
        P(nvnCommandBufferFinalize)(&c2); P(nvnMemoryPoolFinalize)(&mp);
    }
    // 5b'. Fixed slots sized by GetCommandMemoryUsed, as Tears of the Kingdom gives each model's prerecorded lists (GitHub issue #61): a list measured at S bytes is recorded
    //      into slot k = base + S × k with AddCommandMemory(max(S, 512)), and slot k + 1 is recorded right after it. The list here is an uniform update of n bytes and a
    //      uniform buffer bind (the last record before the END in the crash), the next slot's starts with a call. Slot 0 must still be its two records, in one piece inside
    //      its S bytes, after slot 1 is recorded: when the host linked pieces with JUMPs in command memory, S = 496 put a JUMP at 488 whose target slot 1 overwrote
    {
        static uint8_t m[8192] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c3;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c3, &dev);
        static uint8_t ub[1024]; NVNcommandBuffer co; P(nvnCommandBufferInitialize)(&co, &dev);
        P(nvnCommandBufferBeginRecording)(&co); NVNcommandHandle other = P(nvnCommandBufferEndRecording)(&co);   // an empty recording to call
        int broken = 0, lists = 0, first_bad = -1;
        for (int n = 0; n <= 1000; n += 8, lists++) {
            P(nvnCommandBufferAddCommandMemory)(&c3, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c3);   // measure, in ample memory
            P(nvnCommandBufferUpdateUniformBuffer)(&c3, 0x10000, 0x1000, 0, (size_t)n, ub); P(nvnCommandBufferBindUniformBuffer)(&c3, NVN_SHADER_STAGE_FRAGMENT, 4, 0x20000, 0x100);
            P(nvnCommandBufferEndRecording)(&c3);
            size_t S = P(nvnCommandBufferGetCommandMemoryUsed)(&c3), give = S > 512 ? S : 512;
            memset(m, 0, sizeof m);
            P(nvnCommandBufferAddCommandMemory)(&c3, &mp, 0, give); P(nvnCommandBufferBeginRecording)(&c3);
            P(nvnCommandBufferUpdateUniformBuffer)(&c3, 0x10000, 0x1000, 0, (size_t)n, ub); P(nvnCommandBufferBindUniformBuffer)(&c3, NVN_SHADER_STAGE_FRAGMENT, 4, 0x20000, 0x100);
            NVNcommandHandle h0 = P(nvnCommandBufferEndRecording)(&c3);
            P(nvnCommandBufferAddCommandMemory)(&c3, &mp, (ptrdiff_t)S, give); P(nvnCommandBufferBeginRecording)(&c3);   // the next slot, right behind
            P(nvnCommandBufferCallCommands)(&c3, 1, &other);
            int call_free = P(nvnCommandBufferGetCommandMemoryUsed)(&c3) == 0;   // a call takes no command memory
            P(nvnCommandBufferEndRecording)(&c3);
            int ok = call_free && one_piece(h0, m, S), k = 0, ops[3] = {0};
            size_t ln; uint8_t *L0 = nvn_rec_linear(h0, &ln);   // the records as submitted (compact ones expanded), less the END
            for (const uint8_t *p = L0; ok && p < L0 + ln - 8; k++) {
                op_hdr h; memcpy(&h, p, sizeof h); uint32_t op = h.op;
                if (op == OP_BIND_UBO || op == OP_BIND_UBO_SMALL) {
                    p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, p + 8, &tmp);
                    ok &= b->stage == NVN_SHADER_STAGE_FRAGMENT && b->index == 4 && b->addr == 0x20000 && b->size == 0x100;
                    op = OP_BIND_UBO;
                }
                if (k < 3) ops[k] = (int)op;
                if (!h.size) ok = 0; else p += h.size;
            }
            free(L0);
            ok &= k == 2 && ops[0] == OP_UPDATE_UB && ops[1] == OP_BIND_UBO;
            if (!ok) { broken++; if (first_bad < 0) first_bad = (int)S; }
        }
        printf("(first broken slot size %d)\n", first_bad);
        EXPECT("slotted lists stay whole in their slots after the next slot is recorded", 0, broken);
        EXPECT("lists checked", 126, lists);
        P(nvnCommandBufferFinalize)(&c3); P(nvnMemoryPoolFinalize)(&mp);
    }
    // 5b'''. Calls and copies (NVN): CallCommands copies the called list's pieces into the caller's list, as the driver copies a command set's GPU FIFO entries, so the called
    //        recording's control memory may be given out again once the call is recorded (Donkey Kong Bananza resets its control arena twice per command arena: when a call
    //        pointed at the called list, the caller replayed whatever the next recordings wrote there); CopyCommands copies the records, so its command memory may go too
    {
        static uint8_t cm[4096] __attribute__((aligned(4096))), ctl_a[256] __attribute__((aligned(8))), ctl_b[256] __attribute__((aligned(8)));
        static uint8_t d1[16], d2[16];
        NVNmemoryPoolBuilder mb; NVNmemoryPool mp; P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, cm, sizeof cm); P(nvnMemoryPoolInitialize)(&mp, &mb);
        NVNmemoryPoolBuilder db; NVNmemoryPool dp; static uint8_t dpool[4096] __attribute__((aligned(4096)));
        P(nvnMemoryPoolBuilderSetDefaults)(&db); P(nvnMemoryPoolBuilderSetStorage)(&db, dpool, sizeof dpool); P(nvnMemoryPoolInitialize)(&dp, &db);
        uint8_t *dst = dpool; enum { DSTN = 64 };
        NVNcommandBuffer ca, cc; P(nvnCommandBufferInitialize)(&ca, &dev); P(nvnCommandBufferInitialize)(&cc, &dev);
        memset(d1, 0x11, sizeof d1); memset(d2, 0x22, sizeof d2);
        #define CALLEE() do { P(nvnCommandBufferAddCommandMemory)(&ca, &mp, 0, 1024); P(nvnCommandBufferAddControlMemory)(&ca, ctl_a, sizeof ctl_a); \
            P(nvnCommandBufferBeginRecording)(&ca); P(nvnCommandBufferUpdateUniformBuffer)(&ca, (uintptr_t)dst, DSTN, 0, sizeof d1, d1); hc = P(nvnCommandBufferEndRecording)(&ca); } while (0)
        NVNcommandHandle hc; CALLEE(); size_t rec_sz = P(nvnCommandBufferGetCommandMemoryUsed)(&ca);
        P(nvnCommandBufferAddCommandMemory)(&cc, &mp, 1024, 1024); P(nvnCommandBufferAddControlMemory)(&cc, ctl_b, sizeof ctl_b);
        P(nvnCommandBufferBeginRecording)(&cc); P(nvnCommandBufferCallCommands)(&cc, 1, &hc);
        EXPECT("a call takes no command memory", 0, P(nvnCommandBufferGetCommandMemoryUsed)(&cc));
        P(nvnCommandBufferUpdateUniformBuffer)(&cc, (uintptr_t)dst, DSTN, 16, sizeof d2, d2);
        NVNcommandHandle hcall = P(nvnCommandBufferEndRecording)(&cc);
        uint64_t e0 = ctl_at(hcall, 0), e1 = ctl_at(hcall, 1);
        EXPECT("the caller's list: the called piece, then its own", 1, ctl_kind(e0) == CTL_CHUNK && !(e0 & CTL_LAST) && ctl_addr(e0) == cm && ctl_len(e0) == rec_sz
               && ctl_kind(e1) == CTL_CHUNK && (e1 & CTL_LAST) && ctl_addr(e1) == cm + 1024 && ctl_len(e1) == rec_sz);
        memset(ctl_a, 0, sizeof ctl_a);   // the called recording's control memory is given out again
        memset(dst, 0, DSTN); P(nvnQueueSubmitCommands)(&q, 1, &hcall); P(nvnQueueFinish)(&q);
        EXPECT("the call replays after the called list's control memory is reused", 1, !memcmp(dst, d1, 16) && !memcmp(dst + 16, d2, 16));
        CALLEE();
        P(nvnCommandBufferAddCommandMemory)(&cc, &mp, 2048, 1024); P(nvnCommandBufferAddControlMemory)(&cc, ctl_b, sizeof ctl_b);
        P(nvnCommandBufferBeginRecording)(&cc); P(nvnCommandBufferCopyCommands)(&cc, 1, &hc);
        EXPECT("a copy takes the copied records' command memory", rec_sz, P(nvnCommandBufferGetCommandMemoryUsed)(&cc));
        NVNcommandHandle hcopy = P(nvnCommandBufferEndRecording)(&cc);
        EXPECT("the copy is the caller's own piece", 1, one_piece(hcopy, cm + 2048, rec_sz));
        memset(cm, 0, 1024); memset(ctl_a, 0, sizeof ctl_a);   // both of the source's memories are given out again
        memset(dst, 0, DSTN); P(nvnQueueSubmitCommands)(&q, 1, &hcopy); P(nvnQueueFinish)(&q);
        EXPECT("the copy replays after the source's command and control memory are reused", 0, memcmp(dst, d1, 16));
        #undef CALLEE
        // A list that outgrows its control memory goes on in the block the game's callback adds, through a CTL_LINK, and replays whole
        NVNcommandBuffer cl; P(nvnCommandBufferInitialize)(&cl, &dev); P(nvnCommandBufferSetMemoryCallback)(&cl, ctl_cb); P(nvnCommandBufferSetMemoryCallbackData)(&cl, NULL);
        P(nvnCommandBufferAddCommandMemory)(&ca, &mp, 0, 1024); P(nvnCommandBufferAddControlMemory)(&ca, ctl_a, sizeof ctl_a);
        P(nvnCommandBufferBeginRecording)(&ca); P(nvnCommandBufferUpdateUniformBuffer)(&ca, (uintptr_t)dst, DSTN, 0, sizeof d1, d1); hc = P(nvnCommandBufferEndRecording)(&ca);
        g_ctloff = 0; g_ctl_calls = 0;
        P(nvnCommandBufferAddCommandMemory)(&cl, &mp, 1024, 2048); P(nvnCommandBufferAddControlMemory)(&cl, g_ctlmem + 3072, 16);
        P(nvnCommandBufferBeginRecording)(&cl);
        for (int k = 0; k < 8; k++) { P(nvnCommandBufferCallCommands)(&cl, 1, &hc); P(nvnCommandBufferUpdateUniformBuffer)(&cl, (uintptr_t)dst, DSTN, 16 + (k & 1) * 16, sizeof d2, d2); }
        NVNcommandHandle hl = P(nvnCommandBufferEndRecording)(&cl);
        int links = 0, pieces = 0; const uint8_t *e = (const uint8_t *)(uintptr_t)hl;
        for (int g = 0; g < 64; g++) { uint64_t v; memcpy(&v, e, 8); if (ctl_kind(v) == CTL_LINK) { links++; e = ctl_addr(v); continue; } pieces++; e += 8; if (v & CTL_LAST) break; }
        EXPECT("16 pieces, linked across the callback's blocks", 1, pieces == 16 && links >= 2 && g_ctl_calls >= 2);
        size_t ln; uint8_t *lin = nvn_rec_linear(hl, &ln); free(lin);
        EXPECT("the linked list holds all 16 records", 16 * rec_sz + 8, ln);
        EXPECT("control used: the entries in the last block", 1, P(nvnCommandBufferGetControlMemoryUsed)(&cl) > 0 && P(nvnCommandBufferGetControlMemoryUsed)(&cl) <= 32);
        memset(dst, 0, DSTN); P(nvnQueueSubmitCommands)(&q, 1, &hl); P(nvnQueueFinish)(&q);
        EXPECT("the linked list replays", 1, !memcmp(dst, d1, 16) && !memcmp(dst + 16, d2, 16) && !memcmp(dst + 32, d2, 16));
        P(nvnCommandBufferFinalize)(&ca); P(nvnCommandBufferFinalize)(&cc); P(nvnCommandBufferFinalize)(&cl); P(nvnMemoryPoolFinalize)(&mp); P(nvnMemoryPoolFinalize)(&dp);
    }
    // 5b''''. Compact records (nvn_obj.h OPC_*): each call recorded both ways expands to exactly the canonical records, in no more command memory
    {
        static uint8_t m[16384] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer ce;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&ce, &dev);
        NVNbufferRange r3[3] = {{0x20000, 0x100}, {0x21000, 0}, {0x22000, 0x300}}, rbig[2] = {{0x20000, 0x100}, {0x21000, UINT64_C(0x100000001)}};
        NVNtextureHandle th[3] = {0x1ff00001, 0x00100002, 3}; NVNimageHandle ih[3] = {7, 8, 9}; NVNseparateSamplerHandle sh[2] = {{4}, {5}};
        float vp[12] = {0, 0, 1280, 720, 10, 20, 30, 40, -1, -2, 3.5f, 4.5f};
        NVNvertexAttribState va[17]; for (int i = 0; i < 17; i++) { P(nvnVertexAttribStateSetDefaults)(&va[i]); P(nvnVertexAttribStateSetFormat)(&va[i], NVN_FORMAT_RGBA32F, 4 * i); }
        NVNvertexStreamState vs[3]; for (int i = 0; i < 3; i++) { P(nvnVertexStreamStateSetDefaults)(&vs[i]); P(nvnVertexStreamStateSetStride)(&vs[i], 16 + i); }
        int bad = 0, smaller = 0, cases = 0; size_t saved = 0;
        #define EQ(...) do { size_t u[2]; uint8_t *l[2]; size_t ln[2]; \
            for (int k = 0; k < 2; k++) { g_rec_compact = !k; P(nvnCommandBufferAddCommandMemory)(&ce, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&ce); __VA_ARGS__; \
                NVNcommandHandle hh = P(nvnCommandBufferEndRecording)(&ce); u[k] = P(nvnCommandBufferGetCommandMemoryUsed)(&ce); l[k] = nvn_rec_linear(hh, &ln[k]); } \
            g_rec_compact = 1; cases++; smaller += u[0] < u[1]; saved += u[1] - u[0]; \
            if (ln[0] != ln[1] || memcmp(l[0], l[1], ln[0]) || u[0] > u[1]) { bad++; printf("compact mismatch at line %d: %zu / %zu bytes, linear %zu / %zu\n", __LINE__, u[0], u[1], ln[0], ln[1]); } \
            free(l[0]); free(l[1]); } while (0)
        #define CB_ &ce
        EQ(P(nvnCommandBufferDrawArrays)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLES, 3, 36));
        EQ(P(nvnCommandBufferDrawArraysInstanced)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP, 0, 4, 3, 2));
        EQ(P(nvnCommandBufferDrawArraysInstanced)(CB_, NVN_DRAW_PRIMITIVE_POINTS, 1, 4, 0, 0));
        EQ(P(nvnCommandBufferDrawArraysIndirect)(CB_, NVN_DRAW_PRIMITIVE_LINES, 0x30000));
        EQ(h_nvnCommandBufferDrawMeshTasks(CB_, 2, 9));
        EQ(h_nvnCommandBufferDrawMeshTasksIndirect(CB_, 0x30040));
        EQ(P(nvnCommandBufferDrawElements)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, 99, UINT64_C(0x1234567890)));
        EQ(P(nvnCommandBufferDrawElementsBaseVertex)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_BYTE, 6, 0x10000, -7));
        EQ(P(nvnCommandBufferDrawElementsInstanced)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 6, 0x10000, 5, 1, 3));
        EQ(P(nvnCommandBufferDrawElementsIndirect)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 0x10000, 0x20000));
        EQ(P(nvnCommandBufferSetViewport)(CB_, 1, 2, 300, 400));
        EQ(P(nvnCommandBufferSetViewports)(CB_, 2, 3, vp));
        EQ(P(nvnCommandBufferSetStencilRef)(CB_, NVN_FACE_FRONT_AND_BACK, 0x80));
        EQ(P(nvnCommandBufferSetStencilMask)(CB_, NVN_FACE_BACK, 0xff));
        EQ(P(nvnCommandBufferSetStencilValueMask)(CB_, NVN_FACE_FRONT, 0));
        EQ(P(nvnCommandBufferSetStencilRef)(CB_, NVN_FACE_FRONT, 1 << 20));   // past the compact form: canonical both ways
        EQ(P(nvnCommandBufferBindImage)(CB_, NVN_SHADER_STAGE_COMPUTE, 3, 12));
        EQ(P(nvnCommandBufferBindImages)(CB_, NVN_SHADER_STAGE_FRAGMENT, 1, 3, ih));
        EQ(P(nvnCommandBufferBindTextures)(CB_, NVN_SHADER_STAGE_FRAGMENT, 2, 3, th));
        EQ(CB(&ce)->tex_base = 5; CB(&ce)->samp_base = 2; P(nvnCommandBufferBindTextures)(CB_, NVN_SHADER_STAGE_VERTEX, 0, 3, th); CB(&ce)->tex_base = CB(&ce)->samp_base = 0);
        EQ(P(nvnCommandBufferBindSeparateSamplers)(CB_, NVN_SHADER_STAGE_FRAGMENT, 0, 2, sh));
        EQ(P(nvnCommandBufferBindSeparateTexture)(CB_, NVN_SHADER_STAGE_FRAGMENT, 4, (NVNseparateTextureHandle){77}));
        EQ(h_nvnCommandBufferBindTexturesDeferred(CB_, NVN_SHADER_STAGE_FRAGMENT, 0, 2, th));
        EQ(P(nvnCommandBufferBindUniformBuffers)(CB_, NVN_SHADER_STAGE_VERTEX, 4, 3, r3));
        EQ(P(nvnCommandBufferBindUniformBuffers)(CB_, NVN_SHADER_STAGE_VERTEX, 14, 3, r3));   // past slot 16: one by one both ways
        EQ(P(nvnCommandBufferBindUniformBuffers)(CB_, NVN_SHADER_STAGE_FRAGMENT, 0, 2, rbig));
        EQ(P(nvnCommandBufferBindStorageBuffer)(CB_, NVN_SHADER_STAGE_COMPUTE, 5, 0x40000, 0x1000));
        EQ(P(nvnCommandBufferBindStorageBuffers)(CB_, NVN_SHADER_STAGE_COMPUTE, 1, 3, r3));
        EQ(P(nvnCommandBufferBindStorageBuffers)(CB_, NVN_SHADER_STAGE_COMPUTE, 0, 2, rbig));
        EQ(P(nvnCommandBufferBindVertexBuffers)(CB_, 1, 3, r3));
        EQ(P(nvnCommandBufferBindVertexBuffers)(CB_, 0, 2, rbig));
        EQ(P(nvnCommandBufferBindVertexAttribState)(CB_, 17, va));
        EQ(P(nvnCommandBufferBindVertexStreamState)(CB_, 3, vs));
        EQ(P(nvnCommandBufferBindVertexBuffer)(CB_, 0, 0x20000, 0x1000); P(nvnCommandBufferBindUniformBuffer)(CB_, NVN_SHADER_STAGE_VERTEX, 3, 0x30000, 0x100);
           P(nvnCommandBufferBindTexture)(CB_, NVN_SHADER_STAGE_FRAGMENT, 0, 0x1ff00002); P(nvnCommandBufferDrawElementsBaseVertex)(CB_, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 300, 0x10000, 7));
        NVNbufferRange ua[3] = {{0x30000, 0x100}, {0x30100, 0x10000}, {0x7fff00, 0}}, un[2] = {{0x30010, 0x100}, {0x30000, 0x40000}};
        EQ(P(nvnCommandBufferBindUniformBuffer)(CB_, NVN_SHADER_STAGE_FRAGMENT, 13, UINT64_C(0xfedcba987600), 0x3ffff));
        EQ(P(nvnCommandBufferBindUniformBuffer)(CB_, NVN_SHADER_STAGE_FRAGMENT, 2, 0x30010, 0x100));   // not 256-aligned: the 24-byte form
        EQ(P(nvnCommandBufferBindUniformBuffers)(CB_, NVN_SHADER_STAGE_GEOMETRY, 1, 3, ua));
        EQ(P(nvnCommandBufferBindUniformBuffers)(CB_, NVN_SHADER_STAGE_VERTEX, 0, 2, un));
        NVNcolorState cs; P(nvnColorStateSetDefaults)(&cs); P(nvnColorStateSetBlendEnable)(&cs, 3, 1); P(nvnColorStateSetLogicOp)(&cs, NVN_LOGIC_OP_XOR);
        NVNdepthStencilState dss; P(nvnDepthStencilStateSetDefaults)(&dss); P(nvnDepthStencilStateSetDepthFunc)(&dss, NVN_DEPTH_FUNC_GEQUAL); P(nvnDepthStencilStateSetStencilTestEnable)(&dss, 1);
        NVNblendState bls; P(nvnBlendStateSetDefaults)(&bls); P(nvnBlendStateSetBlendTarget)(&bls, 2);
        NVNchannelMaskState cms; P(nvnChannelMaskStateSetDefaults)(&cms); P(nvnChannelMaskStateSetChannelMask)(&cms, 7, 1, 0, 1, 0);
        NVNpolygonState pls; P(nvnPolygonStateSetDefaults)(&pls); P(nvnPolygonStateSetCullFace)(&pls, NVN_FACE_BACK);
        NVNmultisampleState mss; P(nvnMultisampleStateSetDefaults)(&mss); P(nvnMultisampleStateSetSamples)(&mss, 4);
        EQ(P(nvnCommandBufferBindColorState)(CB_, &cs)); EQ(P(nvnCommandBufferBindDepthStencilState)(CB_, &dss)); EQ(P(nvnCommandBufferBindBlendState)(CB_, &bls));
        EQ(P(nvnCommandBufferBindChannelMaskState)(CB_, &cms)); EQ(P(nvnCommandBufferBindPolygonState)(CB_, &pls)); EQ(P(nvnCommandBufferBindMultisampleState)(CB_, &mss));
        EQ(P(nvnCommandBufferBarrier)(CB_, 0x1f)); EQ(P(nvnCommandBufferBarrier)(CB_, 1 << 30));
        EQ(P(nvnCommandBufferSetPrimitiveRestart)(CB_, 1, -1)); EQ(P(nvnCommandBufferSetPrimitiveRestart)(CB_, 1, 0xffff)); EQ(P(nvnCommandBufferSetPrimitiveRestart)(CB_, 0, 0x7fffffff));
        EQ(P(nvnCommandBufferSetScissor)(CB_, 0, 0, 1920, 1080)); EQ(P(nvnCommandBufferSetScissor)(CB_, -4, 0, 70000, 8));
        #undef EQ
        #undef CB_
        printf("(compact records: %d cases, %d smaller, %zu bytes saved)\n", cases, smaller, saved);
        EXPECT("compact records expand to the canonical ones, in no more bytes", 0, bad);
        EXPECT("compact records are smaller in most cases", 1, smaller >= 35);
        P(nvnCommandBufferFinalize)(&ce); P(nvnMemoryPoolFinalize)(&mp);
    }
    // 5b''. Host segment sizes (hostseg_size), as Super Mario Odyssey grants 512 bytes from an empty pool before recording: such a grant gets a HOSTSEG_SMALL segment; a recording
    //       that outgrows it with no callback goes on in a full one, as a second piece of its control list, and replays whole through its handle; a record larger than HOSTSEG_SZ gets a segment that holds it;
    //       a free small segment is not taken for a full one, and the cap counts every segment's bytes
    {
        NVNcommandBuffer c4, c5; P(nvnCommandBufferInitialize)(&c4, &dev); P(nvnCommandBufferInitialize)(&c5, &dev);
        EXPECT("empty pool, 512 bytes, outside a recording: a small segment", HOSTSEG_SMALL, hostseg_size(CB(&c4), 1, 512));
        EXPECT("empty pool, 0 bytes: a full segment", HOSTSEG_SZ, hostseg_size(CB(&c4), 1, 0));
        EXPECT("empty pool, more than HOSTSEG_SMALL: a full segment", HOSTSEG_SZ, hostseg_size(CB(&c4), 1, HOSTSEG_SMALL + 1));
        EXPECT("a record larger than HOSTSEG_SZ: a segment that holds it", 1, hostseg_size(CB(&c4), 0, HOSTSEG_SZ + 64) >= HOSTSEG_SZ + 64);
        P(nvnCommandBufferAddCommandMemory)(&c4, NULL, 0, 512);
        uint8_t *small = CB(&c4)->cur;
        EXPECT("the grant lends HOSTSEG_SMALL bytes", HOSTSEG_SMALL, CB(&c4)->end - small);
        P(nvnCommandBufferBeginRecording)(&c4);
        EXPECT("empty pool, 512 bytes, inside a recording: a full segment", HOSTSEG_SZ, hostseg_size(CB(&c4), 1, 512));
        static uint8_t dst[8192] __attribute__((aligned(4096))), d1[32], d2[HOSTSEG_SMALL + 256];
        memset(dst, 0, sizeof dst); memset(d1, 0x35, sizeof d1); memset(d2, 0x6A, sizeof d2);
        NVNmemoryPoolBuilder db; NVNmemoryPool dp; P(nvnMemoryPoolBuilderSetDefaults)(&db); P(nvnMemoryPoolBuilderSetStorage)(&db, dst, sizeof dst); P(nvnMemoryPoolInitialize)(&dp, &db);
        P(nvnCommandBufferUpdateUniformBuffer)(&c4, (uintptr_t)dst, sizeof dst, 0, sizeof d1, d1);
        P(nvnCommandBufferUpdateUniformBuffer)(&c4, (uintptr_t)dst, sizeof dst, 64, sizeof d2, d2);
        NVNcommandHandle h4 = P(nvnCommandBufferEndRecording)(&c4);
        op_hdr h0; memcpy(&h0, small, sizeof h0); uint64_t e0 = ctl_at(h4, 0), e1 = ctl_at(h4, 1);
        EXPECT("the list's first piece is the first record, in the small segment", 1, ctl_kind(e0) == CTL_CHUNK && !(e0 & CTL_LAST) && ctl_addr(e0) == small && ctl_len(e0) == h0.size);
        EXPECT("the record that does not fit is the last piece, elsewhere", 1, ctl_kind(e1) == CTL_CHUNK && (e1 & CTL_LAST) && (ctl_addr(e1) < small || ctl_addr(e1) >= small + HOSTSEG_SMALL));
        P(nvnQueueSubmitCommands)(&q, 1, &h4); P(nvnQueueFinish)(&q);
        EXPECT("the first record replays", 0, memcmp(dst, d1, sizeof d1));
        EXPECT("the record in the second piece replays", 0, memcmp(dst + 64, d2, sizeof d2));
        P(nvnCommandBufferBeginRecording)(&c4);
        uint8_t *big = cmd_alloc(CB(&c4), HOSTSEG_SZ + 64);
        EXPECT("a record larger than HOSTSEG_SZ fits its segment", 1, CB(&c4)->end - big >= HOSTSEG_SZ + 64);
        P(nvnCommandBufferEndRecording)(&c4);
        pthread_mutex_lock(&g_hostseg_mu); int si = -1; for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].mem == small) si = i; if (si >= 0) g_hostseg[si].busy = 0; pthread_mutex_unlock(&g_hostseg_mu);
        P(nvnCommandBufferAddCommandMemory)(&c5, NULL, 0, 0);
        EXPECT("a free small segment is not taken for a full one", 1, si >= 0 && CB(&c5)->cur != small && CB(&c5)->end - CB(&c5)->cur == HOSTSEG_SZ);
        P(nvnCommandBufferAddCommandMemory)(&c5, NULL, 0, 512);
        EXPECT("but is taken again for a small one", 1, CB(&c5)->cur == small);
        uint64_t sum = 0; for (int i = 0; i < g_nhostseg; i++) sum += g_hostseg[i].n;
        EXPECT("the cap counts every segment's bytes", sum, g_hostseg_bytes);
        P(nvnCommandBufferFinalize)(&c4); P(nvnCommandBufferFinalize)(&c5); P(nvnMemoryPoolFinalize)(&dp);
    }

    // 5b. Sync must hold the CPU back: first queue 24 copies of 64 MiB to keep the GPU busy, with the last one copying src to dst;
    //     the CPU rewrites src only after the fence wait returns. If the wait is released early, that last copy reads the rewritten src.
    {
        size_t big = 64u << 20, pgsz = 16384;
        uint8_t *heap; posix_memalign((void **)&heap, pgsz, 4 * big);
        uint8_t *busy_a = heap, *busy_b = heap + big, *bsrc = heap + 2 * big, *bdst = heap + 3 * big;
        memset(heap, 0, 4 * big); memset(bsrc, 0x5A, big);
        NVNmemoryPoolBuilder hb; NVNmemoryPool hp; P(nvnMemoryPoolBuilderSetDefaults)(&hb); P(nvnMemoryPoolBuilderSetStorage)(&hb, heap, 4 * big);
        P(nvnMemoryPoolInitialize)(&hp, &hb);
        P(nvnCommandBufferBeginRecording)(&cb);
        for (int i = 0; i < 24; i++) P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)busy_a, (NVNbufferAddress)busy_b, big, 0);
        P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)bsrc, (NVNbufferAddress)bdst, big, 0);
        NVNcommandHandle hb2 = P(nvnCommandBufferEndRecording)(&cb);
        NVNsync sb; P(nvnSyncInitialize)(&sb, &dev);
        uint64_t t0 = now_ns();
        P(nvnQueueSubmitCommands)(&q, 1, &hb2); P(nvnQueueFenceSync)(&q, &sb, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
        int w0 = P(nvnSyncWait)(&sb, 0);
        int w1 = P(nvnSyncWait)(&sb, 10000000000ull);
        double ms = (now_ns() - t0) / 1e6;
        memset(bsrc, 0xC3, big);                                       // rewrite only after the wait is released
        size_t bad = 0; for (size_t i = 0; i < big; i += 4096) bad += bdst[i] != 0x5A;
        printf("(GPU busy for about %.1f ms)\n", ms);
        EXPECT("zero-timeout wait right after submit = timeout (2)", 2, w0);
        EXPECT("long wait = condition satisfied (1)", 1, w1);
        EXPECT("dst holds only the pre-rewrite value (16384 samples)", 0, bad);
        P(nvnMemoryPoolFinalize)(&hp); free(heap);
    }

    // 6. Sync / window / present
    NVNsync s1, s2; P(nvnSyncInitialize)(&s1, &dev); P(nvnSyncInitialize)(&s2, &dev);
    EXPECT("wait on an unfenced sync = timeout (2)", 2, P(nvnSyncWait)(&s1, 0));
    P(nvnQueueFenceSync)(&q, &s1, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    EXPECT("wait after fence = already signaled (0)", 0, P(nvnSyncWait)(&s1, 0));
    NVNtexture sc[2]; NVNtextureBuilder tb; P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetWidth)(&tb, 1280); P(nvnTextureBuilderSetHeight)(&tb, 720); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetStorage)(&tb, &pool, 0); P(nvnTextureInitialize)(&sc[0], &tb); P(nvnTextureInitialize)(&sc[1], &tb);
    EXPECT("texture width", 1280, P(nvnTextureGetWidth)(&sc[0]));
    NVNtexture *scp[2] = {&sc[0], &sc[1]};
    NVNwindowBuilder wb; NVNwindow win; P(nvnWindowBuilderSetDevice)(&wb, &dev); P(nvnWindowBuilderSetDefaults)(&wb);
    NVNtexture *missing_first[2] = {NULL, &sc[1]};
    P(nvnWindowBuilderSetTextures)(&wb, 2, missing_first);
    EXPECT("WindowInitialize with null first texture", 1, P(nvnWindowInitialize)(&win, &wb));
    P(nvnWindowBuilderSetTextures)(&wb, 2, scp); EXPECT("WindowInitialize succeeds", 1, P(nvnWindowInitialize)(&win, &wb));
    int idx[4]; unsigned bi = 0; while (strcmp(nvn_names[bi], "nvnCommandBufferBarrier")) bi++; uint64_t barriers0 = nvn_slot[bi].cnt;   // earlier tests bind barriers too
    for (int f = 0; f < 4; f++) {
        P(nvnWindowAcquireTexture)(&win, &s2, &idx[f]);
        for (int b = 0; b < 5; b++) P(nvnCommandBufferBarrier)(&cb, 0);
        P(nvnQueuePresentTexture)(&q, &win, idx[f]);
    }
    EXPECT("swapchain rotates 0,1,0,1", 0x0101, idx[0] << 12 | idx[1] << 8 | idx[2] << 4 | idx[3]);
    EXPECT("sync signaled after acquire (0)", 0, P(nvnSyncWait)(&s2, 0));
    EXPECT("Barrier count = 4 frames × 5", 20, nvn_slot[bi].cnt - barriers0);

    // 6b. Readback: pixel (x,y) = {x*16+y, y*16+x, 0x55, 0xAA} (expected values computed directly from the formula)
    {
        #define PX(x, y, c) ((uint8_t)((c) == 0 ? (x) * 16 + (y) : (c) == 1 ? (y) * 16 + (x) : (c) == 2 ? 0x55 : 0xAA))
        static uint8_t rb[256 << 10] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder rpb; NVNmemoryPool rp; P(nvnMemoryPoolBuilderSetDefaults)(&rpb); P(nvnMemoryPoolBuilderSetStorage)(&rpb, rb, sizeof rb);
        P(nvnMemoryPoolInitialize)(&rp, &rpb);
        uint8_t *stage = rb + 0x8000, *outb = rb + 0xC000;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) for (int c = 0; c < 4; c++) stage[(y * 8 + x) * 4 + c] = PX(x, y, c);
        NVNtextureBuilder b; NVNtexture ta; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b);
        P(nvnTextureBuilderSetWidth)(&b, 8); P(nvnTextureBuilderSetHeight)(&b, 4); P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8);
        P(nvnTextureBuilderSetStorage)(&b, &rp, 0); P(nvnTextureInitialize)(&ta, &b);
        NVNcopyRegion all = {0, 0, 0, 8, 4, 1}, sub = {2, 1, 0, 5, 3, 1}, cp = {1, 0, 0, 4, 2, 1};
        P(nvnCommandBufferBeginRecording)(&cb);
        P(nvnCommandBufferSetCopyRowStride)(&cb, 0);
        P(nvnCommandBufferCopyBufferToTexture)(&cb, (NVNbufferAddress)stage, &ta, NULL, &all, 0);
        P(nvnCommandBufferSetCopyRowStride)(&cb, 64);
        P(nvnCommandBufferCopyTextureToBuffer)(&cb, &ta, NULL, &cp, (NVNbufferAddress)outb, 0);
        NVNcommandHandle hr = P(nvnCommandBufferEndRecording)(&cb);
        memset(outb, 0xEE, 256);
        P(nvnQueueSubmitCommands)(&q, 1, &hr); P(nvnQueueFinish)(&q);
        uint8_t got[5 * 3 * 4]; P(nvnTextureReadTexels)(&ta, NULL, &sub, got);
        int bad = 0; for (int y = 0; y < 3; y++) for (int x = 0; x < 5; x++) for (int c = 0; c < 4; c++) bad += got[(y * 5 + x) * 4 + c] != PX(x + 2, y + 1, c);
        EXPECT("ReadTexels 5x3 sub-region byte for byte (GPU-written texture)", 0, bad);
        uint8_t gs[4 * 40]; memset(gs, 0xEE, sizeof gs); P(nvnTextureReadTexelsStrided)(&ta, NULL, &all, gs, 40, 0);
        bad = 0; for (int y = 0; y < 4; y++) { for (int x = 0; x < 8; x++) for (int c = 0; c < 4; c++) bad += gs[y * 40 + x * 4 + c] != PX(x, y, c); for (int k = 32; k < 40; k++) bad += gs[y * 40 + k] != 0xEE; }
        EXPECT("ReadTexelsStrided stride 40 (row padding untouched)", 0, bad);
        bad = 0; for (int y = 0; y < 2; y++) { for (int x = 0; x < 4; x++) for (int c = 0; c < 4; c++) bad += outb[y * 64 + x * 4 + c] != PX(x + 1, y, c); for (int k = 16; k < 64; k++) bad += outb[y * 64 + k] != 0xEE; }
        EXPECT("CopyTextureToBuffer stride 64 (GPU copy-back)", 0, bad);
        // Packaged texture: the pool memory is the ground truth (block-linear); ReadTexels deswizzles and reads it directly
        uint8_t lin[16 * 8 * 4]; for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 4; c++) lin[(y * 16 + x) * 4 + c] = PX(x, y, c);
        NVNtexture tb; NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}};
        P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetWidth)(&b, 16); P(nvnTextureBuilderSetHeight)(&b, 8); P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8);
        P(nvnTextureBuilderSetPackagedTextureLayout)(&b, &lay); P(nvnTextureBuilderSetStorage)(&b, &rp, 0x10000); P(nvnTextureInitialize)(&tb, &b);
        nvn_texdesc dd = ((tex_t *)&tb)->d;
        if (nvn_swizzle(&dd, 0, 0, lin, 64, rb + 0x10000)) { printf("nvn_swizzle failed\n"); g_fail++; }
        NVNcopyRegion s2 = {3, 2, 0, 9, 5, 1}; uint8_t g2[9 * 5 * 4];
        P(nvnTextureReadTexels)(&tb, NULL, &s2, g2);
        bad = 0; for (int y = 0; y < 5; y++) for (int x = 0; x < 9; x++) for (int c = 0; c < 4; c++) bad += g2[(y * 9 + x) * 4 + c] != PX(x + 3, y + 2, c);
        EXPECT("ReadTexels packaged texture 9x5 (block-linear pool memory read directly)", 0, bad);
        P(nvnMemoryPoolFinalize)(&rp);
    }

    // 6b2. MINIMAL_LAYOUT storage: two 320x320 two-byte textures back to back, as before the mesh data in Tears of the Kingdom's Depths (GitHub issue #183). Each takes 204,800 bytes;
    // WriteTexels of the first must leave the second, and the bytes after both, alone
    {
        enum { bytes = 320 * 320 * 2, guard = 65536 };
        static uint8_t ms[2 * bytes + guard] __attribute__((aligned(16384)));
        static uint8_t px[bytes], back[bytes];
        memset(ms, 0xa7, sizeof ms);
        for (int i = 0; i < bytes; i++) px[i] = (uint8_t)(i * 19 + (i >> 8));
        NVNmemoryPoolBuilder mpb; NVNmemoryPool mp; P(nvnMemoryPoolBuilderSetDefaults)(&mpb); P(nvnMemoryPoolBuilderSetStorage)(&mpb, ms, sizeof ms);
        P(nvnMemoryPoolInitialize)(&mp, &mpb);
        NVNtexture mt[2]; NVNtextureBuilder b;
        for (int k = 0; k < 2; k++) {
            P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b);
            P(nvnTextureBuilderSetSize2D)(&b, 320, 320); P(nvnTextureBuilderSetFlags)(&b, NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT);
            P(nvnTextureBuilderSetFormat)(&b, k ? NVN_FORMAT_RGB565 : NVN_FORMAT_RGB5A1);
            P(nvnTextureBuilderSetStorage)(&b, &mp, k * bytes); P(nvnTextureInitialize)(&mt[k], &b);
            EXPECT("MINIMAL_LAYOUT 320x320 two-byte texture takes 204,800 bytes", bytes, P(nvnTextureGetStorageSize)(&mt[k]));
            NVNcopyRegion all = {0, 0, 0, 320, 320, 1};
            P(nvnTextureWriteTexels)(&mt[k], NULL, &all, px);
            int bad = 0; for (int i = (k + 1) * bytes; i < (int)sizeof ms; i++) bad += ms[i] != 0xa7;
            EXPECT("MINIMAL_LAYOUT WriteTexels leaves the storage after the texture alone", 0, bad);
            P(nvnTextureReadTexels)(&mt[k], NULL, &all, back);
            EXPECT("MINIMAL_LAYOUT texels read back as written", 0, memcmp(px, back, bytes));
        }
        P(nvnTextureFinalize)(&mt[0]); P(nvnTextureFinalize)(&mt[1]); P(nvnMemoryPoolFinalize)(&mp);
    }

    // Packaged H/D/W nibbles describe physical pitch independently of API SPARSE.
    {
        NVNtextureBuilder b = {0}; NVNpackagedTextureLayout layout={{3,4,0,0,0,0,0,0}};
        P(nvnTextureBuilderSetDevice)(&b,&dev);
        P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetSize2D)(&b,2052,1026);
        P(nvnTextureBuilderSetFormat)(&b,NVN_FORMAT_RGBA_ASTC_6x6); P(nvnTextureBuilderSetFlags)(&b,NVN_TEXTURE_FLAGS_IMAGE);
        P(nvnTextureBuilderSetPackagedTextureLayout)(&b,&layout);
        EXPECT("packaged width nibble reaches description",0x404,((tex_builder_t*)&b)->d.pkg_layout);
        EXPECT("packaged full tiles use padded pitch",0x120000,P(nvnTextureBuilderGetStorageSize)(&b));
        EXPECT("packaged pitch leaves public flags unchanged",NVN_TEXTURE_FLAGS_IMAGE,P(nvnTextureBuilderGetFlags)(&b));
        NVNtextureSparseTileLayout sparse; P(nvnTextureBuilderGetSparseTileLayout)(&b,&sparse);
        EXPECT("packaged pitch does not advertise sparse tiles",0,sparse.numTiledLevels);
        layout.layout[1]=0; P(nvnTextureBuilderSetPackagedTextureLayout)(&b,&layout);
        EXPECT("ordinary packaged pitch remains unpadded",0x102000,P(nvnTextureBuilderGetStorageSize)(&b));
        P(nvnTextureBuilderSetPackagedTextureLayout)(&b,NULL);
        EXPECT("clearing packaged layout clears width too",0,((tex_builder_t*)&b)->d.pkg_layout);
    }

    // 6c. Write-protected dirty pages: after a packaged texture is uploaded its pool pages are read-only; game write → page fault → mark dirty → re-upload on next use; a direct host read() write needs prepare first
    {
        static uint8_t wb_[256 << 10] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder wpb; NVNmemoryPool wp; P(nvnMemoryPoolBuilderSetDefaults)(&wpb); P(nvnMemoryPoolBuilderSetStorage)(&wpb, wb_, sizeof wb_);
        P(nvnMemoryPoolInitialize)(&wp, &wpb);
        NVNtextureBuilder b; NVNtexture tw; NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}};
        P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetWidth)(&b, 16); P(nvnTextureBuilderSetHeight)(&b, 8);
        P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetPackagedTextureLayout)(&b, &lay);
        P(nvnTextureBuilderSetStorage)(&b, &wp, 0x10000); P(nvnTextureInitialize)(&tw, &b);
        nvn_texdesc dd = ((tex_t *)&tw)->d;
        uint8_t lin[16 * 8 * 4], *store = wb_ + 0x10000, *outb = wb_ + 0x20000;
        #define FILLPAT(k) for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 4; c++) lin[(y * 16 + x) * 4 + c] = (uint8_t)(x * 16 + y + (k) * 3 + c * 41)
        FILLPAT(0); nvn_swizzle(&dd, 0, 0, lin, 64, store);
        NVNcopyRegion all = {0, 0, 0, 16, 8, 1};
        // Check the actual GPU-side contents with a GPU copy-back (CopyTextureToBuffer reading the MTLTexture): expected = pattern version k
        #define GPU_READ_EQ(k, name) do { P(nvnCommandBufferBeginRecording)(&cb); P(nvnCommandBufferSetCopyRowStride)(&cb, 0); \
            P(nvnCommandBufferCopyTextureToBuffer)(&cb, &tw, NULL, &all, (NVNbufferAddress)outb, 0); NVNcommandHandle hh = P(nvnCommandBufferEndRecording)(&cb); \
            P(nvnQueueSubmitCommands)(&q, 1, &hh); P(nvnQueueFinish)(&q); FILLPAT(k); EXPECT(name, 0, memcmp(outb, lin, sizeof lin)); } while (0)
        uint64_t up0, re0, fa0, pr0, up, re, fa, pr; mtl_pkg_stats(&up0, &re0); nvn_dirty_stats(&fa0, &pr0);
        GPU_READ_EQ(0, "first upload: GPU-side contents = version 0");
        mtl_pkg_stats(&up, &re); EXPECT("first uploads +1", 1, up - up0);
        FILLPAT(1); nvn_swizzle(&dd, 0, 0, lin, 64, store);          // game-style CPU write: protected page → page fault → hook marks it dirty
        nvn_dirty_stats(&fa, &pr); EXPECT("game write triggers ≥ 1 page fault (caught by the hook, no crash)", 1, fa - fa0 >= 1);
        GPU_READ_EQ(1, "after re-upload: GPU-side contents = version 1");
        mtl_pkg_stats(&up, &re); EXPECT("pool contents changed: re-uploads +1", 1, re - re0);
        uint64_t fa1 = fa; GPU_READ_EQ(1, "reuse with unchanged contents: still version 1");
        nvn_dirty_stats(&fa, &pr); mtl_pkg_stats(&up, &re); EXPECT("unchanged contents: page faults +0", 0, fa - fa1); EXPECT("unchanged contents: re-uploads still 1", 1, re - re0);
        // Pitfall 1: host read() writing directly into protected pages. Without prepare the kernel returns EFAULT (no page fault); after prepare the read succeeds, the pages are marked dirty and re-uploaded next time
        char path[1024]; snprintf(path, sizeof path, "%s/t_nvn_rd_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "."); int fd = mkstemp(path);   // per-user temp directory, do not write to /tmp
        FILLPAT(2); uint8_t tmpst[sizeof lin * 4]; memset(tmpst, 0, sizeof tmpst);
        static uint8_t full[0x10000]; memcpy(full, store, sizeof full);   // existing storage contents (block-linear); write version 2 into a copy and then save it to a file
        nvn_swizzle(&dd, 0, 0, lin, 64, full); if (write(fd, full, ((tex_t *)&tw)->L.size) < 0) g_fail++;
        ssize_t r0 = pread(fd, store, ((tex_t *)&tw)->L.size, 0); int e0 = errno;
        EXPECT("without prepare: read() into protected pages returns -1 with EFAULT", 1, r0 == -1 && e0 == EFAULT);
        dm_guest_write_prepare(store, ((tex_t *)&tw)->L.size);
        ssize_t r1 = pread(fd, store, ((tex_t *)&tw)->L.size, 0);
        EXPECT("read() succeeds after prepare", (long long)((tex_t *)&tw)->L.size, r1);
        close(fd); unlink(path);
        GPU_READ_EQ(2, "re-upload after host write: GPU-side contents = version 2");
        mtl_pkg_stats(&up, &re); EXPECT("host write triggers a re-upload, 2 in total", 2, re - re0);
        P(nvnMemoryPoolFinalize)(&wp);
    }
    // 6c1. Writes beside a texture (pkg_edges_clean): the texture starts 4 KB into a host page whose first 4 KB hold other data (Pokémon Sword: the pool's object). A write there dirties the
    //      page, but the texture's bytes are as uploaded: protected again, no re-upload; a write into the texture's own bytes in that page still re-uploads
    {
        static uint8_t eb_[256 << 10] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder epb; NVNmemoryPool ep; P(nvnMemoryPoolBuilderSetDefaults)(&epb); P(nvnMemoryPoolBuilderSetStorage)(&epb, eb_, sizeof eb_);
        P(nvnMemoryPoolInitialize)(&ep, &epb);
        NVNtextureBuilder b; NVNtexture te; NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}};
        P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetWidth)(&b, 16); P(nvnTextureBuilderSetHeight)(&b, 8);
        P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetPackagedTextureLayout)(&b, &lay);
        P(nvnTextureBuilderSetStorage)(&b, &ep, 0x11000); P(nvnTextureInitialize)(&te, &b);
        nvn_texdesc dd = ((tex_t *)&te)->d;
        uint8_t lin[16 * 8 * 4], *store = eb_ + 0x11000, *outb = eb_ + 0x30000;
        FILLPAT(5); nvn_swizzle(&dd, 0, 0, lin, 64, store);
        NVNcopyRegion all = {0, 0, 0, 16, 8, 1};
        #define GPU_READ_EQ_E(k, name) do { P(nvnCommandBufferBeginRecording)(&cb); P(nvnCommandBufferSetCopyRowStride)(&cb, 0); \
            P(nvnCommandBufferCopyTextureToBuffer)(&cb, &te, NULL, &all, (NVNbufferAddress)outb, 0); NVNcommandHandle hh = P(nvnCommandBufferEndRecording)(&cb); \
            P(nvnQueueSubmitCommands)(&q, 1, &hh); P(nvnQueueFinish)(&q); FILLPAT(k); EXPECT(name, 0, memcmp(outb, lin, sizeof lin)); } while (0)
        uint64_t up0, re0, es0, up, re; mtl_pkg_stats(&up0, &re0); es0 = mtl_pkg_edge_skips();
        GPU_READ_EQ_E(5, "texture sharing its host page: first upload = version 5");
        for (int i = 0; i < 3; i++) { eb_[0x10800 + i] ^= 0x5a; GPU_READ_EQ_E(5, "a write beside the texture: still version 5"); }
        mtl_pkg_stats(&up, &re);
        EXPECT("writes beside the texture: no re-upload", 0, re - re0);
        EXPECT("writes beside the texture: found clean three times", 3, mtl_pkg_edge_skips() - es0);
        FILLPAT(6); nvn_swizzle(&dd, 0, 0, lin, 64, store);
        GPU_READ_EQ_E(6, "a write into the texture's own bytes in that page: version 6");
        mtl_pkg_stats(&up, &re); EXPECT("a write into the texture: re-upload +1", 1, re - re0);
        P(nvnMemoryPoolFinalize)(&ep);
    }

    // 6c2. WriteTexels region writes: the result must be byte-identical to "decode the whole level → patch the region → re-encode" (Metroid Dread's minimap writes one texel at a time, which used to cost a whole-level round trip each).
    //      RGBA8 100×37 (width not a whole number of GOBs), BC1 64×64 (4×4 blocks, 8 bytes per block), RGBA16F 33×17 two-layer array; several regions each, including 1×1, a whole row, GOB-crossing and the whole level
    {
        static uint8_t wtmem[1 << 18] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder wb2; NVNmemoryPool wtp; P(nvnMemoryPoolBuilderSetDevice)(&wb2, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&wb2);
        P(nvnMemoryPoolBuilderSetStorage)(&wb2, wtmem, sizeof wtmem); P(nvnMemoryPoolBuilderSetFlags)(&wb2, 0x24); P(nvnMemoryPoolInitialize)(&wtp, &wb2);
        struct { int fmt, w, h, layers; int rg[5][4]; } cs[] = {
            {NVN_FORMAT_RGBA8, 100, 37, 1, {{0, 0, 1, 1}, {99, 36, 1, 1}, {13, 5, 40, 9}, {0, 20, 100, 1}, {0, 0, 100, 37}}},
            {NVN_FORMAT_RGBA_DXT1, 64, 64, 1, {{0, 0, 4, 4}, {60, 60, 4, 4}, {16, 8, 32, 20}, {0, 32, 64, 4}, {0, 0, 64, 64}}},
            {NVN_FORMAT_RGBA16F, 33, 17, 2, {{32, 16, 1, 1}, {1, 2, 30, 13}, {0, 0, 33, 1}, {5, 9, 17, 8}, {0, 0, 33, 17}}},
        };
        int bad = 0, n = 0; uint32_t seed = 12345;
        for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
            NVNtextureBuilder b; NVNtexture t; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b);
            P(nvnTextureBuilderSetTarget)(&b, cs[c].layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D);
            P(nvnTextureBuilderSetSize3D)(&b, cs[c].w, cs[c].h, cs[c].layers); P(nvnTextureBuilderSetFormat)(&b, cs[c].fmt);
            P(nvnTextureBuilderSetStorage)(&b, &wtp, 0); P(nvnTextureInitialize)(&t, &b);
            const tex_t *tt = (const tex_t *)&t; int bw, bh, bpb = nvn_fmt_bpb(cs[c].fmt, &bw, &bh);
            size_t lp = (size_t)((cs[c].w + bw - 1) / bw) * bpb, lr = (size_t)(cs[c].h + bh - 1) / bh;
            for (int k = 0; k < 5; k++) {
                for (size_t i = 0; i < tt->L.size; i++) wtmem[i] = (uint8_t)(seed = seed * 1103515245u + 12345u) >> 3;   // random old content
                static uint8_t want[1 << 18], lin[1 << 16], src[1 << 16];
                int x = cs[c].rg[k][0], y = cs[c].rg[k][1], w = cs[c].rg[k][2], h = cs[c].rg[k][3], z = cs[c].layers - 1;
                size_t wbb = (size_t)((w + bw - 1) / bw) * bpb, hbb = (size_t)(h + bh - 1) / bh;
                for (size_t i = 0; i < wbb * hbb; i++) src[i] = (uint8_t)(seed = seed * 1103515245u + 12345u) >> 5;
                memcpy(want, wtmem, tt->L.size);   // expected: decode the whole level, patch, re-encode (only the last layer)
                nvn_deswizzle(&tt->d, 0, (unsigned)z, want, lin, lp);
                for (size_t r = 0; r < hbb; r++) memcpy(lin + ((size_t)y / bh + r) * lp + (size_t)(x / bw) * bpb, src + r * wbb, wbb);
                nvn_swizzle(&tt->d, 0, (unsigned)z, lin, lp, want); (void)lr;
                NVNtextureView vw; P(nvnTextureViewSetDefaults)(&vw); P(nvnTextureViewSetLayers)(&vw, z, 1);
                NVNcopyRegion rg = {x, y, 0, w, h, 1};
                write_texels(tt, cs[c].layers > 1 ? (const view_t *)&vw : NULL, &rg, src, 0, 0);
                n++; if (memcmp(want, wtmem, tt->L.size)) bad++;
            }
            P(nvnTextureFinalize)(&t);
        }
        EXPECT("WriteTexels region write byte-identical to a whole-level round trip (15 cases)", 0, bad); EXPECT("WriteTexels region case count", 15, n);
        P(nvnMemoryPoolFinalize)(&wtp);
    }

    // 6c2b. WriteTexels into linear textures (LINEAR, LINEAR_RENDER_TARGET): rows of the texture's stride, the row padding and the bytes after the texture untouched, and ReadTexels gives
    //       back what was written. The block-linear writes refuse a linear layout and the whole-level path dropped the write (a native-texture movie into LINEAR | VIDEO_DECODE drew black,
    //       nxnative PR #1983). A region outside the rows is refused and writes nothing. A texture a view on its pool's buffer takes stays that view: no CPU-written texture is added
    {
        static uint8_t lmem[1 << 16] __attribute__((aligned(16384)));
        static uint8_t want[1 << 16], src[1 << 14], back[1 << 14];
        NVNmemoryPoolBuilder lb; NVNmemoryPool lpool; P(nvnMemoryPoolBuilderSetDevice)(&lb, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&lb);
        P(nvnMemoryPoolBuilderSetStorage)(&lb, lmem, sizeof lmem); P(nvnMemoryPoolBuilderSetFlags)(&lb, 0x24); P(nvnMemoryPoolInitialize)(&lpool, &lb);
        struct { int flags, w, h, stride; int rg[4][4]; int strided; } cs[] = {
            {NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_VIDEO_DECODE, 37, 5, 256, {{0, 0, 37, 5}, {3, 1, 10, 2}, {36, 4, 1, 1}, {0, 2, 37, 1}}, 0},
            {NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, 16, 16, 128, {{0, 0, 16, 16}, {5, 7, 9, 6}, {0, 15, 16, 1}, {15, 0, 1, 16}}, 1},
        };
        int bad = 0, n = 0, unwritten = 0, read_bad = 0, made_cpu = 0; uint32_t seed = 777;
        for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
            NVNtextureBuilder b; NVNtexture t; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b);
            P(nvnTextureBuilderSetTarget)(&b, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetFlags)(&b, cs[c].flags);
            P(nvnTextureBuilderSetSize2D)(&b, cs[c].w, cs[c].h); P(nvnTextureBuilderSetStride)(&b, cs[c].stride);
            P(nvnTextureBuilderSetStorage)(&b, &lpool, 0); P(nvnTextureInitialize)(&t, &b);
            const tex_t *tt = (const tex_t *)&t; size_t span = tt->L.size + 512;   // the texture and the bytes after it
            unsigned cpu0 = mtl_cpu_tex_count();
            EXPECT("linear texture stride from the builder", cs[c].stride, (long long)tex_linear_row(tt));
            for (size_t i = 0; i < span; i++) lmem[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 3);   // random old content
            memcpy(want, lmem, span);
            for (int k = 0; k < 4; k++) {
                int x = cs[c].rg[k][0], y = cs[c].rg[k][1], w = cs[c].rg[k][2], h = cs[c].rg[k][3];
                size_t sr = cs[c].strided ? (size_t)w * 4 + 12 : (size_t)w * 4;   // the strided case's source rows carry 12 bytes of padding
                for (size_t i = 0; i < sr * (size_t)h; i++) src[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 5);
                for (int r = 0; r < h; r++) memcpy(want + (size_t)(y + r) * (size_t)cs[c].stride + (size_t)x * 4, src + (size_t)r * sr, (size_t)w * 4);
                NVNcopyRegion rg = {x, y, 0, w, h, 1};
                if (cs[c].strided) P(nvnTextureWriteTexelsStrided)(&t, NULL, &rg, src, (ptrdiff_t)sr, 0); else P(nvnTextureWriteTexels)(&t, NULL, &rg, src);
                n++; bad += memcmp(want, lmem, span) != 0;
            }
            NVNcopyRegion out = {0, cs[c].h - 1, 0, cs[c].w, 2, 1};   // a row past the last: refused, nothing written
            memset(src, 0x5A, (size_t)cs[c].w * 8); P(nvnTextureWriteTexels)(&t, NULL, &out, src);
            unwritten += memcmp(want, lmem, span) != 0;
            NVNcopyRegion all = {0, 0, 0, cs[c].w, cs[c].h, 1};
            P(nvnTextureReadTexels)(&t, NULL, &all, back);
            for (int r = 0; r < cs[c].h; r++) read_bad += memcmp(back + (size_t)r * (size_t)cs[c].w * 4, want + (size_t)r * (size_t)cs[c].stride, (size_t)cs[c].w * 4) != 0;
            made_cpu += (int)(mtl_cpu_tex_count() - cpu0);
            P(nvnTextureFinalize)(&t);
        }
        EXPECT("WriteTexels into linear textures: rows of the stride, padding and the bytes after untouched (8 cases)", 0, bad); EXPECT("linear WriteTexels case count", 8, n);
        EXPECT("WriteTexels into a linear texture past its rows writes nothing", 0, unwritten);
        EXPECT("ReadTexels of a linear texture gives back what WriteTexels wrote (rows)", 0, read_bad);
        EXPECT("linear textures viewed on their pool's buffer are not made CPU-written by WriteTexels", 0, made_cpu);
        P(nvnMemoryPoolFinalize)(&lpool);
    }

    // 6c3. GPU copies stored into the destination's storage on the GPU (nvn_mtl.m gsb_store / gsb_store_buf, the streamer's level copies), run through the queue: a region of a level
    //      copied from a texture of other dimensions (another block height) and a whole level, leaving the pool byte-identical to "decode the source level → patch the destination level →
    //      re-encode", the rest of it untouched; RGBA8 and BC1; the same regions from a buffer in the pool with a padded row stride. The pool is filled
    //      from the generator's high bits: its low byte repeats every 256 bytes, and copies between level-aligned regions then moved bytes equal to those they replaced
    #define STORE_RUN(rec) do { P(nvnCommandBufferBeginRecording)(&cb); P(nvnCommandBufferSetCopyRowStride)(&cb, 0); P(nvnCommandBufferSetCopyImageStride)(&cb, 0); rec; \
        NVNcommandHandle hh = P(nvnCommandBufferEndRecording)(&cb); P(nvnQueueSubmitCommands)(&q, 1, &hh); P(nvnQueueFinish)(&q); } while (0)
    {
        static uint8_t mmem[1 << 18] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder mb; NVNmemoryPool mp; P(nvnMemoryPoolBuilderSetDevice)(&mb, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&mb);
        P(nvnMemoryPoolBuilderSetStorage)(&mb, mmem, sizeof mmem); P(nvnMemoryPoolBuilderSetFlags)(&mb, 0x24); P(nvnMemoryPoolInitialize)(&mp, &mb);
        struct { int fmt, sw, dw; int rg[2][6]; } cs[] = {   // source sw×sw with 3 levels, destination dw×dw with 4 levels at pool offset 0x20000; regions (sx, sy, dx, dy, w, h): level 0 → 1, then level 1 → 2
            {NVN_FORMAT_RGBA8, 32, 64, {{8, 4, 4, 8, 16, 12}, {0, 0, 0, 0, 16, 16}}},
            {NVN_FORMAT_RGBA_DXT1, 64, 128, {{8, 4, 4, 8, 16, 12}, {0, 0, 0, 0, 32, 32}}},
        };
        int bad = 0, n = 0; uint32_t seed = 777;
        for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
            NVNtextureBuilder b; NVNtexture s, d; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetTarget)(&b, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetFormat)(&b, cs[c].fmt);
            P(nvnTextureBuilderSetSize3D)(&b, cs[c].sw, cs[c].sw, 1); P(nvnTextureBuilderSetLevels)(&b, 3); P(nvnTextureBuilderSetStorage)(&b, &mp, 0); P(nvnTextureInitialize)(&s, &b);
            P(nvnTextureBuilderSetSize3D)(&b, cs[c].dw, cs[c].dw, 1); P(nvnTextureBuilderSetLevels)(&b, 4); P(nvnTextureBuilderSetStorage)(&b, &mp, 0x20000); P(nvnTextureInitialize)(&d, &b);
            const tex_t *ts = (const tex_t *)&s, *td = (const tex_t *)&d; int bw, bh, bpb = nvn_fmt_bpb(cs[c].fmt, &bw, &bh);
            for (int k = 0; k < 2; k++) {
                static uint8_t want[1 << 18], slin[1 << 16], dlin[1 << 16], src[1 << 16];
                unsigned sl = (unsigned)k, dl = (unsigned)k + 1; int sx = cs[c].rg[k][0], sy = cs[c].rg[k][1], dx = cs[c].rg[k][2], dy = cs[c].rg[k][3], w = cs[c].rg[k][4], h = cs[c].rg[k][5];
                size_t slp = (size_t)(((cs[c].sw >> sl) + bw - 1) / bw) * bpb, dlp = (size_t)(((cs[c].dw >> dl) + bw - 1) / bw) * bpb, wbb = (size_t)((w + bw - 1) / bw) * bpb, hbb = (size_t)(h + bh - 1) / bh;
                NVNcopyRegion sr = {sx, sy, 0, w, h, 1}, dr = {dx, dy, 0, w, h, 1};
                for (size_t i = 0; i < sizeof mmem; i++) mmem[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
                memcpy(want, mmem, sizeof mmem);
                nvn_deswizzle(&ts->d, sl, 0, ts->mem, slin, slp); nvn_deswizzle(&td->d, dl, 0, td->mem, dlin, dlp);
                for (size_t r = 0; r < hbb; r++) memcpy(dlin + ((size_t)dy / bh + r) * dlp + (size_t)(dx / bw) * bpb, slin + ((size_t)sy / bh + r) * slp + (size_t)(sx / bw) * bpb, wbb);
                nvn_swizzle(&td->d, dl, 0, dlin, dlp, want + 0x20000);
                NVNtextureView sv, dv; P(nvnTextureViewSetDefaults)(&sv); P(nvnTextureViewSetLevels)(&sv, (int)sl, 1); P(nvnTextureViewSetDefaults)(&dv); P(nvnTextureViewSetLevels)(&dv, (int)dl, 1);
                STORE_RUN(P(nvnCommandBufferCopyTextureToTexture)(&cb, &s, &sv, &sr, &d, &dv, &dr, 0));
                n++; if (memcmp(want, mmem, sizeof mmem)) { bad++; printf("2D case %u.%d texture copy differs\n", c, k); }
                for (size_t i = 0; i < sizeof mmem; i++) mmem[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
                size_t row = wbb + 16; uint8_t *bsrc = mmem + 0x30000; for (size_t i = 0; i < row * hbb; i++) bsrc[i] = src[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 20);
                memcpy(want, mmem, sizeof mmem);
                nvn_deswizzle(&td->d, dl, 0, td->mem, dlin, dlp);
                for (size_t r = 0; r < hbb; r++) memcpy(dlin + ((size_t)dy / bh + r) * dlp + (size_t)(dx / bw) * bpb, src + r * row, wbb);
                nvn_swizzle(&td->d, dl, 0, dlin, dlp, want + 0x20000);
                STORE_RUN(P(nvnCommandBufferSetCopyRowStride)(&cb, (ptrdiff_t)row); P(nvnCommandBufferSetCopyImageStride)(&cb, (ptrdiff_t)(row * hbb));
                          P(nvnCommandBufferCopyBufferToTexture)(&cb, (NVNbufferAddress)bsrc, &d, &dv, &dr, 0));
                n++; if (memcmp(want, mmem, sizeof mmem)) { bad++; printf("2D case %u.%d buffer copy differs\n", c, k); }
            }
            P(nvnTextureFinalize)(&s); P(nvnTextureFinalize)(&d);
        }
        EXPECT("copies stored into the destination's storage on the GPU, byte-identical to a level round trip (8 cases)", 0, bad); EXPECT("copy store case count", 8, n);
        P(nvnMemoryPoolFinalize)(&mp);
    }

    // 6c4. The same into 3D textures, slice by slice (GitHub #1856: LEGO Star Wars fills BC6H light-probe volumes a slice at a time): a buffer copy into a BC6H MINIMAL_LAYOUT volume (padded row
    //      and image strides) and into level 1 of an RGBA8 volume with a deeper block (its planes interleave in the block), and a 3D → 3D texture copy, each stored on the GPU; and a 3D
    //      WriteTexels region (written in place on the CPU); each byte-identical to "decode the 3D level → patch its slices → re-encode", with the rest of the pool untouched
    {
        static uint8_t vmem[1 << 20] __attribute__((aligned(16384))), want[1 << 20], dlin[1 << 18], slin[1 << 18], src[1 << 16];
        NVNmemoryPoolBuilder mb; NVNmemoryPool mp; P(nvnMemoryPoolBuilderSetDevice)(&mb, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&mb);
        P(nvnMemoryPoolBuilderSetStorage)(&mb, vmem, sizeof vmem); P(nvnMemoryPoolBuilderSetFlags)(&mb, 0x24); P(nvnMemoryPoolInitialize)(&mp, &mb);
        NVNtextureBuilder b; NVNtexture tv[3]; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetTarget)(&b, NVN_TEXTURE_TARGET_3D);
        P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_BPTC_UFLOAT); P(nvnTextureBuilderSetFlags)(&b, NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT); P(nvnTextureBuilderSetSize3D)(&b, 64, 64, 8);
        P(nvnTextureBuilderSetStorage)(&b, &mp, 0); P(nvnTextureInitialize)(&tv[0], &b);
        P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetFlags)(&b, 0); P(nvnTextureBuilderSetSize3D)(&b, 32, 32, 32); P(nvnTextureBuilderSetLevels)(&b, 2);
        P(nvnTextureBuilderSetStorage)(&b, &mp, 0x40000); P(nvnTextureInitialize)(&tv[1], &b);
        P(nvnTextureBuilderSetSize3D)(&b, 16, 16, 16); P(nvnTextureBuilderSetLevels)(&b, 1); P(nvnTextureBuilderSetStorage)(&b, &mp, 0xc0000); P(nvnTextureInitialize)(&tv[2], &b);
        const tex_t *bc = (const tex_t *)&tv[0], *vol = (const tex_t *)&tv[1], *vsrc = (const tex_t *)&tv[2];
        EXPECT("the RGBA8 volume has a block deeper than one GOB", 1, vol->L.tileD > 0);
        // kind: 0 buffer copy, 1 texture copy from vsrc (level 0, region at sx, sy, sz), 2 WriteTexels; padded strides for the buffer copies
        struct { const tex_t *t; int kind; unsigned level; NVNcopyRegion r; int sx, sy, sz; } cs[] = {
            {bc, 0, 0, {8, 4, 2, 32, 16, 3}, 0, 0, 0}, {bc, 0, 0, {0, 0, 7, 64, 64, 1}, 0, 0, 0},
            {vol, 0, 1, {4, 4, 5, 8, 8, 6}, 0, 0, 0}, {vol, 1, 0, {3, 7, 9, 12, 8, 5}, 2, 1, 4}, {vol, 2, 0, {5, 2, 17, 9, 20, 7}, 0, 0, 0},
        };
        int bad = 0, n = 0; uint32_t seed = 4242;
        for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
            const tex_t *t = cs[c].t; const NVNcopyRegion *r = &cs[c].r; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
            unsigned lv = cs[c].level; uint32_t lw = t->d.width >> lv, lh = t->d.height >> lv; uint64_t toff = (uint64_t)(t->mem - vmem);
            size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh, wbb = (size_t)((r->width + bw - 1) / bw) * bpb, hbb = (size_t)(r->height + bh - 1) / bh;
            size_t row = cs[c].kind == 0 ? wbb + 16 : wbb, img = cs[c].kind == 0 ? row * hbb + 32 : row * hbb;
            for (size_t i = 0; i < sizeof vmem; i++) vmem[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
            if (cs[c].kind == 1) {   // the source's bytes are the source volume's own, region at (sx, sy, sz)
                size_t slp = (size_t)vsrc->d.width * 4, slr = vsrc->d.height;
                nvn_deswizzle(&vsrc->d, 0, 0, vsrc->mem, slin, slp);
                for (int z = 0; z < r->depth; z++) for (size_t y = 0; y < hbb; y++) memcpy(src + z * img + y * row, slin + ((size_t)(cs[c].sz + z) * slr + (size_t)cs[c].sy + y) * slp + (size_t)cs[c].sx * 4, wbb);
            } else for (size_t i = 0; i < img * (size_t)r->depth; i++) src[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 20);
            uint8_t *bsrc = vmem + 0xe0000; if (cs[c].kind == 0) memcpy(bsrc, src, img * (size_t)r->depth);   // the copy's source, in the pool
            memcpy(want, vmem, sizeof vmem);
            nvn_deswizzle(&t->d, lv, 0, t->mem, dlin, lp);
            for (int z = 0; z < r->depth; z++) for (size_t y = 0; y < hbb; y++)
                memcpy(dlin + ((size_t)(r->zoffset + z) * lr + (size_t)r->yoffset / bh + y) * lp + (size_t)(r->xoffset / bw) * bpb, src + z * img + y * row, wbb);
            nvn_swizzle(&t->d, lv, 0, dlin, lp, want + toff);
            NVNcopyRegion sr = {cs[c].sx, cs[c].sy, cs[c].sz, r->width, r->height, r->depth};
            NVNtextureView v; P(nvnTextureViewSetDefaults)(&v); P(nvnTextureViewSetLevels)(&v, (int)lv, 1);
            if (cs[c].kind == 0) STORE_RUN(P(nvnCommandBufferSetCopyRowStride)(&cb, (ptrdiff_t)row); P(nvnCommandBufferSetCopyImageStride)(&cb, (ptrdiff_t)img);
                                           P(nvnCommandBufferCopyBufferToTexture)(&cb, (NVNbufferAddress)bsrc, (const NVNtexture *)t, &v, r, 0));
            else if (cs[c].kind == 1) STORE_RUN(P(nvnCommandBufferCopyTextureToTexture)(&cb, &tv[2], NULL, &sr, (const NVNtexture *)t, &v, r, 0));
            else write_texels(t, NULL, r, src, row, img);
            n++; if (memcmp(want, vmem, sizeof vmem)) { bad++; printf("3D case %u differs\n", c); }
        }
        EXPECT("copies into 3D textures stored slice by slice, byte-identical to a level round trip (5 cases)", 0, bad); EXPECT("3D copy store case count", 5, n);
        #undef STORE_RUN
        for (int i = 0; i < 3; i++) P(nvnTextureFinalize)(&tv[i]);
        P(nvnMemoryPoolFinalize)(&mp);
    }

    // 6d. Lifecycle: in-flight GPU work is 0 when suspend returns; queued replay waits for resume.
    {
        size_t big = 64u << 20; uint8_t *heap; posix_memalign((void **)&heap, 16384, 2 * big); memset(heap, 0, 2 * big);
        NVNmemoryPoolBuilder hb; NVNmemoryPool hp; P(nvnMemoryPoolBuilderSetDefaults)(&hb); P(nvnMemoryPoolBuilderSetStorage)(&hb, heap, 2 * big);
        P(nvnMemoryPoolInitialize)(&hp, &hb);
        P(nvnCommandBufferBeginRecording)(&cb);
        for (int i = 0; i < 24; i++) P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)heap, (NVNbufferAddress)(heap + big), big, 0);
        NVNcommandHandle hbusy = P(nvnCommandBufferEndRecording)(&cb);
        P(nvnQueueSubmitCommands)(&q, 1, &hbusy);
        NVNsync busy_sync; P(nvnSyncInitialize)(&busy_sync, &dev);
        P(nvnQueueFenceSync)(&q, &busy_sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
        // Submit may return before the worker encodes anything. Observe this
        // queue's receipt before checking its physical GPU completion.
        uint64_t f = 0; int encoded = 0; struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 5;
        pthread_mutex_lock(&nq_mu);
        while (!(encoded = nq_sync_encoded((const sync_t *)&busy_sync, &f)))
            if (pthread_cond_timedwait(&nq_cv, &nq_mu, &deadline)) break;
        pthread_mutex_unlock(&nq_mu);
        EXPECT("busy submission receives its own physical fence", 1, encoded);
        int busy_before = !mtl_wait(f, 0);
        dm_gfx_suspend(1);
        EXPECT("GPU really busy before suspend (zero-timeout wait not complete)", 1, busy_before);
        EXPECT("in-flight work complete when suspend returns", 1, mtl_wait(f, 0));
        P(nvnCommandBufferBeginRecording)(&cb); P(nvnCommandBufferBarrier)(&cb, 0); NVNcommandHandle hsmall = P(nvnCommandBufferEndRecording)(&cb);
        uint64_t suspended_fence = mtl_last_fence();
        g_sq = &q; g_sh = hsmall; g_sub = P(nvnQueueSubmitCommands); atomic_store(&g_sstarted, 0); atomic_store(&g_sdone, 0);
        pthread_t th; pthread_create(&th, NULL, susp_thr, NULL);
        int started = wait_started();
        EXPECT("during suspend another thread enters submit", 1, started);
        EXPECT("background submission has not reached the GPU", suspended_fence, mtl_last_fence());
        dm_gfx_suspend(0);
        pthread_join(th, NULL);
        EXPECT("that submit is let through and returns after resume", 1, atomic_load(&g_sdone));
        P(nvnQueueFinish)(&q); P(nvnSyncFinalize)(&busy_sync); P(nvnMemoryPoolFinalize)(&hp); free(heap);
    }

    // 6e. Present pacing (real-device semantics): present returns immediately; blocking happens at acquire - swapchain of 3, interval 1, 60 Hz, the 4th acquire waits until texture 0 has been displayed for a full period
    {
        nvn_present_config(60, 0);
        NVNwindowBuilder wb2; NVNwindow w2; P(nvnWindowBuilderSetDevice)(&wb2, &dev); P(nvnWindowBuilderSetDefaults)(&wb2);
        NVNtexture *sc3[3] = {&sc[0], &sc[1], &sc[0]};
        P(nvnWindowBuilderSetTextures)(&wb2, 3, sc3); P(nvnWindowInitialize)(&w2, &wb2); P(nvnWindowSetPresentInterval)(&w2, 1);
        int ix;
        for (int k = 0; k < 3; k++) {
            P(nvnWindowAcquireTexture)(&w2, NULL, &ix);
            P(nvnQueuePresentTexture)(&q, &w2, ix);
        }
        P(nvnWindowAcquireTexture)(&w2, NULL, &ix);
        EXPECT("4th acquire returns texture 0", 0, ix);
        P(nvnQueuePresentTexture)(&q, &w2, ix);
        P(nvnWindowAcquireTexture)(&w2, NULL, &ix);
        EXPECT("5th acquire returns texture 1", 1, ix);
        nvn_present_config(0, 0);
    }

    // 6f. Present thread: when the drawable stays unavailable for a long time (the present thread stalls 200 ms each time), the present call still returns immediately;
    //     after the swapchain has gone a full round, re-acquiring texture 0 has to wait until its present has been processed (first enqueue + 200 ms)
    {
        nvn_present_config(0, 0);   // do not wait on the display link; only check that the present thread hands it back
        const double S = 200;
        mtl_present_test_stall((uint64_t)(S * 1e6));
        NVNwindowBuilder wb3; NVNwindow w3; P(nvnWindowBuilderSetDevice)(&wb3, &dev); P(nvnWindowBuilderSetDefaults)(&wb3);
        NVNtexture *sc3[3] = {&sc[0], &sc[1], &sc[0]};
        P(nvnWindowBuilderSetTextures)(&wb3, 3, sc3); P(nvnWindowInitialize)(&w3, &wb3);
        int ix;
        for (int k = 0; k < 3; k++) {
            P(nvnWindowAcquireTexture)(&w3, NULL, &ix);
            P(nvnQueuePresentTexture)(&q, &w3, ix);
        }
        P(nvnWindowAcquireTexture)(&w3, NULL, &ix);
        EXPECT("acquire returns texture 0", 0, ix);
        mtl_present_test_stall(0); mtl_present_flush();
    }

    // 6g. SetNumActiveTextures: acquire hands out the first n of the swap chain (Super Mario Bros. Wonder sets 2 of 3), the window's own call changes it, a count out of range keeps all
    {
        nvn_present_config(0, 0);
        NVNwindowBuilder wb4; NVNwindow w4; P(nvnWindowBuilderSetDevice)(&wb4, &dev); P(nvnWindowBuilderSetDefaults)(&wb4);
        NVNtexture *sc3[3] = {&sc[0], &sc[1], &sc[0]};
        typedef void (*set_b)(NVNwindowBuilder *, int); typedef int (*get_b)(const NVNwindowBuilder *);
        typedef void (*set_w)(NVNwindow *, int); typedef int (*get_w)(const NVNwindow *);
        P(nvnWindowBuilderSetTextures)(&wb4, 3, sc3);
        EXPECT("builder: active defaults to the swap chain", 3, ((get_b)proc("nvnWindowBuilderGetNumActiveTextures"))(&wb4));
        ((set_b)proc("nvnWindowBuilderSetNumActiveTextures"))(&wb4, 2);
        EXPECT("builder: 2 active", 2, ((get_b)proc("nvnWindowBuilderGetNumActiveTextures"))(&wb4));
        P(nvnWindowInitialize)(&w4, &wb4);
        EXPECT("window: 2 active", 2, ((get_w)proc("nvnWindowGetNumActiveTextures"))(&w4));
        int ix, seq = 0;
        for (int k = 0; k < 4; k++) { P(nvnWindowAcquireTexture)(&w4, NULL, &ix); seq = seq * 10 + ix; P(nvnQueuePresentTexture)(&q, &w4, ix); }
        EXPECT("2 of 3 active: textures 0 1 0 1", 101, seq);
        ((set_w)proc("nvnWindowSetNumActiveTextures"))(&w4, 3);
        int seen = 0;
        for (int k = 0; k < 3; k++) { P(nvnWindowAcquireTexture)(&w4, NULL, &ix); seen |= 1 << ix; P(nvnQueuePresentTexture)(&q, &w4, ix); }
        EXPECT("3 active again: every texture handed out", 7, seen);
        ((set_w)proc("nvnWindowSetNumActiveTextures"))(&w4, 4);
        EXPECT("4 of 3 (out of range): all stay active", 3, ((get_w)proc("nvnWindowGetNumActiveTextures"))(&w4));
        mtl_present_flush();
    }

    // 7. Unimplemented functions: return 0, report a failure, report only once
    // Use a function that is still unimplemented (nvnBufferBuilderSetDefaults, used before, was implemented later and crashes on NULL)
    unsigned ui = 0; while (strcmp(nvn_names[ui], "nvnBufferBuilderGetDevice")) ui++;
    ((void (*)(void *))proc("nvnBufferBuilderGetDevice"))(NULL); ((void (*)(void *))proc("nvnBufferBuilderGetDevice"))(NULL);
    EXPECT("unimplemented function marked as reported ([RED]) after a call", 1, g_unimpl_seen[ui] > 0);
    EXPECT("unimplemented function count 2", 2, nvn_slot[ui].cnt);
    for (int i = 0; i < 254; i++) ((void (*)(void *))proc("nvnBufferBuilderGetDevice"))(NULL);   // the 256th call is where a one-byte count wrapped to 0, and the next call reported again
    EXPECT("unimplemented function count 256", 256, nvn_slot[ui].cnt);
    EXPECT("unimplemented function stays reported past 256 calls", 1, g_unimpl_seen[ui]);

    // 8. A texture buffer as 2D rows for wrapped 1D fetches (x & 0x3FFF, x >> 14; nvn_mtl.m tbuf_as_2d): full rows back to back, one short row rounded to the alignment
    {
        uint32_t w, h, row;
        EXPECT("256 R32 elements: one row", 1, nvn_tbuf_rows(256, 4, 64, &w, &h, &row) && w == 256 && h == 1 && row == 1024);
        EXPECT("10 R32 elements: one row rounded up to the 64-byte alignment", 1, nvn_tbuf_rows(10, 4, 64, &w, &h, &row) && w == 10 && h == 1 && row == 64);
        EXPECT("16384 RGBA32F elements: exactly one full row", 1, nvn_tbuf_rows(16384, 16, 256, &w, &h, &row) && w == 16384 && h == 1 && row == 262144);
        EXPECT("16385 R32 elements: two rows of 16384", 1, nvn_tbuf_rows(16385, 4, 64, &w, &h, &row) && w == 16384 && h == 2 && row == 65536);
        EXPECT("1,048,576 R32 elements: 64 rows", 1, nvn_tbuf_rows(1048576, 4, 64, &w, &h, &row) && w == 16384 && h == 64 && row == 65536);
        EXPECT("no elements: no rows", 0, nvn_tbuf_rows(0, 4, 64, &w, &h, &row));
        EXPECT("several rows whose alignment would break the wrapped address: refused", 0, nvn_tbuf_rows(20000, 4, 3, &w, &h, &row));
    }

    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
