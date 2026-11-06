// System mixer microphone streams with owned capture sessions and shared PCM rings.
#include "sf_service.h"
#include "audio_input_ring.h"
#include "capture_apple.h"
#include <math.h>
#include <mach/mach.h>
extern int svc_thread(int num, uint64_t *regs);

enum { SX_STREAMS = 7 };
typedef struct { uint64_t pid; unsigned refs; } sx_client;
typedef struct {
    sx_client *client;
    uint32_t id, device, state, format, channels, rate;
    void *work; size_t work_size;
    char name[256];
    dm_capture *capture;
    audio_input_ring ring;
    pthread_mutex_t mutex; // callback and control data; never held while disposing the AudioQueue
    float gain, target, step;
    uint64_t ramp;
    uint32_t state_r, state_w, data_r, data_w, threshold;
} sx_stream;
// Only IPC dispatch/close touches these (hle_sf's lock). The sample callback
// touches its own stream under mutex and never takes the IPC lock.
static sx_client *sx_process;
static sx_stream *sx_streams[SX_STREAMS];
static uint32_t sx_next_id = 1;
static uint32_t sx_u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t sx_u64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int sx_shape(const sf_request *q, size_t bytes, unsigned na, int pid, unsigned copies) {
    return q->size >= bytes && q->na == na && q->send_pid == pid && q->ncopy == copies &&
           !q->nb && !q->nx && !q->nw && !q->nc && !q->nmove && !q->nobjects && (!na || !q->a[0].mode);
}
static void sx_reply32(sf_reply *r, uint32_t v) { memcpy(r->data, &v, 4); r->size = 4; }
static int sx_name(const sf_buffer *b) { return b->data && b->size && b->size < 256; }
static void sx_signal(uint32_t handle) {
    if (handle) { uint64_t regs[8] = {handle}; svc_thread(0x11, regs); }
}
static void sx_event_close(uint32_t *read, uint32_t *write) {
    sf_close_handle(*write); sf_close_handle(*read); *read = *write = 0;
}
static void sx_state(sx_stream *s, uint32_t state) {
    if (s->state != state) { s->state = state; sx_signal(s->state_w); }
}
static void sx_notify(sx_stream *s) {
    uint32_t filled;
    if (s->data_w && air_filled(&s->ring, &filled) && filled >= s->threshold) sx_signal(s->data_w);
}
static void sx_samples(void *context, const float *pcm, uint32_t frames) {
    sx_stream *s = context;
    pthread_mutex_lock(&s->mutex);
    // The input is a mono microphone. Replicate it into the requested logical
    // channels; stream gain applies before integer saturation. Float stays float.
    uint8_t converted[256 * 24];
    while (frames) {
        uint32_t n = frames < 256 ? frames : 256, bytes = s->format == 2 ? 2 : 4;
        for (uint32_t k = 0; k < n; k++) {
            if (s->ramp) { s->gain += s->step; if (!--s->ramp) s->gain = s->target; }
            double v = isfinite(pcm[k]) ? (double)pcm[k] * s->gain : 0;
            uint32_t sample = 0;
            if (s->format == 5) { float f = (float)v; memcpy(&sample, &f, 4); }
            else {
                double scale = s->format == 2 ? 32768.0 : s->format == 3 ? 8388608.0 : 2147483648.0;
                double q = v * scale;
                if (q < -scale) q = -scale;
                if (q > scale - 1) q = scale - 1;
                sample = (uint32_t)(int32_t)q;
            }
            for (uint32_t ch = 0; ch < s->channels; ch++)
                memcpy(converted + ((size_t)k * s->channels + ch) * bytes, &sample, bytes);
        }
        air_write(&s->ring, converted, n, guest_ns());
        pcm += n; frames -= n;
    }
    sx_notify(s);
    pthread_mutex_unlock(&s->mutex);
}
static int sx_work_valid(void *work, size_t size) {
    // Like the service's process-memory map: reject holes/readonly memory before
    // retaining the guest's work buffer. Its lifetime ends only after CloseStream.
    vm_address_t at = (uintptr_t)work, end = at + size;
    if (!at || end < at) return 0;
    while (at < end) {
        vm_address_t base = at; vm_size_t length = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = vm_region_64(mach_task_self(), &base, &length, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (kr || base > at || length <= at - base || (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) return 0;
        length -= at - base;
        if (length >= end - at) return 1;
        at += length;
    }
    return 0;
}
static void sx_client_release(sx_client *c) { if (!--c->refs) { sx_process = NULL; free(c); } }
static void sx_user_close(void *state) { sx_client_release(state); }
static void sx_stream_close(void *state) {
    sx_stream *s = state;
    dm_capture_close(s->capture); // joins callbacks before the ring, events or mutex can disappear
    sx_event_close(&s->data_r, &s->data_w); sx_event_close(&s->state_r, &s->state_w);
    for (unsigned k = 0; k < SX_STREAMS; k++) if (sx_streams[k] == s) sx_streams[k] = NULL;
    sx_client_release(s->client); pthread_mutex_destroy(&s->mutex); free(s);
}
static Result sx_stream_request(void *state, const sf_request *q, sf_reply *r) {
    sx_stream *s = state;
    static const uint8_t sizes[] = {24, 0, 0, 0, 16, 0, 0, 8, 0, 8, 4, 4, 0};
    if (q->command >= sizeof sizes) return 0x1ba0a;
    if (!sx_shape(q, sizes[q->command], q->command == 12, 0, 0)) return 0x1ce0a;
    switch (q->command) {
    case 0: {
        if (s->state != 1) return 0xc8099;
        uint32_t format = sx_u32(q->data), ch = sx_u32(q->data + 4), rate = sx_u32(q->data + 8);
        uint32_t capacity = sx_u32(q->data + 12), initial = sx_u32(q->data + 16), flags = sx_u32(q->data + 20);
        if (format < 2 || format > 5) return 0x40699;
        if (ch != 1 && ch != 2 && ch != 6) return 0x1499;
        if (rate != 16000 && rate != 32000 && rate != 44100 && rate != 48000) return 0x699;
        if (!capacity || capacity >= INT_MAX || initial > capacity || flags > 1) return 0xc8899;
        if (!air_bind(&s->ring, s->work, s->work_size, capacity, ch * (format == 2 ? 2 : 4), flags & 1)) return 0xc8e99;
        dm_guest_write_prepare(s->work, s->work_size);
        s->format = format; s->channels = ch; s->rate = rate;
        s->capture = dm_capture_open(rate, sx_samples, s);
        if (!s->capture) return 0xa99;
        sx_state(s, 2);
        LOG("audsmx: input %u opened (%u Hz, %u channels, format %u, %u frames)", s->id, rate, ch, format, capacity);
        return 0;
    }
    case 1:
        if (s->state == 1) return 0;
        dm_capture_close(s->capture); s->capture = NULL; sx_state(s, 1); return 0;
    case 2: sx_reply32(r, s->state); return 0;
    case 3:
        pthread_mutex_lock(&s->mutex); memcpy(r->data, &s->target, 4); pthread_mutex_unlock(&s->mutex);
        r->size = 4; return 0;
    case 4: {
        float gain; memcpy(&gain, q->data, 4); uint64_t ns = sx_u64(q->data + 8);
        if (!(gain >= 0 && gain <= 128) || ns > UINT64_C(5000000000)) return 0xc8899;
        pthread_mutex_lock(&s->mutex);
        s->target = gain; s->ramp = ns * s->rate / UINT64_C(1000000000);
        if (s->ramp) s->step = (gain - s->gain) / (float)s->ramp; else s->gain = gain;
        pthread_mutex_unlock(&s->mutex); return 0;
    }
    case 5:
        if (s->state_r) return 0xc9699;
        s->state_r = hle_new_event(&s->state_w); r->copy = hle_dup_readable_handle(s->state_r); return 0;
    case 6: sx_event_close(&s->state_r, &s->state_w); return 0;
    case 7: {
        uint32_t mode = sx_u32(q->data), threshold = sx_u32(q->data + 4);
        if (mode < 1 || mode > 3 || threshold > INT_MAX) return 0xc8899;
        // InputStream uses filled-frame notifications (mode 2). Output/free and
        // clock events belong to the as-yet unsupported output mixer path.
        if (mode != 2) return 0xcb299;
        pthread_mutex_lock(&s->mutex);
        if (s->data_r) { pthread_mutex_unlock(&s->mutex); return 0xc9699; }
        s->threshold = threshold; s->data_r = hle_new_event(&s->data_w);
        r->copy = hle_dup_readable_handle(s->data_r);
        if (s->capture) sx_notify(s);
        pthread_mutex_unlock(&s->mutex); return 0;
    }
    case 8:
        pthread_mutex_lock(&s->mutex); sx_event_close(&s->data_r, &s->data_w); pthread_mutex_unlock(&s->mutex); return 0;
    case 12:
        if (!sx_name(&q->a[0])) return 0xc8e99;
        memcpy(s->name, q->a[0].data, q->a[0].size); s->name[q->a[0].size] = 0; return 0;
    default:
        // The guest implements input purge by advancing its own reader.
        // Clock/remote prime/purge are the opposite (output) direction.
        return 0xcb299;
    }
}
static sx_stream *sx_find(uint64_t pid, uint32_t id) {
    for (unsigned k = 0; k < SX_STREAMS; k++)
        if (sx_streams[k] && sx_streams[k]->id == id && sx_streams[k]->client->pid == pid) return sx_streams[k];
    return NULL;
}
Result sf_audsmx_attach(uint64_t pid, uint32_t id, uint32_t device) {
    sx_stream *s = sx_find(pid, id);
    if (!s) return 0xcb099;
    if (device < 2 || device > 7) return 0x40299;
    if (s->state == 1) return 0xc8099;
    if (s->state == 3) { s->device = device; return 0; }
    if (!dm_capture_start(s->capture)) return 0xcbe99;
    s->device = device; sx_state(s, 3); LOG("audsmx: input %u attached to microphone", id); return 0;
}
Result sf_audsmx_detach(uint64_t pid, uint32_t id) {
    sx_stream *s = sx_find(pid, id);
    if (!s) return 0xcb099;
    dm_capture_stop(s->capture);
    if (s->state == 3) sx_state(s, 2);
    return 0;
}
static Result sx_user_request(void *state, const sf_request *q, sf_reply *r) {
    sx_client *c = state;
    if (q->command != 1) return 0x1ba0a; // input starts via auddmg attachment, not EngageStreams' system-node group API
    if (!sx_shape(q, 32, 1, 1, 0)) return 0x1ce0a;
    if (q->pid != c->pid || (sx_u64(q->data + 8) && sx_u64(q->data + 8) != q->pid)) return 0x60a;
    uint32_t mixer = sx_u32(q->data), flags = sx_u32(q->data + 4);
    if (flags != 2) { LOG("[RED] audsmx: stream direction/options %#x are not implemented", flags); return 0xcb299; }
    if (mixer != 1) return 0xcc499;
    if (!sx_name(&q->a[0])) return 0xc8e99;
    void *work = (void *)(uintptr_t)sx_u64(q->data + 16); size_t size = sx_u64(q->data + 24);
    if (!work || !size || (((uintptr_t)work | size) & 4095)) return 0xc8899;
    if (!sx_work_valid(work, size)) return 0xc8a99;
    unsigned slot = 0; while (slot < SX_STREAMS && sx_streams[slot]) slot++;
    if (slot == SX_STREAMS || !sx_next_id) return 0xa99;
    sx_stream *s = calloc(1, sizeof *s); if (!s) return 0xa99;
    s->client = c; c->refs++; s->id = sx_next_id++; s->state = 1;
    s->work = work; s->work_size = size; s->gain = s->target = 1;
    pthread_mutex_init(&s->mutex, NULL);
    memcpy(s->name, q->a[0].data, q->a[0].size);
    sx_streams[slot] = s;
    sx_reply32(r, s->id); r->child = (sf_object){sx_stream_request, sx_stream_close, s};
    return 0;
}
static Result sx_root_request(void *state, const sf_request *q, sf_reply *r) {
    (void)state;
    if (q->command != 0) return 0x1ba0a;
    if (!sx_shape(q, 16, 0, 1, 1)) return 0x1ce0a;
    if (q->copies[0] != 0xffff8001u) return 0xe401; // current process; never use a host Mach/POSIX handle supplied by the guest
    if (sx_u32(q->data) != 1 || q->pid != DM_GUEST_PROCESS_ID) return 0xc8899;
    if (sx_process) return 0xa99;
    sx_process = calloc(1, sizeof *sx_process); if (!sx_process) return 0xa99;
    sx_process->pid = q->pid; sx_process->refs = 1;
    r->child = (sf_object){sx_user_request, sx_user_close, sx_process}; return 0;
}
sf_object sf_audsmx_root(void) { return (sf_object){sx_root_request, NULL, NULL}; }
