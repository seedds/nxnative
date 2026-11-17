// hle_audio.c unit test (silent sink): SDK 21 / 22 InputStream (microphone, pitch-tracking, voice input).
// Tests device enumeration (virtual & raw input devices), property querying, stream lifecycle (open, start, stop, close),
// format verification, buffer capacity / free count tracking, gain controls, and frame reading.
#include "../hle_audio.c"
#include <time.h>

__thread _Atomic int dm_host_work;
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = NULL; void *v = om[i].v; om[i].v = NULL; return v; } return NULL; }

module_t *module_of(const void *p) { (void)p; return NULL; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
static _Atomic int g_sig;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add_explicit(&g_sig, 1, memory_order_seq_cst); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }

static char stream[0x350];
#define FAIL(...) do { h_is_Close(stream); printf("FAIL " __VA_ARGS__); puts(""); return 1; } while (0)

int main(void) {
    enum { CAP = 4800, READ_FRAMES = 960 };
    char name[VDEV_NAME] = {0}, raw_name[VDEV_NAME] = {0};

    // 1. Device enumeration and default naming
    if (h_is_List(name, 1) != 1) FAIL("ListVirtualInputDevices count");
    if (strcmp(name, VDEV_IN) != 0) FAIL("ListVirtualInputDevices name: '%s' != '%s'", name, VDEV_IN);

    h_is_DefDev(name);
    if (strcmp(name, VDEV_IN) != 0) FAIL("GetDefaultVirtualInputDevice name: '%s' != '%s'", name, VDEV_IN);

    h_is_RawDev(raw_name);
    if (strcmp(raw_name, RAW_DEV_IN) != 0) FAIL("GetTrueRawAudioInputDevice name: '%s' != '%s'", raw_name, RAW_DEV_IN);

    // 2. Unopened stream queries
    if (h_is_State(stream) != OS_NONE) FAIL("state of stream before open");
    if (h_is_Filled(stream) != 0) FAIL("filled frames before open");
    if (h_is_Free(stream) != 0) FAIL("free frames before open");
    if (h_is_Capacity(stream) != 0) FAIL("capacity before open");

    // 3. Virtual device properties
    vdev_props props; memset(&props, 0xee, sizeof props);
    if (h_is_Props(&props, name) != RES_OK) FAIL("properties of virtual device");
    if (strcmp(props.name, name) != 0 || props.min_ns != OS_MIN_NS)
        FAIL("properties content: name='%.16s', min_ns=%lld", props.name, (long long)props.min_ns);

    // 4. Open input stream (48 kHz mono PCM16)
    os_format fmt = {OS_PCM16, 1, 48000};
    if (h_is_Open(stream, NULL, 0, name, &fmt, CAP) != RES_OK) FAIL("OpenInputStream");
    if (h_is_State(stream) != OS_STOPPED) FAIL("state after open != OS_STOPPED");
    if (h_is_Capacity(stream) != CAP) FAIL("capacity after open: %d != %d", h_is_Capacity(stream), CAP);
    if (h_is_Free(stream) != CAP) FAIL("free frames after open: %d != %d", h_is_Free(stream), CAP);

    os_format stream_fmt = h_is_Format(stream);
    if (stream_fmt.format != OS_PCM16 || stream_fmt.channels != 1 || stream_fmt.rate != 48000)
        FAIL("stream format mismatch: fmt=%d ch=%d rate=%d", stream_fmt.format, stream_fmt.channels, stream_fmt.rate);

    // 5. Gain controls
    if (h_is_GetGain(stream) != 1.0f) FAIL("default gain != 1.0f");
    h_is_SetGain(stream, 0.75f, 0);
    if (h_is_GetGain(stream) != 0.75f) FAIL("updated gain != 0.75f");

    // 6. Start input stream
    if (h_is_Start(stream) != RES_OK) FAIL("StartInputStream");
    if (h_is_State(stream) != OS_STARTED) FAIL("state after start != OS_STARTED");

    // 7. Read input stream (silent sink / test mode returns zeroed PCM16 frames)
    int16_t buf[READ_FRAMES]; memset(buf, 0x5a, sizeof buf);
    int read_count = h_is_Read(stream, buf, READ_FRAMES);
    if (read_count != READ_FRAMES) FAIL("ReadInputStream read count: %d != %d", read_count, READ_FRAMES);
    for (int i = 0; i < READ_FRAMES; i++) {
        if (buf[i] != 0) FAIL("starved read was not zero-filled at index %d", i);
    }

    // 8. Purge
    h_is_Purge(stream);
    if (h_is_Filled(stream) != 0) FAIL("filled frames after purge != 0");

    // 9. Stop and close
    h_is_Stop(stream);
    if (h_is_State(stream) != OS_STOPPED) FAIL("state after stop != OS_STOPPED");

    h_is_Close(stream);
    if (h_is_State(stream) != OS_NONE) FAIL("state after close != OS_NONE");

    printf("PASS t_input_stream: virtual and raw audio input streams verified\n");
    return 0;
}
