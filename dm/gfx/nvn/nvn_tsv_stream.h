// Bounded opt-in diagnostic output. Deferred producers never touch the file.
#ifndef NVN_TSV_STREAM_H
#define NVN_TSV_STREAM_H
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *file;
    char *bytes;
    size_t capacity, used, flushed;
    uint64_t dropped, reported;
    pthread_mutex_t append_mu, drain_mu;
} nvn_tsv_stream;
#define NVN_TSV_STREAM_INIT { .append_mu = PTHREAD_MUTEX_INITIALIZER, .drain_mu = PTHREAD_MUTEX_INITIALIZER }

// Called before publication to producers. capacity=0 retains ordinary stdio.
// Touch every page now: a long capture must not fault fresh pages mid-frame.
static int nvn_tsv_stream_init(nvn_tsv_stream *s, FILE *file, size_t capacity) {
    if (capacity) {
        s->bytes = malloc(capacity);
        if (!s->bytes) return -1;
        volatile char *p = s->bytes;
        for (size_t i = 0; i < capacity; i += 4096) p[i] = 0;
        p[capacity - 1] = 0;
    }
    s->file = file;
    s->capacity = capacity;
    return 0;
}

static void nvn_tsv_stream_printf(nvn_tsv_stream *s, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (!s->bytes) {
        vfprintf(s->file, format, args);
    } else {
        char row[512];
        int n = vsnprintf(row, sizeof row, format, args);
        pthread_mutex_lock(&s->append_mu);
        if (n < 0 || (size_t)n >= sizeof row || (size_t)n > s->capacity - s->used) {
            s->dropped++;   // Never append a partial row or flush on overflow.
        } else {
            memcpy(s->bytes + s->used, row, (size_t)n);
            s->used += (size_t)n;
        }
        pthread_mutex_unlock(&s->append_mu);
    }
    va_end(args);
}

// Explicit suspend/quit boundary, not the per-frame path. Prefixes are immutable:
// producers append to the tail while file I/O proceeds without append_mu held.
// Storage is not recycled; repeated drains write only newly appended bytes.
static int nvn_tsv_stream_drain(nvn_tsv_stream *s) {
    if (!s->file) return 0;
    pthread_mutex_lock(&s->drain_mu);
    int failed = 0;
    if (s->bytes) {
        pthread_mutex_lock(&s->append_mu);
        size_t end = s->used;
        uint64_t dropped = s->dropped;
        pthread_mutex_unlock(&s->append_mu);
        size_t count = end - s->flushed;
        size_t written = count ? fwrite(s->bytes + s->flushed, 1, count, s->file) : 0;
        s->flushed += written;
        failed = written != count;
        if (!failed && dropped != s->reported) {
            if (fprintf(s->file, "# deferred TSV rows dropped: %llu (capacity %zu bytes)\n",
                        (unsigned long long)dropped, s->capacity) < 0) failed = 1;
            else s->reported = dropped;
        }
    }
    if (fflush(s->file)) failed = 1;
    pthread_mutex_unlock(&s->drain_mu);
    return failed ? -1 : 0;
}
#endif
