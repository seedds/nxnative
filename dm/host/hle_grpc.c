// gRPC's internal errors, seen through observation gate OBS_GRPC_ERROR (dm.h) on grpc_error_create's entry, which a game profile's patches.py places in a main that
// links gRPC: Super Mario Bros. Wonder 1.2.1 (main+0xb57b40, dm/games/010015100B514000/patches.py). Nintendo's build of gRPC has its logging compiled out (no trace
// strings, nothing reads gpr_log's function), so GRPC_VERBOSITY and GRPC_TRACE print nothing; a call that fails as UNAVAILABLE before any socket (no root certificates,
// no subchannel) otherwise leaves no line at all. Each distinct source file and line logs once, with the error's text, up to 96 of them.
// grpc_error_create(const char *file, int line, const grpc_slice &desc, grpc_error **referencing, size_t count): x0 the file (a string in main), w1 the line, x2 the
// slice. A grpc_slice is {refcount, data}: with no refcount the bytes are inline (length byte +8, bytes +9, at most 23), otherwise length +8 and a pointer +0x10.
// The text is gRPC's own description (a server name or address at most, never a token or a payload); printable ASCII only, cut at 160 characters
#include "hle.h"
#include <pthread.h>
#include <string.h>

static void grpc_error_seen(uint64_t *r, int param) {
    (void)param;
    const char *file = (const char *)r[0]; int line = (int)(uint32_t)r[1]; const uint8_t *slice = (const uint8_t *)r[2];
    static struct { const char *file; int line; } seen[96]; static unsigned count; static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock); int fresh = count < 96;
    for (unsigned i = 0; i < count && fresh; i++) if (seen[i].file == file && seen[i].line == line) fresh = 0;
    if (fresh) { seen[count].file = file; seen[count].line = line; count++; }
    pthread_mutex_unlock(&lock);
    if (!fresh) return;
    char text[161] = ""; size_t n = 0;
    if (slice) {
        uint64_t refcount; memcpy(&refcount, slice, 8);
        const uint8_t *bytes; uint64_t length;
        if (!refcount) { length = slice[8]; bytes = slice + 9; if (length > 23) length = 23; }
        else { memcpy(&length, slice + 8, 8); memcpy(&bytes, slice + 16, 8); }
        for (uint64_t i = 0; bytes && i < length && n < sizeof text - 1; i++) text[n++] = bytes[i] >= 32 && bytes[i] < 127 ? (char)bytes[i] : '?';
        text[n] = 0;
    }
    const char *short_file = file ? file : "?", *core = file ? strstr(file, "src/core/") : NULL; if (core) short_file = core + 9;
    LOG("grpc: error at %s:%d: %s", short_file, line, text);
}
__attribute__((constructor)) static void grpc_attach(void) { gate_obs_set(OBS_GRPC_ERROR, grpc_error_seen); }
