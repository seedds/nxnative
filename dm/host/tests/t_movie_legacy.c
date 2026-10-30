// SDK 3 movie construction: call the registered hook, configure through SDK
// stand-ins, then copy synthetic decoded frames. No game files or device needed.
#include "../hle_movie.c"
#include <assert.h>
#include <limits.h>
#include <time.h>

__thread _Atomic int dm_host_work;
typedef struct { uint8_t *base; size_t cap; int off, size; } test_buffer;
typedef struct { int width, height, channels, rate; } test_format;
static uint64_t test_vtable[16];

static void test_set_data(void *b, void *p, size_t cap) { test_buffer *v = b; *v = (test_buffer){p, cap, 0, 0}; }
static void test_set_range(void *b, int off, int size) { test_buffer *v = b; assert(off >= 0 && size >= 0 && (size_t)off + size <= v->cap); v->off = off; v->size = size; }
static void *test_data(const void *b) { const test_buffer *v = b; return v->base + v->off; }
static size_t test_cap(const void *b) { return ((const test_buffer *)b)->cap; }
static size_t test_size(const void *b) { return ((const test_buffer *)b)->size; }
static int test_find_i32(const void *m, const char *key, int *value) {
    const test_format *f = m;
    if (!strcmp(key, "width")) *value = f->width;
    else if (!strcmp(key, "height")) *value = f->height;
    else if (!strcmp(key, "channel-count")) *value = f->channels;
    else if (!strcmp(key, "sample-rate")) *value = f->rate;
    else return 0;
    return 1;
}
static int test_find_buffer(const void *m, const char *key, void **buffer) { (void)m; (void)key; *buffer = NULL; return 0; }
static void test_set_i32(void *m, const char *key, int value) { (void)m; (void)key; (void)value; }
static void test_set_rect(void *m, const char *key, int l, int t, int r, int b) { (void)m; (void)key; (void)l; (void)t; (void)r; (void)b; }
static void test_signal(void *event) { (void)event; abort(); }

void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZTVN5movie7DecoderE")) return test_vtable;
    if (!strcmp(name, "_ZN5movie6Buffer18SetDataAndCapacityEPvm")) return (void *)test_set_data;
    if (!strcmp(name, "_ZN5movie6Buffer8SetRangeEii")) return (void *)test_set_range;
    if (!strcmp(name, "_ZNK5movie6Buffer4DataEv")) return (void *)test_data;
    if (!strcmp(name, "_ZNK5movie6Buffer8CapacityEv")) return (void *)test_cap;
    if (!strcmp(name, "_ZNK5movie6Buffer4SizeEv")) return (void *)test_size;
    if (!strcmp(name, "_ZN5movie9MediaData8SetInt32EPKci")) return (void *)test_set_i32;
    if (!strcmp(name, "_ZN5movie9MediaData7SetRectEPKciiii")) return (void *)test_set_rect;
    if (!strcmp(name, "_ZNK5movie9MediaData9FindInt32EPKcPi")) return (void *)test_find_i32;
    if (!strcmp(name, "_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE")) return (void *)test_find_buffer;
    if (!strcmp(name, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE")) return (void *)test_signal;
    assert(!"unexpected SDK symbol lookup"); return NULL;
}
uint64_t now_ns(void) { struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param)) { (void)id; (void)fn; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
// This fixture has no guest scheduler; retain real mutex acquisition and worker
// teardown. Scheduler handoff is covered by t_preempt, not this constructor test.
void dm_host_lock(pthread_mutex_t *m) { assert(pthread_mutex_lock(m) == 0); }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) { (void)fn; (void)arg; (void)prio; (void)stack_size; abort(); }
int nvn_host_write_texture(const void *tex, const void *src, int w, int h, size_t row, int *tw, int *th) { (void)tex; (void)src; (void)w; (void)h; (void)row; (void)tw; (void)th; abort(); }
int nvn_host_signal_sync(void *sync) { (void)sync; abort(); }

static void check_copy(void **self, const uint8_t *frame, int bytes) {
    mdec_t *d = md_of(self);
    d->out[0].frm = malloc(bytes); assert(d->out[0].frm);
    memcpy(d->out[0].frm, frame, bytes); d->out[0].pic = 1; d->out[0].bytes = d->out[0].cap = bytes;
    // Undersized, exactly sized and spare-capacity CPU buffers; an existing
    // nonzero read offset must be reset before any copy. No legacy size contract.
    for (int extra = -1; extra <= 1; extra++) {
        int capacity = bytes + extra * (extra > 0 ? 8 : 1);
        uint8_t *guarded = malloc((size_t)capacity + 2); assert(guarded);
        memset(guarded, 0xa5, (size_t)capacity + 2);
        test_buffer buffer = {guarded + 1, (size_t)capacity, 1, 1};
        int result = h_movie_GetOut(self, 0, &buffer);
        assert(result == (extra < 0 ? 0x100b : 0));
        assert(buffer.off == 0 && buffer.size == (extra < 0 ? 0 : bytes));
        if (extra >= 0) assert(!memcmp(buffer.base, frame, bytes));
        assert(guarded[0] == 0xa5);
        for (int i = extra < 0 ? 0 : bytes; i <= capacity; i++) assert(buffer.base[i] == 0xa5);
        free(guarded);
    }
}

static void check_legacy(int audio) {
    void *self[2] = {NULL, NULL};
    void (*ctor)(void **, int, int) = hle_force_h_movie_Decoder3.fn;
    ctor(self, audio ? MD_TYPE_AAC : MD_TYPE_AVC, audio ? 1 : 3);
    mdec_t *d = md_of(self); assert(d && self[0] == (uint8_t *)test_vtable + 0x10);
    assert(d->mode == 1 && d->fmt == (audio ? MD_FMT_PCM16 : MD_FMT_NV12));
    test_format f = {2, 2, 2, 48000};
    assert(h_movie_Configure(self, &f) == 0); // missed constructor used to leave an incompatible SDK object here
    if (audio) assert(d->ch == 2 && d->rate == 48000);
    else assert(d->w == 2 && d->h == 2);
    const uint8_t pcm[] = {0x01, 0x02, 0x03, 0x04}, nv12[] = {16, 32, 48, 64, 96, 128};
    check_copy(self, audio ? pcm : nv12, audio ? sizeof pcm : sizeof nv12);
    h_movie_DecD1(self); assert(!self[1]);
}

static void check_modern(void) {
    void *self[2]; int size, mode; test_format f = {2, 2, 2, 48000};
    h_movie_Decoder4(self, MD_TYPE_AVC, MD_FMT_NV12, 1);
    assert(h_movie_Configure(self, &f) == 0);
    assert(md_of(self)->fmt == MD_FMT_NV12 && h_movie_OutSize(self, &size, &mode) == 0 && size == 6 && mode == 1);
    const uint8_t nv12[] = {16, 32, 48, 64, 96, 128}; check_copy(self, nv12, sizeof nv12);
    h_movie_DecD1(self); assert(!self[1]);
    h_movie_Decoder(self, MD_TYPE_AAC, MD_FMT_PCM16, 1, 0);
    assert(h_movie_Configure(self, &f) == 0);
    assert(h_movie_OutSize(self, &size, &mode) == 0 && size == 4096 && mode == 1);
    h_movie_DecD1(self); assert(!self[1]);
    h_movie_Decoder4(self, MD_TYPE_AVC, MD_FMT_ABGR, MD_MODE_TEX);
    assert(md_of(self) && md_of(self)->fmt == MD_FMT_ABGR && md_of(self)->mode == MD_MODE_TEX);
    h_movie_DecD1(self); assert(!self[1]);
}

static void check_rejected(int type, int fmt) {
    void *self[2] = {NULL, NULL}; h_movie_Decoder3(self, type, fmt);
    assert(self[0] == (uint8_t *)test_vtable + 0x10 && !self[1]);
    assert(h_movie_Configure(self, NULL) == 0x1000);
    h_movie_DecD1(self); assert(!self[1]);
}

int main(void) {
    assert(!strcmp(hle_force_h_movie_Decoder3.name, "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatE"));
    check_legacy(0); check_legacy(1); check_modern();
    // Old ARGB (2) stays unsupported; old/unknown values may not select newer
    // NV12 (4), nor fall back to PCM16 merely because the decoder is audio.
    const int unsupported[] = {INT_MIN, -1, 0, 2, 4, 5, INT_MAX};
    for (size_t i = 0; i < sizeof unsupported / sizeof *unsupported; i++) {
        check_rejected(MD_TYPE_AVC, unsupported[i]);
        check_rejected(MD_TYPE_AAC, unsupported[i]);
    }
    check_rejected(MD_TYPE_AVC, 1); check_rejected(MD_TYPE_AAC, 3);
    puts("movie SDK 3: registered C1, configure, guarded CPU NV12/PCM copies, unsupported formats rejected; modern constructors unchanged");
    return 0;
}
